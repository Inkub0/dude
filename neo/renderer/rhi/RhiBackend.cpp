/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.cpp for license header)
===========================================================================
*/

// DUDE RHI command executor — Phase 3 Chunks C–F.
//
// Translates the idTech4 backend command list into RHI calls; this layer is
// what the Vulkan backend will reuse unchanged. Current coverage:
//   - 2D views (menu/console/HUD/loading): full old-style shader stage
//     rendering through the generic program
//   - 3D views: depth prepass + stencil shadows + interactions (RhiWorld.cpp),
//     ambient shader passes, fog + blend lights, then _currentRender
//     post-process surfaces (heat haze / RoE grabber warp) and the optional
//     DUDE film-grain / chromatic-aberration fullscreen pass
//   - RC_COPY_RENDER: explicit framebuffer-to-image copies
//
// Two acknowledged impurities, both bridged until Phase 4:
//   - engine textures bind via idImage::Bind() (core-safe since the
//     Image_load.cpp guards), not through DrawArgs::textures
//   - polygon offset / framebuffer copies are direct GL calls (need RHI
//     dynamic state / CopyFramebufferToImage in the Vulkan backend)

#include "sys/platform.h"
#include "idlib/geometry/JointTransform.h"
#include "framework/FileSystem.h"
#include "framework/CmdSystem.h"
#include "renderer/RenderWorld_local.h"
#include "renderer/tr_local.h"
#include "renderer/VertexCache.h"
#include "renderer/Cinematic.h"
#include "renderer/rhi/RHI.h"
#include "renderer/rhi/GL3Local.h"
#include "renderer/rhi/RenderParams.h"
#include "renderer/rhi/RhiTess.h"
#include "renderer/rhi/ArbParamsBlock.h"
#include "renderer/rhi/MaterialIR.h"

#include "sys/sys_imgui.h"

// ---------------------------------------------------------------------------
// Active-backend selection (Phase 4 M0, docs/vulkan-backend.md). R_InitOpenGL
// records which RHI backend it brought up; the executor and every helper ask
// GetRHI() instead of naming one. Defaults to GL3 so tool code that runs before
// selection (and is already gated on glConfig.rhiBackend) can't dereference NULL.
// ---------------------------------------------------------------------------
namespace rhi {

static BackendType rhiActiveBackend = BT_GL3;

void SetActiveBackend( BackendType type ) {
	rhiActiveBackend = type;
}

BackendType GetActiveBackendType() {
	return rhiActiveBackend;
}

RHI *GetRHI() {
#ifdef DHEWM3_VULKAN
	if ( rhiActiveBackend == BT_VULKAN ) {
		RHI *vk = GetVulkanRHI();
		if ( vk ) {
			return vk;
		}
		// M0: the Vulkan backend doesn't exist yet; R_InitOpenGL never selects
		// BT_VULKAN, so this is a defensive fallback, not a live path.
	}
#endif
	return GetGL3RHI();
}

} // namespace rhi

// DUDE post-process toggles (defined in RenderSystem_init.cpp), improvements
// menu, default off — the fullscreen film-grain / chromatic-aberration pass
extern idCVar r_postFilmGrain;
extern idCVar r_postFilmGrainSize;
extern idCVar r_postChromaticAberration;
extern idCVar r_rhiAA;
extern idCVar r_fxaaStrength;
extern idCVar r_hdrTonemap;
extern idCVar r_hdrExposure;
extern idCVar r_hdrOverbright;

// DUDE gamma/brightness in shader (RenderSystem_init.cpp). On the core context
// there is no fixed-function/ARB gamma and SDL3 has no hardware gamma ramp, so
// r_gammaInShader is applied as a final fullscreen pass (RB_RHI_GammaBrightness).
extern idCVar r_gammaInShader;
extern idCVar r_gamma;
extern idCVar r_brightness;

// DUDE berserk vision (RHI backends): a faithful port of the stock ARB material
// textures/decals/berserk (materials/decals.mtr). That material recursively re-samples the
// previous frame magnified ~3% about the centre (centerscale), gated by the berserk2
// texture's radial alpha (sharp centre, feedback edges) — the "streak zoom". Its recursive
// _scratch capture doesn't accumulate on the RHI path, so the exact same operations run
// every frame into a ping-pong render target instead (berserk_accum), and the display blit
// shows the accumulated buffer (the `berserk` builtin). Legacy keeps its original path.
// The look constants (0.95 centerscale per 60fps-frame, full mask feedback, full-res trail)
// are baked into the RB_RHI_BerserkAccum call below — tuned and locked, no user knobs.
// game-driven strength: 1 while berserk is active, ramps 1->0 over the 2s wind-down after it
// ends (PlayerView.cpp writes it, this backend reads it as both the intercept trigger and the
// wind-down fade). Not a user knob — the game<->renderer bridge that drives the effect.
idCVar r_berserkFade( "r_berserkFade", "0", CVAR_RENDERER | CVAR_FLOAT,
	"berserk vision effect strength (set by the game: 1 active, fading to 0 as berserk ends)" );

// DUDE: brightness scale for cube-map ("sheen") reflections on glass etc.
// (r_gl3ReflectionScale, defined in RenderSystem_init.cpp) — the enhancement
// backends light the scene brighter than the original renderer, so the
// environment-map reflection reads too strong; 0.7 (-30%) matches the legacy
// look, 1.0 leaves the cube untouched. Applied in RB_RHI_RenderTexgenStage.

static unsigned char *rbCaptureDest = NULL;

void RB_RHI_CaptureNextSwap( unsigned char *dest ) {
	rbCaptureDest = dest;
}

/*
=============
RB_RHI_CopyCurrentRender

Snapshot the framebuffer viewport into the oversized POT _currentRender
texture. Direct-GL idImage copy, core-safe like the depth capture in
RhiWorld (TODO(RHI): becomes CopyFramebufferToImage in the Vulkan backend).
=============
*/
static void RB_RHI_CopyCurrentRender( const viewDef_t *viewDef ) {
	globalImages->currentRenderImage->CopyFramebuffer( viewDef->viewport.x1, viewDef->viewport.y1,
		viewDef->viewport.x2 - viewDef->viewport.x1 + 1,
		viewDef->viewport.y2 - viewDef->viewport.y1 + 1, true );
}

// ---------------------------------------------------------------------------
// DUDE SMAA 1x (r_rhiAA 2, docs/antialiasing.md). Vendored reference
// implementation (neo/shaders/smaa.glsl); the two constant lookup textures
// ship as byte arrays and upload once as idImages.
// ---------------------------------------------------------------------------
#include "renderer/rhi/smaa/AreaTex.h"
#include "renderer/rhi/smaa/SearchTex.h"

static rhi::RenderTargetHandle	rhiSmaaEdgesRT = 0;		// RG edge mask (RGBA8)
static rhi::RenderTargetHandle	rhiSmaaWeightsRT = 0;	// blending weights (RGBA8)
static rhi::RenderTargetHandle	rhiSmaaSceneRT = 0;		// de-POT'd LDR scene copy (RGBA8)
static int rhiSmaaW = 0, rhiSmaaH = 0;
// The LUTs are NOT idImages: AreaTex is 160x560 and idImage::GenerateImage
// hard-errors on non-power-of-2 dimensions (vanilla mipmap/scaling assumptions).
// On GL3 they upload as raw NPOT GL textures (core GL 3.0+); on Vulkan they go
// through the RHI CreateTexture2D image path (qgl is NULL there, and VK has no
// power-of-2 restriction). Either way the handle rides DrawArgs::textures like the
// render-target images do — a raw GL name on GL3, an RHI ImageHandle on VK.
static rhi::ImageHandle rhiSmaaAreaTex = 0;
static rhi::ImageHandle rhiSmaaSearchTex = 0;

// target creation and the raw target-image binds in the SMAA draws bypass the
// idImage bind cache in backEnd.glState.tmu; invalidate it so the engine
// re-issues its binds (same fix as RB_RHI_ForgetTexBinds in RhiWorld.cpp)
static void RB_RHI_AAForgetTexBinds() {
	for ( int i = 0; i < MAX_MULTITEXTURE_UNITS; i++ ) {
		backEnd.glState.tmu[i].current2DMap = -1;
		backEnd.glState.tmu[i].current3DMap = -1;
		backEnd.glState.tmu[i].currentCubeMap = -1;
	}
}

// uploads one LUT as an uncompressed, unmipped RGBA8 GL texture. Binds on the
// current active unit, so callers must RB_RHI_AAForgetTexBinds() afterwards.
static GLuint RB_RHI_SmaaUploadLut( const byte *src, int srcChannels, int w, int h, GLint filter ) {
	byte *pic = (byte *)R_StaticAlloc( w * h * 4 );
	for ( int i = 0; i < w * h; i++ ) {
		pic[i*4+0] = src[i*srcChannels+0];
		pic[i*4+1] = srcChannels > 1 ? src[i*srcChannels+1] : 0;
		pic[i*4+2] = 0;
		pic[i*4+3] = 255;
	}
	GLuint tex = 0;
	qglGenTextures( 1, &tex );
	qglBindTexture( GL_TEXTURE_2D, tex );
	qglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pic );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	qglBindTexture( GL_TEXTURE_2D, 0 );
	R_StaticFree( pic );
	return tex;
}

// Vulkan LUT upload: the same RG/R -> RGBA8 expansion as the GL path above, but
// through the RHI CreateTexture2D image path (qgl is NULL under VK). NPOT is fine
// (the p-o-2 restriction was idImage-only), and CreateTexture2D's blocking upload
// runs on its own uploadCb + fence, independent of the still-recording frame command
// buffer, so a first-use mid-frame upload is safe. Returns an RHI ImageHandle that
// rides DrawArgs::textures like the render-target images do.
static rhi::ImageHandle RB_RHI_SmaaUploadLutVk( rhi::RHI *r, const byte *src, int srcChannels, int w, int h, int filter ) {
	byte *pic = (byte *)R_StaticAlloc( w * h * 4 );
	for ( int i = 0; i < w * h; i++ ) {
		pic[i*4+0] = src[i*srcChannels+0];
		pic[i*4+1] = srcChannels > 1 ? src[i*srcChannels+1] : 0;
		pic[i*4+2] = 0;
		pic[i*4+3] = 255;
	}
	rhi::ImageHandle img = r->CreateTexture2D( w, h, pic, filter, TR_CLAMP, false );
	R_StaticFree( pic );
	return img;
}

// edges + weights targets (and the LDR scene copy when asked for), sized to the
// view; self-heals after a lost context the same way the SSR target does
static bool RB_RHI_EnsureSmaaTargets( rhi::RHI *r, int w, int h, bool needScene ) {
	const bool vkMode = ( rhi::GetActiveBackendType() == rhi::BT_VULKAN );
	if ( rhiSmaaEdgesRT && r->GetRenderTargetImage( rhiSmaaEdgesRT ) == 0 ) {
		rhiSmaaEdgesRT = rhiSmaaWeightsRT = rhiSmaaSceneRT = 0;	// lost context (vid_restart)
		rhiSmaaW = rhiSmaaH = 0;
		rhiSmaaAreaTex = rhiSmaaSearchTex = 0;	// the raw LUT names died with the context
	}
	if ( !rhiSmaaEdgesRT || rhiSmaaW != w || rhiSmaaH != h ) {
		if ( rhiSmaaEdgesRT )   { r->DestroyRenderTarget( rhiSmaaEdgesRT );   rhiSmaaEdgesRT = 0; }
		if ( rhiSmaaWeightsRT ) { r->DestroyRenderTarget( rhiSmaaWeightsRT ); rhiSmaaWeightsRT = 0; }
		if ( rhiSmaaSceneRT )   { r->DestroyRenderTarget( rhiSmaaSceneRT );   rhiSmaaSceneRT = 0; }
		rhiSmaaEdgesRT = r->CreateRenderTarget( rhi::IF_RGBA8, w, h );
		rhiSmaaWeightsRT = r->CreateRenderTarget( rhi::IF_RGBA8, w, h );
		RB_RHI_AAForgetTexBinds();		// create() disturbed unit 0's cached bind
		if ( !rhiSmaaEdgesRT || !rhiSmaaWeightsRT ) {
			rhiSmaaW = rhiSmaaH = 0;
			return false;
		}
		rhiSmaaW = w;
		rhiSmaaH = h;
	}
	if ( needScene && !rhiSmaaSceneRT ) {
		rhiSmaaSceneRT = r->CreateRenderTarget( rhi::IF_RGBA8, w, h );
		RB_RHI_AAForgetTexBinds();
		if ( !rhiSmaaSceneRT ) {
			return false;
		}
	}
	if ( !rhiSmaaAreaTex ) {
		// AreaTex is RG (two packed coverage areas), bilinear — SMAA
		// interpolates between sub-areas; SearchTex must be point-sampled
		if ( vkMode ) {
			rhiSmaaAreaTex = RB_RHI_SmaaUploadLutVk( r, areaTexBytes, 2, AREATEX_WIDTH, AREATEX_HEIGHT, TF_LINEAR );
			rhiSmaaSearchTex = RB_RHI_SmaaUploadLutVk( r, searchTexBytes, 1, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, TF_NEAREST );
		} else {
			rhiSmaaAreaTex = RB_RHI_SmaaUploadLut( areaTexBytes, 2, AREATEX_WIDTH, AREATEX_HEIGHT, GL_LINEAR );
			rhiSmaaSearchTex = RB_RHI_SmaaUploadLut( searchTexBytes, 1, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, GL_NEAREST );
			RB_RHI_AAForgetTexBinds();		// the uploads disturbed the active unit's cached bind
		}
	}
	return rhiSmaaAreaTex != 0 && rhiSmaaSearchTex != 0;
}

// one fullscreen NDC quad through a SMAA program; tex0..tex2 are GL texture
// names — render-target images or the raw LUTs (0 = leave the unit alone)
static void RB_RHI_SmaaDraw( rhi::RHI *r, rhi::ShaderHandle prog, const rhi::RenderParams &parms,
                             rhi::ImageHandle tex0, rhi::ImageHandle tex1, rhi::ImageHandle tex2 = 0 ) {
	idDrawVert quad[4];
	memset( quad, 0, sizeof( quad ) );
	quad[0].xyz.Set( -1.0f, -1.0f, 0.0f ); quad[0].st[0] = 0.0f; quad[0].st[1] = 0.0f;
	quad[1].xyz.Set(  1.0f, -1.0f, 0.0f ); quad[1].st[0] = 1.0f; quad[1].st[1] = 0.0f;
	quad[2].xyz.Set(  1.0f,  1.0f, 0.0f ); quad[2].st[0] = 1.0f; quad[2].st[1] = 1.0f;
	quad[3].xyz.Set( -1.0f,  1.0f, 0.0f ); quad[3].st[0] = 0.0f; quad[3].st[1] = 1.0f;
	glIndex_t idx[6] = { 0, 1, 2, 0, 2, 3 };

	rhi::BufferHandle vb, ib, ub;
	int vertOfs = r->AllocVertices( quad, sizeof( quad ), &vb );
	int idxOfs = r->AllocIndices( idx, sizeof( idx ), &ib );
	int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

	rhi::PipelineDesc pd;
	pd.stateBits = GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK;
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
	da.textures[0] = tex0;
	da.textures[1] = tex1;
	da.textures[2] = tex2;
	r->Draw( da );

	backEnd.pc.c_drawElements++;
}

/*
=============
RB_RHI_SmaaEndUnitState

After a multi-unit SMAA draw the GL active unit is left on 1/2, and idImage::Bind
binds on the *active* unit while recording under currenttmu — the next
CopyFramebuffer (film grain's snapshot) would land its bind on the wrong unit and
then skip the "already bound" rebind on unit 0, sampling a stale SMAA target instead
of _currentRender. Leave the chain on unit 0 with the bind cache invalidated so every
following bind re-issues cleanly. VK has no active-unit state (each Draw fully
specifies its texture set) and gl3ActiveTexture is a NULL qgl pointer there, so skip it.
=============
*/
static void RB_RHI_SmaaEndUnitState( rhi::RHI *r ) {
	if ( rhi::GetActiveBackendType() != rhi::BT_VULKAN ) {
		RB_RHI_AAForgetTexBinds();
		rhi::gl3ActiveTexture( GL_TEXTURE0 );
		backEnd.glState.currenttmu = 0;
	}
}

/*
=============
RB_RHI_SmaaEdgesWeights

The first two SMAA 1x passes over an exact-size scene image: luma edge detection ->
blending weights (AreaTex/SearchTex LUTs on units 1/2), leaving the weights in
rhiSmaaWeightsRT for a following neighborhood-blend pass (either RB_RHI_SmaaChain's
blend, or the fused hdrresolve_smaa resolve). Both edge/weight targets discard on
non-edge pixels, so both are cleared. The caller must have run RB_RHI_EnsureSmaaTargets.
Does NOT restore unit state — the caller's blend pass is also multi-unit, so the shared
RB_RHI_SmaaEndUnitState runs once after it. Returns false if a shader/image is missing.
=============
*/
static bool RB_RHI_SmaaEdgesWeights( rhi::RHI *r, rhi::ImageHandle sceneImg, int w, int h ) {
	rhi::ShaderHandle edgesProg = r->LoadShader( "smaa_edges" );
	rhi::ShaderHandle weightsProg = r->LoadShader( "smaa_weights" );
	if ( !edgesProg || !weightsProg || !sceneImg ) {
		return false;
	}

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	// SMAA_RT_METRICS
	parms.localParam0[0] = 1.0f / w;
	parms.localParam0[1] = 1.0f / h;
	parms.localParam0[2] = (float)w;
	parms.localParam0[3] = (float)h;

	rhi::ClearArgs clear;
	memset( &clear, 0, sizeof( clear ) );
	clear.color = true;

	// pass 1: luma edge detection
	r->BeginTargetPass( rhiSmaaEdgesRT, &clear );
	RB_RHI_SmaaDraw( r, edgesProg, parms, sceneImg, 0 );
	r->EndPass();

	// pass 2: blending weights (edge mask on 0, the two LUTs on 1/2)
	r->BeginTargetPass( rhiSmaaWeightsRT, &clear );
	RB_RHI_SmaaDraw( r, weightsProg, parms, r->GetRenderTargetImage( rhiSmaaEdgesRT ),
	                 rhiSmaaAreaTex, rhiSmaaSearchTex );
	r->EndPass();
	return true;
}

/*
=============
RB_RHI_SmaaChain

The three SMAA 1x passes over an exact-size scene image: edges+weights (above) then
neighborhood blend. outputRT 0 writes the resolved image to the backbuffer (LDR path;
the caller already set viewport/scissor); otherwise into the given float target (HDR).
Returns false (leaving the frame untouched) if anything is missing. The fused HDR path
(RB_RHI_HdrResolveSmaaFused) reuses RB_RHI_SmaaEdgesWeights and does the blend itself.
=============
*/
static bool RB_RHI_SmaaChain( rhi::RHI *r, rhi::ImageHandle sceneImg, rhi::RenderTargetHandle outputRT, int w, int h ) {
	rhi::ShaderHandle blendProg = r->LoadShader( "smaa_blend" );
	if ( !blendProg || !sceneImg ) {
		return false;
	}
	if ( !RB_RHI_SmaaEdgesWeights( r, sceneImg, w, h ) ) {
		return false;
	}

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	// SMAA_RT_METRICS (the blend samples the weights + scene at texel offsets)
	parms.localParam0[0] = 1.0f / w;
	parms.localParam0[1] = 1.0f / h;
	parms.localParam0[2] = (float)w;
	parms.localParam0[3] = (float)h;

	// pass 3: neighborhood blend -> backbuffer or the HDR AA target
	if ( outputRT ) {
		r->BeginTargetPass( outputRT, NULL );
		RB_RHI_SmaaDraw( r, blendProg, parms, sceneImg, r->GetRenderTargetImage( rhiSmaaWeightsRT ) );
		r->EndPass();
	} else {
		RB_RHI_SmaaDraw( r, blendProg, parms, sceneImg, r->GetRenderTargetImage( rhiSmaaWeightsRT ) );
	}

	RB_RHI_SmaaEndUnitState( r );
	return true;
}

