/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.cpp for license header)
===========================================================================
*/

// DUDE RHI 3D world rendering — Phase 3 Chunk E.
//
// Depth prepass, stencil shadow volumes and per-light interactions,
// mirroring RB_STD_FillDepthBuffer / RB_StencilShadowPass / RB_T_Shadow /
// RB_ARB2_DrawInteractions pass for pass and state bit for state bit.
// The interaction decomposition (RB_CreateSingleDrawInteractions and
// RB_DetermineLightScale) is REUSED from tr_render.cpp — it is pure
// idTech4 semantics; only its draw callback is ours.
//
// Stencil state is driven with direct qgl calls like the legacy path
// (TODO(RHI): becomes pipeline state in the Vulkan backend). Shadow
// volumes always use glStencilOpSeparate — core since GL 2.0.

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/RenderWorld_local.h"			// RR16: iterate the render world's lightDefs (off-screen lights for reflections)
#include "renderer/VertexCache.h"
#include "renderer/Model.h"
#include "renderer/rhi/RHI.h"
#include "renderer/rhi/GL3Local.h"
#include "renderer/rhi/RenderParams.h"
#include "renderer/rhi/RhiTess.h"
#include "framework/FileSystem.h"
#include "framework/DeclSkin.h"					// idDeclSkin (entity skin -> AO name for lazy bake)
#include "tools/compilers/aobake/aobake.h"		// AO_BakeModelToCache (lazy occlusion-map baking)
#include "framework/DeclManager.h"					// DECL_MATERIAL enumeration (decal image precache)

extern idCVar r_useCarmacksReverse;		// defined in RenderSystem_init.cpp, no tr_local decl

// GL 3.0 core enum; not always pulled in by the legacy gl.h include path
#ifndef GL_CLIP_DISTANCE0
#define GL_CLIP_DISTANCE0 0x3000
#endif

/*
===================
interaction draw context

RB_CreateSingleDrawInteractions takes a plain function pointer, so the
per-surface stream handles and per-light state travel through this.
===================
*/
static struct {
	rhi::RHI *			r;
	const viewDef_t *	viewDef;
	float				mvp[16];
	rhi::BufferHandle	vb, ib;
	int					vertOfs, idxOfs;
	rhi::ShaderHandle	interactionProg;
	rhi::ShaderHandle	interactionRtProg;	// ray-query variant (R3 RT sun shadows); 0 on non-RT hardware
	rhi::ShaderHandle	ambientProg;
	int					depthFuncBits;		// GLS_DEPTHFUNC_EQUAL, LESS for translucents
	int					stencilState;		// rhi::StencilState for interaction pipelines
										// (Vulkan; GL keeps driving qglStencil* directly)

	// shadow mapping (DUDE Phase 3.5). The current light either uses a shadow map
	// (lightShadowMapped) or the stencil path. For a projected/spot light the lookup
	// reuses the light-projection texgen (S/T/Q + falloff) and a 2D depth map on unit
	// 7 (shadowImage). For a point light (lightShadowCube) it uses a cube depth map on
	// unit 8 (shadowCubeImage) indexed by the world-space light->frag direction, with
	// lightRange normalizing the stored radial distance.
	bool				lightShadowMapped;
	rhi::ImageHandle	shadowImage;
	// DUDE sun shadow maps (r_shadowMapSun): the 2D map on unit 7 was rendered through a
	// per-view fitted VIRTUAL projection (rhiSunPlanes), not the light's own texgen — the
	// receiver must sample with those planes and take its compare depth from the virtual
	// falloff plane (shader mode 3). Only meaningful while lightShadowMapped is set.
	bool				lightSunShadow;
	// RT sun shadows (R3): this light took the sun route WITHOUT a rendered map - the
	// virtual-map fit declined (light inside the fitted view sphere) but the ray path
	// serves it anyway. rhiSunPlanes/shadowImage are stale for this light; only shader
	// mode 4 is valid, and the receiver fill downgrades to unshadowed if the TLAS died.
	bool				lightRtOnly;
	// r_rtAllLights: any shadow-casting light, of any species, routed to the inline ray - or
	// r_rtMovingLights: this point light's pose changed recently, so it renders no cube
	// at all — the interaction shader traces one ray toward the light (mode 4) instead.
	bool				lightRtPoint;
	// r_rtShadowBlur (docs/rtx-shadow-blur.md): this ray-served light (either route above) had
	// its hard ray traced per screen pixel and blurred right before its interactions draw -
	// they look the result up (shader mode 5, shadowMaskImage on unit 13) instead of tracing
	// inline. Surfaces the screen-space mask can't describe keep the inline mode-4 ray.
	bool				lightRtBlur;
	rhi::ImageHandle	shadowMaskImage;
	// r_rtShadowBlurStagger: the mask was made on an EARLIER frame - interactions look it up
	// reprojected (shader mode 6) through that frame's world-to-clip matrix
	bool				shadowMaskStale;
	float				shadowMaskViewProj[16];
	bool				lightShadowCube;
	rhi::ImageHandle	shadowCubeImage;
	// static/dynamic split (r_shadowMapCacheSplit): when a cached static point light also
	// has movers, they render into a second cube on unit 12 (shadowCubeDynImage). The
	// interaction shader samples min(u_shadowCube, u_shadowCubeDyn) when this is set.
	bool				lightHasDynamicLayer;
	rhi::ImageHandle	shadowCubeDynImage;
	float				shadowCubeDynScale;	// static/dynamic face-res ratio (r_shadowMapSplitDynDrop)
	float				lightRange;
	// The player flashlight (light shader "lights/flashlight5", a narrow projected
	// spot that hugs surfaces and sweeps every frame) self-shadows badly at the
	// world/model depth bias — it needs a much smaller compare bias to avoid
	// peter-panning and swimming shadow edges. Detected per-light by shader name.
	bool				lightIsFlashlight;
} ictx;

// Persistent shadow-map render target, kept across frames and recreated only
// when the resolution cvar changes or the context is lost (vid_restart returns
// a fresh backend, so a stale handle just fails GetRenderTargetImage and we
// remake it). One target reused serially by every shadow-mapped light in a frame.
static rhi::RenderTargetHandle rhiShadowMap = 0;
static int rhiShadowMapSize = 0;

// DUDE sun shadow maps (r_shadowMapSun): the current sun light's fitted virtual
// projection — world-space S, T, Q, depth planes. Written by RB_RHI_ShadowMapPassSun,
// read by the receiver parms fill (mode 3) while ictx.lightSunShadow is set.
// rhiSunTexelWorld = one sun-map texel's world size at the fit center, for the
// normal-offset bias (r_shadowMapNormalOffset).
static idPlane rhiSunPlanes[4];
static float rhiSunTexelWorld = 0.0f;

// The cube depth target currently selected for this light's render/sample. It comes
// from either the static cube cache (rhiCubeCache, keyed per light — see
// RB_RHI_AcquireCubeTarget) or the per-tier scratch pool (RB_RHI_ShadowPoolTarget), not
// a single persistent allocation. rhiShadowCubeSize is its face resolution.
static rhi::RenderTargetHandle rhiShadowCube = 0;
static int rhiShadowCubeSize = 0;
// Static/dynamic split (r_shadowMapCacheSplit, lever B): when a moving/animated caster
// shares a static point light, the world casters render into the cached cube above and
// the movers into this scratch cube (regenerated every frame). 0 = no dynamic layer this
// light (the common case). The interaction pass samples min(rhiShadowCube, rhiShadowCubeDyn).
static rhi::RenderTargetHandle rhiShadowCubeDyn = 0;
// r_shadowMapSplitDynDrop: static-to-dynamic face-resolution ratio (1.0 = same size).
// Rides u_pbrParms2.z into the shader, which widens the dynamic cube's PCF disc and
// depth bias to its coarser texels.
static float rhiShadowCubeDynScale = 1.0f;

// r_shadowMapDebug: perforated (grate/fence) caster surfaces drawn into the map
// this view — confirms the alpha-tested casters are actually reaching the pass.
static int rhiShadowPerfCasters = 0;
// r_shadowMapDebug: point lights that got a cube shadow map this view.
static int rhiShadowCubeLights = 0;
// r_shadowMapDebug: caster draws submitted into cube faces this view (after per-face
// culling) — 0 means nothing reached the cube (culled / empty), so no shadow.
static int rhiShadowCubeCasters = 0;
// r_shadowMapDebug: cube faces rasterized vs skipped by the view-frustum face cull.
static int rhiShadowCubeFaces = 0;
static int rhiShadowCubeFacesCulled = 0;

// Adaptive shadow resolution (r_shadowMapSizeScale). A light's radius decides a
// resolution "tier": each tier doubles/halves the base cvar size so a texel maps to
// roughly the same world distance regardless of how far the shadow reaches — big
// lights get more resolution (fewer jaggies), small ones get less (cheaper). Tiers
// are power-of-two shifts around the base; the range below keeps VRAM bounded.
static const int SHADOW_TIER_MIN = -1;		// one step below base
static const int SHADOW_TIER_MAX =  2;		// up to 4x base
static const int SHADOW_NTIERS   = SHADOW_TIER_MAX - SHADOW_TIER_MIN + 1;

static void RB_RHI_ForgetTexBinds();		// defined below; used by the pool allocator

// One render target per tier, allocated lazily and kept across frames. Serial reuse
// within a frame is unchanged; the pool only prevents destroy/recreate thrash when
// consecutive lights land in different tiers. A lost context (GetRenderTargetImage
// == 0 after vid_restart) invalidates the slot and it rebuilds on next use.
struct shadowRtSlot_t {
	rhi::RenderTargetHandle	rt;
	int						size;
};
static shadowRtSlot_t rhiShadowMapPool[SHADOW_NTIERS];	// 2D (projected/spot)
static shadowRtSlot_t rhiShadowCubePool[SHADOW_NTIERS];	// cube (point/omni)

// Map a light radius to a tier index in [0, SHADOW_NTIERS). Tier for the reference
// radius (r_shadowMapSizeScaleRadius) is the base size; each doubling of radius adds
// a tier. With scaling off, everything lands on the base tier.
static int RB_RHI_ShadowTier( float radius ) {
	if ( !r_shadowMapSizeScale.GetBool() ) {
		return -SHADOW_TIER_MIN;			// index of shift 0 (the base)
	}
	float ref = r_shadowMapSizeScaleRadius.GetFloat();
	if ( ref < 1.0f ) {
		ref = 1.0f;
	}
	float f = radius / ref;
	if ( f < 1e-4f ) {
		f = 1e-4f;
	}
	const float log2f_ = idMath::Log( f ) / idMath::Log( 2.0f );			// log base 2
	int shift = idMath::Ftoi( floorf( log2f_ + 0.5f ) );				// round(log2(f))
	shift = idMath::ClampInt( SHADOW_TIER_MIN, SHADOW_TIER_MAX, shift );
	return shift - SHADOW_TIER_MIN;			// -> [0, SHADOW_NTIERS)
}

// Base size scaled by a tier's power-of-two shift, clamped to [lo, hi].
static int RB_RHI_TierSize( int base, int tierIdx, int lo, int hi ) {
	const int shift = tierIdx + SHADOW_TIER_MIN;
	int size = ( shift >= 0 ) ? ( base << shift ) : ( base >> ( -shift ) );
	return idMath::ClampInt( lo, hi, size );
}

// Fetch (or lazily create) the pooled render target for a tier at the given size.
// Dedupes clamp collisions: if another live slot already holds this size, reuse it
// so the [lo,hi] clamp never allocates two identical targets. isCube picks the pool
// and the create call. Returns 0 only if creation failed.
static rhi::RenderTargetHandle RB_RHI_ShadowPoolTarget( rhi::RHI *r, bool isCube, int tierIdx, int size ) {
	shadowRtSlot_t *pool = isCube ? rhiShadowCubePool : rhiShadowMapPool;
	shadowRtSlot_t &slot = pool[tierIdx];

	// live and correct already?
	if ( slot.rt && slot.size == size && r->GetRenderTargetImage( slot.rt ) != 0 ) {
		return slot.rt;
	}
	// reuse an identical-size live slot (clamp collision) instead of allocating twice
	for ( int i = 0; i < SHADOW_NTIERS; i++ ) {
		if ( i != tierIdx && pool[i].rt && pool[i].size == size
		     && r->GetRenderTargetImage( pool[i].rt ) != 0 ) {
			slot = pool[i];
			return slot.rt;
		}
	}
	// (re)create this slot; drop a stale/wrong-size handle first, but not if another
	// slot is sharing it (clamp collision) — only destroy handles this slot owns
	if ( slot.rt ) {
		bool shared = false;
		for ( int i = 0; i < SHADOW_NTIERS; i++ ) {
			if ( i != tierIdx && pool[i].rt == slot.rt ) {
				shared = true;
				break;
			}
		}
		if ( !shared ) {
			r->DestroyRenderTarget( slot.rt );
		}
		slot.rt = 0;
	}
	slot.rt = isCube ? r->CreateRenderTargetCube( rhi::IF_DEPTH24, size )
	                 : r->CreateRenderTarget( rhi::IF_DEPTH24, size, size );
	slot.size = size;
	RB_RHI_ForgetTexBinds();				// create() disturbed unit 0's cached bind
	return slot.rt;
}

// r_shadowMapDebug >= 2: count a nextOnLight drawSurf chain (occluders / receivers).
static int RB_RHI_CountLightChain( const drawSurf_t *surf ) {
	int n = 0;
	for ( ; surf; surf = surf->nextOnLight ) {
		n++;
	}
	return n;
}

// Creating a render target binds a texture on unit 0 directly (bypassing the
// RB_RHI_BindUnit cache in backEnd.glState.tmu), then unbinds it. Afterwards that
// cache disagrees with GL, so the next interaction can skip a needed rebind and later
// lights sample an absent unit-0 texture — the "changing a shadow setting breaks all
// lights" symptom on a live resolution change. Forget the cached binds so they re-issue.
// Vulkan (Phase 4 M3): RB_RHI_BindUnit records image handles here instead of
// touching GL; RB_RHI_VkTextures copies them into a draw's DrawArgs. Handles
// persist across draws exactly like GL binds do.
static rhi::ImageHandle rhiVkUnits[16];	// units 0-7 + shadow cube 8 + SSAO 9 + occlusion 10 + parallax 11 + dynamic shadow cube 12
										// + 13/14 set straight on DrawArgs + tess height 15 (DUDE)

static void RB_RHI_VkTextures( rhi::DrawArgs &da ) {
	if ( rhi::GetActiveBackendType() != rhi::BT_VULKAN ) {
		return;
	}
	for ( int i = 0; i < 8; i++ ) {
		da.textures[i] = rhiVkUnits[i];
	}
	da.shadowCube = rhiVkUnits[8];
	da.ssao = rhiVkUnits[9];
	da.occlusion = rhiVkUnits[10];
	da.parallax = rhiVkUnits[11];
	da.shadowCubeDyn = rhiVkUnits[12];
	da.tessHeight = rhiVkUnits[15];		// DUDE signed tess displacement height
}

static void RB_RHI_ForgetTexBinds() {
	for ( int i = 0; i < MAX_MULTITEXTURE_UNITS; i++ ) {
		backEnd.glState.tmu[i].current2DMap = -1;
		backEnd.glState.tmu[i].current3DMap = -1;
		backEnd.glState.tmu[i].currentCubeMap = -1;
	}
	memset( rhiVkUnits, 0, sizeof( rhiVkUnits ) );
}

/*
===================
RB_RHI_BindUnit
===================
*/
static void RB_RHI_BindUnit( int unit, idImage *image ) {
	// Vulkan: demand-load and record the handle for RB_RHI_VkTextures; no GL
	if ( rhi::GetActiveBackendType() == rhi::BT_VULKAN ) {
		if ( unit >= 0 && unit < 16 && image != NULL ) {
			image->Bind();		// upload trigger only under Vulkan
			if ( image->rhiHandle == 0 ) {
				// a white fallback silently breaks shading (e.g. a white
				// normalization cube map inverts the whole diffuse term), so
				// say which image failed to bridge — once per image
				static int warned = 0;
				if ( warned < 16 ) {
					warned++;
					common->Warning( "VK: no RHI image for '%s' (unit %d) - using white",
					                 image->imgName.c_str(), unit );
				}
			}
			rhiVkUnits[unit] = image->rhiHandle ? image->rhiHandle
			                                    : globalImages->whiteImage->rhiHandle;
		}
		return;
	}

	// Skip the active-unit switch and rebind when this image is already bound
	// on this unit. Under one light the normalization cube, light falloff /
	// projection and specular-table maps are identical across every surface —
	// only bump/diffuse/specular change — so most of the 7 per-draw binds in
	// the interaction pass are redundant. idImage::Bind already caches the
	// glBindTexture, but the glActiveTexture around it was issued regardless.
	const tmu_t *tmu = &backEnd.glState.tmu[unit];
	if ( image->texnum != idImage::TEXTURE_NOT_LOADED ) {
		if ( ( image->type == TT_2D   && tmu->current2DMap   == image->texnum ) ||
		     ( image->type == TT_CUBIC && tmu->currentCubeMap == image->texnum ) ||
		     ( image->type == TT_3D   && tmu->current3DMap   == image->texnum ) ) {
			return;
		}
	}
	rhi::gl3ActiveTexture( GL_TEXTURE0 + unit );
	backEnd.glState.currenttmu = unit;
	image->Bind();
}

// bind an already-resolved RHI image handle on a multitexture unit for the
// post/SSAO/SSR passes. GL3: raw glActiveTexture+bind (these targets aren't
// idImages). Vulkan: record into rhiVkUnits so RB_RHI_VkTextures carries it into
// the next draw's DrawArgs (units 1-7 -> textures[], 9 -> ssao, 10 -> occ). Used
// directly for an MRT second attachment (GetRenderTargetImage2), which has no
// RenderTargetHandle of its own.
static void RB_RHI_BindRTImage( rhi::RHI *r, int unit, rhi::ImageHandle img ) {
	if ( rhi::GetActiveBackendType() == rhi::BT_VULKAN ) {
		if ( unit >= 0 && unit < 12 ) {
			rhiVkUnits[unit] = img;
		}
		return;
	}
	rhi::gl3ActiveTexture( GL_TEXTURE0 + unit );
	qglBindTexture( GL_TEXTURE_2D, (GLuint)img );
	rhi::gl3ActiveTexture( GL_TEXTURE0 );
	backEnd.glState.currenttmu = 0;
}

// bind a render-target texture (GetRenderTargetImage handle) on a multitexture unit.
static void RB_RHI_BindRTUnit( rhi::RHI *r, int unit, rhi::RenderTargetHandle rt ) {
	RB_RHI_BindRTImage( r, unit, r->GetRenderTargetImage( rt ) );
}

// GTAO render targets (docs/ssao-gtao.md). Two RGBA8 screen-space buffers: the raw
// horizon-search output and the bilaterally-blurred result the ambient pass / debug
// overlay consume. R = ambient visibility, GBA = view-space bent normal. Rebuilt on
// resolution change and invalidated on a lost context (vid_restart). Declared up here
// because the ambient pass (RB_RHI_DrawInteraction, below) samples the result.
static rhi::RenderTargetHandle rhiSsaoRT     = 0;	// raw AO, then the separable-blur ping-pong
static rhi::RenderTargetHandle rhiSsaoBlurRT = 0;	// the other ping-pong buffer
static rhi::RenderTargetHandle rhiSsaoResultRT = 0;	// whichever holds the finished AO this frame
static int  rhiSsaoW = 0, rhiSsaoH = 0;				// AO buffer size (may be < view for half-res)
static int  rhiSsaoViewW = 0, rhiSsaoViewH = 0;		// full view size the AO covers
static bool rhiSsaoAppliedThisView = false;			// AO was produced for the view being drawn

// r_decalMetalMask: this view's screen-space metal retention under decals (RB_RHI_DecalMaskPass);
// 0 = none, interactions keep full metalness. Up here because RB_RHI_DrawInteraction binds it.
static rhi::ImageHandle rhiDecalMaskImage = 0;

// DUDE wet blood film (r_decalWetBlood, docs/readme-changes.md 1.7). This view's blood decals,
// collected by RB_RHI_DecalMaskPass and lit once per light by RB_RHI_DrawFilmInteractions as
// surfaces of their own. rhiFilmDraw is set while one of them is being submitted, so
// RB_RHI_DrawInteraction knows the draw is a film and not the material it nominally belongs to.
struct rhiFilmSurf_t {
	const drawSurf_t *		surf;
	const shaderStage_t *	cover;			// the unlit blend stage that draws the blood
	const shaderStage_t *	reliefStage;	// the material's own bump stage (for its matrix), or NULL
	idImage *				relief;			// normal map of the blood's own height
	float					mode;			// 1 darken (zero, 1-src_color), 2 filter: interaction.frag's film mode
	idBounds				bounds;			// local space, for the per-light cull
};
static idList<rhiFilmSurf_t>	rhiFilmSurfs;
static const rhiFilmSurf_t *	rhiFilmDraw = NULL;

// the frontend lights this surface itself (a map-placed decal whose material authors bump +
// specular, as the texture packs do): the film replaces that highlight rather than doubling it.
// din->surf is a per-light copy, so match on what it shares with the ambient surface.
static bool RB_RHI_SurfHasFilm( const drawSurf_t *surf ) {
	for ( int i = 0; i < rhiFilmSurfs.Num(); i++ ) {
		if ( rhiFilmSurfs[i].surf->material == surf->material && rhiFilmSurfs[i].surf->space == surf->space ) {
			return true;
		}
	}
	return false;
}

// SSAO Phase 1 prefiltered depth mip chain (docs/ssao-perf-optimization.md, r_ssaoDepthMip).
// A single AO-resolution RGBA16F target holding LINEAR view-space eye depth in R with a
// render-generated mip chain: level 0 is written by ssao_depthmip.frag, coarser levels by
// ssao_depthdown.frag (a max/farthest downsample, avoiding the fg/bg averaging halo).
// ssao.frag's horizon march reads a coarser mip for farther steps, so far taps touch a
// small cache-local footprint. Rebuilt on resize / lost context; freed when toggled off.
static rhi::RenderTargetHandle rhiSsaoDepthMipRT = 0;
static int  rhiSsaoDepthMipW = 0, rhiSsaoDepthMipH = 0;	// == AO buffer size
static int  rhiSsaoDepthMipLevels = 0;					// mip count (0 = not built)

// GTAO temporal accumulation (docs/ssao-gtao.md, r_ssaoTemporal). Two ping-ponged history
// buffers hold the accumulated AO+bent so we can read last frame's result while writing
// this one; the resolve reprojects it by camera motion (the shared rhiTemporalCam state
// below) and clamps to the local current-frame range. History is invalidated on resize /
// lost context / temporal toggle so a re-enable never blends stale data.
static rhi::RenderTargetHandle rhiSsaoHistRT[2] = { 0, 0 };
static int  rhiSsaoHistIdx  = 0;					// which history slot receives this frame's resolve
static int  rhiSsaoHistW = 0, rhiSsaoHistH = 0;		// history buffer size (matches the AO buffer)
static bool rhiSsaoHistValid = false;				// the read slot holds a usable previous frame
static float rhiSsaoJitterPhase = 0.0f;				// per-frame noise rotation (golden-ratio walk in [0,1))

// H4 RTAO (docs/rtx-rtao.md): the ray pass's noisy normalized-hit-distance target (R16F,
// view res). H4a produces it (one cosine hemisphere ray/pixel against the TLAS); H4c
// denoises it via NRD ReBLUR; H4d composites into the standard AO buffer. VK + RT only.
// r_rtShadowBlur (docs/rtx-shadow-blur.md): ONE set of view-sized targets shared by every
// blurred light of a view - each light's mask is built inside the light loop right before its
// interactions draw (the shadow maps share their target the same way), and nothing survives the
// frame. Ray = RGBA16F visibility / half-widths / view distance; Ping = the horizontal pass;
// Mask = the vertical pass = the finished visibility, single-channel R16F because every lit
// fragment of the light samples it (a quarter of the bytes of the RGBA16F working targets);
// Tile = the 8x8 edge classification (raw, then reach).
static rhi::RenderTargetHandle rhiRtBlurRayRT = 0, rhiRtBlurPingRT = 0, rhiRtBlurMaskRT = 0, rhiRtBlurTileRT[2] = { 0, 0 };
static int   rhiRtBlurW = 0, rhiRtBlurH = 0;
static bool  rhiRtBlurThisView = false;
static struct {
	float				invView[16];
	float				viewProj[16];		// this view's world-to-clip (jitter included): stored with a staggered mask
	rhi::ShaderHandle	rayProg, tileProg, blurProg;
} rhiRtBlur;
// r_rtShadowBlurStagger: lights far from the viewer refresh their mask every 2nd / 3rd frame. That
// needs what the shared targets deliberately don't have - a mask that outlives the frame - so each
// staggered light owns a view-sized RGBA8 mask (visibility + the view distance it was traced for)
// plus the camera it was made with. Slots are keyed by the light's persistent index and dropped
// after a few seconds unused.
#define RT_BLUR_MAX_SLOTS 64
#define RT_BLUR_SLOT_IDLE_FRAMES 300
struct rtBlurSlot_t {
	int						lightIndex;
	rhi::RenderTargetHandle	maskRT;
	int						lastUpdateFrame;	// rhiRtBlurFrame when the mask was last rendered
	int						lastUsedFrame;		// 0 = free slot
	unsigned long long		poseTok;			// light pose + size the mask was made for
	float					viewProj[16];
};
static rtBlurSlot_t rhiRtBlurSlots[RT_BLUR_MAX_SLOTS];
static int  rhiRtBlurFrame = 0;				// main views that ran the blur
static bool rhiRtBlurLastStale = false;		// out-params of the last RB_RHI_RtShadowBlurLight
static float rhiRtBlurLastViewProj[16];
static rhi::RenderTargetHandle rhiRtaoRayRT = 0;
static int   rhiRtaoW = 0, rhiRtaoH = 0;			// the RESOLUTION the ray pass + NRD run at (scaled by r_rtaoResScale)
static rhi::RenderTargetHandle rhiRtaoVelRT = 0;	// r_rtaoResScale < 1: full-res velocity downsampled to the ray/NRD resolution for NRD's IN_MV
static int   rhiRtaoVelW = 0, rhiRtaoVelH = 0;
static bool  rhiRtaoRanThisView = false;			// ray output produced this view (overlay gate)
// H4b guide inputs + H4c denoise state: NRD's IN_VIEWZ (signed linear Z, R16F) and
// IN_NORMAL_ROUGHNESS (world-space encoding-2 pack, RGBA16F) targets, plus the previous
// frame's camera for the denoiser's reprojection (its own staging — the shared temporal
// cam state advances on a different schedule).
static rhi::RenderTargetHandle rhiRtaoResolveRT = 0;	// H4d: the composited standard-AO buffer
static int   rhiRtaoResolveW = 0, rhiRtaoResolveH = 0;	// resolve target is full-res, tracked separately from the scaled ray/NRD size
static bool  rhiRtaoDenoisedThisView = false;		// NRD ran: GetRtaoOutputImage is current
static float rhiRtaoPrevProj[16];
static float rhiRtaoPrevView[16];
static float rhiRtaoPrevJitter[2] = { 0.0f, 0.0f };
static bool  rhiRtaoPrevValid = false;

// H6 RT GI (docs/rtx-rtgi.md): the radiance-gather ray target (attachment 0 RGBA16F radiance, 1/2 the
// same NRD guides RTAO writes). Full-res in H6a (no NRD / resScale yet); rhiRtgiRanThisView gates the
// r_rtgiDebug overlay. With r_rtgi 0 (default) none of this allocates.
static rhi::RenderTargetHandle rhiRtgiRayRT = 0;
static int  rhiRtgiW = 0, rhiRtgiH = 0;
static bool rhiRtgiRanThisView = false;
// H6c: the fullscreen GI apply is called once per view but from two sites (before FSR2's opaque snapshot
// so the additive bounce is IN the snapshot and FSR2's auto-reactive mask doesn't flag it as flash/particle
// content — the "square" artifact; and a fallback after post when SSR is off and no snapshot fires). This
// makes RB_RHI_RtgiApply idempotent per view so whichever site fires first wins and it never double-adds.
static bool rhiRtgiAppliedThisView = false;
// H6b (docs/rtx-rtgi.md): NRD REBLUR_DIFFUSE denoise state — a reduced-res velocity for IN_MV and the
// previous-frame camera for the reprojection, mirroring the rhiRtao* denoise statics.
static rhi::RenderTargetHandle rhiRtgiVelRT = 0;
static int  rhiRtgiVelW = 0, rhiRtgiVelH = 0;
// H6f: per-pixel history-confidence for NRD (IN_DIFF_CONFIDENCE), computed from this frame's raw bounce vs
// last frame's reprojected denoised GI — kills edge ghosting without the flat-area dimming of a global antilag.
static rhi::RenderTargetHandle rhiRtgiConfRT = 0;
static int  rhiRtgiConfW = 0, rhiRtgiConfH = 0;
// H6g: full-res NRD guides (viewZ + normal) for checkerboard mode, where the radiance is half-packed and can
// no longer carry the guides at its own resolution. Reuses the gather MRT layout (att1 viewZ, att2 normal).
static rhi::RenderTargetHandle rhiRtgiGuideRT = 0;
static int  rhiRtgiGuideW = 0, rhiRtgiGuideH = 0;
static bool rhiRtgiDenoisedThisView = false;		// the GI NRD denoise ran; GetRtgiOutputImage is current
static float rhiRtgiPrevProj[16];
static float rhiRtgiPrevView[16];
static float rhiRtgiPrevJitter[2] = { 0.0f, 0.0f };
static bool  rhiRtgiPrevValid = false;

// ---- shared camera temporal state (docs/fsr-temporal-pipeline.md, increment A1) ----
// The SSAO and SSR temporal resolves both reproject last frame's history by pure camera
// motion, so both need the previous rendered frame's world->clip. They kept two private
// copies before; this is the single source they now share (and that motion-vector
// generation + FSR2 will consume). Only the main fullscreen view reaches either resolve
// (both early-out on !viewEntitys / isSubview / !fullscreen), so exactly one view updates
// this per rendered frame.
//
// prevViewProj holds the PREVIOUS frame's world->clip; it is advanced from a staged copy
// exactly once per rendered frame (frame-count guarded) so the second consumer in a frame
// still reprojects against the previous frame, not this frame's freshly-staged value. The
// UN-jittered projection is staged so temporal jitter (increment B) never smears the
// history. historyReset flags a camera cut / teleport (origin jump > r_temporalResetDist)
// so the consumers drop stale history instead of leaning on the neighbourhood clamp.
struct rhiTemporalCam_t {
	float	prevViewProj[16];		// previous rendered frame's world->clip (un-jittered proj*view)
	float	stagedViewProj[16];		// this frame's world->clip, promoted to prev next frame
	idVec3	prevOrigin;				// previous frame's view origin (discontinuity detector)
	idVec3	stagedOrigin;
	int		stagedFrame;			// tr.frameCount of the last stage (once-per-frame advance guard)
	bool	havePrevVP;				// prevViewProj holds a usable previous frame
	bool	haveStaged;
	bool	historyReset;			// this frame is a discontinuity (cut / teleport)
};
static rhiTemporalCam_t rhiTemporalCam;		// zero-initialised: haveStaged / havePrevVP start false

// defined with the other matrix helpers further down; forward-declared for the temporal
// helpers here (myGlMultMatrix already comes from a header)
static bool R_InvertGLMatrix( const float in[16], float out[16] );

// Advance prev<-staged once per rendered frame, classify a discontinuity, and build this
// frame's reprojection (current view space -> previous frame's clip). Returns whether the
// current view matrix inverted (reproj is valid); *prevUsable is true when a usable
// previous frame exists AND this frame is not a cut.
static bool RB_RHI_TemporalReproj( const viewDef_t *viewDef, float reproj[16], bool *prevUsable ) {
	rhiTemporalCam_t &tc = rhiTemporalCam;

	if ( tc.stagedFrame != tr.frameCount ) {			// first temporal consumer of a new frame
		if ( tc.haveStaged ) {
			memcpy( tc.prevViewProj, tc.stagedViewProj, sizeof( tc.prevViewProj ) );
			tc.prevOrigin = tc.stagedOrigin;
			tc.havePrevVP = true;
		}
		const float resetDist = r_temporalResetDist.GetFloat();
		tc.historyReset = tc.havePrevVP && resetDist > 0.0f
			&& ( viewDef->renderView.vieworg - tc.prevOrigin ).LengthSqr() > resetDist * resetDist;
		tc.haveStaged  = false;
		tc.stagedFrame = tr.frameCount;
	}

	float invViewCur[16];
	const bool haveInv = R_InvertGLMatrix( viewDef->worldSpace.modelViewMatrix, invViewCur );
	if ( haveInv ) {
		myGlMultMatrix( invViewCur, tc.prevViewProj, reproj );
	}
	*prevUsable = tc.havePrevVP && !tc.historyReset;
	return haveInv;
}

// Record this frame's world->clip (un-jittered) as next frame's "previous"; called by a
// temporal resolve once it commits, so prev advances exactly once per rendered frame.
static void RB_RHI_TemporalStageCur( const viewDef_t *viewDef ) {
	rhiTemporalCam_t &tc = rhiTemporalCam;
	myGlMultMatrix( viewDef->worldSpace.modelViewMatrix, viewDef->unjitteredProjectionMatrix, tc.stagedViewProj );
	tc.stagedOrigin = viewDef->renderView.vieworg;
	tc.haveStaged   = true;
}

// Drop the shared camera history (lost context / target realloc).
static void RB_RHI_TemporalResetCam( void ) {
	rhiTemporalCam.havePrevVP   = false;
	rhiTemporalCam.haveStaged   = false;
	rhiTemporalCam.historyReset = false;
	rhiTemporalCam.stagedFrame  = -1;
}

// R1/C2: the FSR2 dispatch (RhiBackend) consumes the shared discontinuity signal. Runs the
// idempotent per-frame classify, then stages this frame unconditionally so prev keeps
// advancing (and cuts keep being detected) even when no temporal SSAO/SSR consumer is
// active. True = drop FSR2's history: first frame, lost history, or a cut/teleport.
bool RB_RHI_TemporalFsrReset( const viewDef_s *viewDef ) {
	float reproj[16];
	bool prevUsable = false;
	RB_RHI_TemporalReproj( viewDef, reproj, &prevUsable );
	RB_RHI_TemporalStageCur( viewDef );
	return !prevUsable;
}

// ---- per-object motion-vector cache (docs/fsr-temporal-pipeline.md, increment A2) ----
// viewEntity_t is frame-temporary, so to build per-object motion vectors we copy each drawn
// space's model->clip out of the normal prepass and read last frame's value back next frame.
// Keyed by entityDef->index (dense, bounded by peak live entities). The world/BSP shares
// viewDef->worldSpace (entityDef == NULL) under a reserved key, so camera motion falls out of
// the exact same path — worldSpace.modelViewMatrix is the pure view matrix, so its stored MVP
// is the world->clip A1's camera reprojection also tracks. Rigid transform only: GPU-skinned /
// tessellation-deformed vertices reuse the rigid prev matrix (documented v1 limitation).
//
// The normal prepass runs once per rendered frame, primary fullscreen view only (it early-outs
// on isSubview / non-fullscreen), so each entity appears at most once here and the index key is
// unambiguous — mirrors/subviews never reach this cache. A per-entry frameCount stamp evicts
// despawned / not-seen-last-frame entities: their next appearance starts at zero velocity
// (prevMvp := curMvp) instead of a bogus jump from a stale matrix.
struct rhiPrevModel_t {
	float		mvp[16];	// previous frame's model->clip (the RB_RHI_SpaceMvp value)
	const void	*entityDef;	// identity guard: the idRenderEntityLocal* this slot held last frame.
							// entityDef->index is a world slot handle FindNull reuses the instant an
							// entity is freed, so without this a despawn+respawn into the same slot on
							// consecutive frames would feed the new entity its predecessor's matrix
							// (a one-frame bogus velocity — e.g. a rocket that spawns debris on impact).
	int			frame;		// tr.frameCount when last written
	bool		valid;		// mvp holds a real matrix (distinguishes a never-written slot)
};
static idList<rhiPrevModel_t>	rhiPrevModels;		// [0] = worldSpace; [entityDef->index + 1] = entities
// previous rendered frame's applied projection jitter (pixels, +Y-up), for the R1/B velocity
// jitter-cancellation. Advanced once per frame in the normal prepass (primary fullscreen view).
static float rhiPrevJitter[2] = { 0.0f, 0.0f };

// Read this space's previous-frame MVP into prevMvp and stage curMvp for next frame. When no
// usable previous frame exists (first sighting / despawn gap / a second primary view in one
// frame) prevMvp is set to curMvp so the shader writes zero velocity. Called once per space.
static void RB_RHI_MotionPrevMvp( const viewEntity_t *space, const float curMvp[16], float prevMvp[16] ) {
	const int key = space->entityDef ? space->entityDef->index + 1 : 0;
	if ( key < 0 ) {						// paranoia: a negative index would corrupt the list
		memcpy( prevMvp, curMvp, sizeof( float ) * 16 );
		return;
	}
	if ( key >= rhiPrevModels.Num() ) {
		rhiPrevModel_t empty;
		memset( &empty, 0, sizeof( empty ) );
		while ( rhiPrevModels.Num() <= key ) {
			rhiPrevModels.Append( empty );
		}
	}
	rhiPrevModel_t &e = rhiPrevModels[key];
	// usable = written on the immediately preceding rendered frame BY THE SAME ENTITY. The pointer
	// guard rejects a reused slot (a fresh idRenderEntityLocal is a different pointer), so a
	// despawn+respawn into the same index starts at zero velocity instead of inheriting the
	// previous occupant's matrix. Any older stamp is a despawn gap / first sighting / a duplicate
	// view this frame -> also zero velocity. (worldSpace is entityDef==NULL both sides -> matches.)
	if ( e.valid && e.frame == tr.frameCount - 1 && e.entityDef == (const void *)space->entityDef ) {
		memcpy( prevMvp, e.mvp, sizeof( float ) * 16 );
	} else {
		memcpy( prevMvp, curMvp, sizeof( float ) * 16 );
	}
	memcpy( e.mvp, curMvp, sizeof( float ) * 16 );
	e.entityDef = (const void *)space->entityDef;
	e.frame = tr.frameCount;
	e.valid = true;
}

// Drop all cached matrices (lost context / target realloc). The frame stamp already evicts
// despawns, but a vid_restart can reuse indices, so wipe on the same hook as the camera state.
static void RB_RHI_MotionResetCache( void ) {
	rhiPrevModels.SetNum( 0 );
	rhiPrevJitter[0] = rhiPrevJitter[1] = 0.0f;
}

// SSR render targets (docs/ssr.md, Phase C.2.1). RGBA16F so reflected HDR energy
// survives the intermediate; the march renders at r_ssrResScale of the view and the
// composite upsamples with full-res Fresnel/gloss weighting. Temporal accumulation
// mirrors the SSAO history machinery above (own matrices — different pass timing).
static rhi::RenderTargetHandle rhiSsrRT = 0;		// marched reflection color (a = hit mask)
static int  rhiSsrW = 0, rhiSsrH = 0;				// march buffer size
static rhi::RenderTargetHandle rhiSsrHistRT[2] = { 0, 0 };	// temporal history ping-pong
static int  rhiSsrHistIdx = 0;						// which slot receives this frame's resolve
static int  rhiSsrHistW = 0, rhiSsrHistH = 0;
static bool rhiSsrHistValid = false;				// the read slot holds a usable previous frame
static float rhiSsrJitterPhase = 0.0f;				// per-frame march jitter rotation

// RR6 RT reflections (docs/rtx-reflections.md): the RT reflection (ssr_rt) used to composite one
// sharp ray straight onto the scene, so where SSR persistently misses (grout lines) it aliased into a
// firefly grid. Give it the SAME temporal treatment SSR gets: render the RT contribution into its own
// target (a = hit mask), accumulate it across frames through ssr_temporal against a reprojected history
// ping-pong, then additively composite the smoothed result. Sized to the SSR march resolution so the
// two layers share a grain (and scale together with r_ssrResScale). A per-frame ray jitter (ssr_rt,
// r_rtReflJitter) gives the accumulation something to average, so the grid resolves rather than smears.
static rhi::RenderTargetHandle rhiVolRT = 0;			// volumetric lighting march result (rgb in-scatter, a transmittance)
static int   rhiVolW = 0, rhiVolH = 0;
static rhi::RenderTargetHandle rhiVolBlurRT = 0;			// low-res depth-aware blur of the march (resolves the per-pixel dither)
static rhi::RenderTargetHandle rhiVolUpRT = 0;			// depth-aware upsample of the march (full res) = the temporal pass's "current" (OLD resolve path only)
static rhi::RenderTargetHandle rhiVolHistRT[2] = { 0, 0 };	// temporal history ping-pong (full res OLD path, LOW res fast path)
static int   rhiVolHistIdx = 0;
static int   rhiVolFullW = 0, rhiVolFullH = 0;			// allocated size of the history group (full OLD, low fast)
static bool  rhiVolHistValid = false;
static bool  rhiVolFastPrev = false;					// r_volFastResolve the history group was last sized for (toggling recreates)
static float rhiVolJitterPhase = 0.0f;

// Volumetric lighting lifts light everywhere inside lit volumes, which flattens contact shading, so while it
// is active the AO producers (GTAO / RTAO) use r_volAoIntensity when that is higher than their own
// intensity cvar. Nothing is written back: volumetrics off -> each producer's own value again. Same
// activity test as RB_RHI_VolumetricLighting (cvar + VK + ray query + HDR frame).
static float RB_RHI_AoIntensityWithVolumetrics( rhi::RHI *r, float ownIntensity ) {
	if ( r_volumetrics.GetBool() && r_volAoIntensity.GetFloat() > ownIntensity
			&& rhi::GetActiveBackendType() == rhi::BT_VULKAN && r->SupportsRayQuery() && RB_RHI_HdrCaptureActive() ) {
		return r_volAoIntensity.GetFloat();
	}
	return ownIntensity;
}					// per-frame golden-ratio rotation of the march sample offsets
static rhi::RenderTargetHandle rhiRtReflRT = 0;		// this frame's RT reflection contribution (a = hit mask)
// RR16 (docs/rtx-reflections.md): the view's lights gathered into a device buffer the fullscreen RT
// reflection pass loops (buffer_reference), so an off-screen-reflected hit is lit by the REAL scene
// lights, not a fixed key light. Ring across a few frame slots because UpdateBuffer is a bare coherent
// memcpy (no fence): writing a fresh slot each frame can't race the prior frame's in-flight GPU read.
#define RT_REFL_MAX_LIGHTS   256
#define RT_REFL_LIGHT_SLOTS  3
static rhi::BufferHandle    s_rtReflLightBuf  = 0;
static unsigned long long   s_rtReflLightAddr = 0;
static int                  s_sceneFogCount   = 0;	// stock fog-light volumes appended AFTER the lights in the current slot (volumetrics only)
#define SCENE_FOG_MAX        8
static int  rhiRtReflW = 0, rhiRtReflH = 0;
static rhi::RenderTargetHandle rhiRtReflHistRT[2] = { 0, 0 };	// temporal history ping-pong
static int  rhiRtReflHistIdx = 0;					// slot that receives this frame's resolve
static int  rhiRtReflHistW = 0, rhiRtReflHistH = 0;
static bool rhiRtReflHistValid = false;				// the read slot holds a usable previous frame

// SSR Hi-Z (docs/ssao-perf-optimization.md, r_ssrHiZ): a min-Z (nearest-surface) linear-
// depth mip chain at the SSR march resolution, so the march leaps provably-empty span.
// Same machinery as the SSAO depth mip, with a MIN downsample instead of MAX.
static rhi::RenderTargetHandle rhiSsrDepthMinRT = 0;
static int  rhiSsrDepthMinW = 0, rhiSsrDepthMinH = 0;	// == SSR march buffer size
static int  rhiSsrDepthMinLevels = 0;					// mip count (0 = not built)

// SSR glossy reflections (docs/ssr.md, r_ssrGlossy): a colour mip pyramid of the
// (temporally-accumulated) reflection buffer at the SSR march resolution. The composite
// samples it at a roughness-proportional LOD so rough surfaces blur. Same mipped-target
// machinery as the SSR Hi-Z above, but RGBA16F and an average (box) downsample.
static rhi::RenderTargetHandle rhiSsrColorMipRT = 0;
static int  rhiSsrColorMipW = 0, rhiSsrColorMipH = 0;	// == SSR march buffer size
static int  rhiSsrColorMipLevels = 0;					// mip count (0 = not built)

// DUDE berserk vision feedback trail (docs / memory berserk-vision-rhi, RB_RHI_BerserkAccum).
// A ping-pong RGBA8 pair reproducing the stock ARB material's recursive _scratch feedback the
// RHI path can't accumulate: each frame folds the freshly captured scene with the previous
// frame magnified ~3-5% about the centre, gated by the berserk2 radial mask (see
// berserk_accum.frag), so older frames fan out as streaks. Sized to the view. Invalidated on
// resize / lost context; a large time gap re-seeds so a re-entry starts a clean trail.
static rhi::RenderTargetHandle rhiBerserkTrailRT[2] = { 0, 0 };
static int  rhiBerserkIdx = 0;						// slot that received the last sample is 1 - this
static int  rhiBerserkW = 0, rhiBerserkH = 0;		// trail buffer size
static bool rhiBerserkValid = false;				// a usable previous trail exists (history read ok)
static int  rhiBerserkLastTick = -100000;			// Sys_Milliseconds of the last accumulation sample

// DUDE hell-time / Artifact vision (D3XP FullscreenFX_Helltime, RB_RHI_HelltimeAccum). Its own
// ping-pong pair, separate from berserk's, reproducing the stock recursive _accum zoom-feedback
// (textures/smf/bloodorb{1,2,3}) the RHI can't accumulate — the same class of failure as _scratch.
// Per-level tint/scale/rotation + the inverted bloodorb3 radial mask (see helltime_accum.frag).
static rhi::RenderTargetHandle rhiHelltimeTrailRT[2] = { 0, 0 };
static int  rhiHelltimeIdx = 0;
static int  rhiHelltimeW = 0, rhiHelltimeH = 0;
static bool rhiHelltimeValid = false;
static int  rhiHelltimeLastTick = -100000;

// Normal G-buffer (Option B): bump-mapped view-space normals written by an extra opaque
// geometry pass, so SSAO uses real per-pixel normals instead of reconstructing from depth.
static rhi::RenderTargetHandle rhiNormalRT = 0;		// RGBA8 view-normal + depth (color+depth target)
// the normal buffer SSAO/SSR/debug actually sample this view: rhiNormalRT (standalone pass)
// or the merged handle from BeginNormalPrepass (r_ssaoMergeNormal). Both expose the normal
// (GetRenderTargetImage) + the SSR rough/metal MRT (GetRenderTargetImage2) when rhiNormalMrt.
static rhi::RenderTargetHandle rhiNormalResultRT = 0;
static int  rhiNormalW = 0, rhiNormalH = 0;			// full view resolution
static bool rhiNormalReadyThisView = false;			// the normal buffer was produced for this view
// CACHE KEYS of the persistent standalone target (what rhiNormalRT was CREATED with).
// Written only by EnsureNormalTarget + the full context reset - NEVER per view: resetting
// them each view forced a key mismatch and a full-res target destroy/recreate EVERY FRAME
// whenever the velocity path ran (found via the gbuffer DIAG spam; silent since R1/A2).
static bool rhiNormalMrt = false;					// target has the SSR rough/metal attachment (docs/ssr.md)
static bool rhiNormalVel = false;					// target has the RG16F velocity attachment (R1/A2)
static bool rhiNormalAlbedo = false;				// target has the RGBA8 RTGI receiver-albedo attachment (docs/rtx-rtgi.md H6c)
// what this view's ACTIVE normal result (standalone target OR merged handle) carries -
// per-view state, reset each world draw, consumed by SSAO/SSR/FSR2
static bool rhiNormalMrtThisView = false;
static bool rhiNormalVelThisView = false;
static bool rhiNormalAlbedoThisView = false;		// RTGI H6c: this view's normal target carries the albedo attachment (GetRenderTargetImage4)

// ---- per-surface occlusion-map auto-load cache (docs/occlusion-maps.md) ----
// Keyed by (render model, surface index) -- stable per model surface and shared across every
// instance, so it is immune to skins and to materials shared across different models (the
// whack-a-mole the material-name scheme hit). NOTE: the draw-time surf->geo is a per-light
// culled copy (lightTris), NOT the model surface geometry, so we identify the surface by
// matching the (skin-remapped) draw material back to a model surface instead. Each entry
// caches the resolved AO idImage (NULL = "checked, none"); idImage objects persist across
// vid_restart, so caching the pointer is safe. bakeAO* clears this via R_ResetOcclusionMapCache.
struct rhiAoCacheEntry_t { const void *model; int surfIndex; idImage *img; };
static idList<rhiAoCacheEntry_t> rhiAoCache;
static idHashIndex               rhiAoCacheHash;
static idStrList                 rhiAoBakedModels;	// model|skin the lazy path already attempted

void R_ResetOcclusionMapCache( void ) {
	rhiAoCache.Clear();
	rhiAoCacheHash.Free();
	rhiAoBakedModels.Clear();
}

extern idCVar r_occlusionMapPrecache;
static bool RB_RHI_AoFileExists( const char *path ) {
	ID_TIME_T ts;
	return fileSystem->ReadFile( path, NULL, &ts ) >= 0;
}

// Load a resolved (extensionless) AO image name, treating the default checkerboard as "none".
static idImage *RB_RHI_LoadAo( const char *name ) {
	idImage *img = globalImages->ImageFromFile( name, TF_DEFAULT, true, TR_CLAMP, TD_HIGH_QUALITY );
	return ( img == globalImages->defaultImage ) ? NULL : img;
}

// Mod-friendly "_ao" naming convention (docs/occlusion-maps.md): if the surface's diffuse map
// is textures/x/foo (or foo_d), and a sibling textures/x/foo_ao (.tga/.dds) exists, use it.
// Lets a modpack drop in AO textures without editing materials. Returns NULL if none found.
static idImage *RB_RHI_ModAoConvention( const idMaterial *mat ) {
	const char *diffuse = NULL;
	for ( int i = 0; i < mat->GetNumStages(); i++ ) {
		const shaderStage_t *st = mat->GetStage( i );
		if ( st->lighting == SL_DIFFUSE && st->texture.image ) {
			diffuse = st->texture.image->imgName.c_str();
			break;
		}
	}
	if ( !diffuse || !diffuse[0] || strchr( diffuse, '(' ) ) {
		return NULL;		// no plain diffuse (image programs like addnormals() are skipped)
	}

	idStr base = diffuse;
	base.StripFileExtension();
	idStr candidates[2];
	int n = 0;
	candidates[n++] = base + "_ao";			// foo -> foo_ao
	if ( base.Length() > 2 && idStr::Icmp( base.Right( 2 ).c_str(), "_d" ) == 0 ) {
		candidates[n++] = base.Left( base.Length() - 2 ) + "_ao";	// foo_d -> foo_ao
	}
	for ( int c = 0; c < n; c++ ) {
		if ( RB_RHI_AoFileExists( ( candidates[c] + ".tga" ).c_str() )
		  || RB_RHI_AoFileExists( ( candidates[c] + ".dds" ).c_str() ) ) {
			return RB_RHI_LoadAo( candidates[c].c_str() );
		}
	}
	return NULL;
}

// Resolve (and cache) this surface's occlusion map. Order: a mod-supplied <diffuse>_ao texture,
// else our generated per-model-surface bake (generated/aomaps/<model>_sN), optionally lazily
// baked when r_occlusionMapsAutoBake is on. An explicit occlusionmap material stage is handled
// by the caller and takes priority. Returns NULL when none.
//
// `mat` is the draw material (already skin/customShader remapped). We find which model surface
// it came from by remapping each surface's material the same way and matching -- reliable at
// draw time, where surf->geo is a per-light culled copy that never matches model geometry.
//
// `model` is the base model (stable, names the generated path); `snapshot` is the entity's
// instantiated dynamic model when `model` is dynamic (an MD5 character), else NULL. The base
// MD5 model exposes no surfaces, so for it we walk the snapshot and key by each surface's
// persistent id (== mesh index) -- exactly what AO_BakeModelToCache wrote for the bind pose.
// Static models are walked directly and keyed by surface position, as before.
static idImage *RB_RHI_SurfaceOcclusion( const idRenderModel *model, const idRenderModel *snapshot,
                                         const idMaterial *mat,
                                         const idDeclSkin *skin, const idMaterial *customShader ) {
	if ( !model || !mat ) {
		return NULL;
	}
	const idRenderModel *enumModel = model;
	bool keyById = false;
	if ( model->IsDynamicModel() != DM_STATIC && snapshot ) {
		enumModel = snapshot;		// base MD5 model has no surfaces; the snapshot carries them
		keyById = true;
	}
	int surfIndex = -1;
	for ( int s = 0; s < enumModel->NumSurfaces(); s++ ) {
		const modelSurface_t *ms = enumModel->Surface( s );
		const idMaterial *sm = ms->shader;
		if ( !sm ) {
			continue;
		}
		sm = R_RemapShaderBySkin( sm, skin, customShader );
		if ( sm && idStr::Icmp( sm->GetName(), mat->GetName() ) == 0 ) {
			surfIndex = keyById ? ms->id : s;
			break;
		}
	}
	if ( surfIndex < 0 ) {
		return NULL;
	}

	const int key = rhiAoCacheHash.GenerateKey( (int)( ( (size_t)model >> 4 ) & 0x7fffffff ), surfIndex );
	for ( int i = rhiAoCacheHash.First( key ); i != -1; i = rhiAoCacheHash.Next( i ) ) {
		if ( rhiAoCache[i].model == model && rhiAoCache[i].surfIndex == surfIndex ) {
			return rhiAoCache[i].img;
		}
	}

	idImage *img = RB_RHI_ModAoConvention( mat );	// (2) modpack _ao override

	if ( !img ) {									// (3) our generated per-model-surface bake
		idStr path;
		AO_GeneratedPathForSurface( model->Name(), surfIndex, path );
		bool exists = RB_RHI_AoFileExists( path.c_str() );

		// lazy bake (dev, r_occlusionMapsAutoBake): bake the whole model once per model+skin
		// (through the entity's skin, for the cavity bump), then re-probe. CPU + filesystem
		// only, so it's safe here; the one-time hitch is the documented cost of the toggle.
		idStr bakedKey = idStr( model->Name() ) + "|" + ( skin ? skin->GetName() : "" );
		if ( !exists && r_occlusionMapsAutoBake.GetBool()
		     && rhiAoBakedModels.FindIndex( bakedKey ) < 0 ) {
			rhiAoBakedModels.Append( bakedKey );
			AO_BakeModelToCache( model, skin );
			exists = RB_RHI_AoFileExists( path.c_str() );
		}
		if ( exists ) {
			img = RB_RHI_LoadAo( path.c_str() );
		}
	}

	rhiAoCacheEntry_t e;
	e.model = model;
	e.surfIndex = surfIndex;
	e.img = img;
	const int idx = rhiAoCache.Append( e );
	rhiAoCacheHash.Add( key, idx );
	return img;
}

/*
===================
RB_RHI_PrecacheOcclusionMapsForModel

DUDE benchmark/hitch fix (docs/benchmark.md): the baked occlusion maps (generated/aomaps/<model>_sN)
were loaded on the render thread the first time a light's interaction pass bound each one, so a demo
or a walk into a new area hitched ~7-27 ms per never-before-lit model. Called from the model
manager's EndLevelLoad (image manager still insideLevelLoad), this touches every existing AO file
for a loaded model so globalImages->EndLevelLoad then loads them on the parallel loader, resident
before play. Load only (never bakes: the r_occlusionMapsAutoBake dev path stays lazy). Same
ImageFromFile params as the draw-time RB_RHI_LoadAo so the exact same idImage is reused, not
duplicated. Respects the live cvars: off when occlusion maps or precaching are disabled, so the
in-game quality toggles keep working (a mid-game enable just falls back to the old lazy load).
===================
*/
void RB_RHI_PrecacheOcclusionMapsForModel( const idRenderModel *model ) {
	if ( !glConfig.rhiBackend || !model ) {
		return;
	}
	if ( !r_occlusionMaps.GetBool() || !r_occlusionMapPrecache.GetBool() ) {
		return;
	}
	// probe surfaces from 0 up; AO bakes are contiguous (static: surface index; MD5: mesh id),
	// so stop after a few consecutive misses. Cap keeps a pathological model bounded.
	const int MAX_SURF = 64;
	const int MISS_STOP = 4;
	int misses = 0;
	for ( int s = 0; s < MAX_SURF && misses < MISS_STOP; s++ ) {
		idStr path;
		AO_GeneratedPathForSurface( model->Name(), s, path );
		if ( !RB_RHI_AoFileExists( path.c_str() ) ) {
			misses++;
			continue;
		}
		misses = 0;
		// identical params to RB_RHI_LoadAo -> the resolve path finds this same image, already resident
		globalImages->ImageFromFile( path.c_str(), TF_DEFAULT, true, TR_CLAMP, TD_HIGH_QUALITY );
	}
}

/*
===================
R_PrecacheDecalImages

DUDE (docs/benchmark.md): decal materials (textures/decals, e.g. the blood splats) carry
generated bump images -- smoothnormals(heightmap(...)) -- computed on the CPU the first time the
decal is projected and bound, which stalled the render thread ~8-12 ms per new splat type. Walk the
already-parsed materials this level pulled in and force-load any unloaded decal-texture image up
front, on the main thread at load time (or, for a render demo, before timing starts). Only touches
parsed materials (so nothing unused is pulled in) and honors image_preload (off = keep lazy).
===================
*/
void R_PrecacheDecalImages( void ) {
	if ( !cvarSystem->GetCVarBool( "image_preload" ) ) {
		return;		// user opted into fully lazy image loading
	}
	const int n = declManager->GetNumDecls( DECL_MATERIAL );
	for ( int i = 0; i < n; i++ ) {
		const idDecl *d = declManager->DeclByIndex( DECL_MATERIAL, i, false );
		if ( !d || d->GetState() != DS_PARSED ) {
			continue;		// unreferenced this level -> don't parse/load it
		}
		const idMaterial *mat = static_cast<const idMaterial *>( d );
		for ( int s = 0; s < mat->GetNumStages(); s++ ) {
			const shaderStage_t *st = mat->GetStage( s );
			idImage *img = st->texture.image;
			if ( !img || img->texnum != idImage::TEXTURE_NOT_LOADED ) {
				continue;	// no image or already resident
			}
			// the generated bump program embeds the source path, so match the substring
			if ( strstr( img->imgName.c_str(), "textures/decals/" ) == NULL ) {
				continue;
			}
			// this decal is referenced by the level, so keep it: without the flag the
			// globalImages->EndLevelLoad purge that follows (real-play path) would drop it
			img->levelLoadReferenced = true;
			img->ActuallyLoadImage( true, false );	// main-thread, load-time: no timed-frame stall
		}
	}
}

/*
===================
RB_RHI_ResolvePbrMaterial

Resolve a material's effective PBR response (docs/pbr-materials.md), outputting all
four params: metalness, roughness, wetness (specular-energy multiplier) and env (metal
env-glow multiplier over the global r_pbrEnvScale). A category-tagged material tracks
its category row in the per-category defaults table (the Categories sliders); a pinned
(NONE) material carries its own baked/override values, with the global roughness
fallback. Per-material wetness/env override columns always win. Metalness comes back
sanity-clamped to [0,1]. Shared by the lit interaction fill and the SSR G-buffer pass
so both see the same surface response; returns the category.
===================
*/
static int RB_RHI_ResolvePbrMaterial( const idMaterial *mat, float &metal, float &rough,
                                      float &wet, float &env ) {
	const int pbrCat = mat ? mat->GetPbrCategory() : PBR_CAT_NONE;
	const float tblMetal = mat ? mat->GetPbrMetalness() : -1.0f;
	const float tblRough = mat ? mat->GetPbrRoughness() : -1.0f;
	const float matWet   = mat ? mat->GetPbrWetness() : -1.0f;
	const float matEnv   = mat ? mat->GetPbrEnv() : -1.0f;
	if ( pbrCat > PBR_CAT_NONE && pbrCat < PBR_CAT_COUNT ) {
		// category-tagged: the whole class tracks its row in the defaults table
		R_PbrCategoryDefaults( pbrCat, metal, rough, wet, env );
	} else {
		// pinned / long-tail: the material's own baked values, global roughness fallback
		metal = tblMetal >= 0.0f ? tblMetal : 0.0f;
		rough = tblRough >= 0.0f ? tblRough : r_pbrRoughness.GetFloat();
		wet   = 1.0f;
		env   = 1.0f;
	}
	// per-material override columns (5th/6th) win over the category defaults
	if ( matWet >= 0.0f ) wet = matWet;
	if ( matEnv >= 0.0f ) env = matEnv;
	metal = idMath::ClampFloat( 0.0f, 1.0f, metal );
	return pbrCat;
}

// two-output shim for callers that only need metalness/roughness (SSR G-buffer)
static int RB_RHI_ResolvePbrMaterial( const idMaterial *mat, float &metal, float &rough ) {
	float wet, env;
	return RB_RHI_ResolvePbrMaterial( mat, metal, rough, wet, env );
}

/*
===================
RB_RHI_TessellateSurf

DUDE tessellation (docs/tessellation.md): should this surface route through the
PN-triangle tessellation pipeline? True only on the Vulkan backend with
r_tessellation on, for a character/monster mesh — the worldspawn BSP (index 0),
static props and the first-person viewmodel are left flat. The opaque prepass +
interaction/ambient passes call with forBlendPass = false (they must agree under
depth-EQUAL, so translucent / pure-emissive surfaces are excluded); the blended
material pass calls with forBlendPass = true so blood-overlay decals — translucent,
projected onto the monster's model — PN-tessellate too.

Under Roadmap B deform-once (r_tessDeform), the BODY is drawn from a pre-deformed
buffer (tri->tessDeformVB), NOT this fixed-function path — RB_RHI_TessOrDeform /
RB_RHI_ApplyDeform substitute it and clear the tess flag. The forBlendPass = true
fixed-function path then only ever runs for a blood-overlay decal, which is a SEPARATE
surface with no deform buffer; it follows the base mesh's (welded) normals — welded on
tri->verts in idMD5Mesh::UpdateSurface so the decal rides the same PN surface as the
deformed body instead of clipping through it.
===================
*/
bool RB_RHI_TessellateSurf( const drawSurf_s *surfIn, bool forBlendPass ) {
	const drawSurf_t *surf = surfIn;
	if ( !r_tessellation.GetBool()
	     || rhi::GetActiveBackendType() != rhi::BT_VULKAN ) {
		return false;
	}
	const viewEntity_t *space = surf->space;
	if ( space == NULL || space->entityDef == NULL || space->entityDef->index == 0 ) {
		return false;
	}
	if ( space->weaponDepthHack || space->modelDepthHack != 0.0f ) {
		return false;
	}
	const idMaterial *mat = surf->material;
	if ( mat == NULL || mat->HasGui() ) {
		return false;
	}
	// Translucent surfaces don't seal depth, so they can't tessellate in the opaque
	// prepass/interaction passes (depth-EQUAL). The blended pass, though, draws them
	// directly — where following the tessellated base is exactly what a blood decal
	// needs — so allow them there.
	if ( !forBlendPass && mat->Coverage() == MC_TRANSLUCENT ) {
		return false;
	}
	// The blend pass tessellates conformal decal overlays (blood on the body) so they
	// follow the deformed mesh. But a character entity can ALSO carry view-oriented
	// additive glow/particle surfaces that inherit its model path — e.g. the RoE harvest
	// "soul" aura, emitted from the corpse material via `deform particle` as
	// textures/particles/ember_mid. Those are camera-facing sprite quads with degenerate
	// normals; PN-tessellating + inward-displacing them collapses/clips the quads and the
	// aura vanishes (Vulkan-only, only when r_tessellation is on — hence GL3 is unaffected).
	// Blood decals alpha-blend and conform to the mesh; glows/particles are purely additive,
	// so skip a surface whose ambient stages are ALL additive.
	if ( forBlendPass ) {
		bool anyAmbient = false, allAdditive = true;
		for ( int i = 0; i < mat->GetNumStages(); i++ ) {
			const shaderStage_t *st = mat->GetStage( i );
			if ( st->lighting != SL_AMBIENT ) {
				continue;
			}
			anyAmbient = true;
			if ( ( st->drawStateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) )
			     != ( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE ) ) {
				allAdditive = false;
				break;
			}
		}
		if ( anyAmbient && allAdditive ) {
			return false;
		}
	}
	const char *matName = mat->GetName();

	// Characters/monsters only, gated on the ENTITY's model path rather than the
	// surface material. A blood overlay is added to the monster's model but carries a
	// textures/decals material — the entity's model still resolves to the monster md5,
	// so it's recognised as monster geometry and can follow the deformed base. This is
	// also more robust than IsDynamicModel() for props (a knocked-over moveable becomes
	// a dynamic AF ragdoll and slipped a DM_STATIC test — that's how chair3 leaked).
	// md5 model files live under models/md5/monsters/ (monsters), models/md5/chars/
	// (human NPC bodies — security, marine, suit, labcoat...), models/md5/characters/
	// (NPC def_head heads: models/md5/characters/npcs/heads|zheads/...) and
	// models/md5/heads/ (the main-character def_heads — betruger, campbell, swann,
	// sarge, player). Match all four substrings so every human/monster body AND head
	// tessellates. Every md5mesh whose path contains "heads/" is a character/zombie
	// head (verified — no props or world geometry live there), so "heads/" is safe.
	// Props are models/mapobjects/. Cutscene actors live under models/md5/cinematics/
	// but so does cinematic SCENERY (rocks, walls, clouds, wall meshes) and the actor's
	// weapon props — so "cinematics/" can't be whitelisted wholesale. A cinematic ACTOR,
	// though, wears a real character/monster SKIN (models/characters/mcneil/body,
	// .../male_npc/soldier/soldier, models/monsters/imp/imp), while cinematic scenery and
	// weapon props do not. So admit a cinematics/ mesh only when its MATERIAL is a
	// character/monster skin — the intro McNeil + marine tessellate, the rocks stay flat.
	// (Blood-overlay decals still ride in on the model path: their monster model resolves
	// to monsters/… even though the decal material is textures/decals/….)
	const idRenderModel *entModel = space->entityDef->parms.hModel;
	const char *modelName = entModel ? entModel->Name() : "";
	const bool modelIsChar = idStr::FindText( modelName, "monsters/", false ) != -1
	  || idStr::FindText( modelName, "characters/", false ) != -1
	  || idStr::FindText( modelName, "chars/", false ) != -1
	  || idStr::FindText( modelName, "heads/", false ) != -1;
	const bool cinematicActor = idStr::FindText( modelName, "cinematics/", false ) != -1
	  && ( idStr::FindText( matName, "models/characters/", false ) != -1
	    || idStr::FindText( matName, "models/monsters/", false ) != -1 );
	if ( !modelIsChar && !cinematicActor ) {
		return false;
	}
	// Small, highly convex facial sub-meshes (eyes, teeth, tongue, mouth interiors,
	// jaws, eyelashes) get inflated by PN straight through the surrounding face.
	// Exclude them by material name so r_tessMinEdge can stay low enough to catch
	// ears/fingers without touching them. The head *skin* (which carries the ears) is
	// a different material and still tessellates.
	//  - Monsters + player name these "...eye..."/"...teeth..."/"...tongue..."/
	//    "...mouth..."/"...jaw..." (cacoeye, cacodemon_mouth, pinky/teeth, mtongue,
	//    zjaw01...). The only non-facial "eye" matches (skcubeyellow, hell eyeskin
	//    walls) are static/world, already excluded above.
	//  - Human NPC eyes/teeth/tongue are the shared models/characters/common/ folder
	//    (left*/right* eyes, teeth*, tongue) — all facial bits, no body geometry.
	//  - "lashes" (not "lash") avoids matching the commando's muzzle flash (mflash).
	// (docs/tessellation.md)
	// Rigid headgear — helmets, goggles/visors, eyeglasses — is hard, thin, often
	// near-flat shell geometry: PN smoothing balloons it and inward displacement
	// cracks the visor/lens open (the security guard's goggles broke visibly). It
	// gains nothing from tessellation (it isn't organic), so exclude it while the
	// surrounding face skin/ears still smooth.
	//  - "gog"      : security guard headgear when the regsec skin is applied
	//                 (helmet+goggles+mask baked as one material,
	//                 models/characters/male_npc/security/gog).
	//  - "zsechead" : the security head *shell* itself (models/monsters/zsecurity/
	//                 zsechead2/zsechead3) — the helmeted head, shared by the living
	//                 guard and the zombie-sec (skins just pick which name shows, so
	//                 both spellings turn up depending on the guard). The zsec zombie
	//                 *body* is dsecurity/zsheild, not zsechead, so it still smooths.
	//  - "gogs"/"marsec": the goggle lenses + mars-sec mask on that head
	//                 (zsgogs/zsgogs2 also match "gog").
	//  - "helmet"   : sarge/marine helmets (models/characters/sarge2/helmet...).
	//  - "glasses"  : scientist eyeglasses (models/characters/scientist/head02/
	//                 glasses2 + its glasses2_fx lens-reflection pass).
	// (confirmed by r_tessDebug capture; docs/tessellation.md)
	static const char * const tessSkipNames[] = {
		"eye", "teeth", "tongue", "mouth", "jaw", "lashes", "characters/common/",
		"gog", "zsechead", "marsec", "helmet", "glasses"
	};
	for ( int i = 0; i < (int)( sizeof( tessSkipNames ) / sizeof( tessSkipNames[0] ) ); i++ ) {
		if ( idStr::FindText( matName, tessSkipNames[i], false ) != -1 ) {
			return false;
		}
	}
	// For the OPAQUE passes, exclude purely emissive surfaces (no lit stage) — a
	// monitor/screen/glow panel is drawn only in the flat depth-EQUAL ambient pass,
	// never tessellated, so sealing its depth at the PN position would drop it
	// (dark/shimmering panels). A lit surface that ALSO carries an ambient/blend stage
	// (e.g. the imp's conditional "burning corpse" FX over its bump/diffuse/specular)
	// still tessellates. In the blended pass we ARE that ambient/decal draw, so a
	// pure-ambient surface (a blood decal) is exactly what we want to tessellate there.
	if ( !forBlendPass ) {
		bool hasLitStage = false;
		for ( int i = 0; i < mat->GetNumStages(); i++ ) {
			if ( mat->GetStage( i )->lighting != SL_AMBIENT ) {
				hasLitStage = true;
				break;
			}
		}
		if ( !hasLitStage ) {
			return false;
		}
	}

	// r_tessDebug: log each accepted material once, together with the model it belongs
	// to, so a surface that shouldn't be tessellated (a leaked eye/glasses/visor
	// material) can be pinned down by walking up to the offending character and reading
	// the console — the model name tells us which .md5mesh (body vs def_head) it is.
	if ( r_tessDebug.GetBool() ) {
		static idList<idStr> tessLogged;
		if ( tessLogged.FindIndex( idStr( matName ) ) == -1 ) {
			tessLogged.Append( idStr( matName ) );
			const idRenderModel *m = space->entityDef->parms.hModel;
			common->Printf( "tess: %s  <-  %s (dm=%d idx=%d)\n", matName,
			                m ? m->Name() : "<null>",
			                m ? (int)m->IsDynamicModel() : -1, space->entityDef->index );
		}
	}
	return true;
}

// fill the tess params (level, LOD distance, displacement, min edge) into a
// RenderParams the tessellation stages read; global cvar values, identical in
// every pass so zfill/interaction/ambient displace to the same depth.
void RB_RHI_SetTessParms( rhi::RenderParams &parms ) {
	parms.tessParms[0] = r_tessLevel.GetFloat();
	parms.tessParms[1] = r_tessMaxDist.GetFloat();
	parms.tessParms[2] = r_tessDisplace.GetFloat();	// Phase 2 displacement strength
	parms.tessParms[3] = r_tessMinEdge.GetFloat();	// min edge length to subdivide (perf/LOD guard;
													// the eye-bulge it was added for is fixed properly now
													// by signed displacement, docs/tessellation.md)
}

// For a tessellated surface, copy the bump stage's texture matrix into parms (so the
// pass builds the SAME bump texcoord the lit passes displace with) and return the bump
// image to bind (unit 1 in zfill, unit 2 in the fog pass). Mirrors R_SetDrawInteraction /
// the SSAO G-buffer so the displacement is bit-identical → depth-EQUAL holds. Shared by
// the zfill prepass and the fog interaction pass.
idImage *RB_RHI_TessBumpForZfill( const drawSurf_t *surf, rhi::RenderParams &parms ) {
	const shaderStage_t *bumpStage = surf->material->GetBumpStage();
	const float *regs = surf->shaderRegisters;
	idImage *bumpImg = globalImages->flatNormalMap;
	parms.bumpMatrixS[0] = 1.0f;
	parms.bumpMatrixT[1] = 1.0f;
	if ( bumpStage && bumpStage->texture.image ) {
		bumpImg = bumpStage->texture.image;
		if ( bumpStage->texture.hasMatrix ) {
			parms.bumpMatrixS[0] = regs[bumpStage->texture.matrix[0][0]];
			parms.bumpMatrixS[1] = regs[bumpStage->texture.matrix[0][1]];
			parms.bumpMatrixS[3] = regs[bumpStage->texture.matrix[0][2]];
			parms.bumpMatrixT[0] = regs[bumpStage->texture.matrix[1][0]];
			parms.bumpMatrixT[1] = regs[bumpStage->texture.matrix[1][1]];
			parms.bumpMatrixT[3] = regs[bumpStage->texture.matrix[1][2]];
			if ( parms.bumpMatrixS[3] < -40.0f || parms.bumpMatrixS[3] > 40.0f ) parms.bumpMatrixS[3] -= (int)parms.bumpMatrixS[3];
			if ( parms.bumpMatrixT[3] < -40.0f || parms.bumpMatrixT[3] > 40.0f ) parms.bumpMatrixT[3] -= (int)parms.bumpMatrixT[3];
		}
	}
	return bumpImg;
}

/*
=================
RB_RHI_BindTessHeight

DUDE (docs/tessellation.md): bind the SIGNED height map the tess-eval stage displaces
with, on unit 15, for EVERY pass that can tessellate.

Must be called at every tess draw site, with `tess` false as well as true. rhiVkUnits[]
persists between draws, so a tessellated draw that skipped this would inherit the
PREVIOUS surface's height map -- and if zfill and the interaction pass ever displaced a
surface with different heights, the lit draw would fail its depth-EQUAL test and the
character's surfaces would vanish. Binding the flat mid-grey image when a surface does
not tessellate (or has no height source) resets that state and means "no displacement".
=================
*/
idImage *RB_RHI_TessHeightImage( const drawSurf_s *surf, bool tess ) {
	idImage *height = NULL;
	if ( tess && surf && surf->material ) {
		const shaderStage_t *bumpStage = surf->material->GetBumpStage();
		if ( bumpStage ) {
			height = bumpStage->tessHeightImage;
		}
	}
	return height ? height : R_TessFlatHeightImage();	// mid-grey: (h - 0.5) == 0
}

static void RB_RHI_BindTessHeight( const drawSurf_t *surf, bool tess ) {
	if ( rhi::GetActiveBackendType() != rhi::BT_VULKAN ) {
		return;			// GL3 has no tessellation stages
	}
	RB_RHI_BindUnit( 15, RB_RHI_TessHeightImage( surf, tess ) );
}

// Roadmap B (docs/tessellation.md): classify a surface for the tessellated draw. Returns whether to
// FIXED-FUNCTION tessellate (the shipping .tesc/.tese path); sets outUseDeform = draw the pre-deformed
// expanded buffer instead (classifier-approved AND deform-once dispatched this frame). Mutually
// exclusive; a classifier-excluded surface (eyes/teeth/headgear/etc.) gets BOTH false and draws its
// base geometry, so deform never inflates what fixed-function tess correctly skips.
static bool RB_RHI_TessOrDeform( const drawSurf_t *surf, const srfTriangles_t *tri, bool &outUseDeform ) {
	const bool cand = RB_RHI_TessellateSurf( surf, false );
	outUseDeform = cand && tri->tessDeformVB && tri->tessDeformIB;
	return cand && !outUseDeform;
}

// Perforated casters in the RT shadow rays (rt_perforated.glsl): hand a ray-tracing draw the geometry
// table (st + coverage texture of a grate candidate) and the grates' shadow strength. They ride
// u_shadowFalloffS, which only the shadow-MAP modes read. All zero = the ray ignores grates.
//
// PER LIGHT: only a light with a grate inside its volume gets them. The grate-aware ray can't force
// everything opaque, and a ray traced without that flag loses the hardware's fastest traversal -
// paid by every pixel the light shades, grate or no grate. Every other light keeps the fixed
// force-opaque ray it always had. Decided once per light per frame (the fill runs per surface).
// PER PIXEL (perf pass 4): the shader also gets the boxes of the grates inside the light's volume
// (up to RT_PERF_MAX_BOXES, the last one swallowing any overflow) and only a pixel whose segment to
// the light passes through one of them traces the grate-aware ray. One small see-through object in
// a light's box (measured: a 28-unit pipe cap 179 units away) no longer taxes every pixel it shades.
// The boxes ride a small per-frame storage buffer (frame slots like the reflection light list); its
// address goes in u_shadowProjectionQ.xy, the count in u_shadowFalloffS.w - both unread by RT modes.
#define RT_PERF_MAX_BOXES	16
#define RT_PERF_MAX_LIGHTS	256
#define RT_PERF_SLOTS		4
static rhi::BufferHandle	s_rtPerfBoxBuf = 0;
static unsigned long long	s_rtPerfBoxAddr = 0;

static void RB_RHI_FillPerforatedParms( rhi::RHI *r, rhi::RenderParams &parms, const viewLight_t *vLight ) {
	memset( parms.shadowFalloffS, 0, sizeof( parms.shadowFalloffS ) );
	if ( !r_rtPerforatedShadows.GetBool() || !r_shadowMapPerforated.GetBool() ) {
		return;
	}
	static const viewLight_t *cachedLight = NULL;
	static int cachedFrame = -1;
	static int cachedCount = 0;					// boxes uploaded for this light; 0 = no grate in its volume
	static unsigned long long cachedAddr = 0;
	static int frameLights = 0, frameOfLights = -1;
	if ( vLight != cachedLight || tr.frameCount != cachedFrame ) {
		if ( tr.frameCount != frameOfLights ) {
			frameOfLights = tr.frameCount;
			frameLights = 0;
		}
		cachedLight = vLight;
		cachedFrame = tr.frameCount;
		cachedCount = 0;
		cachedAddr = 0;
		idBounds boxes[RT_PERF_MAX_BOXES];
		int total = 0;
		if ( vLight != NULL && vLight->frustumTris != NULL ) {
			total = R_RtGatherPerforatedBoxes( vLight->frustumTris->bounds, boxes, RT_PERF_MAX_BOXES );
		}
		if ( total > 0 ) {
			if ( s_rtPerfBoxBuf == 0 ) {
				s_rtPerfBoxBuf = r->CreateBuffer( rhi::BU_STORAGE,
					RT_PERF_SLOTS * RT_PERF_MAX_LIGHTS * RT_PERF_MAX_BOXES * 8 * (int)sizeof( float ), NULL );
				s_rtPerfBoxAddr = ( s_rtPerfBoxBuf != 0 ) ? r->GetBufferDeviceAddress( s_rtPerfBoxBuf ) : 0;
			}
			const int n = Min( total, RT_PERF_MAX_BOXES );
			if ( s_rtPerfBoxAddr != 0 && frameLights < RT_PERF_MAX_LIGHTS ) {
				float data[RT_PERF_MAX_BOXES * 8];
				for ( int i = 0; i < n; i++ ) {
					// padded: the ray starts a little off the surface, and a flat grate is a zero-thickness box
					data[i * 8 + 0] = boxes[i][0].x - 2.0f; data[i * 8 + 1] = boxes[i][0].y - 2.0f; data[i * 8 + 2] = boxes[i][0].z - 2.0f; data[i * 8 + 3] = 0.0f;
					data[i * 8 + 4] = boxes[i][1].x + 2.0f; data[i * 8 + 5] = boxes[i][1].y + 2.0f; data[i * 8 + 6] = boxes[i][1].z + 2.0f; data[i * 8 + 7] = 0.0f;
				}
				const int slot = (int)( tr.frameCount % RT_PERF_SLOTS );
				const int ofs = ( slot * RT_PERF_MAX_LIGHTS + frameLights ) * RT_PERF_MAX_BOXES * 8 * (int)sizeof( float );
				r->UpdateBuffer( s_rtPerfBoxBuf, ofs, n * 8 * (int)sizeof( float ), data );
				cachedAddr = s_rtPerfBoxAddr + (unsigned long long)ofs;
				cachedCount = n;
				frameLights++;
			} else {
				cachedCount = -1;				// no list to hand over: every pixel of this light takes the grate-aware ray
			}
		}
	}
	if ( cachedCount == 0 ) {
		return;
	}
	const unsigned long long geoTable = r->GetRtGeoTableAddress();
	if ( geoTable == 0 ) {
		return;
	}
	const unsigned int geoLo = (unsigned int)( geoTable & 0xFFFFFFFFull );
	const unsigned int geoHi = (unsigned int)( geoTable >> 32 );
	memcpy( &parms.shadowFalloffS[0], &geoLo, sizeof( geoLo ) );	// bit-cast, NOT a value cast
	memcpy( &parms.shadowFalloffS[1], &geoHi, sizeof( geoHi ) );
	parms.shadowFalloffS[2] = idMath::ClampFloat( 0.0f, 1.0f, r_shadowMapPerforatedStrength.GetFloat() );
	parms.shadowFalloffS[3] = (float)cachedCount;					// > 0: test the segment against that many boxes; < 0: no list
	const unsigned int boxLo = (unsigned int)( cachedAddr & 0xFFFFFFFFull );
	const unsigned int boxHi = (unsigned int)( cachedAddr >> 32 );
	memset( parms.shadowProjectionQ, 0, sizeof( parms.shadowProjectionQ ) );
	memcpy( &parms.shadowProjectionQ[0], &boxLo, sizeof( boxLo ) );
	memcpy( &parms.shadowProjectionQ[1], &boxHi, sizeof( boxHi ) );
}

// r_rtShadowBlur: shader parms for an interaction that looks this light's blurred mask up
// (unit 13). A mask made THIS frame is addressed by gl_FragCoord (mode 5). A staggered light's
// mask from an earlier frame (r_rtShadowBlurStagger) is addressed by reprojection through that
// frame's world-to-clip matrix (mode 6) - and because it cannot answer for points that were
// hidden / off screen / outside the light's rect back then, that draw binds the ray-query variant
// with the TLAS, exactly like a mode-4 draw, so the shader can fall back to the hard ray.
static void RB_RHI_FillBlurMaskParms( rhi::RenderParams &parms, bool &rtSun ) {
	parms.shadowParms[0] = 5.0f;
	parms.shadowParms[1] = ( rhiRtBlurW > 0 ) ? 1.0f / rhiRtBlurW : 0.0f;
	parms.shadowParms[2] = ( rhiRtBlurH > 0 ) ? 1.0f / rhiRtBlurH : 0.0f;
	if ( !ictx.shadowMaskStale ) {
		return;
	}
	const unsigned long long tlas = ictx.r->GetTlasAddress();
	if ( tlas == 0 || ictx.interactionRtProg == 0 ) {
		return;		// no fallback available: the unreprojected lookup beats no shadow at all
	}
	parms.shadowParms[0] = 6.0f;
	memcpy( parms.prevMvpMatrix, ictx.shadowMaskViewProj, sizeof( parms.prevMvpMatrix ) );
	const unsigned int tlasLo = (unsigned int)( tlas & 0xFFFFFFFFu );
	const unsigned int tlasHi = (unsigned int)( tlas >> 32 );
	memcpy( &parms.rtParms[0], &tlasLo, sizeof( tlasLo ) );	// bit-cast, NOT a value cast
	memcpy( &parms.rtParms[1], &tlasHi, sizeof( tlasHi ) );
	parms.rtParms[2] = r_rtSunShadowOffset.GetFloat();
	parms.rtParms[3] = 100000.0f;
	RB_RHI_FillPerforatedParms( ictx.r, parms, backEnd.vLight );
	rtSun = true;
}

/*
===================
RB_RHI_DrawInteraction

Callback for RB_CreateSingleDrawInteractions; the RenderParams mapping is
documented member by member in shaders/renderparms.glsl.
===================
*/
// DUDE PBR (docs/pbr-materials.md): opt-in GGX interaction path + wet-blood film, filled on the
// non-ambient interaction draw (ambient keeps the vanilla path, left zero by the memset). Sets
// pbrOrganicSpecFallback for the later specular-image swap. Extracted verbatim from
// RB_RHI_DrawInteraction to cut its length; reads din + globals + cvars, writes only parms (+ the
// fallback flag). clayWorld forces dielectric for the r_whiteWorld clay render.
static void RB_RHI_FillPbrAndFilm( rhi::RenderParams &parms, const drawInteraction_t *din,
                                   bool clayWorld, bool &pbrOrganicSpecFallback ) {
	if ( r_pbr.GetBool() && !din->ambientLight ) {
		const idMaterial *mat = din->surf->material;
		float metal, rough, wetMul, envMul;
		const int pbrCat = RB_RHI_ResolvePbrMaterial( mat, metal, rough, wetMul, envMul );
		// wetness = a specular-energy multiplier (the water/sweat/slime film) —
		// deliberately not metalness, which would tint and darken like bronze. Comes from
		// the material's per-category defaults row (or a per-material override column),
		// resolved above; neutral 1 leaves the specular unchanged.
		float specScale = r_pbrSpecScale.GetFloat() * wetMul;
		// clay world: force dielectric so metals don't kill the white diffuse / tint the
		// specular — the clay render should read metal and non-metal surfaces the same
		parms.pbrParms[0] = clayWorld ? 0.0f : metal;
		parms.pbrParms[1] = rough;
		// 1 = PBR; 2 = PBR + the decal metal mask is bound on unit 14 (r_decalMetalMask)
		parms.pbrParms[2] = rhiDecalMaskImage ? 2.0f : 1.0f;
		parms.pbrParms[3] = specScale;
		// metal diffuse-kill strength kd: 1 = physical (metals lose all diffuse
		// colour), <1 keeps that share of the asset albedo so metals aren't dark
		// and off-colour where stock D3 has no environment to reflect (sec. 5).
		parms.pbrParms2[0] = 1.0f - r_pbrMetalDiffuse.GetFloat();
		// PBR tuning knobs (Developer tab) ride localParam1.zw — the SSAO block
		// below only writes .xy on this (non-ambient) path, so no clash.
		parms.localParam1[2] = r_pbrToksvigBase.GetFloat();
		parms.localParam1[3] = r_pbrFireflyClamp.GetFloat();
		// Phase C.1 metal environment floor rides the occlusionParms spare slot
		// (the occlusion block below only writes .xyz). The per-category env default
		// (or a per-material override column) scales the global r_pbrEnvScale; resolved
		// above as envMul, neutral 1 keeps the global value.
		parms.occlusionParms[3] = r_pbrEnvScale.GetFloat() * envMul;

		// organic materials authored without a specular stage (most blood decals
		// — bloodpool01 — and the gibs) would zero the GGX lobe through the
		// black spec mask, leaving the wetness sliders inert on exactly the
		// surfaces they exist for. Substitute the material's own diffuse as the
		// mask (the gleam then follows the blood shape and density, tinting
		// dark red like real blood) with a neutral white modifier; the actual
		// image swap happens at the bind below.
		if ( ( pbrCat == PBR_CAT_FLESH || pbrCat == PBR_CAT_SKIN || pbrCat == PBR_CAT_EYES )
		     && din->specularImage == globalImages->blackImage ) {
			pbrOrganicSpecFallback = true;
			parms.specularModifier[0] = parms.specularModifier[1] =
			parms.specularModifier[2] = parms.specularModifier[3] = 1.0f;
		}

		// DUDE wet blood film: this draw is a blood decal lit as its own surface, not the material
		// it nominally belongs to - a smooth dielectric film, no category, no dielectric dimming.
		// u_channelMask (free in the interaction shaders) switches interaction.frag to film mode.
		if ( rhiFilmDraw ) {
			pbrOrganicSpecFallback = false;
			parms.pbrParms[0] = 0.0f;
			parms.pbrParms[1] = idMath::ClampFloat( 0.05f, 1.0f, r_decalWetBloodRough.GetFloat() );
			parms.pbrParms[3] = 1.0f;
			parms.occlusionParms[3] = 0.0f;
			parms.channelMask[0] = rhiFilmDraw->mode;
			parms.channelMask[1] = idMath::ClampFloat( 0.0f, 1.0f, r_decalWetBloodRelief.GetFloat() );
			// zw: the coverage window the film feathers in over. The floor keeps the film off the
			// faint mist splat textures trail off into - blood too thin to see should not glint.
			const float filmFloor = idMath::ClampFloat( 0.0f, 0.95f, r_decalWetBloodThreshold.GetFloat() );
			parms.channelMask[2] = filmFloor;
			parms.channelMask[3] = Min( filmFloor + 0.25f, 1.0f );
		}
	}
}

// Skin / flesh subsurface-scattering interaction params (docs/skin-shading.md): a wrapped-diffuse
// terminator warmth on the skin/flesh categories (all tiers, direct-light only). Writes only
// parms.skinParms — a strict no-op (memset zero) everywhere else. Extracted from DrawInteraction.
static void RB_RHI_FillSkinSSS( rhi::RenderParams &parms, const drawInteraction_t *din ) {
	if ( r_skinShading.GetFloat() > 0.0f && !din->ambientLight && !rhiFilmDraw ) {
		const int skinCat = din->surf->material->GetPbrCategory();
		if ( skinCat == PBR_CAT_SKIN || skinCat == PBR_CAT_FLESH ) {
			parms.skinParms[0] = idMath::ClampFloat( 0.0f, 1.0f, r_skinShading.GetFloat() );
			// The RGB wrap-width ratio IS the terminator's warm colour: red light penetrates the
			// dermis deepest, green less, blue least (the classic skin diffusion profile). Hell
			// flesh (gibs, raw meat, imp hide) scatters deeper and redder than face skin.
			const float rad = r_skinScatter.GetFloat();		// red-channel scatter reach
			if ( skinCat == PBR_CAT_FLESH ) {
				parms.skinParms[1] = rad * 1.30f;
				parms.skinParms[2] = rad * 0.30f;
				parms.skinParms[3] = rad * 0.15f;
			} else {
				parms.skinParms[1] = rad * 1.00f;
				parms.skinParms[2] = rad * 0.35f;
				parms.skinParms[3] = rad * 0.20f;
			}
		}
	}
}

// Shadow-map / RT-shadow interaction params (DUDE Phase 3.5): the 4-way sun / RT-point /
// 2D-spot / cube ladder plus the r_rtShadowBlur mask path. Writes parms and the two draw-mode
// flags rtSun (bind the ray-query interaction variant) and blurMask (this light's blur mask on
// unit 13). Extracted verbatim from RB_RHI_DrawInteraction.
static void RB_RHI_FillShadowParms( rhi::RenderParams &parms, const drawInteraction_t *din,
                                    bool &rtSun, bool &blurMask ) {
	// shadow mapping (DUDE Phase 3.5): only the regular interaction shader samples
	// the depth map — the ambientLight pass has no shadow term. The 2D lookup uses a
	// SEPARATE, unbaked projection texgen (shadowProjection[], filled below), NOT the
	// cookie texgen (lightProjection[]): the cookie carries the light stage's texture
	// matrix (rotating fan gobos etc.), but the caster renders the depth map with the
	// raw projection, so sampling with the baked cookie UV would slide the shadow
	// across a static depth field (the fan-shadow bug). Left zero (memset) for stencil
	// / unshadowed lights -> u_shadowParms.x == 0 -> visibility 1.
	if ( ( ictx.lightShadowMapped || ictx.lightShadowCube || ictx.lightRtPoint ) && !din->ambientLight ) {
		// Receiver-dependent acne bias: flat world/BSP surfaces tolerate the tight
		// world bias; models (non-static-world entities) have curved, high-slope
		// geometry that self-shadows and needs a larger bias. Perforated world
		// geometry (grates, fences) already falls in the world group via the static
		// world model, so it must NOT be keyed on material coverage -- doing so would
		// also drag in alpha-tested character skins, which are models and want the
		// model bias.
		const idRenderEntityLocal *redef = din->surf->space->entityDef;
		const bool worldReceiver = redef && redef->parms.hModel
		    && redef->parms.hModel->IsStaticWorldModel();
		// The flashlight overrides the receiver-based bias with its own small value:
		// its narrow cone lands nearly parallel to the surfaces it grazes, so the
		// world/model bias (tuned for shadow-casting fans etc.) pushes the shadow off
		// the caster (peter-panning) and makes the edge swim as the light sweeps.
		const float bias = ictx.lightIsFlashlight
		    ? r_shadowMapFlashlightBias.GetFloat()
		    : ( worldReceiver ? r_shadowMapBias.GetFloat() : r_shadowMapModelBias.GetFloat() );

		// Slope-scaled bias strength (interaction.frag): the shader grows the flat
		// bias above by this * tan(surface-to-light angle) to cover the receiver's
		// depth change across a shadow texel at grazing angles (banded acne). Rides
		// the spare pbrParms2.y slot; 0 keeps the old constant bias.
		parms.pbrParms2[1] = r_shadowMapSlopeBias.GetFloat();

		// r_rtShadowBlur: the mask was traced from the OPAQUE depth buffer, so it only describes
		// fragments that ARE that depth - opaque chains (drawn depth-EQUAL) outside the weapon /
		// model depth-range hacks. Translucent interactions (depth-LESS, floating in front of the
		// traced surface) and depth-hacked view weapons keep the inline hard ray (mode 4).
		blurMask = ictx.lightRtBlur && ictx.depthFuncBits == GLS_DEPTHFUNC_EQUAL
			&& !din->surf->space->weaponDepthHack && din->surf->space->modelDepthHack == 0.0f;

		if ( ictx.lightShadowMapped && ictx.lightSunShadow ) {
			// DUDE sun shadow map (mode 3): the map was rendered through the per-view
			// fitted VIRTUAL projection (rhiSunPlanes), so the lookup must use those
			// same planes — the light's own texgen never saw this map. The compare
			// reference is the virtual depth plane (shadowFalloffS -> var_ShadowProjection.z);
			// the sun map's depth unit spans the whole fitted region, so the bias has
			// its own (smaller) cvar. Slope-scaling still applies via pbrParms2.y.
			parms.shadowParms[0] = 3.0f;		// sun: virtual-projection 2D map on unit 7
			parms.shadowParms[1] = ( rhiShadowMapSize > 0 ) ? 1.0f / (float)rhiShadowMapSize : 0.0f;
			parms.shadowParms[2] = r_shadowMapSunBias.GetFloat();
			// normal-offset bias (interaction.vert/.tese): w = one sun texel's world
			// size at the fit center; pbrParms2.w = the offset strength in texels
			parms.shadowParms[3] = rhiSunTexelWorld;
			parms.pbrParms2[3] = r_shadowMapNormalOffset.GetFloat();
			idPlane rawLp;
			R_GlobalPlaneToLocal( din->surf->space->modelMatrix, rhiSunPlanes[0], rawLp );
			memcpy( parms.shadowProjectionS, rawLp.ToFloatPtr(), 16 );
			R_GlobalPlaneToLocal( din->surf->space->modelMatrix, rhiSunPlanes[1], rawLp );
			memcpy( parms.shadowProjectionT, rawLp.ToFloatPtr(), 16 );
			R_GlobalPlaneToLocal( din->surf->space->modelMatrix, rhiSunPlanes[2], rawLp );
			memcpy( parms.shadowProjectionQ, rawLp.ToFloatPtr(), 16 );
			R_GlobalPlaneToLocal( din->surf->space->modelMatrix, rhiSunPlanes[3], rawLp );
			memcpy( parms.shadowFalloffS, rawLp.ToFloatPtr(), 16 );

			// DUDE RT sun shadows (R3, docs/rtx-shadow-roadmap.md): keep every mode-3
			// binding live (the sun map still renders this increment, so nothing stale
			// gets bound), but hand the fragment shader the scene TLAS and flip to mode
			// 4 - interaction_rt.frag traces the sun visibility instead of sampling the
			// map. Needs the RT variant loaded and a live persistent world scene
			// (R_RtWorldUpdate auto-builds it while r_rtSunShadows is on).
			if ( blurMask ) {
				RB_RHI_FillBlurMaskParms( parms, rtSun );
			} else if ( r_rtSunShadows.GetBool() && ictx.interactionRtProg != 0 ) {
				const unsigned long long tlas = ictx.r->GetTlasAddress();
				if ( tlas != 0 ) {
					parms.shadowParms[0] = 4.0f;
					const unsigned int tlasLo = (unsigned int)( tlas & 0xFFFFFFFFu );
					const unsigned int tlasHi = (unsigned int)( tlas >> 32 );
					memcpy( &parms.rtParms[0], &tlasLo, sizeof( tlasLo ) );	// bit-cast, NOT a value cast
					memcpy( &parms.rtParms[1], &tlasHi, sizeof( tlasHi ) );
					parms.rtParms[2] = r_rtSunShadowOffset.GetFloat();
					parms.rtParms[3] = 100000.0f;
					RB_RHI_FillPerforatedParms( ictx.r, parms, backEnd.vLight );	// after the mode-3 fill above: the ray modes own u_shadowFalloffS
					rtSun = true;
				}
			}
			if ( ictx.lightRtOnly && !rtSun && !blurMask ) {
				// the RT-only route lost its TLAS mid-frame: rhiSunPlanes are stale for
				// this light, so unshadowed beats sampling a map that was never fitted
				parms.shadowParms[0] = 0.0f;
			}
		} else if ( ictx.lightRtPoint ) {
			// RT moving point light (r_rtMovingLights): no cube was rendered at all —
			// mode 4 traces one ray toward u_localLightOrigin, which for a point light
			// IS the light position, so the sun ray code serves it unchanged (the ray
			// stops just short of the origin; distLight bounds it, rtParms.w is inert).
			// TLAS gone mid-frame downgrades to unshadowed, same as the sun RT route.
			const unsigned long long tlas = ictx.r->GetTlasAddress();
			if ( blurMask ) {
				RB_RHI_FillBlurMaskParms( parms, rtSun );
			} else if ( tlas != 0 && ictx.interactionRtProg != 0 ) {
				parms.shadowParms[0] = 4.0f;
				const unsigned int tlasLo = (unsigned int)( tlas & 0xFFFFFFFFu );
				const unsigned int tlasHi = (unsigned int)( tlas >> 32 );
				memcpy( &parms.rtParms[0], &tlasLo, sizeof( tlasLo ) );	// bit-cast, NOT a value cast
				memcpy( &parms.rtParms[1], &tlasHi, sizeof( tlasHi ) );
				parms.rtParms[2] = r_rtSunShadowOffset.GetFloat();
				parms.rtParms[3] = 100000.0f;
				RB_RHI_FillPerforatedParms( ictx.r, parms, backEnd.vLight );
				rtSun = true;		// bind the ray-query interaction variant for this draw
			} else {
				parms.shadowParms[0] = 0.0f;
			}
		} else if ( ictx.lightShadowMapped ) {
			parms.shadowParms[0] = 1.0f;		// projected/spot: 2D map on unit 7
			parms.shadowParms[1] = ( rhiShadowMapSize > 0 ) ? 1.0f / (float)rhiShadowMapSize : 0.0f;
			parms.shadowParms[2] = bias;

			// UNBAKED projection for the 2D shadow lookup: the RAW light-projection
			// planes transformed into this surface's local space, exactly as the
			// caster does (RB_RHI_ShadowCasterChain), so the sampled UV lands in the
			// same frame the depth was written. Equals lightProjection[] for lights
			// without a projection texture matrix; differs (and fixes the swimming
			// shadow) for rotating/scrolling gobos like the ceiling-fan lights.
			idPlane rawLp;
			R_GlobalPlaneToLocal( din->surf->space->modelMatrix, backEnd.vLight->lightProject[0], rawLp );
			memcpy( parms.shadowProjectionS, rawLp.ToFloatPtr(), 16 );
			R_GlobalPlaneToLocal( din->surf->space->modelMatrix, backEnd.vLight->lightProject[1], rawLp );
			memcpy( parms.shadowProjectionT, rawLp.ToFloatPtr(), 16 );
			R_GlobalPlaneToLocal( din->surf->space->modelMatrix, backEnd.vLight->lightProject[2], rawLp );
			memcpy( parms.shadowProjectionQ, rawLp.ToFloatPtr(), 16 );
		} else {
			parms.shadowParms[0] = 2.0f;		// point/omni: cube map on unit 8
			parms.shadowParms[1] = ( rhiShadowCubeSize > 0 ) ? 1.0f / (float)rhiShadowCubeSize : 0.0f;
			parms.shadowParms[2] = bias;
			parms.shadowParms[3] = ictx.lightRange;	// radial-distance normalizer
			// static/dynamic split: also sample the movers' cube (unit 12) and take the
			// darker of the two. pbrParms2.z is the hasDynamicLayer flag the shader gates on.
			// dynamic-layer gate doubling as the static/dynamic face-res ratio: 0 = no
			// dynamic cube; >= 1 = sample it, widening its PCF disc + depth bias by this
			// factor (r_shadowMapSplitDynDrop renders the movers' cube at a lower tier).
			parms.pbrParms2[2] = ( ictx.lightHasDynamicLayer && ictx.shadowCubeDynImage ) ? ictx.shadowCubeDynScale : 0.0f;
			// normal-offset bias (interaction.vert/.tese): strength in texels; the shader
			// derives the world size per texel from 2*dist/res (exact for a cube face)
			parms.pbrParms2[3] = r_shadowMapNormalOffset.GetFloat();
		}
	}
}

static void RB_RHI_FillSsaoParms( rhi::RenderParams &parms, const drawInteraction_t *din ) {
	// DUDE SSAO (docs/ssao-gtao.md Phase C). Occlude the ambient term always, and the
	// direct-light diffuse by r_ssaoDirectLight -- Doom 3 has ~no ambient, so darkening
	// only the ambient pass is invisible in normal scenes; the direct-light term is what
	// actually makes AO show. r_ssaoDirectLight 0 = ambient-only (most faithful). Both
	// shaders sample the AO buffer bound once on unit 9 (see RB_RHI_DrawWorld); neither
	// path uses localParam0/localParam1 otherwise. localParam0 = (enable, floor, 1/viewW,
	// 1/viewH); localParam1 (direct lights only) = (direct strength, specular-occlusion).
	if ( rhiSsaoAppliedThisView && rhiSsaoResultRT != 0 ) {
		const float directStrength = din->ambientLight
			? 1.0f : idMath::ClampFloat( 0.0f, 1.0f, r_ssaoDirectLight.GetFloat() );
		if ( din->ambientLight || directStrength > 0.0f ) {
			parms.localParam0[0] = 1.0f;
			parms.localParam0[1] = idMath::ClampFloat( 0.0f, 1.0f, r_ssaoFloor.GetFloat() );
			parms.localParam0[2] = ( rhiSsaoViewW > 0 ) ? 1.0f / rhiSsaoViewW : 0.0f;
			parms.localParam0[3] = ( rhiSsaoViewH > 0 ) ? 1.0f / rhiSsaoViewH : 0.0f;
			if ( !din->ambientLight ) {
				parms.localParam1[0] = directStrength;
				parms.localParam1[1] = r_ssaoSpecular.GetBool() ? 1.0f : 0.0f;
			} else {
				// C.2: bias the ambient cube lookup toward the bent normal. The bent
				// normal is view-space in the AO buffer; the shader rotates it to world
				// with the inverse view rotation, so pass the world->eye view matrix.
				parms.localParam1[0] = r_ssaoBentNormal.GetBool()
					? idMath::ClampFloat( 0.0f, 1.0f, r_ssaoBentStrength.GetFloat() ) : 0.0f;
				memcpy( parms.modelViewMatrix, ictx.viewDef->worldSpace.modelViewMatrix,
				        sizeof( parms.modelViewMatrix ) );
			}
		}
	}
}

static bool RB_RHI_FillContactShadowParms( rhi::RenderParams &parms, const drawInteraction_t *din ) {
	// DUDE screen-space contact shadows (docs/contact-shadows.md). interaction.frag's
	// contactShadowTerm marches the captured scene depth (_currentDepth, unit 16 - already
	// sampleable, SSAO consumed it earlier this frame) from the fragment toward the light and
	// darkens where a nearer occluder is found, grounding small geometry the shadow map misses.
	// Opt-in, non-ambient (direct-light) draws only, raster tiers (Ultra Nightmare uses
	// r_rtAllLights). Fills the view-space march transforms (model->view, view->clip), the depth
	// linearisation pair, the depth-texel scale and the view-Y sign, mirroring the SSR conventions.
	// r_rtAllLights (Ultra Nightmare) traces a real hard ray per light through interaction_rt.frag,
	// which strictly supersedes this screen-space approximation - so skip contact shadows wholesale
	// whenever it is on (a true no-op, not just redundant), regardless of preset.
	bool contactShadows = false;
	if ( r_contactShadows.GetBool() && !r_rtAllLights.GetBool()
	     && !din->ambientLight && R_BackendSupportsEnhancements() ) {
		idImage *cd = globalImages->currentDepthImage;
		const bool vkModeCS = ( rhi::GetActiveBackendType() == rhi::BT_VULKAN );
		const bool depthReady = cd && cd->uploadWidth > 0 && cd->uploadHeight > 0
			&& ( vkModeCS ? ( cd->rhiCaptured && cd->rhiHandle != 0 ) : true );
		if ( depthReady ) {
			contactShadows = true;
			const int steps = idMath::ClampInt( 2, 32, r_contactShadowSteps.GetInteger() );
			parms.contactParms[0] = (float)steps;
			parms.contactParms[1] = r_contactShadowLength.GetFloat();
			parms.contactParms[2] = r_contactShadowThickness.GetFloat();
			parms.contactParms[3] = idMath::ClampFloat( 0.0f, 1.0f, r_contactShadowStrength.GetFloat() );
			// view-space march transforms: this surface's model->view and the view's view->clip
			memcpy( parms.modelViewMatrix, din->surf->space->modelViewMatrix, sizeof( parms.modelViewMatrix ) );
			memcpy( parms.projectionMatrix, ictx.viewDef->projectionMatrix, sizeof( parms.projectionMatrix ) );
			rhi::FillDepthParms( parms, ictx.viewDef->projectionMatrix );	// window depth -> view z (cinematics cram the near plane)
			// map a march point's view-[0,1] uv into the depth texture's texel space: the GL capture
			// is power-of-two padded, the VK capture is native (ratio ~1). fullView / uploadSize.
			const int fullW = ictx.viewDef->viewport.x2 - ictx.viewDef->viewport.x1 + 1;
			const int fullH = ictx.viewDef->viewport.y2 - ictx.viewDef->viewport.y1 + 1;
			parms.depthTexRecip[0] = (float)fullW / (float)cd->uploadWidth;
			parms.depthTexRecip[1] = (float)fullH / (float)cd->uploadHeight;
			parms.windowCoord[2] = vkModeCS ? -1.0f : 1.0f;	// view-Y sign for the top-down VK depth buffer (deviceRow)
		}
	}
	return contactShadows;
}

static idImage *RB_RHI_FillOcclusionParms( rhi::RenderParams &parms, const drawInteraction_t *din ) {
	// DUDE material AO map (docs/occlusion-maps.md). A per-surface baked occlusion texture
	// (the material's `occlusionmap` stage) darkens the ambient term, and -- scaled by
	// r_occlusionMapDirect -- the direct-light diffuse, reusing the SSAO application path in
	// ambientlight.frag / interaction.frag. Independent of SSAO (works with it off). Bound on
	// unit 10 below; u_occlusionParms = (enable, ambient strength, direct strength, unused).
	// GetOcclusionStage() is NULL on every stock material, so this stays inert on the base game.
	idImage *occlusionImage = NULL;
	if ( r_occlusionMaps.GetBool() && R_BackendSupportsEnhancements() && din->surf->material ) {
		const idMaterial *mat = din->surf->material;
		const shaderStage_t *ocl = mat->GetOcclusionStage();
		if ( ocl && ocl->texture.image ) {
			// explicit occlusionmap stage (modpack-authored) always wins
			const float *regs = din->surf->shaderRegisters;
			if ( !regs || regs[ocl->conditionRegister] != 0.0f ) {
				occlusionImage = ocl->texture.image;
			}
		} else {
			// no explicit stage: auto-load a baked map from generated/aomaps, but only for
			// model-entity surfaces (props/NPCs) -- never the static world BSP, matching the
			// "give kick to objects, not map geometry" intent (docs/occlusion-maps.md). This
			// also contains the material-name keying's blast radius to non-world surfaces.
			const idRenderEntityLocal *redef = din->surf->space ? din->surf->space->entityDef : NULL;
			const bool worldSurf = redef && redef->parms.hModel
			    && redef->parms.hModel->IsStaticWorldModel();
			if ( redef && !worldSurf ) {
				// redef->dynamicModel is this frame's instantiated snapshot for MD5/dynamic
				// entities (NULL for static models); the resolver needs it to enumerate the
				// surfaces the base MD5 model doesn't expose.
				occlusionImage = RB_RHI_SurfaceOcclusion( redef->parms.hModel, redef->dynamicModel,
				                                          mat, redef->parms.customSkin,
				                                          redef->parms.customShader );
			}
		}
	}
	if ( occlusionImage ) {
		const float scale = idMath::ClampFloat( 0.0f, 1.0f, r_occlusionMapScale.GetFloat() );
		parms.occlusionParms[0] = 1.0f;
		parms.occlusionParms[1] = scale;	// ambient-term strength (ambientlight.frag reads .y)
		// direct-diffuse strength (interaction.frag reads .z); left 0 on the ambient pass
		parms.occlusionParms[2] = din->ambientLight ? 0.0f
			: scale * idMath::ClampFloat( 0.0f, 1.0f, r_occlusionMapDirect.GetFloat() );
	}
	return occlusionImage;
}

static idImage *RB_RHI_FillParallaxParms( rhi::RenderParams &parms, const drawInteraction_t *din ) {
	// DUDE parallax occlusion mapping (docs/parallax.md). The interaction pass marches the
	// bump stage's captured height map to offset the surface UVs, faking per-pixel relief on
	// flat world geometry. Fragment-only (no geometry moved), so the flat depth prepass and
	// GLS_DEPTHFUNC_EQUAL are untouched. Interaction pass only for now: the ambient pass keeps
	// flat UVs until it gets a matching offset (Phase C). GetParallaxStage() is NULL on stock
	// assets and with r_parallax off, so this stays inert on the base game.
	//
	// World (BSP) surfaces only: POM assumes a locally flat surface with a uniform tangent
	// basis, which holds for walls/floors but not props/characters -- on those the offset
	// smears and deforms edges. Props are the tessellation feature's job, not this one. The
	// static-world test mirrors the occlusion-map path above.
	//
	// Opaque only: on alpha-tested (perforated) surfaces -- grates, fences, fans -- the UV
	// march swims across the cut-out holes and fights the alpha test, looking worse than
	// flat. The height-derived detail those need lives in their silhouette, not their depth.
	idImage *parallaxImage = NULL;
	if ( r_parallax.GetBool() && R_BackendSupportsEnhancements() && !din->ambientLight
			&& din->surf->material && din->surf->material->Coverage() == MC_OPAQUE ) {
		const idRenderEntityLocal *redef = din->surf->space ? din->surf->space->entityDef : NULL;
		const bool worldSurf = redef && redef->parms.hModel
		    && redef->parms.hModel->IsStaticWorldModel();
		const shaderStage_t *px = worldSurf ? din->surf->material->GetParallaxStage() : NULL;
		if ( px && px->parallaxImage ) {
			parallaxImage = px->parallaxImage;
			// The material's parallaxScale is the heightmap() bake magnitude (~3..10); convert
			// to a UV displacement depth. Doom 3's height maps are low-contrast (authored only
			// to derive normals), so the per-unit factor is generous to make the relief read;
			// r_parallaxScale is the artist master knob on top (default 1, crank to exaggerate).
			const float depth = px->parallaxScale * r_parallaxScale.GetFloat() * 0.015f;
			parms.parallaxParms[0] = 1.0f;
			parms.parallaxParms[1] = idMath::ClampFloat( 0.0f, 0.3f, depth );
			parms.parallaxParms[2] = (float)r_parallaxMinSteps.GetInteger();
			parms.parallaxParms[3] = (float)r_parallaxMaxSteps.GetInteger();
			parms.parallaxParms2[0] = idMath::ClampFloat( 0.0f, 1.0f, r_parallaxShadow.GetFloat() );
		}
	}
	return parallaxImage;
}

static void RB_RHI_DrawInteraction( const drawInteraction_t *din ) {
	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	memcpy( parms.mvpMatrix, ictx.mvp, sizeof( parms.mvpMatrix ) );
	bool rtSun = false;			// mode-4 draw -> bind the ray-query interaction variant
	bool blurMask = false;		// mode-5 draw -> this light's blurred ray mask on unit 13

	memcpy( parms.localLightOrigin, din->localLightOrigin.ToFloatPtr(), 16 );
	memcpy( parms.localViewOrigin, din->localViewOrigin.ToFloatPtr(), 16 );
	memcpy( parms.lightProjectionS, din->lightProjection[0].ToFloatPtr(), 16 );
	memcpy( parms.lightProjectionT, din->lightProjection[1].ToFloatPtr(), 16 );
	memcpy( parms.lightProjectionQ, din->lightProjection[2].ToFloatPtr(), 16 );
	memcpy( parms.lightFalloffS, din->lightProjection[3].ToFloatPtr(), 16 );
	memcpy( parms.bumpMatrixS, din->bumpMatrix[0].ToFloatPtr(), 16 );
	memcpy( parms.bumpMatrixT, din->bumpMatrix[1].ToFloatPtr(), 16 );
	memcpy( parms.diffuseMatrixS, din->diffuseMatrix[0].ToFloatPtr(), 16 );
	memcpy( parms.diffuseMatrixT, din->diffuseMatrix[1].ToFloatPtr(), 16 );
	memcpy( parms.specularMatrixS, din->specularMatrix[0].ToFloatPtr(), 16 );
	memcpy( parms.specularMatrixT, din->specularMatrix[1].ToFloatPtr(), 16 );

	switch ( din->vertexColor ) {
	case SVC_IGNORE:
		parms.vertexColorAdd[0] = parms.vertexColorAdd[1] = parms.vertexColorAdd[2] = parms.vertexColorAdd[3] = 1.0f;
		break;
	case SVC_MODULATE:
		parms.vertexColorModulate[0] = parms.vertexColorModulate[1] = parms.vertexColorModulate[2] = parms.vertexColorModulate[3] = 1.0f;
		break;
	case SVC_INVERSE_MODULATE:
		parms.vertexColorModulate[0] = parms.vertexColorModulate[1] = parms.vertexColorModulate[2] = parms.vertexColorModulate[3] = -1.0f;
		parms.vertexColorAdd[0] = parms.vertexColorAdd[1] = parms.vertexColorAdd[2] = parms.vertexColorAdd[3] = 1.0f;
		break;
	}

	// r_whiteWorld 2 (clay): the light+material colour is baked into din->diffuseColor, so
	// force the modifier white to strip both — the surface then reads only occlusion/relief
	// (SSAO + POM self-shadow) in grey, with no coloured-light tint. Metalness is zeroed below.
	const bool clayWorld = r_whiteWorld.GetInteger() >= 2;
	if ( clayWorld ) {
		parms.diffuseModifier[0] = parms.diffuseModifier[1] = parms.diffuseModifier[2] = parms.diffuseModifier[3] = 1.0f;
		parms.specularModifier[0] = parms.specularModifier[1] = parms.specularModifier[2] = parms.specularModifier[3] = 1.0f;
	} else {
		memcpy( parms.diffuseModifier, din->diffuseColor.ToFloatPtr(), 16 );
		memcpy( parms.specularModifier, din->specularColor.ToFloatPtr(), 16 );
	}
	// DUDE: decal sheen control — both the vanilla and the GGX lobe multiply by this modifier
	float decalSheen = rhiFilmDraw ? 1.0f : R_DecalSheenScale( din->surf->material );
	if ( !rhiFilmDraw && rhiFilmSurfs.Num() > 0 && din->surf->material->GetSort() == SS_DECAL
		&& RB_RHI_SurfHasFilm( din->surf ) ) {
		decalSheen = 0.0f;		// wet blood film on: it IS this decal's highlight
	}
	if ( decalSheen != 1.0f ) {
		parms.specularModifier[0] *= decalSheen;
		parms.specularModifier[1] *= decalSheen;
		parms.specularModifier[2] *= decalSheen;
	}

	// DUDE Phase 3.5 specular tuning (interaction.frag). Defaults reproduce
	// vanilla: scale 1, shading model 0 (the N.H lookup table). Only consumed by
	// the regular interaction shader; the ambientLight shader ignores it.
	parms.specularParms[0] = r_specularScale.GetFloat();
	parms.specularParms[1] = r_specularExp.GetFloat();
	parms.specularParms[2] = (float)r_shading.GetInteger();
	// w carries the point-light cube PCF tap count (the interaction shader's cube
	// branch reads it; the free specularParms slot avoids growing u_shadowParms past 4).
	parms.specularParms[3] = (float)r_shadowMapCubePcf.GetInteger();

	// PBR (GGX) interaction path + wet-blood film — extracted to RB_RHI_FillPbrAndFilm.
	bool pbrOrganicSpecFallback = false;
	RB_RHI_FillPbrAndFilm( parms, din, clayWorld, pbrOrganicSpecFallback );

	// skin / flesh subsurface scattering — extracted to RB_RHI_FillSkinSSS.
	RB_RHI_FillSkinSSS( parms, din );

	// shadow-map / RT-shadow params (the 4-way ladder) -> RB_RHI_FillShadowParms.
	RB_RHI_FillShadowParms( parms, din, rtSun, blurMask );

	// ambientlight.vert rebuilds a tangent-to-global rotation from these
	const float *mm = din->surf->space->modelMatrix;
	for ( int row = 0; row < 3; row++ ) {
		float *dst = row == 0 ? parms.modelMatrixRow0 : ( row == 1 ? parms.modelMatrixRow1 : parms.modelMatrixRow2 );
		dst[0] = mm[row];
		dst[1] = mm[row + 4];
		dst[2] = mm[row + 8];
		dst[3] = mm[row + 12];
	}

	RB_RHI_FillSsaoParms( parms, din );

	bool contactShadows = RB_RHI_FillContactShadowParms( parms, din );

	idImage *occlusionImage = RB_RHI_FillOcclusionParms( parms, din );

	// RTGI reconcile (docs/rtx-rtgi.md H6c): on an AMBIENT draw with r_rtgi on, the fullscreen GI apply
	// (RB_RHI_RtgiApply) adds real bounce light on top of this fake cube-map ambient. Hand ambientlight.frag
	// r_rtgiAmbientKeep in the otherwise-unused u_color.x, encoded +1 so 0 = "GI off, keep 1 (no dim)", and
	// it scales the fake ambient down so the two do not double-count.
	if ( din->ambientLight && r_rtgi.GetBool() ) {
		parms.color[0] = idMath::ClampFloat( 0.0f, 1.0f, r_rtgiAmbientKeep.GetFloat() ) + 1.0f;
	}

	idImage *parallaxImage = RB_RHI_FillParallaxParms( parms, din );

	// texture units exactly as RB_ARB2_DrawInteraction / the README table
	RB_RHI_BindUnit( 0, din->ambientLight ? globalImages->ambientNormalMap : globalImages->normalCubeMapImage );
	RB_RHI_BindUnit( 1, din->bumpImage );
	RB_RHI_BindUnit( 2, din->lightFalloffImage );
	RB_RHI_BindUnit( 3, din->lightImage );
	RB_RHI_BindUnit( 4, din->diffuseImage );
	RB_RHI_BindUnit( 5, pbrOrganicSpecFallback ? din->diffuseImage : din->specularImage );
	RB_RHI_BindUnit( 6, globalImages->specularTableImage );
	// unit 10: per-material baked occlusion map (u_occlusionMap). Only bound when this
	// surface has one and r_occlusionMaps is on; the shader gates on u_occlusionParms.x,
	// so other draws simply don't sample it. (Unit 9 = SSAO is bound once in DrawWorld.)
	if ( occlusionImage ) {
		RB_RHI_BindUnit( 10, occlusionImage );
	}
	// unit 11: per-material parallax height map (u_parallaxMap). Only bound when this surface
	// carries one and r_parallax is on; the shader gates on u_parallaxParms.x (docs/parallax.md).
	if ( parallaxImage ) {
		RB_RHI_BindUnit( 11, parallaxImage );
	}

	// DUDE tessellation: PN-smooth enemy/prop meshes. Both the ambient and the
	// per-light interaction pass must tessellate whenever the depth prepass did,
	// or the depth-EQUAL test drops the surface (docs/tessellation.md). The
	// interaction and ambientlight shaders both carry a tess variant.
	bool tess = RB_RHI_TessellateSurf( din->surf, false );
	// Roadmap B: if this classifier-approved surface was deform-once dispatched, draw its pre-deformed
	// expanded buffer (rebinds vb/ib/count, clears tess) instead of fixed-function tessellating.
	rhi::BufferHandle dvb = ictx.vb, dib = ictx.ib;
	int dVertOfs = ictx.vertOfs, dIdxOfs = ictx.idxOfs;
	int idxCount = din->surf->geo->numIndexes;
	RB_RHI_ApplyDeform( din->surf->geo, dvb, dVertOfs, dib, dIdxOfs, idxCount, tess );
	if ( tess ) {
		RB_RHI_SetTessParms( parms );
	}

	rhi::BufferHandle ub;
	int uniOfs = ictx.r->AllocUniforms( &parms, sizeof( parms ), &ub );

	rhi::PipelineDesc pd;
	pd.stateBits = GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | ictx.depthFuncBits;
	pd.shader = din->ambientLight ? ictx.ambientProg : ( rtSun ? ictx.interactionRtProg : ictx.interactionProg );
	pd.vertexLayout = rhi::VL_DRAWVERT;
	pd.cullType = RB_RHI_CullFor( ictx.viewDef, CT_FRONT_SIDED );
	pd.stencilState = ictx.stencilState;
	pd.tessellate = tess;
	RB_RHI_BindTessHeight( din->surf, tess );	// DUDE: unit 15 signed height (docs/tessellation.md)
	ictx.r->BindPipeline( pd );

	rhi::DrawArgs da;
	memset( &da, 0, sizeof( da ) );
	da.vertexBuffer = dvb;
	da.vertexOffset = dVertOfs;
	da.indexBuffer = dib;
	da.firstIndex = dIdxOfs / (int)sizeof( glIndex_t );
	da.indexCount = idxCount;
	da.uniformBuffer = ub;
	da.uniformOffset = uniOfs;
	da.uniformSize = sizeof( parms );
	RB_RHI_VkTextures( da );		// VK: units 0-6 recorded by the binds above
	if ( ictx.lightShadowMapped && !din->ambientLight ) {
		da.textures[7] = ictx.shadowImage;	// 2D depth map for u_shadowMap (unit 7)
	} else if ( ictx.lightShadowCube && !din->ambientLight ) {
		da.shadowCube = ictx.shadowCubeImage;	// cube depth map for u_shadowCube (unit 8)
		if ( ictx.lightHasDynamicLayer && ictx.shadowCubeDynImage ) {
			da.shadowCubeDyn = ictx.shadowCubeDynImage;	// movers' cube for u_shadowCubeDyn (unit 12)
		}
	}
	if ( blurMask ) {
		da.shadowMask = ictx.shadowMaskImage;	// blurred RT shadow mask for u_shadowMask (unit 13)
	}
	da.decalMask = rhiDecalMaskImage;			// decal metal retention for u_decalMask (unit 14); VK only, GL3 raw-binds it
	if ( contactShadows && rhi::GetActiveBackendType() == rhi::BT_VULKAN ) {
		da.contactDepth = globalImages->currentDepthImage->rhiHandle;	// captured scene depth for u_contactDepth (unit 16); GL3 raw-binds it loop-global
	}
	ictx.r->Draw( da );

	backEnd.pc.c_drawElements++;
	backEnd.pc.c_drawIndexes += din->surf->geo->numIndexes;
}

/*
===================
RB_RHI_CreateDrawInteractions

Mirrors RB_ARB2_CreateDrawInteractions: state is constant over the chain,
the idTech4 stage decomposition runs unchanged.
===================
*/
static void RB_RHI_CreateDrawInteractions( const drawSurf_t *surf ) {
	for ( ; surf; surf = surf->nextOnLight ) {
		if ( !surf->geo || ( !surf->geo->ambientCache && !surf->geo->gpuSkinVB ) ) {
			continue;
		}
		RB_RHI_SpaceMvp( ictx.viewDef, surf->space, ictx.mvp );
		RB_RHI_StreamAmbient( ictx.r, surf->geo, ictx.vb, ictx.vertOfs, ictx.ib, ictx.idxOfs );

		// stage decomposition + register evaluation + depth-range hacks;
		// calls RB_RHI_DrawInteraction 1..n times
		RB_CreateSingleDrawInteractions( surf, RB_RHI_DrawInteraction );
	}
}

/*
===================
RB_RHI_DrawFilmInteractions

DUDE wet blood film (r_decalWetBlood): lights this view's blood decals as surfaces of their own
under backEnd.vLight. Blood only reads wet when something on it mirrors a light to the eye, and
Doom 3's lights are points - so the glint has to come off the blood's OWN relief (bulging
edges, ripples inside), which a wall underneath cannot supply. The frontend cannot light these:
a projected decal (every splat the game spawns, stock or mod) gets no interactions at all, and
stock blood materials have no lit stages. So the backend synthesizes a specular-only interaction
per decal per overlapping light and runs it through the regular interaction path - shadows, RT
modes and PBR all apply. Called with the translucent-chain state (depth LESS, no stencil); the
decal is coplanar with its wall, so it needs the ambient pass's polygon offset to win that test.
===================
*/
// scales r_decalWetBlood onto the specular colour. Calibrated blind and far too hot: tuned
// in-game, the cvar's default landed at 0.057 - hence its 0..0.5 range. Left alone so tuned
// values keep their look.
static const float RHI_FILM_GAIN = 3.0f;

static void RB_RHI_DrawFilmInteractions( rhi::RHI *r, const viewDef_t *viewDef, const viewLight_t *vLight ) {
	const float strength = r_decalWetBlood.GetFloat();
	if ( rhiFilmSurfs.Num() == 0 || strength <= 0.0f || !vLight->lightDef ) {
		return;
	}
	for ( int i = 0; i < rhiFilmSurfs.Num(); i++ ) {
		const rhiFilmSurf_t &film = rhiFilmSurfs[i];
		const drawSurf_t *surf = film.surf;
		if ( R_CullLocalBox( film.bounds, surf->space->modelMatrix, 6, vLight->lightDef->frustum ) ) {
			continue;
		}
		RB_RHI_SpaceMvp( viewDef, surf->space, ictx.mvp );
		RB_RHI_StreamAmbient( r, surf->geo, ictx.vb, ictx.vertOfs, ictx.ib, ictx.idxOfs );

		const idMaterial *shader = surf->material;
		const bool polyOffset = shader->TestMaterialFlag( MF_POLYGONOFFSET );
		if ( polyOffset ) {
			if ( qglEnable != NULL ) {
				qglEnable( GL_POLYGON_OFFSET_FILL );
				qglPolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
			}
			r->SetPolygonOffset( true, r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
		}

		rhiFilmDraw = &film;
		RB_CreateFilmDrawInteractions( surf, film.cover, film.relief, film.reliefStage,
		                               strength * RHI_FILM_GAIN, RB_RHI_DrawInteraction );
		rhiFilmDraw = NULL;

		if ( polyOffset ) {
			if ( qglDisable != NULL ) {
				qglDisable( GL_POLYGON_OFFSET_FILL );
			}
			r->SetPolygonOffset( false, 0.0f, 0.0f );
		}
	}
}

/*
===================
RB_RHI_StencilShadowPass

Mirrors RB_StencilShadowPass/RB_T_Shadow, always through StencilOpSeparate
(core since GL 2.0; the multi-pass fallbacks are for pre-2.0 hardware the
GL3 backend can't run on anyway).
===================
*/
static void RB_RHI_StencilShadowPass( rhi::RHI *r, const viewDef_t *viewDef, const drawSurf_t *drawSurfs, rhi::ShaderHandle shadowProg ) {
	if ( !r_shadows.GetBool() || !drawSurfs ) {
		return;
	}

	if ( r_shadowPolygonFactor.GetFloat() || r_shadowPolygonOffset.GetFloat() ) {
		if ( qglPolygonOffset != NULL ) {
			qglPolygonOffset( r_shadowPolygonFactor.GetFloat(), -r_shadowPolygonOffset.GetFloat() );
			qglEnable( GL_POLYGON_OFFSET_FILL );
		}
		r->SetPolygonOffset( true, r_shadowPolygonFactor.GetFloat(), -r_shadowPolygonOffset.GetFloat() );
	}

	if ( qglStencilFunc != NULL ) {
		qglStencilFunc( GL_ALWAYS, 1, 255 );
	}

	const GLenum firstFace = viewDef->isMirror ? GL_FRONT : GL_BACK;
	const GLenum secondFace = viewDef->isMirror ? GL_BACK : GL_FRONT;
	const bool zFail = r_useCarmacksReverse.GetBool();
	const bool mirror = viewDef->isMirror;

	rhi::PipelineDesc pd;
	pd.stateBits = GLS_DEPTHMASK | GLS_COLORMASK | GLS_ALPHAMASK | GLS_DEPTHFUNC_LESS;
	pd.shader = shadowProg;
	pd.vertexLayout = rhi::VL_SHADOW;
	pd.cullType = CT_TWO_SIDED;

	const viewEntity_t *currentSpace = NULL;
	float mvp[16];
	idVec4 localLight( 0.0f, 0.0f, 0.0f, 0.0f );

	for ( const drawSurf_t *surf = drawSurfs; surf; surf = surf->nextOnLight ) {
		const srfTriangles_t *tri = surf->geo;

		if ( !tri->shadowCache ) {
			continue;
		}

		if ( surf->space != currentSpace ) {
			currentSpace = surf->space;
			RB_RHI_SpaceMvp( viewDef, surf->space, mvp );
			R_GlobalPointToLocal( surf->space->modelMatrix, backEnd.vLight->globalLightOrigin, localLight.ToVec3() );
		}

		// scissor like RB_RenderDrawSurfChainWithFunction
		if ( r_useScissor.GetBool() && !backEnd.currentScissor.Equals( surf->scissorRect ) ) {
			backEnd.currentScissor = surf->scissorRect;
			r->SetScissor( viewDef->viewport.x1 + backEnd.currentScissor.x1,
			               viewDef->viewport.y1 + backEnd.currentScissor.y1,
			               backEnd.currentScissor.x2 + 1 - backEnd.currentScissor.x1,
			               backEnd.currentScissor.y2 + 1 - backEnd.currentScissor.y1 );
		}

		// front/rear cap requirements — identical logic to RB_T_Shadow
		int numIndexes;
		bool external = false;
		if ( !r_useExternalShadows.GetInteger() ) {
			numIndexes = tri->numIndexes;
		} else if ( r_useExternalShadows.GetInteger() == 2 ) {
			numIndexes = tri->numShadowIndexesNoCaps;
		} else if ( !( surf->dsFlags & DSF_VIEW_INSIDE_SHADOW ) ) {
			numIndexes = tri->numShadowIndexesNoCaps;
			external = true;
		} else if ( !backEnd.vLight->viewInsideLight && !( tri->shadowCapPlaneBits & SHADOW_CAP_INFINITE ) ) {
			if ( backEnd.vLight->viewSeesShadowPlaneBits & tri->shadowCapPlaneBits ) {
				numIndexes = tri->numShadowIndexesNoFrontCaps;
			} else {
				numIndexes = tri->numShadowIndexesNoCaps;
			}
			external = true;
		} else {
			numIndexes = tri->numIndexes;
		}

		rhi::RenderParams parms;
		memset( &parms, 0, sizeof( parms ) );
		memcpy( parms.mvpMatrix, mvp, sizeof( parms.mvpMatrix ) );
		memcpy( parms.localLightOrigin, localLight.ToFloatPtr(), 16 );

		rhi::BufferHandle ub;
		int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

		rhi::BufferHandle vb, ib;
		int vertOfs, idxOfs;
		RB_RHI_StreamShadow( r, tri, vb, vertOfs, ib, idxOfs );

		rhi::DrawArgs da;
		memset( &da, 0, sizeof( da ) );
		da.vertexBuffer = vb;
		da.vertexOffset = vertOfs;
		da.indexBuffer = ib;
		da.firstIndex = idxOfs / (int)sizeof( glIndex_t );
		da.indexCount = numIndexes;
		da.uniformBuffer = ub;
		da.uniformOffset = uniOfs;
		da.uniformSize = sizeof( parms );

		// each stencil-op change is a pipeline on Vulkan (pd.stencilState) and
		// free-floating qglStencilOpSeparate state on GL — both driven here
		if ( !zFail ) {
			// depth-pass with preload for volumes clipped by near/far planes
			if ( !external ) {
				if ( qglStencilOpSeparate != NULL ) {
					qglStencilOpSeparate( firstFace, GL_KEEP, tr.stencilDecr, tr.stencilDecr );
					qglStencilOpSeparate( secondFace, GL_KEEP, tr.stencilIncr, tr.stencilIncr );
				}
				pd.stencilState = mirror ? rhi::SS_VOLUME_PRELOAD_MIRROR : rhi::SS_VOLUME_PRELOAD;
				r->BindPipeline( pd );
				r->Draw( da );
			}
			if ( qglStencilOpSeparate != NULL ) {
				qglStencilOpSeparate( firstFace, GL_KEEP, GL_KEEP, tr.stencilIncr );
				qglStencilOpSeparate( secondFace, GL_KEEP, GL_KEEP, tr.stencilDecr );
			}
			pd.stencilState = mirror ? rhi::SS_VOLUME_ZPASS_MIRROR : rhi::SS_VOLUME_ZPASS;
			r->BindPipeline( pd );
			r->Draw( da );
		} else {
			// Carmack's Reverse (Z-fail) — patent expired 2019-10-13
			if ( !external ) {
				if ( qglStencilOpSeparate != NULL ) {
					qglStencilOpSeparate( firstFace, GL_KEEP, tr.stencilDecr, GL_KEEP );
					qglStencilOpSeparate( secondFace, GL_KEEP, tr.stencilIncr, GL_KEEP );
				}
				pd.stencilState = mirror ? rhi::SS_VOLUME_ZFAIL_MIRROR : rhi::SS_VOLUME_ZFAIL;
			} else {
				// external volumes render depth-pass even in z-fail mode
				if ( qglStencilOpSeparate != NULL ) {
					qglStencilOpSeparate( firstFace, GL_KEEP, GL_KEEP, tr.stencilIncr );
					qglStencilOpSeparate( secondFace, GL_KEEP, GL_KEEP, tr.stencilDecr );
				}
				pd.stencilState = mirror ? rhi::SS_VOLUME_ZPASS_MIRROR : rhi::SS_VOLUME_ZPASS;
			}
			r->BindPipeline( pd );
			r->Draw( da );
		}

		backEnd.pc.c_shadowElements++;
		backEnd.pc.c_shadowIndexes += numIndexes;
	}

	if ( r_shadowPolygonFactor.GetFloat() || r_shadowPolygonOffset.GetFloat() ) {
		if ( qglDisable != NULL ) {
			qglDisable( GL_POLYGON_OFFSET_FILL );
		}
		r->SetPolygonOffset( false, 0.0f, 0.0f );
	}

	// interactions test against the unshadowed value
	if ( qglStencilFunc != NULL ) {
		qglStencilFunc( GL_GEQUAL, 128, 255 );
		qglStencilOp( GL_KEEP, GL_KEEP, GL_KEEP );
	}
}

/*
===================
RB_RHI_FillDepthBuffer

Mirrors RB_STD_FillDepthBuffer/RB_T_FillDepthBuffer through the zfill
program (opaque solid, perforated alpha-tested, subview down-modulate).
===================
*/
// Capture the sealed scene depth into _currentDepth (currentDepthImage) for the passes
// that sample it (soft particles, SSAO, SSR). Extracted from the depth prepass so the
// r_ssaoMergeNormal path — which seals depth in the gbuffer prepass instead of zfill —
// captures from the identical point. (M5: CopyDepthbuffer routes through the RHI capture on VK.)
static void RB_RHI_CaptureCurrentDepth( const viewDef_t *viewDef ) {
	bool getDepthCapture = r_enableDepthCapture.GetInteger() == 1
		|| ( r_enableDepthCapture.GetInteger() == -1
		     && ( r_useSoftParticles.GetBool()
		          || ( ( r_ssao.GetBool() || r_ssr.GetBool() || r_rtReflections.GetBool()	// RT reflections read _currentDepth (decoupled from r_ssr 2026-09-23)
		                 || ( r_pbr.GetBool() && ( r_decalMetalMask.GetFloat() > 0.0f || r_decalRoughen.GetFloat() > 0.0f
		                                        || r_decalWetBlood.GetFloat() > 0.0f ) ) )
		               && R_BackendSupportsEnhancements() ) ) );
	if ( getDepthCapture && viewDef->renderView.viewID >= 0
	     && ( qglReadBuffer != NULL || rhi::GetActiveBackendType() == rhi::BT_VULKAN ) ) {
		globalImages->currentDepthImage->CopyDepthbuffer( viewDef->viewport.x1,
			viewDef->viewport.y1,
			viewDef->viewport.x2 - viewDef->viewport.x1 + 1,
			viewDef->viewport.y2 - viewDef->viewport.y1 + 1, true );
	}
}

static void RB_RHI_FillDepthBuffer( rhi::RHI *r, const viewDef_t *viewDef ) {
	rhi::ShaderHandle zfill = r->LoadShader( "zfill" );

	// subview near-clip plane (mirror / camera views): the legacy path forces
	// the alpha test to fail behind the plane with an alpha-notch texgen trick
	// (RB_T_FillDepthBuffer). On core we clip geometrically with
	// gl_ClipDistance[0], written by zfill.vert. Only the depth fill needs it:
	// the interaction and opaque material passes are depth-EQUAL, so they
	// inherit the clip exactly like the legacy notch. The plane is transformed
	// into each surface's model space, matching R_GlobalPlaneToLocal() there.
	const bool useClipPlane = viewDef->numClipPlanes > 0;
	if ( useClipPlane && qglEnable != NULL ) {
		// Vulkan needs no enable: zfill.vert always writes gl_ClipDistance[0]
		// and the plane is zero when unused
		qglEnable( GL_CLIP_DISTANCE0 );
	}

	if ( qglStencilFunc != NULL ) {
		qglStencilFunc( GL_ALWAYS, 1, 255 );
	}

	const viewEntity_t *currentSpace = NULL;
	float mvp[16];
	float localClipPlane[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

	// Phase 3.2b consume (r_vkBdaZfill 2): collect solid-opaque, front-sided, addressable WORLD-BSP
	// surfaces and draw the whole bucket with one indirect call after the loop instead of per-surface.
	// Restricted to worldSpace because all BSP surfaces are one render entity sharing ONE scissor (the
	// portal-chain union the normal path also draws them all with) — so the batch replays that exact
	// scissor. Static-model props (their own per-entity scissors) stay per-surface. Gated off unless the
	// VK backend has BDA + the batch shader; front-sided, non-clip views only. See DrawZfillBatch.
	const bool batchZfill = r->ZfillBatchEnabled()
		&& !useClipPlane		// the batch shader writes no gl_ClipDistance; a clip-plane subview must clip
		&& ( RB_RHI_CullFor( viewDef, CT_FRONT_SIDED ) == CT_FRONT_SIDED );
	static idList<rhi::RHI::ZfillBatchItem> zfillBatchItems;
	static idList<idScreenRect> zfillBatchScissors;		// parallel to zfillBatchItems; the batch groups by this
	zfillBatchItems.SetNum( 0, false );		// reuse capacity across frames
	zfillBatchScissors.SetNum( 0, false );

	drawSurf_t **drawSurfs = (drawSurf_t **)&viewDef->drawSurfs[0];
	for ( int i = 0; i < viewDef->numDrawSurfs; i++ ) {
		const drawSurf_t *surf = drawSurfs[i];
		const srfTriangles_t *tri = surf->geo;
		const idMaterial *shader = surf->material;

		if ( !shader->IsDrawn() ) {
			continue;
		}
		// translucent surfaces don't put anything in the depth buffer
		if ( shader->Coverage() == MC_TRANSLUCENT ) {
			continue;
		}
		// sky surfaces (portal sky, skybox) render at the far plane and must
		// NOT seal depth here — sealing is what makes forceOpaque portal-sky
		// occlude the geometry in front of it (see the sky shaders' z = w)
		texgen_t tg = shader->Texgen();
		if ( tg == TG_SCREEN || tg == TG_SCREEN2 || tg == TG_SKYBOX_CUBE || tg == TG_WOBBLESKY_CUBE ) {
			continue;
		}
		if ( !tri->numIndexes || ( !tri->ambientCache && !tri->gpuSkinVB ) ) {
			continue;
		}

		const float *regs = surf->shaderRegisters;

		// if all stages of a material have been conditioned off, don't draw
		int stage;
		for ( stage = 0; stage < shader->GetNumStages(); stage++ ) {
			if ( regs[shader->GetStage( stage )->conditionRegister] != 0 ) {
				break;
			}
		}
		if ( stage == shader->GetNumStages() ) {
			continue;
		}

		if ( surf->space != currentSpace ) {
			currentSpace = surf->space;
			RB_RHI_SpaceMvp( viewDef, surf->space, mvp );
			if ( useClipPlane ) {
				idPlane local;
				R_GlobalPlaneToLocal( surf->space->modelMatrix, viewDef->clipPlanes[0], local );
				localClipPlane[0] = local[0];
				localClipPlane[1] = local[1];
				localClipPlane[2] = local[2];
				localClipPlane[3] = local[3];
			}
		}
		if ( surf->space->weaponDepthHack ) {
			RB_EnterWeaponDepthHack();
		}
		if ( surf->space->modelDepthHack != 0.0f ) {
			RB_EnterModelDepthHack( surf->space->modelDepthHack );
		}

		if ( r_useScissor.GetBool() && !backEnd.currentScissor.Equals( surf->scissorRect ) ) {
			backEnd.currentScissor = surf->scissorRect;
			r->SetScissor( viewDef->viewport.x1 + backEnd.currentScissor.x1,
			               viewDef->viewport.y1 + backEnd.currentScissor.y1,
			               backEnd.currentScissor.x2 + 1 - backEnd.currentScissor.x1,
			               backEnd.currentScissor.y2 + 1 - backEnd.currentScissor.y1 );
		}

		if ( shader->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
			if ( qglEnable != NULL ) {
				qglEnable( GL_POLYGON_OFFSET_FILL );
				qglPolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
			}
			r->SetPolygonOffset( true, r_offsetFactor.GetFloat(),
			                     r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
		}

		// subviews down-modulate the color buffer, everything else draws black
		int stateBits;
		float color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		if ( shader->GetSort() == SS_SUBVIEW ) {
			stateBits = GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO | GLS_DEPTHFUNC_LESS;
			color[0] = color[1] = color[2] = 1.0f / backEnd.overBright;
		} else {
			stateBits = GLS_DEPTHFUNC_LESS;
		}

		rhi::BufferHandle vb, ib;
		int vertOfs, idxOfs;
		RB_RHI_StreamAmbient( r, tri, vb, vertOfs, ib, idxOfs );

		// DUDE tessellation: the prepass must PN-subdivide enemy/prop surfaces
		// with the exact factors the interaction pass uses, so the sealed depth
		// lines up under the depth-EQUAL interactions (docs/tessellation.md).
		bool tess = RB_RHI_TessellateSurf( surf, false );
		// Roadmap B: draw the pre-deformed expanded buffer if this surface was deform-once dispatched
		// (ApplyDeform rebinds vb/ib/count + clears tess, so the tess-setup below is skipped).
		int idxCount = tri->numIndexes;
		RB_RHI_ApplyDeform( tri, vb, vertOfs, ib, idxOfs, idxCount, tess );

		rhi::PipelineDesc pd;
		pd.stateBits = stateBits;
		pd.shader = zfill;
		pd.vertexLayout = rhi::VL_DRAWVERT;
		pd.cullType = RB_RHI_CullFor( viewDef, CT_FRONT_SIDED );
		pd.stencilState = rhi::SS_ALWAYS;	// stencil test on, always pass (GL parity)
		pd.tessellate = tess;
		RB_RHI_BindTessHeight( surf, tess );	// DUDE: unit 15 signed height (docs/tessellation.md)

		rhi::DrawArgs da;
		memset( &da, 0, sizeof( da ) );
		da.vertexBuffer = vb;
		da.vertexOffset = vertOfs;
		da.indexBuffer = ib;
		da.firstIndex = idxOfs / (int)sizeof( glIndex_t );
		da.indexCount = idxCount;

		bool drawSolid = ( shader->Coverage() == MC_OPAQUE );

		// Phase 3.2b consume mode 2: defer solid-opaque, non-hacked, addressable surfaces into the
		// batched indirect draw, tagged with their scissor (grouped at emit). Everything the batch
		// pipeline/shader can't express (perforated, subview, depth-hack, polygon offset, tessellated,
		// or streamed/skinned geometry with no device address) stays on this per-surface path.
		// Bit-identical: the batch replays the same mvp * vec4(pos,1) with invariant depth.
		if ( batchZfill && drawSolid && shader->GetSort() != SS_SUBVIEW
		     && !surf->space->weaponDepthHack && surf->space->modelDepthHack == 0.0f
		     && !shader->TestMaterialFlag( MF_POLYGONOFFSET ) && !tess ) {
			unsigned long long vbAddr = r->GetBufferDeviceAddress( vb );
			unsigned long long ibAddr = r->GetBufferDeviceAddress( ib );
			if ( vbAddr != 0 && ibAddr != 0 ) {
				rhi::RHI::ZfillBatchItem it;
				it.vbAddr = vbAddr + (unsigned long long)vertOfs;
				it.ibAddr = ibAddr + (unsigned long long)idxOfs;
				it.indexCount = idxCount;
				memcpy( it.mvp, mvp, sizeof( it.mvp ) );
				zfillBatchItems.Append( it );
				zfillBatchScissors.Append( surf->scissorRect );		// grouped by scissor at emit
				continue;		// drawn later as part of the batch
			}
		}

		if ( shader->Coverage() == MC_PERFORATED ) {
			// alpha-tested stages; if none were live, fall back to solid. Each stage draws
			// under its OWN privatePolygonOffset (stock RB_PrepareStageTexturing parity):
			// the burning-corpse dissolve seals its skin threshold at depth D and its fire
			// threshold at D+bias, so the lit passes and the fire stage -- both at
			// DEPTHFUNC_EQUAL -- each match only their own band (docs/burn-away-ember.md).
			bool didDraw = false;
			for ( stage = 0; stage < shader->GetNumStages(); stage++ ) {
				const shaderStage_t *pStage = shader->GetStage( stage );
				if ( !pStage->hasAlphaTest ) {
					continue;
				}
				if ( regs[pStage->conditionRegister] == 0 ) {
					continue;
				}
				didDraw = true;

				color[3] = regs[pStage->color.registers[3]];
				if ( color[3] <= 0.0f ) {
					continue;
				}

				rhi::RenderParams parms;
				memset( &parms, 0, sizeof( parms ) );
				memcpy( parms.mvpMatrix, mvp, sizeof( parms.mvpMatrix ) );
				memcpy( parms.color, color, sizeof( parms.color ) );
				memcpy( parms.clipPlane, localClipPlane, sizeof( parms.clipPlane ) );
				parms.alphaTest[0] = regs[pStage->alphaTestRegister];
				parms.alphaTest[1] = 1.0f;
				if ( pStage->texture.hasMatrix ) {
					parms.diffuseMatrixS[0] = regs[pStage->texture.matrix[0][0]];
					parms.diffuseMatrixS[1] = regs[pStage->texture.matrix[0][1]];
					parms.diffuseMatrixS[3] = regs[pStage->texture.matrix[0][2]];
					parms.diffuseMatrixT[0] = regs[pStage->texture.matrix[1][0]];
					parms.diffuseMatrixT[1] = regs[pStage->texture.matrix[1][1]];
					parms.diffuseMatrixT[3] = regs[pStage->texture.matrix[1][2]];
				} else {
					parms.diffuseMatrixS[0] = 1.0f;
					parms.diffuseMatrixT[1] = 1.0f;
				}
				idImage *tessBump = NULL;
				if ( tess ) {
					RB_RHI_SetTessParms( parms );
					tessBump = RB_RHI_TessBumpForZfill( surf, parms );
				}

				rhi::BufferHandle ub;
				int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

				RB_RHI_StagePolyOffsetBegin( r, pStage );
				RB_RHI_BindUnit( 0, pStage->texture.image );
				if ( tess ) {
					RB_RHI_BindUnit( 1, tessBump );
				}
				RB_RHI_VkTextures( da );
				r->BindPipeline( pd );
				da.uniformBuffer = ub;
				da.uniformOffset = uniOfs;
				da.uniformSize = sizeof( parms );
				r->Draw( da );
				backEnd.pc.c_drawElements++;
				RB_RHI_StagePolyOffsetEnd( r, pStage, shader );
			}
			if ( !didDraw ) {
				drawSolid = true;
			}
		}

		if ( drawSolid ) {
			rhi::RenderParams parms;
			memset( &parms, 0, sizeof( parms ) );
			memcpy( parms.mvpMatrix, mvp, sizeof( parms.mvpMatrix ) );
			memcpy( parms.color, color, sizeof( parms.color ) );
			memcpy( parms.clipPlane, localClipPlane, sizeof( parms.clipPlane ) );
			parms.diffuseMatrixS[0] = 1.0f;
			parms.diffuseMatrixT[1] = 1.0f;
			idImage *tessBump = NULL;
			if ( tess ) {
				RB_RHI_SetTessParms( parms );
				tessBump = RB_RHI_TessBumpForZfill( surf, parms );
			}

			rhi::BufferHandle ub;
			int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

			RB_RHI_BindUnit( 0, globalImages->whiteImage );
			if ( tess ) {
				RB_RHI_BindUnit( 1, tessBump );
			}
			RB_RHI_VkTextures( da );
			r->BindPipeline( pd );
			da.uniformBuffer = ub;
			da.uniformOffset = uniOfs;
			da.uniformSize = sizeof( parms );
			r->Draw( da );
			backEnd.pc.c_drawElements++;
		}

		if ( shader->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
			if ( qglDisable != NULL ) {
				qglDisable( GL_POLYGON_OFFSET_FILL );
			}
			r->SetPolygonOffset( false, 0.0f, 0.0f );
		}
		if ( surf->space->weaponDepthHack || surf->space->modelDepthHack != 0.0f ) {
			RB_LeaveDepthHack();
		}
	}

	// Phase 3.2b consume mode 2: draw the deferred bucket as one indirect call PER DISTINCT SCISSOR.
	// Surfaces of the same entity share a scissor, so this collapses each entity's surfaces (and the
	// BSP's) into one draw while each group rasterizes with its exact scissor — pixel-identical, and
	// safe for portal-clipped entities (which ignoring the scissor would not be). Clear the polygon
	// bias once up front; the loop may have left a per-surface scissor, but every group sets its own.
	if ( batchZfill && zfillBatchItems.Num() > 0 ) {
		r->SetPolygonOffset( false, 0.0f, 0.0f );
		// reorder items so each scissor's items are contiguous, and record one group (range + scissor)
		// per distinct scissor. One DrawZfillBatch call uploads all items once and draws per group.
		static idList<rhi::RHI::ZfillBatchItem> ordered;
		static idList<rhi::RHI::ZfillBatchGroup> groups;
		static idList<bool> used;
		ordered.SetNum( 0, false );
		groups.SetNum( 0, false );
		const int n = zfillBatchItems.Num();
		used.SetNum( n, false );
		for ( int k = 0; k < n; k++ ) {
			used[k] = false;
		}
		for ( int a = 0; a < n; a++ ) {
			if ( used[a] ) {
				continue;
			}
			const idScreenRect sc = zfillBatchScissors[a];
			rhi::RHI::ZfillBatchGroup grp;
			grp.firstItem = ordered.Num();
			for ( int b = a; b < n; b++ ) {
				if ( !used[b] && zfillBatchScissors[b].Equals( sc ) ) {
					ordered.Append( zfillBatchItems[b] );
					used[b] = true;
				}
			}
			grp.itemCount = ordered.Num() - grp.firstItem;
			// GL-convention scissor rect (as SetScissor takes); full view when scissoring is off
			if ( r_useScissor.GetBool() ) {
				grp.scissorX = viewDef->viewport.x1 + sc.x1;
				grp.scissorY = viewDef->viewport.y1 + sc.y1;
				grp.scissorW = sc.x2 + 1 - sc.x1;
				grp.scissorH = sc.y2 + 1 - sc.y1;
			} else {
				grp.scissorX = viewDef->viewport.x1 + viewDef->scissor.x1;
				grp.scissorY = viewDef->viewport.y1 + viewDef->scissor.y1;
				grp.scissorW = viewDef->scissor.x2 + 1 - viewDef->scissor.x1;
				grp.scissorH = viewDef->scissor.y2 + 1 - viewDef->scissor.y1;
			}
			groups.Append( grp );
		}
		r->DrawZfillBatch( ordered.Ptr(), ordered.Num(), groups.Ptr(), groups.Num() );
	}

	if ( useClipPlane && qglDisable != NULL ) {
		qglDisable( GL_CLIP_DISTANCE0 );
	}

	// make the early depth pass available to shaders (soft particles, SSAO, SSR, etc.)
	RB_RHI_CaptureCurrentDepth( viewDef );
}

// Static/dynamic split (r_shadowMapCacheSplit): which layer a cube-shadow caster belongs
// to. A dynamic caster is one whose model regenerates geometry every frame (animated md5s,
// ragdolls, particles — IsDynamicModel() != DM_STATIC); it can never cache-hit, so it goes
// into the per-frame dynamic cube. Everything else (world BSP + static-model props) is
// static and goes into the cached cube. This is the SAME per-caster test RB_RHI_CubeToken
// uses to raise its 'dynamic' flag, so the token classification and the render can never
// disagree about which layer a caster lands in.
enum casterFilter_t { CF_ALL, CF_STATIC, CF_DYNAMIC };
static bool RB_RHI_CasterIsDynamic( const drawSurf_t *surf ) {
	const idRenderEntityLocal *edef = surf->space ? surf->space->entityDef : NULL;
	return edef && edef->parms.hModel && edef->parms.hModel->IsDynamicModel() != DM_STATIC;
}

/*
===================
RB_RHI_ShadowCasterAllowed

Shared caster filter for the 2D and cube shadow passes. Mirrors Interaction.cpp's
shadow rules (material casts, entity not noShadow, per-view/per-light suppression)
plus the perforated override that lets noShadows grates/fences cast. The suppression
check is what keeps the player's own first-person weapon out of the map in his view.
===================
*/
static bool RB_RHI_ShadowCasterAllowed( const drawSurf_t *surf ) {
	// grates / fences / foliage are nearly always flagged noShadows in the base
	// assets (textures/base_floor/sflgratetrans*, decals/fgrill3) because a solid
	// stencil volume can't punch holes. A shadow map can, so the perforated override
	// lets them cast, ignoring the noShadows flags that only ever served the stencil path.
	const bool perforatedOverride = r_shadowMapPerforated.GetBool()
	    && surf->material && surf->material->Coverage() == MC_PERFORATED;

	// The first-person view model (weaponDepthHack) is at the player's world position, so
	// anything it casts into a room light's shadow map lands as a gun-shaped blob on the
	// nearby floor/walls (see Interaction.cpp). A shared shadow map can't self-shadow the
	// weapon without also casting it on the world, so keep the whole view model out of the
	// map. r_shadowMapViewWeapon (default 0) gates this: 0 => never cast; 1 => cast even when
	// the material is flagged noShadows. Translucent invis skins never cast either way.
	if ( surf->space->weaponDepthHack ) {
		if ( !r_shadowMapViewWeapon.GetBool()
		     || ( surf->material && surf->material->Coverage() == MC_TRANSLUCENT ) ) {
			return false;
		}
	} else if ( surf->material && !surf->material->SurfaceCastsShadow() && !perforatedOverride ) {
		return false;
	}
	const idRenderEntityLocal *edef = surf->space->entityDef;
	if ( edef ) {
		if ( edef->parms.noShadow && !perforatedOverride ) {
			return false;
		}
		if ( !r_skipSuppress.GetBool() ) {
			if ( edef->parms.suppressShadowInViewID
			     && edef->parms.suppressShadowInViewID == backEnd.viewDef->renderView.viewID ) {
				return false;
			}
			if ( backEnd.vLight->lightDef
			     && edef->parms.suppressShadowInLightID
			     && edef->parms.suppressShadowInLightID == backEnd.vLight->lightDef->parms.lightId ) {
				return false;
			}
		}
	}
	return true;
}

/*
===================
RB_RHI_SetupCasterCoverage

Shared perforated-caster setup for the 2D and cube passes. Routes the first live
alpha-tested stage's coverage texture + threshold + matrix into parms (so the caster
shader punches the shadow out exactly like the visible surface), picks the caster
cull mode, sets *perforated for the debug counter, and returns the coverage image to
bind on unit 0 (whiteImage for opaque casters, whose disabled alpha test never fires).
===================
*/
static idImage *RB_RHI_SetupCasterCoverage( const drawSurf_t *surf, rhi::RenderParams &parms,
                                            int &smCull, bool &perforated ) {
	idImage *coverImage = globalImages->whiteImage;
	perforated = surf->material && surf->material->Coverage() == MC_PERFORATED;
	if ( perforated ) {
		const float *regs = surf->shaderRegisters;
		const shaderStage_t *aStage = NULL;
		for ( int stage = 0; regs && stage < surf->material->GetNumStages(); stage++ ) {
			const shaderStage_t *pStage = surf->material->GetStage( stage );
			if ( pStage->hasAlphaTest && regs[pStage->conditionRegister] != 0 ) {
				aStage = pStage;
				break;
			}
		}
		if ( aStage ) {
			parms.alphaTest[0] = regs[aStage->alphaTestRegister];
			parms.alphaTest[1] = 1.0f;
			// z = perforated shadow strength (r_shadowMapPerforatedStrength): < 1 makes the
			// caster shaders dither-discard map texels so PCF lightens the shadow (grates/
			// fences stop blacking out whole rooms; vanilla faked these with light textures)
			parms.alphaTest[2] = idMath::ClampFloat( 0.0f, 1.0f, r_shadowMapPerforatedStrength.GetFloat() );
			if ( aStage->texture.hasMatrix ) {
				parms.diffuseMatrixS[0] = regs[aStage->texture.matrix[0][0]];
				parms.diffuseMatrixS[1] = regs[aStage->texture.matrix[0][1]];
				parms.diffuseMatrixS[3] = regs[aStage->texture.matrix[0][2]];
				parms.diffuseMatrixT[0] = regs[aStage->texture.matrix[1][0]];
				parms.diffuseMatrixT[1] = regs[aStage->texture.matrix[1][1]];
				parms.diffuseMatrixT[3] = regs[aStage->texture.matrix[1][2]];
			} else {
				parms.diffuseMatrixS[0] = 1.0f;
				parms.diffuseMatrixT[1] = 1.0f;
			}
			if ( aStage->texture.image ) {
				coverImage = aStage->texture.image;
			}
		}
		// perforated casters are typically thin, single-sided planes; second-depth
		// back-face culling would drop them entirely depending on which way they face
		// the light, so always render both sides. Being thin, they have no self-shadow
		// acne for second-depth to fix in the first place.
		smCull = CT_TWO_SIDED;
	} else {
		// caster face selection (r_shadowMapCull): rendering only back faces
		// ("second-depth") keeps directly-lit front faces out of the map, which is the
		// standard cure for grazing-angle self-shadow acne. 0/1/2 = front/back/two-sided.
		smCull = CT_BACK_SIDED;
		if ( r_shadowMapCull.GetInteger() == 0 ) {
			smCull = CT_FRONT_SIDED;
		} else if ( r_shadowMapCull.GetInteger() == 2 ) {
			smCull = CT_TWO_SIDED;
		}
	}
	return coverImage;
}

/*
===================
RB_RHI_ShadowCasterChain

Renders one interaction chain's depth from a projected light's point of view through
the shadow_sm program. The light-projection planes are transformed into each surface's
model space exactly like the interaction pass, so shadow_sm.vert projects to the same
cookie UV and writes the linear falloff as depth.

projPlanes overrides the projection: 4 world-space planes (S, T, Q, falloff/depth) —
the sun pass (RB_RHI_ShadowMapPassSun) renders through a per-view fitted VIRTUAL
projection instead of the light's own texgen. NULL = the light's lightProject[0..3].
===================
*/
static void RB_RHI_ShadowCasterChain( rhi::RHI *r, const drawSurf_t *surf, rhi::ShaderHandle prog,
                                      const idPlane *projPlanes = NULL ) {
	if ( projPlanes == NULL ) {
		projPlanes = backEnd.vLight->lightProject;		// idPlane[4]: S, T, Q, falloff
	}
	for ( ; surf; surf = surf->nextOnLight ) {
		const srfTriangles_t *tri = surf->geo;
		if ( !tri || ( !tri->ambientCache && !tri->gpuSkinVB ) || !tri->numIndexes ) {
			continue;
		}
		if ( !RB_RHI_ShadowCasterAllowed( surf ) ) {
			continue;
		}

		rhi::RenderParams parms;
		memset( &parms, 0, sizeof( parms ) );
		idPlane lp;
		R_GlobalPlaneToLocal( surf->space->modelMatrix, projPlanes[0], lp );
		memcpy( parms.lightProjectionS, lp.ToFloatPtr(), 16 );
		R_GlobalPlaneToLocal( surf->space->modelMatrix, projPlanes[1], lp );
		memcpy( parms.lightProjectionT, lp.ToFloatPtr(), 16 );
		R_GlobalPlaneToLocal( surf->space->modelMatrix, projPlanes[2], lp );
		memcpy( parms.lightProjectionQ, lp.ToFloatPtr(), 16 );
		R_GlobalPlaneToLocal( surf->space->modelMatrix, projPlanes[3], lp );
		memcpy( parms.lightFalloffS, lp.ToFloatPtr(), 16 );

		int smCull;
		bool perforatedCaster;
		idImage *coverImage = RB_RHI_SetupCasterCoverage( surf, parms, smCull, perforatedCaster );

		// DUDE tessellation: a PN-tessellated + displaced character must cast from its
		// DEFORMED surface, or its shadow keeps the low-poly silhouette while the lit
		// body is rounded. shadow_sm.tese runs the same dudeTessPN + dudeTessDisplace as
		// zfill, driven by the same global tess params + bump, so the occluder surface
		// coincides with the receiver's lit surface by construction. GL3 never tessellates.
		idImage *bumpImg = NULL;
		bool useDeform = false;
		const bool tess = RB_RHI_TessOrDeform( surf, tri, useDeform );
		if ( tess ) {
			RB_RHI_SetTessParms( parms );
			bumpImg = RB_RHI_TessBumpForZfill( surf, parms );	// sets bumpMatrix; tese displaces
		}

		rhi::BufferHandle vb, ib;
		int vertOfs, idxOfs;
		RB_RHI_StreamAmbient( r, tri, vb, vertOfs, ib, idxOfs );

		rhi::BufferHandle ub;
		int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

		rhi::PipelineDesc pd;
		pd.stateBits = GLS_DEPTHFUNC_LESS;			// depth write on; color discarded (drawbuffer NONE)
		pd.shader = prog;
		pd.vertexLayout = rhi::VL_DRAWVERT;
		pd.cullType = smCull;
		pd.tessellate = tess;
		RB_RHI_BindTessHeight( surf, tess );	// DUDE: unit 15 signed height (docs/tessellation.md)

		RB_RHI_BindUnit( 0, coverImage );
		if ( tess ) {
			RB_RHI_BindUnit( 1, bumpImg );	// shadow_sm.tese displacement source (unit 0 is coverage)
		}
		r->BindPipeline( pd );

		rhi::DrawArgs da;
		memset( &da, 0, sizeof( da ) );
		da.vertexBuffer = useDeform ? tri->tessDeformVB : vb;
		da.vertexOffset = useDeform ? 0 : vertOfs;
		da.indexBuffer = useDeform ? tri->tessDeformIB : ib;
		da.firstIndex = useDeform ? 0 : ( idxOfs / (int)sizeof( glIndex_t ) );
		da.indexCount = useDeform ? tri->tessDeformIndexes : tri->numIndexes;
		da.uniformBuffer = ub;
		da.uniformOffset = uniOfs;
		da.uniformSize = sizeof( parms );
		RB_RHI_VkTextures( da );		// VK: coverage (unit 0) + bump (unit 1 when tessellating) into DrawArgs
		r->Draw( da );

		backEnd.pc.c_shadowElements++;
		if ( perforatedCaster ) {
			rhiShadowPerfCasters++;
		}
	}
}

// Forward decls: the 2D shadow-map cache (RB_RHI_Acquire2DTarget) and the shared token
// hash (RB_RHI_CubeToken) live further down with the point-cube cache, but this 2D pass
// uses them. r_shadowMapDebug counters: 2D cache hits vs maps actually rendered.
static unsigned long long RB_RHI_CubeToken( const viewLight_t *vLight, float range, int size, bool *outDynamic, unsigned long long *outLightTok = NULL, bool staticOnly = false );
static rhi::RenderTargetHandle RB_RHI_Acquire2DTarget( rhi::RHI *r, int lightIndex, int size, unsigned long long token, bool &hit );
static unsigned long long RB_RHI_HashBytes( unsigned long long h, const void *data, size_t n );
static int rhiMapCacheHits = 0;
static int rhiMapCacheRendered = 0;

/*
===================
RB_RHI_ShadowMapPass

Renders the current light's occluder depth into the shared shadow-map target.
Returns false (→ caller uses the stencil path) if the target can't be created.
===================
*/
static bool RB_RHI_ShadowMapPass( rhi::RHI *r, viewLight_t *vLight, rhi::ShaderHandle prog ) {
	// cap at the GL context's 2D texture limit so large requests degrade gracefully
	const int mapHi = idMath::ClampInt( 256, 4096, glConfig.maxTextureSize );
	const int base = idMath::ClampInt( 256, mapHi, r_shadowMapSize.GetInteger() );
	// adaptive per-light resolution from the light radius (RB_RHI_ShadowTier); the
	// pool keeps one target per tier so alternating light sizes don't thrash. The
	// rhiShadowMap/Size globals track the selected slot because DrawInteraction reads
	// rhiShadowMapSize for the texel size and the caller samples GetRenderTargetImage.
	const float radius = vLight->lightDef->parms.lightRadius.Length();
	const int tier = RB_RHI_ShadowTier( radius );
	const int size = RB_RHI_TierSize( base, tier, 256, mapHi );
	// static cache (the 2D analogue of the point-cube cache, section 5): a projected light
	// whose pose and casters are unchanged since last frame samples the stored map and
	// skips the render. Reuses the cube token (range is meaningless for 2D -- pass 0);
	// pose + axis + caster set + size fully key a static projected light's depth map. A
	// light with an animated caster reports dynamic and stays on the scratch pool (never
	// hits). rhiShadowMap/Size track the selected target; the caller samples it right away.
	const int lightIndex = vLight->lightDef->index;
	bool dynamic = false;
	const unsigned long long token = RB_RHI_CubeToken( vLight, 0.0f, size, &dynamic );
	bool hit = false;
	rhiShadowMap = dynamic ? 0 : RB_RHI_Acquire2DTarget( r, lightIndex, size, token, hit );
	if ( rhiShadowMap == 0 ) {
		rhiShadowMap = RB_RHI_ShadowPoolTarget( r, false, tier, size );		// scratch fallback
	}
	rhiShadowMapSize = size;
	if ( rhiShadowMap == 0 ) {
		return false;
	}
	if ( hit ) {
		rhiMapCacheHits++;
		return true;		// unchanged since last frame -- sample the stored map, skip render
	}
	rhiMapCacheRendered++;

	rhi::ClearArgs clear;
	memset( &clear, 0, sizeof( clear ) );
	clear.depth = true;
	r->BeginTargetPass( rhiShadowMap, &clear );

	// no polygon offset: shadow_sm writes gl_FragDepth, which polygon offset does
	// not affect — the depth-compare bias (r_shadowMapBias) does the acne control
	// single complete occluder set (full ambientTris, view-independent) built in
	// idInteraction::AddActiveInteraction — see the shadowMapCasters comment there
	RB_RHI_ShadowCasterChain( r, vLight->shadowMapCasters, prog );

	r->EndPass();		// restores the backbuffer + the main view's viewport
	return true;
}

/*
===================
Sun shadow maps (r_shadowMapSun — docs/shadow-research.md item 1, milestone 1)

Oversize "sun replacement" omni lights and parallel lights can't use the per-light
shadow paths — a cube can't resolve a shadow thrown thousands of units, and there is
no directional projection — so they used to fall back to Carmack stencil volumes
(fill-rate heavy, low-poly silhouettes, the CPU volume build). Instead, render their
occluders through a per-view fitted VIRTUAL projection into the ordinary 2D pass:

 - The covered region is the view frustum's bounding sphere out to r_shadowMapSunRange
   (a sphere, so the fit's SIZE is rotation-invariant — no wobble as the camera turns).
 - An oversize omni gets a perspective frustum from the light origin subtending that
   sphere (a virtual spot light aimed at the view); a parallel light gets an ortho
   projection along its direction (Q plane == constant 1 runs through the same
   shadow_sm math unchanged: ndc = 2*s - q with q == 1).
 - The projection is expressed as the same 4 texgen planes (S, T, Q, depth) the whole
   2D pipeline already speaks; shadow_sm renders the casters, and the receiver samples
   with the same planes in shader mode 3 (compare ref = the virtual depth plane).
 - The fit is quantized (sphere center snapped to a texel-scale grid) so an idle view
   produces bit-identical planes and a stable cache token — the static 2D cache then
   skips the re-render entirely while nothing moves.

The fitted planes are stashed in rhiSunPlanes (declared with the shadow globals up top)
for the receiver fill; a successful pass sets ictx.lightShadowMapped + lightSunShadow,
which suppresses the stencil draw for this light (useStencil sees shadowMapped) — that
is the fps win.
===================
*/
static bool RB_RHI_ShadowMapPassSun( rhi::RHI *r, viewLight_t *vLight, rhi::ShaderHandle prog ) {
	const viewDef_t *viewDef = backEnd.viewDef;
	if ( !vLight->shadowMapCasters || !vLight->lightDef ) {
		return false;
	}

	// ---- fit: bounding sphere of the view frustum out to r_shadowMapSunRange ----
	const float range = r_shadowMapSunRange.GetFloat();
	const idVec3 org = viewDef->renderView.vieworg;
	const idVec3 fwd = viewDef->renderView.viewaxis[0];
	const float tx = idMath::Tan( DEG2RAD( viewDef->renderView.fov_x * 0.5f ) );
	const float ty = idMath::Tan( DEG2RAD( viewDef->renderView.fov_y * 0.5f ) );
	// sphere centered halfway out on the view axis; radius reaches the far corners
	// (and trivially contains the near end). Not minimal, but stable and simple.
	idVec3 C = org + fwd * ( 0.5f * range );
	const float R = idMath::Sqrt( 0.25f + tx * tx + ty * ty ) * range;

	// quantize the center so a stationary view yields identical planes + cache token
	const float grid = R / 32.0f;
	for ( int i = 0; i < 3; i++ ) {
		C[i] = idMath::Rint( C[i] / grid ) * grid;
	}

	// ---- virtual projection planes (world space) ----
	const bool isParallel = vLight->lightDef->parms.parallel;
	idVec3 n;			// projection axis, pointing away from the light
	float zNear, zFar;	// depth-plane span along n (world units)
	if ( isParallel ) {
		// parallel: shadows travel opposite the (normalized) lightCenter direction
		n = vLight->lightDef->parms.lightCenter;
		if ( n.Normalize() == 0.0f ) {
			n.Set( 0.0f, 0.0f, 1.0f );		// same default as R_DeriveLightData
		}
		n = -n;
		// pancake: catch casters far toward the light (ceilings, skylights, terrain)
		zNear = C * n - 8192.0f;
		zFar  = C * n + R;
	} else {
		// oversize omni: perspective from the light origin, subtending the sphere
		const idVec3 O = vLight->globalLightOrigin;
		n = C - O;
		const float dist = n.Normalize();
		if ( dist <= R * 1.05f ) {
			return false;		// light inside/near the covered region: no usable frustum -> stencil
		}
		// depth-plane span in ABSOLUTE n·P terms (n·C == n·O + dist), matching the
		// depth plane below which dots world positions directly
		zNear = C * n - R;
		zFar  = C * n + R;
		// build the S/T planes about the light origin below; fall through with n set
	}

	idVec3 rightV = ( idMath::Fabs( n.z ) < 0.99f ) ? ( n.Cross( idVec3( 0, 0, 1 ) ) ) : ( n.Cross( idVec3( 1, 0, 0 ) ) );
	rightV.Normalize();
	idVec3 upV = rightV.Cross( n );

	// planes in the a*x+b*y+c*z+d form shadow_sm consumes: s=dot(P,S), t=dot(P,T),
	// q=dot(P,Q), depth=dot(P,F); ndc = (2s-q, 2t-q, ., q)
	float fitWidth = 2.0f * R;		// world width the map spans at the fit center (ortho exact)
	if ( isParallel ) {
		// ortho: s = 0.5 + (P-C)·right/(2R); q = 1; depth spans [zNear, zFar] along n
		rhiSunPlanes[0].SetNormal( rightV / ( 2.0f * R ) );
		rhiSunPlanes[0][3] = 0.5f - ( C * rightV ) / ( 2.0f * R );
		rhiSunPlanes[1].SetNormal( upV / ( 2.0f * R ) );
		rhiSunPlanes[1][3] = 0.5f - ( C * upV ) / ( 2.0f * R );
		rhiSunPlanes[2].SetNormal( vec3_origin );
		rhiSunPlanes[2][3] = 1.0f;
	} else {
		// perspective from O: with p = P - O, z = p·n, tanT covering the sphere:
		// s/q = 0.5 + (p·right)/(2 z tanT), q = z  ->  S = 0.5*n + right/(2 tanT)
		const idVec3 O = vLight->globalLightOrigin;
		const float dist = ( C - O ).Length();
		const float tanT = R / idMath::Sqrt( dist * dist - R * R );
		fitWidth = 2.0f * dist * tanT;		// frustum width at the fit center
		idVec3 sN = 0.5f * n + rightV / ( 2.0f * tanT );
		rhiSunPlanes[0].SetNormal( sN );
		rhiSunPlanes[0][3] = -( sN * O );
		idVec3 tN = 0.5f * n + upV / ( 2.0f * tanT );
		rhiSunPlanes[1].SetNormal( tN );
		rhiSunPlanes[1][3] = -( tN * O );
		rhiSunPlanes[2].SetNormal( n );
		rhiSunPlanes[2][3] = -( n * O );
	}
	// depth plane: 0..1 over [zNear, zFar] along n (both species)
	rhiSunPlanes[3].SetNormal( n / ( zFar - zNear ) );
	rhiSunPlanes[3][3] = -zNear / ( zFar - zNear );

	// ---- target + cache ----
	const int mapHi = idMath::ClampInt( 256, 4096, glConfig.maxTextureSize );
	const int size = idMath::ClampInt( 256, mapHi, r_shadowMapSize.GetInteger() );	// base res; no radius tiering (the fit IS the sizing)
	const int lightIndex = vLight->lightDef->index;
	bool dynamic = false;
	unsigned long long token = RB_RHI_CubeToken( vLight, 0.0f, size, &dynamic );
	// fold the quantized fit into the token: a moved view = new planes = re-render;
	// an idle view = identical planes = static-cache hit. R rides along because every
	// plane scales with it and it changes with fov (scripted zooms, g_fov) even while
	// C and range hold still — without it a fov change would sample a stale map
	// through mismatched planes.
	token = RB_RHI_HashBytes( token, C.ToFloatPtr(), 3 * (int)sizeof( float ) );
	token = RB_RHI_HashBytes( token, &range, (int)sizeof( range ) );
	token = RB_RHI_HashBytes( token, &R, (int)sizeof( R ) );
	bool hit = false;
	rhiShadowMap = dynamic ? 0 : RB_RHI_Acquire2DTarget( r, lightIndex, size, token, hit );
	if ( rhiShadowMap == 0 ) {
		rhiShadowMap = RB_RHI_ShadowPoolTarget( r, false, -SHADOW_TIER_MIN, size );	// scratch fallback (base tier)
	}
	rhiShadowMapSize = size;
	rhiSunTexelWorld = fitWidth / (float)size;	// for the normal-offset bias (set on cache hits too)
	if ( rhiShadowMap == 0 ) {
		return false;
	}
	if ( hit ) {
		rhiMapCacheHits++;
		return true;		// planes re-derived above are bit-identical to the cached render
	}
	rhiMapCacheRendered++;

	rhi::ClearArgs clear;
	memset( &clear, 0, sizeof( clear ) );
	clear.depth = true;
	r->BeginTargetPass( rhiShadowMap, &clear );
	RB_RHI_ShadowCasterChain( r, vLight->shadowMapCasters, prog, rhiSunPlanes );
	r->EndPass();
	return true;
}

/*
===================
Point-light (omni) cube shadow maps
===================
*/

// The six cube faces in the standard GL convention: (forward, up) pairs for a camera
// at the light looking along each axis. Rendering GL face N with this basis makes the
// depth we store line up with the face GL selects when interaction.frag samples with
// the world-space light->frag direction — so no coordinate juggling is needed.
static const idVec3 cubeFaceForward[6] = {
	idVec3(  1,  0,  0 ), idVec3( -1,  0,  0 ),
	idVec3(  0,  1,  0 ), idVec3(  0, -1,  0 ),
	idVec3(  0,  0,  1 ), idVec3(  0,  0, -1 ) };
static const idVec3 cubeFaceUp[6] = {
	idVec3(  0, -1,  0 ), idVec3(  0, -1,  0 ),
	idVec3(  0,  0,  1 ), idVec3(  0,  0, -1 ),
	idVec3(  0, -1,  0 ), idVec3(  0, -1,  0 ) };

// column-major (GL) 4x4 multiply: out = a * b
static void RB_RHI_Mat4Mul( const float a[16], const float b[16], float out[16] ) {
	for ( int c = 0; c < 4; c++ ) {
		for ( int rr = 0; rr < 4; rr++ ) {
			out[c * 4 + rr] = a[0 * 4 + rr] * b[c * 4 + 0] + a[1 * 4 + rr] * b[c * 4 + 1]
			                + a[2 * 4 + rr] * b[c * 4 + 2] + a[3 * 4 + rr] * b[c * 4 + 3];
		}
	}
}

// 90-degree perspective * look-at-from-origin for one cube face. Geometry fed to this
// is already in light-relative, world-oriented space (shadow_sm_cube.vert). gl_FragDepth
// overrides depth, so near/far only bound clipping.
static void RB_RHI_CubeFaceViewProj( int face, float range, float m[16] ) {
	const idVec3 &f = cubeFaceForward[face];
	idVec3 s = f.Cross( cubeFaceUp[face] );
	s.Normalize();
	idVec3 u = s.Cross( f );

	// view (world->view), eye at origin, looking down -z along f
	float V[16];
	V[0] = s.x;  V[4] = s.y;  V[8]  = s.z;  V[12] = 0.0f;
	V[1] = u.x;  V[5] = u.y;  V[9]  = u.z;  V[13] = 0.0f;
	V[2] = -f.x; V[6] = -f.y; V[10] = -f.z; V[14] = 0.0f;
	V[3] = 0.0f; V[7] = 0.0f; V[11] = 0.0f; V[15] = 1.0f;

	const float n = 1.0f;
	const float fr = ( range > n + 1.0f ) ? range : ( n + 1.0f );
	float P[16];
	memset( P, 0, sizeof( P ) );
	P[0]  = 1.0f;						// 1/tan(45) : 90-degree horizontal fov
	P[5]  = 1.0f;						// 90-degree vertical fov (aspect 1)
	P[10] = -( fr + n ) / ( fr - n );
	P[11] = -1.0f;
	P[14] = -( 2.0f * fr * n ) / ( fr - n );

	RB_RHI_Mat4Mul( P, V, m );			// m = P * V
}

// Gribb-Hartmann frustum planes from a light-relative view-projection, translated into
// world space (origin at the light). R_CullLocalBox uses OUTWARD-pointing planes — it
// culls when a box's corners are all on the positive (outside) side, or its sphere
// center is farther than its radius on the positive side — so we negate the standard
// (inward) Gribb-Hartmann planes to match. Normalized so the radius test is correct.
static void RB_RHI_ExtractWorldFrustum( const float m[16], const idVec3 &lightOrigin, idPlane out[6] ) {
	// rows of the column-major matrix
	float row[4][4];
	for ( int i = 0; i < 4; i++ ) {
		row[i][0] = m[0 + i]; row[i][1] = m[4 + i]; row[i][2] = m[8 + i]; row[i][3] = m[12 + i];
	}
	static const int   axis[6] = { 0, 0, 1, 1, 2, 2 };		// left/right, bottom/top, near/far
	static const float sign[6] = { 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f };
	for ( int p = 0; p < 6; p++ ) {
		idPlane pl;
		// negated (outward): the interior is the negative half-space, as R_CullLocalBox expects
		for ( int k = 0; k < 4; k++ ) {
			pl[k] = -( row[3][k] + sign[p] * row[axis[p]][k] );
		}
		// normalize all four components (idPlane::Normalize leaves d unscaled, which
		// would break the radius test in R_CullLocalBox)
		float inv = idMath::InvSqrt( pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2] );
		pl[0] *= inv; pl[1] *= inv; pl[2] *= inv; pl[3] *= inv;
		out[p] = pl.Translate( lightOrigin );	// light-relative -> world
	}
}

// One cube face of a point light's occluder depth. Same caster rules + perforated
// coverage as the 2D pass; adds a per-face frustum cull so a face only pays for the
// geometry actually inside its 90-degree cone.
static void RB_RHI_ShadowCasterChainCube( rhi::RHI *r, const drawSurf_t *surf, rhi::ShaderHandle prog,
                                          const float faceViewProj[16], const idPlane facePlanes[6],
                                          const idVec3 &globalLightOrigin, float range,
                                          casterFilter_t filter ) {
	for ( ; surf; surf = surf->nextOnLight ) {
		const srfTriangles_t *tri = surf->geo;
		if ( !tri || ( !tri->ambientCache && !tri->gpuSkinVB ) || !tri->numIndexes ) {
			continue;
		}
		if ( !RB_RHI_ShadowCasterAllowed( surf ) ) {
			continue;
		}
		// static/dynamic split (r_shadowMapCacheSplit): render only the layer we're
		// filling. CF_ALL (legacy) keeps every allowed caster.
		if ( filter != CF_ALL ) {
			const bool dyn = RB_RHI_CasterIsDynamic( surf );
			if ( ( filter == CF_STATIC && dyn ) || ( filter == CF_DYNAMIC && !dyn ) ) {
				continue;
			}
		}
		// per-face cull: skip casters whose world bounds miss this face's cone
		if ( R_CullLocalBox( tri->bounds, surf->space->modelMatrix, 6, facePlanes ) ) {
			continue;
		}

		rhi::RenderParams parms;
		memset( &parms, 0, sizeof( parms ) );
		memcpy( parms.mvpMatrix, faceViewProj, sizeof( parms.mvpMatrix ) );

		idVec3 localLight;
		R_GlobalPointToLocal( surf->space->modelMatrix, globalLightOrigin, localLight );
		parms.localLightOrigin[0] = localLight.x;
		parms.localLightOrigin[1] = localLight.y;
		parms.localLightOrigin[2] = localLight.z;

		const float *mm = surf->space->modelMatrix;
		for ( int rowi = 0; rowi < 3; rowi++ ) {
			float *dst = rowi == 0 ? parms.modelMatrixRow0 : ( rowi == 1 ? parms.modelMatrixRow1 : parms.modelMatrixRow2 );
			dst[0] = mm[rowi]; dst[1] = mm[rowi + 4]; dst[2] = mm[rowi + 8]; dst[3] = mm[rowi + 12];
		}
		parms.shadowParms[3] = range;		// radial-distance normalizer

		int smCull;
		bool perforatedCaster;
		idImage *coverImage = RB_RHI_SetupCasterCoverage( surf, parms, smCull, perforatedCaster );
		// the cube face view-projections rasterize the opposite winding to the
		// projected 2D pass, so front/back are reversed here: swap them back so
		// r_shadowMapCull's "second-depth" (back) still means faces facing away
		// from the light (verified: cull 1 showed nothing, 0/2 worked).
		if ( smCull == CT_FRONT_SIDED ) {
			smCull = CT_BACK_SIDED;
		} else if ( smCull == CT_BACK_SIDED ) {
			smCull = CT_FRONT_SIDED;
		}

		// DUDE tessellation: cast the point-light shadow from the deformed surface too
		// (same dudeTessPN + dudeTessDisplace as zfill / the lit passes). GL3 never tessellates.
		idImage *bumpImg = NULL;
		bool useDeform = false;
		const bool tess = RB_RHI_TessOrDeform( surf, tri, useDeform );
		if ( tess ) {
			RB_RHI_SetTessParms( parms );
			bumpImg = RB_RHI_TessBumpForZfill( surf, parms );	// sets bumpMatrix; tese displaces
		}

		rhi::BufferHandle vb, ib;
		int vertOfs, idxOfs;
		RB_RHI_StreamAmbient( r, tri, vb, vertOfs, ib, idxOfs );

		rhi::BufferHandle ub;
		int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

		rhi::PipelineDesc pd;
		pd.stateBits = GLS_DEPTHFUNC_LESS;
		pd.shader = prog;
		pd.vertexLayout = rhi::VL_DRAWVERT;
		pd.cullType = smCull;
		pd.tessellate = tess;
		RB_RHI_BindTessHeight( surf, tess );	// DUDE: unit 15 signed height (docs/tessellation.md)

		RB_RHI_BindUnit( 0, coverImage );
		if ( tess ) {
			RB_RHI_BindUnit( 1, bumpImg );	// shadow_sm_cube.tese displacement source (unit 0 is coverage)
		}
		r->BindPipeline( pd );

		rhi::DrawArgs da;
		memset( &da, 0, sizeof( da ) );
		da.vertexBuffer = useDeform ? tri->tessDeformVB : vb;
		da.vertexOffset = useDeform ? 0 : vertOfs;
		da.indexBuffer = useDeform ? tri->tessDeformIB : ib;
		da.firstIndex = useDeform ? 0 : ( idxOfs / (int)sizeof( glIndex_t ) );
		da.indexCount = useDeform ? tri->tessDeformIndexes : tri->numIndexes;
		da.uniformBuffer = ub;
		da.uniformOffset = uniOfs;
		da.uniformSize = sizeof( parms );
		RB_RHI_VkTextures( da );		// VK: coverage (unit 0) + bump (unit 1 when tessellating) into DrawArgs
		r->Draw( da );

		backEnd.pc.c_shadowElements++;
		rhiShadowCubeCasters++;
		if ( perforatedCaster ) {
			rhiShadowPerfCasters++;
		}
	}
}

// TEMP live-tuning knob to test the point-light cube shadow dead-zone: scales the
// radial-depth normalizer AND the cube far plane together. If cranking this up makes
// far/floor shadows reappear, `range` was clipping/clamping distant occluders.
idCVar r_shadowMapPointRangeScale( "r_shadowMapPointRangeScale", "1", CVAR_RENDERER | CVAR_FLOAT,
	"scale point-light cube shadow range (far plane + depth normalizer)", 0.1f, 32.0f );

// Skip rasterizing occluders into cube faces whose 90-degree cone can't overlap the
// camera view frustum: such a face is never sampled by a visible receiver, so its
// geometry is pure waste. The face is still cleared (seamless cube sampling can bleed
// one texel across an edge), just not drawn into. Off = render all 6 faces always.
idCVar r_shadowMapFaceCull( "r_shadowMapFaceCull", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL,
	"cull cube-shadow faces that fall outside the view frustum (big win at high resolution)" );

// True when the whole view frustum (its 8 corners) lies on the outside of one of the
// outward-pointing face-cone planes -> the face and the view are provably disjoint, so
// nothing the player can see samples this face. Conservative: never culls a face the
// view actually overlaps, so no visible shadow is lost.
static bool RB_RHI_ViewOutsideFaceCone( const idPlane facePlanes[6], const idVec3 corners[8] ) {
	for ( int p = 0; p < 6; p++ ) {
		int c = 0;
		for ( ; c < 8; c++ ) {
			if ( facePlanes[p].Distance( corners[c] ) <= 0.0f ) {
				break;			// this corner is inside -> plane doesn't separate
			}
		}
		if ( c == 8 ) {
			return true;		// all 8 corners outside this plane -> disjoint
		}
	}
	return false;
}

// distance used to normalize radial depth: the light's box reach plus any lightCenter
// offset, so every lit caster maps into [0,1].
static float RB_RHI_PointLightRange( const viewLight_t *vLight ) {
	float range = vLight->lightDef->parms.lightRadius.Length()
	            + vLight->lightDef->parms.lightCenter.Length();
	range *= r_shadowMapPointRangeScale.GetFloat();
	return range < 1.0f ? 1.0f : range;
}

/*
===================
Static point-light cube shadow cache (r_shadowMapCache)

Profiling showed cube-map *generation* dominated the frame (re-rendering ~60
static lights every frame). Most point lights and their casters never move, so
their cube never changes. Each cached light keeps its own persistent cube target;
each frame we hash the light + its caster set into a token and, if it matches the
stored one, skip the entire 6-face render and just sample the cube. VRAM is bounded
by r_shadowMapCacheMB (uncached lights fall back to the shared scratch pool and
regenerate every frame, exactly as before).
===================
*/
// Bounded by the point-light budget (r_shadowMapPointLimit maxes at 128) and by the
// backend's render-target table (GL3 MAX_RENDER_TARGETS). VRAM caps live usage lower.
#define MAX_SHADOW_CUBE_CACHE 128

struct shadowCubeCache_t {
	int						lightIndex;		// idRenderLightLocal::index; -1 = free slot
	rhi::RenderTargetHandle	rt;
	int						size;			// face resolution of rt
	unsigned long long		token;			// invalidation hash of light + casters
	unsigned long long		lightTok;		// light-pose-only sub-hash (r_shadowMapCacheDebug: classify a warm miss as light-moved vs caster-moved)
	unsigned long long		faceTok[6];		// per-face token (r_shadowMapCachePerFace): re-render only the cube faces a mover dirtied
	int						lastFrame;		// for LRU eviction
	size_t					bytes;			// VRAM cost estimate
};
static shadowCubeCache_t rhiCubeCache[MAX_SHADOW_CUBE_CACHE];
static size_t rhiCubeCacheBytes = 0;		// sum of live entries' bytes
static int rhiCubeCacheFrameNo = 0;			// bumped once per view
// r_shadowMapDebug: cache hits (render skipped) vs misses (rendered) this view.
static int rhiCubeCacheHits = 0;
static int rhiCubeCacheMiss = 0;
// r_shadowMapDebug diagnostics: lights whose caster set contains an animated (non
// DM_STATIC) occluder, and lights that fell back to the scratch pool (budget/off/full).
static int rhiCubeCacheDynamic = 0;
static int rhiCubeCacheScratch = 0;
// r_shadowMapMaxUpdates: cube re-renders spent this view (cold misses + serviced stale
// misses), and stale misses deferred to a later frame because the budget was spent.
static int rhiCubeUpdatesSpent = 0;
static int rhiCubeCacheDeferred = 0;
// r_shadowMapCacheDebug: rendered misses split by cause + LRU evictions this view. Cold =
// fresh slot (light new to the cache or evicted and returned); warm-caster = an occluder in
// the light's volume moved; warm-light = the light's own pose changed. These say which
// caching lever matters (per-face / static-dynamic split vs eviction-budget tuning).
static int rhiCubeMissCold = 0;
static int rhiCubeMissWarmCaster = 0;
static int rhiCubeMissWarmLight = 0;
static int rhiCubeEvictions = 0;
// r_shadowMapCacheDebug: point lights that used the static/dynamic split this view — a
// moving-caster light kept CACHED for its world layer instead of bypassing the cache.
static int rhiCubeCacheSplit = 0;
static int rhiCubeCacheRtMoving = 0;	// point lights served by a ray instead of any cube (r_rtMovingLights)

// depth24 cube cost: 6 faces, 4 bytes/texel (GL pads DEPTH_COMPONENT24 to 32-bit).
static size_t RB_RHI_CubeBytes( int size ) {
	return (size_t)6 * (size_t)size * (size_t)size * 4;
}

// resolved shadow-cache VRAM budget in bytes.
//   r_shadowMapCacheMB > 0 : that many MB (explicit).
//   r_shadowMapCacheMB == 0 : unlimited.
//   r_shadowMapCacheMB  < 0 : AUTO -- give the cache all the VRAM actually free, minus a safety
//                             reserve, so static cubes almost never evict (eviction + re-render is
//                             the frame-time cost we want to avoid). "Free" is measured live via VMA
//                             (VK_EXT_memory_budget): budget - (usage - ourCacheBytes) is the space
//                             left after everything NON-shadow the app has allocated, and we keep a
//                             reserve on top for transient growth (streaming, RT scene). This scales
//                             to the card (a 24 GB GPU gets a far bigger cache than a 8 GB one) instead
//                             of a blind half-of-VRAM. Falls back to the old half-VRAM on GL3 / no query.
// Note: under r_rtAllLights (with a live TLAS) no shadow maps render, so the cache is force-flushed in
// RB_RHI_ResetShadowTallies and this budget never actually governs anything - it's moot, by design.
static size_t RB_RHI_CacheBudgetBytes() {
	const int mb = r_shadowMapCacheMB.GetInteger();
	if ( mb > 0 ) {
		return (size_t)mb * 1024 * 1024;	// explicit cap
	}
	if ( mb == 0 ) {
		return (size_t)-1;					// unlimited
	}
	// --- AUTO (-1) ---
	// Recompute at most once per view (rhiCubeCacheFrameNo), with hysteresis, so the budget is
	// stable within a view and doesn't flap frame-to-frame (a flapping budget would itself cause
	// the evict/re-cache churn we're trying to kill).
	static size_t autoBudget = 0;
	static int    autoFrame  = -1;
	if ( autoFrame != rhiCubeCacheFrameNo ) {
		autoFrame = rhiCubeCacheFrameNo;
		rhi::RHI *r = rhi::GetRHI();
		uint64_t usage = 0, budget = 0;
		if ( r != NULL && r->GetDeviceLocalMemory( usage, budget ) && budget > 0 ) {
			// what everything NON-shadow currently holds (our cache's own bytes don't count against it)
			const uint64_t nonShadow = ( usage > rhiCubeCacheBytes ) ? ( usage - rhiCubeCacheBytes ) : 0;
			// safety reserve for transient growth: ~8% of VRAM, at least 768 MB
			uint64_t reserve = budget / 12;
			const uint64_t minReserve = (uint64_t)768 * 1024 * 1024;
			if ( reserve < minReserve ) { reserve = minReserve; }
			int64_t target = (int64_t)budget - (int64_t)nonShadow - (int64_t)reserve;
			const int64_t floorBytes = (int64_t)256 * 1024 * 1024;	// always allow a little cache
			if ( target < floorBytes ) { target = floorBytes; }
			// hysteresis: only move the committed budget when the target shifts by > ~5% of VRAM,
			// so a transient usage blip doesn't trigger an eviction wave.
			const uint64_t band = budget / 20;
			if ( autoBudget == 0 || (uint64_t)llabs( target - (int64_t)autoBudget ) > band ) {
				autoBudget = (size_t)target;
			}
		} else if ( autoBudget == 0 ) {
			// GL3 / query failed: the old half-of-VRAM heuristic (2 GB fallback).
			const int vram = glConfig.vidMemMB > 0 ? glConfig.vidMemMB : 2048;
			autoBudget = (size_t)( vram / 2 ) * 1024 * 1024;
		}
	}
	return autoBudget;
}

// FNV-1a over a byte range, folded into an accumulator.
static unsigned long long RB_RHI_HashBytes( unsigned long long h, const void *data, size_t n ) {
	const unsigned char *p = (const unsigned char *)data;
	for ( size_t i = 0; i < n; i++ ) {
		h = ( h ^ p[i] ) * 1099511628211ULL;
	}
	return h;
}

// One caster's identity+transform hash: entityDef index, model matrix, and geometry identity
// (pointer + index count + cache handle — a dynamic model gets a fresh ambient cache each frame,
// so this flips and invalidates it). Shared by the whole-light token and the per-face tokens so
// the two can never disagree about what a caster contributes.
static unsigned long long RB_RHI_CasterHash( const drawSurf_t *surf ) {
	unsigned long long c = 1469598103934665603ULL;			// FNV offset basis
	const idRenderEntityLocal *edef = surf->space ? surf->space->entityDef : NULL;
	const int idx = edef ? edef->index : -1;
	c = RB_RHI_HashBytes( c, &idx, sizeof( idx ) );
	c = RB_RHI_HashBytes( c, surf->space->modelMatrix, 16 * sizeof( float ) );
	const void *geo = surf->geo;
	c = RB_RHI_HashBytes( c, &geo, sizeof( geo ) );
	if ( surf->geo ) {
		c = RB_RHI_HashBytes( c, &surf->geo->numIndexes, sizeof( surf->geo->numIndexes ) );
		c = RB_RHI_HashBytes( c, &surf->geo->ambientCache, sizeof( surf->geo->ambientCache ) );
		// a GPU-skinned caster with r_gpuSkinNoUpload has no per-frame ambient-cache handle to flip,
		// so key the invalidation off gpuSkinFrame (bumped each frame it is re-skinned) — otherwise an
		// animating monster would cast a frozen cube shadow.
		if ( surf->geo->gpuSkinVB ) {
			c = RB_RHI_HashBytes( c, &surf->geo->gpuSkinFrame, sizeof( surf->geo->gpuSkinFrame ) );
		}
	}
	return c;
}

// Invalidation token: the light pose/reach plus every caster's identity and
// transform. A moving light, a swinging door (modelMatrix), or an animating monster
// (regenerated geometry / cache handle) all change the token and force a re-render;
// a fully static light hashes identically every frame and stays cached. The caster
// contributions are summed so frame-to-frame reordering of the list doesn't matter.
static unsigned long long RB_RHI_CubeToken( const viewLight_t *vLight, float range, int size,
                                            bool *outDynamic, unsigned long long *outLightTok,
                                            bool staticOnly ) {
	unsigned long long h = 1469598103934665603ULL;			// FNV offset basis
	h = RB_RHI_HashBytes( h, vLight->globalLightOrigin.ToFloatPtr(), 3 * sizeof( float ) );
	h = RB_RHI_HashBytes( h, vLight->lightDef->parms.lightCenter.ToFloatPtr(), 3 * sizeof( float ) );
	h = RB_RHI_HashBytes( h, &vLight->lightDef->parms.axis, sizeof( idMat3 ) );
	h = RB_RHI_HashBytes( h, &range, sizeof( range ) );
	h = RB_RHI_HashBytes( h, &size, sizeof( size ) );
	// perforated shadow strength bakes a dither into the map (shadow_sm*.frag), so
	// tuning the slider must re-render cached maps to show up
	const float perfStrength = r_shadowMapPerforatedStrength.GetFloat();
	h = RB_RHI_HashBytes( h, &perfStrength, sizeof( perfStrength ) );

	bool dynamic = false;
	unsigned long long casters = 0;
	for ( const drawSurf_t *surf = vLight->shadowMapCasters; surf; surf = surf->nextOnLight ) {
		// animated/particle casters (monsters, ragdolls) change every frame, so a light
		// touching one can never cache-hit; flag it so the caller keeps it on the scratch
		// path instead of wasting a persistent slot + VRAM on it.
		const bool dyn = RB_RHI_CasterIsDynamic( surf );
		if ( dyn ) {
			dynamic = true;
		}
		// staticOnly (r_shadowMapCacheSplit): fold only the static casters into the token,
		// so a mover walking past a static light doesn't invalidate its cached (static) cube.
		// The movers get their own scratch cube instead. Must match CF_STATIC in the render.
		if ( staticOnly && dyn ) {
			continue;
		}
		casters += RB_RHI_CasterHash( surf );
	}
	if ( outDynamic ) {
		*outDynamic = dynamic;
	}
	if ( outLightTok ) {
		*outLightTok = h;		// light-pose-only sub-hash (before folding in the caster set)
	}
	return h ^ casters;
}

// Per-face invalidation tokens (r_shadowMapCachePerFace). faceTok[f] = the light-pose hash XOR
// the summed per-caster hashes of the occluders that actually rasterize into face f — using the
// SAME allow + R_CullLocalBox test the render (RB_RHI_ShadowCasterChainCube) uses, so a face's
// token can never miss a caster the render would draw there (which would leave a stale shadow).
// The light pose is in every face's hash, so a moved light flips all six -> the whole cube
// re-renders. Computed only on a warm miss, to find which faces a mover dirtied.
static void RB_RHI_CubeFaceTokens( const viewLight_t *vLight, float range, int size,
                                   unsigned long long faceTok[6], bool staticOnly ) {
	unsigned long long h = 1469598103934665603ULL;
	h = RB_RHI_HashBytes( h, vLight->globalLightOrigin.ToFloatPtr(), 3 * sizeof( float ) );
	h = RB_RHI_HashBytes( h, vLight->lightDef->parms.lightCenter.ToFloatPtr(), 3 * sizeof( float ) );
	h = RB_RHI_HashBytes( h, &vLight->lightDef->parms.axis, sizeof( idMat3 ) );
	h = RB_RHI_HashBytes( h, &range, sizeof( range ) );
	h = RB_RHI_HashBytes( h, &size, sizeof( size ) );
	const float perfStrength = r_shadowMapPerforatedStrength.GetFloat();
	h = RB_RHI_HashBytes( h, &perfStrength, sizeof( perfStrength ) );

	idPlane facePlanes[6][6];
	for ( int f = 0; f < 6; f++ ) {
		float vp[16];
		RB_RHI_CubeFaceViewProj( f, range, vp );
		RB_RHI_ExtractWorldFrustum( vp, vLight->globalLightOrigin, facePlanes[f] );
	}
	unsigned long long faceCasters[6] = { 0, 0, 0, 0, 0, 0 };
	for ( const drawSurf_t *surf = vLight->shadowMapCasters; surf; surf = surf->nextOnLight ) {
		const srfTriangles_t *tri = surf->geo;
		if ( !tri || ( !tri->ambientCache && !tri->gpuSkinVB ) || !tri->numIndexes ) {
			continue;
		}
		if ( !RB_RHI_ShadowCasterAllowed( surf ) ) {
			continue;
		}
		// static/dynamic split: the per-face tokens gate the cached (static) cube, so on a
		// split light they must ignore movers — exactly as the static whole-cube token does.
		if ( staticOnly && RB_RHI_CasterIsDynamic( surf ) ) {
			continue;
		}
		const unsigned long long c = RB_RHI_CasterHash( surf );
		for ( int f = 0; f < 6; f++ ) {
			// !R_CullLocalBox == the box overlaps this face's cone == the render draws it here
			if ( !R_CullLocalBox( tri->bounds, surf->space->modelMatrix, 6, facePlanes[f] ) ) {
				faceCasters[f] += c;
			}
		}
	}
	for ( int f = 0; f < 6; f++ ) {
		faceTok[f] = h ^ faceCasters[f];
	}
}

// Pick the render target for this light's cube. On a cache hit, returns the stored
// target and sets hit=true so the caller skips the render. On a miss it returns a
// persistent target to render into (token stored for next frame), evicting least-
// recently-used entries to stay under budget. Returns 0 when caching is off or the
// light can't fit the budget -> caller uses the shared scratch pool instead.
//
// 'stale' is set true only for a warm miss: an existing slot that already holds this
// light's previous cube (a moving rigid caster changed the token). Such a light can be
// deferred by r_shadowMapMaxUpdates -- its old cube is still sampleable for a frame.
// For that case the new token is NOT committed here; the caller writes it via *pendingSlot
// once it commits to re-rendering, so a deferred light stays a miss and is retried next
// frame. A cold miss (fresh slot, no prior contents) leaves stale=false and must render.
static rhi::RenderTargetHandle RB_RHI_AcquireCubeTarget( rhi::RHI *r, int lightIndex, int size,
                                                         unsigned long long token, unsigned long long lightTok,
                                                         bool &hit, bool &stale, shadowCubeCache_t **pendingSlot ) {
	hit = false;
	stale = false;
	*pendingSlot = NULL;
	if ( !r_shadowMapCache.GetBool() ) {
		return 0;
	}

	// existing slot for this light? A slot is free when it holds no target (rt == 0);
	// this is the canonical test because the static array is zero-initialized, so a
	// fresh slot has rt == 0 and lightIndex == 0 (a valid index) — keying "free" off
	// lightIndex would never see the startup slots as available.
	shadowCubeCache_t *e = NULL;
	int firstFree = -1;
	for ( int i = 0; i < MAX_SHADOW_CUBE_CACHE; i++ ) {
		if ( rhiCubeCache[i].rt && rhiCubeCache[i].lightIndex == lightIndex ) {
			e = &rhiCubeCache[i];
			break;
		}
		if ( firstFree < 0 && rhiCubeCache[i].rt == 0 ) {
			firstFree = i;
		}
	}

	if ( e ) {
		// context lost after vid_restart -> stored handle no longer valid
		if ( r->GetRenderTargetImage( e->rt ) == 0 ) {
			rhiCubeCacheBytes -= e->bytes;
			if ( firstFree < 0 ) {
				firstFree = (int)( e - rhiCubeCache );	// this slot is now reusable
			}
			e->lightIndex = -1; e->rt = 0; e->bytes = 0; e->token = 0;
			e = NULL;
		} else if ( e->size == size ) {
			e->lastFrame = rhiCubeCacheFrameNo;	// touched this view -> not LRU-evictable
			if ( e->token == token ) {
				hit = true;						// reuse -> skip the whole render
				return e->rt;
			}
			// warm miss: the old cube is still valid to sample, so leave the stored token
			// alone and let the caller decide whether to re-render now or defer a frame
			// (r_shadowMapMaxUpdates). The caller commits the new token via *pendingSlot.
			stale = true;
			*pendingSlot = e;
			return e->rt;
		} else {
			// resolution changed (adaptive tier): drop and reallocate below
			r->DestroyRenderTarget( e->rt );
			rhiCubeCacheBytes -= e->bytes;
			e->lightIndex = -1; e->rt = 0; e->bytes = 0;
			if ( firstFree < 0 ) {
				firstFree = (int)( e - rhiCubeCache );
			}
			e = NULL;
		}
	}

	// need a new target: enforce the VRAM budget by evicting LRU entries (never one
	// touched this frame) until the newcomer fits.
	const size_t need = RB_RHI_CubeBytes( size );
	const size_t budget = RB_RHI_CacheBudgetBytes();
	while ( rhiCubeCacheBytes + need > budget ) {
		int victim = -1;
		for ( int i = 0; i < MAX_SHADOW_CUBE_CACHE; i++ ) {
			if ( rhiCubeCache[i].rt && rhiCubeCache[i].lastFrame != rhiCubeCacheFrameNo
			     && ( victim < 0 || rhiCubeCache[i].lastFrame < rhiCubeCache[victim].lastFrame ) ) {
				victim = i;
			}
		}
		if ( victim < 0 ) {
			return 0;			// nothing evictable this frame -> use scratch, no caching
		}
		r->DestroyRenderTarget( rhiCubeCache[victim].rt );
		rhiCubeCacheBytes -= rhiCubeCache[victim].bytes;
		rhiCubeCache[victim].lightIndex = -1;
		rhiCubeCache[victim].rt = 0;
		rhiCubeCache[victim].bytes = 0;
		rhiCubeEvictions++;			// r_shadowMapCacheDebug: VRAM-pressure churn signal
		if ( firstFree < 0 ) {
			firstFree = victim;
		}
	}
	if ( firstFree < 0 ) {
		return 0;				// slot table full (rare) -> scratch
	}

	rhi::RenderTargetHandle rt = r->CreateRenderTargetCube( rhi::IF_DEPTH24, size );
	if ( rt == 0 ) {
		return 0;
	}
	RB_RHI_ForgetTexBinds();	// create() disturbed unit 0's cached bind
	shadowCubeCache_t &slot = rhiCubeCache[firstFree];
	slot.lightIndex = lightIndex;
	slot.rt = rt;
	slot.size = size;
	slot.token = token;			// we are about to render this token's geometry
	slot.lightTok = lightTok;	// r_shadowMapCacheDebug classification baseline
	for ( int f = 0; f < 6; f++ ) { slot.faceTok[f] = 0; }	// per-face baseline; caller fills it after the cold render
	slot.lastFrame = rhiCubeCacheFrameNo;
	slot.bytes = need;
	rhiCubeCacheBytes += need;
	*pendingSlot = &slot;		// cold miss: hand the slot back so the caller stores per-face tokens
	return rt;					// hit stays false -> caller renders
}

// Drop every cached cube (context loss / explicit reset). Handles may already be
// dead after vid_restart, so only destroy live ones the backend still knows.
static void RB_RHI_ResetCubeCache( rhi::RHI *r ) {
	for ( int i = 0; i < MAX_SHADOW_CUBE_CACHE; i++ ) {
		if ( rhiCubeCache[i].rt && r && r->GetRenderTargetImage( rhiCubeCache[i].rt ) != 0 ) {
			r->DestroyRenderTarget( rhiCubeCache[i].rt );
		}
		rhiCubeCache[i].lightIndex = -1;
		rhiCubeCache[i].rt = 0;
		rhiCubeCache[i].bytes = 0;
		rhiCubeCache[i].token = 0;
	}
	rhiCubeCacheBytes = 0;
}

// ---- Static 2D (projected/spot) shadow-map cache ---------------------------------
// The 2D analogue of the point-cube cache above. Projected depth maps are small (a
// 1024^2 DEPTH24 map is ~4 MB), so this skips the VRAM budget/byte accounting the cube
// cache needs: a small fixed slot table keyed by light index, LRU-evicted only when the
// table is full. Same hit/skip semantics -- a static projected light re-renders its map
// only when it or a caster moves. Gated on r_shadowMapCache (shared with the cube cache).
#define MAX_SHADOW_2D_CACHE 32
struct shadow2DCache_t {
	int						lightIndex;		// idRenderLightLocal::index; -1 = free slot
	rhi::RenderTargetHandle	rt;
	int						size;			// map resolution of rt
	unsigned long long		token;			// invalidation hash of light + casters
	int						lastFrame;		// LRU key; shares rhiCubeCacheFrameNo
};
static shadow2DCache_t rhiMapCache[MAX_SHADOW_2D_CACHE];

// Pick the 2D target for this projected light. On a hit returns the stored map and sets
// hit=true so the caller skips the render; on a miss returns a persistent target to
// render into (token stored for next frame). Returns 0 (caching off, or every slot
// already used this frame) so the caller drops to the shared scratch pool, exactly as
// the pre-cache code always did.
static rhi::RenderTargetHandle RB_RHI_Acquire2DTarget( rhi::RHI *r, int lightIndex, int size,
                                                       unsigned long long token, bool &hit ) {
	hit = false;
	if ( !r_shadowMapCache.GetBool() ) {
		return 0;
	}
	shadow2DCache_t *e = NULL;
	int firstFree = -1;
	for ( int i = 0; i < MAX_SHADOW_2D_CACHE; i++ ) {
		if ( rhiMapCache[i].rt && rhiMapCache[i].lightIndex == lightIndex ) {
			e = &rhiMapCache[i];
			break;
		}
		if ( firstFree < 0 && rhiMapCache[i].rt == 0 ) {
			firstFree = i;
		}
	}
	if ( e ) {
		if ( r->GetRenderTargetImage( e->rt ) == 0 ) {		// context lost (vid_restart)
			if ( firstFree < 0 ) {
				firstFree = (int)( e - rhiMapCache );
			}
			e->lightIndex = -1; e->rt = 0; e->token = 0;
			e = NULL;
		} else if ( e->size == size ) {
			e->lastFrame = rhiCubeCacheFrameNo;
			if ( e->token == token ) {
				hit = true;						// unchanged -- skip the render
				return e->rt;
			}
			e->token = token;					// moved: re-render into the same target
			return e->rt;
		} else {
			r->DestroyRenderTarget( e->rt );	// tier/resolution changed: reallocate
			e->lightIndex = -1; e->rt = 0;
			if ( firstFree < 0 ) {
				firstFree = (int)( e - rhiMapCache );
			}
			e = NULL;
		}
	}
	// need a slot: evict the least-recently-used entry not touched this frame
	if ( firstFree < 0 ) {
		int victim = -1;
		for ( int i = 0; i < MAX_SHADOW_2D_CACHE; i++ ) {
			if ( rhiMapCache[i].rt && rhiMapCache[i].lastFrame != rhiCubeCacheFrameNo
			     && ( victim < 0 || rhiMapCache[i].lastFrame < rhiMapCache[victim].lastFrame ) ) {
				victim = i;
			}
		}
		if ( victim < 0 ) {
			return 0;						// every slot in use this frame -> scratch
		}
		r->DestroyRenderTarget( rhiMapCache[victim].rt );
		rhiMapCache[victim].lightIndex = -1;
		rhiMapCache[victim].rt = 0;
		firstFree = victim;
	}
	rhi::RenderTargetHandle rt = r->CreateRenderTarget( rhi::IF_DEPTH24, size, size );
	if ( rt == 0 ) {
		return 0;
	}
	RB_RHI_ForgetTexBinds();				// create() disturbed unit 0's cached bind
	shadow2DCache_t &slot = rhiMapCache[firstFree];
	slot.lightIndex = lightIndex;
	slot.rt = rt;
	slot.size = size;
	slot.token = token;
	slot.lastFrame = rhiCubeCacheFrameNo;
	return rt;							// hit stays false -> caller renders
}

// Drop every cached 2D map (context loss / cache toggled off / world teardown).
static void RB_RHI_Reset2DCache( rhi::RHI *r ) {
	for ( int i = 0; i < MAX_SHADOW_2D_CACHE; i++ ) {
		if ( rhiMapCache[i].rt && r && r->GetRenderTargetImage( rhiMapCache[i].rt ) != 0 ) {
			r->DestroyRenderTarget( rhiMapCache[i].rt );
		}
		rhiMapCache[i].lightIndex = -1;
		rhiMapCache[i].rt = 0;
		rhiMapCache[i].token = 0;
	}
}

/*
===================
RB_RHI_FreeShadowCubeCache

Public reset, called from idRenderWorldLocal::FreeDefs. A map change or world
teardown frees every light def, so any cached cube keyed to those light indices is
stale (and the next map will reuse those indices). Drop the whole cache here so a
new level can't sample a previous level's cube, and reclaim the VRAM immediately
instead of waiting for LRU eviction. No-op when the backend isn't up or the cache
is already empty. The GL resource deletes run on the main thread between frames,
where the context is current — the same place image purges and R_FreeDerivedData
already free GL objects.
===================
*/
static void RB_RHI_ResetLightBudgetHyst();		// defined with the hysteresis helpers below
static void RB_RHI_ResetRtMovingTrack();		// defined with the moving-light tracker below

// Drop every CPU-side render-target handle the world backend caches across frames
// (shadow map/cube + adaptive pools, SSAO + its temporal history, SSR + its history,
// and the normal G-buffer). These handles are indices into the backend's render-target
// table; after a vid_restart that table is torn down and rebuilt from scratch, so a
// surviving handle would alias a freshly-allocated, unrelated target and paint garbage
// (or reuse a dead GL name from the destroyed context). The matching GPU objects are
// freed by GL3Backend::Shutdown in one sweep over the whole table — this only forgets
// the handles + their cached sizes/validity so each buffer is lazily recreated against
// the new context. Driven by RB_RHI_Shutdown (RhiBackend.cpp).
// DUDE sub-native FSR2: the chromatic-aberration pass's own display-size capture of the upscaled
// composite (RB_RHI_ChromaticAberration), so _currentRender stays at the render size all frame
static rhi::ImageHandle	rhiChromaCompositeCap = 0;
static int				rhiChromaCapW = 0, rhiChromaCapH = 0;

void RB_RHI_ResetWorldTargets( void ) {
	rhiChromaCompositeCap = 0;	rhiChromaCapW = rhiChromaCapH = 0;		// the backend freed the image
	rhiShadowMap = 0;			rhiShadowMapSize = 0;
	rhiShadowCube = 0;			rhiShadowCubeSize = 0;	rhiShadowCubeDyn = 0;	rhiShadowCubeDynScale = 1.0f;
	for ( int i = 0; i < SHADOW_NTIERS; i++ ) {
		rhiShadowMapPool[i].rt  = 0;	rhiShadowMapPool[i].size  = 0;
		rhiShadowCubePool[i].rt = 0;	rhiShadowCubePool[i].size = 0;
	}

	rhiSsaoRT = rhiSsaoBlurRT = rhiSsaoResultRT = 0;
	rhiSsaoW = rhiSsaoH = rhiSsaoViewW = rhiSsaoViewH = 0;
	rhiSsaoAppliedThisView = false;
	rhiSsaoDepthMipRT = 0;		rhiSsaoDepthMipW = rhiSsaoDepthMipH = rhiSsaoDepthMipLevels = 0;
	rhiSsaoHistRT[0] = rhiSsaoHistRT[1] = 0;
	rhiSsaoHistIdx = 0;			rhiSsaoHistW = rhiSsaoHistH = 0;
	rhiSsaoHistValid = false;	RB_RHI_TemporalResetCam();	RB_RHI_MotionResetCache();
	rhiRtaoRayRT = 0;			rhiRtaoW = rhiRtaoH = 0;
	rhiRtaoVelRT = 0;			rhiRtaoVelW = rhiRtaoVelH = 0;
	rhiRtBlurRayRT = rhiRtBlurPingRT = rhiRtBlurMaskRT = rhiRtBlurTileRT[0] = rhiRtBlurTileRT[1] = 0;
	rhiRtBlurW = rhiRtBlurH = 0;	rhiRtBlurThisView = false;
	memset( rhiRtBlurSlots, 0, sizeof( rhiRtBlurSlots ) );		// handles died with the context
	rhiRtaoRanThisView = false;
	rhiRtaoResolveRT = 0;		rhiRtaoResolveW = rhiRtaoResolveH = 0;
	rhiRtaoDenoisedThisView = false;	rhiRtaoPrevValid = false;

	rhiSsrRT = 0;				rhiSsrW = rhiSsrH = 0;
	rhiSsrDepthMinRT = 0;		rhiSsrDepthMinW = rhiSsrDepthMinH = rhiSsrDepthMinLevels = 0;
	rhiSsrColorMipRT = 0;		rhiSsrColorMipW = rhiSsrColorMipH = rhiSsrColorMipLevels = 0;
	rhiSsrHistRT[0] = rhiSsrHistRT[1] = 0;
	rhiSsrHistIdx = 0;			rhiSsrHistW = rhiSsrHistH = 0;
	rhiSsrHistValid = false;
	rhiRtReflRT = 0;			rhiRtReflW = rhiRtReflH = 0;
	rhiVolRT = 0;	rhiVolBlurRT = 0;	rhiVolW = rhiVolH = 0;
	rhiVolUpRT = 0;	rhiVolHistRT[0] = rhiVolHistRT[1] = 0;	rhiVolFullW = rhiVolFullH = 0;	rhiVolHistValid = false;
	s_rtPerfBoxBuf = 0;			s_rtPerfBoxAddr = 0;		// perforated-caster boxes: same
	s_rtReflLightBuf = 0;		s_rtReflLightAddr = 0;		// RR16: handle is stale after a lost context; recreated lazily
	rhiRtReflHistRT[0] = rhiRtReflHistRT[1] = 0;
	rhiRtReflHistIdx = 0;		rhiRtReflHistW = rhiRtReflHistH = 0;
	rhiRtReflHistValid = false;

	rhiNormalRT = 0;			rhiNormalW = rhiNormalH = 0;
	rhiNormalReadyThisView = false;	rhiNormalMrt = false;	rhiNormalVel = false;	rhiNormalAlbedo = false;
	rhiNormalMrtThisView = false;	rhiNormalVelThisView = false;	rhiNormalAlbedoThisView = false;

	rhiBerserkTrailRT[0] = rhiBerserkTrailRT[1] = 0;
	rhiBerserkIdx = 0;			rhiBerserkW = rhiBerserkH = 0;
	rhiBerserkValid = false;	rhiBerserkLastTick = -100000;

	// forget the hell-time ping-pong slots too, so a vid_restart can't leave a stale handle
	// aliasing a reclaimed render-target slot (EnsureHelltimeTrail then reallocates cleanly).
	rhiHelltimeTrailRT[0] = rhiHelltimeTrailRT[1] = 0;
	rhiHelltimeIdx = 0;			rhiHelltimeW = rhiHelltimeH = 0;
	rhiHelltimeValid = false;	rhiHelltimeLastTick = -100000;
}

void RB_RHI_FreeShadowCubeCache() {
	if ( !glConfig.isInitialized ) {
		return;
	}
	RB_RHI_ResetCubeCache( rhi::GetRHI() );
	RB_RHI_Reset2DCache( rhi::GetRHI() );
	// light indices are reused by the next map; drop stale incumbency so a new level's
	// lights don't inherit a phantom budget bonus from the old one.
	RB_RHI_ResetLightBudgetHyst();
	RB_RHI_ResetRtMovingTrack();	// same reuse hazard: stale pose baselines would misread as movement
}

// Render a point light's occluder depth into `cubeTarget`, one 90-degree face at a time.
// renderFace[] gates which faces to (re)draw (per-face invalidation, lever A); faceCull skips
// faces whose cone can't reach the camera — valid only for a throwaway cube regenerated this
// frame, never a cached cube sampled from future angles. filter picks the static/dynamic layer
// (lever B) or CF_ALL for the whole caster set.
static void RB_RHI_RenderCubeFaces( rhi::RHI *r, viewLight_t *vLight, rhi::ShaderHandle prog,
                                    rhi::RenderTargetHandle cubeTarget, float range,
                                    const bool renderFace[6], bool faceCull, casterFilter_t filter ) {
	const idVec3 &L = vLight->globalLightOrigin;

	rhi::ClearArgs clear;
	memset( &clear, 0, sizeof( clear ) );
	clear.depth = true;

	// whole-face view-frustum cull: fetch the camera frustum corners once, then skip
	// the (expensive) occluder rasterization on any face whose cone can't reach the
	// view. The face is still cleared to far depth so seamless cube sampling reads it
	// as "lit" at shared edges — only the geometry, the dominant cost, is skipped.
	idVec3 viewCorners[8];
	const bool doCull = faceCull && r_shadowMapFaceCull.GetBool() && backEnd.viewDef;
	if ( doCull ) {
		backEnd.viewDef->viewFrustum.ToPoints( viewCorners );
	}

	for ( int face = 0; face < 6; face++ ) {
		if ( !renderFace[face] ) {
			continue;			// per-face invalidation: unchanged face keeps its cached depth
		}
		float vp[16];
		RB_RHI_CubeFaceViewProj( face, range, vp );
		idPlane planes[6];
		RB_RHI_ExtractWorldFrustum( vp, L, planes );

		const bool cullFace = doCull && RB_RHI_ViewOutsideFaceCone( planes, viewCorners );

		r->BeginCubeFacePass( cubeTarget, face, &clear );
		if ( !cullFace ) {
			// single complete occluder set (full ambientTris, view-independent); see the
			// shadowMapCasters comment in idInteraction::AddActiveInteraction
			RB_RHI_ShadowCasterChainCube( r, vLight->shadowMapCasters, prog, vp, planes, L, range, filter );
			rhiShadowCubeFaces++;
		} else {
			rhiShadowCubeFacesCulled++;
		}
		r->EndPass();
	}
}

/*
===================
RB_RHI_ShadowMapPassCube

Renders a point light's occluder depth into a cube target, one 90-degree face at a time
with per-face culling. With r_shadowMapCacheSplit a moving-caster light renders its world
occluders into the cached cube and its movers into a per-frame scratch cube (sampled as
min of the two). Returns false (→ stencil fallback) if the cube target can't be created.
===================
*/
static bool RB_RHI_ShadowMapPassCube( rhi::RHI *r, viewLight_t *vLight, rhi::ShaderHandle prog, float range ) {
	// cap the upper bound at what the GL context actually supports for cube maps, so
	// an 8192 request degrades gracefully on cards reporting less (GL 3.3 only
	// guarantees 1024). maxCubeMapSize is queried once at init.
	const int cubeHi = idMath::ClampInt( 128, 4096, glConfig.maxCubeMapSize );
	const int base = idMath::ClampInt( 128, cubeHi, r_shadowMapPointSize.GetInteger() );
	// adaptive per-light resolution from the light radius, pooled per tier (see the
	// 2D pass). rhiShadowCube/Size track the selected slot for the texel size and the
	// caller's GetRenderTargetImage sample.
	const float radius = vLight->lightDef->parms.lightRadius.Length();
	const int tier = RB_RHI_ShadowTier( radius );
	const int size = RB_RHI_TierSize( base, tier, 128, cubeHi );

	// static cache lookup: on a hit the stored cube is unchanged, so we sample it and
	// skip the whole render. On a miss AcquireCubeTarget hands back a persistent target
	// (token stored); if caching is off or the light won't fit the budget it returns 0
	// and we fall back to the shared scratch pool that regenerates every frame.
	const int lightIndex = vLight->lightDef->index;
	// static/dynamic split (r_shadowMapCacheSplit, lever B): a light with a moving/animated
	// caster normally bypasses the cache and regenerates the whole cube every frame. With the
	// split on we cache the STATIC (world) casters and re-render only the movers into a small
	// per-frame cube, sampling min(static, dynamic) in the interaction pass. The token then
	// folds static casters only, so a monster walking past a static light no longer invalidates
	// its cached cube.
	const bool split = r_shadowMapCacheSplit.GetBool();
	bool dynamic = false;
	unsigned long long lightTok = 0;
	const unsigned long long token = RB_RHI_CubeToken( vLight, range, size, &dynamic, &lightTok, /*staticOnly=*/split );
	bool hit = false;
	bool stale = false;
	shadowCubeCache_t *pendingSlot = NULL;
	rhi::RenderTargetHandle target = 0;
	bool splitActive = false;		// this light rendered a cached static + scratch dynamic pair
	if ( dynamic && split ) {
		// try to cache the static layer (static-only token). Hit / warm-miss / eviction all
		// behave exactly like a static light; only the render below is filtered to CF_STATIC.
		target = RB_RHI_AcquireCubeTarget( r, lightIndex, size, token, lightTok, hit, stale, &pendingSlot );
		splitActive = ( target != 0 );
	}
	if ( splitActive ) {
		rhiCubeCacheSplit++;
	} else if ( dynamic ) {
		// split off, or the static layer didn't fit the budget: legacy whole-cube scratch,
		// which costs no persistent VRAM slot and still gets per-face view-frustum culling.
		rhiCubeCacheDynamic++;
	} else {
		// fully static light: unchanged cached path.
		target = RB_RHI_AcquireCubeTarget( r, lightIndex, size, token, lightTok, hit, stale, &pendingSlot );
	}
	const bool cached = ( target != 0 );
	if ( !cached ) {
		if ( !dynamic ) {
			rhiCubeCacheScratch++;		// static light that didn't fit the VRAM budget
		}
		target = RB_RHI_ShadowPoolTarget( r, true, tier, size );
	}
	rhiShadowCube = target;
	rhiShadowCubeSize = size;
	rhiShadowCubeDyn = 0;			// set below only if this light renders a dynamic layer
	rhiShadowCubeDynScale = 1.0f;
	if ( target == 0 ) {
		return false;
	}

	// Does the primary (static/combined) cube need a (re)render this frame? A hit or a
	// deferred warm miss reuses last frame's cube; a split light still re-renders its
	// dynamic layer below regardless.
	bool renderPrimary = true;
	if ( hit ) {
		rhiCubeCacheHits++;
		renderPrimary = false;			// unchanged since last frame
	} else if ( cached ) {
		// Update budget (r_shadowMapMaxUpdates): a warm miss can be deferred — its old cube
		// is still on the slot, so sample that this view and retry next frame. Cold misses
		// (stale == false) have no prior contents and must render regardless of budget.
		const int maxUpdates = r_shadowMapMaxUpdates.GetInteger();
		if ( stale && maxUpdates > 0 && rhiCubeUpdatesSpent >= maxUpdates ) {
			rhiCubeCacheDeferred++;
			renderPrimary = false;		// reuse last frame's cube; token left stale -> retried next view
		} else {
			rhiCubeCacheMiss++;
			// r_shadowMapCacheDebug: attribute this re-render. Cold = fresh slot (light new to the
			// cache or evicted and returned). Warm-light = the light-pose sub-hash differs from the
			// stored one (the light moved). Warm-caster = pose held, so an occluder in the light's
			// volume moved. Classify BEFORE the commit below overwrites the stored baseline.
			if ( !stale ) {
				rhiCubeMissCold++;
			} else if ( pendingSlot && lightTok != pendingSlot->lightTok ) {
				rhiCubeMissWarmLight++;
			} else {
				rhiCubeMissWarmCaster++;
			}
			rhiCubeUpdatesSpent++;
			if ( pendingSlot ) {
				pendingSlot->token = token;		// committing to the re-render: adopt the new token
				pendingSlot->lightTok = lightTok;
			}
		}
	}

	// Primary layer: the cached static cube on a split light (CF_STATIC), else the whole
	// caster set (CF_ALL). Skipped entirely on a hit / deferred warm miss.
	if ( renderPrimary ) {
		// Per-face invalidation (r_shadowMapCachePerFace, lever A): on a WARM miss re-render only the
		// faces a mover actually dirtied; clean faces keep their cached depth. A cold miss (fresh slot)
		// renders all six and stores the baseline; scratch (uncached) is untouched (renderFace all
		// true). On a split light the per-face tokens fold static casters only, matching the render.
		bool renderFace[6] = { true, true, true, true, true, true };
		if ( cached && pendingSlot ) {
			if ( r_shadowMapCachePerFace.GetBool() ) {
				unsigned long long faceTok[6];
				RB_RHI_CubeFaceTokens( vLight, range, size, faceTok, /*staticOnly=*/split );
				for ( int f = 0; f < 6; f++ ) {
					renderFace[f] = stale ? ( faceTok[f] != pendingSlot->faceTok[f] ) : true;
					pendingSlot->faceTok[f] = faceTok[f];	// adopt the new per-face baseline
				}
			} else {
				// per-face OFF: this miss re-renders all six faces but doesn't recompute the per-face
				// tokens, so the stored baseline goes stale. Zero it so a later r_shadowMapCachePerFace
				// 1 can't trust a frozen baseline and skip a face that has since changed (review D1) —
				// the next per-face-on warm miss then re-renders all six once and re-baselines.
				for ( int f = 0; f < 6; f++ ) {
					pendingSlot->faceTok[f] = 0;
				}
			}
		}
		// a cached cube is sampled from arbitrary future camera angles, so it must hold all six
		// faces (no view-frustum face cull); a scratch cube is regenerated this frame, so faces
		// outside the view are safe to skip.
		RB_RHI_RenderCubeFaces( r, vLight, prog, target, range, renderFace, /*faceCull=*/!cached,
		                        splitActive ? CF_STATIC : CF_ALL );
	}

	// Dynamic layer (split only): the movers, re-rendered every frame into a scratch cube.
	// The interaction pass samples min(static, dynamic), so a mover shadows through the
	// cached world cube without ever invalidating it. All six faces, view-frustum culled
	// (this cube lives one frame). Failing to get a scratch target just drops the dynamic
	// shadow this frame (the static shadow still shows); u_shadowCubeDyn stays unbound.
	if ( splitActive ) {
		// r_shadowMapSplitDynDrop: the dynamic cube re-renders every frame (it can never
		// cache), so its resolution is a pure per-frame fill cost — render it N tiers
		// below the static cube. Tier-canonical sizes keep the pool slots stable (no
		// recreate churn); floors at pool tier 0. The shader widens the dynamic PCF disc
		// and depth bias by the resulting ratio, so the movers' shadow reads as a
		// proportionally softer version of the same filter, not a blockier one.
		int tierDyn = tier;
		int sizeDyn = size;
		const int drop = idMath::ClampInt( 0, 2, r_shadowMapSplitDynDrop.GetInteger() );
		if ( drop > 0 && tier > 0 ) {
			tierDyn = ( tier - drop > 0 ) ? ( tier - drop ) : 0;
			sizeDyn = RB_RHI_TierSize( base, tierDyn, 128, cubeHi );
		}
		const rhi::RenderTargetHandle dynTarget = RB_RHI_ShadowPoolTarget( r, true, tierDyn, sizeDyn );
		if ( dynTarget != 0 && dynTarget != target ) {
			const bool allFaces[6] = { true, true, true, true, true, true };
			RB_RHI_RenderCubeFaces( r, vLight, prog, dynTarget, range, allFaces, /*faceCull=*/true, CF_DYNAMIC );
			rhiShadowCubeDyn = dynTarget;
			rhiShadowCubeDynScale = ( sizeDyn > 0 ) ? ( (float)size / (float)sizeDyn ) : 1.0f;
		}
	}
	return true;
}

// A point light may cube-shadow if it casts shadows at all and has interaction
// geometry — same rule as the projected path, minus the projected-vs-point test.
static bool RB_RHI_PointLightShadowEligible( const viewLight_t *vLight ) {
	if ( !vLight->lightDef || !vLight->lightDef->parms.pointLight || vLight->lightDef->parms.parallel ) {
		return false;
	}
	if ( !( vLight->localInteractions || vLight->globalInteractions ) ) {
		return false;
	}
	if ( vLight->lightDef->parms.noShadows || !vLight->lightShader->LightCastsShadows() ) {
		return false;
	}
	return true;
}

// on-screen importance proxy: the light's scissor-rect area (bigger/closer lights
// score higher). Used to spend the point-light shadow budget on what matters most.
static int RB_RHI_LightScore( const viewLight_t *vLight ) {
	const idScreenRect &s = vLight->scissorRect;
	const int w = s.x2 - s.x1 + 1;
	const int h = s.y2 - s.y1 + 1;
	return ( w > 0 && h > 0 ) ? w * h : 0;
}

// ---- Budget hysteresis (r_shadowMapBudgetHysteresis) -----------------------------
// The raw score above is the instantaneous on-screen size, so two similarly-sized
// lights (or one hovering at the frustum edge) can trade places at the rank-limit
// boundary frame to frame — each swap regenerates a cube and pops a shadow on/off. To
// stabilize the shadowed set we remember which lights recently held a cube and give
// those incumbents a small score bonus, so a marginally bigger newcomer can't displace
// them. Keyed by light index (viewLight_t is rebuilt every frame; the state must
// persist), a tiny linear-probe table sized well above any plausible simultaneous
// point-light count. Reset with the cube cache on world teardown.
#define MAX_LIGHT_BUDGET_HYST 256
// How long (in views) an incumbent keeps its bonus after last being budgeted. Only needs
// to bridge brief drop-outs (an occluder flicks in front for a frame); a light that
// leaves the view entirely stops being evaluated and ages out on its own.
#define SHADOW_BUDGET_HYST_WINDOW 120
struct lightBudgetHyst_t {
	int		lightIndex;		// idRenderLightLocal::index
	int		lastFrame;		// last view this light was granted a cube; 0 = empty slot
	bool	incumbent;		// snapshot (taken at view start) of "recently budgeted"
};
static lightBudgetHyst_t rhiLightBudgetHyst[MAX_LIGHT_BUDGET_HYST];

// True if the light was a recent incumbent as of the start of this view. Reads the
// snapshot bit, not lastFrame, so grants made during this view's light loop can't change
// the answer mid-loop — the in-budget decision stays independent of the order lights are
// evaluated in (grants only affect the NEXT view's snapshot). See RB_RHI_SnapshotLightBudgetHyst.
static bool RB_RHI_LightRecentlyBudgeted( int lightIndex ) {
	for ( int i = 0; i < MAX_LIGHT_BUDGET_HYST; i++ ) {
		if ( rhiLightBudgetHyst[i].lastFrame != 0 && rhiLightBudgetHyst[i].lightIndex == lightIndex ) {
			return rhiLightBudgetHyst[i].incumbent;
		}
	}
	return false;
}

// Fold each entry's grant timestamp into an order-stable incumbency bit for this view.
// Called once, before the light loop, right after rhiCubeCacheFrameNo is bumped: at that
// point lastFrame still reflects grants only through PREVIOUS views (this view hasn't
// marked anything yet), so an entry granted 1..window views ago reads as an incumbent.
static void RB_RHI_SnapshotLightBudgetHyst() {
	for ( int i = 0; i < MAX_LIGHT_BUDGET_HYST; i++ ) {
		const int lf = rhiLightBudgetHyst[i].lastFrame;
		const int age = rhiCubeCacheFrameNo - lf;
		rhiLightBudgetHyst[i].incumbent = ( lf != 0 && age >= 1 && age <= SHADOW_BUDGET_HYST_WINDOW );
	}
}

// Record that this light got a cube this view (refreshes its grant timestamp). Does not
// touch the snapshot bit, so it has no effect until the next view's snapshot.
static void RB_RHI_MarkLightBudgeted( int lightIndex ) {
	int free = -1, oldest = -1;
	for ( int i = 0; i < MAX_LIGHT_BUDGET_HYST; i++ ) {
		if ( rhiLightBudgetHyst[i].lastFrame != 0 && rhiLightBudgetHyst[i].lightIndex == lightIndex ) {
			rhiLightBudgetHyst[i].lastFrame = rhiCubeCacheFrameNo;
			return;
		}
		if ( rhiLightBudgetHyst[i].lastFrame == 0 ) {
			if ( free < 0 ) { free = i; }
		} else if ( oldest < 0 || rhiLightBudgetHyst[i].lastFrame < rhiLightBudgetHyst[oldest].lastFrame ) {
			oldest = i;			// fall back to evicting the stalest incumbent if the table is full
		}
	}
	const int slot = ( free >= 0 ) ? free : oldest;
	if ( slot < 0 ) {
		return;					// table full of same-frame entries (>256 point lights): skip, harmless
	}
	rhiLightBudgetHyst[slot].lightIndex = lightIndex;
	rhiLightBudgetHyst[slot].lastFrame = rhiCubeCacheFrameNo;
	rhiLightBudgetHyst[slot].incumbent = false;		// new grant this view: no bonus until next snapshot
}

static void RB_RHI_ResetLightBudgetHyst() {
	memset( rhiLightBudgetHyst, 0, sizeof( rhiLightBudgetHyst ) );
}

// ---- Moving-light detection (r_rtMovingLights) -----------------------------------
// A point light whose POSE (origin/center/axis — intensity and colour deliberately
// excluded, flicker is not movement) changed recently is "moving": the cube path can
// never cache it (every pose change re-renders all its faces), so it routes to the
// ray-query path instead, where a moving light costs the same as a still one. The
// hysteresis window keeps a briefly-paused light on the ray path so the RT-hard vs
// PCF-soft look doesn't flip-flop frame to frame; a light that genuinely stops
// re-caches as a cube after the window (one cold render, then cache hits).
// Same table idiom as the budget hysteresis above: keyed by persistent lightIndex,
// linear probe, stalest-entry eviction, reset with the cache on world teardown.
#define MAX_RT_MOVING_TRACK 256
#define RT_MOVING_HYST_VIEWS 30
struct rtMovingTrack_t {
	int					lightIndex;
	int					lastSeenFrame;		// 0 = empty slot
	int					lastMoveFrame;		// last view the pose hash changed
	unsigned long long	poseTok;
};
static rtMovingTrack_t rhiRtMovingTrack[MAX_RT_MOVING_TRACK];

static void RB_RHI_ResetRtMovingTrack() {
	memset( rhiRtMovingTrack, 0, sizeof( rhiRtMovingTrack ) );
}

static bool RB_RHI_LightIsMoving( const viewLight_t *vLight ) {
	// pose-only hash: the same fields the cube token's light sub-hash folds, minus the
	// settings (range/size/dither) that would masquerade as movement when tuned live
	unsigned long long tok = 1469598103934665603ULL;
	tok = RB_RHI_HashBytes( tok, vLight->globalLightOrigin.ToFloatPtr(), 3 * sizeof( float ) );
	tok = RB_RHI_HashBytes( tok, vLight->lightDef->parms.lightCenter.ToFloatPtr(), 3 * sizeof( float ) );
	tok = RB_RHI_HashBytes( tok, &vLight->lightDef->parms.axis, sizeof( idMat3 ) );

	const int lightIndex = vLight->lightDef->index;
	int free = -1, oldest = -1;
	for ( int i = 0; i < MAX_RT_MOVING_TRACK; i++ ) {
		rtMovingTrack_t &t = rhiRtMovingTrack[i];
		if ( t.lastSeenFrame != 0 && t.lightIndex == lightIndex ) {
			if ( t.poseTok != tok ) {
				t.poseTok = tok;
				t.lastMoveFrame = rhiCubeCacheFrameNo;
			}
			t.lastSeenFrame = rhiCubeCacheFrameNo;
			return ( rhiCubeCacheFrameNo - t.lastMoveFrame ) <= RT_MOVING_HYST_VIEWS;
		}
		if ( t.lastSeenFrame == 0 ) {
			if ( free < 0 ) { free = i; }
		} else if ( oldest < 0 || t.lastSeenFrame < rhiRtMovingTrack[oldest].lastSeenFrame ) {
			oldest = i;
		}
	}
	// first sight: baseline the pose and report "not moving" — a brand-new light gets
	// one cold cube render, and flips to the ray path the first view its pose changes
	const int slot = ( free >= 0 ) ? free : oldest;
	if ( slot >= 0 ) {
		rhiRtMovingTrack[slot].lightIndex = lightIndex;
		rhiRtMovingTrack[slot].lastSeenFrame = rhiCubeCacheFrameNo;
		// impossibly old, NOT 0: frame counters start near 0 after a map load / cache
		// reset, so a 0 baseline would read as "moved recently" for the first window
		rhiRtMovingTrack[slot].lastMoveFrame = -( RT_MOVING_HYST_VIEWS + 1 );
		rhiRtMovingTrack[slot].poseTok = tok;
	}
	return false;
}

// Effective ranking score: raw on-screen size, scaled up for a recent incumbent so it
// resists being bumped out of the r_shadowMapPointLimit set (see hysteresis note above).
static double RB_RHI_LightEffectiveScore( const viewLight_t *vLight ) {
	double s = (double)RB_RHI_LightScore( vLight );
	const int hyst = r_shadowMapBudgetHysteresis.GetInteger();
	if ( hyst > 0 && vLight->lightDef && RB_RHI_LightRecentlyBudgeted( vLight->lightDef->index ) ) {
		s *= 1.0 + (double)hyst / 100.0;
	}
	return s;
}

// Budget: cube-shadow only the r_shadowMapPointLimit highest-scoring point lights this
// view; while r_shadowMapping is on the rest render unshadowed (stencil is off entirely),
// so this bounds the cube-map cost. 0 = all point lights. O(lights^2) but the light count
// per view is small. Ranks by the hysteresis-adjusted score so the set stays stable frame
// to frame; a light granted budget is marked so it earns the incumbency bonus next view.
static bool RB_RHI_PointLightInBudget( const viewDef_t *viewDef, const viewLight_t *self ) {
	const int limit = r_shadowMapPointLimit.GetInteger();
	if ( limit <= 0 ) {
		return true;			// unlimited: score/incumbency irrelevant, nothing to stabilize
	}
	const double myScore = RB_RHI_LightEffectiveScore( self );
	int better = 0;
	for ( viewLight_t *vl = viewDef->viewLights; vl; vl = vl->next ) {
		if ( vl == self || !RB_RHI_PointLightShadowEligible( vl ) ) {
			continue;
		}
		const double sc = RB_RHI_LightEffectiveScore( vl );
		// strict score, ties broken by address so the set is stable and disjoint
		if ( sc > myScore || ( sc == myScore && vl < self ) ) {
			better++;
		}
	}
	const bool inBudget = better < limit;
	if ( inBudget && self->lightDef ) {
		RB_RHI_MarkLightBudgeted( self->lightDef->index );
	}
	return inBudget;
}

static bool RB_RHI_EnsureNormalTarget( rhi::RHI *r, int w, int h, bool wantMrt, bool wantVel, bool wantAlbedo ) {
	if ( rhiNormalRT && r->GetRenderTargetImage( rhiNormalRT ) == 0 ) {
		rhiNormalRT = 0;					// lost context (vid_restart)
		rhiNormalW = rhiNormalH = 0;
	}
	if ( rhiNormalRT && rhiNormalW == w && rhiNormalH == h && rhiNormalMrt == wantMrt && rhiNormalVel == wantVel && rhiNormalAlbedo == wantAlbedo ) {
		return true;
	}
	if ( rhiNormalRT ) { r->DestroyRenderTarget( rhiNormalRT ); rhiNormalRT = 0; }
	// wantMrt adds the SSR roughness/metalness attachment (docs/ssr.md); wantVel adds the RG16F
	// velocity attachment (R1/A2) as a 3rd MRT. colorCount 3 selects A0's mixed-format layout
	// [RGBA8, RGBA8, RG16F]; else the 1/2-attachment RGBA8 layout, byte-identical to before.
	// wantAlbedo (docs/rtx-rtgi.md H6c) forces the fixed 4-attachment layout normal/mat/vel/albedo (RGBA8,
	// RGBA8, RG16F, RGBA8): the caller also forced wantMrt + wantVel, so att1/att2 exist under att3.
	const int colorCount = wantAlbedo ? 4 : ( wantVel ? 3 : ( wantMrt ? 2 : 1 ) );
	rhiNormalRT = r->CreateRenderTargetColorDepth( rhi::IF_RGBA8, w, h, colorCount );
	if ( !rhiNormalRT ) {
		rhiNormalW = rhiNormalH = 0;
		return false;
	}
	rhiNormalW = w;
	rhiNormalH = h;
	rhiNormalMrt = wantMrt;
	rhiNormalVel = wantVel;
	rhiNormalAlbedo = wantAlbedo;
	return true;
}

/*
===================
RB_RHI_NormalPrepass

Renders opaque geometry into the normal G-buffer, writing bump-mapped view-space normals
for SSAO to sample instead of reconstructing flat normals from depth (docs/ssao-gtao.md,
Option B). With r_ssr the same pass also fills a second attachment with the surface's
resolved PBR roughness/metalness (docs/ssr.md). One extra opaque geometry pass; only runs
when SSAO or SSR wants it, for the fullscreen primary view. Simplified vs the depth
prepass: no subview down-modulate / clip planes (primary-view only). Perforated surfaces
(grates, cables, foliage) punch their diffuse alpha out of the normal buffer, matching
the coverage the depth prepass seals.
===================
*/
// Returns true when it took the r_ssaoMergeNormal path — i.e. the gbuffer pass sealed the
// *scene* depth (FrameDepthImage) as well as writing the normal, so the caller must SKIP the
// standalone zfill depth prepass and capture _currentDepth from this pass instead. Returns
// false for the standalone-normal-target path (or when it does nothing), where zfill still
// seals depth as usual.
static bool RB_RHI_NormalPrepass( rhi::RHI *r, const viewDef_t *viewDef ) {
	if ( !R_BackendSupportsEnhancements() ) {
		return false;
	}
	// SSAO wants the buffer when it feeds the horizon search, or when it's being inspected
	// (r_ssaoDebug 3) even if SSAO reconstructs normals from depth — so the debug view
	// always has data. SSR needs it unconditionally (normals to reflect about + the
	// rough/metal attachment).
	// M7: SSR now runs on Vulkan too (RB_RHI_ScreenSpaceReflections), so build the
	// MRT rough/metal attachment on both backends when r_ssr wants it
	// RTGI H6c (docs/rtx-rtgi.md): when r_rtgi is on, the fullscreen GI apply needs a per-pixel receiver
	// albedo, produced as a 4th attachment on the normal prepass. It forces the SSR + velocity attachments
	// (so the layout is fixed normal/mat/vel/albedo) and the STANDALONE path (the merged pass keeps its
	// 3-attachment layout untouched). On the RT tier ssr + velocity are already on, so this only adds att3.
	const bool wantAlbedo = r_rtgi.GetBool() && rhi::GetActiveBackendType() == rhi::BT_VULKAN;
	// RT reflections read the normal + rough/metal MRT this pass fills. Since 2026-09-23 they run decoupled from
	// r_ssr (RT owns the reflective pixels), so gate the prepass on them too — else RT-only mode (r_ssr off) has
	// no G-buffer to reflect about.
	const bool rtReflWants = r_rtReflections.GetBool()
		&& rhi::GetActiveBackendType() == rhi::BT_VULKAN && r->SupportsRayQuery();
	const bool ssrWants = r_ssr.GetBool() || wantAlbedo || rtReflWants;
	const bool ssaoWants = r_ssao.GetBool()
		&& ( r_ssaoNormalBuffer.GetBool() || r_ssaoDebug.GetInteger() == 3 );
	// R1/A2: per-object motion vectors are VK-only (GL3 has no float-colour RT). When on they
	// FORCE the normal prepass to run (for the velocity MRT) even if SSAO/SSR want nothing. The
	// depth-merged pass carries the RG16F attachment too (docs/ssao-normal-merge.md, 2026-09-16),
	// so velocity rides the merge by default; the standalone 3-MRT target is only the fallback.
	// r_fsr (C2) implies them: FSR2 is unusable without a velocity buffer, and MV/jitter are
	// internal infrastructure, not user-facing toggles (plan section E).
	const bool velWants = ( ( r_motionVectors.GetBool() || r_fsr.GetBool() )
		&& rhi::GetActiveBackendType() == rhi::BT_VULKAN ) || wantAlbedo;
	if ( !ssaoWants && !ssrWants && !velWants ) {
		return false;
	}
	const bool fullscreenView = viewDef->isWindowView;	// the primary scene view (set by RenderScene)
	if ( !viewDef->viewEntitys || viewDef->isSubview || !fullscreenView ) {
		return false;
	}

	rhi::ShaderHandle gbufProg = r->LoadShader( "gbuffer" );
	if ( !gbufProg ) {
		return false;
	}

	const int w = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int h = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;

	// clear to a flat camera-facing normal (0.5,0.5,1) and the far plane
	rhi::ClearArgs clear;
	memset( &clear, 0, sizeof( clear ) );
	clear.color = true;
	clear.depth = true;
	clear.rgba[0] = 0.5f; clear.rgba[1] = 0.5f; clear.rgba[2] = 1.0f; clear.rgba[3] = 1.0f;

	// r_ssaoMergeNormal (docs/ssao-normal-merge.md): on Vulkan, render the normal into a pass
	// that shares the *scene* depth — one geometry pass producing depth + normal instead of a
	// standalone target. wantMrt (= ssrWants) also carries SSR's rough/metal MRT attachment,
	// and velWants adds the RG16F velocity attachment (R1/A2), so the merge serves SSR, motion
	// vectors and FSR2 too — every tier can fold the standalone pass away. BeginNormalPrepass
	// returns 0 (→ standalone path) on GL3 or if unsupported.
	//
	// NOT when this view looks at a mirror / portal surface (SS_SUBVIEW): the subview was rendered
	// into the scene colour first, over its whole screen RECT, and it is the zfill prepass that
	// cuts it down to the mirror's shape - it draws every other surface BLACK into the colour
	// buffer before the lights add onto it. The merged pass never touches the scene colour, so the
	// reflection stayed on the walls around an obliquely-seen mirror (mars_city1 restroom). Such a
	// view takes the standalone target + zfill, exactly as with r_ssaoMergeNormal 0.
	bool viewHasSubviewSurf = false;
	for ( int i = 0; i < viewDef->numDrawSurfs; i++ ) {
		if ( viewDef->drawSurfs[i]->material->GetSort() == SS_SUBVIEW ) {
			viewHasSubviewSurf = true;
			break;
		}
	}
	const bool wantMerge = r_ssaoMergeNormal.GetBool() && !viewHasSubviewSurf
		&& rhi::GetActiveBackendType() == rhi::BT_VULKAN && !wantAlbedo;	// RTGI albedo needs the standalone 4-attachment target
	rhi::RenderTargetHandle activeNormalRT = 0;
	bool didMerge = false;
	if ( wantMerge ) {
		activeNormalRT = r->BeginNormalPrepass( w, h, &clear, ssrWants, velWants );
		didMerge = ( activeNormalRT != 0 );	// non-zero → the gbuffer pass shares (seals) scene depth
		if ( didMerge ) {
			// PER-VIEW flags the consumers gate on - the standalone target's cache keys
			// (rhiNormalMrt/Vel) describe a different object and must not be touched here
			rhiNormalMrtThisView = ssrWants;
			rhiNormalVelThisView = velWants;
			rhiNormalAlbedoThisView = false;	// merged path never carries the RTGI albedo (wantAlbedo forces standalone)
		}
	}
	if ( activeNormalRT == 0 ) {
		if ( !RB_RHI_EnsureNormalTarget( r, w, h, ssrWants, velWants, wantAlbedo ) ) {
			return false;
		}
		// This standalone prepass SEALS the scene depth (like BeginNormalPrepass / zfill), so it must
		// keep the view weapon's depth-range hack — otherwise the weapon lands at the wrong depth here
		// and close world geometry intersects it, leaking SSAO/RTGI onto the weapon (docs/rtx-rtgi.md).
		r->SetNextTargetHonorsDepthRange();
		r->BeginTargetPass( rhiNormalRT, &clear );
		activeNormalRT = rhiNormalRT;
		rhiNormalMrtThisView = ssrWants;
		rhiNormalVelThisView = velWants;
		rhiNormalAlbedoThisView = wantAlbedo;
	}

	rhi::PipelineDesc pd;
	pd.stateBits = GLS_DEPTHFUNC_LESS;
	pd.shader = gbufProg;
	pd.vertexLayout = rhi::VL_DRAWVERT;
	pd.cullType = RB_RHI_CullFor( viewDef, CT_FRONT_SIDED );

	const viewEntity_t *currentSpace = NULL;
	float mvp[16];
	float spacePrevMvp[16] = { 0 };		// R1/A2: previous frame's MVP for currentSpace (velWants only)

	// R1/B: cancel the per-frame projection jitter out of the velocity so it stays real motion
	// (FSR2 runs MOTION_VECTORS_JITTER_CANCELLATION unset). The jitter is a depth-independent
	// uniform screen shift, so one per-view correction (jitter_cur - jitter_prev)/viewport in
	// +Y-up UV, added to every velocity, is exact. viewDef->jitter is 0 when no jitter is active
	// (jitter is driven by r_fsr on VK, tr_main.cpp), so this is 0 (a no-op) otherwise.
	// Advanced once per frame (this prepass runs only for the primary fullscreen view).
	float jitterCorr[2] = { 0.0f, 0.0f };
	if ( velWants ) {
		jitterCorr[0] = ( viewDef->jitter[0] - rhiPrevJitter[0] ) / (float)w;
		jitterCorr[1] = ( viewDef->jitter[1] - rhiPrevJitter[1] ) / (float)h;
		rhiPrevJitter[0] = viewDef->jitter[0];
		rhiPrevJitter[1] = viewDef->jitter[1];
	}

	drawSurf_t **drawSurfs = (drawSurf_t **)&viewDef->drawSurfs[0];
	for ( int i = 0; i < viewDef->numDrawSurfs; i++ ) {
		const drawSurf_t *surf = drawSurfs[i];
		const srfTriangles_t *tri = surf->geo;
		const idMaterial *shader = surf->material;

		if ( !shader->IsDrawn() || shader->Coverage() == MC_TRANSLUCENT ) {
			continue;
		}
		texgen_t tg = shader->Texgen();
		if ( tg == TG_SCREEN || tg == TG_SCREEN2 || tg == TG_SKYBOX_CUBE || tg == TG_WOBBLESKY_CUBE ) {
			continue;					// sky must not seal the normal/depth buffer
		}
		if ( !tri->numIndexes || ( !tri->ambientCache && !tri->gpuSkinVB ) ) {
			continue;
		}
		// skip materials with every stage conditioned off (mirror the depth prepass)
		const float *regs = surf->shaderRegisters;
		int stage;
		for ( stage = 0; stage < shader->GetNumStages(); stage++ ) {
			if ( regs[shader->GetStage( stage )->conditionRegister] != 0 ) {
				break;
			}
		}
		if ( stage == shader->GetNumStages() ) {
			continue;
		}

		if ( surf->space != currentSpace ) {
			currentSpace = surf->space;
			RB_RHI_SpaceMvp( viewDef, surf->space, mvp );
			// R1/A2: read this space's previous-frame MVP and stage the current one for next
			// frame. Uses the same mvp fed to u_mvpMatrix, so a static space gives prev == cur
			// (exactly zero velocity). While jitter is off this mvp is un-jittered; increment B
			// must keep jitter out of the velocity (FSR2 runs MOTION_VECTORS_JITTER_CANCELLATION
			// unset), by feeding an un-jittered current MVP or subtracting the jitter delta.
			if ( velWants ) {
				RB_RHI_MotionPrevMvp( surf->space, mvp, spacePrevMvp );
			}
		}

		// bump image + texture matrix for this surface, matching the interaction/specular
		// path (din->bumpImage / din->bumpMatrix; see R_SetDrawInteraction) so the G-buffer's
		// bump detail lands at the SAME scale as the lit surface. Using identity here tiled
		// scaled-bump materials wrong (bumps came out double-size). Flat normal if no bump stage.
		const shaderStage_t *bumpStage = shader->GetBumpStage();
		idImage *bumpImg = globalImages->flatNormalMap;
		float bumpS[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
		float bumpT[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
		if ( bumpStage && bumpStage->texture.image ) {
			bumpImg = bumpStage->texture.image;
			if ( bumpStage->texture.hasMatrix ) {
				bumpS[0] = regs[bumpStage->texture.matrix[0][0]];
				bumpS[1] = regs[bumpStage->texture.matrix[0][1]];
				bumpS[3] = regs[bumpStage->texture.matrix[0][2]];
				bumpT[0] = regs[bumpStage->texture.matrix[1][0]];
				bumpT[1] = regs[bumpStage->texture.matrix[1][1]];
				bumpT[3] = regs[bumpStage->texture.matrix[1][2]];
				// keep scrolls bounded (mirrors R_SetDrawInteraction)
				if ( bumpS[3] < -40.0f || bumpS[3] > 40.0f ) bumpS[3] -= (int)bumpS[3];
				if ( bumpT[3] < -40.0f || bumpT[3] > 40.0f ) bumpT[3] -= (int)bumpT[3];
			}
		}

		// per-surface uniforms shared by every draw of this surface; the coverage draws
		// below only override alphaTest + diffuseMatrix on top of these.
		rhi::RenderParams parms;
		memset( &parms, 0, sizeof( parms ) );
		memcpy( parms.mvpMatrix, mvp, sizeof( parms.mvpMatrix ) );
		memcpy( parms.modelViewMatrix, surf->space->modelViewMatrix, sizeof( parms.modelViewMatrix ) );
		if ( velWants ) {
			memcpy( parms.prevMvpMatrix, spacePrevMvp, sizeof( parms.prevMvpMatrix ) );	// R1/A2 motion vectors
			parms.localParam1[0] = jitterCorr[0];		// R1/B: jitter cancellation (gbuffer.frag adds it)
			parms.localParam1[1] = jitterCorr[1];
		}
		memcpy( parms.bumpMatrixS, bumpS, sizeof( bumpS ) );
		memcpy( parms.bumpMatrixT, bumpT, sizeof( bumpT ) );
		// AO mask (gbuffer.frag alpha): 0 on the view weapon so SSAO skips it — its
		// depth-hacked depth makes the horizon search read far geometry (desk edges etc.)
		parms.localParam0[0] = surf->space->weaponDepthHack ? 0.0f : 1.0f;
		// R1/A2 velocity force-zero (gbuffer.frag localParam0.z): SEPARATE from the AO mask above.
		// The view weapon gets REAL motion vectors by default so FSR2 / temporal reprojection stop
		// garbling its bob/sway and the GUI displays on it (the depth hack only touches Z, so the
		// xy/w velocity is valid; degenerate near-plane w is still guarded per-pixel). Only force it
		// back to zero (the old behaviour) when r_weaponMotionVectors is off, for an instant A/B.
		parms.localParam0[2] = ( surf->space->weaponDepthHack && !r_weaponMotionVectors.GetBool() ) ? 1.0f : 0.0f;
		// SSR material attachment (docs/ssr.md): the surface's resolved PBR response,
		// same chain as the lit path. gbuffer.frag echoes pbrParms into MRT output 1;
		// without the second attachment GL discards that write, so filling is free.
		if ( ssrWants ) {
			float metal, rough;
			RB_RHI_ResolvePbrMaterial( shader, metal, rough );
			parms.pbrParms[0] = metal;
			parms.pbrParms[1] = rough;
		}

		// RTGI receiver albedo (docs/rtx-rtgi.md H6c): flag gbuffer.frag to write out_Albedo and fill the
		// diffuse stage's texture matrix so var_TexCoverage samples the diffuse for EVERY surface (opaque
		// included; the perforated coverage draws below overwrite the matrix with their own, fine). The
		// diffuse IMAGE binds on unit 2 below (whiteImage when GI is off; sampled only if u_localParam0.y>0.5).
		idImage *rtgiDiffuseImg = globalImages->whiteImage;
		if ( wantAlbedo ) {
			parms.localParam0[1] = 1.0f;
			for ( int s = 0; s < shader->GetNumStages(); s++ ) {
				const shaderStage_t *dst = shader->GetStage( s );
				if ( dst->lighting != SL_DIFFUSE || dst->texture.image == NULL || regs[dst->conditionRegister] == 0 ) {
					continue;
				}
				rtgiDiffuseImg = dst->texture.image;
				if ( dst->texture.hasMatrix ) {
					parms.diffuseMatrixS[0] = regs[dst->texture.matrix[0][0]];
					parms.diffuseMatrixS[1] = regs[dst->texture.matrix[0][1]];
					parms.diffuseMatrixS[3] = regs[dst->texture.matrix[0][2]];
					parms.diffuseMatrixT[0] = regs[dst->texture.matrix[1][0]];
					parms.diffuseMatrixT[1] = regs[dst->texture.matrix[1][1]];
					parms.diffuseMatrixT[3] = regs[dst->texture.matrix[1][2]];
				} else {
					parms.diffuseMatrixS[0] = 1.0f; parms.diffuseMatrixS[1] = 0.0f; parms.diffuseMatrixS[3] = 0.0f;
					parms.diffuseMatrixT[0] = 0.0f; parms.diffuseMatrixT[1] = 1.0f; parms.diffuseMatrixT[3] = 0.0f;
				}
				break;
			}
		}

		// DUDE tessellation: subdivide character/monster surfaces here identically to the
		// depth prepass (same PN + displacement) so SSAO's normals follow the rounded
		// silhouette the lit passes draw — otherwise the AO hugs the flat, un-tessellated
		// edges (faceted shadows on a now-rounded model). The bump is already on unit 0
		// with the matching bump matrix (var_TexBump), so gbuffer.tese displaces
		// bit-identically to zfill.tese. pd.tessellate applies to both draws below.
		bool useDeform = false;
		const bool tess = RB_RHI_TessOrDeform( surf, tri, useDeform );
		if ( tess ) {
			RB_RHI_SetTessParms( parms );
		}
		pd.tessellate = tess;
		RB_RHI_BindTessHeight( surf, tess );	// DUDE: unit 15 signed height (docs/tessellation.md)

		rhi::BufferHandle vb, ib;
		int vertOfs, idxOfs;
		RB_RHI_StreamAmbient( r, tri, vb, vertOfs, ib, idxOfs );

		// Skinned motion vectors (r_skinnedMotionVectors): a GPU-skinned surface carries a buffer of its
		// verts' PREVIOUS-frame model-space positions (written by this frame's skin dispatch); hand its
		// device address to gbuffer.vert so the velocity includes the ANIMATION's motion, not only the
		// entity's rigid motion. Only when the draw really uses gpuSkinVB at offset 0 (gl_VertexIndex is
		// the index into that buffer) - not the deform-once expansion - and the pose was skinned this frame.
		if ( velWants && !useDeform && tri->gpuSkinVB && vb == tri->gpuSkinVB && tri->gpuSkinPrevValid
				&& tri->gpuSkinPrevAddr != 0 && tri->gpuSkinFrame == tr.frameCount ) {
			memcpy( &parms.rtParms[0], &tri->gpuSkinPrevAddr, sizeof( tri->gpuSkinPrevAddr ) );
		}

		// Layered surfaces (decals, signs, details) sit coplanar on walls and rely on
		// polygon offset to win the depth test — same as the depth prepass. Without it
		// they z-fight in the normal buffer and their normals flicker against the wall's.
		const bool polyOffset = shader->TestMaterialFlag( MF_POLYGONOFFSET );
		if ( polyOffset ) {
			// Vulkan: the qgl pointers are NULL — calling them segfaults, so guard
			// like every other qgl site here. The RHI dynamic depth bias below is
			// what actually offsets the decal in the normal buffer on Vulkan.
			if ( qglEnable != NULL ) {
				qglEnable( GL_POLYGON_OFFSET_FILL );
				qglPolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
			}
			r->SetPolygonOffset( true, r_offsetFactor.GetFloat(),
			                     r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
		}

		// Depth hacks (view weapon / depth-hacked models) so the geometry rasterizes into
		// the normal buffer exactly as in the main view — same coverage + depth ordering,
		// which keeps the weapon's AO mask reliable even when it overlaps close walls. The
		// projection tweak is already in the MVP (RB_RHI_SpaceMvp); this sets the depth range.
		const bool depthHack = surf->space->weaponDepthHack || surf->space->modelDepthHack != 0.0f;
		if ( surf->space->weaponDepthHack ) {
			RB_EnterWeaponDepthHack();
		}
		if ( surf->space->modelDepthHack != 0.0f ) {
			RB_EnterModelDepthHack( surf->space->modelDepthHack );
		}

		RB_RHI_BindUnit( 0, bumpImg );
		RB_RHI_BindUnit( 2, rtgiDiffuseImg );	// RTGI H6c: receiver-albedo source (gbuffer.frag samples it only when u_localParam0.y>0.5)
		r->BindPipeline( pd );

		rhi::DrawArgs da;
		memset( &da, 0, sizeof( da ) );
		da.vertexBuffer = useDeform ? tri->tessDeformVB : vb;
		da.vertexOffset = useDeform ? 0 : vertOfs;
		da.indexBuffer = useDeform ? tri->tessDeformIB : ib;
		da.firstIndex = useDeform ? 0 : ( idxOfs / (int)sizeof( glIndex_t ) );
		da.indexCount = useDeform ? tri->tessDeformIndexes : tri->numIndexes;
		da.uniformSize = sizeof( parms );

		// Perforated (alpha-tested) surfaces: draw one live alpha-tested stage per coverage
		// mask, binding its diffuse map on unit 1 so gbuffer.frag discards the transparent
		// texels. Mirrors the depth prepass (RB_RHI_FillDepthBuffer) so the normal buffer's
		// coverage matches the sealed depth. Without it the whole card writes a flat normal.
		// Also mirrors its per-stage privatePolygonOffset: when this pass IS the sealed scene
		// depth, the burning-corpse fire threshold must land at D+bias here too, or the
		// EQUAL-drawn ember floods the whole body (docs/burn-away-ember.md).
		bool drewCoverage = false;
		if ( shader->Coverage() == MC_PERFORATED ) {
			for ( int s = 0; s < shader->GetNumStages(); s++ ) {
				const shaderStage_t *pStage = shader->GetStage( s );
				if ( !pStage->hasAlphaTest || regs[pStage->conditionRegister] == 0 ) {
					continue;
				}
				parms.alphaTest[0] = regs[pStage->alphaTestRegister];
				parms.alphaTest[1] = 1.0f;
				if ( pStage->texture.hasMatrix ) {
					parms.diffuseMatrixS[0] = regs[pStage->texture.matrix[0][0]];
					parms.diffuseMatrixS[1] = regs[pStage->texture.matrix[0][1]];
					parms.diffuseMatrixS[3] = regs[pStage->texture.matrix[0][2]];
					parms.diffuseMatrixT[0] = regs[pStage->texture.matrix[1][0]];
					parms.diffuseMatrixT[1] = regs[pStage->texture.matrix[1][1]];
					parms.diffuseMatrixT[3] = regs[pStage->texture.matrix[1][2]];
				} else {
					parms.diffuseMatrixS[0] = 1.0f; parms.diffuseMatrixS[1] = 0.0f; parms.diffuseMatrixS[3] = 0.0f;
					parms.diffuseMatrixT[0] = 0.0f; parms.diffuseMatrixT[1] = 1.0f; parms.diffuseMatrixT[3] = 0.0f;
				}

				rhi::BufferHandle ub;
				int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );
				RB_RHI_StagePolyOffsetBegin( r, pStage );
				RB_RHI_BindUnit( 1, pStage->texture.image );
				da.uniformBuffer = ub;
				da.uniformOffset = uniOfs;
				RB_RHI_VkTextures( da );	// VK: units 0 (bump) + 1 (diffuse coverage) from rhiVkUnits
				r->Draw( da );
				backEnd.pc.c_drawElements++;
				RB_RHI_StagePolyOffsetEnd( r, pStage, shader );
				drewCoverage = true;
			}
		}

		// Opaque surfaces (and perforated materials with no live alpha-tested stage): one
		// solid draw with the coverage test disabled. whiteImage keeps unit 1 valid even
		// though the disabled test short-circuits the fetch in the shader.
		if ( !drewCoverage ) {
			parms.alphaTest[0] = 0.0f;
			parms.alphaTest[1] = 0.0f;
			rhi::BufferHandle ub;
			int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );
			RB_RHI_BindUnit( 1, globalImages->whiteImage );
			da.uniformBuffer = ub;
			da.uniformOffset = uniOfs;
			RB_RHI_VkTextures( da );	// VK: units 0 (bump) + 1 (white) from rhiVkUnits
			r->Draw( da );
			backEnd.pc.c_drawElements++;
		}

		if ( depthHack ) {
			RB_LeaveDepthHack();
		}
		if ( polyOffset ) {
			if ( qglDisable != NULL ) {
				qglDisable( GL_POLYGON_OFFSET_FILL );
			}
			r->SetPolygonOffset( false, 0.0f, 0.0f );
		}
	}

	r->EndPass();
	RB_RHI_ForgetTexBinds();
	rhiNormalResultRT = activeNormalRT;		// standalone rhiNormalRT or the merged handle
	rhiNormalReadyThisView = true;
	return didMerge;
}

// --- Render-target lifecycle helpers ---------------------------------------
// The Ensure* passes below share three exact recipes: (re)allocate a target when it is
// missing, lost (a vid_restart drops the texture but leaves the handle set), or sized to
// a stale resolution, rolling everything back to 0/unsized on a failed create. Each
// per-feature wrapper keeps its name (and all its call sites) but delegates its body here
// so the recipe lives in one place.

// Single plain color target tracked by (handle, curW, curH).
static bool RB_RHI_EnsureRT( rhi::RHI *r, rhi::RenderTargetHandle &rt, int &curW, int &curH,
		rhi::ImageFormat fmt, int w, int h ) {
	if ( rt && r->GetRenderTargetImage( rt ) == 0 ) {	// lost context (vid_restart)
		rt = 0;
		curW = curH = 0;
	}
	if ( rt && curW == w && curH == h ) {
		return true;
	}
	if ( rt ) { r->DestroyRenderTarget( rt ); rt = 0; }
	rt = r->CreateRenderTarget( fmt, w, h );
	if ( !rt ) {
		curW = curH = 0;
		return false;
	}
	curW = w;
	curH = h;
	return true;
}

// Temporal history ping-pong (two slots) tracked by (pair[2], curW, curH, valid). Any
// (re)allocation clears 'valid' so the first resolve reprojects nothing and passes the
// current frame through rather than blending stale or mis-sized data.
static bool RB_RHI_EnsureHistoryPair( rhi::RHI *r, rhi::RenderTargetHandle pair[2],
		int &curW, int &curH, bool &valid, rhi::ImageFormat fmt, int w, int h ) {
	if ( pair[0] && r->GetRenderTargetImage( pair[0] ) == 0 ) {	// lost context (vid_restart)
		pair[0] = pair[1] = 0;
		curW = curH = 0;
	}
	if ( pair[0] && pair[1] && curW == w && curH == h ) {
		return true;
	}
	for ( int i = 0; i < 2; i++ ) {
		if ( pair[i] ) { r->DestroyRenderTarget( pair[i] ); pair[i] = 0; }
	}
	pair[0] = r->CreateRenderTarget( fmt, w, h );
	pair[1] = r->CreateRenderTarget( fmt, w, h );
	valid = false;	// freshly (re)allocated: nothing to reproject yet
	if ( !pair[0] || !pair[1] ) {
		for ( int i = 0; i < 2; i++ ) {
			if ( pair[i] ) { r->DestroyRenderTarget( pair[i] ); pair[i] = 0; }
		}
		curW = curH = 0;
		return false;
	}
	curW = w;
	curH = h;
	return true;
}

// Mipped target tracked by (handle, curW, curH, curLevels). The level count is derived
// from the larger dimension, capped at maxLevels (<= 6); because it is a pure function of
// (w,h,maxLevels) the level check in the early-out never trips spuriously. Returns false
// when the backend has no mipped-target capability (CreateRenderTargetMipped -> 0) so the
// caller falls back to its full-res path.
static bool RB_RHI_EnsureMippedRT( rhi::RHI *r, rhi::RenderTargetHandle &rt, int &curW, int &curH,
		int &curLevels, rhi::ImageFormat fmt, int w, int h, int maxLevels ) {
	if ( rt && r->GetRenderTargetImage( rt ) == 0 ) {	// lost context (vid_restart)
		rt = 0;
		curW = curH = curLevels = 0;
	}
	int levels = 1;
	for ( int d = ( w > h ? w : h ); d > 1 && levels < maxLevels; d >>= 1 ) { levels++; }
	if ( rt && curW == w && curH == h && curLevels == levels ) {
		return true;
	}
	if ( rt ) { r->DestroyRenderTarget( rt ); rt = 0; }
	rt = r->CreateRenderTargetMipped( fmt, w, h, levels );
	if ( !rt ) {
		curW = curH = curLevels = 0;
		return false;
	}
	curW = w;
	curH = h;
	curLevels = levels;
	return true;
}

static bool RB_RHI_EnsureSsaoTargets( rhi::RHI *r, int w, int h ) {
	// a lost context (vid_restart) leaves the handle set but its texture gone
	if ( rhiSsaoRT && r->GetRenderTargetImage( rhiSsaoRT ) == 0 ) {
		rhiSsaoRT = rhiSsaoBlurRT = 0;
		rhiSsaoW = rhiSsaoH = 0;
	}
	if ( rhiSsaoRT && rhiSsaoBlurRT && rhiSsaoW == w && rhiSsaoH == h ) {
		return true;
	}
	if ( rhiSsaoRT )     { r->DestroyRenderTarget( rhiSsaoRT );     rhiSsaoRT = 0; }
	if ( rhiSsaoBlurRT ) { r->DestroyRenderTarget( rhiSsaoBlurRT ); rhiSsaoBlurRT = 0; }
	rhiSsaoRT     = r->CreateRenderTarget( rhi::IF_RGBA8, w, h );
	rhiSsaoBlurRT = r->CreateRenderTarget( rhi::IF_RGBA8, w, h );
	if ( !rhiSsaoRT || !rhiSsaoBlurRT ) {
		if ( rhiSsaoRT )     { r->DestroyRenderTarget( rhiSsaoRT );     rhiSsaoRT = 0; }
		if ( rhiSsaoBlurRT ) { r->DestroyRenderTarget( rhiSsaoBlurRT ); rhiSsaoBlurRT = 0; }
		rhiSsaoW = rhiSsaoH = 0;
		return false;
	}
	rhiSsaoW = w;
	rhiSsaoH = h;
	return true;
}

// SSAO Phase 1: (re)allocate the prefiltered linear-depth mip target at the AO size.
// Returns false when the backend has no mipped-target capability (CreateRenderTargetMipped
// returns 0), so the caller silently falls back to the full-res raw-depth march.
static bool RB_RHI_EnsureSsaoDepthMip( rhi::RHI *r, int w, int h ) {
	// enough levels to cover the horizon radius in coarse mips, but no more than the march
	// can ever read: r_ssaoDepthMipMaxLod clamps its LOD, so levels past the cap would be
	// rendered every frame and never sampled. 6 levels (1/32 footprint) stays the ceiling.
	const int maxLevels = idMath::ClampInt( 2, 6, r_ssaoDepthMipMaxLod.GetInteger() + 1 );
	// R16F: only .r is ever written/read (linear eye depth), so single-channel half-float
	// is bit-identical to RGBA16F at a quarter the bandwidth.
	return RB_RHI_EnsureMippedRT( r, rhiSsaoDepthMipRT, rhiSsaoDepthMipW, rhiSsaoDepthMipH,
			rhiSsaoDepthMipLevels, rhi::IF_R16F, w, h, maxLevels );
}

// Ensure the two ping-ponged temporal-history buffers exist at the AO size. Any
// (re)create -- first use, resolution change, or a lost context -- invalidates the
// history so the resolve falls back to the current frame rather than blending stale
// or garbage data. Only called while r_ssaoTemporal is on.
static bool RB_RHI_EnsureSsaoHistory( rhi::RHI *r, int w, int h ) {
	return RB_RHI_EnsureHistoryPair( r, rhiSsaoHistRT, rhiSsaoHistW, rhiSsaoHistH,
			rhiSsaoHistValid, rhi::IF_RGBA8, w, h );
}

// Invert a GL column-major 4x4 (float[16]) into out (also GL column-major). idMat4 is
// row-major, so transpose the array in and back out; the algorithm is layout-agnostic
// as long as read/write are consistent. Returns false on a singular matrix.
static bool R_InvertGLMatrix( const float in[16], float out[16] ) {
	float m[4][4];
	for ( int c = 0; c < 4; ++c ) {
		for ( int rr = 0; rr < 4; ++rr ) {
			m[rr][c] = in[c * 4 + rr];		// GL(col,row) -> row-major [row][col]
		}
	}
	idMat4 mat( m );
	if ( !mat.InverseSelf() ) {
		return false;
	}
	for ( int c = 0; c < 4; ++c ) {
		for ( int rr = 0; rr < 4; ++rr ) {
			out[c * 4 + rr] = mat[rr][c];	// back to GL column-major
		}
	}
	return true;
}

// One fullscreen NDC quad (identity mvp, st 0..1) through a post shader. Any engine
// (idImage) inputs must already be bound by the caller; rtInput0 (0 = none) is an
// RHI render-target texture bound on unit 0. extraStateBits ORs blend modes on top
// of the replace-mode default (the SSR composite draws additively).
static void RB_RHI_DrawFullscreen( rhi::RHI *r, rhi::ShaderHandle prog,
                                   const rhi::RenderParams &parms, rhi::ImageHandle rtInput0,
                                   int extraStateBits = 0 ) {
	idDrawVert quad[4];
	memset( quad, 0, sizeof( quad ) );
	quad[0].xyz.Set( -1.0f, -1.0f, 0.0f ); quad[0].st[0] = 0.0f; quad[0].st[1] = 0.0f;
	quad[1].xyz.Set(  1.0f, -1.0f, 0.0f ); quad[1].st[0] = 1.0f; quad[1].st[1] = 0.0f;
	quad[2].xyz.Set(  1.0f,  1.0f, 0.0f ); quad[2].st[0] = 1.0f; quad[2].st[1] = 1.0f;
	quad[3].xyz.Set( -1.0f,  1.0f, 0.0f ); quad[3].st[0] = 0.0f; quad[3].st[1] = 1.0f;
	glIndex_t idx[6] = { 0, 1, 2, 0, 2, 3 };

	rhi::BufferHandle vb, ib, ub;
	int vertOfs = r->AllocVertices( quad, sizeof( quad ), &vb );
	int idxOfs  = r->AllocIndices( idx, sizeof( idx ), &ib );
	int uniOfs  = r->AllocUniforms( &parms, sizeof( parms ), &ub );

	rhi::PipelineDesc pd;
	pd.stateBits = GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK | extraStateBits;
	pd.shader = prog;
	pd.vertexLayout = rhi::VL_DRAWVERT;
	pd.cullType = CT_TWO_SIDED;
	r->BindPipeline( pd );

	rhi::DrawArgs da;
	memset( &da, 0, sizeof( da ) );
	da.vertexBuffer = vb;
	da.vertexOffset = vertOfs;
	da.indexBuffer = ib;
	da.firstIndex = idxOfs / (int)sizeof( glIndex_t );
	da.indexCount = 6;
	da.uniformBuffer = ub;
	da.uniformOffset = uniOfs;
	da.uniformSize = sizeof( parms );
	da.textures[0] = rtInput0;		// 0 = leave unit 0 as the caller bound it
	RB_RHI_VkTextures( da );			// VK: pull units 1-10 the caller recorded via RB_RHI_BindRTUnit
	if ( rtInput0 != 0 ) {
		da.textures[0] = rtInput0;	// the unit-0 param wins on both backends
	}
	r->Draw( da );

	backEnd.pc.c_drawElements++;
}

// Decal surface mask -> SSR. The reflection passes read roughness/metalness from the G-buffer's
// material attachment, which knows nothing about decals; resolve it through this view's decal
// retention buffer once (decalmat.frag) and hand every consumer the result, so a stain that is
// dull under direct light is not a mirror in the reflection. Returns matImg untouched when no
// decal drew this view.
static rhi::RenderTargetHandle rhiDecalMatRT = 0;
static int rhiDecalMatW = 0, rhiDecalMatH = 0;

static rhi::ImageHandle RB_RHI_DecalResolveMaterial( rhi::RHI *r, rhi::ImageHandle matImg, int w, int h ) {
	if ( rhiDecalMaskImage == 0 || matImg == 0 ) {
		return matImg;
	}
	rhi::ShaderHandle prog = r->LoadShader( "decalmat" );
	if ( !prog ) {
		return matImg;
	}
	if ( rhiDecalMatRT && r->GetRenderTargetImage( rhiDecalMatRT ) == 0 ) {
		rhiDecalMatRT = 0;					// lost context (vid_restart)
	}
	if ( !rhiDecalMatRT || rhiDecalMatW != w || rhiDecalMatH != h ) {
		if ( rhiDecalMatRT ) { r->DestroyRenderTarget( rhiDecalMatRT ); }
		rhiDecalMatRT = r->CreateRenderTarget( rhi::IF_RGBA8, w, h );
		rhiDecalMatW = w;
		rhiDecalMatH = h;
		if ( !rhiDecalMatRT ) {
			rhiDecalMatW = rhiDecalMatH = 0;
			return matImg;
		}
	}
	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	r->BeginTargetPass( rhiDecalMatRT, NULL );
	r->SetViewport( 0, 0, w, h );
	r->SetScissor( 0, 0, w, h );
	RB_RHI_BindRTImage( r, 1, rhiDecalMaskImage );
	RB_RHI_DrawFullscreen( r, prog, parms, matImg );
	r->EndPass();
	if ( rhi::GetActiveBackendType() != rhi::BT_VULKAN ) {
		rhi::gl3ActiveTexture( GL_TEXTURE0 );
		backEnd.glState.currenttmu = 0;
		backEnd.glState.tmu[0].current2DMap = -1;	// direct binds bypassed the tmu cache
	}
	RB_RHI_ForgetTexBinds();
	return r->GetRenderTargetImage( rhiDecalMatRT );
}

// (re)allocate the berserk temporal trail ping-pong. Both slots are cleared to black on
// creation so the first history read is defined on Vulkan (an unwritten color target's
// layout is UNDEFINED and would trip validation) and starts from nothing on GL.
static bool RB_RHI_EnsureBerserkTrail( rhi::RHI *r, int w, int h ) {
	if ( rhiBerserkTrailRT[0] && r->GetRenderTargetImage( rhiBerserkTrailRT[0] ) == 0 ) {
		rhiBerserkTrailRT[0] = rhiBerserkTrailRT[1] = 0;	// lost context (vid_restart)
		rhiBerserkW = rhiBerserkH = 0;
	}
	if ( rhiBerserkTrailRT[0] && rhiBerserkTrailRT[1] && rhiBerserkW == w && rhiBerserkH == h ) {
		return true;
	}
	for ( int i = 0; i < 2; i++ ) {
		if ( rhiBerserkTrailRT[i] ) { r->DestroyRenderTarget( rhiBerserkTrailRT[i] ); rhiBerserkTrailRT[i] = 0; }
	}
	rhiBerserkTrailRT[0] = r->CreateRenderTarget( rhi::IF_RGBA8, w, h );
	rhiBerserkTrailRT[1] = r->CreateRenderTarget( rhi::IF_RGBA8, w, h );
	if ( !rhiBerserkTrailRT[0] || !rhiBerserkTrailRT[1] ) {
		for ( int i = 0; i < 2; i++ ) {
			if ( rhiBerserkTrailRT[i] ) { r->DestroyRenderTarget( rhiBerserkTrailRT[i] ); rhiBerserkTrailRT[i] = 0; }
		}
		rhiBerserkW = rhiBerserkH = 0;
		return false;
	}
	rhi::ClearArgs clear;
	memset( &clear, 0, sizeof( clear ) );
	clear.color = true;
	r->BeginTargetPass( rhiBerserkTrailRT[0], &clear ); r->EndPass();
	r->BeginTargetPass( rhiBerserkTrailRT[1], &clear ); r->EndPass();
	rhiBerserkW = w;			rhiBerserkH = h;
	rhiBerserkIdx = 0;			rhiBerserkValid = false;
	rhiBerserkLastTick = -100000;
	return true;
}

/*
===================
RB_RHI_BerserkAccum

Advance the berserk-vision feedback buffer one frame and return it for the display blit
in RB_RHI_RenderShaderPasses. A faithful port of the stock ARB material
textures/decals/berserk (materials/decals.mtr): each frame folds the freshly captured
scene with the PREVIOUS frame magnified ~3% about the centre (centerscale 0.97), gated by
the berserk2 texture's alpha (a radial mask — sharp at the centre, feedback at the edges;
rotated over time). The recursion runs in a ping-pong render target — the reliable flavour
of cross-frame feedback (same as the SSAO/SSR history) — because the stock recursive
_scratch capture doesn't accumulate on the RHI path.

baseScale = per-60fps-frame centerscale (0.95 baked in the caller; stock is 0.97), feedback =
mask/feedback strength, fade = 0..1 wind-down (1 active; as it falls the
zoom relaxes to identity and the feedback drops, so the streaks settle and merge back into
the sharp scene). Returns the accumulated image (bound on unit 1 for the display draw) or 0.
===================
*/
rhi::ImageHandle RB_RHI_BerserkAccum( rhi::RHI *r, const viewDef_t *viewDef,
                                      float baseScale, float feedback, float fade,
                                      int trailDiv, int timeMs ) {
	const int fullW = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int fullH = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
	int div = ( trailDiv < 1 ) ? 1 : trailDiv;
	int tw = fullW / div;	if ( tw < 1 ) tw = 1;
	int th = fullH / div;	if ( th < 1 ) th = 1;
	if ( !RB_RHI_EnsureBerserkTrail( r, tw, th ) ) {
		return 0;
	}

	rhi::ShaderHandle accumProg = r->LoadShader( "berserk_accum" );
	if ( !accumProg ) {
		return 0;
	}

	// the rotating radial mask (stock stage 0 = maskcolor berserk2). Cached once; clamp
	// addressing + linear filtering, matching the material's `clamp`.
	static idImage *maskImg = NULL;
	if ( maskImg == NULL ) {
		maskImg = globalImages->ImageFromFile( "textures/decals/berserk2", TF_LINEAR, false,
		                                       TR_CLAMP, TD_HIGH_QUALITY );
	}
	if ( maskImg == NULL || maskImg == globalImages->defaultImage ) {
		return 0;
	}

	const bool vkMode = rhi::GetActiveBackendType() == rhi::BT_VULKAN;

	// a large gap since the last frame means the held trail is stale — berserk was
	// re-entered, or the wind-down ended and a new one began. Drop it so this frame
	// re-seeds from the clean scene instead of recirculating a scene from seconds ago.
	int dt = timeMs - rhiBerserkLastTick;
	if ( dt > 1000 || dt < 0 ) {
		rhiBerserkValid = false;
		dt = 16;
	}
	if ( dt < 1 ) { dt = 1; }

	// framerate-independent zoom: the stock effect magnified 0.97/frame at its ~60fps, so
	// normalise to a 60fps (16.67ms) reference — otherwise it rushes outward at high fps.
	// Wind-down (fade<1) relaxes the magnify toward identity so the streaks stop growing.
	float frameScale = idMath::Pow( baseScale, (float)dt / 16.6667f );
	float scale = 1.0f + ( frameScale - 1.0f ) * fade;

	// mask rotation (stock `rotate time*3`, value in cycles -> radians)
	float ang = idMath::TWO_PI * ( (float)timeMs * 0.001f * 3.0f );

	const int writeIdx = rhiBerserkIdx;
	const int readIdx  = 1 - rhiBerserkIdx;

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	parms.localParam0[0] = scale;
	parms.localParam0[1] = feedback * fade;					// wind-down fades the feedback out
	parms.localParam0[2] = rhiBerserkValid ? 1.0f : 0.0f;
	// _scratch is a POT full-res capture (no crop) — the scene fills only [0,shiftScale] of it.
	// Screen-correct the scene sample so the whole render maps to the trail's [0,1] (.w on both).
	parms.localParam0[3] = ( globalImages->scratchImage->uploadWidth  > 0 )
		? (float)fullW / globalImages->scratchImage->uploadWidth  : 1.0f;
	parms.localParam1[3] = ( globalImages->scratchImage->uploadHeight > 0 )
		? (float)fullH / globalImages->scratchImage->uploadHeight : 1.0f;
	parms.localParam1[0] = cosf( ang );
	parms.localParam1[1] = sinf( ang );
	// Vulkan writes this fullscreen pass through a flipY viewport while _scratch is captured
	// top-down; the shader works in _scratch space so the feedback stays coherent (no per-frame
	// vertical flip). GL has no flipY, so leave it off there.
	parms.localParam1[2] = vkMode ? 1.0f : 0.0f;

	r->BeginTargetPass( rhiBerserkTrailRT[writeIdx], NULL );
	RB_RHI_BindUnit( 0, globalImages->scratchImage );		// current scene (both backends)
	RB_RHI_BindRTUnit( r, 1, rhiBerserkTrailRT[readIdx] );	// previous frame (cleared -> valid on first use)
	RB_RHI_BindUnit( 2, maskImg );							// berserk2 radial mask
	if ( !vkMode ) {
		backEnd.glState.tmu[1].current2DMap = -1;			// direct bind bypassed the tmu cache
	}
	RB_RHI_DrawFullscreen( r, accumProg, parms, 0 );
	r->EndPass();

	rhiBerserkIdx  = readIdx;		// next frame writes the other slot
	rhiBerserkValid = true;
	rhiBerserkLastTick = timeMs;

	rhi::ImageHandle trailImg = r->GetRenderTargetImage( rhiBerserkTrailRT[writeIdx] );

	// bind it on unit 1 for the display draw that follows in the surface loop: GL keeps
	// the binding live; the Vulkan caller re-supplies it via DrawArgs.textures[1].
	RB_RHI_BindRTImage( r, 1, trailImg );
	if ( !vkMode ) {
		backEnd.glState.tmu[1].current2DMap = -1;
	}
	return trailImg;
}

// (re)allocate the hell-time temporal trail ping-pong (mirrors RB_RHI_EnsureBerserkTrail).
static bool RB_RHI_EnsureHelltimeTrail( rhi::RHI *r, int w, int h ) {
	if ( rhiHelltimeTrailRT[0] && r->GetRenderTargetImage( rhiHelltimeTrailRT[0] ) == 0 ) {
		rhiHelltimeTrailRT[0] = rhiHelltimeTrailRT[1] = 0;	// lost context (vid_restart)
		rhiHelltimeW = rhiHelltimeH = 0;
	}
	if ( rhiHelltimeTrailRT[0] && rhiHelltimeTrailRT[1] && rhiHelltimeW == w && rhiHelltimeH == h ) {
		return true;
	}
	for ( int i = 0; i < 2; i++ ) {
		if ( rhiHelltimeTrailRT[i] ) { r->DestroyRenderTarget( rhiHelltimeTrailRT[i] ); rhiHelltimeTrailRT[i] = 0; }
	}
	rhiHelltimeTrailRT[0] = r->CreateRenderTarget( rhi::IF_RGBA8, w, h );
	rhiHelltimeTrailRT[1] = r->CreateRenderTarget( rhi::IF_RGBA8, w, h );
	if ( !rhiHelltimeTrailRT[0] || !rhiHelltimeTrailRT[1] ) {
		for ( int i = 0; i < 2; i++ ) {
			if ( rhiHelltimeTrailRT[i] ) { r->DestroyRenderTarget( rhiHelltimeTrailRT[i] ); rhiHelltimeTrailRT[i] = 0; }
		}
		rhiHelltimeW = rhiHelltimeH = 0;
		return false;
	}
	rhi::ClearArgs clear;
	memset( &clear, 0, sizeof( clear ) );
	clear.color = true;
	r->BeginTargetPass( rhiHelltimeTrailRT[0], &clear ); r->EndPass();
	r->BeginTargetPass( rhiHelltimeTrailRT[1], &clear ); r->EndPass();
	rhiHelltimeW = w;			rhiHelltimeH = h;
	rhiHelltimeIdx = 0;			rhiHelltimeValid = false;
	rhiHelltimeLastTick = -100000;
	return true;
}

/*
===================
RB_RHI_HelltimeAccum

Advance the D3XP hell-time (Artifact) feedback buffer one frame and return it for the display
composite in RB_RHI_RenderShaderPasses (the bloodorbN/cr_draw blit). Faithful port of the stock
recursive _accum zoom-feedback (materials/smf.mtr) — see helltime_accum.frag. `level` selects the
per-level look (0 = HELLTIME/Artifact, 1 = BERSERK, 2 = INVULNERABILITY), which the caller reads
from the bloodorb1/2/3 material name. The scene input is _currentRender (captured by the fx
manager's CaptureCurrentRender before the accum pass). Returns the trail image, or 0.
===================
*/
rhi::ImageHandle RB_RHI_HelltimeAccum( rhi::RHI *r, const viewDef_t *viewDef, int level, int timeMs ) {
	const int fullW = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int fullH = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
	int tw = fullW;	if ( tw < 1 ) tw = 1;
	int th = fullH;	if ( th < 1 ) th = 1;
	if ( !RB_RHI_EnsureHelltimeTrail( r, tw, th ) ) {
		return 0;
	}

	rhi::ShaderHandle accumProg = r->LoadShader( "helltime_accum" );
	if ( !accumProg ) {
		return 0;
	}

	// the radial gate (stock maskcolor stage = bloodorb3.tga). Cached once; clamp + linear,
	// matching the material's `clamp`. Its alpha is ~1 at the centre, ~0 at the edges.
	static idImage *maskImg = NULL;
	if ( maskImg == NULL ) {
		maskImg = globalImages->ImageFromFile( "textures/smf/bloodorb3", TF_LINEAR, false,
		                                       TR_CLAMP, TD_HIGH_QUALITY );
	}
	if ( maskImg == NULL || maskImg == globalImages->defaultImage ) {
		return 0;
	}

	// current scene is _currentRender (the fx manager captured it before this pass); if it was
	// never captured on Vulkan, bail so the display falls back to the plain scene.
	idImage *sceneImg = globalImages->currentRenderImage;
	const bool vkMode = rhi::GetActiveBackendType() == rhi::BT_VULKAN;
	if ( vkMode && ( !sceneImg->rhiCaptured || !sceneImg->rhiHandle ) ) {
		return 0;
	}

	// re-seed after a large gap (re-entry / a new powerup window) so we don't recirculate a
	// scene from seconds ago.
	int dt = timeMs - rhiHelltimeLastTick;
	if ( dt > 1000 || dt < 0 ) {
		rhiHelltimeValid = false;
		dt = 16;
	}
	if ( dt < 1 ) { dt = 1; }
	const float dtNorm = (float)dt / 16.6667f;

	// per-level baked params (materials/smf.mtr bloodorb1/2/3). Scales/tints authored per-60fps
	// frame; framerate-normalise the magnify like berserk. Rotation is a per-frame increment
	// that accumulates through the recursion (stock `rotate 0.005` is a fixed per-frame spin).
	// tint = the stock cr_capture colour (the dominant per-level scene tint on the injected
	// _currentRender), NOT the subtle ac_capture accum tint — that's what colours the whole view.
	float baseScale = 0.995f;
	float rotCyclesPerFrame = 0.0f;
	float tint[3] = { 1.0f, 1.0f, 1.0f };
	const float sPulse = sinf( (float)timeMs * 0.001f * 0.5f * idMath::TWO_PI );	// stock sintable[time*0.5]
	switch ( level ) {
	default:
	case 0:	// HELLTIME / Artifact — neutral (bloodorb1 cr_capture 1,1,1), gentle zoom, no rotation
		baseScale = 0.995f;
		break;
	case 1:	// BERSERK — warm (bloodorb2 cr_capture 1,0.8,0.8), slow spin, subtle scale pulse
		baseScale = 0.990f + sPulse * 0.004f;
		rotCyclesPerFrame = 0.005f;
		tint[0] = 1.0f; tint[1] = 0.8f; tint[2] = 0.8f;
		break;
	case 2:	// INVULNERABILITY — red (bloodorb3 cr_capture 0.8,0.5,0.5), slow spin
		baseScale = 0.995f + sPulse * 0.004f;
		rotCyclesPerFrame = 0.005f;
		tint[0] = 0.8f; tint[1] = 0.5f; tint[2] = 0.5f;
		break;
	}

	const float scale = idMath::Pow( baseScale, dtNorm );
	const float ang = idMath::TWO_PI * rotCyclesPerFrame * dtNorm;		// per-frame spin increment

	const int writeIdx = rhiHelltimeIdx;
	const int readIdx  = 1 - rhiHelltimeIdx;

	// _currentRender is POT-oversized; the scene lives in [0..shiftScale]. Pass shiftScale so the
	// accum un-squashes it into the full trail (helltime_accum.frag scales the scene read by it).
	const int potW = sceneImg->uploadWidth;
	const int potH = sceneImg->uploadHeight;

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	parms.screenCorrection[0] = ( potW > 0 ) ? (float)fullW / potW : 1.0f;
	parms.screenCorrection[1] = ( potH > 0 ) ? (float)fullH / potH : 1.0f;
	parms.localParam0[0] = scale;
	parms.localParam0[1] = 1.0f;						// feedback strength (full mask gate)
	parms.localParam0[2] = rhiHelltimeValid ? 1.0f : 0.0f;
	parms.localParam0[3] = 1.0f;						// maskInvert (bloodorb3 alpha is high-centre)
	parms.localParam1[0] = cosf( ang );
	parms.localParam1[1] = sinf( ang );
	parms.localParam1[2] = vkMode ? 1.0f : 0.0f;		// flipY (see helltime_accum.frag)
	parms.color[0] = tint[0];
	parms.color[1] = tint[1];
	parms.color[2] = tint[2];
	parms.color[3] = 1.0f;

	r->BeginTargetPass( rhiHelltimeTrailRT[writeIdx], NULL );
	RB_RHI_BindUnit( 0, sceneImg );							// _currentRender (both backends)
	RB_RHI_BindRTUnit( r, 1, rhiHelltimeTrailRT[readIdx] );	// previous frame
	RB_RHI_BindUnit( 2, maskImg );							// bloodorb3 radial mask
	if ( !vkMode ) {
		backEnd.glState.tmu[1].current2DMap = -1;			// direct bind bypassed the tmu cache
	}
	RB_RHI_DrawFullscreen( r, accumProg, parms, 0 );
	r->EndPass();

	rhiHelltimeIdx  = readIdx;
	rhiHelltimeValid = true;
	rhiHelltimeLastTick = timeMs;

	rhi::ImageHandle trailImg = r->GetRenderTargetImage( rhiHelltimeTrailRT[writeIdx] );
	RB_RHI_BindRTImage( r, 1, trailImg );
	if ( !vkMode ) {
		backEnd.glState.tmu[1].current2DMap = -1;
	}
	return trailImg;
}

static bool RB_RHI_EnsureSsrTarget( rhi::RHI *r, int w, int h ) {
	return RB_RHI_EnsureRT( r, rhiSsrRT, rhiSsrW, rhiSsrH, rhi::IF_RGBA16F, w, h );
}

// SSR temporal history ping-pong; resolution changes invalidate the history so a
// re-enable / res switch starts clean instead of blending stale or mis-sized data.
static bool RB_RHI_EnsureSsrHistory( rhi::RHI *r, int w, int h ) {
	return RB_RHI_EnsureHistoryPair( r, rhiSsrHistRT, rhiSsrHistW, rhiSsrHistH,
			rhiSsrHistValid, rhi::IF_RGBA16F, w, h );
}

// RR6: (re)allocate the RT-reflection contribution target (this frame's ssr_rt output before temporal).
// RGBA16F to carry HDR reflected radiance; alpha holds the hit mask ssr_temporal's hit-aware feedback
// reads. Sized to the SSR march resolution so RT and SSR share a grain. Mirrors RB_RHI_EnsureSsrTarget.
static bool RB_RHI_EnsureRtReflTarget( rhi::RHI *r, int w, int h ) {
	return RB_RHI_EnsureRT( r, rhiRtReflRT, rhiRtReflW, rhiRtReflH, rhi::IF_RGBA16F, w, h );
}

// RR6: (re)allocate the RT-reflection temporal history ping-pong. Mirrors RB_RHI_EnsureSsrHistory —
// two RGBA16F slots, invalidated (rhiRtReflHistValid=false) on realloc so the first blend reprojects
// nothing and just passes the current frame through.
static bool RB_RHI_EnsureRtReflHistory( rhi::RHI *r, int w, int h ) {
	return RB_RHI_EnsureHistoryPair( r, rhiRtReflHistRT, rhiRtReflHistW, rhiRtReflHistH,
			rhiRtReflHistValid, rhi::IF_RGBA16F, w, h );
}

// SSR Hi-Z (docs/ssao-perf-optimization.md, r_ssrHiZ): (re)allocate the min-Z depth pyramid
// at the SSR march resolution. Mirrors RB_RHI_EnsureSsaoDepthMip exactly; returns false when
// the backend has no mipped-target capability (CreateRenderTargetMipped -> 0), so the caller
// silently falls back to the exact full-res march.
static bool RB_RHI_EnsureSsrDepthMin( rhi::RHI *r, int w, int h ) {
	// 6 levels leaps across the march radius in coarse blocks (1/32 footprint) without an
	// over-long chain.
	return RB_RHI_EnsureMippedRT( r, rhiSsrDepthMinRT, rhiSsrDepthMinW, rhiSsrDepthMinH,
			rhiSsrDepthMinLevels, rhi::IF_R16F, w, h, 6 );
}

// r_ssrGlossy: (re)create the reflection colour mip pyramid at the SSR march resolution.
// RGBA16F to preserve HDR reflected energy across the downsample. Returns false when the
// backend has no mipped-target capability (CreateRenderTargetMipped -> 0), so the caller
// falls back to the exact sharp composite. Mirrors RB_RHI_EnsureSsrDepthMin.
static bool RB_RHI_EnsureSsrColorMip( rhi::RHI *r, int w, int h ) {
	// 6 levels give a broad glossy blur (1/32 footprint ≈ mirror -> fully diffuse across
	// the roughness cutoff) without an over-long chain.
	return RB_RHI_EnsureMippedRT( r, rhiSsrColorMipRT, rhiSsrColorMipW, rhiSsrColorMipH,
			rhiSsrColorMipLevels, rhi::IF_RGBA16F, w, h, 6 );
}

// RR16: gather the lights that can illuminate the reflection into the ring buffer's current slot; return
// the count and point *outAddr at that slot's device address (0 = nothing to light with). We iterate the
// WHOLE render world's lightDefs, NOT viewDef->viewLights: the latter is frustum-culled, so a light whose
// volume doesn't reach the on-screen frustum is dropped even though the reflection can see a surface it
// lights (the user's "off-screen lights are evicted in the reflection"). Instead relevance-cull by distance
// so an off-screen light near the player still lights the reflection. (No shadow rays yet, so a light behind
// a wall can leak - that is the RR16d follow-up; the falloff radius bounds most of it.) Point-light model:
// world origin + reach radius + colour (constant-register lights: summed stage colour; the common
// shaderParm-driven lights: the base RGB parms). fog/blend/ambient cast no directional diffuse -> skipped.
// RR16c: the 4 world-space lightProject planes (S, T, Q, falloff) ride along so the shader can confine each
// light to its real volume - a Doom light only reaches inside its projection frustum (a spot's pyramid, a
// point light's BOX), which the origin+radius sphere over-reaches.
// cull[] = bounding sphere of the light volume, first in the row so the shader rejects a light that cannot
// reach the hit after reading 16 B (the full row per light per pixel cost ~1.5 ms).
struct RtReflLightGPU { float cull[4]; float pos[3]; float radius; float color[3]; unsigned int flags; float proj[4][4]; };	// std430, 112 B

// Shared by RT reflections (RR16) and volumetric lighting (docs/volumetric-lighting.md): the first caller in
// a view fills the slot, later callers in the same view get the cached address/count.
static int RB_RHI_GatherSceneLights( rhi::RHI *r, const viewDef_t *viewDef, unsigned long long *outAddr ) {
	static const viewDef_t *   cacheView  = NULL;
	static int                 cacheFrame = -1;
	static int                 cacheCount = 0;
	static unsigned long long  cacheAddr  = 0;
	// (s_sceneFogCount is part of the cached state: it is only rewritten when the slot is refilled below)
	if ( viewDef != NULL && cacheView == viewDef && cacheFrame == tr.frameCount && s_rtReflLightBuf != 0 ) {
		*outAddr = cacheAddr;
		return cacheCount;
	}
	cacheView = viewDef;	cacheFrame = tr.frameCount;	cacheCount = 0;	cacheAddr = 0;
	s_sceneFogCount = 0;
	*outAddr = 0;
	if ( viewDef == NULL || viewDef->renderWorld == NULL ) {
		return 0;
	}
	if ( s_rtReflLightBuf == 0 ) {
		s_rtReflLightBuf  = r->CreateBuffer( rhi::BU_STORAGE,
			RT_REFL_LIGHT_SLOTS * RT_REFL_MAX_LIGHTS * (int)sizeof( RtReflLightGPU ), NULL );
		s_rtReflLightAddr = ( s_rtReflLightBuf != 0 ) ? r->GetBufferDeviceAddress( s_rtReflLightBuf ) : 0;
	}
	if ( s_rtReflLightBuf == 0 || s_rtReflLightAddr == 0 ) {
		return 0;
	}
	const idRenderWorldLocal *rw = viewDef->renderWorld;
	const idVec3 viewOrg = viewDef->renderView.vieworg;
	const float  REACH   = 2048.0f;					// how far off-screen a light's reach may still touch the reflection
	static RtReflLightGPU lights[ RT_REFL_MAX_LIGHTS ];
	static RtReflLightGPU sorted[ RT_REFL_MAX_LIGHTS ];
	static float          sortKey[ RT_REFL_MAX_LIGHTS ];
	int n = 0;
	// Lights that are ALSO in this view carry evaluated shader registers: use them so a flickering /
	// strobing / fading light does the same in the reflection and in its volumetric shaft. Off-view lights
	// have no registers this frame and fall back to their static colour below.
	const int numDefs = rw->lightDefs.Num();
	const viewLight_t **viewLightOf = (const viewLight_t **)_alloca( ( numDefs + 1 ) * sizeof( viewLight_t * ) );
	memset( viewLightOf, 0, ( numDefs + 1 ) * sizeof( viewLight_t * ) );
	for ( const viewLight_t *vl = viewDef->viewLights; vl != NULL; vl = vl->next ) {
		if ( vl->lightDef != NULL && vl->lightDef->index >= 0 && vl->lightDef->index < numDefs ) {
			viewLightOf[ vl->lightDef->index ] = vl;
		}
	}
	static RtReflLightGPU fogs[ SCENE_FOG_MAX ];
	int nFog = 0;
	for ( int i = 0; i < rw->lightDefs.Num() && n < RT_REFL_MAX_LIGHTS - SCENE_FOG_MAX; i++ ) {
		const idRenderLightLocal *ld = rw->lightDefs[ i ];
		if ( ld == NULL ) { continue; }
		const idMaterial *ls = ld->lightShader;
		if ( ls != NULL && ls->IsFogLight() ) {
			// A stock fog volume: not a light, but volumetric lighting thickens its haze inside it (the
			// point-light scale rises toward r_volFogPointLightScale). Row = volume sphere + the box planes +
			// color[0] = strength from the fog's own opaque distance (RB_FogPass: alpha <= 1 means the
			// 500-unit default); thicker fog = stronger. Rows go AFTER the lights, outside the light count,
			// so RT reflections never see them.
			if ( nFog >= SCENE_FOG_MAX || ld->frustumTris == NULL || ld->frustumTris->bounds.IsCleared() ) { continue; }
			const idVec3 fc = ld->frustumTris->bounds.GetCenter();
			const float  fr = ld->frustumTris->bounds.GetRadius( fc );
			if ( ( fc - viewOrg ).LengthSqr() > ( fr + REACH ) * ( fr + REACH ) ) { continue; }
			float fogDist = ld->parms.shaderParms[ SHADERPARM_ALPHA ];
			if ( fogDist <= 1.0f ) { fogDist = 500.0f; }
			RtReflLightGPU &F = fogs[ nFog++ ];
			memset( &F, 0, sizeof( F ) );
			F.cull[0] = fc[0]; F.cull[1] = fc[1]; F.cull[2] = fc[2]; F.cull[3] = fr;
			F.color[0] = idMath::ClampFloat( 0.2f, 1.0f, 500.0f / fogDist );
			F.flags = 16u;											// bit4 = fog volume
			for ( int p = 0; p < 4; p++ ) {
				F.proj[p][0] = ld->lightProject[p][0];	F.proj[p][1] = ld->lightProject[p][1];
				F.proj[p][2] = ld->lightProject[p][2];	F.proj[p][3] = ld->lightProject[p][3];
			}
			continue;
		}
		if ( ls == NULL || ls->IsBlendLight() || ls->IsAmbientLight() ) { continue; }
		// honour the per-view light gating exactly like the frontend (tr_light.cpp): the player's THIRD-person
		// flashlight / muzzle flash is suppressed in their own view, first-person weapon lights exist only in
		// it. Skipping this put the body-attached world flashlight (it bobs with the walk cycle) in the buffer.
		const int viewID = viewDef->renderView.viewID;
		if ( ld->parms.suppressLightInViewID && ld->parms.suppressLightInViewID == viewID ) { continue; }
		if ( ld->parms.allowLightInViewID && ld->parms.allowLightInViewID != viewID ) { continue; }
		const float lr = ld->parms.lightRadius.Length();
		idVec3 cullOrg = ld->globalLightOrigin;					// bounding sphere of the real light volume (frustum
		float  cullRad = ( lr > 1.0f ) ? lr : 320.0f;			// hull when built: a spot does not use lightRadius)
		if ( ld->frustumTris != NULL && !ld->frustumTris->bounds.IsCleared() ) {
			cullOrg = ld->frustumTris->bounds.GetCenter();
			cullRad = ld->frustumTris->bounds.GetRadius( cullOrg );
		}
		const float reach = cullRad + REACH;					// the light's own reach can bring it into play
		if ( ( cullOrg - viewOrg ).LengthSqr() > reach * reach ) { continue; }	// too far to matter
		idVec3 col;
		const viewLight_t *vl = viewLightOf[ i ];
		const float *cregs = ( vl != NULL && vl->shaderRegisters != NULL ) ? vl->shaderRegisters : ls->ConstantRegisters();
		const int SCENE_LIGHT_MAX_REGS = 4096;		// == MAX_EXPRESSION_REGISTERS (ui/Window.h), not visible here
		if ( cregs == NULL && ls->GetNumRegisters() <= SCENE_LIGHT_MAX_REGS ) {
			// Not in the view's light list and not constant: evaluate the light shader here. "Not in the list"
			// does NOT mean off-screen - the frontend also DROPS a light whose stages evaluate to black
			// (tr_light.cpp), which is exactly a blinking light's off phase; falling back to its static
			// colour then lit the haze at full strength while the light was off.
			static float scratchRegs[ SCENE_LIGHT_MAX_REGS ];
			ls->EvaluateRegisters( scratchRegs, ld->parms.shaderParms, viewDef, ld->parms.referenceSound );
			cregs = scratchRegs;
		}
		if ( cregs != NULL ) {						// evaluated (in-view) or constant registers: sum the live stage colours
			col.Zero();
			for ( int s = 0; s < ls->GetNumStages(); s++ ) {
				const shaderStage_t *st = ls->GetStage( s );
				if ( st == NULL || cregs[ st->conditionRegister ] == 0.0f ) { continue; }
				col[0] += Max( cregs[ st->color.registers[0] ], 0.0f );
				col[1] += Max( cregs[ st->color.registers[1] ], 0.0f );
				col[2] += Max( cregs[ st->color.registers[2] ], 0.0f );
			}
		} else {									// parametric (the usual case): the light's base RGB shader parms
			col.Set( ld->parms.shaderParms[SHADERPARM_RED], ld->parms.shaderParms[SHADERPARM_GREEN], ld->parms.shaderParms[SHADERPARM_BLUE] );
		}
		if ( col[0] < 0.001f && col[1] < 0.001f && col[2] < 0.001f ) { continue; }	// dark / off (the frontend's own threshold)
		// order: projected/parallel (the authored feature lights) first, then nearest first - consumers that
		// keep only a short candidate list per ray (volumetrics) must not lose a spot to a crowd of fill lights
		sortKey[ n ] = ( ( ld->parms.pointLight && !ld->parms.parallel ) ? 1.0e9f : 0.0f )
		             + Max( ( cullOrg - viewOrg ).Length() - cullRad, 0.0f );
		RtReflLightGPU &L = lights[ n++ ];
		L.cull[0] = cullOrg[0]; L.cull[1] = cullOrg[1]; L.cull[2] = cullOrg[2]; L.cull[3] = cullRad;
		L.pos[0] = ld->globalLightOrigin[0];
		L.pos[1] = ld->globalLightOrigin[1];
		L.pos[2] = ld->globalLightOrigin[2];
		L.radius = ( lr > 1.0f ) ? lr : 320.0f;					// a sane reach for degenerate/zero radii
		L.color[0] = col[0]; L.color[1] = col[1]; L.color[2] = col[2];
		L.flags = ( ld->parms.parallel ? 1u : 0u )				// bit0 = parallel/sun (no distance falloff)
		        | ( ld->parms.pointLight ? 0u : 2u )			// bit1 = projected (spot) light: falloff runs along the frustum axis
		        | ( ld->parms.noShadows ? 4u : 0u )				// bit2 = casts no shadows in stock: volumetrics skip its visibility ray
		        | ( ld->parms.allowLightInViewID ? 8u : 0u );	// bit3 = first-person weapon light (flashlight, muzzle flash)
		{
			// bits 8..15 = volumetric haze scale (0..255). A HUGE light (a cave-wide "sky glow", an outdoor
			// fill) is ambience, not a shaft: integrated over its whole volume it washes the view in its own
			// colour. Its haze fades with size beyond r_volLargeLightRadius; room-scale lights stay at 1.
			const float ref = r_volLargeLightRadius.GetFloat();
			float vs = ( ref > 0.0f && cullRad > ref ) ? ( ref / cullRad ) : 1.0f;
			vs *= vs;
			L.flags |= ( (unsigned int)idMath::ClampInt( 0, 255, (int)( vs * 255.0f + 0.5f ) ) ) << 8;
		}
		for ( int p = 0; p < 4; p++ ) {							// RR16c: global-space S/T/Q/falloff planes
			L.proj[p][0] = ld->lightProject[p][0];
			L.proj[p][1] = ld->lightProject[p][1];
			L.proj[p][2] = ld->lightProject[p][2];
			L.proj[p][3] = ld->lightProject[p][3];
		}
	}
	if ( n == 0 ) {
		return 0;
	}
	int order[ RT_REFL_MAX_LIGHTS ];
	for ( int i = 0; i < n; i++ ) {					// insertion sort of indices by key (n <= 256, mostly a handful)
		int j = i;
		while ( j > 0 && sortKey[ order[ j - 1 ] ] > sortKey[ i ] ) { order[ j ] = order[ j - 1 ]; j--; }
		order[ j ] = i;
	}
	for ( int i = 0; i < n; i++ ) { sorted[ i ] = lights[ order[ i ] ]; }
	for ( int i = 0; i < nFog; i++ ) { sorted[ n + i ] = fogs[ i ]; }		// fog volumes ride behind the lights
	s_sceneFogCount = nFog;
	const int slot      = (int)( tr.frameCount % RT_REFL_LIGHT_SLOTS );
	const int slotBytes = RT_REFL_MAX_LIGHTS * (int)sizeof( RtReflLightGPU );
	r->UpdateBuffer( s_rtReflLightBuf, slot * slotBytes, ( n + nFog ) * (int)sizeof( RtReflLightGPU ), sorted );
	*outAddr = s_rtReflLightAddr + (unsigned long long)( slot * slotBytes );
	cacheCount = n;	cacheAddr = *outAddr;
	return n;
}

/*
===================
RB_RHI_ScreenSpaceReflections

DUDE screen-space reflections (docs/ssr.md, PBR Phase C.2/C.2.1). Called by
RB_RHI_DrawView at the translucent split point: lit opaque geometry, emissive
panels/screens and decals are already down, translucents will draw over the result.
Three stages:
  1. march (ssr.frag) into an offscreen buffer at r_ssrResScale of the view;
  2. optional temporal accumulation (ssr_temporal.frag, r_ssrTemporal) blending
     against the camera-reprojected previous frame — resolves the march grain;
  3. full-resolution additive composite (ssr_composite.frag) applying the
     Fresnel/gloss/intensity weight from the G-buffer, so a low-res march only
     softens the reflected image, never the material response.
Fullscreen primary views only — the depth capture, G-buffer and screen mapping all
assume the whole framebuffer at the origin (same rule as SSAO).
===================
*/
// General 4x4 inverse (column-major float[16], the GL matrix layout in/out). Returns false on a
// singular matrix. RR2 uses it to invert the view matrix (view->world) so the RT reflection shader
// can lift a reconstructed view-space hit back to the world-space TLAS. Standard cofactor expansion.
static bool RB_RHI_InvertMatrix( const float m[16], float invOut[16] ) {
	float inv[16];
	inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
	inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
	inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
	inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
	inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
	inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
	inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
	inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
	inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
	inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
	inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
	inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
	inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
	inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
	inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
	inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];
	float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
	if ( det == 0.0f ) {
		return false;
	}
	det = 1.0f / det;
	for ( int i = 0; i < 16; i++ ) {
		invOut[i] = inv[i] * det;
	}
	return true;
}

// Radical-inverse Halton sample in [0,1). RR6c uses Halton(2,3) as the per-frame sub-texel jitter for
// the RT-reflection temporal upscale — a low-discrepancy 2D walk covers the sample grid evenly over a
// short cycle (the same family the R1 FSR jitter uses).
static float RB_RHI_Halton( int index, int base ) {
	float f = 1.0f, r = 0.0f;
	while ( index > 0 ) {
		f /= (float)base;
		r += f * (float)( index % base );
		index /= base;
	}
	return r;
}

void RB_RHI_ScreenSpaceReflections( rhi::RHI *r, const viewDef_t *viewDef ) {
	if ( !R_BackendSupportsEnhancements() ) {
		return;
	}
	// M7: SSR runs on both backends now. On Vulkan the G-buffer / scene-copy binds
	// route through RB_RHI_BindRTUnit/Image into rhiVkUnits (like SSAO) instead of
	// raw GL multitexture, and the shaders carry a view-Y sign + capture row-flip
	// for VK's top-down framebuffer vs the GL-layout _currentRender snapshot.
	const bool vkMode = ( rhi::GetActiveBackendType() == rhi::BT_VULKAN );
	// RR7/#4a: RT reflections (VK + ray-query) REPLACE the SSR composite rather than riding it, and when RT
	// will render this frame the entire SSR march chain (Hi-Z, scene snapshot, march, temporal, glossy) is
	// SKIPPED — post-RR7 ssr_rt traces every reflective pixel itself and reads none of the march's output, so
	// that whole pass is dead work under RT (no double-reflection, no fractional-res strobing). rtWants is the
	// intent; rtWillRender (computed below, before the march) also checks TLAS/targets so a misconfigured RT
	// (no TLAS) with r_ssr on cleanly falls back to the full SSR path.
	// Decoupled from r_ssr 2026-09-23: RT reflections no longer RIDE the SSR pass — they own every reflective
	// pixel, so the pass runs when EITHER SSR or RT wants it, and RT works with r_ssr off entirely. The shared
	// G-buffer/depth RT needs is built by the normal prepass (which now also runs for RT, not just r_ssr).
	const bool rtWants = vkMode && r_rtReflections.GetBool() && r->SupportsRayQuery();
	if ( !r_ssr.GetBool() && !rtWants ) {
		return;
	}
	if ( vkMode && r_ssr.GetBool() ) {
		static bool ssrLiveLogged = false;
		if ( !ssrLiveLogged ) {
			ssrLiveLogged = true;
			common->Printf( "RHI backend: VK SSR (r_ssr) live - G-buffer march + composite\n" );
		}
	}
	if ( !viewDef->viewEntitys || viewDef->isSubview ) {
		return;
	}
	const bool fullscreenView = viewDef->isWindowView;	// the primary scene view (set by RenderScene)
	if ( !fullscreenView ) {
		return;
	}
	// needs this view's MRT G-buffer (normals + rough/metal) and captured depth. The result
	// handle is the standalone rhiNormalRT or, under r_ssaoMergeNormal, the merged handle —
	// both expose GetRenderTargetImage (normal) + GetRenderTargetImage2 (rough/metal).
	if ( !rhiNormalReadyThisView || !rhiNormalMrtThisView || rhiNormalResultRT == 0 ) {
		return;
	}
	// rough/metal as the decals left it (dirt is no mirror); the raw attachment when none drew
	const rhi::ImageHandle matImg = RB_RHI_DecalResolveMaterial( r, r->GetRenderTargetImage2( rhiNormalResultRT ),
		viewDef->viewport.x2 - viewDef->viewport.x1 + 1, viewDef->viewport.y2 - viewDef->viewport.y1 + 1 );
	// depth capture: uploadWidth on GL; rhiCaptured on VK (a demand-load can set
	// uploadWidth there without a real capture), matching the soft-particle idiom
	const bool depthCaptured = vkMode ? globalImages->currentDepthImage->rhiCaptured
	                                  : globalImages->currentDepthImage->uploadWidth > 0;
	if ( matImg == 0 || !depthCaptured ) {
		return;
	}
	rhi::ShaderHandle marchProg = r->LoadShader( "ssr" );
	rhi::ShaderHandle compProg  = r->LoadShader( "ssr_composite" );
	if ( !marchProg || !compProg ) {
		return;
	}

	const int fullW = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int fullH = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
	// Snap the reflection resolution to an INTEGER DIVISOR of the view (full, 1/2, 1/4). A fractional ratio
	// (2/3, 3/4) makes the low-res reflection upscale BEAT into strobing/stripes that even the RT temporal
	// upscale doesn't fully resolve (user-confirmed 2026-09-20), so both paths only ever pick clean 2:1 / 4:1
	// divisors. RT uses its OWN scale (r_rtReflResScale) so the trace resolution is independent of the SSR
	// march resolution (r_ssrResScale); everything else (targets, temporal upscale to full res) follows ssrW/H.
	const float resScale = idMath::ClampFloat( 0.25f, 1.0f,
		rtWants ? r_rtReflResScale.GetFloat() : r_ssrResScale.GetFloat() );
	const int resDiv = ( resScale >= 0.75f ) ? 1 : ( resScale >= 0.375f ) ? 2 : 4;
	int ssrW = fullW / resDiv;
	int ssrH = fullH / resDiv;
	if ( ssrW < 1 ) ssrW = 1;
	if ( ssrH < 1 ) ssrH = 1;
	if ( !RB_RHI_EnsureSsrTarget( r, ssrW, ssrH ) ) {
		return;
	}

	const int uploadW = globalImages->currentDepthImage->uploadWidth;
	const int uploadH = globalImages->currentDepthImage->uploadHeight;
	const float invP00 = ( viewDef->projectionMatrix[0] != 0.0f ) ? 1.0f / viewDef->projectionMatrix[0] : 1.0f;
	const float invP11 = ( viewDef->projectionMatrix[5] != 0.0f ) ? 1.0f / viewDef->projectionMatrix[5] : 1.0f;
	const float viewYSign = vkMode ? -1.0f : 1.0f;		// +1 GL (bottom-up) / -1 VK (top-down FB): flips reconstructed Y

	// #4a: decide RT-vs-SSR BEFORE the march so the whole SSR march chain below can be gated off when RT
	// replaces the composite (post-RR7 ssr_rt reads none of the march's output — it's dead work under RT).
	// Preconditions mirror the old post-march front-load (TLAS/geo device addrs, RT target, invertible view);
	// all inputs are available here (ssrW/ssrH set + EnsureSsrTarget already ran). If RT is enabled but can't
	// run, rtWillRender = false and the SSR path runs exactly as before.
	const unsigned long long tlasAddr = rtWants ? r->GetTlasAddress() : 0;
	const unsigned long long geoAddr  = rtWants ? r->GetRtGeoTableAddress() : 0;
	rhi::ShaderHandle rtProg   = ( rtWants && tlasAddr != 0 && geoAddr != 0 ) ? r->LoadShader( "ssr_rt" ) : 0;
	rhi::ShaderHandle copyProg = rtProg ? r->LoadShader( "ssr_rt_composite" ) : 0;	// RR6 additive re-composite (+blur)
	float invView[16];
	const bool rtWillRender = ( rtProg != 0 && copyProg != 0
		&& RB_RHI_EnsureRtReflTarget( r, ssrW, ssrH )
		&& RB_RHI_InvertMatrix( viewDef->worldSpace.modelViewMatrix, invView ) );

	// ==== SSR march chain + composite: only when screen-space SSR is enabled AND RT isn't replacing it (#4a).
	// Gated on r_ssr so RT-only mode (r_ssr off, r_rtReflections on) never runs the march even on a frame RT
	// can't render (no TLAS yet) — that frame simply has no reflection, no stray SSR. Interior keeps its
	// indentation to match the existing composite-gate style below. ====
	if ( r_ssr.GetBool() && !rtWillRender ) {

	// ---- Hi-Z (r_ssrHiZ, docs/ssao-perf-optimization.md): build a min-Z (nearest-surface)
	// linear-depth pyramid at the SSR march resolution so the march can leap provably-empty
	// span (see ssr.frag hiZMin). Mirrors the SSAO depth-mip build with a MIN downsample.
	// Done BEFORE the scene snapshot below so the pyramid passes' unit-0 binds are then
	// re-established as _currentRender by CopyFramebuffer (the GL march samples it on unit 0). ----
	bool doSsrHiZ  = false;
	int  ssrHiZLod = 0;
	if ( r_ssrHiZ.GetBool() ) {
		if ( RB_RHI_EnsureSsrDepthMin( r, ssrW, ssrH ) ) {
			rhi::ShaderHandle minProg  = r->LoadShader( "ssr_depthmin" );	// linearize into level 0
			rhi::ShaderHandle downProg = r->LoadShader( "ssr_depthdown" );	// min-downsample the chain
			doSsrHiZ = ( minProg != 0 && downProg != 0 && rhiSsrDepthMinLevels >= 2 );
			if ( doSsrHiZ ) {
				rhi::RenderParams pyr;
				memset( &pyr, 0, sizeof( pyr ) );
				pyr.mvpMatrix[0] = pyr.mvpMatrix[5] = pyr.mvpMatrix[10] = pyr.mvpMatrix[15] = 1.0f;
				pyr.depthTexRecip[0] = ( (float)fullW / ssrW ) / uploadW;	// SSR frag -> depth tc
				rhi::FillDepthParms( pyr, backEnd.viewDef ? backEnd.viewDef->projectionMatrix : NULL );	// this view's depth -> view z pair (cinematics cram the near plane)
				pyr.depthTexRecip[1] = ( (float)fullH / ssrH ) / uploadH;
				// level 0: linearize _currentDepth into the min-Z target
				r->BeginTargetPass( rhiSsrDepthMinRT, NULL );
				RB_RHI_BindUnit( 0, globalImages->currentDepthImage );
				RB_RHI_DrawFullscreen( r, minProg, pyr, 0 );
				r->EndPass();
				// levels 1..N-1: MIN (nearest) downsample from the previous level. localParam0.x
				// = the source mip level (VK binds a single-level view -> 0; GL binds the whole
				// texture -> the real level for texelFetch), exactly like ssao_depthdown.
				const bool vkBackend = ( rhi::GetActiveBackendType() == rhi::BT_VULKAN );
				for ( int L = 1; L < rhiSsrDepthMinLevels; L++ ) {
					rhi::RenderParams dp = pyr;
					dp.localParam0[0] = vkBackend ? 0.0f : (float)( L - 1 );
					rhi::ImageHandle src = r->GetRenderTargetMipImage( rhiSsrDepthMinRT, L - 1 );
					r->BeginTargetMipPass( rhiSsrDepthMinRT, L, NULL );
					RB_RHI_DrawFullscreen( r, downProg, dp, src );
					r->EndPass();
				}
				backEnd.glState.tmu[0].current2DMap = -1;	// direct binds bypassed the tmu cache
				ssrHiZLod = idMath::ClampInt( 1, rhiSsrDepthMinLevels - 1, r_ssrHiZLevel.GetInteger() );
			}
		}
	} else if ( rhiSsrDepthMinRT ) {
		// toggled off: reclaim the target so it isn't left resident (mirrors SSAO)
		r->DestroyRenderTarget( rhiSsrDepthMinRT );
		rhiSsrDepthMinRT = 0;
		rhiSsrDepthMinW = rhiSsrDepthMinH = rhiSsrDepthMinLevels = 0;
	}

	// snapshot the lit opaque scene; on GL CopyFramebuffer leaves _currentRender bound
	// on the active unit — exactly where ssr.frag samples it (unit 0), and re-establishes it
	// after the Hi-Z build above bound depth there. On VK the copy routes through the RHI
	// capture path and unit 0 is recorded via rhiVkUnits below, so skip the raw
	// gl3ActiveTexture (a NULL qgl pointer on the Vulkan backend).
	if ( !vkMode ) {
		rhi::gl3ActiveTexture( GL_TEXTURE0 );
		backEnd.glState.currenttmu = 0;
	}
	globalImages->currentRenderImage->CopyFramebuffer( viewDef->viewport.x1,
		viewDef->viewport.y1, fullW, fullH, true );

	// ---- stage 1: march into the offscreen buffer (cleared to 0 = miss) ----
	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	// the real view->clip matrix projects march points back to screen; the inverse
	// factors reconstruct view-space positions (same recipe as ssao.frag)
	memcpy( parms.projectionMatrix, viewDef->projectionMatrix, sizeof( parms.projectionMatrix ) );
	parms.localParam0[0] = invP00;
	parms.localParam0[1] = invP11;
	parms.localParam0[2] = idMath::ClampFloat( 64.0f, 8192.0f, r_ssrMaxDistance.GetFloat() );
	parms.localParam0[3] = idMath::ClampFloat( 1.0f, 256.0f, r_ssrThickness.GetFloat() );
	parms.localParam1[0] = (float)idMath::ClampInt( 4, 64, r_ssrSteps.GetInteger() );
	parms.localParam1[2] = idMath::ClampFloat( 0.02f, 1.0f, r_ssrMaxRoughness.GetFloat() );
	parms.screenCorrection[0] = 1.0f / ssrW;
	parms.screenCorrection[1] = 1.0f / ssrH;
	const int potW = globalImages->currentRenderImage->uploadWidth;
	const int potH = globalImages->currentRenderImage->uploadHeight;
	parms.screenCorrection[2] = potW > 0 ? (float)fullW / potW : 1.0f;
	parms.screenCorrection[3] = potH > 0 ? (float)fullH / potH : 1.0f;
	parms.depthTexRecip[0] = ( (float)fullW / ssrW ) / uploadW;		// gl_FragCoord (SSR) -> depth tc
	rhi::FillDepthParms( parms, backEnd.viewDef ? backEnd.viewDef->projectionMatrix : NULL );	// this view's depth -> view z pair (cinematics cram the near plane)
	parms.depthTexRecip[1] = ( (float)fullH / ssrH ) / uploadH;
	// per-frame jitter rotation so temporal accumulation averages different march
	// offsets (golden-ratio walk, same scheme as SSAO); 0 keeps the static dither
	if ( r_ssrTemporal.GetBool() ) {
		rhiSsrJitterPhase += 0.61803399f;
		rhiSsrJitterPhase -= (float)(int)rhiSsrJitterPhase;
		parms.windowCoord[1] = rhiSsrJitterPhase;
	}
	// view-Y sign (u_windowCoord.z): +1 on GL (gl_FragCoord.y bottom-up, agrees with
	// view +Y and the bottom-up _currentRender capture), -1 on Vulkan (top-down
	// framebuffer). Flips the reconstructed view-space Y and the project-to-screen /
	// capture-sample rows so the march matches the G-buffer normals (same idea as
	// ssao.frag's u_windowCoord.z). Inert at +1 on GL.
	parms.windowCoord[2] = viewYSign;		// viewYSign hoisted above the #4a guard
	// Hi-Z leap LOD for ssr.frag (u_localParam1.w); 0 = feature off -> exact full-res march
	parms.localParam1[3] = (float)ssrHiZLod;

	rhi::ClearArgs clear;
	memset( &clear, 0, sizeof( clear ) );
	clear.color = true;		// rgba 0 = miss everywhere the march discards

	r->BeginTargetPass( rhiSsrRT, &clear );
	// unit 0 = _currentRender; unit 1 = depth; units 2/3 = the G-buffer attachments.
	// GL: the copy above left _currentRender bound on unit 0, units 2/3 raw-bind (tmu
	// cache entries invalidated below). VK: record all four into rhiVkUnits so
	// RB_RHI_DrawFullscreen carries them via DrawArgs.
	if ( vkMode ) {
		RB_RHI_BindRTImage( r, 0, globalImages->currentRenderImage->rhiHandle );
	}
	RB_RHI_BindUnit( 1, globalImages->currentDepthImage );
	if ( vkMode ) {
		RB_RHI_BindRTUnit( r, 2, rhiNormalResultRT );
		RB_RHI_BindRTImage( r, 3, matImg );
	} else {
		rhi::gl3ActiveTexture( GL_TEXTURE0 + 2 );
		qglBindTexture( GL_TEXTURE_2D, (GLuint)r->GetRenderTargetImage( rhiNormalResultRT ) );
		rhi::gl3ActiveTexture( GL_TEXTURE0 + 3 );
		qglBindTexture( GL_TEXTURE_2D, (GLuint)matImg );
		rhi::gl3ActiveTexture( GL_TEXTURE0 );
		backEnd.glState.currenttmu = 0;
		backEnd.glState.tmu[2].current2DMap = -1;
		backEnd.glState.tmu[3].current2DMap = -1;
	}
	// Hi-Z min-Z pyramid on unit 4 for the march's leap test. When off, bind _currentDepth
	// as an unused dummy so the descriptor slot stays valid on VK (ssr.frag routes to the
	// exact march via u_localParam1.w = 0 and never samples it). Mirrors the SSAO unit-2 idiom.
	if ( doSsrHiZ ) {
		RB_RHI_BindRTUnit( r, 4, rhiSsrDepthMinRT );
		if ( !vkMode ) {
			backEnd.glState.tmu[4].current2DMap = -1;	// direct bind bypassed the tmu cache
		}
	} else {
		RB_RHI_BindUnit( 4, globalImages->currentDepthImage );
	}
	RB_RHI_DrawFullscreen( r, marchProg, parms, 0 );
	r->EndPass();

	rhi::RenderTargetHandle resultRT = rhiSsrRT;

	// ---- stage 2: temporal accumulation (docs/ssr.md; mirrors the SSAO history) ----
	if ( r_ssrTemporal.GetBool() && RB_RHI_EnsureSsrHistory( r, ssrW, ssrH ) ) {
		rhi::ShaderHandle tempProg = r->LoadShader( "ssr_temporal" );
		if ( tempProg ) {
			// reproject last frame's reflection by camera motion; the shared camera state
			// (docs/fsr-temporal-pipeline.md A1) advances prev<-cur once per rendered frame
			float reproj[16];
			bool prevUsable;
			const bool haveInv = RB_RHI_TemporalReproj( viewDef, reproj, &prevUsable );
			const bool historyUsable = rhiSsrHistValid && prevUsable && haveInv;

			const int writeIdx = rhiSsrHistIdx;
			const int readIdx  = 1 - rhiSsrHistIdx;

			rhi::RenderParams tempParms = parms;
			if ( haveInv ) {
				memcpy( tempParms.modelViewMatrix, reproj, sizeof( reproj ) );
			}
			tempParms.localParam0[2] = idMath::ClampFloat( 0.0f, 0.97f, r_ssrTemporalFeedback.GetFloat() );
			tempParms.localParam0[3] = historyUsable ? 1.0f : 0.0f;
			tempParms.localParam1[1] = 1.0f;		// history == march resolution: clip taps one texel apart
			tempParms.localParam1[2] = tempParms.localParam1[3] = 0.0f;	// no sample-grid jitter to undo

			// R1/A2 consumption: reproject the reflection history by the per-object velocity buffer
			// when it exists (r_motionVectors, VK) so moving objects stop dragging a reflection
			// ghost. Falls back to the camera-only matrix reproj (localParam1.x = 0).
			rhi::ImageHandle velImg = ( rhiNormalVelThisView && rhiNormalReadyThisView )
				? r->GetRenderTargetImage3( rhiNormalResultRT ) : 0;
			tempParms.localParam1[0] = velImg ? 1.0f : 0.0f;

			// unit 0 = current march (via DrawFullscreen), unit 1 = history read slot,
			// unit 2 = depth, unit 3 = velocity (R1/A2)
			r->BeginTargetPass( rhiSsrHistRT[writeIdx], NULL );
			RB_RHI_BindUnit( 2, globalImages->currentDepthImage );
			if ( vkMode ) {
				// First frame after (re)alloc the read slot was never rendered, so its
				// real layout is still UNDEFINED (the color target's tracker claims
				// SHADER_READ_ONLY only after a write). The shader early-outs on
				// historyUsable=0, but u_history is a statically-used sampler that VK
				// validates regardless — bind the just-marched result (a written,
				// SHADER_READ_ONLY target) until a real history slot exists.
				rhi::RenderTargetHandle histRT = historyUsable ? rhiSsrHistRT[readIdx] : rhiSsrRT;
				RB_RHI_BindRTUnit( r, 1, histRT );			// history read (VK rhiVkUnits[1])
			} else {
				rhi::gl3ActiveTexture( GL_TEXTURE0 + 1 );
				qglBindTexture( GL_TEXTURE_2D, (GLuint)r->GetRenderTargetImage( rhiSsrHistRT[readIdx] ) );
				rhi::gl3ActiveTexture( GL_TEXTURE0 );
				backEnd.glState.currenttmu = 0;
				backEnd.glState.tmu[1].current2DMap = -1;	// direct bind bypassed the tmu cache
			}
			// unit 3 = velocity MRT, or a dummy (u_velocity is a statically-used sampler VK validates)
			if ( velImg ) { RB_RHI_BindRTImage( r, 3, velImg ); }
			else          { RB_RHI_BindUnit( 3, globalImages->currentDepthImage ); }
			RB_RHI_DrawFullscreen( r, tempProg, tempParms, r->GetRenderTargetImage( rhiSsrRT ) );
			r->EndPass();

			resultRT = rhiSsrHistRT[writeIdx];
			rhiSsrHistIdx = readIdx;
			RB_RHI_TemporalStageCur( viewDef );
			rhiSsrHistValid  = true;
		}
	} else {
		// temporal off (or history alloc failed): drop any stale history so a later
		// re-enable starts clean instead of blending garbage
		rhiSsrHistValid = false;
	}

	// ---- stage 2.5: glossy reflection pyramid (r_ssrGlossy, docs/ssr.md) ----
	// Build a colour mip pyramid of the reflection result so the composite can read it at a
	// roughness-proportional LOD (rough surfaces blur, sharp stay sharp). The sharp path is
	// left exactly as before: glossySrcRT stays the single-level resultRT and glossyMaxLod 0
	// tells ssr_composite.frag to sample level 0 with plain texture().
	rhi::RenderTargetHandle glossySrcRT = resultRT;
	float glossyMaxLod = 0.0f;
	if ( r_ssrGlossy.GetBool() && RB_RHI_EnsureSsrColorMip( r, ssrW, ssrH ) ) {
		rhi::ShaderHandle downProg = r->LoadShader( "ssr_colordown" );
		if ( downProg && rhiSsrColorMipLevels >= 2 ) {
			rhi::RenderParams cp;
			memset( &cp, 0, sizeof( cp ) );
			cp.mvpMatrix[0] = cp.mvpMatrix[5] = cp.mvpMatrix[10] = cp.mvpMatrix[15] = 1.0f;
			// level 0: 1:1 copy of the reflection result into the pyramid base (mode 0).
			// Level 0 uses BeginTargetPass (its framebuffer is the base colorFb) — NOT
			// BeginTargetMipPass, which only creates per-level framebuffers for L>=1 and
			// rejects level 0 (a no-op that would leave the base black). Mirrors the Hi-Z
			// build, whose level-0 linearize also uses BeginTargetPass.
			cp.localParam0[0] = 0.0f;	// single-level source -> level 0
			cp.localParam0[1] = 0.0f;	// mode 0 = copy
			r->BeginTargetPass( rhiSsrColorMipRT, NULL );
			RB_RHI_DrawFullscreen( r, downProg, cp, r->GetRenderTargetImage( resultRT ) );
			r->EndPass();
			// levels 1..N: 2x2 box average of the previous level (mode 1). localParam0.x =
			// source level (VK binds a single-level view -> 0; GL binds the whole texture).
			const bool vkBackend = ( rhi::GetActiveBackendType() == rhi::BT_VULKAN );
			for ( int L = 1; L < rhiSsrColorMipLevels; L++ ) {
				rhi::RenderParams dp = cp;
				dp.localParam0[0] = vkBackend ? 0.0f : (float)( L - 1 );
				dp.localParam0[1] = 1.0f;	// mode 1 = downsample
				rhi::ImageHandle src = r->GetRenderTargetMipImage( rhiSsrColorMipRT, L - 1 );
				r->BeginTargetMipPass( rhiSsrColorMipRT, L, NULL );
				RB_RHI_DrawFullscreen( r, downProg, dp, src );
				r->EndPass();
			}
			if ( !vkMode ) {
				backEnd.glState.tmu[0].current2DMap = -1;	// direct binds bypassed the tmu cache
			}
			glossySrcRT  = rhiSsrColorMipRT;
			// max LOD the composite may reach at the roughness cutoff, scaled by r_ssrGlossyScale
			glossyMaxLod = (float)( rhiSsrColorMipLevels - 1 )
			             * idMath::ClampFloat( 0.0f, 1.0f, r_ssrGlossyScale.GetFloat() );
		}
	} else if ( rhiSsrColorMipRT && !r_ssrGlossy.GetBool() ) {
		// toggled off: reclaim the pyramid so it isn't left resident (mirrors the Hi-Z reclaim)
		r->DestroyRenderTarget( rhiSsrColorMipRT );
		rhiSsrColorMipRT = 0;
		rhiSsrColorMipW = rhiSsrColorMipH = rhiSsrColorMipLevels = 0;
	}

	// ---- stage 3: full-res additive composite over the lit scene (inside the #4a !rtWillRender guard) ----
	rhi::RenderParams compParms;
	memset( &compParms, 0, sizeof( compParms ) );
	compParms.mvpMatrix[0] = compParms.mvpMatrix[5] = compParms.mvpMatrix[10] = compParms.mvpMatrix[15] = 1.0f;
	compParms.localParam0[0] = invP00;
	compParms.localParam0[1] = invP11;
	compParms.localParam0[2] = idMath::ClampFloat( 0.05f, 0.95f, r_ssrRoughnessFade.GetFloat() );	// roughness fade start
	compParms.localParam0[3] = idMath::ClampFloat( 0.0f, 64.0f, r_ssrFireflyClamp.GetFloat() );	// reflected-radiance cap (fireflies)
	compParms.localParam1[1] = r_ssrIntensity.GetFloat();
	compParms.localParam1[2] = idMath::ClampFloat( 0.02f, 1.0f, r_ssrMaxRoughness.GetFloat() );
	compParms.localParam1[3] = glossyMaxLod;	// r_ssrGlossy: >0 = sample the pyramid by roughness; 0 = sharp
	// RR14 specular occlusion strength (localParam1.x): 0 when no AO ran this view so ssr_composite is a no-op
	compParms.localParam1[0] = ( rhiSsaoAppliedThisView ? idMath::ClampFloat( 0.0f, 1.0f, r_specOcclusion.GetFloat() ) : 0.0f );
	compParms.screenCorrection[0] = 1.0f / fullW;
	compParms.screenCorrection[1] = 1.0f / fullH;
	compParms.depthTexRecip[0] = 1.0f / uploadW;
	rhi::FillDepthParms( compParms, backEnd.viewDef ? backEnd.viewDef->projectionMatrix : NULL );	// this view's depth -> view z pair (cinematics cram the near plane)
	compParms.depthTexRecip[1] = 1.0f / uploadH;
	compParms.windowCoord[2] = viewYSign;		// view-Y sign for the NdotV reconstruction (VK)

	// EndPass restored the view scissor; keep the CPU cache in step, and cover the
	// whole view in case the last surface left a crop before the target passes
	r->SetScissor( tr.viewportOffset[0] + viewDef->viewport.x1,
	               tr.viewportOffset[1] + viewDef->viewport.y1, fullW, fullH );
	backEnd.currentScissor = viewDef->scissor;

	// unit 0 = SSR result (via DrawFullscreen, a color target -> VK cancels the scene
	// flip), unit 1 = depth, units 2/3 = G-buffer
	RB_RHI_BindUnit( 1, globalImages->currentDepthImage );
	if ( vkMode ) {
		RB_RHI_BindRTUnit( r, 2, rhiNormalResultRT );
		RB_RHI_BindRTImage( r, 3, matImg );
	} else {
		rhi::gl3ActiveTexture( GL_TEXTURE0 + 2 );
		qglBindTexture( GL_TEXTURE_2D, (GLuint)r->GetRenderTargetImage( rhiNormalResultRT ) );
		rhi::gl3ActiveTexture( GL_TEXTURE0 + 3 );
		qglBindTexture( GL_TEXTURE_2D, (GLuint)matImg );
		rhi::gl3ActiveTexture( GL_TEXTURE0 );
		backEnd.glState.currenttmu = 0;
		backEnd.glState.tmu[2].current2DMap = -1;
		backEnd.glState.tmu[3].current2DMap = -1;
	}
	// RR14: AO buffer on unit 4 for specular occlusion. When AO ran this view bind it (roughness-aware
	// dim in ssr_composite.frag); otherwise bind white so ao = 1 leaves the reflection unchanged.
	if ( rhiSsaoAppliedThisView ) {
		RB_RHI_BindRTUnit( r, 4, rhiSsaoResultRT );
		if ( !vkMode ) { backEnd.glState.tmu[4].current2DMap = -1; }	// direct bind bypassed the tmu cache (mirrors the Hi-Z idiom above)
	} else {
		RB_RHI_BindUnit( 4, globalImages->whiteImage );
	}
	RB_RHI_DrawFullscreen( r, compProg, compParms, r->GetRenderTargetImage( glossySrcRT ),
	                       GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );
	}	// end #4a guard — entire SSR march chain + composite skipped when RT replaces it

	// ---- RR2/RR6/RR7: RT reflections on monsters + world (docs/rtx-reflections.md) ----
	// RT traces EVERY reflective pixel itself (RR7: no SSR miss-gate — the gate made RT inherit SSR's
	// fractional-res coverage strobing/striping) and shades the hit from its gpuSkinVB (monster) / world
	// attributes. The contribution renders into its OWN target (a = hit mask), is temporally accumulated
	// and UPSCALED to full res (RR6a/c) so it neither fireflies nor stripes, then composited additively —
	// REPLACING the SSR composite above. Preconditions were front-loaded into rtWillRender, so tlasAddr /
	// geoAddr / rtProg / copyProg / invView are ready.
	if ( rtWillRender ) {
		// item 6: the _currentRender snapshot #4a kept here (a safety net for downstream refraction /
		// heat-haze, which sample _currentRender) is dropped — no pass in this function reads it under RT,
		// and those materials re-capture _currentRender on demand. Saves the full-res blit. If a heat-haze /
		// refraction surface ever reads a stale through-glass scene under RT, restore the CopyFramebuffer.
		{
			// RR6c per-frame sub-texel jitter for the temporal UPSCALE (Halton(2,3), in RT-target texels,
			// scaled by r_rtReflJitter). Shifting the low-res sample grid each frame moves the fractional-
			// upscale beat (the "striped" reflection at e.g. 3/4 res) to a different phase every frame, so the
			// full-res history averages it out. Only meaningful WITH temporal accumulation, so zero when off.
			const bool rtTemporal = r_rtReflTemporal.GetBool();
			float jitterX = 0.0f, jitterY = 0.0f;
			if ( rtTemporal ) {
				const int   jidx   = ( tr.frameCount & 15 ) + 1;			// 16-frame Halton cycle (avoid index 0)
				const float jscale = idMath::ClampFloat( 0.0f, 4.0f, r_rtReflJitter.GetFloat() );
				jitterX = ( RB_RHI_Halton( jidx, 2 ) - 0.5f ) * jscale;		// ±0.5 texel at scale 1
				jitterY = ( RB_RHI_Halton( jidx, 3 ) - 0.5f ) * jscale;
			}

			rhi::RenderParams rp;
			memset( &rp, 0, sizeof( rp ) );
			rp.mvpMatrix[0] = rp.mvpMatrix[5] = rp.mvpMatrix[10] = rp.mvpMatrix[15] = 1.0f;
			memcpy( rp.modelViewMatrix, invView, sizeof( invView ) );	// u_modelViewMatrix = view->world
			rp.localParam0[0] = invP00;
			rp.localParam0[1] = invP11;
			rp.localParam0[2] = idMath::ClampFloat( 0.05f, 0.95f, r_ssrRoughnessFade.GetFloat() );	// roughness fade start
			rp.localParam1[1] = r_ssrIntensity.GetFloat();				// reflection strength (shared with SSR)
			rp.localParam1[2] = idMath::ClampFloat( 0.02f, 1.0f, r_ssrMaxRoughness.GetFloat() );
			// RR6c sample-grid jitter (windowCoord.xy, in RT-target texels): ssr_rt offsets its whole
			// reconstruction by this, shifting the low-res grid so the fractional-upscale beat averages out
			// across the full-res history (see ssr_rt.frag).
			rp.windowCoord[0] = jitterX;
			rp.windowCoord[1] = jitterY;
			// RR6: the ssr_rt TRACE runs at the SSR march resolution (cheap ray-query; shares SSR's grain and
			// scales with r_ssrResScale). RR6c then temporally upscales the result to full res, so the
			// composite reads a full-res history — no bilinear upscale of the fractional-res buffer (the beat).
			rp.screenCorrection[0] = 1.0f / ssrW;
			rp.screenCorrection[1] = 1.0f / ssrH;
			rp.depthTexRecip[0] = ( (float)fullW / ssrW ) / uploadW;
			rhi::FillDepthParms( rp, backEnd.viewDef ? backEnd.viewDef->projectionMatrix : NULL );	// this view's depth -> view z pair (cinematics cram the near plane)
			rp.depthTexRecip[1] = ( (float)fullH / ssrH ) / uploadH;
			rp.windowCoord[2] = viewYSign;
			// TLAS + geo-table device addresses, bit-cast into the float slots (floatBitsToUint in the shader)
			memcpy( &rp.rtParms[0], &tlasAddr, sizeof( tlasAddr ) );		// rtParms.x/y = TLAS
			memcpy( &rp.rtParms[2], &geoAddr,  sizeof( geoAddr ) );		// rtParms.z/w = geo table
			// RR16: gather the scene's real lights into the ring buffer; ssr_rt loops them to light the hit
			// (so an off-screen-reflected surface is lit correctly, not by a fixed key light). color = (light-
			// buffer device addr lo/hi bit-cast, count, ambient); diffuseModifier = (RR16f emissive glow scale,
			// light brightness scale, RR16c frustum gate, real-lights enable). The r_rtReflRealLights 0 key light
			// is a fixed dir hardcoded in-shader.
			unsigned long long rtLightAddr = 0;
			const int rtNumLights = r_rtReflRealLights.GetBool() ? RB_RHI_GatherSceneLights( r, viewDef, &rtLightAddr ) : 0;
			memcpy( &rp.color[0], &rtLightAddr, sizeof( rtLightAddr ) );	// color.xy = light-buffer device address
			rp.color[2] = (float)rtNumLights;
			rp.color[3] = 0.28f;										// ambient term
			rp.diffuseModifier[0] = idMath::ClampFloat( 0.0f, 16.0f, r_rtReflEmissive.GetFloat() );	// RR16f: self-illumination glow scale
			rp.diffuseModifier[1] = idMath::ClampFloat( 0.0f, 8.0f, r_rtReflLightScale.GetFloat() );	// RR16: real-light diffuse brightness scale
			rp.diffuseModifier[2] = r_rtReflLightFrustum.GetBool() ? 1.0f : 0.0f;	// RR16c: confine each light to its projection frustum
			rp.diffuseModifier[3] = ( rtNumLights > 0 ) ? 1.0f : 0.0f;	// real-lights enable (else the fixed key-light fallback)
			rp.localParam1[3] = idMath::ClampFloat( 0.0f, 1.0f, r_rtReflBump.GetFloat() );	// RR10: bump-normal warp amount
			rp.localParam1[0] = ( rhiSsaoAppliedThisView ? idMath::ClampFloat( 0.0f, 1.0f, r_specOcclusion.GetFloat() ) : 0.0f );	// RR14 specular occlusion strength (0 when no AO this view)

			// RR12 lit-scene reuse RETIRED 2026-09-23: RT now shades every reflective pixel at the hit (real
			// lights + per-pixel diffuse + emissive) with ONE consistent model, so there is no on-screen reuse
			// to feed, no reuse<->shade seam, and no dependency on a sampleable scene buffer. localParam0.w /
			// windowCoord.w (the old reuse-enable + reused-HDR firefly clamp) stay 0 from the memset; ssr_rt
			// no longer reads them.

			// ---- Pass A: trace + shade into the dedicated RT target (replace; cleared to 0 = no
			// contribution). unit 0 is an unused-but-bound dummy (reuse retired): bind the normal G-buffer so
			// the fixed VK_TEX_BINDINGS slot holds a valid image. 1 = depth, 2/3 = G-buffer, 4 = AO (RR14).
			rhi::ClearArgs rtClear;
			memset( &rtClear, 0, sizeof( rtClear ) );
			rtClear.color = true;
			r->GpuScopeBegin( "rtrefl.trace" );		// RT reflection ray trace + hit shading (ssrRes)
			r->BeginTargetPass( rhiRtReflRT, &rtClear );
			RB_RHI_BindUnit( 1, globalImages->currentDepthImage );
			RB_RHI_BindRTUnit( r, 2, rhiNormalResultRT );
			RB_RHI_BindRTImage( r, 3, matImg );
			// RR14: AO buffer on unit 4 for specular occlusion (VK-only RT path; white dummy when no AO ran)
			if ( rhiSsaoAppliedThisView ) {
				RB_RHI_BindRTUnit( r, 4, rhiSsaoResultRT );
			} else {
				RB_RHI_BindUnit( 4, globalImages->whiteImage );
			}
			RB_RHI_DrawFullscreen( r, rtProg, rp, r->GetRenderTargetImage( rhiNormalResultRT ) );
			r->EndPass();
			r->GpuScopeEnd();

			rhi::RenderTargetHandle rtResultRT = rhiRtReflRT;
			int rtResW = ssrW, rtResH = ssrH;		// resolution of rtResultRT (ssrRes until the upscale runs)

			// ---- Pass B: temporal accumulation + UPSCALE (r_rtReflTemporal) — the SAME ssr_temporal
			// machinery as SSR stage 2, but the history is FULL res: the ssrRes trace (unit 0) is bilinear-
			// upsampled into a reprojected full-res history, and the per-frame sample-grid jitter makes the
			// accumulation both resolve the firefly grid AND average away the fractional-upscale beat.
			if ( rtTemporal && RB_RHI_EnsureRtReflHistory( r, fullW, fullH ) ) {
				rhi::ShaderHandle tempProg = r->LoadShader( "ssr_temporal" );
				if ( tempProg ) {
					// reproject last frame by camera motion; RB_RHI_TemporalReproj advances prev<-staged
					// only for the first temporal consumer of a frame, so calling it after SSR/SSAO
					// already did just rebuilds this frame's reproj against the same prev (correct).
					float reproj[16];
					bool prevUsable;
					const bool haveInv = RB_RHI_TemporalReproj( viewDef, reproj, &prevUsable );
					const bool historyUsable = rhiRtReflHistValid && prevUsable && haveInv;

					const int writeIdx = rhiRtReflHistIdx;
					const int readIdx  = 1 - rhiRtReflHistIdx;

					rhi::RenderParams tempParms = rp;
					if ( haveInv ) {
						memcpy( tempParms.modelViewMatrix, reproj, sizeof( reproj ) );
					}
					// RR6c: this resolve + the history run at FULL res; the current RT buffer (unit 0) is the
					// ssrRes trace, bilinear-upsampled by the sampler. The 3x3 clip neighbourhood + depth
					// reprojection are therefore full-res (override rp's ssrRes screen/depth mapping).
					tempParms.screenCorrection[0] = 1.0f / fullW;
					tempParms.screenCorrection[1] = 1.0f / fullH;
					tempParms.depthTexRecip[0] = 1.0f / uploadW;
					rhi::FillDepthParms( tempParms, backEnd.viewDef ? backEnd.viewDef->projectionMatrix : NULL );	// this view's depth -> view z pair (cinematics cram the near plane)
					tempParms.depthTexRecip[1] = 1.0f / uploadH;
					tempParms.localParam0[2] = idMath::ClampFloat( 0.0f, 0.97f, r_ssrTemporalFeedback.GetFloat() );
					tempParms.localParam0[3] = historyUsable ? 1.0f : 0.0f;
					// reproject moving objects by the per-object velocity MRT when it exists (R1/A2), else
					// fall back to the camera-only matrix (localParam1.x = 0).
					rhi::ImageHandle velImg = ( rhiNormalVelThisView && rhiNormalReadyThisView )
						? r->GetRenderTargetImage3( rhiNormalResultRT ) : 0;
					tempParms.localParam1[0] = velImg ? 1.0f : 0.0f;
					// The clip neighbourhood steps in TRACE texels (Quarter: 4 output px) so its taps are distinct
					// traced samples, and the trace's sample-grid jitter is undone when reading it - without
					// both, a Quarter trace never accumulates and shimmers.
					tempParms.localParam1[1] = (float)fullW / (float)ssrW;
					tempParms.localParam1[2] = jitterX / (float)ssrW;
					tempParms.localParam1[3] = jitterY / (float)ssrH;

					// unit 0 = current RT contribution (via DrawFullscreen), 1 = history read slot,
					// 2 = depth, 3 = velocity
					r->GpuScopeBegin( "rtrefl.temporal" );		// temporal accumulate + upscale to full res
					r->BeginTargetPass( rhiRtReflHistRT[writeIdx], NULL );
					RB_RHI_BindUnit( 2, globalImages->currentDepthImage );
					// first frame after (re)alloc the read slot was never written (VK layout UNDEFINED);
					// bind the just-rendered cur (a written, SHADER_READ_ONLY target) until a real
					// history slot exists — the shader early-outs on historyUsable = 0 anyway.
					rhi::RenderTargetHandle histRT = historyUsable ? rhiRtReflHistRT[readIdx] : rhiRtReflRT;
					RB_RHI_BindRTUnit( r, 1, histRT );
					if ( velImg ) { RB_RHI_BindRTImage( r, 3, velImg ); }
					else          { RB_RHI_BindUnit( 3, globalImages->currentDepthImage ); }
					RB_RHI_DrawFullscreen( r, tempProg, tempParms, r->GetRenderTargetImage( rhiRtReflRT ) );
					r->EndPass();
					r->GpuScopeEnd();

					rtResultRT = rhiRtReflHistRT[writeIdx];
					rtResW = fullW; rtResH = fullH;		// the result is now the full-res upscaled history
					rhiRtReflHistIdx = readIdx;
					RB_RHI_TemporalStageCur( viewDef );
					rhiRtReflHistValid = true;
				}
			} else {
				rhiRtReflHistValid = false;		// temporal off: drop stale history so a re-enable starts clean
			}

			// ---- Pass C: additive composite of the (smoothed) RT reflection onto the lit scene.
			// EndPass left the target's scissor; restore the full view before drawing to the scene
			// (mirrors the SSR composite scissor set above).
			r->SetScissor( tr.viewportOffset[0] + viewDef->viewport.x1,
			               tr.viewportOffset[1] + viewDef->viewport.y1, fullW, fullH );
			backEnd.currentScissor = viewDef->scissor;
			rhi::RenderParams cp;
			memset( &cp, 0, sizeof( cp ) );
			cp.mvpMatrix[0] = cp.mvpMatrix[5] = cp.mvpMatrix[10] = cp.mvpMatrix[15] = 1.0f;
			// RR6b: box-blur strength + tap spacing; screenCorrection = the RESULT target's texel size so
			// the blur offsets are in its texels (full-res after the RR6c upscale, ssrRes if temporal off).
			// At r_rtReflBlur 0 the frag is a 1:1 additive passthrough.
			cp.localParam0[0] = idMath::ClampFloat( 0.0f, 1.0f, r_rtReflBlur.GetFloat() );
			cp.localParam0[1] = 1.0f;			// tap spacing (texels); ±2 taps -> a 5px spread at scale 1
			// RR8: full-res material mask (roughness fade start + cutoff) so the low-res reflection can't
			// halo past a reflective monster onto the non-reflective background (unit 1 = the G-buffer MRT).
			cp.localParam0[2] = idMath::ClampFloat( 0.05f, 0.95f, r_ssrRoughnessFade.GetFloat() );
			cp.localParam1[2] = idMath::ClampFloat( 0.02f, 1.0f, r_ssrMaxRoughness.GetFloat() );
			cp.screenCorrection[0] = 1.0f / rtResW;
			cp.screenCorrection[1] = 1.0f / rtResH;
			RB_RHI_BindRTImage( r, 1, matImg );		// full-res roughness/metalness for the silhouette mask
			r->GpuScopeBegin( "rtrefl.composite" );	// RR6b blur + additive composite onto the scene
			RB_RHI_DrawFullscreen( r, copyProg, cp, r->GetRenderTargetImage( rtResultRT ),
			                       GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );
			r->GpuScopeEnd();
		}
	}
	RB_RHI_ForgetTexBinds();
}

/*
===================
RB_RHI_VolumetricLighting

DUDE volumetric lighting, Route A (docs/volumetric-lighting.md): the air scatters the scene's real lights
toward the camera - visible cones under spots, shafts through grates, occluders cutting dark wedges.
  1. march (vol_march.frag) at r_volResScale: per pixel, step the view ray to the opaque depth; each step
     evaluates the shared scene-light buffer (RB_RHI_GatherSceneLights - the same rows RT reflections
     use) and, with r_volShadows, one any-hit visibility ray per reaching light against the scene TLAS.
  2. composite (vol_composite.frag) into the live scene target: scene * transmittance + in-scatter.
The per-sample shading is vol_scatter.glsl, deliberately stage-agnostic so the future froxel route
(Route B) moves it into a compute inject kernel unchanged; everything in THIS function is Route A only.

Vulkan + ray-query hardware, HDR scene buffer (the in-scatter is additive HDR radiance), fullscreen primary
view. Visibility needs the RT scene (a TLAS - r_rtSunShadows / r_rtAllLights / r_rtao build it); without
one the march still runs, unshadowed. Opt-in, no preset enables it (fidelity: adds light stock never had).
===================
*/
void RB_RHI_VolumetricLighting( rhi::RHI *r, const viewDef_t *viewDef ) {
	if ( !r_volumetrics.GetBool() || rhi::GetActiveBackendType() != rhi::BT_VULKAN || !r->SupportsRayQuery() ) {
		return;
	}
	const bool fullscreenView = viewDef->isWindowView;	// the primary scene view (set by RenderScene)
	if ( !viewDef->viewEntitys || viewDef->isSubview || !fullscreenView || !RB_RHI_HdrCaptureActive() ) {
		return;
	}
	if ( !globalImages->currentDepthImage->rhiCaptured ) {
		return;
	}
	unsigned long long lightAddr = 0;
	const int numLights = RB_RHI_GatherSceneLights( r, viewDef, &lightAddr );
	if ( numLights <= 0 || lightAddr == 0 ) {
		return;
	}
	rhi::ShaderHandle marchProg = r->LoadShader( "vol_march" );
	rhi::ShaderHandle compProg  = r->LoadShader( "vol_composite" );
	float invView[16];
	if ( !marchProg || !compProg || !RB_RHI_InvertMatrix( viewDef->worldSpace.modelViewMatrix, invView ) ) {
		return;
	}

	const int fullW = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int fullH = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
	// integer divisors only (Half / Quarter), like the reflection buffers: fractional ratios beat on upscale
	const int resDiv = ( r_volResScale.GetFloat() >= 0.375f ) ? 2 : 4;
	const int volW = Max( 1, fullW / resDiv );
	const int volH = Max( 1, fullH / resDiv );
	if ( rhiVolRT && r->GetRenderTargetImage( rhiVolRT ) == 0 ) {
		rhiVolRT = 0;	rhiVolBlurRT = 0;	rhiVolW = rhiVolH = 0;			// lost context (vid_restart)
	}
	if ( rhiVolRT && ( rhiVolW != volW || rhiVolH != volH ) ) {
		r->DestroyRenderTarget( rhiVolRT );
		rhiVolRT = 0;
		if ( rhiVolBlurRT ) { r->DestroyRenderTarget( rhiVolBlurRT ); rhiVolBlurRT = 0; }
	}
	if ( !rhiVolRT ) {
		rhiVolRT = r->CreateRenderTarget( rhi::IF_RGBA16F, volW, volH );
		if ( !rhiVolRT ) {
			rhiVolW = rhiVolH = 0;
			return;
		}
		rhiVolW = volW;	rhiVolH = volH;
	}

	// Temporal history + (OLD path) the full-res upsample target. r_volFastResolve accumulates the history
	// in the LOW-res march grid and upsamples ONCE at composite (Wronski/Hillaire froxel pattern); the old
	// path upsamples to full res first, then runs temporal + composite at full res. The history group is
	// sized per path (histW/histH) and torn down when the size OR the path changes.
	const bool wantTemporal = r_volTemporal.GetBool();
	const bool fastResolve  = r_volFastResolve.GetBool();
	const int  histW = fastResolve ? volW : fullW;
	const int  histH = fastResolve ? volH : fullH;
	if ( ( rhiVolUpRT && r->GetRenderTargetImage( rhiVolUpRT ) == 0 )
			|| ( rhiVolHistRT[0] && r->GetRenderTargetImage( rhiVolHistRT[0] ) == 0 ) ) {
		rhiVolUpRT = 0;	rhiVolHistRT[0] = rhiVolHistRT[1] = 0;	rhiVolFullW = rhiVolFullH = 0;	// lost context
	}
	if ( ( rhiVolUpRT || rhiVolHistRT[0] || rhiVolHistRT[1] )
			&& ( rhiVolFullW != histW || rhiVolFullH != histH || rhiVolFastPrev != fastResolve ) ) {
		if ( rhiVolUpRT ) { r->DestroyRenderTarget( rhiVolUpRT ); rhiVolUpRT = 0; }
		for ( int i = 0; i < 2; i++ ) {
			if ( rhiVolHistRT[i] ) { r->DestroyRenderTarget( rhiVolHistRT[i] ); rhiVolHistRT[i] = 0; }
		}
		rhiVolHistValid = false;
	}
	rhiVolFastPrev = fastResolve;
	rhiVolFullW = histW;	rhiVolFullH = histH;
	// OLD path only: the full-res upsample target that feeds the full-res temporal pass as its "current".
	if ( !fastResolve && !rhiVolUpRT ) {
		rhiVolUpRT = r->CreateRenderTarget( rhi::IF_RGBA16F, fullW, fullH );
		rhiVolHistValid = false;
		if ( !rhiVolUpRT ) {
			rhiVolFullW = rhiVolFullH = 0;
			return;
		}
	}
	if ( wantTemporal && ( !rhiVolHistRT[0] || !rhiVolHistRT[1] ) ) {
		for ( int i = 0; i < 2; i++ ) {
			if ( !rhiVolHistRT[i] ) { rhiVolHistRT[i] = r->CreateRenderTarget( rhi::IF_RGBA16F, histW, histH ); }
		}
		rhiVolHistValid = false;
	}
	rhi::ShaderHandle upProg   = r->LoadShader( "vol_upsample" );
	rhi::ShaderHandle tempProg = wantTemporal ? r->LoadShader( "ssr_temporal" ) : 0;
	if ( !upProg ) {
		return;
	}
	const bool doTemporal = wantTemporal && tempProg && rhiVolHistRT[0] && rhiVolHistRT[1];
	if ( doTemporal ) {
		// fresh, UNCORRELATED sample offsets every frame (vol_march.frag hashes pixel + this index); a
		// structured dither animated per frame left drifting, then striped, residue in the accumulation
		rhiVolJitterPhase = (float)( tr.frameCount & 1023 );
	} else {
		rhiVolHistValid = false;
	}

	const unsigned long long tlasAddr = r->GetTlasAddress();
	const int uploadW = globalImages->currentDepthImage->uploadWidth;
	const int uploadH = globalImages->currentDepthImage->uploadHeight;

	rhi::RenderParams mp;
	memset( &mp, 0, sizeof( mp ) );
	mp.mvpMatrix[0] = mp.mvpMatrix[5] = mp.mvpMatrix[10] = mp.mvpMatrix[15] = 1.0f;
	memcpy( mp.modelViewMatrix, invView, sizeof( invView ) );			// view -> world
	memcpy( &mp.rtParms[0], &tlasAddr, sizeof( tlasAddr ) );			// bit-cast device addresses
	memcpy( &mp.color[0], &lightAddr, sizeof( lightAddr ) );
	mp.color[2] = (float)numLights;
	mp.color[3] = (float)s_sceneFogCount;								// fog-volume rows follow the lights
	mp.localParam0[0] = ( viewDef->projectionMatrix[0] != 0.0f ) ? 1.0f / viewDef->projectionMatrix[0] : 1.0f;
	mp.localParam0[1] = ( viewDef->projectionMatrix[5] != 0.0f ) ? 1.0f / viewDef->projectionMatrix[5] : 1.0f;
	// r_volDensity 1 = a scattering coefficient of 0.001 per world unit: a ~500-unit room scatters about
	// half of a full-strength light toward the eye - thin haze, by design
	mp.localParam0[2] = 0.001f * idMath::ClampFloat( 0.0f, 16.0f, r_volDensity.GetFloat() );
	mp.localParam0[3] = idMath::ClampFloat( 0.0f, 16.0f, r_volIntensity.GetFloat() );
	mp.localParam1[0] = (float)idMath::ClampInt( 4, 64, r_volSteps.GetInteger() );
	mp.localParam1[1] = idMath::ClampFloat( 128.0f, 16384.0f, r_volMaxDist.GetFloat() );
	mp.localParam1[2] = idMath::ClampFloat( -0.9f, 0.9f, r_volAnisotropy.GetFloat() );
	mp.localParam1[3] = ( r_volShadows.GetBool() && tlasAddr != 0 ) ? 1.0f : 0.0f;
	mp.windowCoord[0] = rhiVolJitterPhase;								// advances per frame only while the temporal pass runs
	mp.windowCoord[1] = ( r_volLights.GetInteger() >= 2 ) ? 2.0f : 1.0f;
	mp.windowCoord[2] = -1.0f;											// VK-only pass: top-down view-Y sign
	mp.windowCoord[3] = idMath::ClampFloat( 0.0f, 4.0f, r_volExtinction.GetFloat() );
	mp.diffuseModifier[0] = idMath::ClampFloat( 0.0f, 4.0f, r_volPointLightScale.GetFloat() );
	mp.diffuseModifier[1] = idMath::ClampFloat( 0.0f, 4.0f, r_volFlashlightScale.GetFloat() );
	mp.diffuseModifier[2] = r_volPointShadows.GetBool() ? 1.0f : 0.0f;
	mp.diffuseModifier[3] = idMath::ClampFloat( 0.0f, 4.0f, r_volFogPointLightScale.GetFloat() );	// point-light scale inside fog volumes
	mp.screenCorrection[0] = 1.0f / volW;
	mp.screenCorrection[1] = 1.0f / volH;
	mp.depthTexRecip[0] = ( (float)fullW / volW ) / uploadW;
	mp.depthTexRecip[1] = ( (float)fullH / volH ) / uploadH;
	rhi::FillDepthParms( mp, backEnd.viewDef ? backEnd.viewDef->projectionMatrix : NULL );	// cinematics cram the near plane

	// ---- 1. march
	rhi::ClearArgs clear;
	memset( &clear, 0, sizeof( clear ) );
	clear.color = true;
	r->BeginTargetPass( rhiVolRT, &clear );
	RB_RHI_BindUnit( 0, globalImages->currentDepthImage );
	RB_RHI_DrawFullscreen( r, marchProg, mp, 0 );
	r->EndPass();

	// ---- 1b. low-res depth-aware blur (vol_blur.frag): resolves the march's per-pixel dither spatially, so
	// the temporal pass only has to clean up the remainder (r_volBlur = radius in low-res texels, 0 = off)
	rhi::RenderTargetHandle volLowRT = rhiVolRT;
	const int blurRadius = idMath::ClampInt( 0, 3, r_volBlur.GetInteger() );
	if ( blurRadius > 0 ) {
		rhi::ShaderHandle blurProg = r->LoadShader( "vol_blur" );
		if ( blurProg && !rhiVolBlurRT ) {
			rhiVolBlurRT = r->CreateRenderTarget( rhi::IF_RGBA16F, volW, volH );
		}
		if ( blurProg && rhiVolBlurRT ) {
			rhi::RenderParams bp;
			memset( &bp, 0, sizeof( bp ) );
			bp.mvpMatrix[0] = bp.mvpMatrix[5] = bp.mvpMatrix[10] = bp.mvpMatrix[15] = 1.0f;
			bp.localParam0[0] = (float)volW;
			bp.localParam0[1] = (float)volH;
			bp.localParam0[2] = (float)resDiv;
			bp.localParam0[3] = (float)blurRadius;
			bp.depthTexRecip[0] = 1.0f / uploadW;
			bp.depthTexRecip[1] = 1.0f / uploadH;
			rhi::FillDepthParms( bp, backEnd.viewDef ? backEnd.viewDef->projectionMatrix : NULL );
			r->BeginTargetPass( rhiVolBlurRT, NULL );
			RB_RHI_BindUnit( 1, globalImages->currentDepthImage );
			RB_RHI_DrawFullscreen( r, blurProg, bp, r->GetRenderTargetImage( rhiVolRT ) );
			r->EndPass();
			volLowRT = rhiVolBlurRT;
		}
	}

	// ---- 2. depth-aware upsample (vol_upsample.frag): no shaft bleed across silhouettes. These params serve
	// both the OLD standalone full-res pass AND the fast path's fused upsample-at-composite below.
	rhi::RenderParams up;
	memset( &up, 0, sizeof( up ) );
	up.mvpMatrix[0] = up.mvpMatrix[5] = up.mvpMatrix[10] = up.mvpMatrix[15] = 1.0f;
	up.localParam0[0] = (float)volW;
	up.localParam0[1] = (float)volH;
	up.localParam0[2] = (float)resDiv;
	up.depthTexRecip[0] = 1.0f / uploadW;
	up.depthTexRecip[1] = 1.0f / uploadH;
	rhi::FillDepthParms( up, backEnd.viewDef ? backEnd.viewDef->projectionMatrix : NULL );
	// the signal the temporal pass accumulates: fast path keeps it in the LOW-res grid (upsample deferred to
	// composite); old path upsamples to full res first.
	rhi::RenderTargetHandle volResultRT = volLowRT;
	if ( !fastResolve ) {
		r->BeginTargetPass( rhiVolUpRT, NULL );
		RB_RHI_BindUnit( 1, globalImages->currentDepthImage );
		RB_RHI_DrawFullscreen( r, upProg, up, r->GetRenderTargetImage( volLowRT ) );
		r->EndPass();
		volResultRT = rhiVolUpRT;
	}
	const rhi::RenderTargetHandle volCurRT = fastResolve ? volLowRT : rhiVolUpRT;	// temporal's "current" input

	// ---- 3. temporal accumulation (the shared ssr_temporal): resolves the per-frame-jittered march grain.
	// Reprojected by the OPAQUE surface behind the fog - the wrong depth for the medium itself, harmless for
	// a signal this low-frequency (docs/volumetric-lighting.md). The clip taps step by the march texel so
	// they land on distinct marched samples (the Quarter-res lesson from the RT reflections).
	if ( doTemporal ) {
		float reproj[16];
		bool prevUsable;
		const bool haveInv = RB_RHI_TemporalReproj( viewDef, reproj, &prevUsable );
		const bool historyUsable = rhiVolHistValid && prevUsable && haveInv;
		const int writeIdx = rhiVolHistIdx;
		const int readIdx  = 1 - rhiVolHistIdx;

		rhi::RenderParams tp;
		memset( &tp, 0, sizeof( tp ) );
		tp.mvpMatrix[0] = tp.mvpMatrix[5] = tp.mvpMatrix[10] = tp.mvpMatrix[15] = 1.0f;
		if ( haveInv ) {
			memcpy( tp.modelViewMatrix, reproj, sizeof( reproj ) );
		}
		tp.localParam0[0] = mp.localParam0[0];
		tp.localParam0[1] = mp.localParam0[1];
		tp.localParam0[2] = idMath::ClampFloat( 0.0f, 0.97f, r_volTemporalFeedback.GetFloat() );
		tp.localParam0[3] = historyUsable ? 1.0f : 0.0f;
		rhi::ImageHandle velImg = ( rhiNormalVelThisView && rhiNormalReadyThisView )
			? r->GetRenderTargetImage3( rhiNormalResultRT ) : 0;
		tp.localParam1[0] = velImg ? 1.0f : 0.0f;
		// fast path: temporal runs in the native LOW-res grid, so neighbours are distinct marched samples
		// (tap step 1) and the depth fetch carries the march's low->full stretch; old path upscales a full-res
		// buffer that still only holds resDiv-distinct samples, so taps step by resDiv.
		tp.localParam1[1] = fastResolve ? 1.0f : (float)resDiv;
		tp.particleRadius[0] = 1.0f;									// volumetric buffer: accumulate sky pixels too (lit air in front of the sky)
		tp.windowCoord[2] = -1.0f;
		tp.screenCorrection[0] = 1.0f / ( fastResolve ? volW : fullW );
		tp.screenCorrection[1] = 1.0f / ( fastResolve ? volH : fullH );
		tp.depthTexRecip[0] = ( fastResolve ? ( (float)fullW / volW ) : 1.0f ) / uploadW;
		tp.depthTexRecip[1] = ( fastResolve ? ( (float)fullH / volH ) : 1.0f ) / uploadH;
		rhi::FillDepthParms( tp, backEnd.viewDef ? backEnd.viewDef->projectionMatrix : NULL );

		r->BeginTargetPass( rhiVolHistRT[writeIdx], NULL );
		RB_RHI_BindUnit( 2, globalImages->currentDepthImage );
		RB_RHI_BindRTUnit( r, 1, historyUsable ? rhiVolHistRT[readIdx] : volCurRT );	// never an unwritten slot
		if ( velImg ) { RB_RHI_BindRTImage( r, 3, velImg ); }
		else          { RB_RHI_BindUnit( 3, globalImages->currentDepthImage ); }
		RB_RHI_DrawFullscreen( r, tempProg, tp, r->GetRenderTargetImage( volCurRT ) );
		r->EndPass();

		volResultRT = rhiVolHistRT[writeIdx];
		rhiVolHistIdx = readIdx;
		RB_RHI_TemporalStageCur( viewDef );
		rhiVolHistValid = true;
	}

	// ---- 4. composite onto the scene: dst * transmittance + in-scatter.
	// Fast path: the result is still LOW-res, so the composite IS the depth-aware upsample (vol_upsample with
	// the composite blend) - one full-res pass instead of a separate upsample + a full-res composite.
	r->SetScissor( tr.viewportOffset[0] + viewDef->viewport.x1,
	               tr.viewportOffset[1] + viewDef->viewport.y1, fullW, fullH );
	backEnd.currentScissor = viewDef->scissor;
	if ( fastResolve ) {
		RB_RHI_BindUnit( 1, globalImages->currentDepthImage );
		RB_RHI_DrawFullscreen( r, upProg, up, r->GetRenderTargetImage( volResultRT ),
		                       GLS_SRCBLEND_ONE | GLS_DSTBLEND_SRC_ALPHA );
	} else {
		rhi::RenderParams cp;
		memset( &cp, 0, sizeof( cp ) );
		cp.mvpMatrix[0] = cp.mvpMatrix[5] = cp.mvpMatrix[10] = cp.mvpMatrix[15] = 1.0f;
		RB_RHI_DrawFullscreen( r, compProg, cp, r->GetRenderTargetImage( volResultRT ),
		                       GLS_SRCBLEND_ONE | GLS_DSTBLEND_SRC_ALPHA );
	}
	RB_RHI_ForgetTexBinds();
}

/*
===================
RB_RHI_SSAOPass

GTAO screen-space ambient occlusion (docs/ssao-gtao.md). Runs right after the depth
prepass, once globalImages->currentDepthImage holds this view's depth, and before the
ambient/interaction passes that consume the AO buffer. Two fullscreen passes: the
horizon search (ssao) then a bilateral denoise (ssao_blur). Only ever active with
r_ssao on an enhancement backend, and only for the fullscreen primary view (the depth
capture and screen mapping assume the whole framebuffer at the origin).
===================
*/
static void RB_RHI_SSAOPass( rhi::RHI *r, const viewDef_t *viewDef ) {
	// RTGI supersedes screen-space AO: it grounds the scene with real one-bounce occlusion + bounce light, and
	// the fake cube-map ambient GTAO used to darken is fully replaced by GI (r_rtgiAmbientKeep). So AO is one
	// technique at a time — RTAO already stands down for r_rtgi, and GTAO stands down here too (no wasted pass;
	// consumers gate on rhiSsaoAppliedThisView so they simply apply no AO this view).
	if ( !r_ssao.GetBool() || r_rtgi.GetBool() || !R_BackendSupportsEnhancements() ) {
		return;
	}
	// H4d (docs/rtx-rtao.md): RTAO produced, denoised and composited this view's AO —
	// the whole GTAO chain (depth mip, horizon march, blur, temporal) is skipped. RTAO
	// failing for any reason leaves the flag false and GTAO runs exactly as before.
	if ( rhiRtaoDenoisedThisView ) {
		return;
	}
	const bool fullscreenView = viewDef->isWindowView;	// the primary scene view (set by RenderScene)
	if ( !viewDef->viewEntitys || viewDef->isSubview || !fullscreenView ) {
		return;
	}
	if ( globalImages->currentDepthImage->uploadWidth <= 0 ) {
		return;		// no depth captured yet (shouldn't happen after the prepass)
	}

	rhi::ShaderHandle ssaoProg = r->LoadShader( "ssao" );
	rhi::ShaderHandle blurProg = r->LoadShader( "ssao_blur" );
	if ( !ssaoProg || !blurProg ) {
		return;
	}

	const int   fullW    = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int   fullH    = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
	const float resScale = idMath::ClampFloat( 0.25f, 1.0f, r_ssaoResScale.GetFloat() );
	int aoW = (int)( fullW * resScale + 0.5f );
	int aoH = (int)( fullH * resScale + 0.5f );
	if ( aoW < 1 ) aoW = 1;
	if ( aoH < 1 ) aoH = 1;
	if ( !RB_RHI_EnsureSsaoTargets( r, aoW, aoH ) ) {
		return;
	}

	// SSAO Phase 1 (docs/ssao-perf-optimization.md): build the prefiltered linear-depth
	// mip chain when enabled and the backend supports mipped targets. depthTexRecip.z
	// carries the max LOD to ssao.frag (0 = off -> the raw full-res path).
	bool doDepthMip = false;
	rhi::ShaderHandle depthMipProg = 0, depthDownProg = 0;
	if ( r_ssaoDepthMip.GetBool() ) {
		if ( RB_RHI_EnsureSsaoDepthMip( r, aoW, aoH ) ) {
			depthMipProg  = r->LoadShader( "ssao_depthmip" );	// linearize into level 0
			depthDownProg = r->LoadShader( "ssao_depthdown" );	// max-downsample the chain
			doDepthMip = ( depthMipProg != 0 && depthDownProg != 0 );
		}
	} else if ( rhiSsaoDepthMipRT ) {
		// toggled off: reclaim the target so it isn't left resident
		r->DestroyRenderTarget( rhiSsaoDepthMipRT );
		rhiSsaoDepthMipRT = 0;
		rhiSsaoDepthMipW = rhiSsaoDepthMipH = rhiSsaoDepthMipLevels = 0;
	}

	// horizon-search sample budget (clamped to the shader's MAX_SLICES / MAX_STEPS).
	// r_ssaoTemporalTrade (A/B, docs/ssao-perf-optimization.md Phase 4) overrides it with
	// a fixed 4x6 budget funded by forced temporal accumulation, so the brute-force
	// preset budgets can be compared live against the cheaper temporal look.
	const bool temporalTrade = r_ssaoTemporalTrade.GetBool();
	const bool temporalOn    = r_ssaoTemporal.GetBool() || temporalTrade;
	const int slices = temporalTrade ? 4 : idMath::ClampInt( 1, 8,  r_ssaoSlices.GetInteger() );
	const int steps  = temporalTrade ? 6 : idMath::ClampInt( 1, 12, r_ssaoSteps.GetInteger() );

	const float *proj  = viewDef->projectionMatrix;
	const float invP00 = ( proj[0] != 0.0f ) ? 1.0f / proj[0] : 1.0f;
	const float invP11 = ( proj[5] != 0.0f ) ? 1.0f / proj[5] : 1.0f;
	const float radius = r_ssaoRadius.GetFloat();
	// a world length maps to AO-target pixels at depth d as radiusPixFactor / d
	const float radiusPixFactor = 0.5f * radius * proj[5] * (float)aoH;

	const int uploadW = globalImages->currentDepthImage->uploadWidth;
	const int uploadH = globalImages->currentDepthImage->uploadHeight;

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	parms.depthTexRecip[0]    = ( (float)fullW / aoW ) / uploadW;	// gl_FragCoord (AO) -> depth tc
	rhi::FillDepthParms( parms, backEnd.viewDef ? backEnd.viewDef->projectionMatrix : NULL );	// this view's depth -> view z pair (cinematics cram the near plane)
	parms.depthTexRecip[1]    = ( (float)fullH / aoH ) / uploadH;
	// SSAO Phase 1: .z = depth-mip max LOD (0 = off, ssao.frag uses the raw path), .w = LOD bias.
	// r_ssaoDepthMipMaxLod caps how coarse the march may go — the coarsest (box-averaged) mips are
	// where occlusion smears across silhouettes into halos. Clamp to the built chain and keep >= 1
	// when on, since ssao.frag treats .z < 0.5 as the feature-off flag.
	float mipMaxLod = (float)( rhiSsaoDepthMipLevels - 1 );
	mipMaxLod = idMath::ClampFloat( 1.0f, mipMaxLod, (float)r_ssaoDepthMipMaxLod.GetInteger() );
	parms.depthTexRecip[2]    = doDepthMip ? mipMaxLod : 0.0f;
	parms.depthTexRecip[3]    = r_ssaoDepthMipBias.GetFloat();
	parms.screenCorrection[0] = 1.0f / aoW;				// gl_FragCoord -> [0,1] uv
	parms.screenCorrection[1] = 1.0f / aoH;
	parms.localParam0[0] = invP00;
	parms.localParam0[1] = invP11;
	parms.localParam0[2] = radius;
	parms.localParam0[3] = RB_RHI_AoIntensityWithVolumetrics( r, r_ssaoIntensity.GetFloat() );
	parms.localParam1[0] = radiusPixFactor;
	parms.localParam1[1] = (float)steps;
	parms.localParam1[2] = (float)slices;
	parms.localParam1[3] = r_ssaoBentNormal.GetBool() ? 1.0f : 0.0f;
	// use the bump-mapped normal G-buffer (unit 1) if the normal prepass produced one this
	// view; otherwise ssao.frag reconstructs the normal from depth (windowCoord.x = flag)
	const bool useNormalBuf = rhiNormalReadyThisView && rhiNormalResultRT != 0 && r_ssaoNormalBuffer.GetBool();
	parms.windowCoord[0] = useNormalBuf ? 1.0f : 0.0f;
	// view-Y sign for ssao.frag's position reconstruction: +1 on GL (gl_FragCoord.y
	// bottom-up), -1 on Vulkan (top-down) so reconstructed positions match the view-
	// space G-buffer normal — otherwise AO is wrong on floors/ceilings
	parms.windowCoord[2] = ( rhi::GetActiveBackendType() == rhi::BT_VULKAN ) ? -1.0f : 1.0f;
	// per-frame noise rotation for temporal accumulation: advance a golden-ratio walk so
	// each frame's horizon search jitters differently, giving the temporal pass distinct
	// samples to average. 0 when temporal is off -> ssao.frag falls back to the plain dither.
	if ( temporalOn ) {
		rhiSsaoJitterPhase += 0.61803399f;
		rhiSsaoJitterPhase -= (float)(int)rhiSsaoJitterPhase;	// keep in [0,1)
		parms.windowCoord[1] = rhiSsaoJitterPhase;
	}

	// SSAO Phase 1: linearize _currentDepth into level 0 of the depth-mip target, then
	// box-average it down the chain. Done before the horizon search so ssao.frag can
	// textureLod a coarser mip for its far steps (the cache win).
	if ( doDepthMip ) {
		// level 0: linearize _currentDepth into the mip target. The normal G-buffer rides
		// on unit 1 so ssao_depthmip.frag bakes the weapon mask into the chain (weapon
		// texels -> far depth) and the horizon march skips its per-tap full-res mask
		// fetch; a dummy when no normal buffer exists this view (the shader gates on
		// windowCoord.x and never samples it, but Vulkan validates the descriptor slot).
		r->BeginTargetPass( rhiSsaoDepthMipRT, NULL );
		RB_RHI_BindUnit( 0, globalImages->currentDepthImage );
		if ( useNormalBuf ) {
			RB_RHI_BindRTUnit( r, 1, rhiNormalResultRT );
			backEnd.glState.tmu[1].current2DMap = -1;	// direct bind bypassed the tmu cache
		} else {
			RB_RHI_BindUnit( 1, globalImages->currentDepthImage );
		}
		RB_RHI_DrawFullscreen( r, depthMipProg, parms, 0 );
		r->EndPass();
		// levels 1..N-1: max-downsample from the previous level (a conservative farthest-
		// depth filter that avoids the foreground/background averaging halo). Source level
		// bound on unit 0 via DrawFullscreen; localParam0.x = the source mip level the
		// downsample shader texelFetches (Vulkan binds a single-level view -> 0; GL3 binds
		// the whole texture -> the real level).
		const bool vkBackend = ( rhi::GetActiveBackendType() == rhi::BT_VULKAN );
		for ( int L = 1; L < rhiSsaoDepthMipLevels; L++ ) {
			rhi::RenderParams dp = parms;
			dp.localParam0[0] = vkBackend ? 0.0f : (float)( L - 1 );
			rhi::ImageHandle src = r->GetRenderTargetMipImage( rhiSsaoDepthMipRT, L - 1 );
			r->BeginTargetMipPass( rhiSsaoDepthMipRT, L, NULL );
			RB_RHI_DrawFullscreen( r, depthDownProg, dp, src );
			r->EndPass();
		}
		// direct binds bypassed the tmu cache; forget unit 0 so the horizon search's
		// cache-aware depth bind below actually re-issues.
		backEnd.glState.tmu[0].current2DMap = -1;
	}

	// horizon search: _currentDepth (unit 0) [+ normal G-buffer on unit 1] -> raw AO
	r->BeginTargetPass( rhiSsaoRT, NULL );
	RB_RHI_BindUnit( 0, globalImages->currentDepthImage );
	if ( useNormalBuf ) {
		RB_RHI_BindRTUnit( r, 1, rhiNormalResultRT );	// normal G-buffer (GL raw bind / VK rhiVkUnits[1])
		// direct bind bypassed the tmu cache; forget unit 1 so the blur's depth bind re-issues
		backEnd.glState.tmu[1].current2DMap = -1;
	}
	// SSAO Phase 1: the prefiltered depth mip on unit 2 for the far-step taps. When off,
	// bind _currentDepth there as an unused dummy so the descriptor slot stays valid
	// (ssao.frag routes to the raw path via depthTexRecip.z, never sampling it).
	if ( doDepthMip ) {
		RB_RHI_BindRTUnit( r, 2, rhiSsaoDepthMipRT );
		backEnd.glState.tmu[2].current2DMap = -1;	// direct bind bypassed the tmu cache
	} else {
		RB_RHI_BindUnit( 2, globalImages->currentDepthImage );
	}
	RB_RHI_DrawFullscreen( r, ssaoProg, parms, 0 );
	r->EndPass();

	// separable bilateral denoise: horizontal (rhiSsaoRT -> rhiSsaoBlurRT) then vertical
	// (rhiSsaoBlurRT -> rhiSsaoRT), so the finished AO lands back in rhiSsaoRT. Half the
	// tap count of the old NxN box for the same reach. localParam0.x picks the axis; the
	// AO texture is unit 0 (via DrawFullscreen), depth is unit 1 — the linear mip's
	// level 0 when it exists (already linear, AO-res, R16F: cheaper than re-deriving
	// from full-res _currentDepth per tap; blurParms.depthTexRecip.z routes the shader),
	// _currentDepth otherwise.
	rhi::RenderParams blurParms = parms;
	blurParms.localParam0[0] = 0.0f;	// horizontal
	r->BeginTargetPass( rhiSsaoBlurRT, NULL );
	if ( doDepthMip ) {
		RB_RHI_BindRTUnit( r, 1, rhiSsaoDepthMipRT );
		backEnd.glState.tmu[1].current2DMap = -1;	// direct bind bypassed the tmu cache
	} else {
		RB_RHI_BindUnit( 1, globalImages->currentDepthImage );
	}
	RB_RHI_DrawFullscreen( r, blurProg, blurParms, r->GetRenderTargetImage( rhiSsaoRT ) );
	r->EndPass();

	blurParms.localParam0[0] = 1.0f;	// vertical
	r->BeginTargetPass( rhiSsaoRT, NULL );
	if ( doDepthMip ) {
		RB_RHI_BindRTUnit( r, 1, rhiSsaoDepthMipRT );
		backEnd.glState.tmu[1].current2DMap = -1;	// direct bind bypassed the tmu cache
	} else {
		RB_RHI_BindUnit( 1, globalImages->currentDepthImage );
	}
	RB_RHI_DrawFullscreen( r, blurProg, blurParms, r->GetRenderTargetImage( rhiSsaoBlurRT ) );
	r->EndPass();

	rhiSsaoResultRT = rhiSsaoRT;		// finished AO the ambient/interaction passes sample

	// temporal accumulation (docs/ssao-gtao.md, r_ssaoTemporal): blend this frame's
	// denoised AO (in rhiSsaoRT) with the previous frame's result reprojected by camera
	// motion, into a ping-ponged history buffer that then feeds the lighting. Static
	// world -> the only motion is the camera, so a single reproj matrix (current view
	// space -> previous clip) suffices; the shader neighbourhood-clamps to kill ghosting.
	if ( temporalOn && RB_RHI_EnsureSsaoHistory( r, aoW, aoH ) ) {
		rhi::ShaderHandle tempProg = r->LoadShader( "ssao_temporal" );
		if ( tempProg ) {
			// reproject last frame's AO by camera motion; the shared camera state
			// (docs/fsr-temporal-pipeline.md A1) advances prev<-cur once per rendered frame
			float reproj[16];
			bool prevUsable;
			const bool haveInv = RB_RHI_TemporalReproj( viewDef, reproj, &prevUsable );
			const bool historyUsable = rhiSsaoHistValid && prevUsable && haveInv;

			const int writeIdx = rhiSsaoHistIdx;
			const int readIdx  = 1 - rhiSsaoHistIdx;

			rhi::RenderParams tempParms = parms;
			if ( haveInv ) {
				memcpy( tempParms.modelViewMatrix, reproj, sizeof( reproj ) );
			}
			tempParms.localParam0[2] = idMath::ClampFloat( 0.0f, 0.97f, r_ssaoTemporalFeedback.GetFloat() );
			tempParms.localParam0[3] = historyUsable ? 1.0f : 0.0f;

			// R1/A2 consumption: reproject the AO history by the per-object velocity buffer when it
			// exists (r_motionVectors, VK) so moving objects stop dragging their AO. Falls back to
			// the camera-only matrix reproj (localParam1.x = 0).
			rhi::ImageHandle velImg = ( rhiNormalVelThisView && rhiNormalReadyThisView )
				? r->GetRenderTargetImage3( rhiNormalResultRT ) : 0;
			tempParms.localParam1[0] = velImg ? 1.0f : 0.0f;

			// unit 0 = current AO (rhiSsaoRT, via DrawFullscreen), unit 1 = history read
			// slot (a render target, direct-bound like the normal buffer), unit 2 = depth,
			// unit 3 = velocity (R1/A2)
			r->BeginTargetPass( rhiSsaoHistRT[writeIdx], NULL );
			RB_RHI_BindUnit( 2, globalImages->currentDepthImage );
			RB_RHI_BindRTUnit( r, 1, rhiSsaoHistRT[readIdx] );	// history read (GL raw / VK rhiVkUnits[1])
			backEnd.glState.tmu[1].current2DMap = -1;			// direct bind bypassed the tmu cache
			// unit 3 = velocity MRT, or a dummy (u_velocity is a statically-used sampler VK validates)
			if ( velImg ) { RB_RHI_BindRTImage( r, 3, velImg ); }
			else          { RB_RHI_BindUnit( 3, globalImages->currentDepthImage ); }
			RB_RHI_DrawFullscreen( r, tempProg, tempParms, r->GetRenderTargetImage( rhiSsaoRT ) );
			r->EndPass();

			rhiSsaoResultRT = rhiSsaoHistRT[writeIdx];	// accumulated AO the lighting samples
			rhiSsaoHistIdx  = readIdx;					// next frame writes the other slot
			RB_RHI_TemporalStageCur( viewDef );
			rhiSsaoHistValid  = true;					// the write slot now holds a usable history
		}
	} else {
		// temporal off (or history alloc failed): use the plain denoised AO and drop any
		// stale history so a later re-enable starts clean instead of blending garbage
		rhiSsaoHistValid = false;
	}

	// tell the ambient pass (RB_RHI_DrawInteraction) an AO buffer is ready for this
	// view; the screen->AO uv mapping needs the full view size
	rhiSsaoViewW = fullW;
	rhiSsaoViewH = fullH;
	rhiSsaoAppliedThisView = true;

	// our fullscreen draws bind textures directly (da.textures + RB_RHI_BindUnit),
	// leaving the tmu cache disagreeing with GL; forget them so the per-light binds
	// that follow re-issue instead of skipping a needed unit-0 rebind
	RB_RHI_ForgetTexBinds();

	// EndPass restored the pre-pass viewport/scissor; keep the CPU scissor cache
	// in step for the per-light passes that follow
	backEnd.currentScissor = viewDef->scissor;
}

/*
===================
RB_RHI_RtaoPass

H4a of RTAO (docs/rtx-rtao.md): the noisy ray pass. One cosine-weighted hemisphere
occlusion ray per pixel against the scene TLAS, written to an R16F target as NRD's
normalized hit distance (miss/sky/weapon = 1.0). Runs right after the GTAO pass point,
once _currentDepth and the normal G-buffer exist for this view. Produces INPUT for the
H4c NRD denoise — nothing consumes it yet beyond the r_rtaoDebug overlay, so H4a is
visually inert in normal play. VK + RT hardware only; the TLAS is auto-built because
r_rtao joined the RT-world implication gate (tr_main R_RtWorldUpdate).
===================
*/
static void RB_RHI_RtaoPass( rhi::RHI *r, const viewDef_t *viewDef ) {
	rhiRtaoRanThisView = false;
	rhiRtaoDenoisedThisView = false;	// reset on EVERY early-out path — RB_RHI_SSAOPass gates on it
	// H6b (docs/rtx-rtgi.md): when r_rtgi is on, GI REPLACES RTAO — the single NRD instance denoises GI this
	// frame (never both denoisers in one frame), and screen-space GTAO (RB_RHI_SSAOPass, which runs next)
	// fills the scalar AO the ambient/reflections still consume. RTAO stands down whenever GI is active.
	if ( !r_rtao.GetBool() || !r_ssao.GetBool() || r_rtgi.GetBool() || rhi::GetActiveBackendType() != rhi::BT_VULKAN ) {
		return;
	}
	const bool fullscreenView = viewDef->isWindowView;	// the primary scene view (set by RenderScene)
	if ( !viewDef->viewEntitys || viewDef->isSubview || !fullscreenView ) {
		return;
	}
	if ( globalImages->currentDepthImage->uploadWidth <= 0
			|| !rhiNormalReadyThisView || rhiNormalResultRT == 0 ) {
		return;		// needs the sealed depth + the bump-mapped normal G-buffer
	}
	const unsigned long long tlas = r->GetTlasAddress();
	if ( tlas == 0 ) {
		return;		// no RT scene (non-RT hardware, or the world build hasn't run yet)
	}
	rhi::ShaderHandle prog = r->LoadShader( "rtao_ray" );
	if ( !prog ) {
		return;
	}
	// The traced rays are only ever CONSUMED through the NRD denoise + resolve below, which needs the
	// velocity MRT (r_motionVectors / r_fsr). Without it the screen-space GTAO
	// chain runs instead (RB_RHI_SSAOPass) - so do not trace a full-res ray pass first just to throw it
	// away: that was the one case where BOTH AO producers cost GPU time in the same frame. The raw-ray debug
	// overlay (r_rtaoDebug 1) is the only consumer of an un-denoised ray buffer.
	{
		const bool canDenoise = rhiNormalVelThisView
			&& r->GetRenderTargetImage3( rhiNormalResultRT ) != 0;
		if ( !canDenoise && r_rtaoDebug.GetInteger() != 1 ) {
			return;
		}
	}

	const int w = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int h = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
	// r_rtaoResScale: trace the rays AND run the NRD denoiser (the bulk of RTAO's cost) at a
	// fraction of the screen, then bilinearly upscale the denoised occlusion in the resolve. AO is
	// low-frequency so a fractional scale is fine here. 1.0 -> rw==w: the path below is byte-identical.
	const float resScale = idMath::ClampFloat( 0.25f, 1.0f, r_rtaoResScale.GetFloat() );
	int rw = (int)( w * resScale + 0.5f ), rh = (int)( h * resScale + 0.5f );
	if ( rw < 1 ) rw = 1;
	if ( rh < 1 ) rh = 1;
	bool rtaoScaled = ( rw != w || rh != h );
	// scaled velocity for NRD's IN_MV FIRST: NRD runs at the ray resolution, so its motion-vector
	// input must match. Downsample the full-res velocity (normal prepass attachment) with a plain
	// copy. If the target can't be allocated, fall back to full res this frame (a full-res velocity
	// must never feed a scaled NRD).
	if ( rhiRtaoVelRT && r->GetRenderTargetImage( rhiRtaoVelRT ) == 0 ) { rhiRtaoVelRT = 0; rhiRtaoVelW = rhiRtaoVelH = 0; }
	if ( rhiRtaoVelRT && ( !rtaoScaled || rhiRtaoVelW != rw || rhiRtaoVelH != rh ) ) {
		r->DestroyRenderTarget( rhiRtaoVelRT ); rhiRtaoVelRT = 0; rhiRtaoVelW = rhiRtaoVelH = 0;
	}
	if ( rtaoScaled && !rhiRtaoVelRT ) {
		rhiRtaoVelRT = r->CreateRenderTarget( rhi::IF_RG16F, rw, rh );
		if ( rhiRtaoVelRT ) { rhiRtaoVelW = rw; rhiRtaoVelH = rh; }
		else { rw = w; rh = h; rtaoScaled = false; }		// OOM: this frame runs full res
	}
	// (re)create the R16F ray target on first use / resize / scale change / lost context
	if ( rhiRtaoRayRT && r->GetRenderTargetImage( rhiRtaoRayRT ) == 0 ) {
		rhiRtaoRayRT = 0;
		rhiRtaoW = rhiRtaoH = 0;
	}
	// The ray target is a 3-attachment MRT: the ray pass writes the two NRD guides itself (view z @1, packed
	// world normal @2) - it already fetches depth + normal per pixel - instead of two more fullscreen passes.
	if ( rhiRtaoRayRT && ( rhiRtaoW != rw || rhiRtaoH != rh ) ) {
		r->DestroyRenderTarget( rhiRtaoRayRT );
		rhiRtaoRayRT = 0;
		// the resolve target is full-res (it upscales); recreate it only if truly lost. But NRD is
		// sized to the ray resolution, so a scale change must reset its history (old-res reprojection).
		rhiRtaoPrevValid = false;
	}
	if ( !rhiRtaoRayRT ) {
		rhiRtaoRayRT = r->CreateRenderTargetRtaoMrt( rw, rh );
		if ( !rhiRtaoRayRT ) {
			return;
		}
		rhiRtaoW = rw;
		rhiRtaoH = rh;
	}

	float invView[16];
	if ( !RB_RHI_InvertMatrix( viewDef->worldSpace.modelViewMatrix, invView ) ) {
		return;
	}

	const float *proj  = viewDef->projectionMatrix;
	const float invP00 = ( proj[0] != 0.0f ) ? 1.0f / proj[0] : 1.0f;
	const float invP11 = ( proj[5] != 0.0f ) ? 1.0f / proj[5] : 1.0f;
	const float radius = r_rtaoRadius.GetFloat();

	// per-frame index for rtao_ray's IGN reorientation (the temporal accumulation sees a differently
	// oriented dither each frame - no 45-degree residue to survive the r_rtaoResScale upscale)
	const float rtaoFrameIndex = (float)( tr.frameCount & 0xFFFF );

	const int uploadW = globalImages->currentDepthImage->uploadWidth;
	const int uploadH = globalImages->currentDepthImage->uploadHeight;

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	memcpy( parms.modelViewMatrix, invView, sizeof( invView ) );	// u_modelViewMatrix = view->world
	// TLAS device address: bit-cast into the rtParms lanes (ssr_rt idiom, NOT a value cast)
	const unsigned int tlasLo = (unsigned int)( tlas & 0xFFFFFFFFull );
	const unsigned int tlasHi = (unsigned int)( tlas >> 32 );
	memcpy( &parms.rtParms[0], &tlasLo, sizeof( tlasLo ) );
	memcpy( &parms.rtParms[1], &tlasHi, sizeof( tlasHi ) );
	parms.localParam0[0] = invP00;
	parms.localParam0[1] = invP11;
	parms.localParam0[2] = radius;
	parms.localParam0[3] = 0.5f * radius;			// hitDist normalization A — MUST match the
	parms.localParam1[1] = 0.1f;					// ... ReblurHitDistanceParameters (H4c)
	parms.localParam1[0] = rtaoFrameIndex;
	parms.localParam1[3] = 0.004f;					// ray origin pulled toward the eye, per unit of view depth (no crease leak; see rtao_ray.frag)
	// the ray pass runs at rw x rh: gl_FragCoord -> [0,1] uv is 1/rw, and -> the full-res depth tc
	// carries the (w/rw) stretch (mirrors RB_RHI_SSAOPass's scaled march). At scale 1 this is 1/w, 1/uploadW.
	parms.screenCorrection[0] = 1.0f / rw;
	parms.screenCorrection[1] = 1.0f / rh;
	parms.depthTexRecip[0] = ( (float)w / rw ) / uploadW;
	rhi::FillDepthParms( parms, backEnd.viewDef ? backEnd.viewDef->projectionMatrix : NULL );	// this view's depth -> view z pair (cinematics cram the near plane)
	parms.depthTexRecip[1] = ( (float)h / rh ) / uploadH;
	parms.windowCoord[2] = -1.0f;					// VK-only pass: top-down view-Y sign

	r->BeginTargetPass( rhiRtaoRayRT, NULL );
	RB_RHI_BindUnit( 0, globalImages->currentDepthImage );
	RB_RHI_BindRTUnit( r, 1, rhiNormalResultRT );
	backEnd.glState.tmu[1].current2DMap = -1;		// direct bind bypassed the tmu cache
	RB_RHI_DrawFullscreen( r, prog, parms, 0 );
	r->EndPass();
	rhiRtaoRanThisView = true;

	// ---- H4b guide inputs + H4c NRD denoise (docs/rtx-rtao.md) ----
	// The denoiser needs motion vectors for its reprojection; without the velocity MRT
	// (r_motionVectors / r_fsr off) only the raw ray pass runs (r_rtaoDebug 1).
	rhi::ImageHandle velImg = ( rhiNormalVelThisView && rhiNormalReadyThisView )
		? r->GetRenderTargetImage3( rhiNormalResultRT ) : 0;
	if ( velImg != 0 ) {
		// (H4b guide inputs: written by the ray pass above into attachments 1/2 of rhiRtaoRayRT)

		// r_rtaoResScale: NRD's IN_MV must match the ray/denoise resolution. Downsample the full-res
		// velocity into rhiRtaoVelRT with a plain copy (smaa_copy = texture(src, uv)). u_screenCorrection
		// (1,1) makes it sample the whole source; the smaller target bilinearly downsamples.
		rhi::ImageHandle nrdVel = velImg;
		if ( rtaoScaled && rhiRtaoVelRT != 0 ) {
			rhi::ShaderHandle copyProg = r->LoadShader( "smaa_copy" );
			if ( copyProg != 0 ) {
				rhi::RenderParams cp;
				memset( &cp, 0, sizeof( cp ) );
				cp.mvpMatrix[0] = cp.mvpMatrix[5] = cp.mvpMatrix[10] = cp.mvpMatrix[15] = 1.0f;
				cp.screenCorrection[0] = 1.0f; cp.screenCorrection[1] = 1.0f;
				r->BeginTargetPass( rhiRtaoVelRT, NULL );
				RB_RHI_DrawFullscreen( r, copyProg, cp, velImg );
				r->EndPass();
				nrdVel = r->GetRenderTargetImage( rhiRtaoVelRT );
			}
		}

		// H4c: the ReBLUR denoise on the frame command buffer
		rhi::RtaoDenoiseArgs da;
		memset( &da, 0, sizeof( da ) );
		da.rayRT = rhiRtaoRayRT;
		da.viewzRT  = rhiRtaoRayRT;		da.viewzAtt  = 1;		// the guides are attachments 1/2 of the ray target
		da.normalRT = rhiRtaoRayRT;		da.normalAtt = 2;
		da.velImage = nrdVel;
		da.w = rw;
		da.h = rh;
		// NRD requires NON-JITTERED matrices (the FSR2 Halton jitter lives in the shear
		// terms of projectionMatrix); the jitter itself rides cameraJitter below
		memcpy( da.viewToClip, viewDef->unjitteredProjectionMatrix, sizeof( da.viewToClip ) );
		memcpy( da.worldToView, viewDef->worldSpace.modelViewMatrix, sizeof( da.worldToView ) );
		memcpy( da.viewToClipPrev, rhiRtaoPrevValid ? rhiRtaoPrevProj : da.viewToClip, sizeof( da.viewToClipPrev ) );
		memcpy( da.worldToViewPrev, rhiRtaoPrevValid ? rhiRtaoPrevView : da.worldToView, sizeof( da.worldToViewPrev ) );
		// jitter: engine +Y-up pixels -> the top-left convention (the FSR2-verified mapping). NRD's
		// cameraJitter is in pixels of the render (ray) resolution, so scale by resScale when reduced.
		da.jitterX = -viewDef->jitter[0] * resScale;
		da.jitterY = viewDef->jitter[1] * resScale;
		da.jitterPrevX = rhiRtaoPrevValid ? -rhiRtaoPrevJitter[0] * resScale : da.jitterX;
		da.jitterPrevY = rhiRtaoPrevValid ?  rhiRtaoPrevJitter[1] * resScale : da.jitterY;
		// engine velocity = (currUV - prevUV) in +Y-up UV; NRD wants prevUV = uv + mv in
		// top-down UV: x negates, y's two flips cancel -> scale (-1, +1)
		da.mvScaleX = -1.0f;
		da.mvScaleY = 1.0f;
		da.hitDistA = 0.5f * radius;			// mirrors rtao_ray.frag's normalization
		da.hitDistB = 0.1f;
		da.denoisingRange = 20000.0f;			// < the ~30000 sky depth: sky excluded
		da.frameIndex = (unsigned int)tr.frameCount;
		da.reset = !rhiRtaoPrevValid;
		// RTAO ReBLUR accumulation window (r_rtaoHistory): 0 keeps NRD's default (~30), >0 overrides
		// it so the 1-spp occlusion can accumulate longer (smoother, less noise) or shorter (snappier).
		da.maxAccumFrames = r_rtaoHistory.GetInteger();
		rhiRtaoDenoisedThisView = r->RtaoDenoise( da );

		// ---- H4d: composite into the STANDARD AO buffer and hand it to the lighting ----
		// After this, RB_RHI_SSAOPass (which runs next) sees rhiRtaoDenoisedThisView and
		// skips the whole GTAO chain — RTAO owns this view's AO.
		rhi::ShaderHandle resProg = rhiRtaoDenoisedThisView ? r->LoadShader( "rtao_resolve" ) : 0;
		if ( resProg != 0 && r->GetRtaoOutputImage() != 0 ) {
			// the resolve target is FULL-res (it upscales the scaled NRD output) - independent of the ray/NRD
			// scale, so it recreates only on a lost context or a real view resize
			if ( rhiRtaoResolveRT && r->GetRenderTargetImage( rhiRtaoResolveRT ) == 0 ) { rhiRtaoResolveRT = 0; rhiRtaoResolveW = rhiRtaoResolveH = 0; }
			if ( rhiRtaoResolveRT && ( rhiRtaoResolveW != w || rhiRtaoResolveH != h ) ) {
				r->DestroyRenderTarget( rhiRtaoResolveRT ); rhiRtaoResolveRT = 0; rhiRtaoResolveW = rhiRtaoResolveH = 0;
			}
			if ( rhiRtaoResolveRT == 0 ) {
				rhiRtaoResolveRT = r->CreateRenderTargetMipped( rhi::IF_RGBA8, w, h, 1 );
				if ( rhiRtaoResolveRT ) { rhiRtaoResolveW = w; rhiRtaoResolveH = h; }
			}
			if ( rhiRtaoResolveRT != 0 ) {
				rhi::RenderParams rp;
				memset( &rp, 0, sizeof( rp ) );
				rp.mvpMatrix[0] = rp.mvpMatrix[5] = rp.mvpMatrix[10] = rp.mvpMatrix[15] = 1.0f;
				// RTAO's own intensity (default neutral 1.0) — r_ssaoIntensity's 1.2 compensates
				// for the screen-space march and stays GTAO-only (user decision 2026-09-17)
				rp.localParam0[0] = RB_RHI_AoIntensityWithVolumetrics( r, r_rtaoIntensity.GetFloat() );
				rp.screenCorrection[0] = 1.0f / w;
				rp.screenCorrection[1] = 1.0f / h;
				rp.depthTexRecip[0] = 1.0f / uploadW;
				rhi::FillDepthParms( rp, backEnd.viewDef ? backEnd.viewDef->projectionMatrix : NULL );	// this view's depth -> view z pair (cinematics cram the near plane)
				rp.depthTexRecip[1] = 1.0f / uploadH;
				r->BeginTargetPass( rhiRtaoResolveRT, NULL );
				RB_RHI_BindRTUnit( r, 1, rhiNormalResultRT );
				backEnd.glState.tmu[1].current2DMap = -1;
				RB_RHI_BindUnit( 2, globalImages->currentDepthImage );
				RB_RHI_DrawFullscreen( r, resProg, rp, r->GetRtaoOutputImage() );
				r->EndPass();

				// the consumers' contract (identical to the GTAO tail of RB_RHI_SSAOPass)
				rhiSsaoResultRT = rhiRtaoResolveRT;
				rhiSsaoViewW = w;
				rhiSsaoViewH = h;
				rhiSsaoAppliedThisView = true;
			} else {
				rhiRtaoDenoisedThisView = false;	// no resolve target -> let GTAO run instead
			}
		} else if ( rhiRtaoDenoisedThisView ) {
			rhiRtaoDenoisedThisView = false;		// no resolve program -> let GTAO run instead
		}
	} else {
		// bring-up observability: the debug overlay falls back to the raw rays when the
		// denoise can't run, which looks like "denoiser broken" — say why, once
		static bool warnedPrereq = false;
		if ( !warnedPrereq ) {
			warnedPrereq = true;
			common->Warning( "RTAO: denoise prerequisite missing (velocity buffer: r_motionVectors / r_fsr)" );
		}
	}
	// stage this frame's camera for next frame's reprojection (un-jittered, as passed)
	memcpy( rhiRtaoPrevProj, viewDef->unjitteredProjectionMatrix, sizeof( rhiRtaoPrevProj ) );
	memcpy( rhiRtaoPrevView, viewDef->worldSpace.modelViewMatrix, sizeof( rhiRtaoPrevView ) );
	rhiRtaoPrevJitter[0] = viewDef->jitter[0];
	rhiRtaoPrevJitter[1] = viewDef->jitter[1];
	rhiRtaoPrevValid = true;

	RB_RHI_ForgetTexBinds();
	backEnd.currentScissor = viewDef->scissor;
}

/*
===================
RB_RHI_RtgiPass

H6a of RT GI (docs/rtx-rtgi.md): the noisy radiance-gather pass. One cosine-weighted hemisphere ray
per pixel against the scene TLAS - the SAME ray as RTAO, but the hit is SHADED (bindless albedo *
scene lights + emissive, lifted from ssr_rt) and its incoming radiance returned into an RGBA16F target
instead of an occlusion scalar. Produces the noisy INPUT for the H6b NRD DIFFUSE denoise; for now the
only consumer is the r_rtgiDebug overlay, so with r_rtgi 0 (default) the pass never runs and both
normal play and the shipped RTAO path are untouched. VK + RT hardware only; shares RTAO's TLAS
(r_rtgi joined the RT-world implication gate + the per-frame-rebuild exception in tr_main).
===================
*/
static void RB_RHI_RtgiPass( rhi::RHI *r, const viewDef_t *viewDef ) {
	rhiRtgiRanThisView = false;
	rhiRtgiDenoisedThisView = false;	// reset on every early-out (the apply gates on it)
	rhiRtgiAppliedThisView = false;		// re-armed each view: the apply fires once from whichever site is first
	if ( !r_rtgi.GetBool() || rhi::GetActiveBackendType() != rhi::BT_VULKAN ) {
		return;
	}
	const bool fullscreenView = viewDef->isWindowView;	// the primary scene view (set by RenderScene)
	if ( !viewDef->viewEntitys || viewDef->isSubview || !fullscreenView ) {
		return;
	}
	if ( globalImages->currentDepthImage->uploadWidth <= 0
			|| !rhiNormalReadyThisView || rhiNormalResultRT == 0 ) {
		return;		// needs the sealed depth + the bump-mapped normal G-buffer (same as RTAO)
	}
	const unsigned long long tlas = r->GetTlasAddress();
	const unsigned long long geoTable = r->GetRtGeoTableAddress();
	if ( tlas == 0 || geoTable == 0 ) {
		return;		// no RT scene / geo table yet (non-RT hardware, or the world build hasn't run)
	}
	rhi::ShaderHandle prog = r->LoadShader( "rtgi_ray" );
	if ( !prog ) {
		return;
	}

	const int w = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int h = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
	// r_rtgiResScale: trace the gather at a fraction of the screen - bounce light is very low-frequency, so
	// a reduced-res gather + bilinear upscale is nearly free of visible quality but the biggest cost lever (a
	// full-res single sample was ~4 ms). The debug overlay upscales via smaa_copy; H6b's NRD runs at this res.
	// H6g checkerboard: trace a full-res horizontal checkerboard (half the pixels, packed left) instead of a
	// uniform downscale — sharp edges at ~half the rays. Needs the velocity MRT (NRD IN_MV) and a full-res
	// guide pass; falls back to resScale if the velocity is missing. When active the gather target is FULL res
	// (the left half holds the packed samples), so the resScale downscale is bypassed.
	const bool cbActive = r_rtgiCheckerboard.GetBool() && rhiNormalVelThisView && rhiNormalReadyThisView;
	const float resScale = idMath::ClampFloat( 0.25f, 1.0f, r_rtgiResScale.GetFloat() );
	// resScale is the RTGI internal resolution for BOTH paths; checkerboard just traces half of those pixels
	// (so its sharp-edge win is greatest at resScale 1, softer as resScale drops - it is a res, not a fixed).
	int rw = (int)( w * resScale + 0.5f );
	int rh = (int)( h * resScale + 0.5f );
	if ( rw < 1 ) rw = 1;
	if ( rh < 1 ) rh = 1;
	if ( cbActive ) { rw &= ~1; if ( rw < 2 ) rw = 2; }		// even width: the 2x1 checkerboard packs cleanly into the left half
	// (re)create the RGBA16F radiance MRT on first use / resize / scale change / lost context.
	if ( rhiRtgiRayRT && r->GetRenderTargetImage( rhiRtgiRayRT ) == 0 ) {
		rhiRtgiRayRT = 0; rhiRtgiW = rhiRtgiH = 0;
	}
	if ( rhiRtgiRayRT && ( rhiRtgiW != rw || rhiRtgiH != rh ) ) {
		r->DestroyRenderTarget( rhiRtgiRayRT ); rhiRtgiRayRT = 0; rhiRtgiW = rhiRtgiH = 0;
	}
	if ( !rhiRtgiRayRT ) {
		rhiRtgiRayRT = r->CreateRenderTargetRtgiMrt( rw, rh );
		if ( !rhiRtgiRayRT ) {
			return;
		}
		rhiRtgiW = rw; rhiRtgiH = rh;
	}

	float invView[16];
	if ( !RB_RHI_InvertMatrix( viewDef->worldSpace.modelViewMatrix, invView ) ) {
		return;
	}
	const float *proj  = viewDef->projectionMatrix;
	const float invP00 = ( proj[0] != 0.0f ) ? 1.0f / proj[0] : 1.0f;
	const float invP11 = ( proj[5] != 0.0f ) ? 1.0f / proj[5] : 1.0f;
	const float radius = idMath::ClampFloat( 8.0f, 4096.0f, r_rtgiRadius.GetFloat() );
	const float giFrameIndex = (float)( tr.frameCount & 0xFFFF );	// IGN reorientation, same as RTAO
	const int uploadW = globalImages->currentDepthImage->uploadWidth;
	const int uploadH = globalImages->currentDepthImage->uploadHeight;

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	memcpy( parms.modelViewMatrix, invView, sizeof( invView ) );	// u_modelViewMatrix = view->world
	const unsigned int tlasLo = (unsigned int)( tlas & 0xFFFFFFFFull );
	const unsigned int tlasHi = (unsigned int)( tlas >> 32 );
	memcpy( &parms.rtParms[0], &tlasLo, sizeof( tlasLo ) );			// rtParms.x/y = TLAS (bit-cast)
	memcpy( &parms.rtParms[1], &tlasHi, sizeof( tlasHi ) );
	memcpy( &parms.rtParms[2], &geoTable, sizeof( geoTable ) );		// rtParms.z/w = geo table (bit-cast)
	parms.localParam0[0] = invP00;
	parms.localParam0[1] = invP11;
	parms.localParam0[2] = radius;
	parms.localParam0[3] = idMath::ClampFloat( 0.0f, 16.0f, r_rtgiEmissiveClamp.GetFloat() );	// emissive peak clamp (pre-NRD variance cut)
	parms.localParam1[0] = giFrameIndex;
	parms.localParam1[1] = r_rtgiShadow.GetBool() ? 1.0f : 0.0f;	// H6d: dominant-light shadow ray at the bounce hit
	// H6g checkerboard parity: 0 = off (resScale path), 1 = even frame, 2 = odd frame. The gather packs the
	// left half of the rw x rh buffer accordingly; NRD's constant BLACK mode + frameIndex reconstructs it (the
	// screenCorrection/depthTexRecip below are the same resScale mapping). MUST use the same frameIndex NRD gets.
	parms.localParam1[2] = cbActive ? (float)( 1 + ( (int)tr.frameCount & 1 ) ) : 0.0f;
	parms.localParam1[3] = 0.004f;					// ray origin pulled toward the eye per unit depth (crease-leak fix)
	// H6g golden-angle IGN reorientation, hoisted out of the shader: the per-frame lattice rotation (cos/sin)
	// and the hashed per-frame offset depend only on the frame index, so computing them once here beats a
	// per-pixel cos+sin+3-round-PCG-hash in every traced fragment (rtgi_ray.frag). Bit-for-bit the same inputs
	// the shader used (uint(giFrameIndex), the same PCG constants, the golden-ratio sequence masked to 255).
	// Delivered in u_particleRadius (.xy = cos/sin, .zw = offset); the shader applies the mat2 rotate + add.
	{
		const unsigned int fi = (unsigned int)giFrameIndex;			// == tr.frameCount & 0xFFFF, matching uint(u_localParam1.x)
		unsigned int fh = fi * 747796405u + 2891336453u;			// PCG hash (same rounds as the old shader)
		fh = ( ( fh >> ( ( fh >> 28u ) + 4u ) ) ^ fh ) * 277803737u;
		fh = ( fh >> 22u ) ^ fh;
		float gAng = (float)( fi & 255u ) * 0.61803399f;			// golden-ratio sequence -> low-discrepancy angle
		gAng = ( gAng - idMath::Floor( gAng ) ) * 6.28318531f;		// fract() then *2pi, as the shader did
		parms.particleRadius[0] = idMath::Cos( gAng );
		parms.particleRadius[1] = idMath::Sin( gAng );
		parms.particleRadius[2] = (float)( ( fh >> 3u ) & 1023u );
		parms.particleRadius[3] = (float)( ( fh >> 13u ) & 1023u );
	}
	// the gather runs at rw x rh: gl_FragCoord -> [0,1] uv is 1/rw, and -> the full-res depth tc carries
	// the (w/rw) stretch (mirrors RB_RHI_RtaoPass / RB_RHI_SSAOPass's scaled march). At scale 1: 1/w, 1/uploadW.
	parms.screenCorrection[0] = 1.0f / rw;
	parms.screenCorrection[1] = 1.0f / rh;
	parms.depthTexRecip[0] = ( (float)w / rw ) / uploadW;
	rhi::FillDepthParms( parms, backEnd.viewDef ? backEnd.viewDef->projectionMatrix : NULL );	// this view's depth -> view z pair
	parms.depthTexRecip[1] = ( (float)h / rh ) / uploadH;
	parms.windowCoord[2] = -1.0f;					// VK-only pass: top-down view-Y sign

	// scene lights to shade the hit (shared buffer with RT reflections / volumetrics; cached per view)
	unsigned long long giLightAddr = 0;
	const int giNumLights = RB_RHI_GatherSceneLights( r, viewDef, &giLightAddr );
	memcpy( &parms.color[0], &giLightAddr, sizeof( giLightAddr ) );	// color.xy = light-buffer device address
	parms.color[2] = (float)giNumLights;
	parms.color[3] = 0.0f;							// no ambient floor at the hit: a pure bounce (only lit surfaces contribute)
	parms.diffuseModifier[0] = idMath::ClampFloat( 0.0f, 16.0f, r_rtgiEmissive.GetFloat() );	// self-illum glow scale
	parms.diffuseModifier[1] = idMath::ClampFloat( 0.0f, 8.0f, r_rtgiLightScale.GetFloat() );	// diffuse brightness at the hit
	parms.diffuseModifier[2] = 1.0f;				// confine each light to its Doom projection volume (RR16c-style)
	parms.diffuseModifier[3] = ( giNumLights > 0 ) ? 1.0f : 0.0f;	// real-lights enable

	r->GpuScopeBegin( "rtgi.ray" );					// r_vkGpuTime 2: the one-bounce ray trace + hit shading
	// Checkerboard packs every traced sample into the LEFT HALF of the buffer; NRD (BLACK mode, x>>1) and
	// the confidence pass read only that half, so the right half is written and immediately thrown away —
	// its fragments sample depth+normal and early-out (rtgi_ray.frag) for nothing. Restrict the draw to the
	// left half: mathematically identical (the right half is unread), skipping ~half the ray-pass fragments.
	// The shader keeps its own right-half early-out as the correctness guarantee if a backend ignores this.
	// Skipped under r_rtgiDebug (the overlay blits the whole gather buffer, so the right half must stay valid).
	if ( cbActive && r_rtgiDebug.GetInteger() == 0 ) {
		r->SetNextTargetPassArea( 0, 0, rw / 2, rh );
	}
	r->BeginTargetPass( rhiRtgiRayRT, NULL );
	RB_RHI_BindUnit( 0, globalImages->currentDepthImage );
	RB_RHI_BindRTUnit( r, 1, rhiNormalResultRT );
	backEnd.glState.tmu[1].current2DMap = -1;		// direct bind bypassed the tmu cache
	RB_RHI_DrawFullscreen( r, prog, parms, 0 );		// set 2 (bindless materials) is auto-bound by the backend
	r->EndPass();
	r->GpuScopeEnd();
	rhiRtgiRanThisView = true;
	rhiRtgiDenoisedThisView = false;

	// H6g: full-res NRD guides for checkerboard (the half-packed gather can't carry them). Ray-free pass over
	// depth + normal into a full-res MRT (att1 viewZ, att2 normal), which the denoise points NRD's guides at.
	rhi::RenderTargetHandle giGuideRT = 0;
	if ( cbActive ) {
		// the guides are NRD's "full resolution" for checkerboard = the rw x rh gather resolution (the radiance
		// is the half-packed part). Same coordinate mapping as the gather so guide and radiance line up.
		if ( rhiRtgiGuideRT && r->GetRenderTargetImage( rhiRtgiGuideRT ) == 0 ) { rhiRtgiGuideRT = 0; rhiRtgiGuideW = rhiRtgiGuideH = 0; }
		if ( rhiRtgiGuideRT && ( rhiRtgiGuideW != rw || rhiRtgiGuideH != rh ) ) { r->DestroyRenderTarget( rhiRtgiGuideRT ); rhiRtgiGuideRT = 0; rhiRtgiGuideW = rhiRtgiGuideH = 0; }
		if ( !rhiRtgiGuideRT ) {
			rhiRtgiGuideRT = r->CreateRenderTargetRtgiMrt( rw, rh );
			if ( rhiRtgiGuideRT ) { rhiRtgiGuideW = rw; rhiRtgiGuideH = rh; }
		}
		rhi::ShaderHandle guideProg = rhiRtgiGuideRT ? r->LoadShader( "rtgi_guides" ) : 0;
		if ( guideProg != 0 ) {
			rhi::RenderParams gp;
			memset( &gp, 0, sizeof( gp ) );
			gp.mvpMatrix[0] = gp.mvpMatrix[5] = gp.mvpMatrix[10] = gp.mvpMatrix[15] = 1.0f;
			memcpy( gp.modelViewMatrix, invView, sizeof( invView ) );	// u_modelViewMatrix = view->world (normal encode)
			gp.screenCorrection[0] = 1.0f / rw;	gp.screenCorrection[1] = 1.0f / rh;
			gp.depthTexRecip[0] = ( (float)w / rw ) / uploadW;	gp.depthTexRecip[1] = ( (float)h / rh ) / uploadH;
			r->GpuScopeBegin( "rtgi.guides" );		// r_vkGpuTime 2: full-res NRD guide fill (checkerboard only)
			r->BeginTargetPass( rhiRtgiGuideRT, NULL );
			RB_RHI_BindUnit( 0, globalImages->currentDepthImage );
			RB_RHI_BindRTUnit( r, 1, rhiNormalResultRT );
			backEnd.glState.tmu[1].current2DMap = -1;
			RB_RHI_DrawFullscreen( r, guideProg, gp, 0 );
			r->EndPass();
			r->GpuScopeEnd();
			giGuideRT = rhiRtgiGuideRT;
		}
	}

	// ---- H6b: NRD REBLUR_DIFFUSE denoise (docs/rtx-rtgi.md), mirroring RB_RHI_RtaoPass's denoise ----
	// Needs the velocity MRT (r_fsr / r_motionVectors) for IN_MV; without it the raw gather is used (grainy).
	rhi::ImageHandle giVelImg = ( rhiNormalVelThisView && rhiNormalReadyThisView )
		? r->GetRenderTargetImage3( rhiNormalResultRT ) : 0;
	if ( giVelImg != 0 ) {
		rhi::ImageHandle nrdVel = giVelImg;
		if ( rw != w || rh != h ) {
			// downsample the full-res velocity to the GI ray/NRD resolution (smaa_copy = texture(src, uv))
			if ( rhiRtgiVelRT && r->GetRenderTargetImage( rhiRtgiVelRT ) == 0 ) { rhiRtgiVelRT = 0; rhiRtgiVelW = rhiRtgiVelH = 0; }
			if ( rhiRtgiVelRT && ( rhiRtgiVelW != rw || rhiRtgiVelH != rh ) ) { r->DestroyRenderTarget( rhiRtgiVelRT ); rhiRtgiVelRT = 0; rhiRtgiVelW = rhiRtgiVelH = 0; }
			if ( !rhiRtgiVelRT ) {
				rhiRtgiVelRT = r->CreateRenderTarget( rhi::IF_RG16F, rw, rh );
				if ( rhiRtgiVelRT ) { rhiRtgiVelW = rw; rhiRtgiVelH = rh; }
			}
			rhi::ShaderHandle copyProg = rhiRtgiVelRT ? r->LoadShader( "smaa_copy" ) : 0;
			if ( copyProg != 0 ) {
				rhi::RenderParams cp;
				memset( &cp, 0, sizeof( cp ) );
				cp.mvpMatrix[0] = cp.mvpMatrix[5] = cp.mvpMatrix[10] = cp.mvpMatrix[15] = 1.0f;
				cp.screenCorrection[0] = 1.0f; cp.screenCorrection[1] = 1.0f;
				r->GpuScopeBegin( "rtgi.vel" );		// r_vkGpuTime 2: velocity downsample to GI res (resScale only)
				r->BeginTargetPass( rhiRtgiVelRT, NULL );
				RB_RHI_DrawFullscreen( r, copyProg, cp, giVelImg );
				r->EndPass();
				r->GpuScopeEnd();
				nrdVel = r->GetRenderTargetImage( rhiRtgiVelRT );
			}
		}

		// H6f: history-confidence pass (docs/rtx-rtgi.md). Compare this frame's raw bounce luminance against
		// last frame's DENOISED GI reprojected by the motion vectors, and write a per-pixel confidence that
		// tells NRD to drop history where the lighting moved (a lit silhouette under the camera) but keep it
		// on stable surfaces — killing the edge smear without the flat dimming a global antilag would cause.
		// Needs a prior denoised frame (GetRtgiOutputImage != 0, still last frame's until RtaoDenoise below
		// overwrites it) and the velocity we just resolved. r_rtgiConfidence 0 disables it (NRD antilag only).
		rhi::ImageHandle giConfImg = 0;
		const rhi::ImageHandle giPrevDenoised = r->GetRtgiOutputImage();
		// Runs for both paths. In checkerboard the raw is half-packed, so the shader (u_localParam0.w = parity)
		// only computes confidence at this frame's TRACED cells (reading the packed sample) and trusts history
		// on the reconstructed cells.
		if ( r_rtgiConfidence.GetFloat() > 0.0f && rhiRtgiPrevValid && giPrevDenoised != 0 ) {
			if ( rhiRtgiConfRT && r->GetRenderTargetImage( rhiRtgiConfRT ) == 0 ) { rhiRtgiConfRT = 0; rhiRtgiConfW = rhiRtgiConfH = 0; }
			if ( rhiRtgiConfRT && ( rhiRtgiConfW != rw || rhiRtgiConfH != rh ) ) { r->DestroyRenderTarget( rhiRtgiConfRT ); rhiRtgiConfRT = 0; rhiRtgiConfW = rhiRtgiConfH = 0; }
			if ( !rhiRtgiConfRT ) {
				rhiRtgiConfRT = r->CreateRenderTarget( rhi::IF_R16F, rw, rh );	// R8+ per NRD; R16F is the single-channel target we have
				if ( rhiRtgiConfRT ) { rhiRtgiConfW = rw; rhiRtgiConfH = rh; }
			}
			rhi::ShaderHandle confProg = rhiRtgiConfRT ? r->LoadShader( "rtgi_confidence" ) : 0;
			if ( confProg != 0 ) {
				rhi::RenderParams confParms;
				memset( &confParms, 0, sizeof( confParms ) );
				confParms.mvpMatrix[0] = confParms.mvpMatrix[5] = confParms.mvpMatrix[10] = confParms.mvpMatrix[15] = 1.0f;
				confParms.screenCorrection[0] = 1.0f / rw;			// texel size for the 3x3 tap of the noisy current signal
				confParms.screenCorrection[1] = 1.0f / rh;
				confParms.localParam0[0] = idMath::ClampFloat( 0.0f, 16.0f, r_rtgiConfidence.GetFloat() );	// sensitivity
				confParms.localParam0[1] = 0.1f;					// confidence floor: never fully reject history (avoids edge frying)
				confParms.localParam0[2] = 0.35f;					// noise deadzone: ignore sub-noise relative diffs so stable surfaces keep full history
				confParms.localParam0[3] = cbActive ? (float)( 1 + ( (int)tr.frameCount & 1 ) ) : 0.0f;	// H6g: checkerboard parity (0 = off, packed-read for traced cells)
				confParms.windowCoord[2] = -1.0f;					// VK top-down: flip the velocity row (ssr_temporal convention)
				r->GpuScopeBegin( "rtgi.conf" );		// r_vkGpuTime 2: H6f history-confidence pass
				r->BeginTargetPass( rhiRtgiConfRT, NULL );
				RB_RHI_BindRTImage( r, 1, giPrevDenoised );			// unit 1 = last frame's denoised GI (YCoCg)
				RB_RHI_BindRTImage( r, 2, nrdVel );					// unit 2 = motion vectors at GI res
				RB_RHI_DrawFullscreen( r, confProg, confParms, r->GetRenderTargetImage( rhiRtgiRayRT ) );	// unit 0 = this frame's raw gather
				r->EndPass();
				r->GpuScopeEnd();
				giConfImg = r->GetRenderTargetImage( rhiRtgiConfRT );
			}
		}

		rhi::RtaoDenoiseArgs da;
		memset( &da, 0, sizeof( da ) );
		da.giMode = true;								// denoise GI radiance (REBLUR_DIFFUSE), not RTAO occlusion
		da.rayRT = rhiRtgiRayRT;						// att0 = RGBA16F radiance+hitdist; guides in att1/att2
		// resScale path: guides ride in the gather MRT (att1/att2). Checkerboard: radiance is half-packed, so
		// the guides come from the full-res rtgi_guides pass instead (same att1/att2 layout). da.checkerboard =
		// constant BLACK (1); the gather already packed the left half to match NRD's frameIndex parity.
		const bool cbDenoise = cbActive && giGuideRT != 0;
		da.viewzRT  = cbDenoise ? giGuideRT : rhiRtgiRayRT;	da.viewzAtt  = 1;
		da.normalRT = cbDenoise ? giGuideRT : rhiRtgiRayRT;	da.normalAtt = 2;
		da.checkerboard = cbDenoise ? 1 : 0;			// 1 = BLACK (constant); NRD flips per frameIndex
		da.velImage = nrdVel;
		da.w = rw;	da.h = rh;
		memcpy( da.viewToClip,  viewDef->unjitteredProjectionMatrix, sizeof( da.viewToClip ) );
		memcpy( da.worldToView, viewDef->worldSpace.modelViewMatrix, sizeof( da.worldToView ) );
		memcpy( da.viewToClipPrev,  rhiRtgiPrevValid ? rhiRtgiPrevProj : da.viewToClip,  sizeof( da.viewToClipPrev ) );
		memcpy( da.worldToViewPrev, rhiRtgiPrevValid ? rhiRtgiPrevView : da.worldToView, sizeof( da.worldToViewPrev ) );
		da.jitterX = -viewDef->jitter[0] * resScale;
		da.jitterY =  viewDef->jitter[1] * resScale;
		da.jitterPrevX = rhiRtgiPrevValid ? -rhiRtgiPrevJitter[0] * resScale : da.jitterX;
		da.jitterPrevY = rhiRtgiPrevValid ?  rhiRtgiPrevJitter[1] * resScale : da.jitterY;
		da.mvScaleX = -1.0f;	da.mvScaleY = 1.0f;
		da.hitDistA = 0.5f * radius;	da.hitDistB = 0.1f;		// MUST match rtgi_ray.frag's normHitDist
		da.maxAccumFrames = r_rtgiHistory.GetInteger();			// GI-only ReBLUR history cap (ghosting vs noise lever)
		da.confImage = giConfImg;								// H6f: IN_DIFF_CONFIDENCE (0 = not provided this frame)
		da.denoisingRange = 20000.0f;
		da.frameIndex = (unsigned int)tr.frameCount;
		da.reset = !rhiRtgiPrevValid;
		r->GpuScopeBegin( "rtgi.denoise" );						// r_vkGpuTime 2: NRD REBLUR_DIFFUSE denoise dispatches
		rhiRtgiDenoisedThisView = r->RtaoDenoise( da );			// giMode -> GI diffuse denoise into GetRtgiOutputImage
		r->GpuScopeEnd();
	}
	// stage this frame's camera for next frame's GI reprojection (un-jittered, as passed)
	memcpy( rhiRtgiPrevProj, viewDef->unjitteredProjectionMatrix, sizeof( rhiRtgiPrevProj ) );
	memcpy( rhiRtgiPrevView, viewDef->worldSpace.modelViewMatrix, sizeof( rhiRtgiPrevView ) );
	rhiRtgiPrevJitter[0] = viewDef->jitter[0];
	rhiRtgiPrevJitter[1] = viewDef->jitter[1];
	rhiRtgiPrevValid = true;

	RB_RHI_ForgetTexBinds();
	backEnd.currentScissor = viewDef->scissor;
}

/*
===================
RB_RHI_RtShadowBlurBegin / RB_RHI_RtShadowBlurLight

r_rtShadowBlur (docs/rtx-shadow-blur.md): a screen-space blur ON TOP of the hard ray-traced
shadows the engine already has (r_rtSunShadows, r_rtMovingLights - shader mode 4, one inline
ray per lit fragment). Which lights are ray-served, and everything about every other light, is
untouched; with the cvar off nothing here runs.

A forward, per-light renderer multiplies the shadow term into the light inside the interaction
shader, so "blur the shadow" needs the shadow as an image first. Per blurred light, right before
its interactions draw:
  1. rtshadow_ray   - the mode-4 ray for every screen pixel in the light's rect (closest hit, so
                      the occluder distance sizes the penumbra). Same ray count as the inline
                      path; the interactions then skip theirs.
  2. rtshadow_tiles - 8x8 tile classification (raw, then dilated by the blur's reach)
  3. rtshadow_blur  - horizontal then vertical; early-outs on one tile fetch wherever no shadow
                      edge is within reach, which is nearly everywhere
The mask is deterministic and lives for one light of one frame: no history, no noise.

Begin: once per view, before the light loop. Needs the normal G-buffer (ray-origin offset, the
view-weapon mask) - present whenever SSAO / RTAO / motion vectors run their prepass; without it
(or on any other failure) rhiRtBlurThisView stays false and every light keeps its inline ray.

Light: must NOT RB_RHI_ForgetTexBinds() - the loop's AO buffer rides unit 9 for every light.
Units 0-1 are rebound by each interaction draw (Vulkan binds record unconditionally).
===================
*/
static void RB_RHI_RtShadowBlurBegin( rhi::RHI *r, const viewDef_t *viewDef ) {
	rhiRtBlurThisView = false;
	if ( !r_rtShadowBlur.GetBool() || rhi::GetActiveBackendType() != rhi::BT_VULKAN
			|| r_skipInteractions.GetBool()
			|| !( r_rtSunShadows.GetBool() || r_rtMovingLights.GetBool() || r_rtAllLights.GetBool() ) ) {
		return;
	}
	const bool fullscreenView = viewDef->isWindowView;	// the primary scene view (set by RenderScene)
	if ( !viewDef->viewEntitys || viewDef->isSubview || !fullscreenView ) {
		return;		// the mask is addressed by gl_FragCoord of the main view
	}
	if ( globalImages->currentDepthImage->uploadWidth <= 0
			|| !rhiNormalReadyThisView || rhiNormalResultRT == 0 || r->GetTlasAddress() == 0 ) {
		return;
	}
	rhiRtBlur.rayProg  = r->LoadShader( "rtshadow_ray" );
	rhiRtBlur.tileProg = r->LoadShader( "rtshadow_tiles" );
	rhiRtBlur.blurProg = r->LoadShader( "rtshadow_blur" );
	if ( !rhiRtBlur.rayProg || !rhiRtBlur.tileProg || !rhiRtBlur.blurProg
			|| !RB_RHI_InvertMatrix( viewDef->worldSpace.modelViewMatrix, rhiRtBlur.invView ) ) {
		return;
	}

	const int w = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int h = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
	if ( rhiRtBlurRayRT && r->GetRenderTargetImage( rhiRtBlurRayRT ) == 0 ) {
		rhiRtBlurRayRT = rhiRtBlurPingRT = rhiRtBlurMaskRT = rhiRtBlurTileRT[0] = rhiRtBlurTileRT[1] = 0;	// lost context
	}
	rhiRtBlurFrame++;
	myGlMultMatrix( viewDef->worldSpace.modelViewMatrix, viewDef->projectionMatrix, rhiRtBlur.viewProj );
	// staggered-mask slots: gone with a resize / lost context / the option; idle ones are freed
	for ( int i = 0; i < RT_BLUR_MAX_SLOTS; i++ ) {
		rtBlurSlot_t &sl = rhiRtBlurSlots[i];
		if ( sl.lastUsedFrame == 0 ) {
			continue;
		}
		const bool lost = sl.maskRT && r->GetRenderTargetImage( sl.maskRT ) == 0;
		if ( lost || w != rhiRtBlurW || h != rhiRtBlurH || !r_rtShadowBlurStagger.GetBool()
				|| rhiRtBlurFrame - sl.lastUsedFrame > RT_BLUR_SLOT_IDLE_FRAMES ) {
			if ( sl.maskRT && !lost ) { r->DestroyRenderTarget( sl.maskRT ); }
			memset( &sl, 0, sizeof( sl ) );
		}
	}
	if ( w != rhiRtBlurW || h != rhiRtBlurH ) {
		if ( rhiRtBlurRayRT )     { r->DestroyRenderTarget( rhiRtBlurRayRT );     rhiRtBlurRayRT = 0; }
		if ( rhiRtBlurPingRT )    { r->DestroyRenderTarget( rhiRtBlurPingRT );    rhiRtBlurPingRT = 0; }
		if ( rhiRtBlurMaskRT )    { r->DestroyRenderTarget( rhiRtBlurMaskRT );    rhiRtBlurMaskRT = 0; }
		if ( rhiRtBlurTileRT[0] ) { r->DestroyRenderTarget( rhiRtBlurTileRT[0] ); rhiRtBlurTileRT[0] = 0; }
		if ( rhiRtBlurTileRT[1] ) { r->DestroyRenderTarget( rhiRtBlurTileRT[1] ); rhiRtBlurTileRT[1] = 0; }
		rhiRtBlurW = w;
		rhiRtBlurH = h;
	}
	const int tw = ( w + 7 ) / 8, th = ( h + 7 ) / 8;
	if ( !rhiRtBlurRayRT ) {
		// the MRT variant carries the RG8 summary attachment for the fast tile pass; fall back to
		// the plain RGBA16F target (summary attachment absent -> the tile pass reads attachment 0)
		rhiRtBlurRayRT = r->CreateRenderTargetBlurRay( w, h );
		if ( !rhiRtBlurRayRT ) { rhiRtBlurRayRT = r->CreateRenderTargetMipped( rhi::IF_RGBA16F, w, h, 1 ); }
	}
	if ( !rhiRtBlurPingRT )    { rhiRtBlurPingRT    = r->CreateRenderTargetMipped( rhi::IF_RGBA16F, w, h, 1 ); }
	if ( !rhiRtBlurMaskRT )    { rhiRtBlurMaskRT    = r->CreateRenderTargetMipped( rhi::IF_R16F, w, h, 1 ); }
	if ( !rhiRtBlurTileRT[0] ) { rhiRtBlurTileRT[0] = r->CreateRenderTargetMipped( rhi::IF_RGBA8, tw, th, 1 ); }
	if ( !rhiRtBlurTileRT[1] ) { rhiRtBlurTileRT[1] = r->CreateRenderTargetMipped( rhi::IF_RGBA8, tw, th, 1 ); }
	rhiRtBlurThisView = rhiRtBlurRayRT && rhiRtBlurPingRT && rhiRtBlurMaskRT && rhiRtBlurTileRT[0] && rhiRtBlurTileRT[1];

}

// The screen rect the blur actually has to cover for one light. vLight->scissorRect is the union of
// its surfaces' SCISSORS, and a surface's scissor is its whole area's portal rect - so one wall of
// the room the viewer stands in makes the light "full screen" even if the lit surfaces fill a
// corner of it. The inline mode-4 ray never paid for that (it only runs on the fragments of lit
// surfaces); a screen-space pass does, for every pass. So: the union of the PROJECTED BOUNDS of the
// opaque interaction surfaces (the only fragments that sample the mask - translucent ones keep the
// inline ray), each clipped to its scissor. A surface crossing the near plane keeps its scissor.
static idScreenRect RB_RHI_RtShadowBlurRect( const viewDef_t *viewDef, const viewLight_t *vLight, int w, int h ) {
	idScreenRect u;
	u.Clear();
	const float *p = viewDef->projectionMatrix;
	const drawSurf_t *chains[2] = { vLight->localInteractions, vLight->globalInteractions };
	for ( int c = 0; c < 2; c++ ) {
		for ( const drawSurf_t *surf = chains[c]; surf; surf = surf->nextOnLight ) {
			idScreenRect sr = surf->scissorRect;
			const srfTriangles_t *tri = surf->geo;
			if ( tri != NULL && surf->space != NULL && !tri->bounds.IsCleared() ) {
				const float *mv = surf->space->modelViewMatrix;
				float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f, minW = 1e30f;
				bool behind = false;
				for ( int i = 0; i < 8 && !behind; i++ ) {
					const float x = tri->bounds[i & 1][0], y = tri->bounds[( i >> 1 ) & 1][1], z = tri->bounds[( i >> 2 ) & 1][2];
					const float ex = mv[0] * x + mv[4] * y + mv[8] * z + mv[12];
					const float ey = mv[1] * x + mv[5] * y + mv[9] * z + mv[13];
					const float ez = mv[2] * x + mv[6] * y + mv[10] * z + mv[14];
					const float cw = p[3] * ex + p[7] * ey + p[11] * ez + p[15];
					if ( !( cw > 1.0f ) ) {
						behind = true;		// at / behind the near plane: the projection is meaningless
						break;
					}
					const float px = ( ( p[0] * ex + p[4] * ey + p[8] * ez + p[12] ) / cw * 0.5f + 0.5f ) * (float)w;
					const float py = ( ( p[1] * ex + p[5] * ey + p[9] * ez + p[13] ) / cw * 0.5f + 0.5f ) * (float)h;
					minW = Min( minW, cw );
					minX = Min( minX, px );	maxX = Max( maxX, px );
					minY = Min( minY, py );	maxY = Max( maxY, py );
				}
				if ( !behind ) {
					// pad: sub-pixel jitter, plus 4 world units at the nearest corner for geometry the
					// GPU moves past its CPU bounds (tessellation displacement)
					const float pad = 8.0f + 4.0f * p[0] * 0.5f * (float)w / minW;
					idScreenRect br;
					br.x1 = (short)idMath::ClampFloat( 0.0f, (float)( w - 1 ), minX - pad );
					br.y1 = (short)idMath::ClampFloat( 0.0f, (float)( h - 1 ), minY - pad );
					br.x2 = (short)idMath::ClampFloat( 0.0f, (float)( w - 1 ), maxX + pad );
					br.y2 = (short)idMath::ClampFloat( 0.0f, (float)( h - 1 ), maxY + pad );
					sr.Intersect( br );
				}
			}
			if ( sr.x2 >= sr.x1 && sr.y2 >= sr.y1 ) {
				u.Union( sr );
			}
		}
	}
	u.Intersect( vLight->scissorRect );
	return u;
}

static rhi::ImageHandle RB_RHI_RtShadowBlurLight( rhi::RHI *r, const viewDef_t *viewDef, const viewLight_t *vLight ) {
	const unsigned long long tlas = r->GetTlasAddress();
	if ( tlas == 0 || !vLight->lightDef ) {
		return 0;
	}
	const int w = rhiRtBlurW, h = rhiRtBlurH;
	const idScreenRect lr = RB_RHI_RtShadowBlurRect( viewDef, vLight, w, h );
	if ( lr.x2 < lr.x1 || lr.y2 < lr.y1 ) {
		return 0;		// no opaque surface of this light on screen: nothing would sample a mask
	}

	// How wide the blur gets. The width follows the geometry - 0 where the caster touches the
	// receiver, growing with the gap: halfWidth = blurScale * dOccluder / ( dLight - dOccluder ) -
	// and r_rtShadowBlurIntensity scales it. blurScale is a length in world units (it is the radius
	// a physical light would need to cast that penumbra, but it is only a blur amount here): 3 at
	// intensity 1 for ordinary fixtures, growing with the light's own volume so a 5000-unit "sun
	// replacement" omni doesn't get a bulb-sized blur. Parallel suns sit 100000 units out along
	// their direction: 1 degree of angular radius at intensity 1.
	const renderLight_t &lp = vLight->lightDef->parms;
	const idVec3 &lorg = vLight->globalLightOrigin;
	float lightRadius = 3.0f * r_rtShadowBlurIntensity.GetFloat();
	if ( lp.parallel ) {
		lightRadius = ( lorg - viewDef->renderView.vieworg ).Length() * idMath::Tan( DEG2RAD( 1.0f ) ) * r_rtShadowBlurIntensity.GetFloat();
	} else if ( lp.pointLight ) {
		const float maxAxis = Max( lp.lightRadius.x, Max( lp.lightRadius.y, lp.lightRadius.z ) );
		lightRadius *= Max( 1.0f, maxAxis / 256.0f );
	}

	// r_rtShadowBlurMinPenumbra: skip the blur entirely for a light whose penumbra is sub-pixel. A
	// distant and/or small light casts a shadow edge finer than one screen pixel, so softening it is
	// invisible yet still pays the full per-light ray + tiles + separable blur. Leave such a light on
	// its crisp inline hard ray instead (the caller falls back to it when this returns 0, and any
	// stagger slot it held ages out and frees itself in RB_RHI_RtShadowBlurBegin). The estimate
	// mirrors the shader's own width - penumbra(px) ~= 2 * lightRadius(world) * (proj[0]*0.5*w) /
	// receiverDepth - with a unit occluder/receiver gap ratio as the representative case, and takes the
	// nearest the lit geometry can sit (origin minus the light's own radius) as the receiver depth so a
	// large nearby light is never wrongly skipped. Parallel suns keep a fixed ~1deg angular penumbra
	// everywhere, so they are never skipped.
	const float minPenumbra = r_rtShadowBlurMinPenumbra.GetFloat();
	if ( minPenumbra > 0.0f && !lp.parallel ) {
		float depth = ( lorg - viewDef->renderView.vieworg ).Length();
		if ( lp.pointLight ) {
			depth -= Max( lp.lightRadius.x, Max( lp.lightRadius.y, lp.lightRadius.z ) );
		}
		if ( depth < 1.0f ) { depth = 1.0f; }
		const float pxPerUnit = viewDef->projectionMatrix[0] * 0.5f * (float)w;	// at view depth 1
		const float penumbraPx = 2.0f * lightRadius * pxPerUnit / depth;			// full width, gap ratio ~1
		if ( penumbraPx < minPenumbra ) {
			return 0;		// sub-pixel penumbra: the hard inline shadow is visually identical
		}
	}

	rhiRtBlurLastStale = false;

	// ---- r_rtShadowBlurStagger: how often does this light's mask refresh? By the distance from the
	// viewer to the light's ORIGIN: nearer than StaggerNear every frame, up to StaggerFar every 2nd,
	// beyond every 3rd. (The first cut measured to the light's VOLUME, which is 0 for every light
	// you stand inside - indoors that is nearly all of them, so nothing was ever staggered and the
	// option saved nothing.) A parallel light's origin is a fiction 100000 units away and it shadows
	// everything around the viewer: it always refreshes every frame. Which frame a light refreshes
	// on is offset by its index, so the refreshes spread evenly instead of all landing on the same
	// frame. A mask is NEVER reused when the light itself moved or changed size, or when it is
	// older than its interval allows.
	rtBlurSlot_t *slot = NULL;
	if ( r_rtShadowBlurStagger.GetBool() && ictx.interactionRtProg != 0 ) {
		const float dist = vLight->lightDef->parms.parallel ? 0.0f
			: ( vLight->globalLightOrigin - viewDef->renderView.vieworg ).Length();
		const int interval = ( dist < r_rtShadowBlurStaggerNear.GetFloat() ) ? 1
		                   : ( dist < r_rtShadowBlurStaggerFar.GetFloat() ) ? 2 : 3;
		if ( interval > 1 ) {
			unsigned long long tok = 1469598103934665603ULL;
			tok = RB_RHI_HashBytes( tok, vLight->globalLightOrigin.ToFloatPtr(), 3 * sizeof( float ) );
			tok = RB_RHI_HashBytes( tok, vLight->lightDef->parms.lightCenter.ToFloatPtr(), 3 * sizeof( float ) );
			tok = RB_RHI_HashBytes( tok, &vLight->lightDef->parms.axis, sizeof( idMat3 ) );
			tok = RB_RHI_HashBytes( tok, vLight->lightDef->parms.lightRadius.ToFloatPtr(), 3 * sizeof( float ) );
			const int lightIndex = vLight->lightDef->index;
			int freeSlot = -1;
			for ( int i = 0; i < RT_BLUR_MAX_SLOTS; i++ ) {
				if ( rhiRtBlurSlots[i].lastUsedFrame != 0 && rhiRtBlurSlots[i].lightIndex == lightIndex ) {
					slot = &rhiRtBlurSlots[i];
					break;
				}
				if ( rhiRtBlurSlots[i].lastUsedFrame == 0 && freeSlot < 0 ) {
					freeSlot = i;
				}
			}
			if ( slot == NULL && freeSlot >= 0 ) {
				slot = &rhiRtBlurSlots[freeSlot];
				memset( slot, 0, sizeof( *slot ) );
				slot->lightIndex = lightIndex;
			}
			if ( slot != NULL ) {
				if ( !slot->maskRT ) {
					slot->maskRT = r->CreateRenderTargetMipped( rhi::IF_RGBA8, w, h, 1 );
					slot->lastUpdateFrame = 0;
				}
				if ( !slot->maskRT ) {
					slot = NULL;		// out of memory: this light simply refreshes every frame
				}
			}
			if ( slot != NULL ) {
				slot->lastUsedFrame = rhiRtBlurFrame;
				const int age = rhiRtBlurFrame - slot->lastUpdateFrame;
				const bool scheduled = ( ( rhiRtBlurFrame + lightIndex ) % interval ) == 0;
				const bool reusable = slot->lastUpdateFrame != 0 && age < interval && slot->poseTok == tok && !scheduled;
				if ( reusable ) {
					rhiRtBlurLastStale = true;
					memcpy( rhiRtBlurLastViewProj, slot->viewProj, sizeof( rhiRtBlurLastViewProj ) );
					return r->GetRenderTargetImage( slot->maskRT );
				}
				slot->poseTok = tok;
			}
		}
	}
	const int blurTaps = 24;		// = K in rtshadow_blur.frag = HW_MAX in rtshadow_tiles.frag: the widest half-width, pixels

	// lp / lorg / lightRadius (the world-space blur scale) were computed above, before the stagger
	// and sub-pixel-penumbra early-outs.
	const float *proj = viewDef->projectionMatrix;
	const unsigned int tlasLo = (unsigned int)( tlas & 0xFFFFFFFFull );
	const unsigned int tlasHi = (unsigned int)( tlas >> 32 );
	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	memcpy( parms.modelViewMatrix, rhiRtBlur.invView, sizeof( rhiRtBlur.invView ) );	// view -> world
	memcpy( &parms.rtParms[0], &tlasLo, sizeof( tlasLo ) );		// bit-cast, NOT a value cast
	memcpy( &parms.rtParms[1], &tlasHi, sizeof( tlasHi ) );
	parms.rtParms[2] = r_rtSunShadowOffset.GetFloat();			// ray-origin normal offset, shared with mode 4
	parms.localLightOrigin[0] = lorg.x;		// WORLD space here (fullscreen pass, no model)
	parms.localLightOrigin[1] = lorg.y;
	parms.localLightOrigin[2] = lorg.z;
	parms.localLightOrigin[3] = 1.0f;
	memcpy( parms.lightProjectionS, vLight->lightProject[0].ToFloatPtr(), 4 * sizeof( float ) );
	memcpy( parms.lightProjectionT, vLight->lightProject[1].ToFloatPtr(), 4 * sizeof( float ) );
	memcpy( parms.lightProjectionQ, vLight->lightProject[2].ToFloatPtr(), 4 * sizeof( float ) );
	memcpy( parms.lightFalloffS,    vLight->lightProject[3].ToFloatPtr(), 4 * sizeof( float ) );
	parms.localParam0[0] = ( proj[0] != 0.0f ) ? 1.0f / proj[0] : 1.0f;
	parms.localParam0[1] = ( proj[5] != 0.0f ) ? 1.0f / proj[5] : 1.0f;
	parms.localParam0[2] = lightRadius;
	parms.localParam0[3] = proj[0] * 0.5f * (float)w;			// world units -> pixels at view distance 1
	parms.localParam1[0] = (float)blurTaps;
	parms.localParam1[1] = proj[8];		// the projection's shear terms = this frame's FSR2 jitter
	parms.localParam1[2] = proj[9];
	parms.localParam1[3] = r_rtShadowBlurCurve.GetFloat();	// how the width grows with the gap (1 = geometric)
	RB_RHI_FillPerforatedParms( r, parms, vLight );
	parms.screenCorrection[0] = 1.0f / w;
	parms.screenCorrection[1] = 1.0f / h;
	parms.depthTexRecip[0] = 1.0f / globalImages->currentDepthImage->uploadWidth;
	rhi::FillDepthParms( parms, backEnd.viewDef ? backEnd.viewDef->projectionMatrix : NULL );	// this view's depth -> view z pair (cinematics cram the near plane)
	parms.depthTexRecip[1] = 1.0f / globalImages->currentDepthImage->uploadHeight;
	// "lit, nothing to spread, never traced": what the big targets clear to (color targets
	// always clear on begin) and what the tile + blur passes skip
	rhi::ClearArgs clearLit;
	memset( &clearLit, 0, sizeof( clearLit ) );
	clearLit.color = true;
	clearLit.rgba[0] = 1.0f;
	rhi::ClearArgs clearZero;
	memset( &clearZero, 0, sizeof( clearZero ) );
	clearZero.color = true;

	// The rects, all GL convention (bottom-up; the backend converts). Each stage reads blurTaps
	// pixels beyond what the next one writes:
	//   vRect  = the light's rect + 3            what the interactions sample; vertical blur writes it
	//   hRect  = vRect grown by blurTaps in y    horizontal blur writes it (the vertical pass reads that far)
	//   rayRect = the light's rect + blurTaps + 3, grown to whole 8x8 TILES (tiles count from the
	//            TOP row, so the alignment is done in top-down rows) - rays write it, and the tile
	//            classification reads exactly these tiles, never a pixel outside
	// Every pass on a view-sized target is restricted to its rect (SetNextTargetPassArea): with
	// every light ray-traced (r_rtAllLights) a view holds many of these per frame, and three
	// full-target clears per light were the bulk of a small light's cost. Outside a pass's area
	// the target is undefined - which is why no stage may read beyond the previous one's rect.
	const int wide = blurTaps + 3, tight = 3;
	const int vx1 = Max( 0, lr.x1 - tight ), vx2 = Min( w - 1, lr.x2 + tight );
	const int vy1 = Max( 0, lr.y1 - tight ), vy2 = Min( h - 1, lr.y2 + tight );
	const int hy1 = Max( 0, lr.y1 - wide ),  hy2 = Min( h - 1, lr.y2 + wide );
	const int rx1 = ( Max( 0, lr.x1 - wide ) / 8 ) * 8;
	const int rx2 = Min( w - 1, ( Min( w - 1, lr.x2 + wide ) / 8 ) * 8 + 7 );
	const int rTop = ( ( h - 1 - Min( h - 1, lr.y2 + wide ) ) / 8 ) * 8;				// top-down rows
	const int rBot = Min( h - 1, ( ( h - 1 - Max( 0, lr.y1 - wide ) ) / 8 ) * 8 + 7 );
	const int ry1 = h - 1 - rBot, ry2 = h - 1 - rTop;								// back to GL rows

	// No ray where the light is black: a light's volume is a box, its projection * falloff usually
	// round, so the box's corners receive exactly nothing - shadowed or not. The ray pass can test
	// that itself when the light has ONE active stage with a plain 2D projection image (no texture
	// matrix: a rotating gobo's lookup isn't the raw lightProject planes; no cinematic). Anything
	// else keeps tracing the whole volume, as before.
	idImage *atProjImg = globalImages->whiteImage;
	idImage *atFalloffImg = vLight->falloffImage ? vLight->falloffImage : globalImages->whiteImage;
	{
		const idMaterial *ls = vLight->lightShader;
		const float *lregs = vLight->shaderRegisters;
		const shaderStage_t *only = NULL;
		int active = 0;
		for ( int i = 0; ls && lregs && i < ls->GetNumStages(); i++ ) {
			const shaderStage_t *st = ls->GetStage( i );
			if ( lregs[st->conditionRegister] != 0 ) {
				only = st;
				active++;
			}
		}
		if ( active == 1 && vLight->falloffImage && only->texture.image && !only->texture.hasMatrix
				&& !only->texture.cinematic && only->texture.image->type == TT_2D ) {
			atProjImg = only->texture.image;
			parms.rtParms[3] = 1.0f;
		}
	}

	// r_rtShadowBlurCheckerboard: trace a static screen checkerboard (half the rays, u_color.x). The skipped
	// pixels (ray target b < 0) are reconstructed INSIDE the horizontal blur pass from their two traced
	// horizontal neighbours (always the opposite parity in a (x^y) checkerboard), so there is no separate
	// fill pass; the tile classification already ignores holes (they read as "never traced").
	const bool cbActive = r_rtShadowBlurCheckerboard.GetBool();
	parms.color[0] = cbActive ? 1.0f : 0.0f;					// u_color.x = checkerboard enable (rtshadow_ray.frag)

	r->GpuScopeBegin( "rtblur.ray" );
	r->SetNextTargetPassArea( rx1, ry1, rx2 - rx1 + 1, ry2 - ry1 + 1 );
	r->BeginTargetPass( rhiRtBlurRayRT, &clearLit );
	r->SetScissor( rx1, ry1, rx2 - rx1 + 1, ry2 - ry1 + 1 );
	RB_RHI_BindUnit( 0, globalImages->currentDepthImage );
	RB_RHI_BindRTUnit( r, 1, rhiNormalResultRT );
	RB_RHI_BindUnit( 2, atFalloffImg );
	RB_RHI_BindUnit( 3, atProjImg );
	RB_RHI_DrawFullscreen( r, rhiRtBlur.rayProg, parms, 0 );
	r->EndPass();
	r->GpuScopeEnd();

	const rhi::RenderTargetHandle srcRT = rhiRtBlurRayRT;		// tiles + the horizontal blur read the (checkerboard) ray target directly

	rhi::RenderParams tp;
	memset( &tp, 0, sizeof( tp ) );
	tp.mvpMatrix[0] = tp.mvpMatrix[5] = tp.mvpMatrix[10] = tp.mvpMatrix[15] = 1.0f;
	for ( int pass = 0; pass < 2; pass++ ) {
		// exactly the ray rect's tiles, both passes (pass 1 reads 64 ray texels per tile; pass 2's
		// reach search reads tiles beyond them, which the full clear of these small targets left
		// at "no blur"). The tile targets are 1/64 of the view: they clear whole.
		const int th = ( h + 7 ) / 8;
		const int tyTop = rTop / 8, tyBot = rBot / 8;
		tp.localParam0[0] = (float)pass;
		// pass 0 reads the ray pass's compact RG8 summary (attachment 1, 2 B/texel) instead of the
		// 8-byte RGBA16F, when the MRT target carried it and r_rtShadowBlurFastTiles is on
		const rhi::ImageHandle summaryImg = r->GetRenderTargetImage2( srcRT );
		const bool fastTiles = ( pass == 0 ) && r_rtShadowBlurFastTiles.GetBool() && summaryImg != 0;
		tp.localParam0[1] = fastTiles ? 1.0f : 0.0f;
		r->GpuScopeBegin( "rtblur.tiles" );
		r->BeginTargetPass( rhiRtBlurTileRT[pass], &clearZero );
		r->SetScissor( rx1 / 8, th - 1 - tyBot, rx2 / 8 - rx1 / 8 + 1, tyBot - tyTop + 1 );
		RB_RHI_DrawFullscreen( r, rhiRtBlur.tileProg, tp,
			( pass == 0 ) ? ( fastTiles ? summaryImg : r->GetRenderTargetImage( srcRT ) )
			              : r->GetRenderTargetImage( rhiRtBlurTileRT[0] ) );
		r->EndPass();
		r->GpuScopeEnd();
	}

	rhi::RenderParams bp;
	memset( &bp, 0, sizeof( bp ) );
	bp.mvpMatrix[0] = bp.mvpMatrix[5] = bp.mvpMatrix[10] = bp.mvpMatrix[15] = 1.0f;
	for ( int pass = 0; pass < 2; pass++ ) {
		const int py1 = ( pass == 0 ) ? hy1 : vy1, py2 = ( pass == 0 ) ? hy2 : vy2;
		bp.localParam0[0] = ( pass == 0 ) ? 1.0f : 0.0f;		// axis
		bp.localParam0[1] = ( pass == 0 ) ? 0.0f : 1.0f;
		bp.localParam0[3] = cbActive ? 1.0f : 0.0f;			// checkerboard: the horizontal pass reconstructs skipped pixels
		// a staggered light's final pass goes into ITS mask, cleared WHOLE (no area): outside this
		// frame's rect it must read "lit, view distance 1" = no valid data, never an older frame's
		// leftovers - a later frame may reproject anywhere onto it
		const bool intoSlot = ( pass == 1 && slot != NULL );
		bp.localParam0[2] = intoSlot ? 1.0f : 0.0f;			// pack the view distance into GB
		r->GpuScopeBegin( ( pass == 0 ) ? "rtblur.blurH" : ( intoSlot ? "rtblur.blurV(slot)" : "rtblur.blurV" ) );
		if ( !intoSlot ) {
			r->SetNextTargetPassArea( vx1, py1, vx2 - vx1 + 1, py2 - py1 + 1 );
		}
		r->BeginTargetPass( ( pass == 0 ) ? rhiRtBlurPingRT : ( intoSlot ? slot->maskRT : rhiRtBlurMaskRT ), &clearLit );
		r->SetScissor( vx1, py1, vx2 - vx1 + 1, py2 - py1 + 1 );
		RB_RHI_BindRTUnit( r, 1, rhiRtBlurTileRT[1] );
		RB_RHI_DrawFullscreen( r, rhiRtBlur.blurProg, bp,
			r->GetRenderTargetImage( ( pass == 0 ) ? srcRT : rhiRtBlurPingRT ) );
		r->EndPass();
		r->GpuScopeEnd();
	}

	// drop the tile target from the recorded units: the next blurred light renders INTO it, and a
	// later draw that doesn't rebind unit 1 must not carry an attachment along
	RB_RHI_BindRTImage( r, 1, 0 );
	backEnd.currentScissor = viewDef->scissor;	// the loop re-applies the light's scissor right after
	if ( slot != NULL ) {
		slot->lastUpdateFrame = rhiRtBlurFrame;
		memcpy( slot->viewProj, rhiRtBlur.viewProj, sizeof( slot->viewProj ) );
		return r->GetRenderTargetImage( slot->maskRT );
	}
	return r->GetRenderTargetImage( rhiRtBlurMaskRT );
}

/*
===================
RB_RHI_RtaoDebugOverlay

r_rtaoDebug visualization: blit the RTAO chain's intermediates over the finished view
(1 = the raw noisy ray output; 2 = the NRD-denoised result once H4c lands). Reuses the
ssao_debug fullscreen program (mode 1 shows the red channel as the AO scalar).
===================
*/
void RB_RHI_RtaoDebugOverlay( rhi::RHI *r, const viewDef_t *viewDef ) {
	const int mode = r_rtaoDebug.GetInteger();
	if ( mode <= 0 || !rhiRtaoRanThisView || rhiRtaoRayRT == 0 ) {
		return;
	}
	// 1 = the raw noisy ray output; 2 = the NRD-denoised result (falls back to raw
	// while the denoiser hasn't run — no velocity buffer, non-RT hardware, etc.)
	rhi::ImageHandle srcImg = r->GetRenderTargetImage( rhiRtaoRayRT );
	if ( mode >= 2 && rhiRtaoDenoisedThisView && r->GetRtaoOutputImage() != 0 ) {
		srcImg = r->GetRtaoOutputImage();
	}
	if ( srcImg == 0 ) {
		return;
	}
	rhi::ShaderHandle prog = r->LoadShader( "ssao_debug" );
	if ( !prog ) {
		return;
	}
	const int w = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int h = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	parms.localParam0[0] = 1.0f;					// ssao_debug mode 1: show the AO scalar (.r)

	r->SetViewport( tr.viewportOffset[0] + viewDef->viewport.x1,
	                tr.viewportOffset[1] + viewDef->viewport.y1, w, h );
	r->SetScissor( tr.viewportOffset[0] + viewDef->viewport.x1,
	               tr.viewportOffset[1] + viewDef->viewport.y1, w, h );
	backEnd.currentScissor = viewDef->scissor;

	RB_RHI_DrawFullscreen( r, prog, parms, srcImg );
	RB_RHI_ForgetTexBinds();
}

/*
===================
RB_RHI_RtgiApply

H6c of RT GI (docs/rtx-rtgi.md): add one bounce of indirect diffuse into the lit HDR scene, scene-wide.
For each pixel indirect = receiver albedo (the RTGI prepass's 4th attachment, GetRenderTargetImage4) *
the gathered irradiance (rtgi_ray.frag: raw in H6a, NRD-denoised in H6b). Additive into the live rhiHdrRT,
after world lighting and before FSR2/tonemap, so DoF/FSR2/bloom pick it up. No-op unless r_rtgi is on and
both the gather and the albedo attachment were produced this view. VK + RT hardware only.
===================
*/
void RB_RHI_RtgiApply( rhi::RHI *r, const viewDef_t *viewDef ) {
	if ( !r_rtgi.GetBool() || r_rtgiDebug.GetInteger() > 0 || rhi::GetActiveBackendType() != rhi::BT_VULKAN ) {
		return;			// off, or the debug overlay owns the screen instead of the apply
	}
	if ( rhiRtgiAppliedThisView ) {
		return;			// already added this view (called before the FSR2 snapshot; the post-chain site is a no-op)
	}
	if ( !rhiRtgiRanThisView || rhiRtgiRayRT == 0 || !rhiNormalAlbedoThisView ) {
		return;			// no gather / no receiver-albedo attachment this view
	}
	// H6b: the DENOISED GI radiance (GetRtgiOutputImage) when the NRD pass ran this view; else the raw gather.
	const rhi::ImageHandle giImg = rhiRtgiDenoisedThisView ? r->GetRtgiOutputImage()
		: r->GetRenderTargetImage( rhiRtgiRayRT );
	const rhi::ImageHandle albedoImg = r->GetRenderTargetImage4( rhiNormalResultRT );
	if ( giImg == 0 || albedoImg == 0 ) {
		return;
	}
	rhi::ShaderHandle prog = r->LoadShader( "rtgi_apply" );
	if ( !prog ) {
		return;
	}
	const int w = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int h = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
	const int uploadW = globalImages->currentDepthImage->uploadWidth;
	const int uploadH = globalImages->currentDepthImage->uploadHeight;

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	parms.localParam0[0] = idMath::ClampFloat( 0.0f, 8.0f, r_rtgiIntensity.GetFloat() );
	// GI (reduced res) + albedo (full res) both sample at gl_FragCoord/(viewW,viewH): the GI buffer
	// upscales, albedo is 1:1. depth uses the POT _currentDepth tc for the sky skip.
	parms.screenCorrection[0] = 1.0f / w;
	parms.screenCorrection[1] = 1.0f / h;
	parms.depthTexRecip[0] = 1.0f / uploadW;
	parms.depthTexRecip[1] = 1.0f / uploadH;
	// Joint-bilateral upscale (r_rtgiBilateralUpscale): only when the GI is actually reduced-res; at full res a
	// bilinear 1:1 fetch is exact and the bilateral would just self-blur. Needs the GI buffer size + the normal.
	const bool bilat = r_rtgiBilateralUpscale.GetBool() && ( rhiRtgiW < w || rhiRtgiH < h ) && rhiNormalResultRT != 0;
	parms.localParam0[1] = bilat ? 1.0f : 0.0f;
	parms.localParam0[2] = ( rhiRtgiW > 0 ) ? 1.0f / rhiRtgiW : 1.0f / w;	// GI reduced-res texel (2x2 footprint)
	parms.localParam0[3] = ( rhiRtgiH > 0 ) ? 1.0f / rhiRtgiH : 1.0f / h;
	parms.localParam1[0] = 24.0f;							// depth sensitivity: ~4% relative depth diff halves the weight
	parms.localParam1[1] = 8.0f;							// normal power: sharply rejects across creases

	// Fog-aware GI (r_rtgiFogAttenuate): this apply is additive and runs BEFORE the fog blend, so a far
	// surface buried in fog keeps its full bounce and the distance fog washes out. Fade the GI with the same
	// ramp Doom 3's fog image uses (alpha = 1 - 0.982^(255*d), d = 0.5*dist/fogDist), driven by the view's
	// primary fog light, so where the fog goes opaque the bounce goes with it. localParam1.z = strength,
	// localParam1.w = the exp() coef; both 0 (no attenuation) when attenuation is off or the view has no fog.
	parms.localParam1[2] = 0.0f;
	parms.localParam1[3] = 0.0f;
	const float fogAtten = idMath::ClampFloat( 0.0f, 4.0f, r_rtgiFogAttenuate.GetFloat() );
	if ( fogAtten > 0.0f ) {
		for ( const viewLight_t *vL = viewDef->viewLights; vL; vL = vL->next ) {
			if ( !vL->lightShader->IsFogLight() ) {
				continue;
			}
			const shaderStage_t *fs = vL->lightShader->GetStage( 0 );
			const float fogDensity = vL->shaderRegisters[fs->color.registers[3]];
			const float fogDist = ( fogDensity <= 1.0f ) ? DEFAULT_FOG_DISTANCE : fogDensity;
			// transmittance T = exp( dist * coef ); coef = 127.5 * ln(0.982) / fogDist reproduces the fog
			// image's ~90%-opaque-at-fogDist curve (0.982^(255 * 0.5*dist/fogDist)). The shader raises T to
			// the strength power, so strength > 1 overdrives the falloff (GI dies sooner than the fog is thick).
			parms.localParam1[2] = fogAtten;
			parms.localParam1[3] = -2.31587f / fogDist;
			break;		// first fog light; layered / localized fog is approximated by this primary volume
		}
	}

	RB_RHI_BindRTImage( r, 1, albedoImg );					// unit 1 = receiver albedo (prepass att3)
	RB_RHI_BindUnit( 2, globalImages->currentDepthImage );	// unit 2 = depth (sky skip + bilateral)
	// unit 3 = full-res view normal (bilateral weight). Bound even when bilat is off (the shader just doesn't
	// sample it then) so the descriptor set is always complete; falls back to depth if the normal RT is absent.
	RB_RHI_BindRTUnit( r, 3, rhiNormalResultRT != 0 ? rhiNormalResultRT : rhiRtgiRayRT );
	backEnd.glState.tmu[3].current2DMap = -1;				// direct bind bypassed the tmu cache
	// unit 0 = GI radiance (rtInput0); additive (ONE/ONE) into the live HDR scene target
	RB_RHI_DrawFullscreen( r, prog, parms, giImg, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );
	RB_RHI_ForgetTexBinds();
	rhiRtgiAppliedThisView = true;			// once per view: the other call site now no-ops
}

/*
===================
RB_RHI_RtgiDebugOverlay

r_rtgiDebug (H6a): blit the raw noisy radiance-gather buffer over the finished view so the GI gather can
be eyeballed before the NRD denoise (H6b) and the ambient apply (H6c) land. RGB radiance, so it uses
smaa_copy (a plain texture blit) rather than ssao_debug's scalar view; HDR values above 1 clip on the
LDR overlay, which is fine for a bring-up readout. No-op unless r_rtgi + the debug cvar are on and the
pass produced a buffer this view.
===================
*/
void RB_RHI_RtgiDebugOverlay( rhi::RHI *r, const viewDef_t *viewDef ) {
	if ( r_rtgiDebug.GetInteger() <= 0 || !rhiRtgiRanThisView || rhiRtgiRayRT == 0 ) {
		return;
	}
	// mode 1 = the raw noisy gather; mode 2 = the NRD-denoised output (falls back to raw when the denoise
	// didn't run). Both buffers are NRD-YCoCg-packed, so rtgi_debug decodes them to linear.
	rhi::ImageHandle srcImg = ( r_rtgiDebug.GetInteger() >= 2 && rhiRtgiDenoisedThisView && r->GetRtgiOutputImage() != 0 )
		? r->GetRtgiOutputImage()
		: r->GetRenderTargetImage( rhiRtgiRayRT );
	if ( srcImg == 0 ) {
		return;
	}
	rhi::ShaderHandle prog = r->LoadShader( "rtgi_debug" );
	if ( !prog ) {
		return;
	}
	const int w = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int h = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	parms.screenCorrection[0] = 1.0f;				// rtgi_debug samples at var_TexCoord (0..1); screenCorrection unused
	parms.screenCorrection[1] = 1.0f;

	r->SetViewport( tr.viewportOffset[0] + viewDef->viewport.x1,
	                tr.viewportOffset[1] + viewDef->viewport.y1, w, h );
	r->SetScissor( tr.viewportOffset[0] + viewDef->viewport.x1,
	               tr.viewportOffset[1] + viewDef->viewport.y1, w, h );
	backEnd.currentScissor = viewDef->scissor;

	RB_RHI_DrawFullscreen( r, prog, parms, srcImg );
	RB_RHI_ForgetTexBinds();
}

/*
===================
RB_RHI_SSAODebugOverlay

r_ssaoDebug visualization: blit the finished AO buffer over the scene (1 = AO scalar,
2 = bent normal). Called at the end of the 3D view, after the scene is drawn, so it
isn't overwritten by the light passes. No-op unless SSAO and the debug cvar are on.
===================
*/
void RB_RHI_SSAODebugOverlay( rhi::RHI *r, const viewDef_t *viewDef ) {
	const int mode = r_ssaoDebug.GetInteger();
	if ( mode <= 0 || !r_ssao.GetBool() || !R_BackendSupportsEnhancements() ) {
		return;
	}
	// Modes 1/2 show the AO result; mode 3 the raw normal G-buffer. When no AO producer ran this
	// view, rhiSsaoResultRT still points at a buffer from an earlier frame (its handle is never
	// cleared), so blitting it shows a stale, frozen image. That happens whenever RTGI is on:
	// it supersedes screen-space AO, so RB_RHI_SSAOPass (GTAO) and RB_RHI_RtaoPass both stand down
	// and rhiSsaoAppliedThisView stays false. Gate modes 1/2 on that flag so the AO channel shows
	// nothing rather than a stale frame (use r_rtgiDebug to inspect the GI). Mode 3 is unaffected —
	// the normal prepass still runs under RTGI (it feeds the GI receiver albedo).
	if ( mode < 3 && !rhiSsaoAppliedThisView ) {
		return;
	}
	rhi::RenderTargetHandle srcRT = ( mode >= 3 ) ? rhiNormalResultRT : rhiSsaoResultRT;
	if ( !srcRT || r->GetRenderTargetImage( srcRT ) == 0 ) {
		return;
	}
	rhi::ShaderHandle prog = r->LoadShader( "ssao_debug" );
	if ( !prog ) {
		return;
	}

	const int w = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int h = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	parms.localParam0[0] = (float)mode;		// 1 = AO, 2 = bent normal, 3 = normal G-buffer

	r->SetViewport( tr.viewportOffset[0] + viewDef->viewport.x1,
	                tr.viewportOffset[1] + viewDef->viewport.y1, w, h );
	r->SetScissor( tr.viewportOffset[0] + viewDef->viewport.x1,
	               tr.viewportOffset[1] + viewDef->viewport.y1, w, h );
	backEnd.currentScissor = viewDef->scissor;

	RB_RHI_DrawFullscreen( r, prog, parms, r->GetRenderTargetImage( srcRT ) );

	// direct bind above; forget the tmu cache so later 2D/GUI binds re-issue
	RB_RHI_ForgetTexBinds();
}

/*
===================
RB_RHI_DepthOfField

DUDE weapon-reload depth-of-field (r_dof). As the weapon reloads the game eases
r_weaponReloadFocus 0->1->0 (idPlayerView::RenderPlayerView); this pass keeps the
near, depth-hacked weapon sharp and blurs the world beyond it, scaled by that
envelope. A single-pass depth-gated disc blur (depthoffield.frag) over the
captured scene, drawn onto the live target at the end of the fullscreen 3D view
(weapon included) before the tonemap/HUD, so it works on both RHI backends and in
the HDR float path. Zero cost unless a reload is in progress (focus 0 early-outs).
===================
*/
void RB_RHI_DepthOfField( rhi::RHI *r, const viewDef_t *viewDef ) {
	if ( !r_dof.GetBool() || !R_BackendSupportsEnhancements() ) {
		return;
	}
	const float focus = idMath::ClampFloat( 0.0f, 1.0f, r_weaponReloadFocus.GetFloat() );
	if ( focus <= 0.0f ) {
		return;		// not reloading -> nothing to blur
	}
	const bool fullscreenView = viewDef->isWindowView;	// the primary scene view (set by RenderScene)
	if ( !viewDef->viewEntitys || viewDef->isSubview || !fullscreenView ) {
		return;		// primary world view only (not mirrors / GUI renderDefs)
	}

	rhi::ShaderHandle prog = r->LoadShader( "depthoffield" );
	if ( !prog ) {
		return;
	}

	const int w = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int h = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;

	// snapshot the finished scene colour and the sealed scene depth. The weapon
	// view model was drawn into both with the depth hack, so it sits in the near
	// depth band (reads as sharp) while the world beyond blurs.
	globalImages->currentRenderImage->CopyFramebuffer( viewDef->viewport.x1, viewDef->viewport.y1, w, h, true );
	globalImages->currentDepthImage->CopyDepthbuffer( viewDef->viewport.x1, viewDef->viewport.y1, w, h, true );

	const int colPotW = globalImages->currentRenderImage->uploadWidth  > 0 ? globalImages->currentRenderImage->uploadWidth  : w;
	const int colPotH = globalImages->currentRenderImage->uploadHeight > 0 ? globalImages->currentRenderImage->uploadHeight : h;
	const int depPotW = globalImages->currentDepthImage->uploadWidth   > 0 ? globalImages->currentDepthImage->uploadWidth   : w;
	const int depPotH = globalImages->currentDepthImage->uploadHeight  > 0 ? globalImages->currentDepthImage->uploadHeight  : h;

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	parms.screenCorrection[0] = (float)w / colPotW;			// viewport uv -> _currentRender
	parms.screenCorrection[1] = (float)h / colPotH;
	parms.localParam0[0] = focus;
	parms.localParam0[1] = idMath::ClampFloat( 0.0f, 64.0f, r_dofBlurRadius.GetFloat() );
	parms.localParam0[2] = r_dofFocusStart.GetFloat();
	parms.localParam0[3] = r_dofFocusEnd.GetFloat();
	parms.localParam1[0] = (float)w / depPotW;				// viewport uv -> _currentDepth
	parms.localParam1[1] = (float)h / depPotH;
	parms.localParam1[2] = 1.0f / (float)w;					// pixel -> uv step for the blur disc
	parms.localParam1[3] = 1.0f / (float)h;
	// Vulkan stores the DEPTH capture top-down (native) while the colour capture reads
	// upright under the same fullscreen-quad uv, so with one uv the depth is V-flipped
	// relative to the colour (the weapon reads far, the far world reads near). Flip ONLY
	// the depth sample's V on Vulkan; GL shares the framebuffer orientation for both, so
	// 0 there. (The colour capture stays unflipped — it already displays upright.)
	parms.windowCoord[0] = ( rhi::GetActiveBackendType() == rhi::BT_VULKAN ) ? 1.0f : 0.0f;

	r->SetViewport( tr.viewportOffset[0] + viewDef->viewport.x1,
	                tr.viewportOffset[1] + viewDef->viewport.y1, w, h );
	r->SetScissor( tr.viewportOffset[0] + viewDef->viewport.x1,
	               tr.viewportOffset[1] + viewDef->viewport.y1, w, h );
	backEnd.currentScissor = viewDef->scissor;

	RB_RHI_BindUnit( 0, globalImages->currentRenderImage );
	RB_RHI_BindUnit( 1, globalImages->currentDepthImage );
	RB_RHI_DrawFullscreen( r, prog, parms, globalImages->currentRenderImage->rhiHandle );

	// direct binds above; forget the tmu cache so later 2D/GUI binds re-issue
	RB_RHI_ForgetTexBinds();
}

/*
===================
RB_RHI_ChromaticAberration

DUDE chromatic aberration (r_postChromaticAberration) as a SCENE-ONLY pass at the
end of the fullscreen 3D view — the same spot, and the same copy-the-scene-then-
draw-it-back mechanic, as RB_RHI_DepthOfField above.

Split out of the swap-time resolve so the radial RGB fringe only touches the
rendered world. On Vulkan (always) and GL3 + r_hdr the scene lives in an offscreen
buffer and the HUD / console / FPS counter composite into it AFTER this point; the
resolve then tonemaps + grains scene and HUD together, so chroma folded into the
resolve was splitting the HUD text and the FPS counter too. Running it here, before
those 2D views land, keeps them clean. (Film grain stays full-frame in the resolve —
that's the conventional look and it doesn't fringe.)

Reuses postprocess.frag with grain forced to 0 (chroma-only); strength 0 collapses
the three taps to one fetch, so it's a passthrough. In an HDR frame the scene capture
is RGBA16F, so the split samples the un-banded float scene — the very reason chroma
was folded into the resolve originally. GL3 without r_hdr keeps using RB_RHI_PostProcess
(grain + chroma together), which already runs before the HUD; the caller gates this
pass to the resolve-folded frames so the two never double up.
===================
*/
// DUDE sub-native FSR2: `rectOverride` is the display-size composite's rect once the frame target
// has switched to it after the upscale -- the pass then covers the whole upscaled image instead
// of the view's (smaller, render-size) viewport. NULL = the viewport, as before.
void RB_RHI_ChromaticAberration( rhi::RHI *r, const viewDef_t *viewDef, const idScreenRect *rectOverride ) {
	const float chroma = r_postChromaticAberration.GetFloat();
	if ( chroma <= 0.0f || !R_BackendSupportsEnhancements() ) {
		return;
	}
	const bool fullscreenView = viewDef->isWindowView;	// the primary scene view (set by RenderScene)
	if ( !viewDef->viewEntitys || viewDef->isSubview || !fullscreenView ) {
		return;		// primary world view only (not mirrors / GUI renderDefs)
	}

	rhi::ShaderHandle prog = r->LoadShader( "postprocess" );
	if ( !prog ) {
		return;
	}

	const idScreenRect vp = rectOverride ? *rectOverride : viewDef->viewport;
	const int w = vp.x2 - vp.x1 + 1;
	const int h = vp.y2 - vp.y1 + 1;

	// snapshot the finished scene colour (world + weapon, after DoF / FSR / the bloom
	// measurement) so the pass reads-and-writes the one live scene target
	rhi::ImageHandle srcImg = 0;
	int potW = w, potH = h;
	if ( rectOverride ) {
		// sub-native FSR2: the composite is display-size while _currentRender was just captured at
		// the (smaller) render size by DoF / haze / smoke this same frame -- sharing it would
		// reallocate the capture image on every frame. The upscaled pass keeps its own exact-size
		// snapshot; forgotten with the world targets on vid_restart (the backend frees the image).
		if ( rhiChromaCompositeCap != 0 && ( rhiChromaCapW != w || rhiChromaCapH != h ) ) {
			r->RetireImage( rhiChromaCompositeCap );
			rhiChromaCompositeCap = 0;
		}
		if ( rhiChromaCompositeCap == 0 ) {
			rhiChromaCompositeCap = r->CreateCaptureImage( w, h, false, true );
			rhiChromaCapW = w;
			rhiChromaCapH = h;
		}
		if ( rhiChromaCompositeCap == 0 ) {
			return;
		}
		r->CopyFramebufferToImage( rhiChromaCompositeCap, 0, 0, vp.x1, vp.y1, w, h, false );
		srcImg = rhiChromaCompositeCap;
	} else {
		globalImages->currentRenderImage->CopyFramebuffer( vp.x1, vp.y1, w, h, true );
		potW = globalImages->currentRenderImage->uploadWidth  > 0 ? globalImages->currentRenderImage->uploadWidth  : w;
		potH = globalImages->currentRenderImage->uploadHeight > 0 ? globalImages->currentRenderImage->uploadHeight : h;
		srcImg = globalImages->currentRenderImage->rhiHandle;
	}

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	parms.screenCorrection[0] = (float)w / potW;		// viewport uv -> _currentRender (NPOT)
	parms.screenCorrection[1] = (float)h / potH;
	parms.windowCoord[2] = 0.5f;						// aberration center in uv
	parms.windowCoord[3] = 0.5f;
	parms.localParam0[0] = 0.0f;						// grain OFF here (the resolve grains the whole frame)
	parms.localParam0[2] = chroma;						// chromatic aberration strength

	r->SetViewport( tr.viewportOffset[0] + vp.x1,
	                tr.viewportOffset[1] + vp.y1, w, h );
	r->SetScissor( tr.viewportOffset[0] + vp.x1,
	               tr.viewportOffset[1] + vp.y1, w, h );
	backEnd.currentScissor = viewDef->scissor;

	if ( !rectOverride ) {
		RB_RHI_BindUnit( 0, globalImages->currentRenderImage );
	}
	RB_RHI_DrawFullscreen( r, prog, parms, srcImg );

	// direct binds above; forget the tmu cache so later 2D/GUI binds re-issue
	RB_RHI_ForgetTexBinds();
}

// ---- HDR eye adaptation / auto-exposure (Phase B1, docs/hdr-pipeline.md) ----
extern idCVar r_hdrEyeAdaptation;
extern idCVar r_hdrAdaptSpeed;
extern idCVar r_hdrExposure;
extern idCVar r_hdrAdaptBrighten;
extern idCVar r_hdrAdaptDarken;
extern idCVar r_hdrAdaptKey;
extern idCVar r_hdrAdaptCenter;
extern idCVar r_hdrTonemap;
extern idCVar r_hdrAdaptWhitePoint;
extern idCVar r_hdrDudeKnee;

// Single-level luma reduction targets (64 -> 8 -> 1), each 8x8-averaging into the next. Deliberately
// NOT a mip-render chain (BeginTargetMipPass): that path has no write->read barrier between levels and
// faulted the GPU (device-loss, black screen) when driven every frame. These go through the ordinary
// BeginTargetPass path the resolve / SSAO-temporal / DoF use, which is properly synchronized.
static rhi::RenderTargetHandle rhiLumaA = 0;				// 64x64 RGBA16F, .r = log-luma of the centre-cropped scene
static rhi::RenderTargetHandle rhiLumaB = 0;				// 8x8 RGBA16F, .r = 8x8 average of A
static rhi::RenderTargetHandle rhiLumaC = 0;				// 1x1 RGBA16F, .r = 8x8 average of B = geometric-mean log-luma
static rhi::RenderTargetHandle rhiExposureRT[2] = { 0, 0 };	// 1x1 RGBA16F adapted-exposure ping-pong (.r)
static bool rhiExposureValid = false;						// the read slot holds a usable previous exposure
static int rhiExposureIdx = 0;								// write slot
static int rhiExposureLastTick = -100000;					// Sys_Milliseconds() of the last update

// Ensure the luma reduction targets + the 1x1 exposure ping-pong exist; invalidate on lost context.
static bool RB_RHI_EnsureEyeAdaptTargets( rhi::RHI *r ) {
	if ( rhiLumaA && r->GetRenderTargetImage( rhiLumaA ) == 0 ) {
		rhiLumaA = rhiLumaB = rhiLumaC = 0;				// lost context (vid_restart)
	}
	if ( !rhiLumaA || !rhiLumaB || !rhiLumaC ) {
		if ( rhiLumaA ) { r->DestroyRenderTarget( rhiLumaA ); rhiLumaA = 0; }
		if ( rhiLumaB ) { r->DestroyRenderTarget( rhiLumaB ); rhiLumaB = 0; }
		if ( rhiLumaC ) { r->DestroyRenderTarget( rhiLumaC ); rhiLumaC = 0; }
		rhiLumaA = r->CreateRenderTarget( rhi::IF_RGBA16F, 64, 64 );
		rhiLumaB = r->CreateRenderTarget( rhi::IF_RGBA16F, 8, 8 );
		rhiLumaC = r->CreateRenderTarget( rhi::IF_RGBA16F, 1, 1 );
		if ( !rhiLumaA || !rhiLumaB || !rhiLumaC ) {
			if ( rhiLumaA ) { r->DestroyRenderTarget( rhiLumaA ); rhiLumaA = 0; }
			if ( rhiLumaB ) { r->DestroyRenderTarget( rhiLumaB ); rhiLumaB = 0; }
			if ( rhiLumaC ) { r->DestroyRenderTarget( rhiLumaC ); rhiLumaC = 0; }
			return false;
		}
	}
	if ( rhiExposureRT[0] && r->GetRenderTargetImage( rhiExposureRT[0] ) == 0 ) {
		rhiExposureRT[0] = rhiExposureRT[1] = 0;
		rhiExposureValid = false;
	}
	if ( !rhiExposureRT[0] || !rhiExposureRT[1] ) {
		for ( int i = 0; i < 2; ++i ) {
			if ( rhiExposureRT[i] ) { r->DestroyRenderTarget( rhiExposureRT[i] ); rhiExposureRT[i] = 0; }
		}
		// RGBA16F (not R16F): GL3 CreateRenderTarget has no single-channel colour target; 1x1 so size is trivial.
		rhiExposureRT[0] = r->CreateRenderTarget( rhi::IF_RGBA16F, 1, 1 );
		rhiExposureRT[1] = r->CreateRenderTarget( rhi::IF_RGBA16F, 1, 1 );
		rhiExposureValid = false;								// nothing to ease from until one frame lands
		if ( !rhiExposureRT[0] || !rhiExposureRT[1] ) {
			for ( int i = 0; i < 2; ++i ) {
				if ( rhiExposureRT[i] ) { r->DestroyRenderTarget( rhiExposureRT[i] ); rhiExposureRT[i] = 0; }
			}
			return false;
		}
		// clear both slots so the first prev-exposure read is defined on Vulkan (an unwritten
		// colour target's layout is UNDEFINED and would trip validation), like the berserk trail.
		rhi::ClearArgs clear;
		memset( &clear, 0, sizeof( clear ) );
		clear.color = true;
		r->BeginTargetPass( rhiExposureRT[0], &clear ); r->EndPass();
		r->BeginTargetPass( rhiExposureRT[1], &clear ); r->EndPass();
		rhiExposureIdx = 0;
		rhiExposureLastTick = -100000;
	}
	return true;
}

/*
===================
RB_RHI_EyeAdaptExposure

HDR eye adaptation (Phase B1). Reduce the finished HDR scene to a 1x1 geometric-mean
luminance (log-luma mip chain), then ease a per-frame adapted exposure toward the
mid-gray target into a 1x1 ping-pong (the temporal lag is the eye-adapt feel), and
return that 1x1 exposure image for the resolve to sample. Returns 0 (static exposure)
when adaptation is off, no tonemap curve is active, or the targets can't be built.
Mirrors the FXAA pattern (BeginTargetPass reading rhiHdrRT), so it is safe at resolve
time; measured before the resolve's own SetFrameTarget(0).
===================
*/
rhi::ImageHandle RB_RHI_EyeAdaptExposure( rhi::RHI *r, rhi::ImageHandle sceneImg ) {
	if ( !r_hdrEyeAdaptation.GetBool() || r_hdrTonemap.GetInteger() < 1 || sceneImg == 0 ) {
		return 0;
	}
	// Cinematics never adapt: the director lit and graded each shot, and an auto-exposure that
	// re-meters on every camera cut pumps the brightness across the edit. The cvar is NOT touched -
	// the pass just stands down (static exposure, as if it were off) while a cinematic camera is
	// the main view, and picks up again by itself when gameplay returns; the adapted exposure it
	// had reached is still in its history, so it resumes from there.
	if ( RB_RHI_CinematicView() ) {
		return 0;
	}
	if ( !R_BackendSupportsEnhancements() || !RB_RHI_EnsureEyeAdaptTargets( r ) ) {
		return 0;
	}
	rhi::ShaderHandle lumaProg = r->LoadShader( "hdrluma" );
	rhi::ShaderHandle downProg = r->LoadShader( "hdrlumadown" );
	rhi::ShaderHandle expProg  = r->LoadShader( "hdrexpose" );
	if ( !lumaProg || !downProg || !expProg ) {
		return 0;
	}
	// 1. luminance reduction: scene -> A (64x64 log-luma, centre-cropped) -> B (8x8) -> C (1x1),
	// each an 8x8 box-average, through the ordinary BeginTargetPass path — target->target reads are
	// properly synchronized there, unlike the barrier-less mip-render chain (which faulted the GPU
	// when driven every frame). C.r ends as the geometric-mean log-luma.
	rhi::RenderParams p;
	memset( &p, 0, sizeof( p ) );
	p.mvpMatrix[0] = p.mvpMatrix[5] = p.mvpMatrix[10] = p.mvpMatrix[15] = 1.0f;
	p.localParam0[0] = idMath::ClampFloat( 0.05f, 1.0f, r_hdrAdaptCenter.GetFloat() );	// central metering crop (hdrluma)

	r->BeginTargetPass( rhiLumaA, NULL );					// scene -> 64x64 log-luma
	RB_RHI_DrawFullscreen( r, lumaProg, p, sceneImg );
	r->EndPass();

	r->BeginTargetPass( rhiLumaB, NULL );					// 64 -> 8 (each texel = 8x8 average)
	RB_RHI_DrawFullscreen( r, downProg, p, r->GetRenderTargetImage( rhiLumaA ) );
	r->EndPass();

	r->BeginTargetPass( rhiLumaC, NULL );					// 8 -> 1 (8x8 average = geometric mean)
	RB_RHI_DrawFullscreen( r, downProg, p, r->GetRenderTargetImage( rhiLumaB ) );
	r->EndPass();

	// 2. temporal adaptation into the 1x1 exposure ping-pong
	int timeMs = Sys_Milliseconds();
	int dt = timeMs - rhiExposureLastTick;
	if ( dt > 1000 || dt < 0 ) { rhiExposureValid = false; dt = 16; }	// paused / first frame -> snap
	if ( dt < 1 ) { dt = 1; }
	rhiExposureLastTick = timeMs;
	float speed = r_hdrAdaptSpeed.GetFloat();
	if ( speed < 0.01f ) { speed = 0.01f; }
	const float tau = 1.0f / speed;										// time constant, seconds
	const float dtSec = (float)dt * 0.001f;
	const float alpha = 1.0f - idMath::Pow( 2.71828183f, -dtSec / tau );

	const int writeIdx = rhiExposureIdx;
	const int readIdx  = 1 - writeIdx;

	rhi::RenderParams ep;
	memset( &ep, 0, sizeof( ep ) );
	ep.mvpMatrix[0] = ep.mvpMatrix[5] = ep.mvpMatrix[10] = ep.mvpMatrix[15] = 1.0f;
	ep.localParam0[0] = r_hdrExposure.GetFloat();						// neutral "mid" exposure
	ep.localParam0[1] = r_hdrAdaptBrighten.GetFloat();					// added as the scene darkens
	ep.localParam0[2] = r_hdrAdaptDarken.GetFloat();					// removed as the scene brightens
	ep.localParam0[3] = alpha;
	ep.localParam1[0] = rhiExposureValid ? 1.0f : 0.0f;					// ease from prev, else snap
	ep.localParam1[1] = 0.0f;											// luma source is a single-level 1x1 target (lod 0)
	ep.localParam1[2] = r_hdrAdaptKey.GetFloat();						// key: scene luminance mapping to r_hdrExposure
	ep.localParam1[3] = r_hdrAdaptWhitePoint.GetFloat();				// DUDE adaptive white-point strength (0 = off)
	ep.color[0]       = r_hdrDudeKnee.GetFloat();						// knee, for the static-equivalent white point W0

	rhi::ImageHandle lumaAvg = r->GetRenderTargetImage( rhiLumaC );		// the 1x1 average

	r->BeginTargetPass( rhiExposureRT[writeIdx], NULL );
	RB_RHI_BindRTUnit( r, 1, rhiExposureRT[readIdx] );					// previous exposure on unit 1
	RB_RHI_DrawFullscreen( r, expProg, ep, lumaAvg );					// luma 1x1 on unit 0
	r->EndPass();

	rhiExposureIdx  = readIdx;											// next frame writes the other slot
	rhiExposureValid = true;
	RB_RHI_ForgetTexBinds();

	return r->GetRenderTargetImage( rhiExposureRT[writeIdx] );			// fresh exposure for the resolve
}

// ---- HDR bloom (Phase C, docs/hdr-pipeline.md) ----
extern idCVar r_hdrBloom;
extern idCVar r_hdrBloomThreshold;

#define RHI_BLOOM_MAX 6
static rhi::RenderTargetHandle rhiBloomD[RHI_BLOOM_MAX] = { 0 };	// downsample chain (threshold -> smaller)
static rhi::RenderTargetHandle rhiBloomU[RHI_BLOOM_MAX] = { 0 };	// upsample-combine chain (result in [0])
static int rhiBloomW[RHI_BLOOM_MAX] = { 0 };
static int rhiBloomH[RHI_BLOOM_MAX] = { 0 };
static int rhiBloomLevels = 0;
static int rhiBloomBaseW = 0, rhiBloomBaseH = 0;					// vid size the chain was built for

static void RB_RHI_DestroyBloom( rhi::RHI *r ) {
	for ( int i = 0; i < RHI_BLOOM_MAX; i++ ) {
		if ( rhiBloomD[i] ) { r->DestroyRenderTarget( rhiBloomD[i] ); rhiBloomD[i] = 0; }
		if ( rhiBloomU[i] ) { r->DestroyRenderTarget( rhiBloomU[i] ); rhiBloomU[i] = 0; }
	}
	rhiBloomLevels = 0; rhiBloomBaseW = rhiBloomBaseH = 0;
}

// Ensure the down+up bloom chains exist at the current resolution; rebuild on resize / lost context.
static bool RB_RHI_EnsureBloom( rhi::RHI *r ) {
	const int vw = glConfig.vidWidth, vh = glConfig.vidHeight;
	if ( rhiBloomD[0] && r->GetRenderTargetImage( rhiBloomD[0] ) == 0 ) {	// lost context (vid_restart)
		for ( int i = 0; i < RHI_BLOOM_MAX; i++ ) { rhiBloomD[i] = rhiBloomU[i] = 0; }
		rhiBloomLevels = 0; rhiBloomBaseW = rhiBloomBaseH = 0;
	}
	if ( rhiBloomLevels >= 2 && rhiBloomBaseW == vw && rhiBloomBaseH == vh ) {
		return true;
	}
	RB_RHI_DestroyBloom( r );
	int w = vw / 2, h = vh / 2, n = 0;
	for ( ; n < RHI_BLOOM_MAX && w >= 8 && h >= 8; n++ ) {
		rhiBloomD[n] = r->CreateRenderTarget( rhi::IF_RGBA16F, w, h );
		rhiBloomU[n] = r->CreateRenderTarget( rhi::IF_RGBA16F, w, h );
		if ( !rhiBloomD[n] || !rhiBloomU[n] ) { break; }
		rhiBloomW[n] = w; rhiBloomH[n] = h;
		w /= 2; h /= 2;
	}
	rhiBloomLevels = n;
	if ( rhiBloomLevels < 2 ) { RB_RHI_DestroyBloom( r ); return false; }	// need at least down + up
	rhiBloomBaseW = vw; rhiBloomBaseH = vh;
	return true;
}

/*
===================
RB_RHI_Bloom

HDR bloom (Phase C). Threshold the bright HDR scene at half res, downsample it through a chain
of ever-smaller targets (the downsample IS the blur), then tent-upsample back up, adding each
level's downsample content in-shader (VK BeginTargetPass always clears, so the up-chain writes a
separate set of targets rather than accumulating in place). Returns the half-res glow
(rhiBloomU[0]) for the resolve to bilinear-upsample and add before the tonemap; 0 when bloom is
off. Runs at the end of the primary 3D view (scene-only), like eye adaptation.
===================
*/
rhi::ImageHandle RB_RHI_Bloom( rhi::RHI *r, rhi::ImageHandle sceneImg ) {
	if ( r_hdrBloom.GetFloat() <= 0.0f || sceneImg == 0 ) {
		return 0;
	}
	if ( !R_BackendSupportsEnhancements() || !RB_RHI_EnsureBloom( r ) ) {
		return 0;
	}
	rhi::ShaderHandle threshProg = r->LoadShader( "bloomthreshold" );
	rhi::ShaderHandle downProg   = r->LoadShader( "bloomdown" );
	rhi::ShaderHandle upProg     = r->LoadShader( "bloomup" );
	if ( !threshProg || !downProg || !upProg ) {
		return 0;
	}
	const int N = rhiBloomLevels;

	// threshold: scene -> D[0] (half res, bright pixels only)
	rhi::RenderParams p;
	memset( &p, 0, sizeof( p ) );
	p.mvpMatrix[0] = p.mvpMatrix[5] = p.mvpMatrix[10] = p.mvpMatrix[15] = 1.0f;
	p.localParam0[0] = r_hdrBloomThreshold.GetFloat();
	r->BeginTargetPass( rhiBloomD[0], NULL );
	RB_RHI_DrawFullscreen( r, threshProg, p, sceneImg );
	r->EndPass();

	// downsample chain: D[i-1] -> D[i]
	for ( int i = 1; i < N; i++ ) {
		rhi::RenderParams dp;
		memset( &dp, 0, sizeof( dp ) );
		dp.mvpMatrix[0] = dp.mvpMatrix[5] = dp.mvpMatrix[10] = dp.mvpMatrix[15] = 1.0f;
		dp.localParam0[0] = 1.0f / (float)rhiBloomW[i-1];		// source texel size
		dp.localParam0[1] = 1.0f / (float)rhiBloomH[i-1];
		r->BeginTargetPass( rhiBloomD[i], NULL );
		RB_RHI_DrawFullscreen( r, downProg, dp, r->GetRenderTargetImage( rhiBloomD[i-1] ) );
		r->EndPass();
	}

	// upsample-combine chain: U[i] = tent_up( src ) + D[i]; src = D[N-1] to start, then U[i+1]
	for ( int i = N - 2; i >= 0; i-- ) {
		rhi::RenderParams up;
		memset( &up, 0, sizeof( up ) );
		up.mvpMatrix[0] = up.mvpMatrix[5] = up.mvpMatrix[10] = up.mvpMatrix[15] = 1.0f;
		up.localParam0[0] = 1.0f / (float)rhiBloomW[i+1];		// smaller-level texel size (tent offsets)
		up.localParam0[1] = 1.0f / (float)rhiBloomH[i+1];
		rhi::RenderTargetHandle srcRT = ( i == N - 2 ) ? rhiBloomD[N-1] : rhiBloomU[i+1];
		r->BeginTargetPass( rhiBloomU[i], NULL );
		RB_RHI_BindRTUnit( r, 1, rhiBloomD[i] );				// this level's downsample content -> unit 1 (u_add)
		RB_RHI_DrawFullscreen( r, upProg, up, r->GetRenderTargetImage( srcRT ) );	// upsample source -> unit 0
		r->EndPass();
	}

	RB_RHI_ForgetTexBinds();
	return r->GetRenderTargetImage( rhiBloomU[0] );				// half-res glow (resolve bilinear-upsamples + adds)
}

/*
===================
RB_RHI_MotionVectorDebugOverlay

r_mvDebug visualization (docs/fsr-temporal-pipeline.md R1/A2): blit the velocity MRT over the
finished 3D view so the motion-vector convention can be checked — the #1 FSR2 integration bug.
The velocity stored is currUV - prevUV, so a static world under a RIGHTWARD camera pan moved
LEFT on screen (negative x): mode 1 tints it toward green (−x → below-0.5 red, so green
dominates). A violent full-screen smear that follows the pan the WRONG way means the sign is
inverted. No-op unless r_motionVectors + r_mvDebug are on and the standalone velocity target
exists (VK only).
===================
*/
// R1/C2: the target whose RG16F attachment holds this view's per-object velocity - the merged
// prepass's pseudo-handle by default, else the standalone 3-MRT target (attachment 2) - or 0
// when it wasn't produced (MV off, GL3, subview).
// The FSR2 dispatch reads it; same existence conditions the r_mvDebug overlay uses.
rhi::RenderTargetHandle RB_RHI_VelocityTargetThisView( void ) {
	return ( rhiNormalReadyThisView && rhiNormalVelThisView ) ? rhiNormalResultRT : 0;
}

void RB_RHI_MotionVectorDebugOverlay( rhi::RHI *r, const viewDef_t *viewDef ) {
	const int mode = r_mvDebug.GetInteger();
	if ( mode <= 0 || !( r_motionVectors.GetBool() || r_fsr.GetBool() ) || !R_BackendSupportsEnhancements() ) {
		return;		// velocity is produced by r_motionVectors OR implied by r_fsr (C2)
	}
	if ( rhi::GetActiveBackendType() != rhi::BT_VULKAN
	     || !rhiNormalReadyThisView || !rhiNormalVelThisView || !rhiNormalResultRT ) {
		return;					// velocity MRT is VK-only and only exists when it was produced this view
	}
	rhi::ImageHandle velImg = r->GetRenderTargetImage3( rhiNormalResultRT );
	if ( !velImg ) {
		return;
	}
	rhi::ShaderHandle prog = r->LoadShader( "ssao_debug" );
	if ( !prog ) {
		return;
	}

	const int w = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int h = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	parms.localParam0[0] = ( mode >= 2 ) ? 5.0f : 4.0f;		// ssao_debug: 4 = direction, 5 = magnitude
	// per-frame UV velocity is tiny, so amplify for visibility; the magnitude view reads darker
	// than the centred direction view, so it gets a larger base gain. r_mvDebugScale multiplies
	// both so slow motion can be cranked up live (magnitude stays black where there is NO motion).
	parms.localParam0[1] = ( ( mode >= 2 ) ? 40.0f : 12.0f ) * r_mvDebugScale.GetFloat();

	r->SetViewport( tr.viewportOffset[0] + viewDef->viewport.x1,
	                tr.viewportOffset[1] + viewDef->viewport.y1, w, h );
	r->SetScissor( tr.viewportOffset[0] + viewDef->viewport.x1,
	               tr.viewportOffset[1] + viewDef->viewport.y1, w, h );
	backEnd.currentScissor = viewDef->scissor;

	RB_RHI_DrawFullscreen( r, prog, parms, velImg );

	RB_RHI_ForgetTexBinds();
}

/*
===================
RB_RHI_DecalMaskPass

DUDE decal metal mask (r_decalMetalMask, docs/readme-changes.md 1.6). Dirt on metal is not
metal: before the light loop, every overlay decal's coverage is drawn into a screen-space
"metal retention" buffer (1 = untouched, multiply-blended so overlaps compound), and the PBR
interaction scales its metalness by it (unit 14, gated by u_pbrParms.z). A forward renderer
lights the wall long before the decal blends over it, so this is the only point where the
decal can change how the wall underneath responds to light.

Coverage comes from the decal's own unlit blend stages - whatever that blend would hide of the
surface (alpha / darken / filter; see decalmask.frag). Additive stages (glows, scorch embers)
hide nothing and are skipped, as are lit stages: a translucent lit decal adds light on top of
the wall rather than covering it, and its diffuse alpha is rarely authored.

Primary fullscreen view only, like the other screen-space buffers. The target has no depth,
so decalmask.frag tests against _currentDepth - the pass stands down without that capture.
===================
*/
static rhi::RenderTargetHandle rhiDecalMaskRT = 0;
static int  rhiDecalMaskW = 0, rhiDecalMaskH = 0;

/*
===================
RB_RHI_BloodRelief

The wet blood film's relief: a normal map of the blood's own height. A material that authors
a bump stage (the texture packs' heightmap( splat )) keeps it; everything else - all of stock,
and every splat the game spawns - gets one generated from its cover image with the engine's own
image programs. Brighter = thicker for the darken blend, darker = thicker for a filter blend;
smoothnormals() takes the JPEG / mottling noise out so the film ripples instead of sparkling.
The bake is moderate on purpose: r_decalWetBloodRelief scales the slope in the shader, live.
Resolved once per material stage; idImage and idMaterial pointers live as long as the renderer.
===================
*/
struct rhiReliefEntry_t {
	const idMaterial *	material;
	int					stageNum;
	idImage *			image;
};
static idList<rhiReliefEntry_t> rhiReliefCache;

static idImage *RB_RHI_BloodRelief( const idMaterial *shader, int stageNum, const shaderStage_t *cover, float mode ) {
	for ( int i = 0; i < rhiReliefCache.Num(); i++ ) {
		if ( rhiReliefCache[i].material == shader && rhiReliefCache[i].stageNum == stageNum ) {
			return rhiReliefCache[i].image;
		}
	}
	idImage *image = NULL;
	const shaderStage_t *bumpStage = shader->GetBumpStage();
	if ( bumpStage && bumpStage->texture.image ) {
		image = bumpStage->texture.image;
	} else if ( cover->texture.image && cover->texture.image->imgName.Length() > 0 ) {
		idStr source = cover->texture.image->imgName;
		if ( mode > 1.5f ) {
			source = idStr( "invertColor(" ) + source + ")";
		}
		const idStr program = idStr( "smoothnormals(heightmap(" ) + source + ", 4))";
		image = globalImages->ImageFromFile( program.c_str(), TF_DEFAULT, true, cover->texture.image->repeat, TD_BUMP );
	}
	rhiReliefEntry_t e;
	e.material = shader;
	e.stageNum = stageNum;
	e.image = image;
	rhiReliefCache.Append( e );
	return image;
}

static bool RB_RHI_DecalMaskStageMode( const shaderStage_t *pStage, float &mode ) {
	const int src = pStage->drawStateBits & GLS_SRCBLEND_BITS;
	const int dst = pStage->drawStateBits & GLS_DSTBLEND_BITS;
	if ( src == GLS_SRCBLEND_SRC_ALPHA && dst == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA ) {
		mode = 0.0f;
		return true;
	}
	if ( src == GLS_SRCBLEND_ZERO && dst == GLS_DSTBLEND_ONE_MINUS_SRC_COLOR ) {
		mode = 1.0f;
		return true;
	}
	if ( ( src == GLS_SRCBLEND_DST_COLOR && dst == GLS_DSTBLEND_ZERO )
		|| ( src == GLS_SRCBLEND_ZERO && dst == GLS_DSTBLEND_SRC_COLOR ) ) {
		mode = 2.0f;
		return true;
	}
	return false;
}

static void RB_RHI_DecalMaskPass( rhi::RHI *r, const viewDef_t *viewDef ) {
	rhiDecalMaskImage = 0;
	rhiFilmSurfs.SetNum( 0, false );
	const float strength = idMath::ClampFloat( 0.0f, 1.0f, r_decalMetalMask.GetFloat() );
	const float roughen = idMath::ClampFloat( 0.0f, 1.0f, r_decalRoughen.GetFloat() );
	const float wetBlood = r_decalWetBlood.GetFloat();
	// metalness / roughness only exist on the PBR path
	if ( ( strength <= 0.0f && roughen <= 0.0f && wetBlood <= 0.0f ) || !r_pbr.GetBool() || !R_BackendSupportsEnhancements() ) {
		if ( rhiDecalMaskRT ) { r->DestroyRenderTarget( rhiDecalMaskRT ); rhiDecalMaskRT = 0; rhiDecalMaskW = rhiDecalMaskH = 0; }
		return;
	}
	const bool fullscreenView = viewDef->isWindowView;	// the primary scene view (set by RenderScene)
	if ( !viewDef->viewEntitys || viewDef->isSubview || !fullscreenView || viewDef->renderView.viewID < 0 ) {
		return;
	}
	const bool vkMode = rhi::GetActiveBackendType() == rhi::BT_VULKAN;
	idImage *depthImg = globalImages->currentDepthImage;
	const bool depthCaptured = vkMode ? ( depthImg->rhiCaptured && depthImg->rhiHandle != 0 )
	                                  : ( depthImg->uploadWidth > 0 && depthImg->texnum != idImage::TEXTURE_NOT_LOADED );
	if ( !depthCaptured ) {
		return;
	}

	// anything to draw? most views have no qualifying decal - skip the target pass entirely
	drawSurf_t **drawSurfs = (drawSurf_t **)&viewDef->drawSurfs[0];
	int first = -1;
	for ( int i = 0; i < viewDef->numDrawSurfs; i++ ) {
		const idMaterial *shader = drawSurfs[i]->material;
		if ( shader->Coverage() == MC_TRANSLUCENT && shader->IsDrawn() && !shader->HasGui()
			&& ( shader->GetSort() == SS_DECAL || shader->TestMaterialFlag( MF_POLYGONOFFSET ) ) ) {
			first = i;
			break;
		}
	}
	if ( first < 0 ) {
		return;
	}

	rhi::ShaderHandle prog = r->LoadShader( "decalmask" );
	if ( !prog ) {
		return;
	}
	const int w = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int h = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
	if ( rhiDecalMaskRT && r->GetRenderTargetImage( rhiDecalMaskRT ) == 0 ) {
		rhiDecalMaskRT = 0;					// lost context (vid_restart)
	}
	if ( !rhiDecalMaskRT || rhiDecalMaskW != w || rhiDecalMaskH != h ) {
		if ( rhiDecalMaskRT ) { r->DestroyRenderTarget( rhiDecalMaskRT ); }
		rhiDecalMaskRT = r->CreateRenderTarget( rhi::IF_RGBA8, w, h );
		rhiDecalMaskW = w;
		rhiDecalMaskH = h;
		if ( !rhiDecalMaskRT ) {
			rhiDecalMaskW = rhiDecalMaskH = 0;
			return;
		}
	}

	rhi::ClearArgs clear;
	memset( &clear, 0, sizeof( clear ) );
	clear.color = true;
	clear.rgba[0] = clear.rgba[1] = clear.rgba[2] = clear.rgba[3] = 1.0f;	// full retention
	r->BeginTargetPass( rhiDecalMaskRT, &clear );
	r->SetViewport( 0, 0, w, h );
	r->SetScissor( 0, 0, w, h );

	const viewEntity_t *currentSpace = NULL;
	float mvp[16];
	for ( int i = first; i < viewDef->numDrawSurfs; i++ ) {
		const drawSurf_t *surf = drawSurfs[i];
		const srfTriangles_t *tri = surf->geo;
		const idMaterial *shader = surf->material;
		if ( shader->Coverage() != MC_TRANSLUCENT || !shader->IsDrawn() || shader->HasGui()
			|| !( shader->GetSort() == SS_DECAL || shader->TestMaterialFlag( MF_POLYGONOFFSET ) ) ) {
			continue;
		}
		// the view weapon's depth-hacked depth would never match the capture
		if ( surf->space->weaponDepthHack || surf->space->modelDepthHack != 0.0f ) {
			continue;
		}
		if ( !tri->numIndexes || !tri->ambientCache ) {
			continue;
		}
		const float *regs = surf->shaderRegisters;

		bool streamed = false;
		bool filmed = false;
		rhi::BufferHandle vb = 0, ib = 0;
		int vertOfs = 0, idxOfs = 0;

		for ( int stage = 0; stage < shader->GetNumStages(); stage++ ) {
			const shaderStage_t *pStage = shader->GetStage( stage );
			float mode;
			if ( pStage->lighting != SL_AMBIENT || regs[pStage->conditionRegister] == 0
				|| pStage->newStage != NULL || pStage->texture.image == NULL
				|| pStage->texture.cinematic != NULL || pStage->texture.texgen != TG_EXPLICIT
				|| !RB_RHI_DecalMaskStageMode( pStage, mode ) ) {
				continue;
			}

			// r_decalWetBlood: hand this blood decal to the light loop as a film of its own
			// (RB_RHI_DrawFilmInteractions). Darken / filter blends only - alpha-blended "glow
			// splats" are not blood - and only geometry that carries a tangent space: world and
			// model surfaces, and projected decals since R_DecalWindingFrame (overlays do not).
			// A decal whose blend pass tessellates rides a displaced mesh the flat film would miss.
			if ( wetBlood > 0.0f && !filmed && mode >= 1.0f && tri->tangentsCalculated && tri->verts
				&& rhiFilmSurfs.Num() < 96 && R_DecalIsBlood( shader ) && !RB_RHI_TessellateSurf( surf, true ) ) {
				idImage *relief = RB_RHI_BloodRelief( shader, stage, pStage, mode );
				if ( relief ) {
					rhiFilmSurf_t film;
					film.surf = surf;
					film.cover = pStage;
					film.reliefStage = shader->GetBumpStage();
					film.relief = relief;
					film.mode = mode;
					// a projected decal never computes its bounds (nothing culls one); a model
					// surface - dmap merges an area's same-material decals into one - has them
					if ( tri->numVerts <= 64 ) {
						film.bounds.Clear();
						for ( int v = 0; v < tri->numVerts; v++ ) {
							film.bounds.AddPoint( tri->verts[v].xyz );
						}
					} else {
						film.bounds = tri->bounds;
					}
					rhiFilmSurfs.Append( film );
					filmed = true;
				}
			}

			if ( surf->space != currentSpace ) {
				currentSpace = surf->space;
				RB_RHI_SpaceMvp( viewDef, surf->space, mvp );
			}
			if ( !streamed ) {
				RB_RHI_StreamAmbient( r, tri, vb, vertOfs, ib, idxOfs );
				streamed = true;
			}

			rhi::RenderParams parms;
			memset( &parms, 0, sizeof( parms ) );
			memcpy( parms.mvpMatrix, mvp, sizeof( parms.mvpMatrix ) );
			if ( pStage->texture.hasMatrix ) {
				parms.diffuseMatrixS[0] = regs[pStage->texture.matrix[0][0]];
				parms.diffuseMatrixS[1] = regs[pStage->texture.matrix[0][1]];
				parms.diffuseMatrixS[3] = regs[pStage->texture.matrix[0][2]];
				parms.diffuseMatrixT[0] = regs[pStage->texture.matrix[1][0]];
				parms.diffuseMatrixT[1] = regs[pStage->texture.matrix[1][1]];
				parms.diffuseMatrixT[3] = regs[pStage->texture.matrix[1][2]];
				if ( parms.diffuseMatrixS[3] < -40.0f || parms.diffuseMatrixS[3] > 40.0f ) parms.diffuseMatrixS[3] -= (int)parms.diffuseMatrixS[3];
				if ( parms.diffuseMatrixT[3] < -40.0f || parms.diffuseMatrixT[3] > 40.0f ) parms.diffuseMatrixT[3] -= (int)parms.diffuseMatrixT[3];
			} else {
				parms.diffuseMatrixS[0] = 1.0f;
				parms.diffuseMatrixT[1] = 1.0f;
			}
			// stage + vertex colour exactly as the decal itself draws: game decals fade out
			// through them, and the metal comes back as they do
			parms.color[0] = regs[pStage->color.registers[0]];
			parms.color[1] = regs[pStage->color.registers[1]];
			parms.color[2] = regs[pStage->color.registers[2]];
			parms.color[3] = regs[pStage->color.registers[3]];
			switch ( pStage->vertexColor ) {
			case SVC_IGNORE:
				parms.vertexColorAdd[0] = parms.vertexColorAdd[1] = parms.vertexColorAdd[2] = parms.vertexColorAdd[3] = 1.0f;
				break;
			case SVC_MODULATE:
				parms.vertexColorModulate[0] = parms.vertexColorModulate[1] = parms.vertexColorModulate[2] = parms.vertexColorModulate[3] = 1.0f;
				break;
			case SVC_INVERSE_MODULATE:
				parms.vertexColorModulate[0] = parms.vertexColorModulate[1] = parms.vertexColorModulate[2] = parms.vertexColorModulate[3] = -1.0f;
				parms.vertexColorAdd[0] = parms.vertexColorAdd[1] = parms.vertexColorAdd[2] = parms.vertexColorAdd[3] = 1.0f;
				break;
			}
			parms.localParam0[0] = mode;
			parms.localParam0[1] = strength;		// r: metalness
			parms.localParam0[2] = roughen;			// g: smoothness
			parms.depthTexRecip[0] = 1.0f / depthImg->uploadWidth;
			parms.depthTexRecip[1] = 1.0f / depthImg->uploadHeight;
			rhi::FillDepthParms( parms, viewDef->projectionMatrix );

			rhi::PipelineDesc pd;
			// retention multiplies into the buffer; no depth attachment to test or write
			pd.stateBits = GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO | GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK;
			pd.shader = prog;
			pd.vertexLayout = rhi::VL_DRAWVERT;
			pd.cullType = RB_RHI_CullFor( viewDef, shader->GetCullType() );
			r->BindPipeline( pd );

			RB_RHI_BindUnit( 0, pStage->texture.image );
			RB_RHI_BindUnit( 1, depthImg );

			rhi::BufferHandle ub;
			int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );
			rhi::DrawArgs da;
			memset( &da, 0, sizeof( da ) );
			da.vertexBuffer = vb;
			da.vertexOffset = vertOfs;
			da.indexBuffer = ib;
			da.firstIndex = idxOfs / (int)sizeof( glIndex_t );
			da.indexCount = tri->numIndexes;
			da.uniformBuffer = ub;
			da.uniformOffset = uniOfs;
			da.uniformSize = sizeof( parms );
			RB_RHI_VkTextures( da );
			r->Draw( da );
			backEnd.pc.c_drawElements++;
		}
	}

	r->EndPass();
	RB_RHI_ForgetTexBinds();
	rhiDecalMaskImage = r->GetRenderTargetImage( rhiDecalMaskRT );
}


/*
===================
RB_RHI_PrepareViewLighting

Builds this view's lighting inputs before the per-light loop and binds the
ones the loop samples on fixed units: the depth+normal G-buffer (or the
standalone zfill), RTAO/GTAO, RT GI, the RT-shadow-blur targets, and the
decal metal mask. Sets the per-view rhiNormal* / rhiSsaoAppliedThisView
flags the loop reads. Lifted verbatim from RB_RHI_DrawWorld.
===================
*/
static void RB_RHI_PrepareViewLighting( rhi::RHI *r, const viewDef_t *viewDef, bool vkMode ) {
	// normal G-buffer (Option B): bump-mapped view normals for SSAO to sample instead of
	// reconstructing from depth. docs/ssao-normal-merge.md: with r_ssaoMergeNormal the
	// gbuffer pass ALSO seals the scene depth, folding the standalone zfill prepass away
	// (~0.65 ms / ~69% of SSAO's cost). So run it first: if it merged, it replaces zfill
	// and we capture _currentDepth from its sealed depth; otherwise zfill seals depth as
	// before. The return value is the single source of truth — no duplicate gate to drift,
	// and a BeginNormalPrepass fallback (returns 0) cleanly leaves zfill to seal depth.
	// per-view state only - the target CACHE keys (rhiNormalMrt/Vel) stay untouched, or the
	// standalone velocity target would key-mismatch and recreate every frame (the DIAG spam)
	rhiNormalReadyThisView = false;	rhiNormalMrtThisView = false;	rhiNormalVelThisView = false;	rhiNormalAlbedoThisView = false;
	r->GpuScopeBegin( "prepass (depth+gbuffer)" );
	const bool mergedDepth = RB_RHI_NormalPrepass( r, viewDef );
	if ( mergedDepth ) {
		RB_RHI_CaptureCurrentDepth( viewDef );
	} else {
		RB_RHI_FillDepthBuffer( r, viewDef );
	}
	r->GpuScopeEnd();

	// SSAO (GTAO) builds the AO buffer from the just-captured scene depth, before
	// the ambient/interaction passes that consume it (docs/ssao-gtao.md). Each view
	// starts with no AO; the pass sets rhiSsaoAppliedThisView when it produces one.
	rhiSsaoAppliedThisView = false;
	// RTAO first (docs/rtx-rtao.md H4d): when it rays+denoises+composites this view's AO,
	// the GTAO pass sees the flag and skips its whole chain; any RTAO failure falls
	// through to GTAO unchanged.
	r->GpuScopeBegin( "ao (rtao/gtao)" );
	RB_RHI_RtaoPass( r, viewDef );
	RB_RHI_SSAOPass( r, viewDef );
	r->GpuScopeEnd();
	// H6a RT GI (docs/rtx-rtgi.md): the radiance-gather pass (behind r_rtgi, default off). Runs after the
	// AO passes since it reuses the same sealed depth + normal G-buffer. The pass opens its OWN sub-scopes
	// (rtgi.ray / rtgi.guides / rtgi.conf / rtgi.denoise) for r_vkGpuTime 2, so the ray trace and the NRD
	// denoise are timed separately — scopes do not nest, so there is no outer "rtgi" scope here.
	RB_RHI_RtgiPass( r, viewDef );
	// r_rtShadowBlur: validate the view + size the shared targets; the light loop builds each
	// ray-served light's mask on demand (RB_RHI_RtShadowBlurLight)
	RB_RHI_RtShadowBlurBegin( r, viewDef );
	// r_decalMetalMask: decal coverage -> metal retention, consumed by the PBR interactions below
	RB_RHI_DecalMaskPass( r, viewDef );
	if ( rhiDecalMaskImage && !vkMode ) {
		// GL3: raw-bind on unit 14 for the whole light loop (nothing in it touches that unit);
		// VK carries it per draw as DrawArgs::decalMask
		rhi::gl3ActiveTexture( GL_TEXTURE0 + 14 );
		qglBindTexture( GL_TEXTURE_2D, (GLuint)rhiDecalMaskImage );
		rhi::gl3ActiveTexture( GL_TEXTURE0 );
		backEnd.glState.currenttmu = 0;
	}

	// Bind the AO buffer on unit 9 for the whole per-light loop: both the ambient and
	// interaction shaders sample u_ssao there, and nothing in the loop touches unit 9
	// (Draw binds 0-8). Bound once here rather than per draw; the enable flag in
	// localParam0.x (set per surface) decides whether a shader actually reads it.
	if ( rhiSsaoAppliedThisView ) {
		// GL: raw-bind on unit 9. VK: record into rhiVkUnits[9] so every interaction/
		// ambient draw's RB_RHI_VkTextures carries it as DrawArgs::ssao for the loop.
		RB_RHI_BindRTUnit( r, 9, rhiSsaoResultRT );
	}

	// DUDE screen-space contact shadows (docs/contact-shadows.md): bind the captured scene depth
	// (_currentDepth, already sampleable - SSAO consumed it earlier this frame) on unit 16 for the
	// whole light loop. GL3 raw-binds it (nothing in the loop touches unit 16); VK carries it per
	// draw as DrawArgs::contactDepth. interaction.frag's contactShadowTerm only samples it when
	// r_contactShadows filled contactParms this surface, so the bind is harmless when the feature is off.
	if ( r_contactShadows.GetBool() && !r_rtAllLights.GetBool() && !vkMode && R_BackendSupportsEnhancements()
	     && globalImages->currentDepthImage->uploadWidth > 0 ) {
		rhi::gl3ActiveTexture( GL_TEXTURE0 + 16 );
		qglBindTexture( GL_TEXTURE_2D, globalImages->currentDepthImage->texnum );
		rhi::gl3ActiveTexture( GL_TEXTURE0 );
		backEnd.glState.currenttmu = 0;
	}
}

/*
===================
RB_RHI_ResetShadowTallies

Zeroes this view's shadow-map / cube-cache performance counters, advances the
cube-cache frame number, freezes the light-budget hysteresis for a stable
ranking, and frees the whole cache when r_shadowMapCache is off. Lifted
verbatim from RB_RHI_DrawWorld.
===================
*/
static void RB_RHI_ResetShadowTallies( rhi::RHI *r ) {
	rhiShadowPerfCasters = 0;
	rhiShadowCubeLights = 0;
	rhiShadowCubeCasters = 0;
	rhiShadowCubeFaces = 0;
	rhiShadowCubeFacesCulled = 0;
	rhiCubeCacheHits = 0;
	rhiCubeCacheMiss = 0;
	rhiCubeCacheDynamic = 0;
	rhiCubeCacheScratch = 0;
	rhiCubeUpdatesSpent = 0;
	rhiCubeCacheDeferred = 0;
	rhiCubeMissCold = 0;
	rhiCubeMissWarmCaster = 0;
	rhiCubeMissWarmLight = 0;
	rhiCubeEvictions = 0;
	rhiCubeCacheSplit = 0;
	rhiCubeCacheRtMoving = 0;
	rhiMapCacheHits = 0;
	rhiMapCacheRendered = 0;
	rhiCubeCacheFrameNo++;			// bump before the light loop so lastFrame == "this view"
	RB_RHI_SnapshotLightBudgetHyst();	// freeze incumbency for this view (order-stable ranking)
	// Free the whole cache when it can't be used, so its VRAM doesn't linger - and so the
	// cache budget stops applying the moment it's redundant:
	//   - r_shadowMapCache off (explicit), or
	//   - r_rtAllLights with a live TLAS: every shadow-casting light takes the inline RT ray and
	//     NO shadow maps are rendered at all, so any cached cubes/2D maps are dead weight. Hand
	//     that VRAM straight back to the RT scene instead of holding a whole budget's worth of
	//     shadows nothing will ever sample. (Without a TLAS - RT off/unsupported - lights fall
	//     back to maps, so the normal cache + budget still apply.)
	const bool rtAllLightsServes = r_rtAllLights.GetBool() && r != NULL && r->GetTlasAddress() != 0;
	if ( !r_shadowMapCache.GetBool() || rtAllLightsServes ) {
		if ( rhiCubeCacheBytes > 0 ) {
			RB_RHI_ResetCubeCache( r );
		}
		RB_RHI_Reset2DCache( r );		// 2D maps are tiny; a no-op once already empty
	}
}

/*
===================
RB_RHI_LogShadowMapSummary

r_shadowMapDebug: one line summarizing how this view's lights were classified
and how the cube cache performed. Lifted verbatim from RB_RHI_DrawWorld.
===================
*/
static void RB_RHI_LogShadowMapSummary( int dbgLit, int dbgProjected, int dbgShadowMapped,
                                        int dbgNoShadow, int dbgPoint, int dbgParallel,
                                        int dbgNoLightDef, int dbgStencilBig ) {
	if ( r_shadowMapDebug.GetBool() ) {
		common->Printf( "shadowMap: %d lit | projected %d (2D-mapped %d [%d hit/%d rendered], no-shadow %d) | point %d (cube-mapped %d, cube-casters %d, faces %d drawn/%d culled) | cache %d hit/%d rendered/%d scratch, %d dynamic, %d deferred (%zu MB) | stencil-big %d | parallel %d | no-lightDef %d | perforated-casters %d | r_shadowMapping %d\n",
		                dbgLit, dbgProjected, dbgShadowMapped, rhiMapCacheHits, rhiMapCacheRendered, dbgNoShadow,
		                dbgPoint, rhiShadowCubeLights, rhiShadowCubeCasters,
		                rhiShadowCubeFaces, rhiShadowCubeFacesCulled,
		                rhiCubeCacheHits, rhiCubeCacheMiss, rhiCubeCacheScratch, rhiCubeCacheDynamic, rhiCubeCacheDeferred,
		                rhiCubeCacheBytes / ( 1024 * 1024 ),
		                dbgStencilBig, dbgParallel, dbgNoLightDef,
		                rhiShadowPerfCasters, r_shadowMapping.GetInteger() );
	}
}

/*
===================
RB_RHI_LogShadowCacheSummary

r_shadowMapCacheDebug: accumulates the per-view cube-cache tallies over ~1s
and prints a single sustained line. Lifted verbatim from RB_RHI_DrawWorld.
===================
*/
static void RB_RHI_LogShadowCacheSummary() {
	// r_shadowMapCacheDebug: accumulate the per-view cube-cache tallies over ~1 second and
	// print a single sustained line, so the dominant re-render cause is legible instead of
	// per-frame spam. Hit% is over cached lookups (excludes scratch/dynamic lights, which
	// never enter the cache). This is what says which caching lever is worth building.
	if ( r_shadowMapCacheDebug.GetBool() ) {
		static int accHits, accCold, accWarmCaster, accWarmLight, accScratch, accDynamic, accDeferred, accEvict, accFaces, accSplit, accRtMoving, accFrames, accStartMs;
		const int nowMs = Sys_Milliseconds();
		if ( accStartMs == 0 ) { accStartMs = nowMs; }
		accHits       += rhiCubeCacheHits;
		accCold       += rhiCubeMissCold;
		accWarmCaster += rhiCubeMissWarmCaster;
		accWarmLight  += rhiCubeMissWarmLight;
		accScratch    += rhiCubeCacheScratch;
		accDynamic    += rhiCubeCacheDynamic;
		accDeferred   += rhiCubeCacheDeferred;
		accEvict      += rhiCubeEvictions;
		accFaces      += rhiShadowCubeFaces;		// actual cube FACES rasterized -> the GPU cost per-face invalidation cuts
		accSplit      += rhiCubeCacheSplit;			// moving-caster lights kept cached via the static/dynamic split
		accRtMoving   += rhiCubeCacheRtMoving;		// moving lights served by a ray, no cube at all (r_rtMovingLights)
		accFrames++;
		if ( nowMs - accStartMs >= 1000 ) {
			const int rendered = accCold + accWarmCaster + accWarmLight;
			const int lookups  = accHits + rendered;
			const int hitPct   = lookups > 0 ? ( 100 * accHits ) / lookups : 0;
			common->Printf( "cubeCache/s: %d hit (%d%%) | rendered %d = cold %d + warm[caster %d, light %d] | faces %d | split %d, rtMoving %d, scratch %d, dynamic %d, deferred %d, evict %d | %d frames\n",
			                accHits, hitPct, rendered, accCold, accWarmCaster, accWarmLight, accFaces,
			                accSplit, accRtMoving, accScratch, accDynamic, accDeferred, accEvict, accFrames );
			accHits = accCold = accWarmCaster = accWarmLight = accScratch = accDynamic = accDeferred = accEvict = accFaces = accSplit = accRtMoving = accFrames = 0;
			accStartMs = nowMs;
		}
	}
}

/*
===================
lightShadowClass_t

Per-light shadow classification: cheap read-only queries of the viewLight,
computed once per light in RB_RHI_DrawWorld and shared by the technique
selection, the per-light debug readout, and the stencil-vs-map draw decision.
===================
*/
struct lightShadowClass_t {
	bool	castsShadows;		// stencil shadow volumes present (global||local shadows)
	bool	hasInteractions;	// local||global interaction geometry to light
	bool	lightMayShadow;		// light-level flags permit shadow casting
	bool	isPoint;
	bool	isParallel;
	bool	smEnabled;			// r_shadowMapping
	bool	oversize;			// giant "sun replacement" omni routed away from a single map
	bool	rtServesAll;		// r_rtAllLights serves this light with an inline ray
};

/*
===================
RB_RHI_ClassifyLightShadow

Sets ictx.lightIsFlashlight and returns this light's shadow classification:
pure read-only frontend queries (lightDef->parms, light shader) plus the
r_rtAllLights / r_shadowMapStencilRadius policy gates. Lifted verbatim from
RB_RHI_DrawWorld.
===================
*/
static lightShadowClass_t RB_RHI_ClassifyLightShadow( rhi::RHI *r, viewLight_t *vLight ) {
	// Flag the player flashlight so its interactions get the dedicated small
	// shadow bias (see r_shadowMapFlashlightBias). Keyed on the light shader
	// name: base Doom 3 uses "lights/flashlight5"; a substring match also
	// covers D3XP/mod flashlight variants without hardcoding one path.
	ictx.lightIsFlashlight = vLight->lightShader
	    && idStr::FindText( vLight->lightShader->GetName(), "flashlight", false ) != -1;

	// stencil shadow volumes present for this light (built only for opaque,
	// non-noShadows casters)
	const bool castsShadows = ( vLight->globalShadows || vLight->localShadows );

	// Perforated grates/fences are flagged noShadows, so they build NO stencil
	// shadow volumes — gating the shadow-map pass on castsShadows would skip a
	// light that only illuminates a grate, defeating the whole perforated
	// feature. Instead run the map whenever the light is permitted to cast
	// shadows (light-level flags) and has interaction geometry to render into
	// it; the per-surface caster rules still apply inside the pass.
	const bool hasInteractions = ( vLight->localInteractions || vLight->globalInteractions );
	const bool lightMayShadow = vLight->lightDef && !vLight->lightDef->parms.noShadows
	    && vLight->lightShader->LightCastsShadows();
	const bool isPoint = vLight->lightDef && vLight->lightDef->parms.pointLight;
	const bool isParallel = vLight->lightDef && vLight->lightDef->parms.parallel;
	// M7: shadow maps run on Vulkan now (the RHI depth render-target family
	// is live) — projected/spot lights get a 2D depth map, budgeted point
	// lights a cube depth map, everything else still falls back to stencil.
	const bool smEnabled = r_shadowMapping.GetBool();

	// DUDE Phase 3.5: giant "sun replacement" lights (Phobos fakes its sky
	// with omni lights up to radius 5000) look poor as a single shadow map —
	// one cube can't resolve a shadow thrown thousands of units, so its edges
	// pixelate, and the map wastes VRAM. Above r_shadowMapStencilRadius (largest
	// light_radius axis, world units) we skip the map and let Carmack's stencil
	// volumes handle it: pixel-exact and distance-independent. 0 disables.
	const float smStencilRadius = r_shadowMapStencilRadius.GetFloat();
	float lightMaxAxis = 0.0f;
	if ( vLight->lightDef ) {
		const idVec3 &lr = vLight->lightDef->parms.lightRadius;
		lightMaxAxis = Max( lr.x, Max( lr.y, lr.z ) );
	}
	// The player flashlight is a narrow projected spot with flashRadius 400
	// (weapon_flashlight.def), which trips the 255 default cutoff even though a
	// single 2D map resolves a bounded cone perfectly — a false positive of a rule
	// meant for giant omni suns. Exempt it so it takes the (tessellatable) 2D-map
	// path like every other projected light instead of a low-poly stencil shadow.
	const bool oversize = smEnabled && smStencilRadius > 0.0f
	    && lightMaxAxis > smStencilRadius && !ictx.lightIsFlashlight;

	// r_rtAllLights: EVERY shadow-casting light takes the ray route - checked ahead of all
	// the map routes and independent of r_shadowMapping. Mode 4 traces one ray from the lit
	// fragment toward u_localLightOrigin, which is right for every species: a point light's
	// position, a projected light's apex, and for a parallel light the far-away parallel
	// point (the normalized difference IS its direction) - the same code the sun and
	// moving-light routes already run. No map of any kind is rendered, and no stencil
	// volumes are drawn. Without the RT variant / a live TLAS the light falls through to
	// its usual route.
	const bool rtServesAll = r_rtAllLights.GetBool() && lightMayShadow && hasInteractions
		&& ictx.interactionRtProg != 0 && r->GetTlasAddress() != 0;
	lightShadowClass_t cls;
	cls.castsShadows    = castsShadows;
	cls.hasInteractions = hasInteractions;
	cls.lightMayShadow  = lightMayShadow;
	cls.isPoint         = isPoint;
	cls.isParallel      = isParallel;
	cls.smEnabled       = smEnabled;
	cls.oversize        = oversize;
	cls.rtServesAll     = rtServesAll;
	return cls;
}

/*
===================
RB_RHI_SelectLightShadowTechnique

Runs the shadow-technique ladder for one light and fills ictx: the
r_rtAllLights inline ray, then (shadow mapping on) the sun/2D/cube routes with
their r_rtSunShadows / r_rtMovingLights ray shortcuts, then the r_rtShadowBlur
screen-space mask. Bumps dbgShadowMapped and the global cube tallies. Lifted
verbatim from RB_RHI_DrawWorld.
===================
*/
static void RB_RHI_SelectLightShadowTechnique( rhi::RHI *r, const viewDef_t *viewDef,
        viewLight_t *vLight, const lightShadowClass_t &cls, rhi::ShaderHandle shadowMapProg,
        rhi::ShaderHandle shadowCubeProg, int &dbgShadowMapped ) {
	const bool hasInteractions = cls.hasInteractions;
	const bool lightMayShadow  = cls.lightMayShadow;
	const bool isPoint         = cls.isPoint;
	const bool isParallel      = cls.isParallel;
	const bool smEnabled       = cls.smEnabled;
	const bool oversize        = cls.oversize;
	const bool rtServesAll     = cls.rtServesAll;

	ictx.lightShadowMapped = false;
	ictx.lightSunShadow = false;
	ictx.lightRtOnly = false;
	ictx.lightRtPoint = false;
	ictx.lightRtBlur = false;
	ictx.shadowMaskImage = 0;
	ictx.shadowMaskStale = false;
	ictx.shadowImage = 0;
	ictx.lightShadowCube = false;
	ictx.shadowCubeImage = 0;
	ictx.lightHasDynamicLayer = false;
	ictx.shadowCubeDynImage = 0;
	ictx.shadowCubeDynScale = 1.0f;

	if ( rtServesAll ) {
		ictx.lightRtPoint = true;
		ictx.lightRange = isPoint ? RB_RHI_PointLightRange( vLight ) : 0.0f;
		dbgShadowMapped++;
	}

	if ( !rtServesAll && smEnabled && lightMayShadow && hasInteractions ) {
		if ( ( oversize || isParallel ) && r_shadowMapSun.GetBool() ) {
			// DUDE sun shadow maps (docs/shadow-research.md item 1): oversize
			// "sun replacement" omnis and parallel lights render a per-view
			// fitted virtual 2D map instead of the stencil fallback. The fit
			// requires the light to sit OUTSIDE the fitted view sphere (a
			// projection from the light toward the view) — a distant sky sun
			// qualifies; a big omni you stand next to does not, and declines.
			//
			// RT sun shadows (R3): when the ray path is live it serves ANY sun
			// with a single ray toward the light origin — no fitted map needed —
			// so SKIP the sun-map render entirely (checked first). That map would
			// only ever be sampled by the non-RT fallback. This is the perf win
			// for fit-PASSING suns (parallel skies, mars_city1) that used to
			// render a full 2048² depth pass and then ignore it in favour of rays;
			// fit-DECLINED low omnis (commoutside) already took this route on
			// decline. lightRtOnly marks "sun route, no map rendered": mode 4
			// samples no map (unit 7 is a 0 handle, identical to unshadowed
			// lights), and the receiver fill downgrades to unshadowed if the TLAS
			// dies mid-frame. rhiSunPlanes stay stale-but-unread under mode 4.
			const bool rtServesSun = r_rtSunShadows.GetBool()
				&& ictx.interactionRtProg != 0 && r->GetTlasAddress() != 0;
			if ( rtServesSun ) {
				ictx.lightShadowMapped = true;
				ictx.lightSunShadow = true;
				ictx.lightRtOnly = true;
				ictx.shadowImage = r->GetRenderTargetImage( rhiShadowMap );	// 0 when no map rendered; unread by mode 4
				dbgShadowMapped++;
			} else if ( RB_RHI_ShadowMapPassSun( r, vLight, shadowMapProg ) ) {
				ictx.lightShadowMapped = true;
				ictx.lightSunShadow = true;
				ictx.shadowImage = r->GetRenderTargetImage( rhiShadowMap );
				dbgShadowMapped++;
			}
		}
		if ( !ictx.lightShadowMapped && !oversize && !isPoint && !isParallel ) {
			// projected / spot light: single 2D depth map
			if ( RB_RHI_ShadowMapPass( r, vLight, shadowMapProg ) ) {
				ictx.lightShadowMapped = true;
				ictx.shadowImage = r->GetRenderTargetImage( rhiShadowMap );
				dbgShadowMapped++;
			}
		} else if ( !ictx.lightShadowMapped && isPoint && !isParallel
		            // an oversize point light reaches the cube path only when the sun
		            // fit declined it (light inside the fitted region = a big indoor
		            // omni, not a distant sun): the adaptive tiers give it a usable
		            // cube, and stencil stays the last resort. With r_shadowMapSun
		            // off, oversize keeps the old direct-to-stencil routing for A/B.
		            && ( !oversize || r_shadowMapSun.GetBool() ) ) {
			// RT moving lights (Option A, docs/shadow-research.md 2026-08): a point
			// light whose pose changed recently can never cache-hit — the map path
			// re-renders its whole cube every frame — so serve it with one ray
			// toward the light instead (mode 4, no cube at all). Checked BEFORE the
			// budget so ray-served lights don't consume cube slots. Costs nothing
			// when still: the tracker's hysteresis re-caches paused lights.
			const bool rtServesMoving = r_rtMovingLights.GetBool()
				&& ictx.interactionRtProg != 0 && r->GetTlasAddress() != 0
				&& RB_RHI_LightIsMoving( vLight );
			if ( rtServesMoving ) {
				ictx.lightRtPoint = true;
				ictx.lightRange = RB_RHI_PointLightRange( vLight );
				rhiCubeCacheRtMoving++;
				dbgShadowMapped++;
			} else if ( RB_RHI_PointLightInBudget( viewDef, vLight ) ) {
				// point / omni light: 6-face cube map, budgeted by on-screen
				// importance (r_shadowMapPointLimit) so a busy room stays bounded
				const float range = RB_RHI_PointLightRange( vLight );
				if ( RB_RHI_ShadowMapPassCube( r, vLight, shadowCubeProg, range ) ) {
					ictx.lightShadowCube = true;
					ictx.shadowCubeImage = r->GetRenderTargetImage( rhiShadowCube );
					// static/dynamic split: a second (movers) cube to min() with the static one
					if ( rhiShadowCubeDyn != 0 ) {
						ictx.lightHasDynamicLayer = true;
						ictx.shadowCubeDynImage = r->GetRenderTargetImage( rhiShadowCubeDyn );
						ictx.shadowCubeDynScale = rhiShadowCubeDynScale;
					}
					ictx.lightRange = range;
					rhiShadowCubeLights++;
				}
			}
		}
	}

	// either shadow-map technique (or the ray route) replaces the stencil test
	// r_rtShadowBlur: a light one of the ray routes above just claimed gets its hard ray
	// traced per screen pixel + blurred NOW (the scene pass is suspended exactly as for a
	// shadow-map render); its interactions then look the mask up instead of tracing inline.
	// Any failure leaves the light on its inline hard ray, unchanged.
	if ( rhiRtBlurThisView && ( ictx.lightRtOnly || ictx.lightRtPoint ) ) {
		ictx.shadowMaskImage = RB_RHI_RtShadowBlurLight( r, viewDef, vLight );
		ictx.lightRtBlur = ictx.shadowMaskImage != 0;
		ictx.shadowMaskStale = ictx.lightRtBlur && rhiRtBlurLastStale;
		if ( ictx.shadowMaskStale ) {
			memcpy( ictx.shadowMaskViewProj, rhiRtBlurLastViewProj, sizeof( ictx.shadowMaskViewProj ) );
		}
	}
}

/*
===================
RB_RHI_LogLightShadowClass

r_shadowMapDebug 2: per-light readout (species, chosen technique, adaptive
resolution, receiver/caster counts, distance/radius/range). Lifted verbatim
from RB_RHI_DrawWorld.
===================
*/
static void RB_RHI_LogLightShadowClass( const viewDef_t *viewDef, viewLight_t *vLight, const lightShadowClass_t &cls ) {
	const bool isPoint        = cls.isPoint;
	const bool isParallel     = cls.isParallel;
	const bool lightMayShadow = cls.lightMayShadow;
	const bool oversize       = cls.oversize;
	const bool rtServesAll    = cls.rtServesAll;
	// r_shadowMapDebug 2: per-light readout so we can see, when a shadow blinks
	// out, exactly what changed — is the light even here, did it get a map, and
	// how many occluders reached it (interactions + off-screen casters)?
	if ( r_shadowMapDebug.GetInteger() >= 2 && vLight->lightDef ) {
		const idVec3 lorg = vLight->globalLightOrigin;
		const idVec3 dv = lorg - viewDef->renderView.vieworg;
		const float dist = dv.Length();
		const float radius = vLight->lightDef->parms.lightRadius.Length();
		// litG/litL = lit (receiver) surface counts; casters = occluders drawn
		// into the shadow map. range = cube depth normalizer / far plane.
		const float effRange = isPoint ? RB_RHI_PointLightRange( vLight ) : 0.0f;
		// adaptive resolution this light would/did use, so the tier picks show up
		const int dbgLo = isPoint ? 128 : 256;
		const int dbgHi = isPoint ? idMath::ClampInt( 128, 4096, glConfig.maxCubeMapSize )
		                          : idMath::ClampInt( 256, 4096, glConfig.maxTextureSize );
		const int dbgBase = idMath::ClampInt( dbgLo, dbgHi,
		    isPoint ? r_shadowMapPointSize.GetInteger() : r_shadowMapSize.GetInteger() );
		const int dbgRes = RB_RHI_TierSize( dbgBase, RB_RHI_ShadowTier( radius ), dbgLo, dbgHi );
		common->Printf( "  light %d: %s%s map=%s res=%d litG=%d litL=%d casters=%d dist=%.0f radius=%.0f range=%.0f\n",
		                vLight->lightDef->index,
		                isPoint ? "point" : ( isParallel ? "parallel" : "projected" ),
		                lightMayShadow ? "" : " (noShadow)",
		                ictx.lightRtPoint ? ( rtServesAll ? "rtAll" : "rtMove" ) : ( ictx.lightShadowCube ? "cube" : ( ictx.lightSunShadow ? "sun" : ( ictx.lightShadowMapped ? "2D" : ( oversize ? "stencil" : "none" ) ) ) ),
		                dbgRes,
		                RB_RHI_CountLightChain( vLight->globalInteractions ),
		                RB_RHI_CountLightChain( vLight->localInteractions ),
		                RB_RHI_CountLightChain( vLight->shadowMapCasters ),
		                dist, radius, effRange );
	}
}

/*
===================
RB_RHI_DrawWorld

Depth prepass + stencil shadows + per-light interactions, following the
RB_STD_DrawView / RB_ARB2_DrawInteractions pass order.
===================
*/
void RB_RHI_DrawWorld( rhi::RHI *r, viewDef_s *viewDef ) {
	rhi::ShaderHandle shadowProg = r->LoadShader( "shadow" );
	rhi::ShaderHandle shadowMapProg = r->LoadShader( "shadow_sm" );
	rhi::ShaderHandle shadowCubeProg = r->LoadShader( "shadow_sm_cube" );

	ictx.r = r;
	ictx.viewDef = viewDef;
	ictx.interactionProg = r->LoadShader( "interaction" );
	// RT sun shadows (R3, docs/rtx-shadow-roadmap.md): the ray-query variant's SPIR-V
	// capability only pipeline-creates on RT hardware, so it loads gated; 0 elsewhere
	// and the mode-4 override in RB_RHI_DrawInteraction stays off.
	ictx.interactionRtProg = r->SupportsRayQuery() ? r->LoadShader( "interaction_rt" ) : 0;
	ictx.ambientProg = r->LoadShader( "ambientlight" );
	ictx.stencilState = rhi::SS_ALWAYS;

	// sets backEnd.lightScale/overBright, read by the reused
	// RB_CreateSingleDrawInteractions
	RB_DetermineLightScale();

	// depth prepass with stencil test enabled for invariance with the
	// shadowed passes (matches RB_STD_FillDepthBuffer)
	if ( qglEnable != NULL ) {
		qglEnable( GL_STENCIL_TEST );
	}
	backEnd.currentScissor = viewDef->scissor;

	// Phase 4 M4 (docs/vulkan-backend.md): the light loop now runs under
	// Vulkan — stencil shadow volumes + interactions. The render-target
	// enhancement passes (normal prepass, SSAO, SSR, shadow maps) need the
	// VK render-target family and arrive together at M7.
	const bool vkMode = rhi::GetActiveBackendType() == rhi::BT_VULKAN;
	if ( vkMode ) {
		RB_RHI_LogOnce( "VK: world = depth + stencil + shadow maps + interactions + HDR + SSAO (SSR composites later, in the view pass)" );
	}

	// Build + bind this view's lighting inputs (G-buffer/depth, AO, GI, decal mask)
	// for the per-light loop.
	RB_RHI_PrepareViewLighting( r, viewDef, vkMode );

	// per-light shadowing and adding (matches RB_ARB2_DrawInteractions)
	// r_shadowMapDebug counts how each lit light was classified this view
	int dbgLit = 0, dbgProjected = 0, dbgShadowMapped = 0, dbgPoint = 0,
	    dbgParallel = 0, dbgNoShadow = 0, dbgNoLightDef = 0, dbgStencilBig = 0;
	RB_RHI_ResetShadowTallies( r );
	if ( !r_skipInteractions.GetBool() ) {
		for ( viewLight_t *vLight = viewDef->viewLights; vLight; vLight = vLight->next ) {
			backEnd.vLight = vLight;

			// fogging / blend lights happen after shader passes (Chunk F)
			if ( vLight->lightShader->IsFogLight() || vLight->lightShader->IsBlendLight() ) {
				continue;
			}
			if ( !vLight->localInteractions && !vLight->globalInteractions && !vLight->translucentInteractions ) {
				continue;
			}
			dbgLit++;
			if ( !vLight->lightDef ) {
				dbgNoLightDef++;
			} else if ( vLight->lightDef->parms.parallel ) {
				dbgParallel++;
			} else if ( vLight->lightDef->parms.pointLight ) {
				dbgPoint++;
			} else {
				dbgProjected++;
				if ( !( vLight->globalShadows || vLight->localShadows ) ) {
					dbgNoShadow++;
				}
			}

			// DUDE Phase 3.5: choose the shadow technique for this light. Shadow maps
			// handle projected/spot lights (2D) and the budgeted top point lights
			// (cube); everything else — parallel lights, out-of-budget point lights —
			// falls back to stencil. That mix is exactly the free per-light selection.
			// Reading lightDef->parms here is a read-only frontend query.
			const lightShadowClass_t cls = RB_RHI_ClassifyLightShadow( r, vLight );
			RB_RHI_SelectLightShadowTechnique( r, viewDef, vLight, cls, shadowMapProg,
			                                   shadowCubeProg, dbgShadowMapped );

			const bool shadowMapped = ictx.lightShadowMapped || ictx.lightShadowCube || ictx.lightRtPoint;

			RB_RHI_LogLightShadowClass( viewDef, vLight, cls );

			// castsShadows / smEnabled / oversize drive the stencil-vs-map draw decision below
			const bool castsShadows = cls.castsShadows;
			const bool smEnabled    = cls.smEnabled;
			const bool oversize     = cls.oversize;

			// A light draws stencil shadow volumes when shadow mapping is fully off
			// (the vanilla path), OR when shadow mapping is on but this particular
			// light is an oversize "sun" routed to stencil (r_shadowMapStencilRadius).
			// That per-light mix is the free selection the interaction loop allows.
			// Everything else with a map draws unshadowed off the map; a light with
			// neither (out-of-budget point, parallel) still renders unshadowed.
			const bool useStencil = castsShadows && !shadowMapped && ( !smEnabled || oversize );
			if ( oversize && useStencil ) {
				dbgStencilBig++;
			}

			// scissor + stencil-clear setup: needed for either shadow technique.
			// Clear stencil only for the stencil path — the shadow-map path never
			// tests stencil.
			if ( castsShadows || shadowMapped || smEnabled ) {
				backEnd.currentScissor = vLight->scissorRect;
				if ( r_useScissor.GetBool() ) {
					r->SetScissor( viewDef->viewport.x1 + backEnd.currentScissor.x1,
					               viewDef->viewport.y1 + backEnd.currentScissor.y1,
					               backEnd.currentScissor.x2 + 1 - backEnd.currentScissor.x1,
					               backEnd.currentScissor.y2 + 1 - backEnd.currentScissor.y1 );
				}
				if ( useStencil ) {
					if ( qglClear != NULL ) {
						qglClear( GL_STENCIL_BUFFER_BIT );
					} else {
						// mid-pass, scissored stencil clear to the unshadowed value
						r->ClearStencilBuffer( 1 << ( glConfig.stencilBits - 1 ) );
					}
				}
			}
			if ( !useStencil && qglStencilFunc != NULL ) {
				// stencil always passes; visibility comes from the map (if any)
				qglStencilFunc( GL_ALWAYS, 128, 255 );
			}

			ictx.depthFuncBits = GLS_DEPTHFUNC_EQUAL;
			if ( !useStencil ) {
				// shadow-mapped, or shadow mapping on but this light isn't mapped
				// (out-of-budget point, parallel, failed pass): draw unshadowed
				ictx.stencilState = rhi::SS_ALWAYS;
				RB_RHI_CreateDrawInteractions( vLight->localInteractions );
				RB_RHI_CreateDrawInteractions( vLight->globalInteractions );
			} else {
				// interactions depth-test EQUAL and stencil-test GEQUAL 128
				// against the volumes (StencilShadowPass sets the GL state)
				ictx.stencilState = rhi::SS_SHADOW_TEST;
				RB_RHI_StencilShadowPass( r, viewDef, vLight->globalShadows, shadowProg );
				RB_RHI_CreateDrawInteractions( vLight->localInteractions );
				RB_RHI_StencilShadowPass( r, viewDef, vLight->localShadows, shadowProg );
				RB_RHI_CreateDrawInteractions( vLight->globalInteractions );
			}

			// translucent surfaces never get stencil shadowed
			if ( r_skipTranslucent.GetBool() ) {
				continue;
			}
			if ( qglStencilFunc != NULL ) {
				qglStencilFunc( GL_ALWAYS, 128, 255 );
			}
			ictx.stencilState = rhi::SS_ALWAYS;
			ictx.depthFuncBits = GLS_DEPTHFUNC_LESS;
			RB_RHI_CreateDrawInteractions( vLight->translucentInteractions );
			// r_decalWetBlood: blood decals glint off this light as surfaces of their own
			RB_RHI_DrawFilmInteractions( r, viewDef, vLight );
		}
	}
	backEnd.vLight = NULL;

	RB_RHI_LogShadowMapSummary( dbgLit, dbgProjected, dbgShadowMapped, dbgNoShadow,
	                            dbgPoint, dbgParallel, dbgNoLightDef, dbgStencilBig );
	RB_RHI_LogShadowCacheSummary();

	// shader passes run with stencil satisfied everywhere
	if ( qglStencilFunc != NULL ) {
		qglStencilFunc( GL_ALWAYS, 128, 255 );
	}
}

/*
=============================================================================

FOG AND BLEND LIGHTS

Fog and blend lights were the last fixed-function passes in the classic
renderer. They dual-texture straight to the framebuffer (projection/falloff
or fog ramp/enter) instead of interacting with the surface material, driven
by the fog/blendlight GLSL programs. This mirrors RB_STD_FogAllLights /
RB_FogPass / RB_BlendLight state bit for state bit; the texgen planes the old
path fed glTexGen now travel through RenderParams::texGen*.

=============================================================================
*/

/*
===================
RB_RHI_SetSurfScissor
===================
*/
static void RB_RHI_SetSurfScissor( rhi::RHI *r, const viewDef_t *viewDef, const drawSurf_t *surf ) {
	if ( r_useScissor.GetBool() && !backEnd.currentScissor.Equals( surf->scissorRect ) ) {
		backEnd.currentScissor = surf->scissorRect;
		r->SetScissor( viewDef->viewport.x1 + backEnd.currentScissor.x1,
		               viewDef->viewport.y1 + backEnd.currentScissor.y1,
		               backEnd.currentScissor.x2 + 1 - backEnd.currentScissor.x1,
		               backEnd.currentScissor.y2 + 1 - backEnd.currentScissor.y1 );
	}
}

/*
===================
RB_RHI_BlendLightChain

One surface chain for a blend-light stage. Mirrors RB_T_BlendLight: the
light's projection planes go to unit 0 (S/T/Q), the falloff plane to unit 1
(S; T is the constant 0.5 baked into blendlight.vert). backEnd.vLight must be
the current light.
===================
*/
static void RB_RHI_BlendLightChain( rhi::RHI *r, const viewDef_t *viewDef, const drawSurf_t *surf,
                                    rhi::ShaderHandle prog, int stateBits, const float color[4],
                                    idImage *projectionImage, idImage *falloffImage ) {
	for ( ; surf; surf = surf->nextOnLight ) {
		const srfTriangles_t *tri = surf->geo;
		if ( !tri->ambientCache && !tri->gpuSkinVB ) {
			continue;
		}

		float mvp[16];
		RB_RHI_SpaceMvp( viewDef, surf->space, mvp );

		// project the light frustum planes into this surface's local space
		idPlane lightProject[4];
		for ( int i = 0; i < 4; i++ ) {
			R_GlobalPlaneToLocal( surf->space->modelMatrix, backEnd.vLight->lightProject[i], lightProject[i] );
		}

		rhi::RenderParams parms;
		memset( &parms, 0, sizeof( parms ) );
		memcpy( parms.mvpMatrix, mvp, sizeof( parms.mvpMatrix ) );
		memcpy( parms.color, color, sizeof( parms.color ) );
		memcpy( parms.texGen0S, lightProject[0].ToFloatPtr(), 16 );
		memcpy( parms.texGen0T, lightProject[1].ToFloatPtr(), 16 );
		memcpy( parms.texGen0Q, lightProject[2].ToFloatPtr(), 16 );
		memcpy( parms.texGen1S, lightProject[3].ToFloatPtr(), 16 );

		RB_RHI_SetSurfScissor( r, viewDef, surf );

		rhi::BufferHandle ub;
		int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

		rhi::BufferHandle vb, ib;
		int vertOfs, idxOfs;
		RB_RHI_StreamAmbient( r, tri, vb, vertOfs, ib, idxOfs );
		// Roadmap B: a deformed surface draws its expanded buffer here too -- blend lights don't
		// fixed-function tessellate, but must modulate the same deformed geometry / sealed depth.
		bool useDeform = false;
		RB_RHI_TessOrDeform( surf, tri, useDeform );

		RB_RHI_BindUnit( 0, projectionImage );
		RB_RHI_BindUnit( 1, falloffImage );

		rhi::PipelineDesc pd;
		pd.stateBits = stateBits;
		pd.shader = prog;
		pd.vertexLayout = rhi::VL_DRAWVERT;
		pd.cullType = RB_RHI_CullFor( viewDef, CT_FRONT_SIDED );
		r->BindPipeline( pd );

		rhi::DrawArgs da;
		memset( &da, 0, sizeof( da ) );
		da.vertexBuffer = useDeform ? tri->tessDeformVB : vb;
		da.vertexOffset = useDeform ? 0 : vertOfs;
		da.indexBuffer = useDeform ? tri->tessDeformIB : ib;
		da.firstIndex = useDeform ? 0 : ( idxOfs / (int)sizeof( glIndex_t ) );
		da.indexCount = useDeform ? tri->tessDeformIndexes : tri->numIndexes;
		da.uniformBuffer = ub;
		da.uniformOffset = uniOfs;
		da.uniformSize = sizeof( parms );
		RB_RHI_VkTextures( da );		// VK: units 0/1 recorded by the binds above
		r->Draw( da );

		backEnd.pc.c_drawElements++;
		backEnd.pc.c_drawIndexes += tri->numIndexes;
	}
}

/*
===================
RB_RHI_BlendLight

Mirrors RB_BlendLight: for each live stage, dual-texture the projection and
falloff to the framebuffer over the light's interaction surfaces.
===================
*/
static void RB_RHI_BlendLight( rhi::RHI *r, const viewDef_t *viewDef, viewLight_t *vLight ) {
	if ( r_skipBlendLights.GetBool() || ( !vLight->globalInteractions && !vLight->localInteractions ) ) {
		return;
	}

	rhi::ShaderHandle prog = r->LoadShader( "blendlight" );
	const idMaterial *lightShader = vLight->lightShader;
	const float *regs = vLight->shaderRegisters;

	for ( int i = 0; i < lightShader->GetNumStages(); i++ ) {
		const shaderStage_t *stage = lightShader->GetStage( i );

		if ( !regs[stage->conditionRegister] ) {
			continue;
		}

		// texture-matrix (scrolling) blend-light projections are essentially
		// unused by stock content; drawn without the matrix until folded into
		// the projection planes (would need RB_BakeTextureMatrixIntoTexgen)
		if ( stage->texture.hasMatrix ) {
			RB_RHI_LogOnce( "blend light texture matrix" );
		}

		int stateBits = GLS_DEPTHMASK | stage->drawStateBits | GLS_DEPTHFUNC_EQUAL;

		// get the modulate values from the light, including alpha (unlike normal lights)
		float color[4];
		color[0] = regs[stage->color.registers[0]];
		color[1] = regs[stage->color.registers[1]];
		color[2] = regs[stage->color.registers[2]];
		color[3] = regs[stage->color.registers[3]];

		RB_RHI_BlendLightChain( r, viewDef, vLight->globalInteractions, prog, stateBits, color,
		                        stage->texture.image, vLight->falloffImage );
		RB_RHI_BlendLightChain( r, viewDef, vLight->localInteractions, prog, stateBits, color,
		                        stage->texture.image, vLight->falloffImage );
	}
}

/*
===================
RB_RHI_FogChain

One surface chain for a fog light. Mirrors RB_T_BasicFog: unit 0 is the fog
distance ramp (S = per-surface plane, T = constant 0.5), unit 1 the enter
fade (S = constant per viewer, T = per-surface top plane).
===================
*/
static void RB_RHI_FogChain( rhi::RHI *r, const viewDef_t *viewDef, const drawSurf_t *surf,
                             rhi::ShaderHandle prog, int stateBits, int cull, const float color[4],
                             const idPlane &fogPlane0, const idPlane &fogPlane2, float enterS,
                             bool allowTess ) {
	for ( ; surf; surf = surf->nextOnLight ) {
		const srfTriangles_t *tri = surf->geo;
		if ( !tri->ambientCache && !tri->gpuSkinVB ) {
			continue;
		}

		float mvp[16];
		RB_RHI_SpaceMvp( viewDef, surf->space, mvp );

		rhi::RenderParams parms;
		memset( &parms, 0, sizeof( parms ) );
		memcpy( parms.mvpMatrix, mvp, sizeof( parms.mvpMatrix ) );
		memcpy( parms.color, color, sizeof( parms.color ) );

		idPlane local;
		// unit 0 S: fog distance ramp (+0.5 to center the 128px ramp texture)
		R_GlobalPlaneToLocal( surf->space->modelMatrix, fogPlane0, local );
		local[3] += 0.5f;
		memcpy( parms.texGen0S, local.ToFloatPtr(), 16 );
		// unit 0 T: constant 0.5
		parms.texGen0T[3] = 0.5f;
		// unit 1 T: enter fade, per-surface top plane (+FOG_ENTER)
		R_GlobalPlaneToLocal( surf->space->modelMatrix, fogPlane2, local );
		local[3] += FOG_ENTER;
		memcpy( parms.texGen1T, local.ToFloatPtr(), 16 );
		// unit 1 S: enter fade, constant per viewer
		parms.texGen1S[3] = enterS;

		// DUDE tessellation: a character/monster surface was PN-tessellated (and
		// normal-displaced) in the depth prepass, so its z-buffer depth is the
		// tessellated depth. This fog interaction pass runs at DEPTHFUNC_EQUAL, so
		// it MUST tessellate the same way (fog.tesc/.tese mirror zfill) or every fog
		// fragment fails the equal test and the model renders un-fogged — a dark
		// silhouette in the fog. Never on the frustum-volume fill (allowTess false).
		idImage *bumpImg = NULL;
		bool useDeform = false;
		const bool tess = allowTess && RB_RHI_TessOrDeform( surf, tri, useDeform );
		if ( tess ) {
			RB_RHI_SetTessParms( parms );
			bumpImg = RB_RHI_TessBumpForZfill( surf, parms );	// sets bumpMatrix; fog.tese displaces
		}

		RB_RHI_SetSurfScissor( r, viewDef, surf );

		rhi::BufferHandle ub;
		int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

		rhi::BufferHandle vb, ib;
		int vertOfs, idxOfs;
		RB_RHI_StreamAmbient( r, tri, vb, vertOfs, ib, idxOfs );

		RB_RHI_BindUnit( 0, globalImages->fogImage );
		RB_RHI_BindUnit( 1, globalImages->fogEnterImage );
		if ( tess ) {
			RB_RHI_BindUnit( 2, bumpImg );	// fog.tese displacement source (unit 2; 0/1 are fog textures)
		}

		rhi::PipelineDesc pd;
		pd.stateBits = stateBits;
		pd.shader = prog;
		pd.vertexLayout = rhi::VL_DRAWVERT;
		pd.cullType = RB_RHI_CullFor( viewDef, cull );
		pd.tessellate = tess;
		RB_RHI_BindTessHeight( surf, tess );	// DUDE: unit 15 signed height (docs/tessellation.md)
		r->BindPipeline( pd );

		rhi::DrawArgs da;
		memset( &da, 0, sizeof( da ) );
		da.vertexBuffer = useDeform ? tri->tessDeformVB : vb;
		da.vertexOffset = useDeform ? 0 : vertOfs;
		da.indexBuffer = useDeform ? tri->tessDeformIB : ib;
		da.firstIndex = useDeform ? 0 : ( idxOfs / (int)sizeof( glIndex_t ) );
		da.indexCount = useDeform ? tri->tessDeformIndexes : tri->numIndexes;
		da.uniformBuffer = ub;
		da.uniformOffset = uniOfs;
		da.uniformSize = sizeof( parms );
		RB_RHI_VkTextures( da );		// VK: units 0/1 (+2 bump when tessellating) recorded by the binds above
		r->Draw( da );

		backEnd.pc.c_drawElements++;
		backEnd.pc.c_drawIndexes += tri->numIndexes;
	}
}

/*
===================
RB_RHI_FogLight

Mirrors RB_FogPass: dual-texture the fog ramp (unit 0) and enter fade
(unit 1) over the light's interaction surfaces, then the light frustum with
DEPTHFUNC_LESS so the far side of the fog volume is filled.
===================
*/
static void RB_RHI_FogLight( rhi::RHI *r, viewDef_t *viewDef, viewLight_t *vLight ) {
	const srfTriangles_t *frustumTris = vLight->frustumTris;
	// if we ran out of vertex cache memory, skip it
	if ( !frustumTris->ambientCache ) {
		return;
	}

	const idMaterial *lightShader = vLight->lightShader;
	const float *regs = vLight->shaderRegisters;
	// assume fog shaders have only a single stage
	const shaderStage_t *stage = lightShader->GetStage( 0 );

	float lightColor[4];
	lightColor[0] = regs[stage->color.registers[0]];
	lightColor[1] = regs[stage->color.registers[1]];
	lightColor[2] = regs[stage->color.registers[2]];
	lightColor[3] = regs[stage->color.registers[3]];

	// fog.frag builds alpha from the two ramps and multiplies rgb by u_color;
	// the old path set qglColor3fv so alpha stays 1 (density lives in
	// lightColor[3], and drives the ramp slope below — not u_color.a)
	float color[4] = { lightColor[0], lightColor[1], lightColor[2], 1.0f };

	// calculate the falloff planes
	float a;
	// if they left the default value on, set a fog distance of 500
	if ( lightColor[3] <= 1.0f ) {
		a = -0.5f / DEFAULT_FOG_DISTANCE;
	} else {
		// otherwise, distance = alpha color
		a = -0.5f / lightColor[3];
	}

	// unit-0 distance plane from the eye-space Z row of the world modelview
	const float *mv = viewDef->worldSpace.modelViewMatrix;
	idPlane fogPlane0( a * mv[2], a * mv[6], a * mv[10], a * mv[14] );

	// unit-1 enter fade: the "top" fade plane, and the eye's distance to it
	idPlane fogPlane2( 0.001f * vLight->fogPlane[0], 0.001f * vLight->fogPlane[1],
	                   0.001f * vLight->fogPlane[2], 0.001f * vLight->fogPlane[3] );
	float s = viewDef->renderView.vieworg * fogPlane2.Normal() + fogPlane2[3];
	float enterS = FOG_ENTER + s;

	rhi::ShaderHandle prog = r->LoadShader( "fog" );

	int stateEqual = GLS_DEPTHMASK | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_EQUAL;
	// interaction chains run at DEPTHFUNC_EQUAL over real geometry -> allow tessellation
	// so PN-tessellated models fog against their own (tessellated) depth.
	RB_RHI_FogChain( r, viewDef, vLight->globalInteractions, prog, stateEqual, CT_FRONT_SIDED, color, fogPlane0, fogPlane2, enterS, true );
	RB_RHI_FogChain( r, viewDef, vLight->localInteractions, prog, stateEqual, CT_FRONT_SIDED, color, fogPlane0, fogPlane2, enterS, true );

	// the light frustum bounding planes aren't in the depth buffer, so use
	// DEPTHFUNC_LESS instead of EQUAL and draw the volume's far (back) side
	drawSurf_t ds;
	memset( &ds, 0, sizeof( ds ) );
	ds.space = &viewDef->worldSpace;
	ds.geo = frustumTris;
	ds.scissorRect = viewDef->scissor;
	int stateLess = GLS_DEPTHMASK | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_LESS;
	// the frustum-volume fill is a synthetic worldspace hull, never a tessellated model
	RB_RHI_FogChain( r, viewDef, &ds, prog, stateLess, CT_BACK_SIDED, color, fogPlane0, fogPlane2, enterS, false );
}

/*
===================
RB_RHI_FogAllLights

Mirrors RB_STD_FogAllLights: after the interaction and ambient passes, add
every fog and blend light. Runs with the stencil test disabled (the classic
path guarantees no double-fogging by scissor alone).
===================
*/
void RB_RHI_FogAllLights( rhi::RHI *r, viewDef_t *viewDef ) {
	// note: r_skipFogLights skips the whole function (blend lights too) exactly
	// as legacy RB_STD_FogAllLights; r_skipBlendLights is checked per blend light
	if ( r_skipFogLights.GetBool() || r_showOverDraw.GetInteger() != 0 || viewDef->isXraySubview ) {
		return;
	}

	// GL: raw stencil-test disable around the pass; Vulkan pipelines carry
	// SS_DISABLED in their PipelineDesc already (the default)
	if ( qglDisable != NULL ) {
		qglDisable( GL_STENCIL_TEST );
	}

	for ( viewLight_t *vLight = viewDef->viewLights; vLight; vLight = vLight->next ) {
		backEnd.vLight = vLight;

		if ( vLight->lightShader->IsFogLight() ) {
			RB_RHI_FogLight( r, viewDef, vLight );
		} else if ( vLight->lightShader->IsBlendLight() ) {
			RB_RHI_BlendLight( r, viewDef, vLight );
		}
	}
	backEnd.vLight = NULL;

	if ( qglEnable != NULL ) {
		qglEnable( GL_STENCIL_TEST );
	}
}