/*
=============
RB_RHI_SmaaPassLDR

SMAA over the finished LDR view: snapshot the backbuffer (POT _currentRender),
de-POT it into an exact-size scene target so the SMAA texel math is clean,
then run the chain back onto the backbuffer. Same viewport/scissor handling
as the FXAA path below.
=============
*/
static bool RB_RHI_SmaaPassLDR( rhi::RHI *r, const viewDef_t *viewDef ) {
	rhi::ShaderHandle copyProg = r->LoadShader( "smaa_copy" );
	if ( !copyProg ) {
		return false;
	}

	int w = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	int h = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
	if ( !RB_RHI_EnsureSmaaTargets( r, w, h, true ) ) {
		return false;
	}

	RB_RHI_CopyCurrentRender( viewDef );

	int potW = globalImages->currentRenderImage->uploadWidth;
	int potH = globalImages->currentRenderImage->uploadHeight;

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	parms.screenCorrection[0] = potW > 0 ? (float)w / potW : 1.0f;
	parms.screenCorrection[1] = potH > 0 ? (float)h / potH : 1.0f;

	rhi::gl3ActiveTexture( GL_TEXTURE0 );
	backEnd.glState.currenttmu = 0;
	globalImages->currentRenderImage->Bind();

	r->BeginTargetPass( rhiSmaaSceneRT, NULL );
	RB_RHI_SmaaDraw( r, copyProg, parms, 0, 0 );
	r->EndPass();

	// the last surface may have left a cropped scissor; cover the whole view
	r->SetScissor( tr.viewportOffset[0] + viewDef->viewport.x1,
	               tr.viewportOffset[1] + viewDef->viewport.y1, w, h );
	backEnd.currentScissor = viewDef->scissor;

	return RB_RHI_SmaaChain( r, r->GetRenderTargetImage( rhiSmaaSceneRT ), 0, w, h );
}

/*
=============
RB_RHI_AAPass

DUDE post-resolve antialiasing (FXAA) over the finished 3D view, run before any
2D/GUI so the HUD/menus stay crisp (shaders/fxaa.*). Opt-in via r_rhiAA; separate
from the hardware MSAA in r_multiSamples (which only covers backbuffer geometry
edges) — FXAA also smooths the specular/normal-map shimmer, at a slight softening.
Runs before RB_RHI_PostProcess so grain/chroma sit on top of the resolved image.
Same _currentRender snapshot + fullscreen-quad path as RB_RHI_PostProcess.
=============
*/
static void RB_RHI_AAPass( rhi::RHI *r, const viewDef_t *viewDef ) {
	if ( r_rhiAA.GetInteger() <= 0 ) {
		return;		// off → exact passthrough, skip the copy+draw entirely
	}

	// SMAA 1x; if its shaders/targets are unavailable fall through to FXAA
	if ( r_rhiAA.GetInteger() == 2 && RB_RHI_SmaaPassLDR( r, viewDef ) ) {
		return;
	}

	rhi::ShaderHandle prog = r->LoadShader( "fxaa" );
	if ( !prog ) {
		return;
	}

	// snapshot the finished 3D view (after fog and post-process surfaces)
	RB_RHI_CopyCurrentRender( viewDef );

	int w = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	int h = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
	int potW = globalImages->currentRenderImage->uploadWidth;
	int potH = globalImages->currentRenderImage->uploadHeight;

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	// content extent in the oversized POT texture; fxaa.frag samples st * screenCorrection
	parms.screenCorrection[0] = potW > 0 ? (float)w / potW : 1.0f;
	parms.screenCorrection[1] = potH > 0 ? (float)h / potH : 1.0f;
	// one screen texel in that same uv space, for the neighbour taps
	parms.localParam1[0] = potW > 0 ? 1.0f / potW : 0.0f;
	parms.localParam1[1] = potH > 0 ? 1.0f / potH : 0.0f;
	// subpixel smoothing amount (r_fxaaStrength); 0 = edge-only FXAA
	parms.localParam0[0] = r_fxaaStrength.GetFloat();

	// fullscreen NDC quad (identity mvp), st 0..1 with GL bottom-left origin
	// matching the framebuffer copy
	idDrawVert quad[4];
	memset( quad, 0, sizeof( quad ) );
	quad[0].xyz.Set( -1.0f, -1.0f, 0.0f ); quad[0].st[0] = 0.0f; quad[0].st[1] = 0.0f;
	quad[1].xyz.Set(  1.0f, -1.0f, 0.0f ); quad[1].st[0] = 1.0f; quad[1].st[1] = 0.0f;
	quad[2].xyz.Set(  1.0f,  1.0f, 0.0f ); quad[2].st[0] = 1.0f; quad[2].st[1] = 1.0f;
	quad[3].xyz.Set( -1.0f,  1.0f, 0.0f ); quad[3].st[0] = 0.0f; quad[3].st[1] = 1.0f;
	glIndex_t idx[6] = { 0, 1, 2, 0, 2, 3 };

	rhi::BufferHandle vb, ib, ub;
	int vertOfs = r->AllocVertices( quad, sizeof( quad ), &vb );
	int idxOfs = r->AllocIndices( idx, sizeof( idx ), &ib );
	int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

	// the last surface may have left a cropped scissor; cover the whole view
	r->SetScissor( tr.viewportOffset[0] + viewDef->viewport.x1,
	               tr.viewportOffset[1] + viewDef->viewport.y1, w, h );
	backEnd.currentScissor = viewDef->scissor;

	rhi::gl3ActiveTexture( GL_TEXTURE0 );
	backEnd.glState.currenttmu = 0;
	globalImages->currentRenderImage->Bind();

	rhi::PipelineDesc pd;
	pd.stateBits = GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK;
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
	r->Draw( da );

	backEnd.pc.c_drawElements++;
}

/*
=============
RB_RHI_PostProcess

DUDE film grain + chromatic aberration in one fullscreen pass over
_currentRender, run at the end of the 3D view before any 2D/GUI so the HUD is
never touched (shaders/postprocess.*). Both effects default off; strength 0
is an exact passthrough, so this is skipped unless a toggle is on.
=============
*/
static void RB_RHI_PostProcess( rhi::RHI *r, const viewDef_t *viewDef ) {
	float grain = r_postFilmGrain.GetFloat();
	float chroma = r_postChromaticAberration.GetFloat();
	if ( grain <= 0.0f && chroma <= 0.0f ) {
		return;
	}

	rhi::ShaderHandle prog = r->LoadShader( "postprocess" );
	if ( !prog ) {
		return;
	}

	// snapshot the finished 3D view (after fog and post-process surfaces)
	RB_RHI_CopyCurrentRender( viewDef );

	int w = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	int h = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
	int potW = globalImages->currentRenderImage->uploadWidth;
	int potH = globalImages->currentRenderImage->uploadHeight;

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	parms.screenCorrection[0] = potW > 0 ? (float)w / potW : 1.0f;
	parms.screenCorrection[1] = potH > 0 ? (float)h / potH : 1.0f;
	parms.windowCoord[2] = 0.5f;		// aberration center in uv
	parms.windowCoord[3] = 0.5f;
	parms.localParam0[0] = grain;
	parms.localParam0[1] = viewDef->floatTime;	// animated grain seed
	parms.localParam0[2] = chroma;
	parms.localParam1[0] = r_postFilmGrainSize.GetFloat();

	// fullscreen NDC quad (identity mvp), st 0..1 with GL bottom-left origin
	// matching the framebuffer copy
	idDrawVert quad[4];
	memset( quad, 0, sizeof( quad ) );
	quad[0].xyz.Set( -1.0f, -1.0f, 0.0f ); quad[0].st[0] = 0.0f; quad[0].st[1] = 0.0f;
	quad[1].xyz.Set(  1.0f, -1.0f, 0.0f ); quad[1].st[0] = 1.0f; quad[1].st[1] = 0.0f;
	quad[2].xyz.Set(  1.0f,  1.0f, 0.0f ); quad[2].st[0] = 1.0f; quad[2].st[1] = 1.0f;
	quad[3].xyz.Set( -1.0f,  1.0f, 0.0f ); quad[3].st[0] = 0.0f; quad[3].st[1] = 1.0f;
	glIndex_t idx[6] = { 0, 1, 2, 0, 2, 3 };

	rhi::BufferHandle vb, ib, ub;
	int vertOfs = r->AllocVertices( quad, sizeof( quad ), &vb );
	int idxOfs = r->AllocIndices( idx, sizeof( idx ), &ib );
	int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

	// the last surface may have left a cropped scissor; cover the whole view
	r->SetScissor( tr.viewportOffset[0] + viewDef->viewport.x1,
	               tr.viewportOffset[1] + viewDef->viewport.y1, w, h );
	backEnd.currentScissor = viewDef->scissor;

	rhi::gl3ActiveTexture( GL_TEXTURE0 );
	backEnd.glState.currenttmu = 0;
	globalImages->currentRenderImage->Bind();

	rhi::PipelineDesc pd;
	pd.stateBits = GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK;
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
	r->Draw( da );

	backEnd.pc.c_drawElements++;
}

/*
=============
RB_RHI_GammaBrightness

Apply r_gamma / r_brightness to the finished frame as one fullscreen pass over
the whole default framebuffer, right before the buffers are swapped. This is the
core-context equivalent of the legacy r_gammaInShader path: the fixed-function/
ARB fragment programs (draw_common.cpp) are never run here, and SDL3 offers no
hardware gamma ramp, so without this the gamma/brightness settings would do
nothing on the opengl3 renderer.

Runs after 3D + 2D/GUI/console (so the HUD and menus are corrected too, matching
hardware gamma) but before the ImGui settings overlay, so the F10 menu itself
stays at a stable, readable brightness while you drag the sliders. Skipped when
r_gammaInShader is off (hardware path) or when both values are identity, which
makes it a zero-cost no-op at the default settings.
=============
*/
static void RB_RHI_GammaBrightness( rhi::RHI *r ) {
	if ( !r_gammaInShader.GetBool() ) {
		return;		// hardware-gamma path (GLimp_SetGamma) handles it
	}
	// An HDR tonemap curve is this frame's display transform; applying the user's
	// r_gamma/r_brightness on top of the tonemapped image skews it (same reason the VK
	// resolve skips its gamma fold while tonemapping), so skip this pass then.
	// (rbHdrFrameActive is declared below this function; use its header accessor.)
	if ( RB_RHI_HdrFrameActive() && r_hdrTonemap.GetInteger() >= 1 ) {
		return;
	}

	float gamma = r_gamma.GetFloat();
	float brightness = r_brightness.GetFloat();
	if ( gamma == 1.0f && brightness == 1.0f ) {
		return;		// identity → exact passthrough, skip the copy+draw entirely
	}

	rhi::ShaderHandle prog = r->LoadShader( "gammabrightness" );
	if ( !prog ) {
		return;
	}

	int w = glConfig.vidWidth;
	int h = glConfig.vidHeight;

	// snapshot the finished frame (3D + 2D/GUI/console) into the oversized POT
	// _currentRender texture, then correct it back onto the framebuffer
	globalImages->currentRenderImage->CopyFramebuffer( 0, 0, w, h, true );

	int potW = globalImages->currentRenderImage->uploadWidth;
	int potH = globalImages->currentRenderImage->uploadHeight;

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	parms.screenCorrection[0] = potW > 0 ? (float)w / potW : 1.0f;
	parms.screenCorrection[1] = potH > 0 ? (float)h / potH : 1.0f;
	// mirror R_SetColorMappings: multiply by brightness (clamped), then pow(1/gamma)
	parms.localParam0[0] = brightness;
	parms.localParam0[1] = 1.0f / gamma;

	// fullscreen NDC quad (identity mvp), st 0..1 with GL bottom-left origin
	// matching the framebuffer copy
	idDrawVert quad[4];
	memset( quad, 0, sizeof( quad ) );
	quad[0].xyz.Set( -1.0f, -1.0f, 0.0f ); quad[0].st[0] = 0.0f; quad[0].st[1] = 0.0f;
	quad[1].xyz.Set(  1.0f, -1.0f, 0.0f ); quad[1].st[0] = 1.0f; quad[1].st[1] = 0.0f;
	quad[2].xyz.Set(  1.0f,  1.0f, 0.0f ); quad[2].st[0] = 1.0f; quad[2].st[1] = 1.0f;
	quad[3].xyz.Set( -1.0f,  1.0f, 0.0f ); quad[3].st[0] = 0.0f; quad[3].st[1] = 1.0f;
	glIndex_t idx[6] = { 0, 1, 2, 0, 2, 3 };

	rhi::BufferHandle vb, ib, ub;
	int vertOfs = r->AllocVertices( quad, sizeof( quad ), &vb );
	int idxOfs = r->AllocIndices( idx, sizeof( idx ), &ib );
	int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

	// the last view left its own viewport/scissor; cover the whole framebuffer
	r->SetViewport( 0, 0, w, h );
	r->SetScissor( 0, 0, w, h );
	backEnd.currentScissor.x1 = 0;
	backEnd.currentScissor.y1 = 0;
	backEnd.currentScissor.x2 = w - 1;
	backEnd.currentScissor.y2 = h - 1;

	rhi::gl3ActiveTexture( GL_TEXTURE0 );
	backEnd.glState.currenttmu = 0;
	globalImages->currentRenderImage->Bind();

	rhi::PipelineDesc pd;
	pd.stateBits = GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK;
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
	r->Draw( da );

	backEnd.pc.c_drawElements++;
}

/*
=============
RB_RHI_HdrBeginFrame / RB_RHI_HdrResolve

HDR render pipeline (r_hdr, docs/hdr-pipeline.md). When enabled, the whole frame
accumulates into a screen-sized RGBA16F scene buffer (with a depth-stencil attachment
for stencil shadows) instead of the 8-bit backbuffer, removing fog/gradient banding.
HdrBeginFrame routes rendering there right after BeginFrame; HdrResolve copies it back
onto the backbuffer at swap time, just before the gamma pass. Phase A is a straight
passthrough resolve (shaders/hdrresolve.*); Phase B folds exposure + tonemap into it.
=============
*/
static rhi::RenderTargetHandle	rhiHdrRT = 0;		// scene buffer (RGBA16F HDR, or RGBA8 for the VK off-HDR post pass) + depth-stencil
static rhi::RenderTargetHandle	rhiHdrAaRT = 0;		// FXAA/SMAA output ping (RGBA16F in HDR, RGBA8 for the VK off-HDR post pass; color only)
static int						rhiHdrW = 0, rhiHdrH = 0;
static bool						rbHdrRtFloat = false;			// rhiHdrRT is RGBA16F (HDR) vs RGBA8 (off-HDR post) — recreate on change
static bool						rbHdrAaFloat = false;			// rhiHdrAaRT is RGBA16F vs RGBA8 — must track rhiHdrRT's format
static bool						rbHdrActiveThisFrame = false;	// scene post-target bound *right now* (view pass)
static bool						rbHdrFrameActive = false;		// this whole frame is a true float-HDR frame
static bool						rbBerserkFrame = false;			// berserk material seen this frame → radial-blur the _scratch blit
static bool						rbHelltimeFrame = false;		// bloodorbN hell-time material seen this frame
static int						rbHelltimeLevel = 0;			// 0/1/2 = HELLTIME/BERSERK/INVULNERABILITY (from the bloodorbN name)

/*
=============
RB_RHI_HdrCaptureActive / RB_RHI_HdrFrameActive

Two separate signals the shared idImage::CopyFramebuffer needs to snapshot
_currentRender correctly in HDR mode — kept apart so the capture format never flips
mid-frame (a format flip forces a full-screen texture realloc; flipping it every
frame between the view's float capture and the post-resolve gamma pass' 8-bit capture
was a ~18ms/frame stall):

  HdrCaptureActive - the float scene FBO is the bound *read source* right now (view
    pass, between HdrBeginFrame and HdrResolve). Selects the read buffer:
    GL_COLOR_ATTACHMENT0 while true, GL_BACK once the backbuffer is rebound. Cleared
    at resolve. False on the legacy backend / when target creation failed, where the
    backbuffer is bound and GL_COLOR_ATTACHMENT0 would be invalid.

  HdrFrameActive - this is an HDR frame, true for its *entire* duration including the
    post-resolve gamma/screenshot passes. Selects the capture *format* (RGBA16F), so
    _currentRender stays one format all frame and doesn't thrash-realloc. The gamma
    pass captures the 8-bit backbuffer into that RGBA16F texture (a harmless upconvert)
    rather than forcing it back to RGBA8.
=============
*/
bool RB_RHI_HdrCaptureActive( void ) {
	return rbHdrActiveThisFrame;
}

bool RB_RHI_HdrFrameActive( void ) {
	return rbHdrFrameActive;
}

/*
=============
RB_RHI_FrameHasWorldScene

True if this frame renders a real fullscreen 3D world view (gameplay, or a level
frozen behind the in-game menu). HDR exists to de-band that scene's lighting/fog
gradients; a worldless frame — the main menu, the load/save GUI, cinematics — has
none of that, and its only 3D content (the cropped menu-planet renderDef) is a
subview and not fullscreen. Routing such a frame through the float buffer + dither
resolve lifts near-black detail (the menu planet's dark limb) into visibility
through the translucent menu buttons, so we keep those frames on the vanilla 8-bit
path. Same fullscreen-and-not-subview test RB_RHI_DrawView uses for the post chain.
=============
*/
static bool RB_RHI_FrameHasWorldScene( const emptyCommand_t *cmds ) {
	for ( ; cmds; cmds = (const emptyCommand_t *)cmds->next ) {
		if ( cmds->commandId != RC_DRAW_VIEW ) {
			continue;
		}
		const viewDef_t *vd = ( (const drawSurfsCommand_t *)cmds )->viewDef;
		if ( !vd || !vd->viewEntitys || vd->isSubview ) {
			continue;
		}
		if ( vd->viewport.x1 <= 0 && vd->viewport.y1 <= 0
		     && vd->viewport.x2 >= glConfig.vidWidth - 1
		     && vd->viewport.y2 >= glConfig.vidHeight - 1 ) {
			return true;
		}
	}
	return false;
}

static void RB_RHI_HdrBeginFrame( rhi::RHI *r, const emptyCommand_t *cmds ) {
	rbHdrActiveThisFrame = false;
	rbHdrFrameActive = false;

	// Baking a glass reflection probe (bakeGlassProbe, tr.takingEnvProbe): the six
	// 90-degree faces must be the clean scene, so they stay on the straight-to-
	// sceneColor path with no offscreen post target. Two reasons this matters:
	//   1. Fidelity — film grain / chromatic aberration / tonemapping belong on the
	//      final frame, not baked into a texture the live frame then samples *and*
	//      re-posts; the probe would double up (the buggy captures literally showed
	//      the HUD + aberration fringe of the frame behind the bake).
	//   2. Correctness — the probe renders at the tile size (r_ssrGlassProbeSize,
	//      e.g. 256) while the Vulkan scene image stays the swapchain size, so the
	//      offscreen target is sized to the tile but every GL->VK y-flip still uses
	//      sceneExtent (the window height). The scene then resolves/reads back out of
	//      register (the validation copy/blit/clear-attachment errors) and the probe
	//      face captured stale full-window content instead. The no-target path keeps
	//      all coordinates on one height (sceneExtent == sceneColor), like GL.
	if ( tr.takingEnvProbe ) {
		return;
	}

	const bool wantHdr = r_hdr.GetBool() && R_BackendSupportsEnhancements();

	// Off-HDR post (Vulkan only): the GL backend runs film grain / chromatic
	// aberration per 3D view and gamma as a swap-time pass over the backbuffer, but
	// on Vulkan the scene lives in an offscreen image that isn't sampleable in place,
	// so those effects need the same route-the-scene-into-a-target-then-resolve flow
	// the HDR path uses — just at RGBA8 instead of RGBA16F. FXAA/SMAA ride the same
	// RGBA8 ping in this mode (see the AA scratch below). Engage it when any of grain /
	// chroma / in-shader gamma / AA is active so the resolve has somewhere to apply them.
	// When HDR is on, the float path already covers all of these.
	const bool vkMode = ( rhi::GetActiveBackendType() == rhi::BT_VULKAN );
	const bool gammaWanted = r_gammaInShader.GetBool()
		&& ( r_gamma.GetFloat() != 1.0f || r_brightness.GetFloat() != 1.0f );
	const bool ldrPostWanted = vkMode && !wantHdr
		&& ( r_postFilmGrain.GetFloat() > 0.0f || r_postChromaticAberration.GetFloat() > 0.0f
		     || gammaWanted || r_rhiAA.GetInteger() > 0 );

	if ( !wantHdr && !ldrPostWanted ) {
		return;		// off → the frame stays on the backbuffer exactly as before
	}

	// worldless frames (main menu, GUIs, cinematics) stay on the 8-bit path so the
	// float+dither resolve can't reveal near-black scene detail through translucent
	// 2D — e.g. the menu planet's dark limb showing through the LOAD GAME button
	if ( !RB_RHI_FrameHasWorldScene( cmds ) ) {
		return;
	}

	int w = glConfig.vidWidth;
	int h = glConfig.vidHeight;

	// (re)create on resolution change, a format change (HDR toggled on/off — the
	// off-HDR post target is RGBA8, the HDR one RGBA16F), or a lost context (vid_restart
	// wipes the backend's targets, so a stale handle reports a null image) — same idiom
	// as the SSAO targets in RhiWorld
	if ( rhiHdrRT && ( rhiHdrW != w || rhiHdrH != h || rbHdrRtFloat != wantHdr
	                   || r->GetRenderTargetImage( rhiHdrRT ) == 0 ) ) {
		r->DestroyRenderTarget( rhiHdrRT );
		rhiHdrRT = 0;
	}
	if ( !rhiHdrRT ) {
		rhiHdrRT = r->CreateRenderTargetColorDepthStencil( wantHdr ? rhi::IF_RGBA16F : rhi::IF_RGBA8, w, h );
		rhiHdrW = w;
		rhiHdrH = h;
		rbHdrRtFloat = wantHdr;
	}
	if ( !rhiHdrRT ) {
		return;		// creation failed (no RGBA16F support?) → fall back to the backbuffer
	}

	// AA scratch: a ping buffer so FXAA/SMAA (r_rhiAA) run scene->scene before the
	// resolve, keeping the anti-aliased image in the scene buffer's own format instead
	// of round-tripping the 8-bit _currentRender. It matches rhiHdrRT's format — RGBA16F
	// in HDR, RGBA8 in the VK off-HDR post pass — so it's recreated when that format flips.
	// Freed when AA turns off, on resize, on a format switch, or on context loss.
	const bool wantAa = ( wantHdr || ldrPostWanted ) && ( r_rhiAA.GetInteger() > 0 );
	if ( rhiHdrAaRT && ( !wantAa || rhiHdrW != w || rhiHdrH != h || rbHdrAaFloat != wantHdr
	                     || r->GetRenderTargetImage( rhiHdrAaRT ) == 0 ) ) {
		r->DestroyRenderTarget( rhiHdrAaRT );
		rhiHdrAaRT = 0;
	}
	if ( wantAa && !rhiHdrAaRT ) {
		rhiHdrAaRT = r->CreateRenderTarget( wantHdr ? rhi::IF_RGBA16F : rhi::IF_RGBA8, w, h );
		rbHdrAaFloat = wantHdr;
	}

	r->SetFrameTarget( rhiHdrRT );
	rbHdrActiveThisFrame = true;
	// only a true float-HDR frame forces _currentRender to RGBA16F; the off-HDR post
	// target is RGBA8, so captures (glass refraction) keep the default 8-bit format
	rbHdrFrameActive = wantHdr;	// stays true past HdrResolve so _currentRender keeps one format all frame
}

// FXAA as a float->float pass (rhiHdrRT color -> rhiHdrAaRT), so the anti-aliased image
// stays in HDR and the resolve's chroma re-samples the AA'd result. Reuses fxaa.frag as-is:
// it samples whatever is bound to unit 0, with screenCorrection/texel set for an exact-size
// (non-POT) source. No-op unless FXAA is on and its scratch buffer exists.
static void RB_RHI_HdrFxaa( rhi::RHI *r ) {
	rhi::ShaderHandle prog = r->LoadShader( "fxaa" );
	if ( !prog || !rhiHdrRT || !rhiHdrAaRT ) {
		return;
	}
	int w = glConfig.vidWidth;
	int h = glConfig.vidHeight;

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	// exact-size float source: content fills the whole [0,1], one texel = 1/dim
	parms.screenCorrection[0] = 1.0f;
	parms.screenCorrection[1] = 1.0f;
	parms.localParam1[0] = 1.0f / w;
	parms.localParam1[1] = 1.0f / h;
	parms.localParam0[0] = r_fxaaStrength.GetFloat();

	idDrawVert quad[4];
	memset( quad, 0, sizeof( quad ) );
	quad[0].xyz.Set( -1.0f, -1.0f, 0.0f ); quad[0].st[0] = 0.0f; quad[0].st[1] = 0.0f;
	quad[1].xyz.Set(  1.0f, -1.0f, 0.0f ); quad[1].st[0] = 1.0f; quad[1].st[1] = 0.0f;
	quad[2].xyz.Set(  1.0f,  1.0f, 0.0f ); quad[2].st[0] = 1.0f; quad[2].st[1] = 1.0f;
	quad[3].xyz.Set( -1.0f,  1.0f, 0.0f ); quad[3].st[0] = 0.0f; quad[3].st[1] = 1.0f;
	glIndex_t idx[6] = { 0, 1, 2, 0, 2, 3 };

	rhi::BufferHandle vb, ib, ub;
	int vertOfs = r->AllocVertices( quad, sizeof( quad ), &vb );
	int idxOfs = r->AllocIndices( idx, sizeof( idx ), &ib );
	int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

	r->BeginTargetPass( rhiHdrAaRT, NULL );		// bind the float ping, viewport = its size

	rhi::PipelineDesc pd;
	pd.stateBits = GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK;
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
	da.textures[0] = r->GetRenderTargetImage( rhiHdrRT );
	r->Draw( da );

	r->EndPass();		// back to the frame target (rhiHdrRT)
	backEnd.pc.c_drawElements++;
}

// SMAA as a float->float chain (rhiHdrRT color -> rhiHdrAaRT), the r_rhiAA 2
// counterpart of RB_RHI_HdrFxaa above. The float scene buffer is exact-size,
// so the chain samples it directly — no de-POT copy needed. Edge detection
// reads unclamped HDR luma, which merely over-detects on >1 highlights.
static bool RB_RHI_HdrSmaa( rhi::RHI *r ) {
	if ( !rhiHdrRT || !rhiHdrAaRT ) {
		return false;
	}
	int w = glConfig.vidWidth;
	int h = glConfig.vidHeight;
	if ( !RB_RHI_EnsureSmaaTargets( r, w, h, false ) ) {
		return false;
	}
	return RB_RHI_SmaaChain( r, r->GetRenderTargetImage( rhiHdrRT ), rhiHdrAaRT, w, h );
}

/*
=============
RB_RHI_HdrResolveSmaaFused

Fuses SMAA 1x's final neighborhood-blend pass INTO the HDR resolve: run edges+weights, then a
single hdrresolve_smaa pass reads the float scene + the weights, does the blend, and applies the
resolve's grain/gamma tail straight to the backbuffer. That drops the separate rhiHdrAaRT
round-trip the classic path needs (blend -> rhiHdrAaRT, then hdrresolve reads it back).

Eligible only when SMAA is the active AA (r_rhiAA 2) and chromatic aberration is OFF — chroma
samples the resolved image at radial offsets, which a single fused pass can't provide (it only has
the AA'd colour at the current fragment). Returns false to fall through to the classic path when
ineligible or when a shader/target is unavailable; true when it has fully resolved the frame.

For zero-weight pixels the neighborhood blend is a pass-through, so with chroma off the fused output
is bit-identical to the classic AA-pass + hdrresolve it replaces.
=============
*/
static bool RB_RHI_HdrResolveSmaaFused( rhi::RHI *r, int w, int h ) {
	if ( r_rhiAA.GetInteger() != 2 || r_postChromaticAberration.GetFloat() > 0.0f || !rhiHdrRT ) {
		return false;
	}
	rhi::ShaderHandle prog = r->LoadShader( "hdrresolve_smaa" );
	if ( !prog ) {
		return false;
	}
	if ( !RB_RHI_EnsureSmaaTargets( r, w, h, false ) ) {
		return false;
	}
	if ( !RB_RHI_SmaaEdgesWeights( r, r->GetRenderTargetImage( rhiHdrRT ), w, h ) ) {
		return false;
	}

	// final fused pass: neighborhood blend + grain + gamma, scene(unit 0) + weights(unit 1) -> backbuffer
	r->SetFrameTarget( 0 );
	r->SetViewport( 0, 0, w, h );
	r->SetScissor( 0, 0, w, h );
	backEnd.currentScissor.x1 = 0;
	backEnd.currentScissor.y1 = 0;
	backEnd.currentScissor.x2 = w - 1;
	backEnd.currentScissor.y2 = h - 1;

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	// SMAA_RT_METRICS (localParam0) — shared by the vertex offset and the blend
	parms.localParam0[0] = 1.0f / w;
	parms.localParam0[1] = 1.0f / h;
	parms.localParam0[2] = (float)w;
	parms.localParam0[3] = (float)h;
	// grain intensity + seed live in windowCoord.xy here (localParam0 is RT_METRICS, chroma is off);
	// grain size + gamma/brightness mirror RB_RHI_HdrResolve exactly (identity on GL, real on VK)
	parms.windowCoord[0] = r_postFilmGrain.GetFloat();
	parms.windowCoord[1] = (float)( Sys_Milliseconds() & 0xffff ) * 0.001f;
	parms.windowCoord[2] = r_hdrExposure.GetFloat();		// exposure (localParam0 is RT_METRICS)
	parms.localParam1[0] = r_postFilmGrainSize.GetFloat();
	parms.localParam1[1] = 1.0f;	// brightness (identity)
	parms.localParam1[2] = 1.0f;	// 1/gamma (identity)
	parms.localParam1[3] = (float)r_hdrTonemap.GetInteger();	// tonemap curve select
	// An active tonemap curve IS this frame's display transform; folding r_gamma/
	// r_brightness on top of it skews the calibrated curve, so leave gamma identity
	// while tonemapping (brightness is then the r_hdrExposure knob). Mode 0 / HDR-off
	// keep the gamma fold.
	if ( rhi::GetActiveBackendType() == rhi::BT_VULKAN && r_gammaInShader.GetBool()
	     && !( rbHdrFrameActive && r_hdrTonemap.GetInteger() >= 1 ) ) {
		parms.localParam1[1] = r_brightness.GetFloat();
		parms.localParam1[2] = ( r_gamma.GetFloat() > 0.0f ) ? 1.0f / r_gamma.GetFloat() : 1.0f;
	}

	idDrawVert quad[4];
	memset( quad, 0, sizeof( quad ) );
	quad[0].xyz.Set( -1.0f, -1.0f, 0.0f ); quad[0].st[0] = 0.0f; quad[0].st[1] = 0.0f;
	quad[1].xyz.Set(  1.0f, -1.0f, 0.0f ); quad[1].st[0] = 1.0f; quad[1].st[1] = 0.0f;
	quad[2].xyz.Set(  1.0f,  1.0f, 0.0f ); quad[2].st[0] = 1.0f; quad[2].st[1] = 1.0f;
	quad[3].xyz.Set( -1.0f,  1.0f, 0.0f ); quad[3].st[0] = 0.0f; quad[3].st[1] = 1.0f;
	glIndex_t idx[6] = { 0, 1, 2, 0, 2, 3 };

	rhi::BufferHandle vb, ib, ub;
	int vertOfs = r->AllocVertices( quad, sizeof( quad ), &vb );
	int idxOfs = r->AllocIndices( idx, sizeof( idx ), &ib );
	int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

	rhi::PipelineDesc pd;
	pd.stateBits = GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK;
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
	da.textures[0] = r->GetRenderTargetImage( rhiHdrRT );
	da.textures[1] = r->GetRenderTargetImage( rhiSmaaWeightsRT );
	r->Draw( da );
	backEnd.pc.c_drawElements++;

	RB_RHI_SmaaEndUnitState( r );		// the scene+weights draw left GL on unit 1
	return true;
}

static void RB_RHI_HdrResolve( rhi::RHI *r ) {
	if ( !rbHdrActiveThisFrame ) {
		return;
	}
	rbHdrActiveThisFrame = false;

	if ( !rhiHdrRT ) {
		r->SetFrameTarget( 0 );		// give up on HDR this frame, back to the backbuffer
		return;
	}

	int w = glConfig.vidWidth;
	int h = glConfig.vidHeight;

	// Fused SMAA resolve (chroma off): the neighborhood blend + grain/gamma tail in one pass,
	// dropping the rhiHdrAaRT round-trip. Falls through to the classic path when ineligible.
	if ( RB_RHI_HdrResolveSmaaFused( r, w, h ) ) {
		return;
	}

	rhi::ShaderHandle prog = r->LoadShader( "hdrresolve" );
	if ( !prog ) {
		r->SetFrameTarget( 0 );		// give up on HDR this frame, back to the backbuffer
		return;
	}

	// AA first (scene->scene into rhiHdrAaRT, in the scene buffer's own format — RGBA16F
	// in HDR, RGBA8 in the off-HDR post pass), so chroma below re-samples the anti-aliased
	// image; the resolve then reads the AA buffer instead of the raw scene buffer. Off → the
	// scratch buffer doesn't exist and we read the scene buffer directly. Mode 2 = SMAA,
	// falling back to FXAA if its shaders/targets are unavailable.
	rhi::RenderTargetHandle sourceRT = rhiHdrRT;
	if ( r_rhiAA.GetInteger() > 0 && rhiHdrAaRT ) {
		if ( r_rhiAA.GetInteger() != 2 || !RB_RHI_HdrSmaa( r ) ) {
			RB_RHI_HdrFxaa( r );
		}
		sourceRT = rhiHdrAaRT;
	}

	// back to the backbuffer, then blit the float scene buffer onto it
	r->SetFrameTarget( 0 );
	r->SetViewport( 0, 0, w, h );
	r->SetScissor( 0, 0, w, h );
	backEnd.currentScissor.x1 = 0;
	backEnd.currentScissor.y1 = 0;
	backEnd.currentScissor.x2 = w - 1;
	backEnd.currentScissor.y2 = h - 1;

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.mvpMatrix[0] = parms.mvpMatrix[5] = parms.mvpMatrix[10] = parms.mvpMatrix[15] = 1.0f;
	// film grain + chromatic aberration are folded into the resolve here (RB_RHI_PostProcess
	// is skipped in HDR mode) so they sample the smooth float buffer instead of the 8-bit
	// _currentRender round-trip that was re-banding the image
	parms.localParam0[0] = r_hdrExposure.GetFloat();		// exposure, applied before the tonemap curve
	parms.localParam0[1] = r_postFilmGrain.GetFloat();
	parms.localParam0[2] = (float)( Sys_Milliseconds() & 0xffff ) * 0.001f;	// animated grain seed
	parms.localParam0[3] = r_postChromaticAberration.GetFloat();
	parms.localParam1[0] = r_postFilmGrainSize.GetFloat();
	parms.localParam1[3] = (float)r_hdrTonemap.GetInteger();	// tonemap curve select
	parms.windowCoord[2] = 0.5f;	// aberration center in uv
	parms.windowCoord[3] = 0.5f;
	// gamma / brightness: folded into the resolve on Vulkan (the backend has no separate
	// LDR gamma tail — RB_RHI_GammaBrightness only runs on GL). GL passes identity here so
	// its standalone gammabrightness pass at swap stays the single point of correction.
	parms.localParam1[1] = 1.0f;	// brightness (identity)
	parms.localParam1[2] = 1.0f;	// 1/gamma (identity)
	// An active tonemap curve IS this frame's display transform; folding r_gamma/
	// r_brightness on top of it skews the calibrated curve, so leave gamma identity
	// while tonemapping (brightness is then the r_hdrExposure knob). Mode 0 / HDR-off
	// keep the gamma fold.
	if ( rhi::GetActiveBackendType() == rhi::BT_VULKAN && r_gammaInShader.GetBool()
	     && !( rbHdrFrameActive && r_hdrTonemap.GetInteger() >= 1 ) ) {
		parms.localParam1[1] = r_brightness.GetFloat();
		parms.localParam1[2] = ( r_gamma.GetFloat() > 0.0f ) ? 1.0f / r_gamma.GetFloat() : 1.0f;
	}

	// fullscreen NDC quad, st 0..1 (the HDR target is exact screen size, so no NPOT correction)
	idDrawVert quad[4];
	memset( quad, 0, sizeof( quad ) );
	quad[0].xyz.Set( -1.0f, -1.0f, 0.0f ); quad[0].st[0] = 0.0f; quad[0].st[1] = 0.0f;
	quad[1].xyz.Set(  1.0f, -1.0f, 0.0f ); quad[1].st[0] = 1.0f; quad[1].st[1] = 0.0f;
	quad[2].xyz.Set(  1.0f,  1.0f, 0.0f ); quad[2].st[0] = 1.0f; quad[2].st[1] = 1.0f;
	quad[3].xyz.Set( -1.0f,  1.0f, 0.0f ); quad[3].st[0] = 0.0f; quad[3].st[1] = 1.0f;
	glIndex_t idx[6] = { 0, 1, 2, 0, 2, 3 };

	rhi::BufferHandle vb, ib, ub;
	int vertOfs = r->AllocVertices( quad, sizeof( quad ), &vb );
	int idxOfs = r->AllocIndices( idx, sizeof( idx ), &ib );
	int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

	rhi::PipelineDesc pd;
	pd.stateBits = GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK;
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
	da.textures[0] = r->GetRenderTargetImage( sourceRT );
	r->Draw( da );

	backEnd.pc.c_drawElements++;
}

/*
========================
RB_RHI_Shutdown

Strong teardown of the GL 3.3 core backend, called from the renderer while the GL
context is still current (vid_restart, before GLimp_Shutdown; and full renderer
shutdown). Switching backends via the Video-Options selector runs a vid_restart, and
without this the GL3 backend left residue: GL3Backend::Shutdown() was never called, so
its render-target table kept dead FBO/texture names from the destroyed context while the
cached client handles (HDR scene buffer, SSAO, SSR, shadow maps) still pointed at them.
A same-resolution switch then reused a dead framebuffer and the frame came up white or
garbled — intermittent because a differing resolution happened to force recreation.

Order matters: free the shadow caches and forget all client-side handles first (plain
CPU-side resets, no GL), then GL3Backend::Shutdown() deletes every GPU object in one
sweep over its table and marks itself uninitialised. Safe to call when the GL3 backend
was never brought up (legacy session): the cache free and GL3Backend::Shutdown() both
self-guard, and zeroing already-zero handles is a no-op.
========================
*/
void RB_RHI_Shutdown( void ) {
	RB_RHI_FreeShadowCubeCache();		// cube + 2D shadow caches, budget hysteresis (self-guards on context)
	RB_RHI_ResetWorldTargets();			// shadow map/pool + SSAO + SSR + normal handles

	// HDR scene buffer + FXAA float ping (this file's statics)
	rhiHdrRT = 0;
	rhiHdrAaRT = 0;
	rhiHdrW = rhiHdrH = 0;

	// SMAA edge/weight/scene targets + LUTs (this file's statics). On GL3 the LUTs
	// are raw GL names GL3Backend::Shutdown() doesn't track, so delete them here
	// (context still current); on VK they're RHI images the backend's own table sweep
	// below frees, so just forget the handles. They re-upload lazily next context.
	rhiSmaaEdgesRT = rhiSmaaWeightsRT = rhiSmaaSceneRT = 0;
	rhiSmaaW = rhiSmaaH = 0;
	if ( rhi::GetActiveBackendType() != rhi::BT_VULKAN ) {
		if ( rhiSmaaAreaTex )   { qglDeleteTextures( 1, &rhiSmaaAreaTex ); }
		if ( rhiSmaaSearchTex ) { qglDeleteTextures( 1, &rhiSmaaSearchTex ); }
	}
	rhiSmaaAreaTex = rhiSmaaSearchTex = 0;

	// deletes rings, VAOs, shader cache and every render target, then clears the
	// backend's table so any stale handle now resolves to a null image
	rhi::GetRHI()->Shutdown();
}

void RB_RHI_LogOnce( const char *what ) {
	static idStr logged;
	if ( logged.Find( what ) < 0 ) {
		logged += what;
		logged += ";";
		common->Printf( "RHI backend: %s (not rendered yet)\n", what );
	}
}

/*
=============
RB_RHI_SpaceMvp

MVP for a model space. The weapon/model depth hacks tweak the projection
matrix; the depth range half of those hacks stays in RB_Enter/LeaveDepthHack
(core-safe — they only call glDepthRange there).
=============
*/
void RB_RHI_SpaceMvp( const viewDef_s *viewDef, const viewEntity_t *space, float mvp[16] ) {
	if ( space->weaponDepthHack || space->modelDepthHack != 0.0f ) {
		float proj[16];
		memcpy( proj, viewDef->projectionMatrix, sizeof( proj ) );
		if ( space->weaponDepthHack ) {
			proj[14] *= 0.25f;
		} else {
			proj[14] -= space->modelDepthHack;
		}
		myGlMultMatrix( space->modelViewMatrix, proj, mvp );
	} else {
		myGlMultMatrix( space->modelViewMatrix, viewDef->projectionMatrix, mvp );
	}

	// Vulkan clip conventions (docs/vulkan-backend.md, decided 2026-08-02):
	// depth range remap z' = 0.5*(z + w) turns GL's -1..1 clip z into Vulkan's
	// 0..1 at the single point every RHI MVP flows through; the Y flip is the
	// backend's negative-height viewport, so the matrices stay comparable
	// with GL3 captures. Column-major GL layout: row 2 = m[2,6,10,14],
	// row 3 = m[3,7,11,15].
	if ( rhi::GetActiveBackendType() == rhi::BT_VULKAN ) {
		for ( int c = 0; c < 4; c++ ) {
			mvp[c * 4 + 2] = 0.5f * ( mvp[c * 4 + 2] + mvp[c * 4 + 3] );
		}
	}
}

/*
=============
RB_RHI_CullFor
=============
*/
int RB_RHI_CullFor( const viewDef_s *viewDef, int cullType ) {
	if ( viewDef->isMirror ) {
		if ( cullType == CT_FRONT_SIDED ) {
			return CT_BACK_SIDED;
		}
		if ( cullType == CT_BACK_SIDED ) {
			return CT_FRONT_SIDED;
		}
	}
	return cullType;
}

/*
=============
per-frame surface streaming with dedup

A surface referenced by several passes (depth fill, per-light interactions,
shader passes) is streamed into the rings once per frame. Entries are keyed
by the vertex-cache block and invalidated whenever a ring orphans its
storage (StreamGeneration) — offsets into orphaned storage must not be
reused for new draws.
=============
*/
struct streamedGeo_t {
	const void *		vertKey;	// vertex cache block
	const void *		idxKey;		// index array — NOT redundant: per-light
									// turbo shadow volumes share one doubled
									// vertex cache but have their own indexes
	rhi::BufferHandle	vb, ib;
	int					vertOfs, idxOfs;
};

static idList<streamedGeo_t>	rbStreamed;
static idHashIndex				rbStreamedHash;
static int						rbStreamedGen = -1;

static bool RB_RHI_StreamLookup( rhi::RHI *r, const void *vertKey, const void *idxKey, rhi::BufferHandle &vb, int &vertOfs, rhi::BufferHandle &ib, int &idxOfs ) {
	if ( r->StreamGeneration() != rbStreamedGen ) {
		rbStreamedGen = r->StreamGeneration();
		rbStreamed.Clear();
		rbStreamedHash.Free();
	}
	int hashKey = (int)( ( ( (uintptr_t)vertKey ^ (uintptr_t)idxKey ) >> 4 ) & 0x7fffffff );
	for ( int i = rbStreamedHash.First( hashKey ); i != -1; i = rbStreamedHash.Next( i ) ) {
		if ( rbStreamed[i].vertKey == vertKey && rbStreamed[i].idxKey == idxKey ) {
			vb = rbStreamed[i].vb;
			vertOfs = rbStreamed[i].vertOfs;
			ib = rbStreamed[i].ib;
			idxOfs = rbStreamed[i].idxOfs;
			return true;
		}
	}
	return false;
}

static void RB_RHI_StreamStore( rhi::RHI *r, const void *vertKey, const void *idxKey, rhi::BufferHandle vb, int vertOfs, rhi::BufferHandle ib, int idxOfs ) {
	// only cache if no ring wrapped during the writes — otherwise earlier
	// offsets may point into orphaned storage
	if ( r->StreamGeneration() != rbStreamedGen ) {
		return;
	}
	streamedGeo_t e;
	e.vertKey = vertKey;
	e.idxKey = idxKey;
	e.vb = vb;
	e.vertOfs = vertOfs;
	e.ib = ib;
	e.idxOfs = idxOfs;
	int index = rbStreamed.Append( e );
	rbStreamedHash.Add( (int)( ( ( (uintptr_t)vertKey ^ (uintptr_t)idxKey ) >> 4 ) & 0x7fffffff ), index );
}

// ---- Phase 2 GPU MD5 skinning (docs/gpu-offload-plan.md) ------------------------------------
// The front end (idMD5Mesh::UpdateSurface) records one job per visible skinned surface; the
// backend flushes them as compute dispatches in the pre-scene window, before the first pass
// opens (so the compute-write -> vertex-read barrier lands before any draw reads the buffer).
struct rbSkinJob_t {
	rhi::ShaderHandle	shader;
	rhi::BufferHandle	outVB, weights, wdesc, wstart, localTbn;
	const void *		jointData;			// R_FrameAlloc snapshot, valid this frame
	int					numJoints;
	int					numOutVerts;
	float				skinScale;
};
static idList<rbSkinJob_t>	rbSkinJobs;

void RB_RHI_AddSkinJob( unsigned int shader, unsigned int outVB, int numOutVerts,
                        unsigned int weightsBuf, unsigned int wdescBuf, unsigned int wstartBuf, unsigned int localTbnBuf,
                        const void *jointData, int numJoints, float skinScale ) {
	rbSkinJob_t j;
	j.shader = shader; j.outVB = outVB; j.numOutVerts = numOutVerts;
	j.weights = weightsBuf; j.wdesc = wdescBuf; j.wstart = wstartBuf; j.localTbn = localTbnBuf;
	j.jointData = jointData; j.numJoints = numJoints; j.skinScale = skinScale;
	rbSkinJobs.Append( j );
}

void RB_RHI_FlushSkinJobs( void ) {
	if ( rbSkinJobs.Num() == 0 ) {
		return;
	}
	rhi::RHI *r = rhi::GetRHI();
	if ( r ) {
		for ( int i = 0; i < rbSkinJobs.Num(); i++ ) {
			const rbSkinJob_t &j = rbSkinJobs[i];
			// per-frame joint palette (small; a shared batched buffer is a later optimisation)
			rhi::BufferHandle jointsBuf = r->CreateBuffer( rhi::BU_STORAGE, j.numJoints * (int)sizeof( idJointMat ), j.jointData );
			if ( !jointsBuf ) {
				continue;
			}
			struct { unsigned int numVerts; float skinScale; } pc = { (unsigned int)j.numOutVerts, j.skinScale };
			rhi::ComputeArgs ca = {};
			ca.shader = j.shader;
			ca.storage[0] = jointsBuf;
			ca.storage[1] = j.weights;
			ca.storage[2] = j.wdesc;
			ca.storage[3] = j.wstart;
			ca.storage[4] = j.outVB;
			ca.storage[5] = j.localTbn;
			ca.pushConstants = &pc;
			ca.pushConstantSize = (int)sizeof( pc );
			ca.groupsX = ( j.numOutVerts + 63 ) / 64; ca.groupsY = 1; ca.groupsZ = 1;
			r->Dispatch( ca );					// records on the frame cb + a compute->vertex barrier
			r->DestroyBuffer( jointsBuf );		// deferred/fence-retired: safe right after recording

		}
	}
	rbSkinJobs.SetNum( 0 );
}

// Roadmap B: compute deform-once tessellation jobs (docs/tessellation.md). Same record-in-front-end /
// dispatch-in-backend pattern as the skin jobs, flushed immediately AFTER them so the skin-write ->
// deform-read dependency is covered by the skin dispatch's trailing COMPUTE->COMPUTE barrier.
struct rbTessJob_t {
	rhi::ShaderHandle	shader;
	rhi::BufferHandle	srcVB;			// GPU source (gpuSkinVB); 0 => upload srcCpu instead
	const void *		srcCpu;			// R_FrameAlloc'd source verts (used when srcVB == 0)
	int					numSrcVerts;
	rhi::BufferHandle	outVB, barySeam, srcTri, height;
	int					numOutVerts;
	float				dispStrength;
};
static idList<rbTessJob_t>	rbTessJobs;

void RB_RHI_AddTessJob( unsigned int shader, unsigned int srcVB, const void *srcCpu, int numSrcVerts,
                        unsigned int outVB, unsigned int barySeam, unsigned int srcTri, unsigned int height,
                        int numOutVerts, float dispStrength ) {
	rbTessJob_t j;
	j.shader = shader; j.srcVB = srcVB; j.srcCpu = srcCpu; j.numSrcVerts = numSrcVerts;
	j.outVB = outVB; j.barySeam = barySeam; j.srcTri = srcTri; j.height = height;
	j.numOutVerts = numOutVerts; j.dispStrength = dispStrength;
	rbTessJobs.Append( j );
}

void RB_RHI_FlushTessJobs( void ) {
	if ( rbTessJobs.Num() == 0 ) {
		return;
	}
	rhi::RHI *r = rhi::GetRHI();
	if ( r ) {
		for ( int i = 0; i < rbTessJobs.Num(); i++ ) {
			const rbTessJob_t &j = rbTessJobs[i];
			rhi::BufferHandle srcBuf = j.srcVB;
			bool ownSrc = false;
			if ( srcBuf == 0 && j.srcCpu ) {
				// CPU source (gpuSkinning off): upload this frame's deformed verts as a storage buffer
				srcBuf = r->CreateBuffer( rhi::BU_STORAGE, j.numSrcVerts * (int)sizeof( idDrawVert ), j.srcCpu );
				ownSrc = true;
			}
			if ( srcBuf == 0 ) {
				continue;
			}
			struct { unsigned int numVerts; float dispStrength; } pc = { (unsigned int)j.numOutVerts, j.dispStrength };
			rhi::ComputeArgs ca = {};
			ca.shader = j.shader;
			ca.storage[0] = srcBuf;
			ca.storage[1] = j.barySeam;
			ca.storage[2] = j.srcTri;
			ca.storage[3] = j.height;
			ca.storage[4] = j.outVB;
			ca.pushConstants = &pc;
			ca.pushConstantSize = (int)sizeof( pc );
			ca.groupsX = ( j.numOutVerts + 63 ) / 64; ca.groupsY = 1; ca.groupsZ = 1;
			r->Dispatch( ca );					// records on the frame cb + a compute->vertex/compute barrier
			if ( ownSrc ) {
				r->DestroyBuffer( srcBuf );		// deferred/fence-retired: safe right after recording
			}
		}
	}
	rbTessJobs.SetNum( 0 );
}

// Roadmap B: if this surface was deformed once this frame (Roadmap B) AND it is a classifier-approved
// tess candidate (the caller passes `tess`, from RB_RHI_TessellateSurf), rebind the draw to the
// pre-deformed expanded buffer + its expanded index count and disable fixed-function tess. Surfaces the
// classifier excludes (eyes/teeth/headgear/etc.) keep `tess` false here, so they NEVER draw the deformed
// buffer even though it may have been dispatched -- they render their base geometry, exactly as today.
void RB_RHI_ApplyDeform( const srfTriangles_s *tri, rhi::BufferHandle &vb, int &vertOfs,
                         rhi::BufferHandle &ib, int &idxOfs, int &idxCount, bool &tess ) {
	if ( tess && tri->tessDeformVB && tri->tessDeformIB ) {
		vb = tri->tessDeformVB; vertOfs = 0;
		ib = tri->tessDeformIB; idxOfs = 0;
		idxCount = tri->tessDeformIndexes;
		tess = false;						// deform-once supersedes the per-pass fixed-function tess
	}
}

/*
=============
RB_RHI_DeformSubStage

The custom-ARB / builtin-ARB / texgen sub-stage helpers below draw a single
material stage with no tesc/tese pipeline, so they cannot fixed-function
tessellate. But a mod's emissive stage (self-illum add, reflective cube, etc.)
on a deform-once BODY still has to track the SAME displaced silhouette the zfill
prepass + interactions drew from, or a DEPTHFUNC_EQUAL emissive fails EQUAL
against the deformed depth and drops out (and a cube/skybox texgen samples the
wrong surface). So redirect these draws to tri->tessDeformVB exactly like the
generic stage path and every other RHI pass. GPU skinning (gpuSkinVB) is already
handled upstream by RB_RHI_StreamAmbient, so this only adds the tess-deform case.
Returns the index count to draw (tessDeformIndexes when redirected, else numIndexes).
=============
*/
static int RB_RHI_DeformSubStage( const drawSurf_t *surf, rhi::BufferHandle &vb, int &vertOfs,
                                  rhi::BufferHandle &ib, int &idxOfs ) {
	const srfTriangles_t *tri = surf->geo;
	int idxCount = tri->numIndexes;
	bool tess = RB_RHI_TessellateSurf( surf, false );	// deform-once uses the non-blend classify
	RB_RHI_ApplyDeform( tri, vb, vertOfs, ib, idxOfs, idxCount, tess );
	return idxCount;
}

// Resolve this surface's indexes to a GPU buffer + byte offset. When the front
// end has a resident index VBO (tri->indexCache, populated only when
// r_useIndexBuffers is set — which the core profile forces on) draw it in
// place; otherwise stream the CPU index array into the ring for this frame.
static void RB_RHI_ResolveIndices( rhi::RHI *r, const srfTriangles_s *tri, rhi::BufferHandle &ib, int &idxOfs ) {
	if ( tri->indexCache && tri->indexCache->vbo ) {
		ib = tri->indexCache->vbo;
		idxOfs = (int)tri->indexCache->offset;
	} else {
		ib = 0;
		idxOfs = r->AllocIndices( tri->indexes, tri->numIndexes * (int)sizeof( glIndex_t ), &ib );
	}
}

void RB_RHI_StreamAmbient( rhi::RHI *r, const srfTriangles_s *tri, rhi::BufferHandle &vb, int &vertOfs, rhi::BufferHandle &ib, int &idxOfs ) {
	// Phase 2 GPU skinning: this surface (or the interaction copy that inherited it) was skinned
	// by the compute lane into a persistent vertex buffer — bind it directly. Indexes are static,
	// so they resolve the normal way. Preferred over ambientCache so every pass draws the GPU pose.
	if ( tri->gpuSkinVB ) {
		vb = tri->gpuSkinVB;
		vertOfs = 0;
		RB_RHI_ResolveIndices( r, tri, ib, idxOfs );
		return;
	}
	// static VBO fast path: the cache block already lives on the GPU (uploaded
	// once at level load, or by AllocFrameTemp for dynamic surfaces), so hand
	// back its handle and offset directly — no per-frame copy.
	if ( tri->ambientCache->vbo ) {
		vb = tri->ambientCache->vbo;
		vertOfs = (int)tri->ambientCache->offset;
		RB_RHI_ResolveIndices( r, tri, ib, idxOfs );
		return;
	}
	// system-memory fallback (ARB_vertex_buffer_object unavailable, or
	// r_useVertexBuffers 0): stream into the ring, deduped per frame
	if ( RB_RHI_StreamLookup( r, tri->ambientCache, tri->indexes, vb, vertOfs, ib, idxOfs ) ) {
		return;
	}
	// stream the whole cache block: several shadow/geometry paths put more
	// data in the cache than tri->numVerts suggests (e.g. the shared shadow
	// caches hold near+far vertex pairs), and the indexes address it
	const idDrawVert *ac = (idDrawVert *)vertexCache.Position( tri->ambientCache );
	vertOfs = r->AllocVertices( ac, tri->ambientCache->size, &vb );
	idxOfs = r->AllocIndices( tri->indexes, tri->numIndexes * (int)sizeof( glIndex_t ), &ib );
	RB_RHI_StreamStore( r, tri->ambientCache, tri->indexes, vb, vertOfs, ib, idxOfs );
}

void RB_RHI_StreamShadow( rhi::RHI *r, const srfTriangles_s *tri, rhi::BufferHandle &vb, int &vertOfs, rhi::BufferHandle &ib, int &idxOfs ) {
	// static VBO fast path (see RB_RHI_StreamAmbient)
	if ( tri->shadowCache->vbo ) {
		vb = tri->shadowCache->vbo;
		vertOfs = (int)tri->shadowCache->offset;
		RB_RHI_ResolveIndices( r, tri, ib, idxOfs );
		return;
	}
	if ( RB_RHI_StreamLookup( r, tri->shadowCache, tri->indexes, vb, vertOfs, ib, idxOfs ) ) {
		return;
	}
	const shadowCache_t *sc = (shadowCache_t *)vertexCache.Position( tri->shadowCache );
	vertOfs = r->AllocVertices( sc, tri->shadowCache->size, &vb );
	idxOfs = r->AllocIndices( tri->indexes, tri->numIndexes * (int)sizeof( glIndex_t ), &ib );
	RB_RHI_StreamStore( r, tri->shadowCache, tri->indexes, vb, vertOfs, ib, idxOfs );
}

/*
=============
RB_RHI_BindStageImage

Binds the stage image (or current cinematic frame) on texture unit 0.
=============
*/
// sentinel from RB_RHI_BindStageImage's Vulkan branch: this stage samples an
// engine capture image (_currentRender/_scratch/...) that has never been
// captured this session — the caller must skip the draw (a dummy bind would
// render the double-vision/berserk overlays as solid white/checker)
static const rhi::ImageHandle RHI_SKIP_STAGE_IMAGE = (rhi::ImageHandle)0xffffffffu;

static rhi::ImageHandle RB_RHI_BindStageImage( const shaderStage_t *pStage, const float *regs, const viewDef_t *viewDef ) {
	const textureStage_t *texture = &pStage->texture;

	// Vulkan backend (Phase 4 M2): no GL binds — demand-load through Bind()
	// (a no-op upload-trigger there) and hand the RHI image handle back for
	// DrawArgs::textures[0].
	if ( rhi::GetActiveBackendType() == rhi::BT_VULKAN ) {
		if ( texture->cinematic ) {
			// M5: per-frame video upload through the RHI staging path
			if ( r_skipDynamicTextures.GetBool() ) {
				return globalImages->defaultImage->rhiHandle;
			}
			cinData_t cin = texture->cinematic->ImageForTime( (int)( 1000 * ( viewDef->floatTime + viewDef->renderView.shaderParms[11] ) ) );
			if ( cin.image ) {
				globalImages->cinematicImage->UploadScratch( cin.image, cin.imageWidth, cin.imageHeight );
				if ( globalImages->cinematicImage->rhiHandle ) {
					return globalImages->cinematicImage->rhiHandle;
				}
			}
			globalImages->blackImage->Bind();
			return globalImages->blackImage->rhiHandle;
		}
		if ( texture->image == globalImages->currentRenderImage
		     || texture->image == globalImages->currentDepthImage
		     || texture->image == globalImages->scratchImage
		     || texture->image == globalImages->scratchImage2
		     || texture->image == globalImages->accumImage ) {
			// capture images (M5): valid only once something captured into them
			if ( texture->image->rhiCaptured && texture->image->rhiHandle ) {
				return texture->image->rhiHandle;
			}
			return RHI_SKIP_STAGE_IMAGE;
		}
		if ( texture->image ) {
			texture->image->Bind();
			if ( texture->image->rhiHandle ) {
				return texture->image->rhiHandle;
			}
			globalImages->defaultImage->Bind();
			return globalImages->defaultImage->rhiHandle;
		}
		return 0;
	}

	rhi::gl3ActiveTexture( GL_TEXTURE0 );
	backEnd.glState.currenttmu = 0;		// keep idImage::Bind's per-tmu cache honest

	if ( texture->cinematic ) {
		if ( r_skipDynamicTextures.GetBool() ) {
			globalImages->defaultImage->Bind();
			return 0;
		}
		cinData_t cin = texture->cinematic->ImageForTime( (int)( 1000 * ( viewDef->floatTime + viewDef->renderView.shaderParms[11] ) ) );
		if ( cin.image ) {
			globalImages->cinematicImage->UploadScratch( cin.image, cin.imageWidth, cin.imageHeight );
		} else {
			globalImages->blackImage->Bind();
		}
	} else if ( texture->image ) {
		texture->image->Bind();
	}
	return 0;
}

/*
=============
RB_RHI_RenderCustomStage

newStage (custom ARB) drawn with its transpiled program pair. The ArbParams
fill mirrors RB_SetProgramEnvironment/Space and the legacy newStage path
(vertexParms -> program.local, fragmentProgramImages -> units).
=============
*/
static void RB_RHI_RenderCustomStage( rhi::RHI *r, const viewDef_t *viewDef, const drawSurf_t *surf,
                                      const shaderStage_t *pStage, rhi::ShaderHandle program, const float mvp[16],
                                      rhi::BufferHandle vb, int vertOfs, rhi::BufferHandle ib, int idxOfs ) {
	const srfTriangles_t *tri = surf->geo;
	const newShaderStage_t *ns = pStage->newStage;
	const float *regs = surf->shaderRegisters;

	rhi::ArbParams ap;
	memset( &ap, 0, sizeof( ap ) );
	memcpy( ap.mvpMatrix, mvp, sizeof( ap.mvpMatrix ) );
	memcpy( ap.modelViewMatrix, surf->space->modelViewMatrix, sizeof( ap.modelViewMatrix ) );
	memcpy( ap.projectionMatrix, viewDef->projectionMatrix, sizeof( ap.projectionMatrix ) );
	ap.textureMatrix[0] = ap.textureMatrix[5] = ap.textureMatrix[10] = ap.textureMatrix[15] = 1.0f;

	// RB_SetProgramEnvironment: env[0] = screen POT correction (both stages),
	// fragment env[1] = window coord scale, fragment env[22] = depth recips;
	// vertex env[1] = global view origin. _currentRender isn't captured yet
	// (Chunk F), so fall back to viewport size while uploadWidth is 0.
	int w = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	int h = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
	int potW = globalImages->currentRenderImage->uploadWidth > 0 ? globalImages->currentRenderImage->uploadWidth : w;
	int potH = globalImages->currentRenderImage->uploadHeight > 0 ? globalImages->currentRenderImage->uploadHeight : h;
	ap.venv[0][0] = ap.fenv[0][0] = (float)w / potW;
	ap.venv[0][1] = ap.fenv[0][1] = (float)h / potH;
	ap.venv[0][3] = ap.fenv[0][3] = 1.0f;
	ap.fenv[1][0] = 1.0f / w;
	ap.fenv[1][1] = 1.0f / h;
	ap.fenv[1][3] = 1.0f;
	if ( globalImages->currentDepthImage->uploadWidth > 0 ) {
		ap.fenv[22][0] = 1.0f / globalImages->currentDepthImage->uploadWidth;
		ap.fenv[22][1] = 1.0f / globalImages->currentDepthImage->uploadHeight;
		ap.fenv[22][2] = (float)potW / globalImages->currentDepthImage->uploadWidth;
		ap.fenv[22][3] = (float)potH / globalImages->currentDepthImage->uploadHeight;
	}
	ap.venv[1][0] = viewDef->renderView.vieworg[0];
	ap.venv[1][1] = viewDef->renderView.vieworg[1];
	ap.venv[1][2] = viewDef->renderView.vieworg[2];
	ap.venv[1][3] = 1.0f;

	// RB_SetProgramEnvironmentSpace: vertex env[5] = view origin in local
	// space, env[6-8] = model matrix rows
	idVec3 localView;
	R_GlobalPointToLocal( surf->space->modelMatrix, viewDef->renderView.vieworg, localView );
	ap.venv[5][0] = localView[0];
	ap.venv[5][1] = localView[1];
	ap.venv[5][2] = localView[2];
	ap.venv[5][3] = 1.0f;
	const float *mm = surf->space->modelMatrix;
	for ( int row = 0; row < 3; row++ ) {
		ap.venv[6 + row][0] = mm[row];
		ap.venv[6 + row][1] = mm[row + 4];
		ap.venv[6 + row][2] = mm[row + 8];
		ap.venv[6 + row][3] = mm[row + 12];
	}

	// vertexParms -> vertex program.local
	for ( int i = 0; i < ns->numVertexParms; i++ ) {
		ap.vlocal[i][0] = regs[ns->vertexParms[i][0]];
		ap.vlocal[i][1] = regs[ns->vertexParms[i][1]];
		ap.vlocal[i][2] = regs[ns->vertexParms[i][2]];
		ap.vlocal[i][3] = regs[ns->vertexParms[i][3]];
	}

	// fragment program images by unit. On Vulkan the qgl* active-unit binds are
	// NULL no-ops, so the images must reach the shader through DrawArgs.textures
	// (with the same _currentRender capture guard the builtin-ARB path uses); on
	// GL3 they still bind through idImage's active-unit path as before.
	rhi::DrawArgs da;
	memset( &da, 0, sizeof( da ) );
	const bool vk = rhi::GetActiveBackendType() == rhi::BT_VULKAN;
	for ( int i = 0; i < ns->numFragmentProgramImages && i < 8; i++ ) {
		idImage *img = ns->fragmentProgramImages[i];
		if ( !img ) {
			continue;
		}
		if ( vk ) {
			if ( img == globalImages->currentRenderImage || img == globalImages->currentDepthImage
			     || img == globalImages->scratchImage || img == globalImages->scratchImage2
			     || img == globalImages->accumImage ) {
				if ( !img->rhiCaptured || !img->rhiHandle ) {
					RB_RHI_LogOnce( "VK: custom-ARB stage sampling a never-captured image skipped" );
					return;
				}
				da.textures[i] = img->rhiHandle;
			} else {
				img->Bind();	// upload trigger only under Vulkan
				da.textures[i] = img->rhiHandle;
			}
		} else {
			rhi::gl3ActiveTexture( GL_TEXTURE0 + i );
			backEnd.glState.currenttmu = i;
			img->Bind();
		}
	}

	rhi::BufferHandle ub;
	int uniOfs = r->AllocUniforms( &ap, sizeof( ap ), &ub );

	rhi::PipelineDesc pd;
	pd.stateBits = ( pStage->drawStateBits & ~GLS_ATEST_BITS );
	if ( !viewDef->viewEntitys ) {
		pd.stateBits |= GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK;
	}
	pd.shader = program;
	pd.vertexLayout = rhi::VL_DRAWVERT;
	pd.cullType = RB_RHI_CullFor( viewDef, surf->material->GetCullType() );
	r->BindPipeline( pd );

	int idxCount = RB_RHI_DeformSubStage( surf, vb, vertOfs, ib, idxOfs );
	da.vertexBuffer = vb;
	da.vertexOffset = vertOfs;
	da.indexBuffer = ib;
	da.firstIndex = idxOfs / (int)sizeof( glIndex_t );
	da.indexCount = idxCount;
	da.uniformBuffer = ub;
	da.uniformOffset = uniOfs;
	da.uniformSize = sizeof( ap );
	r->Draw( da );

	backEnd.pc.c_drawElements++;
	backEnd.pc.c_drawIndexes += tri->numIndexes;
	backEnd.pc.c_drawVertexes += tri->numVerts;
}

/*
=============
RB_RHI_RenderBuiltinArbStage

Vulkan (Phase 4 M5): a stock custom-ARB stage drawn with its hand-translated
builtin program (heathaze family, colorprocess — see IR_VkBuiltinForArb).
These use the RenderParams model: stage vertexParms land in u_localParam0/1
and the fragmentProgramImages bind by unit, mirroring what the ARB programs
read from program.local[0..1] and their fragment maps.
=============
*/
static void RB_RHI_RenderBuiltinArbStage( rhi::RHI *r, const viewDef_t *viewDef, const drawSurf_t *surf,
                                          const shaderStage_t *pStage, rhi::ShaderHandle program, const float mvp[16],
                                          rhi::BufferHandle vb, int vertOfs, rhi::BufferHandle ib, int idxOfs ) {
	const srfTriangles_t *tri = surf->geo;
	const newShaderStage_t *ns = pStage->newStage;
	const float *regs = surf->shaderRegisters;

	// fragment program images by unit; capture images must hold a real capture
	rhi::DrawArgs da;
	memset( &da, 0, sizeof( da ) );
	for ( int i = 0; i < ns->numFragmentProgramImages && i < 8; i++ ) {
		idImage *img = ns->fragmentProgramImages[i];
		if ( !img ) {
			continue;
		}
		if ( img == globalImages->currentRenderImage || img == globalImages->currentDepthImage
		     || img == globalImages->scratchImage || img == globalImages->scratchImage2
		     || img == globalImages->accumImage ) {
			if ( !img->rhiCaptured || !img->rhiHandle ) {
				RB_RHI_LogOnce( "VK: builtin-ARB stage sampling a never-captured image skipped" );
				return;
			}
			da.textures[i] = img->rhiHandle;
		} else {
			img->Bind();	// upload trigger only under Vulkan
			da.textures[i] = img->rhiHandle;
		}
	}

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	memcpy( parms.mvpMatrix, mvp, sizeof( parms.mvpMatrix ) );
	memcpy( parms.modelViewMatrix, surf->space->modelViewMatrix, sizeof( parms.modelViewMatrix ) );
	memcpy( parms.projectionMatrix, viewDef->projectionMatrix, sizeof( parms.projectionMatrix ) );

	// screen POT correction + window coords, as RB_SetProgramEnvironment feeds
	// env[0]/env[1] to the ARB originals
	const int w = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int h = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
	const int potW = globalImages->currentRenderImage->uploadWidth > 0 ? globalImages->currentRenderImage->uploadWidth : w;
	const int potH = globalImages->currentRenderImage->uploadHeight > 0 ? globalImages->currentRenderImage->uploadHeight : h;
	parms.screenCorrection[0] = (float)w / potW;
	parms.screenCorrection[1] = (float)h / potH;
	parms.windowCoord[0] = 1.0f / w;
	// this path only runs on Vulkan: top-down gl_FragCoord vs the GL-layout capture —
	// the shaders add u_windowCoord.w to the row term (0 on GL). The offset is 1.0:
	// screenTc = (fragY*(-1/h) + w.w) * screenCorrection.y, and screenCorrection already
	// carries h/potH, so w.w must be 1.0 for fragY 0..h -> V h/potH..0. The old
	// vidHeight/h only equalled 1.0 when h==vidHeight; in a sub-window render (berserk
	// crops the scene, h != vidHeight) it shifted the sample, so _currentRender heat-haze
	// blood decals showed a shrunk copy of the window. 1.0 is correct at any viewport size.
	parms.windowCoord[1] = -1.0f / h;
	parms.windowCoord[3] = 1.0f;

	// stage vertexParms -> u_localParam0/1 (the stock programs use locals 0/1)
	for ( int i = 0; i < ns->numVertexParms && i < 2; i++ ) {
		float *dst = i == 0 ? parms.localParam0 : parms.localParam1;
		dst[0] = regs[ns->vertexParms[i][0]];
		dst[1] = regs[ns->vertexParms[i][1]];
		dst[2] = regs[ns->vertexParms[i][2]];
		dst[3] = regs[ns->vertexParms[i][3]];
	}

	rhi::BufferHandle ub;
	int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

	rhi::PipelineDesc pd;
	pd.stateBits = ( pStage->drawStateBits & ~GLS_ATEST_BITS );
	if ( !viewDef->viewEntitys ) {
		pd.stateBits |= GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK;
	}
	pd.shader = program;
	pd.vertexLayout = rhi::VL_DRAWVERT;
	pd.cullType = RB_RHI_CullFor( viewDef, surf->material->GetCullType() );
	r->BindPipeline( pd );

	int idxCount = RB_RHI_DeformSubStage( surf, vb, vertOfs, ib, idxOfs );
	da.vertexBuffer = vb;
	da.vertexOffset = vertOfs;
	da.indexBuffer = ib;
	da.firstIndex = idxOfs / (int)sizeof( glIndex_t );
	da.indexCount = idxCount;
	da.uniformBuffer = ub;
	da.uniformOffset = uniOfs;
	da.uniformSize = sizeof( parms );
	r->Draw( da );

	backEnd.pc.c_drawElements++;
	backEnd.pc.c_drawIndexes += tri->numIndexes;
	backEnd.pc.c_drawVertexes += tri->numVerts;
}

/*
=============
RB_RHI_WobbleMatrix

Replicates R_WobbleskyTexGen's per-frame rotation. Returns the 3 rows the
skybox shader dots the (vertex - localViewOrigin) direction against — i.e.
R_LocalPointToGlobal's rows: row[k] = ( m[k], m[4+k], m[8+k] ).
=============
*/
static void RB_RHI_WobbleMatrix( const drawSurf_t *surf, const viewDef_t *viewDef, float rows[3][3] ) {
	const int *parms = surf->material->GetTexGenRegisters();
	float wobbleDegrees = surf->shaderRegisters[parms[0]] * idMath::PI / 180.0f;
	float wobbleSpeed = surf->shaderRegisters[parms[1]] * 2.0f * idMath::PI / 60.0f;
	float rotateSpeed = surf->shaderRegisters[parms[2]] * 2.0f * idMath::PI / 60.0f;

	float a = viewDef->floatTime * wobbleSpeed;
	float s = sin( a ) * sin( wobbleDegrees );
	float c = cos( a ) * sin( wobbleDegrees );
	float z = cos( wobbleDegrees );

	idVec3 axis[3];
	axis[2][0] = c;
	axis[2][1] = s;
	axis[2][2] = z;
	axis[1][0] = -sin( a * 2 ) * sin( wobbleDegrees );
	axis[1][2] = -s * sin( wobbleDegrees );
	axis[1][1] = sqrt( 1.0f - ( axis[1][0] * axis[1][0] + axis[1][2] * axis[1][2] ) );
	axis[1] -= ( axis[2] * axis[1] ) * axis[2];
	axis[1].Normalize();
	axis[0].Cross( axis[1], axis[2] );

	s = sin( rotateSpeed * viewDef->floatTime );
	c = cos( rotateSpeed * viewDef->floatTime );

	float t[16];
	t[0] = axis[0][0] * c + axis[1][0] * s;
	t[4] = axis[0][1] * c + axis[1][1] * s;
	t[8] = axis[0][2] * c + axis[1][2] * s;
	t[1] = axis[1][0] * c - axis[0][0] * s;
	t[5] = axis[1][1] * c - axis[0][1] * s;
	t[9] = axis[1][2] * c - axis[0][2] * s;
	t[2] = axis[2][0];
	t[6] = axis[2][1];
	t[10] = axis[2][2];

	rows[0][0] = t[0]; rows[0][1] = t[4]; rows[0][2] = t[8];
	rows[1][0] = t[1]; rows[1][1] = t[5]; rows[1][2] = t[9];
	rows[2][0] = t[2]; rows[2][1] = t[6]; rows[2][2] = t[10];
}

// ---- DUDE glass reflection probes (docs/ssr.md) ----
// Per portal-area baked cubemaps that replace the generic env/gen* fallback on
// glass while glass SSR is on: head-on panes reflect the actual room (SSR can
// only mirror on-screen content, and a window you face reflects what's behind
// the camera). Baked by the bakeGlassProbe command (queued automatically when
// missing) to fs_savepath envprobes/<map>/, loaded lazily here as native-layout
// cubemaps — the same files the material cubeMap keyword could load.
struct rhiGlassProbe_t {
	int			area;
	idImage *	img;		// NULL until a bake exists on disk
	float		avg;		// mean face brightness [0,1] for energy normalization
	bool		checked;	// disk probed since the last invalidate
	bool		bakeQueued;	// auto-bake buffered once (don't spam the queue)
};
static idList<rhiGlassProbe_t>	rhiGlassProbes;
static idStr					rhiGlassProbeMapName;

// mean RGB brightness [0,1] of a native-layout cube's six faces, or -1 if the
// files can't be loaded. Used to match the probe's energy to the env/gen* cube
// it replaces: materials tuned their stage math for that image's darkness
// (chiglass adds env/gen1 at full vertex colour and stays subtle only because
// gen1 is nearly black), so the swap must not change the average energy.
static float RB_RHI_CubeFilesAvg( const char *base ) {
	byte *pics[6];
	int size = 0;
	ID_TIME_T ts;
	if ( !R_LoadCubeImages( base, CF_NATIVE, pics, &size, &ts ) || size <= 0 ) {
		return -1.0f;
	}
	double sum = 0.0;
	const int pixels = size * size;
	for ( int f = 0; f < 6; f++ ) {
		const byte *p = pics[f];
		for ( int i = 0; i < pixels; i++, p += 4 ) {
			sum += p[0] + p[1] + p[2];
		}
	}
	for ( int f = 0; f < 6; f++ ) {
		R_StaticFree( pics[f] );
	}
	return (float)( sum / ( 6.0 * pixels * 3.0 * 255.0 ) );
}

// cached average brightness of the original env/gen* cubemaps (tiny, loaded once)
struct rhiCubeAvg_t { idStr name; float avg; };
static idList<rhiCubeAvg_t> rhiEnvCubeAvgs;

static float RB_RHI_EnvCubeAvg( const char *name ) {
	for ( int i = 0; i < rhiEnvCubeAvgs.Num(); i++ ) {
		if ( rhiEnvCubeAvgs[i].name.Icmp( name ) == 0 ) {
			return rhiEnvCubeAvgs[i].avg;
		}
	}
	rhiCubeAvg_t e;
	e.name = name;
	e.avg = RB_RHI_CubeFilesAvg( name );
	rhiEnvCubeAvgs.Append( e );
	return e.avg;
}

void RB_RHI_InvalidateGlassProbe( int area ) {
	for ( int i = 0; i < rhiGlassProbes.Num(); i++ ) {
		if ( rhiGlassProbes[i].area == area ) {
			rhiGlassProbes[i].checked = false;
			rhiGlassProbes[i].img = NULL;
			rhiGlassProbes[i].avg = -1.0f;
			rhiGlassProbes[i].bakeQueued = false;
		}
	}
}

static idImage *RB_RHI_GlassProbeForSurface( const viewDef_t *viewDef, const drawSurf_t *surf,
                                             float *probeAvg ) {
	*probeAvg = -1.0f;
	if ( !r_ssrGlassProbes.GetBool() || !tr.primaryWorld ) {
		return NULL;
	}
	// new map: forget the previous map's probes
	const char *mapName = tr.primaryWorld->mapName.c_str();
	if ( rhiGlassProbeMapName.Icmp( mapName ) != 0 ) {
		rhiGlassProbes.Clear();
		rhiGlassProbeMapName = mapName;
	}

	// the pane's area — panes often sit ON an area boundary (window portals), so
	// nudge the center toward the viewer to land on the viewer's side, which is
	// the room the reflection should show
	idVec3 center = surf->geo->bounds.GetCenter();
	idVec3 world;
	R_LocalPointToGlobal( surf->space->modelMatrix, center, world );
	idVec3 toEye = viewDef->renderView.vieworg - world;
	toEye.Normalize();
	int area = tr.primaryWorld->PointInArea( world + toEye * 8.0f );
	if ( area < 0 ) {
		area = tr.primaryWorld->PointInArea( world );
	}
	if ( area < 0 ) {
		return NULL;
	}

	int idx = -1;
	for ( int i = 0; i < rhiGlassProbes.Num(); i++ ) {
		if ( rhiGlassProbes[i].area == area ) {
			idx = i;
			break;
		}
	}
	if ( idx < 0 ) {
		rhiGlassProbe_t e;
		e.area = area;
		e.img = NULL;
		e.avg = -1.0f;
		e.checked = false;
		e.bakeQueued = false;
		idx = rhiGlassProbes.Append( e );
	}
	rhiGlassProbe_t &e = rhiGlassProbes[idx];

	if ( !e.checked ) {
		e.checked = true;
		idStr base;
		R_GlassProbeBasePath( mapName, area, base );
		ID_TIME_T ts;
		if ( fileSystem->ReadFile( va( "%s_px.tga", base.c_str() ), NULL, &ts ) >= 0 ) {
			idImage *img = globalImages->ImageFromFile( base.c_str(), TF_DEFAULT, false,
				TR_CLAMP, TD_HIGH_QUALITY, CF_NATIVE );
			if ( img && img != globalImages->defaultImage ) {
				e.img = img;
				e.avg = RB_RHI_CubeFilesAvg( base.c_str() );
			}
		}
	}

	if ( !e.img && !e.bakeQueued && r_ssrGlassProbeBake.GetBool()
	     && viewDef->viewEntitys && !viewDef->isSubview ) {
		// the bake captures from the current eye position, so only queue while
		// the viewer actually stands in this area; a pane looking into a
		// neighbouring area gets its probe when the player goes there, and the
		// env/gen fallback stands until then
		if ( tr.primaryWorld->PointInArea( viewDef->renderView.vieworg ) == area ) {
			e.bakeQueued = true;
			cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "bakeGlassProbe\n" );
		}
	}
	*probeAvg = e.avg;
	return e.img;
}

/*
=============
RB_RHI_RenderTexgenStage

Fixed-function texgen stages (skybox/wobblesky/diffuse cube, cube reflection,
portal sky) through their dedicated programs. Mirrors RB_PrepareStageTexturing:
the texcoord generation the old path fed glTexGen / a dynamic vertex stream is
reproduced in the shaders from these uniforms and the standard vertex layout.
=============
*/
static void RB_RHI_RenderTexgenStage( rhi::RHI *r, const viewDef_t *viewDef, const drawSurf_t *surf,
                                      const shaderStage_t *pStage, const rhi::StageIR &si, const float mvp[16],
                                      rhi::BufferHandle vb, int vertOfs, rhi::BufferHandle ib, int idxOfs ) {
	const srfTriangles_t *tri = surf->geo;
	const float *regs = surf->shaderRegisters;

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	memcpy( parms.mvpMatrix, mvp, sizeof( parms.mvpMatrix ) );
	parms.color[0] = regs[pStage->color.registers[0]];
	parms.color[1] = regs[pStage->color.registers[1]];
	parms.color[2] = regs[pStage->color.registers[2]];
	parms.color[3] = regs[pStage->color.registers[3]];

	// vertex-colour mode -> modulate/add, exactly as the generic path (and the
	// old fixed-function/ARB reflect stage) does, so environment.vert can fold the
	// stage colour (u_color) into the reflection. SVC_IGNORE zeroes attr_Color and
	// keeps u_color; without this the cube reflection ignored the (dimming) stage
	// colour and washed the surface with a tint.
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

	// DUDE: dampen cube-reflection ("sheen") brightness. On the enhancement
	// backends the scene behind glass is lit brighter than the original renderer
	// (SSAO, emissive fill lights, ambient), so the environment-map sheen — which
	// the reflection modulates by the stage colour (u_color) — reads stronger than
	// on legacy. r_gl3ReflectionScale (default 0.7 = -30%) compensates; 1.0 restores
	// the untouched cube. Only the TG_REFLECT_CUBE (glass etc.) stages are affected.
	// See docs/readme-changes.md.
	if ( si.texgen == TG_REFLECT_CUBE ) {
		const float s = r_gl3ReflectionScale.GetFloat();
		parms.color[0] *= s;
		parms.color[1] *= s;
		parms.color[2] *= s;
	}

	// view origin in this surface's local space (skybox/reflection direction)
	idVec3 localViewOrigin;
	R_GlobalPointToLocal( surf->space->modelMatrix, viewDef->renderView.vieworg, localViewOrigin );
	parms.localViewOrigin[0] = localViewOrigin[0];
	parms.localViewOrigin[1] = localViewOrigin[1];
	parms.localViewOrigin[2] = localViewOrigin[2];
	parms.localViewOrigin[3] = 1.0f;

	const bool vkMode = rhi::GetActiveBackendType() == rhi::BT_VULKAN;
	rhi::ImageHandle vkTex[2] = { 0, 0 };
	if ( !vkMode ) {
		rhi::gl3ActiveTexture( GL_TEXTURE0 );
		backEnd.glState.currenttmu = 0;
	}

	switch ( si.texgen ) {
	case TG_SCREEN:
	case TG_SCREEN2: {
		// screen-position blit: portal sky samples the pre-rendered sky from
		// _currentRender; mirror/xray stages (mirrorRenderMap sets TG_SCREEN +
		// texture.dynamic) sample their own subview capture (_scratch), the
		// image the legacy path bound (RB_SetProgramEnvironment env[0]/[1])
		idImage *screenImg = ( pStage->texture.dynamic && pStage->texture.image )
			? pStage->texture.image : globalImages->currentRenderImage;
		if ( vkMode && !screenImg->rhiCaptured ) {
			return;		// nothing captured yet (first frames of the subview)
		}
		int w = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
		int h = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
		int potW = screenImg->uploadWidth > 0 ? screenImg->uploadWidth : w;
		int potH = screenImg->uploadHeight > 0 ? screenImg->uploadHeight : h;
		parms.screenCorrection[0] = (float)w / potW;
		parms.screenCorrection[1] = (float)h / potH;
		parms.windowCoord[0] = 1.0f / w;
		parms.windowCoord[1] = 1.0f / h;
		if ( vkMode ) {
			// Vulkan gl_FragCoord is top-down but the capture keeps GL's bottom-up
			// layout; portalsky.frag adds u_windowCoord.w to the row term (0 on GL).
			// The offset is 1.0 (screenCorrection already carries h/potH); the old
			// vidHeight/h only matched when h==vidHeight and mis-sampled sub-window
			// (cropped) renders — same fix as the heat-haze path above.
			parms.windowCoord[1] = -1.0f / h;
			parms.windowCoord[3] = 1.0f;
		}
		screenImg->Bind();
		vkTex[0] = screenImg->rhiHandle;
		break;
	}
	case TG_SKYBOX_CUBE:
	case TG_WOBBLESKY_CUBE: {
		float rows[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
		if ( si.texgen == TG_WOBBLESKY_CUBE ) {
			RB_RHI_WobbleMatrix( surf, viewDef, rows );
		}
		for ( int i = 0; i < 3; i++ ) {
			float *dst = i == 0 ? parms.modelMatrixRow0 : ( i == 1 ? parms.modelMatrixRow1 : parms.modelMatrixRow2 );
			dst[0] = rows[i][0]; dst[1] = rows[i][1]; dst[2] = rows[i][2]; dst[3] = 0.0f;
		}
		pStage->texture.image->Bind();
		vkTex[0] = pStage->texture.image->rhiHandle;
		break;
	}
	case TG_DIFFUSE_CUBE:
		pStage->texture.image->Bind();
		vkTex[0] = pStage->texture.image->rhiHandle;
		break;
	case TG_REFLECT_CUBE: {
		// model matrix rows for the bumpy variant's tangent->global rotation
		const float *mm = surf->space->modelMatrix;
		for ( int row = 0; row < 3; row++ ) {
			float *dst = row == 0 ? parms.modelMatrixRow0 : ( row == 1 ? parms.modelMatrixRow1 : parms.modelMatrixRow2 );
			dst[0] = mm[row]; dst[1] = mm[row + 4]; dst[2] = mm[row + 8]; dst[3] = mm[row + 12];
		}
		// reflection cube on unit 0; with glass probes on, a baked room probe
		// (docs/ssr.md) replaces the cube so panes reflect the actual room at
		// any angle. Only the generic grey env/gen* cubes are swapped — other
		// cubemaps are authored content (machine chrome, tinted effects) that a
		// room capture shouldn't override. The probe's brightness is normalized
		// to the replaced cube's average energy: materials tune their stage
		// math around that image's darkness (chiglass1blue ADDS env/gen1 at
		// full vertex colour and reads subtle only because gen1 is nearly
		// black), so swapping content must not change the energy budget.
		// r_ssrGlassProbeScale then applies as the glass-only intensity knob
		// (bump-mapped glass ignores stage colour — vanilla behaviour).
		idImage *cubeImg = pStage->texture.image;
		if ( r_ssr.GetBool() && r_ssrGlassProbes.GetBool() && !surf->material->GetBumpStage()
		     && idStr::Icmpn( cubeImg->imgName, "env/gen", 7 ) == 0 ) {
			float probeAvg = -1.0f;
			idImage *probe = RB_RHI_GlassProbeForSurface( viewDef, surf, &probeAvg );
			if ( probe ) {
				cubeImg = probe;
				float norm = 1.0f;
				const float envAvg = RB_RHI_EnvCubeAvg( pStage->texture.image->imgName );
				if ( envAvg > 0.0f && probeAvg > 0.001f ) {
					norm = idMath::ClampFloat( 0.02f, 4.0f, envAvg / probeAvg );
				}
				const float ps = norm * r_ssrGlassProbeScale.GetFloat();
				parms.color[0] *= ps;
				parms.color[1] *= ps;
				parms.color[2] *= ps;
			}
		}
		cubeImg->Bind();
		vkTex[0] = cubeImg->rhiHandle;
		if ( vkMode && vkTex[0] == 0 ) {
			// a white-dummy fallback washes the pane out (additive white);
			// say which cube failed to bridge instead of hiding it
			static int warned = 0;
			if ( warned < 8 ) {
				warned++;
				common->Warning( "VK: reflection cube '%s' has no RHI image - pane will wash out white",
				                 cubeImg->imgName.c_str() );
			}
		}
		const shaderStage_t *bumpStage = surf->material->GetBumpStage();
		if ( bumpStage ) {
			if ( vkMode ) {
				bumpStage->texture.image->Bind();	// upload trigger only
				vkTex[1] = bumpStage->texture.image->rhiHandle;
			} else {
				rhi::gl3ActiveTexture( GL_TEXTURE1 );
				backEnd.glState.currenttmu = 1;
				bumpStage->texture.image->Bind();
				rhi::gl3ActiveTexture( GL_TEXTURE0 );
				backEnd.glState.currenttmu = 0;
			}
		}
		break;
	}
	default:
		return;
	}

	rhi::BufferHandle ub;
	int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

	const bool sky = ( si.texgen == TG_SCREEN || si.texgen == TG_SCREEN2
		|| si.texgen == TG_SKYBOX_CUBE || si.texgen == TG_WOBBLESKY_CUBE );

	rhi::PipelineDesc pd;
	if ( sky ) {
		// sky is pinned to the far plane by the shader; depth-LEQUAL lets all
		// geometry occlude it and GLS_DEPTHMASK keeps it from writing depth (it
		// was skipped in the prepass), so it fills only empty pixels
		pd.stateBits = ( pStage->drawStateBits & ~( GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHFUNC_EQUAL | GLS_ATEST_BITS ) ) | GLS_DEPTHMASK;
	} else {
		pd.stateBits = ( pStage->drawStateBits & ~GLS_ATEST_BITS );
		if ( !viewDef->viewEntitys ) {
			pd.stateBits |= GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK;
		}
	}
	pd.shader = si.program;
	pd.vertexLayout = rhi::VL_DRAWVERT;
	pd.cullType = RB_RHI_CullFor( viewDef, surf->material->GetCullType() );
	r->BindPipeline( pd );

	rhi::DrawArgs da;
	memset( &da, 0, sizeof( da ) );
	int idxCount = RB_RHI_DeformSubStage( surf, vb, vertOfs, ib, idxOfs );
	da.vertexBuffer = vb;
	da.vertexOffset = vertOfs;
	da.indexBuffer = ib;
	da.firstIndex = idxOfs / (int)sizeof( glIndex_t );
	da.indexCount = idxCount;
	da.uniformBuffer = ub;
	da.uniformOffset = uniOfs;
	da.uniformSize = sizeof( parms );
	if ( vkMode ) {
		da.textures[0] = vkTex[0];
		da.textures[1] = vkTex[1];
	}
	r->Draw( da );

	backEnd.pc.c_drawElements++;
	backEnd.pc.c_drawIndexes += tri->numIndexes;
	backEnd.pc.c_drawVertexes += tri->numVerts;
}

/*
=============
RB_RHI_ParticleLooksLikeSmoke

Heuristic for the smoke-darkness blend: does this particle material read as
smoke/steam/dust rather than a self-lit effect (fire, sparks, glares, energy)?
Doom 3 draws both with the same additive "blend add" and both span the whole
colour/brightness range, so pixels can't tell them apart - but the material
names can. Doom 3's smoke/steam/dust stages carry those words in the material
name (e.g. textures/particles/smokepuff) while flames use firestrip, pfirebig,
flamesparks, spark3 etc. So additive particles are only dimmed when their
material name matches this allow-list; everything else stays bright. Alpha-
blended particles are dimmed regardless (fire/sparks are additive, so alpha
is safe).
=============
*/
static bool RB_RHI_ParticleLooksLikeSmoke( const idMaterial *mat ) {
	if ( !mat ) {
		return false;
	}
	idStr name = mat->GetName();
	static const char * const kw[] = {
		"smoke", "steam", "dust", "fog", "mist", "vapor", "vapour",
		"haze", "smog", "cloud", "fume", "exhaust"
	};
	for ( int i = 0; i < (int)( sizeof( kw ) / sizeof( kw[0] ) ); i++ ) {
		if ( name.Find( kw[i], false ) != -1 ) {
			return true;
		}
	}
	return false;
}

/*
=============
RB_RHI_RenderSoftParticleStage

SteveL #3878 particle softening, ported to the GL3 backend. Fades a particle
quad where it approaches solid scene geometry (sampled from _currentDepth) and
where it gets too close to the eye, so smoke/fog no longer show a hard
intersection seam against walls and floors. Mirrors the soft-particle branch of
RB_STD_T_RenderShaderPasses (draw_common.cpp): the depth test is forced off so
the quad overdraws and the shader does the clipping instead.

Only reached for DSF_SOFT_PARTICLE surfaces with a positive radius and an
additive or src-alpha blend; the front-end (R_AddDrawSurf) already gates the
flag to the GL3/Vulkan backends and to r_useSoftParticles + r_enableDepthCapture.
Returns false (leaving nothing drawn) if the softparticle program failed to
build, so the caller can fall through to the generic path.
=============
*/
static bool RB_RHI_RenderSoftParticleStage( rhi::RHI *r, const viewDef_t *viewDef, const drawSurf_t *surf,
                                            const shaderStage_t *pStage, const float *regs, int src_blend,
                                            const float color[4], const float mvp[16], const srfTriangles_t *tri,
                                            rhi::BufferHandle vb, int vertOfs, rhi::BufferHandle ib, int idxOfs ) {
	rhi::ShaderHandle prog = r->LoadShader( "softparticle" );
	if ( !prog ) {
		return false;	// shader unavailable: let the caller draw it generically
	}

	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	memcpy( parms.mvpMatrix, mvp, sizeof( parms.mvpMatrix ) );

	// diffuse (texture) matrix — identical reg evaluation to the generic path
	if ( pStage->texture.hasMatrix ) {
		parms.diffuseMatrixS[0] = regs[pStage->texture.matrix[0][0]];
		parms.diffuseMatrixS[1] = regs[pStage->texture.matrix[0][1]];
		parms.diffuseMatrixS[2] = 0.0f;
		parms.diffuseMatrixS[3] = regs[pStage->texture.matrix[0][2]];
		parms.diffuseMatrixT[0] = regs[pStage->texture.matrix[1][0]];
		parms.diffuseMatrixT[1] = regs[pStage->texture.matrix[1][1]];
		parms.diffuseMatrixT[2] = 0.0f;
		parms.diffuseMatrixT[3] = regs[pStage->texture.matrix[1][2]];
		if ( parms.diffuseMatrixS[3] < -40.0f || parms.diffuseMatrixS[3] > 40.0f ) {
			parms.diffuseMatrixS[3] -= (int)parms.diffuseMatrixS[3];
		}
		if ( parms.diffuseMatrixT[3] < -40.0f || parms.diffuseMatrixT[3] > 40.0f ) {
			parms.diffuseMatrixT[3] -= (int)parms.diffuseMatrixT[3];
		}
	} else {
		parms.diffuseMatrixS[0] = 1.0f;
		parms.diffuseMatrixT[1] = 1.0f;
	}

	// Particle colour. softparticle.vert computes
	// var_Color = (attr_Color*modulate + add) * u_color. The particle system
	// fades particles through the per-vertex colour, so use it raw; SVC_IGNORE
	// particles (rare, discouraged) carry the fade in the stage colour instead,
	// matching the old-style fallback in the legacy soft path.
	if ( pStage->vertexColor == SVC_IGNORE ) {
		parms.vertexColorAdd[0] = parms.vertexColorAdd[1] = parms.vertexColorAdd[2] = parms.vertexColorAdd[3] = 1.0f;
		parms.color[0] = color[0]; parms.color[1] = color[1]; parms.color[2] = color[2]; parms.color[3] = color[3];
	} else {
		parms.vertexColorModulate[0] = parms.vertexColorModulate[1] = parms.vertexColorModulate[2] = parms.vertexColorModulate[3] = 1.0f;
		parms.color[0] = parms.color[1] = parms.color[2] = parms.color[3] = 1.0f;
	}

	// env[23]: { radius, 1/fadeRange, 1/radius }. fadeRange is the particle
	// diameter for alpha blends (smoke is half as opaque with a wall mid-volume)
	// but the radius for additive blends (glares lose nothing to overdraw).
	float fadeRange = surf->particle_radius;
	if ( src_blend == GLS_SRCBLEND_SRC_ALPHA ) {
		fadeRange = surf->particle_radius * 2.0f;
	}
	parms.particleRadius[0] = surf->particle_radius;
	parms.particleRadius[1] = 1.0f / fadeRange;
	parms.particleRadius[2] = 1.0f / surf->particle_radius;

	// env[24]: colour-channel mask added to the fade multiplier. Additive blends
	// fade their RGB; alpha blends fade their alpha (leave the rest at 1).
	if ( src_blend == GLS_SRCBLEND_SRC_ALPHA ) {
		parms.channelMask[0] = parms.channelMask[1] = parms.channelMask[2] = 1.0f;
		parms.channelMask[3] = 0.0f;
	} else {	// GLS_SRCBLEND_ONE
		parms.channelMask[0] = parms.channelMask[1] = parms.channelMask[2] = 0.0f;
		parms.channelMask[3] = 1.0f;
	}

	// env[22].xy: reciprocal of the (power-of-two) _currentDepth size, mapping
	// gl_FragCoord to a depth texcoord (RB_SetProgramEnvironment / #3877).
	parms.depthTexRecip[0] = 1.0f / globalImages->currentDepthImage->uploadWidth;
	parms.depthTexRecip[1] = 1.0f / globalImages->currentDepthImage->uploadHeight;

	// DUDE smoke-darkness blend: dim smoke/steam/dust where the scene behind it is
	// dark, so puffs fade into shadow instead of reading as grey blobs. Snapshot the
	// lit scene into _currentRender once per view — particles sort back-to-front, so
	// opaque geometry, sky and emissive fills are already down when the first smoke
	// draws — then feed the shader the blend params (localParam0) and the render-
	// target size (localParam1) so it can sample the background luminance.
	// localParam0/1 are otherwise unused here.
	//
	// Alpha-blended particles ('blend blend') are dimmed unconditionally — smoke/fog/
	// dust; fire and sparks are additive, so alpha is safe. Additive particles
	// ('blend add', e.g. the common textures/particles/smokepuff steam) are dimmed
	// only when the material name reads as smoke, so additive flames/glares stay
	// bright. The shader branches alpha vs additive on channelMask.a.
	bool smokeDark = false;
	if ( r_smokeDarkBlend.GetBool() ) {
		if ( src_blend == GLS_SRCBLEND_SRC_ALPHA ) {
			smokeDark = true;
		} else if ( src_blend == GLS_SRCBLEND_ONE ) {
			smokeDark = RB_RHI_ParticleLooksLikeSmoke( surf->material );
		}
	}
	const bool vkMode = rhi::GetActiveBackendType() == rhi::BT_VULKAN;
	if ( smokeDark ) {
		if ( !backEnd.smokeBackgroundCaptured ) {
			if ( vkMode ) {
				RB_RHI_CopyCurrentRender( viewDef );	// RHI copy, no GL binds
			} else {
				rhi::gl3ActiveTexture( GL_TEXTURE2 );
				backEnd.glState.currenttmu = 2;
				RB_RHI_CopyCurrentRender( viewDef );	// binds + fills _currentRender on unit 2
				rhi::gl3ActiveTexture( GL_TEXTURE0 );
				backEnd.glState.currenttmu = 0;
			}
			backEnd.smokeBackgroundCaptured = true;
		}
		parms.localParam0[0] = 1.0f;								// enable
		parms.localParam0[1] = r_smokeDarkBlendFloor.GetFloat();	// opacity on a black background
		parms.localParam0[2] = idMath::ClampFloat( 0.05f, 1.0f, r_smokeDarkBlendKnee.GetFloat() );	// knee luma -> full
		const float rw = (float)globalImages->currentRenderImage->uploadWidth;
		const float rh = (float)globalImages->currentRenderImage->uploadHeight;
		parms.localParam1[0] = rw > 0.0f ? 1.0f / rw : 0.0f;
		parms.localParam1[1] = rh > 0.0f ? 1.0f / rh : 0.0f;
		if ( vkMode && rh > 0.0f ) {
			// Vulkan gl_FragCoord is top-down but the capture keeps GL's
			// bottom-up layout: y' = (vidHeight - fragY) / rh
			parms.localParam1[1] = -1.0f / rh;
			parms.localParam1[3] = (float)glConfig.vidHeight / rh;
		}
	}

	rhi::BufferHandle ub;
	int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

	// unit 0 = particle diffuse, unit 1 = _currentDepth
	rhi::ImageHandle vkTex[3] = { 0, 0, 0 };
	if ( vkMode ) {
		if ( pStage->texture.image ) {
			pStage->texture.image->Bind();	// upload trigger only under Vulkan
			vkTex[0] = pStage->texture.image->rhiHandle;
		}
		vkTex[1] = globalImages->currentDepthImage->rhiHandle;
		if ( smokeDark && globalImages->currentRenderImage->rhiCaptured ) {
			vkTex[2] = globalImages->currentRenderImage->rhiHandle;
		}
	} else {
		rhi::gl3ActiveTexture( GL_TEXTURE0 );
		backEnd.glState.currenttmu = 0;
		if ( pStage->texture.image ) {
			pStage->texture.image->Bind();
		}
		rhi::gl3ActiveTexture( GL_TEXTURE1 );
		backEnd.glState.currenttmu = 1;
		globalImages->currentDepthImage->Bind();
		if ( smokeDark ) {
			// unit 2 = _currentRender (captured lit scene) for the darkness blend
			rhi::gl3ActiveTexture( GL_TEXTURE2 );
			backEnd.glState.currenttmu = 2;
			globalImages->currentRenderImage->Bind();
		}
		rhi::gl3ActiveTexture( GL_TEXTURE0 );
		backEnd.glState.currenttmu = 0;
	}

	// depth test off (the shader fades against captured depth); strip alpha-test
	// bits, which the soft-particle shader doesn't implement
	rhi::PipelineDesc pd;
	pd.stateBits = ( pStage->drawStateBits & ~GLS_ATEST_BITS ) | GLS_DEPTHFUNC_ALWAYS;
	pd.shader = prog;
	pd.vertexLayout = rhi::VL_DRAWVERT;
	pd.cullType = RB_RHI_CullFor( viewDef, surf->material->GetCullType() );
	r->BindPipeline( pd );

	// No RB_RHI_DeformSubStage redirect (unlike the ARB/texgen helpers): a soft particle
	// is a view-oriented, all-additive/alpha sprite, never a classifier-approved tess body,
	// so tri->tessDeformVB is always 0 here and the deform would be a no-op anyway.
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
	if ( vkMode ) {
		da.textures[0] = vkTex[0];
		da.textures[1] = vkTex[1];
		da.textures[2] = vkTex[2];
	}
	r->Draw( da );

	// unbind _currentDepth (and _currentRender) so later stages expecting only
	// unit 0 aren't fed a stale binding
	if ( !vkMode ) {
		rhi::gl3ActiveTexture( GL_TEXTURE1 );
		backEnd.glState.currenttmu = 1;
		globalImages->BindNull();
		if ( smokeDark ) {
			rhi::gl3ActiveTexture( GL_TEXTURE2 );
			backEnd.glState.currenttmu = 2;
			globalImages->BindNull();
		}
		rhi::gl3ActiveTexture( GL_TEXTURE0 );
		backEnd.glState.currenttmu = 0;
	}

	backEnd.pc.c_drawElements++;
	backEnd.pc.c_drawIndexes += tri->numIndexes;
	backEnd.pc.c_drawVertexes += tri->numVerts;
	return true;
}

/*
=============
RB_RHI_StagePolyOffset{Begin,End}

Per-stage polygon offset (Material privatePolygonOffset): some weapon/decal stages
carry their own offset on top of any material-level MF_POLYGONOFFSET. Stock GL applies
it in RB_PrepareStageTexturing / disables it in RB_FinishStageTexturing
(draw_common.cpp:87,260); the RHI stage path never ported it (VK doubly so — qgl* are
NULL no-ops, only r->SetPolygonOffset -> vkCmdSetDepthBias lands). Mirrors the
material-level dual pattern at the top of RB_RHI_RenderShaderPasses. End restores the
material-level offset (or none) rather than leaving the stage's value latched, because
on VK the dynamic depth bias persists per-draw and would bleed into the following stages.

CRUCIAL EXCLUSION: skip DEPTHFUNC_EQUAL stages. Polygon offset only orders geometry
under an *inequality* depth test; at EQUAL a stage must match the zfill prepass depth
exactly, and the prepass carries no per-stage offset, so any bias makes the stage FAIL
EQUAL and vanish. The imp "burning corpse" fire stage is exactly this — privatePolygonOffset
-1 AND drawn at EQUAL (against the deform-once zfill, docs/tessellation.md) — so applying
the offset drops the ember. (Material-level MF_POLYGONOFFSET is fine at EQUAL: the RHI zfill
applies that same offset, so prepass and stage still match.)
=============
*/
static bool RB_RHI_StageWantsPolyOffset( const shaderStage_t *pStage ) {
	return pStage->privatePolygonOffset != 0.0f
	    && ( pStage->drawStateBits & GLS_DEPTHFUNC_EQUAL ) == 0;
}

static void RB_RHI_StagePolyOffsetBegin( rhi::RHI *r, const shaderStage_t *pStage ) {
	if ( !RB_RHI_StageWantsPolyOffset( pStage ) ) {
		return;
	}
	if ( qglEnable != NULL ) {
		qglEnable( GL_POLYGON_OFFSET_FILL );
		qglPolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * pStage->privatePolygonOffset );
	}
	r->SetPolygonOffset( true, r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * pStage->privatePolygonOffset );
}

static void RB_RHI_StagePolyOffsetEnd( rhi::RHI *r, const shaderStage_t *pStage, const idMaterial *shader ) {
	if ( !RB_RHI_StageWantsPolyOffset( pStage ) ) {
		return;
	}
	if ( shader->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
		// a material-level offset wraps the whole stage loop — restore it (matches
		// draw_common.cpp:260 leaving MF_POLYGONOFFSET's offset in place)
		if ( qglEnable != NULL ) {
			qglPolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
		}
		r->SetPolygonOffset( true, r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
	} else {
		if ( qglDisable != NULL ) {
			qglDisable( GL_POLYGON_OFFSET_FILL );
		}
		r->SetPolygonOffset( false, 0.0f, 0.0f );
	}
}

/*
=============
RB_RHI_RenderShaderPasses

Ambient stages of one surface, driven by the Material IR: old-style stages
through the generic program, newStage customs through their transpiled ARB
pairs. Mirrors RB_STD_T_RenderShaderPasses stage for stage.
=============
*/
static void RB_RHI_RenderShaderPasses( rhi::RHI *r, const viewDef_t *viewDef, const drawSurf_t *surf,
                                       const viewEntity_t *&currentSpace, float mvp[16] ) {
	const srfTriangles_t *tri = surf->geo;
	const idMaterial *shader = surf->material;

	if ( !shader->HasAmbient() || shader->IsPortalSky() ) {
		return;
	}

	if ( surf->space != currentSpace ) {
		currentSpace = surf->space;
		RB_RHI_SpaceMvp( viewDef, surf->space, mvp );
	}

	// track scissor state in backEnd.currentScissor like every legacy pass —
	// split tracking desyncs from the GL state the world pass leaves behind
	// (the last light's scissor rect), clipping every surface drawn after
	if ( r_useScissor.GetBool() && !backEnd.currentScissor.Equals( surf->scissorRect ) ) {
		backEnd.currentScissor = surf->scissorRect;
		r->SetScissor( viewDef->viewport.x1 + backEnd.currentScissor.x1,
		               viewDef->viewport.y1 + backEnd.currentScissor.y1,
		               backEnd.currentScissor.x2 + 1 - backEnd.currentScissor.x1,
		               backEnd.currentScissor.y2 + 1 - backEnd.currentScissor.y1 );
	}

	if ( !tri->numIndexes ) {
		return;
	}
	// a GPU-skinned surface (r_gpuSkinNoUpload) has no ambient cache — it rasterizes from gpuSkinVB,
	// which RB_RHI_StreamAmbient binds directly — so treat gpuSkinVB as satisfying this precondition.
	if ( !tri->ambientCache && !tri->gpuSkinVB ) {
		return;
	}

	const float *regs = surf->shaderRegisters;

	// MATERIAL-level polygon offset (MF_POLYGONOFFSET): coplanar decals (bullet/blood
	// hits, signs) rely on it to win the depth test against the wall they sit on. The
	// GL path enables it via qglPolygonOffset; on Vulkan qglEnable is NULL, so the RHI
	// dynamic depth bias (SetPolygonOffset -> vkCmdSetDepthBias) is the only thing that
	// lands. Without it decals z-fight the wall and flicker in/out with the camera. This
	// offset wraps the WHOLE stage loop; the matching disable is at the end of this
	// function (mirrors RB_RHI_FillDepthBuffer). The separate PER-STAGE offset
	// (privatePolygonOffset: dissolve/weapon stages) is bracketed per draw inside the
	// loop by RB_RHI_StagePolyOffset{Begin,End}, which restore this material offset.
	if ( shader->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
		if ( qglEnable != NULL ) {
			qglEnable( GL_POLYGON_OFFSET_FILL );
			qglPolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
		}
		r->SetPolygonOffset( true, r_offsetFactor.GetFloat(),
		                     r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
	}

	// depth range hacks (matrix side handled by RB_RHI_SpaceMvp; these are
	// core-safe depth range calls, mirroring RB_STD_T_RenderShaderPasses)
	if ( surf->space->weaponDepthHack ) {
		RB_EnterWeaponDepthHack();
	}
	if ( surf->space->modelDepthHack != 0.0f && !( surf->dsFlags & DSF_SOFT_PARTICLE ) ) {
		RB_EnterModelDepthHack( surf->space->modelDepthHack );
	}

	// stream this surface's frame-temporary geometry once, draw per stage
	rhi::BufferHandle vb, ib;
	int vertOfs, idxOfs;
	RB_RHI_StreamAmbient( r, tri, vb, vertOfs, ib, idxOfs );

	const rhi::MaterialIR *ir = rhi::IR_Get( shader );

	// DUDE berserk vision: the stock effect is a recursive _scratch feedback
	// (textures/decals/berserk zooms the previous frame 3% each frame) that doesn't
	// accumulate on the RHI path. Reproduce the "streak zoom" instead as a TEMPORAL ghost
	// trail (berserk_accum ping-pong) composited under the live scene: skip the broken
	// overlay here (note berserk is active this frame), then drive the trail + composite
	// when the fullscreen _scratch is blitted back. Legacy keeps its original feedback path.
	if ( idStr::Icmp( shader->GetName(), "textures/decals/berserk" ) == 0 ) {
		rbBerserkFrame = true;
		return;	// _scratch stays the clean scene; the effect happens at the blit
	}
	// the fullscreen blit of the captured scene during berserk (dvMaterial == "_scratch").
	// rbBerserkFrame catches the active powerup (its overlay was seen this frame);
	// r_berserkFade (game-driven) additionally keeps the effect alive through the 2s
	// wind-down after berserk ends, when that overlay is no longer drawn.
	const float berserkFadeCvar = r_berserkFade.GetFloat();
	const bool isBerserkBlit = ( rbBerserkFrame || berserkFadeCvar > 0.0f ) && !viewDef->viewEntitys
		&& idStr::Icmp( shader->GetName(), "_scratch" ) == 0;

	// advance the feedback buffer once for this blit (before the per-stage draw so it's
	// bound for the display). rbBerserkFrame with a not-yet-updated fade cvar (SMP skew on
	// the first active frame) falls back to full strength.
	rhi::ImageHandle berserkTrail = 0;
	if ( isBerserkBlit ) {
		const float berserkFade = ( berserkFadeCvar > 0.0f ) ? berserkFadeCvar : 1.0f;
		if ( R_BackendSupportsEnhancements() ) {
			// baked-in look: 0.95 centerscale per 60fps-frame, full radial-mask feedback,
			// full-res (÷1) trail. See berserk_accum.frag for the stock-faithful math.
			berserkTrail = RB_RHI_BerserkAccum( r, viewDef, 0.95f, 1.0f, berserkFade, 1,
			                                    Sys_Milliseconds() );
		}
	}

	// DUDE hell-time / Artifact vision (D3XP FullscreenFX_Helltime): the stock effect recursively
	// zooms the previous frame into _accum (textures/smf/bloodorb{1,2,3}/ac_capture, masked by
	// bloodorb3.tga) — the same cross-frame capture that can't accumulate on the RHI (like
	// berserk's _scratch), so it shows "ghost duplicates". Suppress the broken capture/draw halves
	// and, at the final cr_draw blit, drive a ping-pong trail (RB_RHI_HelltimeAccum) + composite.
	// The material name carries the level (bloodorb1/2/3). The game's FxFader + Blendback fade the
	// effect in/out automatically, so no bridge cvar is needed. Legacy keeps its own _accum path.
	const char *hlName = shader->GetName();
	bool isHelltimeDraw = false;
	if ( !viewDef->viewEntitys && idStr::Cmpn( hlName, "textures/smf/bloodorb", 21 ) == 0
	     && hlName[21] >= '1' && hlName[21] <= '3' && hlName[22] == '/' ) {
		const int lvl = hlName[21] - '1';			// '1'/'2'/'3' -> 0/1/2
		const char *suffix = hlName + 22;			// "/ac_capture", "/cr_draw", ...
		rbHelltimeFrame = true;
		rbHelltimeLevel = lvl;
		if ( idStr::Icmp( suffix, "/cr_draw" ) == 0 ) {
			isHelltimeDraw = true;					// the display: composite the trail here
		} else {
			return;	// ac_init / ac_capture / cr_capture / ac_draw: kill the broken recursion
		}
	}
	rhi::ImageHandle helltimeTrail = 0;
	float helltimeSsX = 1.0f, helltimeSsY = 1.0f;
	if ( isHelltimeDraw && R_BackendSupportsEnhancements() ) {
		helltimeTrail = RB_RHI_HelltimeAccum( r, viewDef, rbHelltimeLevel, Sys_Milliseconds() );
		// cr_draw's stretchpic texcoords span only [0..shiftScale] (the _currentRender POT
		// convention), but the trail RT is a full-viewport 0..1 buffer. Rescale the display
		// texcoords by 1/shiftScale so the whole trail shows full-screen (centred).
		const int potW = globalImages->currentRenderImage->uploadWidth;
		const int potH = globalImages->currentRenderImage->uploadHeight;
		const int vw = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
		const int vh = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
		if ( potW > 0 ) { helltimeSsX = (float)vw / potW; }
		if ( potH > 0 ) { helltimeSsY = (float)vh / potH; }
	}

	for ( int k = 0; k < ir->surfaceStages.Num(); k++ ) {
		const rhi::StageIR &si = ir->surfaceStages[k];
		const shaderStage_t *pStage = shader->GetStage( si.stageNum );

		if ( regs[pStage->conditionRegister] == 0 ) {
			continue;
		}
		// skip ( GL_ZERO, GL_ONE ) stages, used for some alpha masks
		if ( ( pStage->drawStateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) == ( GLS_SRCBLEND_ZERO | GLS_DSTBLEND_ONE ) ) {
			continue;
		}
		if ( si.kind == rhi::SK_SKIP ) {
			continue;	// reason logged once at IR build
		}
		if ( si.kind == rhi::SK_CUSTOM_ARB ) {
			// _currentRender-sampling stages (heat haze, the RoE grabber warp)
			// are drawn in the post-process pass, after the framebuffer copy —
			// materials referencing _currentRender auto-sort to SS_POST_PROCESS
			RB_RHI_StagePolyOffsetBegin( r, pStage );
			RB_RHI_RenderCustomStage( r, viewDef, surf, pStage, si.program, mvp, vb, vertOfs, ib, idxOfs );
			RB_RHI_StagePolyOffsetEnd( r, pStage, shader );
			continue;
		}
		if ( si.kind == rhi::SK_BUILTIN_ARB ) {
			// Vulkan: stock customs through their hand-translated builtins (M5)
			RB_RHI_StagePolyOffsetBegin( r, pStage );
			RB_RHI_RenderBuiltinArbStage( r, viewDef, surf, pStage, si.program, mvp, vb, vertOfs, ib, idxOfs );
			RB_RHI_StagePolyOffsetEnd( r, pStage, shader );
			continue;
		}
		if ( si.kind == rhi::SK_TEXGEN ) {
			// fixed-function texgen (skybox / cube reflection / portal sky);
			// M5: cube images bind through DrawArgs on Vulkan (the descriptor
			// writer uses each image's own view — cube views included)
			RB_RHI_StagePolyOffsetBegin( r, pStage );
			RB_RHI_RenderTexgenStage( r, viewDef, surf, pStage, si, mvp, vb, vertOfs, ib, idxOfs );
			RB_RHI_StagePolyOffsetEnd( r, pStage, shader );
			continue;
		}

		float color[4];
		color[0] = regs[pStage->color.registers[0]];
		color[1] = regs[pStage->color.registers[1]];
		color[2] = regs[pStage->color.registers[2]];
		color[3] = regs[pStage->color.registers[3]];

		// skip stages that would draw nothing
		if ( ( pStage->drawStateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) == ( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE )
			&& color[0] <= 0 && color[1] <= 0 && color[2] <= 0 ) {
			continue;
		}
		if ( ( pStage->drawStateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) == ( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA )
			&& color[3] <= 0 ) {
			continue;
		}

		// per-stage polygon offset (privatePolygonOffset: dissolve/burning corpse,
		// some weapon stages) wraps the actual draw below — the soft-particle helper
		// or the generic stage. Restored at each exit so it never latches into the
		// next stage on Vulkan's persistent dynamic depth bias.
		RB_RHI_StagePolyOffsetBegin( r, pStage );

		// soft particles (#3878): fade this quad against captured scene depth
		// instead of drawing it as a hard billboard. The front-end flags the
		// surface + radius (GL3/Vulkan only); we only soften additive / src-alpha
		// blends, and only once _currentDepth has actually been captured.
		const int src_blend = pStage->drawStateBits & GLS_SRCBLEND_BITS;
		// "depth captured yet": uploadWidth on GL, rhiCaptured on Vulkan (a
		// demand-load can set uploadWidth there without a real capture)
		const bool depthCaptured = rhi::GetActiveBackendType() == rhi::BT_VULKAN
			? globalImages->currentDepthImage->rhiCaptured
			: globalImages->currentDepthImage->uploadWidth > 0;
		if ( ( surf->dsFlags & DSF_SOFT_PARTICLE ) && surf->particle_radius > 0.0f
			&& ( src_blend == GLS_SRCBLEND_ONE || src_blend == GLS_SRCBLEND_SRC_ALPHA )
			&& depthCaptured ) {
			if ( RB_RHI_RenderSoftParticleStage( r, viewDef, surf, pStage, regs, src_blend, color, mvp, tri, vb, vertOfs, ib, idxOfs ) ) {
				RB_RHI_StagePolyOffsetEnd( r, pStage, shader );
				continue;
			}
		}

		rhi::RenderParams parms;
		memset( &parms, 0, sizeof( parms ) );
		memcpy( parms.mvpMatrix, mvp, sizeof( parms.mvpMatrix ) );

		// texture matrix (same reg evaluation as R_SetDrawInteraction)
		if ( pStage->texture.hasMatrix ) {
			parms.diffuseMatrixS[0] = regs[pStage->texture.matrix[0][0]];
			parms.diffuseMatrixS[1] = regs[pStage->texture.matrix[0][1]];
			parms.diffuseMatrixS[2] = 0.0f;
			parms.diffuseMatrixS[3] = regs[pStage->texture.matrix[0][2]];
			parms.diffuseMatrixT[0] = regs[pStage->texture.matrix[1][0]];
			parms.diffuseMatrixT[1] = regs[pStage->texture.matrix[1][1]];
			parms.diffuseMatrixT[2] = 0.0f;
			parms.diffuseMatrixT[3] = regs[pStage->texture.matrix[1][2]];
			// keep scroll offsets from growing unbounded
			if ( parms.diffuseMatrixS[3] < -40.0f || parms.diffuseMatrixS[3] > 40.0f ) {
				parms.diffuseMatrixS[3] -= (int)parms.diffuseMatrixS[3];
			}
			if ( parms.diffuseMatrixT[3] < -40.0f || parms.diffuseMatrixT[3] > 40.0f ) {
				parms.diffuseMatrixT[3] -= (int)parms.diffuseMatrixT[3];
			}
		} else {
			parms.diffuseMatrixS[0] = 1.0f;
			parms.diffuseMatrixT[1] = 1.0f;
		}

		// berserk vision display (berserk.frag): show the accumulated feedback buffer when
		// present (hasTrail → .z), else the plain captured scene. The stage's texture matrix
		// (the _scratch V-flip) still drives var_TexCoord for both.
		if ( isBerserkBlit ) {
			parms.localParam0[2] = berserkTrail ? 1.0f : 0.0f;
		}
		// hell-time display: same `berserk` display shader — show the accumulated trail (which
		// already folds the sharp centre + the edge zoom-trail) when present, else the plain scene.
		if ( isHelltimeDraw ) {
			parms.localParam0[2] = helltimeTrail ? 1.0f : 0.0f;
			if ( helltimeTrail ) {
				// map cr_draw's [0..shiftScale] stretchpic texcoords onto the full 0..1 trail RT
				// (see the shiftScale computation above). Only when a trail exists; the no-trail
				// fallback keeps cr_draw's own texcoords so it samples _currentRender correctly.
				parms.diffuseMatrixS[0] = ( helltimeSsX > 0.0f ) ? 1.0f / helltimeSsX : 1.0f;
				parms.diffuseMatrixS[1] = 0.0f;
				parms.diffuseMatrixS[3] = 0.0f;
				parms.diffuseMatrixT[0] = 0.0f;
				parms.diffuseMatrixT[1] = ( helltimeSsY > 0.0f ) ? 1.0f / helltimeSsY : 1.0f;
				parms.diffuseMatrixT[3] = 0.0f;
			}
		}

		// vertex color mode: var_Color = (attr_Color*modulate + add) * u_color
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
		parms.color[0] = color[0];
		parms.color[1] = color[1];
		parms.color[2] = color[2];
		parms.color[3] = color[3];

		// DUDE C-lite HDR overbright (docs/hdr-pipeline.md Phase C): scale additive
		// self-illum stages (blend add — lamps, monitor screens, fire, glares) so they
		// exceed 1.0 in the float scene buffer, giving the tonemap curve real highlight
		// range and eye-adaptation bright anchors to measure. Only while the HDR float
		// target is bound (on the 8-bit path >1 would just clip to white) AND a tonemap
		// curve is active (r_hdrTonemap>=1, like r_hdrExposure — so the faithful mode-0
		// look is never touched), RGB only (alpha untouched), and only for the ONE|ONE
		// additive blend. The >1 guard makes r_hdrOverbright 1 skip entirely, so it stays
		// bit-identical whenever it is off. Boosts the old-style generic stages here; soft
		// particles, custom-ARB and texgen stages took an earlier branch and stay unboosted.
		if ( rbHdrActiveThisFrame && r_hdrTonemap.GetInteger() >= 1 && r_hdrOverbright.GetFloat() > 1.0f
		     && ( pStage->drawStateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) == ( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE ) ) {
			const float ob = r_hdrOverbright.GetFloat();
			parms.color[0] *= ob;
			parms.color[1] *= ob;
			parms.color[2] *= ob;
		}

		// fixed-function alpha test bits -> in-shader test (fail if a < ref)
		switch ( pStage->drawStateBits & GLS_ATEST_BITS ) {
		case GLS_ATEST_GE_128:
			parms.alphaTest[0] = 0.5f;
			parms.alphaTest[1] = 1.0f;
			break;
		case GLS_ATEST_EQ_255:
			parms.alphaTest[0] = 1.0f;
			parms.alphaTest[1] = 1.0f;
			break;
		case GLS_ATEST_LT_128:
			RB_RHI_LogOnce( "GLS_ATEST_LT_128 stages" );
			break;
		default:
			break;
		}

		// DUDE tessellation (docs/tessellation.md): this pass draws a material's ambient/emissive
		// stages. Two cases:
		//  - Roadmap B deform-once: a classifier-approved BODY surface was pre-deformed into
		//    tri->tessDeformVB this frame -- draw THAT (the same expanded, uniform-L buffer the zfill
		//    prepass sealed from) so a DEPTHFUNC_EQUAL emissive stage (e.g. the imp burning-corpse
		//    glow) matches the deformed depth instead of failing EQUAL against it. The deform decision
		//    uses TessellateSurf(surf,FALSE) -- NOT the forBlendPass=true call, whose all-additive
		//    exclusion (RhiWorld.cpp) would reject the all-additive glow and keep the ember dropped.
		//    Gating additionally on tessDeformVB!=0 means only a surface actually deform-dispatched (the
		//    body) takes it; a view-oriented sprite/particle glow and the blood decal have none.
		//  - Otherwise (no deform buffer): fixed-function PN-tessellate via generic.tese (forBlendPass
		//    =true). generic.tese ALSO runs dudeTessDisplace off the surface bump (bound on unit 1 below),
		//    so an on-body EQUAL blend stage — the burning-corpse ember, a self-illum add, a mod emissive —
		//    lands on the displaced zfill depth instead of failing EQUAL wherever the body is pushed out
		//    (the bug that left the ember only on flat patches like the soles of the feet). A blood-overlay
		//    decal is a SEPARATE surface with no bump stage, so it binds flatNormalMap => relief 0 => it
		//    merely follows the PN base with no push, as before. (A PURELY additive glow — RoE soul aura —
		//    is excluded from tessellation entirely by RB_RHI_TessellateSurf(forBlendPass), so it never
		//    reaches here; only the deform-once buffer can carry it, hence the FALSE probe above.)
		bool useDeform = false;
		bool tess = RB_RHI_TessellateSurf( surf, true );
		if ( RB_RHI_TessellateSurf( surf, false ) && tri->tessDeformVB && tri->tessDeformIB ) {
			useDeform = true;
			tess = false;			// deform-once supersedes fixed-function tess for the body
		}
		idImage *tessBump = NULL;
		if ( tess ) {
			RB_RHI_SetTessParms( parms );
			// Bind the surface bump on unit 1 so generic.tese displaces this on-body blend
			// stage (the imp/zombie burning-corpse ember, a self-illum add, a mod emissive)
			// with the SAME bump texel / seam / strength as the zfill prepass it must match
			// at depth-EQUAL. Without it the stage PN-follows but does NOT displace, so it
			// fails EQUAL everywhere the body is displaced and survives only on flat patches
			// (the soles of the feet). Bump-less surfaces (blood decals) get flatNormalMap =>
			// relief 0 => no push, unchanged. u_bumpMatrix* is filled into parms here too.
			tessBump = RB_RHI_TessBumpForZfill( surf, parms );
		}

		rhi::BufferHandle ub;
		int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

		rhi::ImageHandle stageImage = RB_RHI_BindStageImage( pStage, regs, viewDef );
		if ( stageImage == RHI_SKIP_STAGE_IMAGE ) {
			RB_RHI_LogOnce( "VK: stage sampling a never-captured _currentRender/_scratch skipped" );
			RB_RHI_StagePolyOffsetEnd( r, pStage, shader );
			continue;
		}

		// 2D views: legacy disables depth test entirely; equivalent here is
		// depth-always + no depth writes
		rhi::PipelineDesc pd;
		pd.stateBits = ( pStage->drawStateBits & ~GLS_ATEST_BITS );
		if ( !viewDef->viewEntitys ) {
			// 2D views (menu/console/HUD/loading): force depth-always. Clear any
			// authored depth-func first -- a stage that carries GLS_DEPTHFUNC_EQUAL
			// (e.g. the menu idlogo cinematic) would otherwise be left with BOTH
			// the EQUAL and ALWAYS bits set, and the compare-op translation picks
			// EQUAL (checked first), discarding every fragment against the 2D depth
			// buffer -> the surface renders black. (In 3D the EQUAL is legit: the
			// depth prepass gives it something to match, which is why the same
			// videoMap materials play fine in-game.)
			pd.stateBits = ( pd.stateBits & ~( GLS_DEPTHFUNC_EQUAL | GLS_DEPTHFUNC_ALWAYS ) )
				| GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK;
		}
		// hell-time display: cr_draw's stock blend is gl_dst_alpha, but the trail already folds
		// the sharp centre + edge zoom-trail, so draw it as an opaque replace of the clean-scene
		// framebuffer — the game's Blendback then cross-fades it in/out by the fader alpha.
		if ( isHelltimeDraw ) {
			pd.stateBits = GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK;
		}
		// berserk / hell-time vision: blit the accumulated feedback buffer through the `berserk`
		// display shader instead of a plain stretch — the zoom-trail lives in that buffer.
		pd.shader = ( isBerserkBlit || isHelltimeDraw ) ? r->LoadShader( "berserk" ) : si.program;
		pd.vertexLayout = rhi::VL_DRAWVERT;
		pd.cullType = RB_RHI_CullFor( viewDef, shader->GetCullType() );
		pd.tessellate = tess;
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
		da.textures[0] = stageImage;	// Vulkan path; 0 on GL3 (binds went via idImage)
		if ( tess ) {
			// unit 1 = displacement bump for generic.tese (Vulkan-only; GL3 never
			// tessellates). tessBump is flatNormalMap when the material has no bump.
			idImage *bumpImg = tessBump ? tessBump : globalImages->flatNormalMap;
			bumpImg->Bind();		// upload trigger under Vulkan
			da.textures[1] = bumpImg->rhiHandle ? bumpImg->rhiHandle
			                                    : globalImages->flatNormalMap->rhiHandle;
		}
		if ( isBerserkBlit ) {
			// unit 1 = the ghost trail (Vulkan needs it in DrawArgs; GL already bound it
			// in RB_RHI_BerserkAccum). Without a trail, bind _scratch as a placeholder so
			// the u_trail sampler is valid on Vulkan — the shader ignores it (hasTrail=0).
			da.textures[1] = berserkTrail ? berserkTrail : stageImage;
		}
		if ( isHelltimeDraw ) {
			// unit 1 = the hell-time trail (VK via DrawArgs; GL bound in RB_RHI_HelltimeAccum).
			// Placeholder = the stage's _currentRender when there's no trail (hasTrail=0).
			da.textures[1] = helltimeTrail ? helltimeTrail : stageImage;
		}
		r->Draw( da );

		backEnd.pc.c_drawElements++;
		backEnd.pc.c_drawIndexes += tri->numIndexes;
		backEnd.pc.c_drawVertexes += tri->numVerts;

		RB_RHI_StagePolyOffsetEnd( r, pStage, shader );
	}

	if ( shader->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
		if ( qglDisable != NULL ) {
			qglDisable( GL_POLYGON_OFFSET_FILL );
		}
		r->SetPolygonOffset( false, 0.0f, 0.0f );
	}
	if ( surf->space->weaponDepthHack || ( surf->space->modelDepthHack != 0.0f && !( surf->dsFlags & DSF_SOFT_PARTICLE ) ) ) {
		RB_LeaveDepthHack();
	}
}

/*
=============
RB_RHI_DrawView
=============
*/
static void RB_RHI_DrawView( rhi::RHI *r, viewDef_t *viewDef ) {
	backEnd.viewDef = viewDef;	// engine helpers (cinematics, counters) read this

	// window clipping, matching RB_BeginDrawingView
	r->SetViewport( tr.viewportOffset[0] + viewDef->viewport.x1,
	                tr.viewportOffset[1] + viewDef->viewport.y1,
	                viewDef->viewport.x2 + 1 - viewDef->viewport.x1,
	                viewDef->viewport.y2 + 1 - viewDef->viewport.y1 );
	r->SetScissor( tr.viewportOffset[0] + viewDef->viewport.x1 + viewDef->scissor.x1,
	               tr.viewportOffset[1] + viewDef->viewport.y1 + viewDef->scissor.y1,
	               viewDef->scissor.x2 + 1 - viewDef->scissor.x1,
	               viewDef->scissor.y2 + 1 - viewDef->scissor.y1 );

	if ( viewDef->viewEntitys ) {
		// 3D world view: clear depth/stencil like RB_BeginDrawingView (color
		// is never cleared — the r_clear pass at frame start handles that),
		// then depth prepass + stencil shadows + interactions
		rhi::ClearArgs clear;
		clear.color = false;
		clear.depth = true;
		clear.stencil = true;
		clear.rgba[0] = clear.rgba[1] = clear.rgba[2] = 0.0f; clear.rgba[3] = 1.0f;
		clear.stencilValue = (unsigned char)( 1 << ( glConfig.stencilBits - 1 ) );
		r->BeginPass( &clear );

		RB_RHI_DrawWorld( r, viewDef );
	} else {
		// 2D view (menu/console/HUD/loading): draw over existing contents
		r->BeginPass( NULL );
	}

	// ambient/emissive shader passes (both view types); for 2D views the GL
	// scissor was just set to the view rect, keep the tracking in sync
	if ( !viewDef->viewEntitys ) {
		backEnd.currentScissor = viewDef->scissor;
	}
	const viewEntity_t *currentSpace = NULL;
	float mvp[16];
	backEnd.currentRenderCopied = false;
	backEnd.smokeBackgroundCaptured = false;

	// non-light-dependent shading. Post-process-sort surfaces (which sample
	// _currentRender) are deferred until after fog, matching RB_STD_DrawView;
	// the drawSurfs are sort-ordered so the first one ends the ambient run.
	// DUDE SSR (docs/ssr.md) composites at the translucent split: once the
	// opaque/emissive/decal surfaces (sort <= SS_DECAL) are down, reflections
	// are added before glass and particles draw over them.
	drawSurf_t **drawSurfs = (drawSurf_t **)&viewDef->drawSurfs[0];
	int i;
	bool ssrDone = !viewDef->viewEntitys || !r_ssr.GetBool();
	for ( i = 0; i < viewDef->numDrawSurfs; i++ ) {
		if ( drawSurfs[i]->material->SuppressInSubview() ) {
			continue;
		}
		if ( viewDef->viewEntitys && drawSurfs[i]->material->GetSort() >= SS_POST_PROCESS ) {
			break;
		}
		if ( !ssrDone && drawSurfs[i]->material->GetSort() > SS_DECAL ) {
			ssrDone = true;
			RB_RHI_ScreenSpaceReflections( r, viewDef );
		}
		RB_RHI_RenderShaderPasses( r, viewDef, drawSurfs[i], currentSpace, mvp );
	}
	if ( !ssrDone ) {
		RB_RHI_ScreenSpaceReflections( r, viewDef );	// view had no translucent surfaces
	}

	// fog and blend lights
	if ( viewDef->viewEntitys ) {
		RB_RHI_FogAllLights( r, viewDef );
	}

	// post-process-sort surfaces, now that fog is down; copy _currentRender
	// first (only in a 3D view) so the SS_POST_PROCESS stages can sample it
	if ( i < viewDef->numDrawSurfs && !r_skipPostProcess.GetBool() ) {
		if ( viewDef->viewEntitys ) {
			RB_RHI_CopyCurrentRender( viewDef );
			backEnd.currentRenderCopied = true;
		}
		currentSpace = NULL;	// force an mvp reload for the first deferred surface
		for ( ; i < viewDef->numDrawSurfs; i++ ) {
			if ( drawSurfs[i]->material->SuppressInSubview() ) {
				continue;
			}
			RB_RHI_RenderShaderPasses( r, viewDef, drawSurfs[i], currentSpace, mvp );
		}
	}

	// DUDE film grain / chromatic aberration over the finished 3D view, before
	// any 2D/GUI (improvements menu, default off). Fullscreen primary view only:
	// subviews (mirrors, camera monitors) are composited into it and grained
	// with it, and cropped GUI-renderDef models (the menu planet) aren't the
	// scene — same fullscreen-and-not-subview test RhiWorld uses.
	const bool fullscreenView = viewDef->viewport.x1 <= 0 && viewDef->viewport.y1 <= 0
		&& viewDef->viewport.x2 >= glConfig.vidWidth - 1
		&& viewDef->viewport.y2 >= glConfig.vidHeight - 1;
	if ( viewDef->viewEntitys && !viewDef->isSubview && fullscreenView ) {
		// The GL per-view post chain (AA / film grain / chromatic aberration) is GL-only
		// here; Vulkan folds grain/chroma into the scene resolve instead (RB_RHI_HdrResolve,
		// over the HDR float buffer or the RGBA8 off-HDR post target — see RB_RHI_HdrBeginFrame).
		if ( rhi::GetActiveBackendType() != rhi::BT_VULKAN && !rbHdrActiveThisFrame ) {
			// post-resolve antialiasing (FXAA) first, so film grain / chromatic
			// aberration are applied on top of the resolved image rather than smoothed
			RB_RHI_AAPass( r, viewDef );
			RB_RHI_PostProcess( r, viewDef );
		}
		// r_ssaoDebug: overlay the AO buffer on top of the finished view. RHI fullscreen
		// draw, so it works on both backends (the SSAO buffer is produced on Vulkan too).
		RB_RHI_SSAODebugOverlay( r, viewDef );
		// DUDE weapon-reload depth-of-field (r_dof): after the full scene incl. the
		// weapon, on both backends and under HDR. Zero cost unless a reload is easing
		// r_weaponReloadFocus above 0 (idPlayerView writes it).
		RB_RHI_DepthOfField( r, viewDef );
	}

	// debug visualization (r_showTris, r_showNormals, debug lines/polygons, …)
	// — Chunk G. Renders through idImmediateMode's core path; each sub-view
	// early-outs on its own cvar, so this is free when nothing is enabled.
	// (Surface-indexed views like r_showTris await the core RB_DrawElements
	// path; the idImmediateMode-based views work now — on Vulkan too, M6, where
	// RB_RenderDebugTools runs the qgl-safe subset.)
	if ( viewDef->viewEntitys ) {
		RB_RenderDebugTools( (drawSurf_t **)&viewDef->drawSurfs[0], viewDef->numDrawSurfs );
	}

	r->EndPass();
}

/*
=============
RB_RHI_ExecuteBackEndCommands

RHI-based replacement for RB_ExecuteBackEndCommands (legacy path untouched).
Backend-neutral: drives whichever rhi::RHI is active (GL3 today, Vulkan later).
=============
*/
void RB_RHI_ExecuteBackEndCommands( const emptyCommand_t *cmds ) {
	rhi::RHI *r = rhi::GetRHI();

	// Phase 4 M3 (docs/vulkan-backend.md): the Vulkan backend draws 2D views
	// and, for world views, the depth prepass + ambient shader passes
	// (RB_RHI_DrawWorld stops before lights/shadows until M4). The GL-only
	// helper passes (HDR routing, gamma, capture, ImGui-on-GL) stay off until
	// their milestones.
	const bool vkMode = ( rhi::GetActiveBackendType() == rhi::BT_VULKAN );

	r->BeginFrame( glConfig.vidWidth, glConfig.vidHeight );

	// Phase 2 GPU skinning: dispatch all recorded MD5 skin jobs now, in the pre-scene window
	// (frame cb open, no render pass yet) so the compute->vertex barrier lands before any draw.
	RB_RHI_FlushSkinJobs();
	// Roadmap B: deform-once tessellation dispatches, immediately AFTER the skin jobs so a deform that
	// reads a surface's gpuSkinVB is ordered by the skin dispatch's trailing COMPUTE->COMPUTE barrier.
	RB_RHI_FlushTessJobs();

	rbBerserkFrame = false;	// set when the berserk material is seen (crop overlay), read at the _scratch blit
	rbHelltimeFrame = false;	// set when a bloodorbN hell-time material is seen, read at its cr_draw blit

	// route the whole frame into the RGBA16F scene buffer (r_hdr) before any clear
	// or view command lands; a no-op that stays on the backbuffer when r_hdr is
	// off or the frame is worldless (menu/GUI/cinematic — see RB_RHI_HdrBeginFrame).
	// M7: live on Vulkan too, via the color render-target family in VulkanBackend.
	RB_RHI_HdrBeginFrame( r, cmds );
	if ( vkMode && r_hdr.GetBool() && RB_RHI_HdrCaptureActive() ) {
		RB_RHI_LogOnce( "VK: HDR scene buffer active (r_hdr) - RGBA16F frame target" );
	}

	for ( ; cmds; cmds = (const emptyCommand_t *)cmds->next ) {
		switch ( cmds->commandId ) {
		case RC_NOP:
			break;
		case RC_DRAW_VIEW:
			RB_RHI_DrawView( r, ((const drawSurfsCommand_t *)cmds)->viewDef );
			break;
		case RC_SET_BUFFER: {
			// single (back) buffer only; keep frame counter + clear semantics
			// exactly matching RB_SetBuffer (r_clear defaults to 2 = black)
			const setBufferCommand_t *cmd = (const setBufferCommand_t *)cmds;
			backEnd.frameCount = cmd->frameCount;
			if ( r_clear.GetFloat() || idStr::Length( r_clear.GetString() ) != 1
				|| r_lockSurfaces.GetBool() || r_singleArea.GetBool() || r_showOverDraw.GetBool() ) {
				rhi::ClearArgs clear;
				clear.color = true;
				clear.depth = clear.stencil = false;
				clear.stencilValue = 0;
				float c[3];
				if ( sscanf( r_clear.GetString(), "%f %f %f", &c[0], &c[1], &c[2] ) == 3 ) {
					clear.rgba[0] = c[0]; clear.rgba[1] = c[1]; clear.rgba[2] = c[2];
				} else if ( r_clear.GetInteger() == 2 ) {
					clear.rgba[0] = clear.rgba[1] = clear.rgba[2] = 0.0f;
				} else if ( r_showOverDraw.GetBool() ) {
					clear.rgba[0] = clear.rgba[1] = clear.rgba[2] = 1.0f;
				} else {
					clear.rgba[0] = 0.4f; clear.rgba[1] = 0.0f; clear.rgba[2] = 0.25f;
				}
				clear.rgba[3] = 1.0f;
				r->BeginPass( &clear );
				r->EndPass();
			}
			break;
		}
		case RC_COPY_RENDER: {
			// explicit _currentRender/_currentDepth copies (e.g. mirror/xray
			// setup); direct-GL idImage copy like the legacy RB_CopyRender
			const copyRenderCommand_t *cmd = (const copyRenderCommand_t *)cmds;
			// M5: on Vulkan CopyFramebuffer routes to the RHI capture copy
			if ( cmd->image && !r_skipCopyTexture.GetBool() ) {
				cmd->image->CopyFramebuffer( cmd->x, cmd->y, cmd->imageWidth, cmd->imageHeight, false );
			}
			break;
		}
		case RC_SWAP_BUFFERS:
			if ( vkMode ) {
				// M7: resolve the scene post-target back onto the scene image, then
				// present. When HDR is on this is the RGBA16F buffer (+FXAA/SMAA); when
				// HDR is off but grain/chroma/gamma want it, it's the RGBA8 off-HDR post
				// target; a no-op (target never bound) when nothing wants it. Folds in
				// film grain + chroma + r_gammaInShader gamma/brightness (the VK backend
				// has no separate LDR gamma tail), and must run before EndFrame blits the
				// scene image to the swapchain.
				RB_RHI_HdrResolve( r );
				// present happens in the backend's EndFrame below (swap capture is
				// serviced after EndFrame). ImGui renders through the backend:
				// EndFrame here runs ImGui::Render and hands the draw data over,
				// drawn into the swapchain image between the scene blit and present.
				D3::ImGuiHooks::EndFrame();
				break;
			}
			// resolve the RGBA16F scene buffer back onto the backbuffer (r_hdr);
			// no-op when HDR is off. Must precede gamma + capture so both operate
			// on the finished LDR image on the backbuffer.
			RB_RHI_HdrResolve( r );
			// correct the finished frame for r_gamma / r_brightness in-shader
			// (core has no fixed-function/hardware gamma); before the capture
			// and the ImGui overlay so screenshots match what's on screen
			RB_RHI_GammaBrightness( r );
			if ( rbCaptureDest ) {
				// back buffer still holds the finished frame; front-buffer
				// reads after swap are undefined under compositors
				qglReadPixels( 0, 0, glConfig.vidWidth, glConfig.vidHeight,
				               GL_RGB, GL_UNSIGNED_BYTE, rbCaptureDest );
				rbCaptureDest = NULL;
			}
			// draw the ImGui menus (F10 DUDE settings) on top of the frame —
			// the legacy path does this in RB_SwapBuffers, which the core
			// executor bypasses. Drawn after the capture so it stays out of
			// screenshots, matching the legacy behaviour.
			D3::ImGuiHooks::EndFrame();
			GLimp_SwapBuffers();
			break;
		default:
			common->Error( "RB_RHI_ExecuteBackEndCommands: bad commandId" );
			break;
		}
	}

	r->EndFrame();

	// M6: Vulkan swap capture (screenshots / tiled captures) — the GL path
	// reads the back buffer in the RC_SWAP_BUFFERS case above; here the scene
	// image still holds the completed frame after present, so read it back now
	if ( vkMode && rbCaptureDest ) {
		if ( !r->ReadPixelsRGB( rbCaptureDest, 0, 0, glConfig.vidWidth, glConfig.vidHeight ) ) {
			RB_RHI_LogOnce( "VK: swap capture readback failed" );
		}
		rbCaptureDest = NULL;
	}
}
