/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#if defined(__ppc__) && defined(__APPLE__)
#include <vecLib/vecLib.h>
#endif
#if defined(__GNUC__) && defined(__SSE2__)
#include <xmmintrin.h>
#endif

#include "sys/platform.h"
#include <algorithm>		// DUDE: std::sort for R_SortDrawSurfs
#include "framework/Session.h"
#include "renderer/RenderWorld_local.h"
#include "renderer/ModelManager.h"
#include "renderer/VertexCache.h"		// R3.5 r_rtAnimBlasTest: index-buffer handle from tri->indexCache
#include "framework/DeclSkin.h"			// rtEmissiveProbe: the skin name

#include "renderer/tr_local.h"
#include "renderer/rhi/RHI.h"

/*
===========================================================================

DUDE: the RT emissive triangle table (docs/rtx-emissive-triangles.md E1 / E2), moved out of
tr_main.cpp as one block (the structural pass): the table's statics, gather, rows, movers,
adopt/append, labels, probe and debug geometry. The RT scene in tr_main.cpp drives it through
R_RtEmissiveUpdate / R_RtEmissiveForget / R_RtEmissiveFree (tr_local.h); everything else here
is the table's own.

===========================================================================
*/
/*
====================================================================================================
RT emissive triangle table (docs/rtx-emissive-triangles.md E1)

The data foundation of the experimental emissive-lighting mode (docs/rtx-experimental-lighting.md):
a device table of every EMISSIVE triangle in the loaded map - a triangle of a surface whose material
has an additive glow stage (neon tube, light tile, LED panel, lava) - with the flux and geometry an
area-light sampler needs (E2 draws a triangle proportional to flux through the CDF, a point on it,
reads the glow texture at that point's st, and traces one visibility ray against the existing TLAS).

Why its own gather: the STRONGEST emitters are not in the RT scene. A pure light-source material
(additive only) classifies MC_TRANSLUCENT -> noShadows (Material.cpp), so R_RtSurfCasts rejects it
and it is in no BLAS and no geo-table row; only glow-on-opaque surfaces (console readouts) are
captured there, and even those only as per-pixel glow at a ray hit, never as a light. An emitter
needs no BLAS (the opaque world provides the occluders for E2's visibility ray), so this gather
walks EVERY surface of the worldspawn areas and the DM_STATIC entity models, minus the caster gate,
plus the emissive-stage test. Screens (GUI / cinematic / render-to-texture surfaces) are excluded:
they keep the legacy fill lights (tr_light.cpp R_QueueEmissiveLight), and their image averages are
the uncomputed white default. GPU-skinned monsters have no CPU positions - skipped (negligible).

flux = world-space area x mean glow luminance (idImage::averageColor, gamma -> linear) x the stage's
constant colour luminance x r_rtEmissiveScale (the D2 per-material table lands later). A register-
driven (pulsing / flickering) stage colour is snapshotted at build (floored, so an OFF phase does not
drop the emitter) and flagged ANIMATED: the flux only ranks the emitter, E2 reads the live colour.

Lifecycle: built once per map (static world + static entities at their load pose; E1e refreshes
movers), beside the persistent RT scene (R_RtWorldUpdate), rebuilt when the cvars change, freed with
the scene. r_rtEmissiveTable 0 = nothing built. Nothing consumes the table until E2, so on or off the
picture is identical; r_rtEmissiveTableDebug draws it (RhiWorld.cpp RB_RHI_RtEmissiveDebugOverlay).
====================================================================================================
*/
// one table entry, 64 B, mirrors rt_emissive.glsl RtEmissiveTri
struct rtEmissiveTriGPU_t {
	float			p0[3];	float			flux;		// world-space vertices; flux = area x luminance x scale
	float			p1[3];	unsigned int	emit;		// bits 0-15 the glow texture's bindless slot + 1 (idImage::rhiHandle), 16-31 RT_EMIT_* flags
	float			p2[3];	float			scale;		// material intensity scale (r_rtEmissiveScale x the D2 table when it lands)
	unsigned int	st0, st1, st2;						// per-vertex st as half2 (packHalf2x16 layout: s low, t high)
	unsigned int	tint;								// the stage's constant colour as RGBA8 (R low byte); alpha 255
	unsigned int	meanRG, meanB;						// the emitter's MEAN linear radiance (glow average x stage colour), half floats: E2 lights with
														// this flat colour (a uniform emitter; the texel pattern is variance, not light)
	unsigned int	reach;								// unused since the census went (2026-10-31): 0, kept for the 64-byte layout (rt_emissive.glsl)
	unsigned int	lightLum;							// unused likewise: 0
};
static const int	RT_EMISSIVE_HEADER_BYTES = 16;		// uint count, float totalFlux, uint cdfOffsetBytes, float meanFlux
static const int	RT_EMISSIVE_MAX_TRIS     = 1 << 18;	// 262144 x 64 B = 16 MB table ceiling (+1 MB CDF); a census overflow is reported
#define RT_EMIT_FLAG_ANIMATED	( 1u << 16 )			// stage colour is register-driven: flux is a snapshot, read the live colour
#define RT_EMIT_FLAG_ENTITY		( 1u << 17 )			// from a static entity model (else worldspawn)
#define RT_EMIT_FLAG_TWOSIDED	( 1u << 18 )			// material creates back sides: its reversed twin is in the table too (both faces emit)
// bit 19 was RT_EMIT_FLAG_INLIGHT (the census, removed 2026-10-31)
#define RT_EMIT_FLAG_DARK		( 1u << 20 )			// the fixture is switched OFF in its current state (skin / parm): zero flux, kept in place
// bit 21 was RT_EMIT_FLAG_DIFFUSEGLOW (flare inference, removed 2026-10-31)
#define RT_EMIT_FLAG_PROFILE_SHIFT 22					// bits 22-23: the emission profile, always 0 in the table now - the map's authored profile rides on the row (rt_emissive.glsl)
// bit 24 was RT_EMIT_FLAG_INDICATOR (emitters.cfg, removed 2026-10-31)

static idList<rtEmissiveTriGPU_t>	s_rtEmissiveTris;			// CPU copy of the table (the debug geometry is built from it)
static rhi::BufferHandle			s_rtEmissiveBuf = 0;		// header + tris + CDF
static unsigned long long			s_rtEmissiveAddr = 0;
static float						s_rtEmissiveTotalFlux = 0.0f;
static idStr						s_rtEmissiveMap;			// the map the table was built for (the once-per-map latch; set even when empty)
static rhi::BufferHandle			s_rtEmissiveDbgVB = 0, s_rtEmissiveDbgIB = 0;	// lazily built for the debug overlay
static int							s_rtEmissiveDbgIndexes = 0;
// E1e movers: every entity-sourced run of the table remembers its entity, its model-space vertices and the model
// matrix it was baked with, so a func_mover (door, lift, platform) that moves gets its triangles' WORLD positions
// re-baked and its table slice (+ debug verts) rewritten in place. Area - hence flux and the CDF - is invariant
// under the rigid move, so only the positions change.
struct rtEmissiveGroup_t {
	int					entityIndex;
	const idRenderModel *model;
	int					firstTri, numTris, firstLocalVert;
	float				lastMatrix[16];
	bool				reported;
	// the glow STATE the slice was baked with (vanilla switches a fixture off by a skin swap, a custom material or a
	// shader parm, e.g. parm7): a change re-gathers the entity in place
	const idDeclSkin	*skin;
	const idMaterial	*customShader;
	float				shaderParms[MAX_ENTITY_SHADER_PARMS];
	bool				dark;			// the current state glows nowhere: the slice is kept at zero flux (never sampled)
	bool				regather;		// DUDE: re-gather on the next refresh whatever the state compare says (the entity
										// came back: in its own slot, or adopted from another - R_RtEmissiveScanNewEntities)
};
static idList<rtEmissiveGroup_t>	s_rtEmissiveGroups;
static idList<idVec3>				s_rtEmissiveLocalVerts;		// 3 per entity-sourced triangle (model space)
static int							s_rtEmissiveMovedGroups = 0;	// movers re-baked since the build (census / debugging)
// E1d labels: one anchor per emissive SURFACE (a run of table triangles of one material), named in the overlay
struct rtEmissiveLabel_t { const idMaterial *mat; int firstTri, numTris; };
static idList<rtEmissiveLabel_t>		s_rtEmissiveLabels;
// per table triangle: its index in GATHER order. The fixture split reorders an entity's slice so each fixture is
// contiguous; the state re-gather rebuilds the slice in gather order and maps it back through this (world triangles
// are never re-gathered: identity)
static idList<int>						s_rtEmissiveTriSrc;
static idList<rtEmissiveScreenLabel_t>	s_rtEmissiveScreenLabels;		// this frame's projected, culled labels

// IEEE half, round-toward-zero (the same conversion RhiWorld.cpp's light buffer uses); packHalf2x16 layout
static unsigned int R_RtF32toF16( float f ) {
	unsigned int x;
	memcpy( &x, &f, sizeof( x ) );
	const unsigned int sign = ( x >> 16 ) & 0x8000u;
	const int exp = (int)( ( x >> 23 ) & 0xFFu ) - 127 + 15;
	if ( exp <= 0 ) { return sign; }
	if ( exp >= 31 ) { return sign | 0x7C00u; }
	return sign | ( (unsigned int)exp << 10 ) | ( ( x & 0x7FFFFFu ) >> 13 );
}
static float R_RtF16toF32( unsigned int h ) {
	const unsigned int sign = ( h & 0x8000u ) << 16;
	const unsigned int exp  = ( h >> 10 ) & 0x1Fu;
	const unsigned int man  = h & 0x3FFu;
	unsigned int x;
	if ( exp == 0 ) { x = sign; }									// zero / denormal -> zero
	else if ( exp == 31 ) { x = sign | 0x7F800000u | ( man << 13 ); }
	else { x = sign | ( ( exp - 15 + 127 ) << 23 ) | ( man << 13 ); }
	float f;
	memcpy( &f, &x, sizeof( f ) );
	return f;
}
static unsigned int R_RtPackHalf2( float a, float b ) { return R_RtF32toF16( a ) | ( R_RtF32toF16( b ) << 16 ); }
static unsigned int R_RtPackRGBA8( const float rgb[3] ) {
	unsigned int c = 0xFF000000u;
	for ( int k = 0; k < 3; k++ ) {
		c |= (unsigned int)( idMath::ClampFloat( 0.0f, 1.0f, rgb[k] ) * 255.0f + 0.5f ) << ( k * 8 );
	}
	return c;
}
static float R_RtSrgbToLinear( float c ) {		// the image average is in the texture's gamma space
	c = idMath::ClampFloat( 0.0f, 1.0f, c );
	return ( c <= 0.04045f ) ? c / 12.92f : idMath::Pow( ( c + 0.055f ) / 1.055f, 2.4f );
}
static float R_RtLuminance( const float rgb[3] ) { return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2]; }

// A "screen" the table leaves to the legacy fill lights: a GUI surface, or a video / render-to-texture stage (census only)
static bool R_RtEmissiveIsScreen( const idMaterial *mat ) {
	if ( mat == NULL ) {
		return false;
	}
	if ( mat->HasGui() ) {
		return true;
	}
	for ( int i = 0; i < mat->GetNumStages(); i++ ) {
		if ( mat->GetStage( i )->texture.cinematic != NULL || mat->GetStage( i )->texture.dynamic != DI_STATIC ) {
			return true;
		}
	}
	return false;
}

// The additive stage that makes a material an emitter for the table, or NULL. Independent of R_RtSurfCasts
// (coverage / shadow flags). The SAME additive test as R_RtMaterialEmitIndex (blend add: src ONE, dst ONE; the first
// such stage is the dominant glow), plus the screen exclusions: a GUI material, a video (cinematic) stage, a
// render-to-texture (dynamic) stage, a generated image (_scratch & co.) or a missing (defaulted) image is NOT a
// table emitter - the GUI/video screens keep the legacy fill lights, and such images never went through the
// averaging path (averageColor is the white default).
static idList<const idImage *>	s_rtEmissiveNotResident;
static idList<bool>				s_rtEmissiveFlareHidden;	// per entity index: a flare sprite sitting on an emitter (r_rtEmissiveHideFlares skips its draw)
static int						s_rtEmissiveFlareCount = 0;	// glow images that were not on the GPU at build time: a later residency rebuilds
static idList<const idRenderModel *>	s_rtEmissiveSeenModel;	// per entity index: the model the build examined there (NULL = empty / not gatherable then);
														// the periodic scan only looks at slots whose model CHANGED since - an entity the build
														// examined and found dark must not read as "new" every 90 frames (a rebuild each time:
														// the hitching of 2026-10-20)
static int						s_rtEmissiveLastRebuildMs = -100000;	// Sys_Milliseconds of the last build: scan / state rebuilds are throttled
// DUDE (2026-10-27): the build's long report (per-material census, flare inference, the light census) prints for the
// first build of a map, after a table cvar changed, or with r_rtEmissiveTableDebug; the rebuilds a map makes on its
// own print one timed line instead - writing ~250 console + log lines was part of each rebuild's hitch
static bool						s_rtEmissiveReport = true;
static idStr					s_rtEmissiveReportedMap;
static void R_RtEmissiveReport( const char *fmt, ... ) id_attribute((format(printf,1,2)));
static void R_RtEmissiveReport( const char *fmt, ... ) {
	if ( !s_rtEmissiveReport ) {
		return;
	}
	char text[1024];
	va_list argptr;
	va_start( argptr, fmt );
	idStr::vsnPrintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );
	common->Printf( "%s", text );
}
// E2 v3 (docs/rtx-experimental-lighting.md "E2 v3"): the emitter ROWS - one per table surface run (= label) - the
// scene-light gather (RhiWorld.cpp RB_RHI_GatherSceneLights) appends to the scene-light buffer as DUDE_LIGHT_EMISSIVE
// rows, so the light grid, the ReSTIR reservoir, the SH side channel, the apply's specular re-shade and the GI bounce
// treat an emitter exactly like a placed light. CPU only (no GPU group buffer): rebuilt whenever flux (a fixture's
// state) or positions (movers) change. The row's triangles are sampled on the GPU straight from the table.
static idList<rtEmissiveRow_t>	s_rtEmissiveRows;
static idList<idVec3>			s_rtEmissiveRowAnim;		// per row: an ANIMATED glow's live / baked stage colour; x < 0 = steady (R_RtEmissiveUpdateAnimatedRows)

// A stable 64-bit id for a fixture at world position p (its emitter surface's CENTROID): quantised to a 0.1-unit grid -
// the centroid is computed identically every load (same triangles, same split), so the key is stable, and 0.1 keeps even
// tightly-packed fixtures distinct. Keyed by the CENTROID, not the paired light's origin: several placed lights bundled
// at one spot (the Mars City landing bay) share an origin, so an origin key collided every fixture lit by that bundle.
static unsigned long long R_RtEmissiveFixtureId( const idVec3 &p ) {
	const long long qx = (long long)idMath::Rint( p.x * 10.0f ) + 0x4000000, qy = (long long)idMath::Rint( p.y * 10.0f ) + 0x4000000, qz = (long long)idMath::Rint( p.z * 10.0f ) + 0x4000000;
	unsigned long long h = 1469598103934665603ull;							// FNV-1a over the three quantised coords
	const unsigned long long parts[3] = { (unsigned long long)qx, (unsigned long long)qy, (unsigned long long)qz };
	for ( int i = 0; i < 3; i++ ) { h ^= parts[i]; h *= 1099511628211ull; }
	return h ? h : 1ull;
}
extern idCVar r_hdrEyeAdaptation;
bool R_HdrEyeAdaptationWanted( void ) { return r_hdrEyeAdaptation.GetBool() || ( r_rtEmissiveLighting.GetBool() && r_rtEmissivePhysical.GetBool() && r_rtEmissiveAutoExposure.GetBool() ); }

// The POWER MATCH (the authored mode's brightness for a surface without a radiance, the Light Editor's "Match a light's
// power" and the switch to physical radiance): the radiance multiplier on the row's mean radiance that makes the surface
// deliver at R/2 what a Doom 3 light of luminance C and radius R does there (L = C R^2 / (16 A), reach boost 2 at that
// distance), capped at r_rtEmissiveMaxRadiance. reach 0 = r_rtEmissiveReachDefault; lightLum 0 = a light of the surface's
// OWN colour (C = its mean luminance, <= 1). Never below the glow as painted (1). (2026-10-31, L5: the effective-area ramp
// that kept small "indicators" at their painted glow went with the census - an authored surface emits as authored.)
float R_RtEmissiveMatchedScale( const rtEmissiveRow_t &row, float reach, float lightLum ) {
	const float maxL    = Max( 1.0f, r_rtEmissiveMaxRadiance.GetFloat() );
	const float meanLum = row.meanRadiance.x * 0.2126f + row.meanRadiance.y * 0.7152f + row.meanRadiance.z * 0.0722f;
	if ( !( meanLum > 1e-6f ) || !( row.area > 1e-3f ) ) { return 1.0f; }
	float R = ( reach > 0.0f ) ? reach : idMath::ClampFloat( 0.0f, 4096.0f, r_rtEmissiveReachDefault.GetFloat() );
	if ( !( R > 0.0f ) ) { R = Max( row.boundRadius * 4.0f, 64.0f ); }
	const float C = ( lightLum > 0.0f ) ? lightLum : Min( meanLum, 1.0f );
	const float Lmatch = Min( C * R * R / ( 16.0f * row.area ), maxL );
	return Max( Lmatch, meanLum ) / meanLum;
}

static float R_RtPointTriangleDistance( const idVec3 &p, const idVec3 &a, const idVec3 &b, const idVec3 &c );	// defined below

// The editor's pick for a per-fixture override: the id of the emitter CLUSTER (row) under the crosshair. Traces (through
// flares) to a surface, finds the nearest table triangle, then the row that holds it, and returns that row's fixture id
// (the paired light origin, or the cluster centroid). matOut / posOut report the material and the fixture position. 0 = miss.
unsigned long long R_RtEmissivePickFixture( const idMaterial **matOut, idVec3 *posOut, float *hitDistOut ) {
	if ( matOut ) { *matOut = NULL; }
	if ( posOut ) { posOut->Zero(); }
	if ( hitDistOut ) { *hitDistOut = 0.0f; }
	idRenderWorldLocal *world = tr.primaryWorld;
	if ( world == NULL || world->mapName.Length() == 0 || s_rtEmissiveRows.Num() == 0 ) { return 0; }
	const idVec3 dir = tr.primaryRenderView.viewaxis[0];
	idVec3 start = tr.primaryRenderView.vieworg;
	// Trace to a world POINT. The trace prefers collision surfaces (a model with clip geometry returns
	// textures/common/collision), so mt.material is unreliable: we use the emitter TABLE at the hit point instead,
	// walking the ray past flares and any non-drawn / collision hit until it lands near an emitter triangle.
	for ( int hit = 0; hit < 8; hit++ ) {
		modelTrace_t mt;
		if ( !world->Trace( mt, start, start + dir * 4096.0f, 0.0f, false, true ) ) { return 0; }
		// the nearest table triangle to the hit point, then the row holding it
		int bestT = -1; float bestD = 1e30f;
		for ( int ri = 0; ri < s_rtEmissiveRows.Num(); ri++ ) {
			const rtEmissiveRow_t &row = s_rtEmissiveRows[ri];
			const float reachD = row.boundRadius + Min( bestD, 24.0f );	// the row's bound sphere cannot hold a nearer triangle: skip it
			if ( ( row.centroid - mt.point ).LengthSqr() > reachD * reachD ) { continue; }	// (look mode picks every frame)
			for ( int t = row.firstTri; t < row.firstTri + row.numTris && t < s_rtEmissiveTris.Num(); t++ ) {
				const rtEmissiveTriGPU_t &e = s_rtEmissiveTris[t];
				const idVec3 a( e.p0[0], e.p0[1], e.p0[2] ), b( e.p1[0], e.p1[1], e.p1[2] ), c( e.p2[0], e.p2[1], e.p2[2] );
				const float d = R_RtPointTriangleDistance( mt.point, a, b, c );
				if ( d < bestD ) { bestD = d; bestT = t; }
			}
		}
		if ( bestT >= 0 && bestD <= 24.0f ) {
			for ( int ri = 0; ri < s_rtEmissiveRows.Num(); ri++ ) {
				const rtEmissiveRow_t &row = s_rtEmissiveRows[ri];
				if ( bestT >= row.firstTri && bestT < row.firstTri + row.numTris ) {
					if ( matOut ) { *matOut = row.mat; }	// the EMITTER's material, never the collision brush
					if ( posOut ) { *posOut = row.centroid; }
					if ( hitDistOut ) { *hitDistOut = ( mt.point - tr.primaryRenderView.vieworg ).Length(); }
					return row.fixtureId;
				}
			}
		}
		// the hit was a collision brush / non-emitter / flare in front of the fixture: step past it and trace again
		start = mt.point + dir * 0.5f;
	}
	return 0;
}

static const shaderStage_t *R_RtEmissiveStage( const idMaterial *mat, const char **why = NULL, const float *entityParms = NULL ) {
	const char *reason = "";
	if ( why ) { *why = ""; }
	if ( mat == NULL ) {
		return NULL;
	}
	const int n = mat->GetNumStages();
	bool anyAdditive = false;
	for ( int i = 0; i < n; i++ ) {
		const shaderStage_t *st = mat->GetStage( i );
		if ( ( st->drawStateBits & GLS_SRCBLEND_BITS ) == GLS_SRCBLEND_ONE && ( st->drawStateBits & GLS_DSTBLEND_BITS ) == GLS_DSTBLEND_ONE ) {
			anyAdditive = true;
		}
	}
	if ( !anyAdditive ) {
		return NULL;														// not self-illuminated at all: no reason to report (and no register work:
	}																		// this test runs over every surface in the periodic scans)
	// stage CONDITIONS (`if ( parm7 == 0 )`: a door light's red / green state, a fixture's on / off) evaluated with the
	// surface's own parms at time 0, so the stage that is actually drawn is the one gathered
	static const float defaultParms[MAX_ENTITY_SHADER_PARMS] = { 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
	float *regs = NULL;
	if ( mat->GetNumRegisters() > 0 ) {
		regs = (float *)_alloca16( mat->GetNumRegisters() * (int)sizeof( float ) );
		viewDef_t probeView;
		memset( &probeView, 0, sizeof( probeView ) );
		mat->EvaluateRegisters( regs, entityParms != NULL ? entityParms : defaultParms, &probeView, NULL );
	}
	if ( mat->HasGui() ) {
		reason = "gui screen";
	} else if ( mat->Deform() != DFRM_NONE ) {
		reason = ( mat->Deform() == DFRM_FLARE || mat->Deform() == DFRM_SPRITE ) ? "flare / sprite (a light's marker, not a surface)" : "deformed";
	} else if ( (int)mat->GetSort() == SS_DECAL ) {
		// a DECAL (textures/decals/*: alphabet signs, markings, dirt) puts its artwork on an additive stage so it stays
		// readable in the dark - that is legibility, not light (user, 2026-10-20)
		reason = "decal";
	}
	for ( int i = 0; i < n && reason[0] == '\0'; i++ ) {
		// the SKY (textures/skies/*: an additive cameraCubeMap stage with texgen skybox / wobblesky) is NOT a table
		// emitter - it was 99.9% of Mars City's flux and drew through the walls (user, 2026-10-19). E4 handles the sky
		// as its own emitter kind.
		const texgen_t tg = mat->GetStage( i )->texture.texgen;
		if ( tg == TG_SKYBOX_CUBE || tg == TG_WOBBLESKY_CUBE ) {
			reason = "sky";
		}
	}
	if ( reason[0] != '\0' ) {
		if ( why ) { *why = reason; }
		return NULL;
	}
	for ( int i = 0; i < n; i++ ) {
		const shaderStage_t *st = mat->GetStage( i );
		const bool additive = ( st->drawStateBits & GLS_SRCBLEND_BITS ) == GLS_SRCBLEND_ONE
		                   && ( st->drawStateBits & GLS_DSTBLEND_BITS ) == GLS_DSTBLEND_ONE;
		if ( !additive ) {
			continue;
		}
		if ( regs != NULL && st->conditionRegister >= 0 && st->conditionRegister < mat->GetNumRegisters() && regs[ st->conditionRegister ] == 0.0f ) {
			reason = "additive stage switched off by its condition (parm7 state)";
			continue;														// the OFF state of a two-state fixture
		}
		if ( st->texture.cinematic != NULL || st->texture.dynamic != DI_STATIC ) {
			if ( why ) { *why = "video / render-to-texture screen"; }
			return NULL;													// a screen: fill-light territory, not a table emitter
		}
		const idImage *img = st->texture.image;
		// DUDE: glow textures upload LAZILY (on a surface's first draw), so a fixture the player has not looked at yet has
		// rhiHandle 0 and is skipped here - the light then "turns on" a second or two later when the periodic residency
		// rebuild catches it (user, 2026-10-21). Force every candidate glow texture RESIDENT at build (VRAM is plentiful,
		// GPU upload is the low-latency path): no pop-in, and the load also computes the bright extents + averageColor the
		// mean radiance reads. Idempotent: an already-resident image is a no-op. Frontend context (R_RenderView -> here).
		if ( img != NULL && img->rhiHandle == 0 && img->generatorFunction == NULL && !img->defaulted
				&& img->type != TT_CUBIC && st->texture.texgen == TG_EXPLICIT ) {
			const_cast<idImage *>( img )->ActuallyLoadImage( true, false );
		}
		// an environment reflection (glass: texgen reflect + cubeMap) or any other generated texgen is not emission
		if ( img == NULL || img->rhiHandle == 0 || img->generatorFunction != NULL || img->defaulted
				|| img->type == TT_CUBIC || st->texture.texgen != TG_EXPLICIT ) {
			if ( img == NULL || img->defaulted )           { reason = "additive stage has no image"; }
			else if ( img->rhiHandle == 0 )                { reason = "glow image not resident on the GPU (rebuilds when it is)"; if ( s_rtEmissiveNotResident.FindIndex( img ) < 0 ) { s_rtEmissiveNotResident.Append( img ); } }
			else if ( img->generatorFunction != NULL )     { reason = "generated image"; }
			else                                           { reason = "cube map / reflection texgen (glass)"; }
			continue;
		}
		if ( why ) { *why = ""; }
		return st;
	}
	if ( why ) { *why = reason; }
	return NULL;
}

// The emissive stage's colour: its rgb/red/green/blue registers evaluated with the owning entity's shaderParms (a
// fixture tinted via _color) at the build view. Returns false (ANIMATED) when the material's registers are not all
// constant - a time / table-driven colour (pulsing, flickering): the value is this instant's phase, so the caller
// floors it (an OFF phase must not drop the emitter from the table) and E2 reads the live colour instead.
static bool R_RtEmissiveStageColor( const idMaterial *mat, const shaderStage_t *st, const float *entityParms, float rgb[3] ) {
	rgb[0] = rgb[1] = rgb[2] = 1.0f;
	const int numRegs = mat->GetNumRegisters();
	if ( numRegs <= 0 ) {
		return true;
	}
	// Nearly every light material takes its colour from the ENTITY parms (`red parm0` / `colored`), which the engine's
	// constant-register check already counts as non-constant - so ConstantRegisters() is NULL for all of them (the
	// first census reported 54/54 materials "register-driven"). Entity parms ARE constant for a given surface; only a
	// TIME-driven colour (tables, sin) is animated. Evaluate at three times with the surface's own parms (the world's
	// parm0-3 = 1) and a zeroed view (no global parms, no sound), as the constant-register check itself does.
	static const float defaultParms[MAX_ENTITY_SHADER_PARMS] = { 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
	static const float probeTimes[3] = { 0.0f, 0.37f, 1.71f };
	const float *parms = ( entityParms != NULL ) ? entityParms : defaultParms;
	float *regs = (float *)_alloca16( numRegs * (int)sizeof( float ) );
	viewDef_t probeView;
	memset( &probeView, 0, sizeof( probeView ) );
	bool animated = false;
	float first[3] = { 1.0f, 1.0f, 1.0f };
	for ( int t = 0; t < 3; t++ ) {
		probeView.floatTime = probeTimes[t];
		mat->EvaluateRegisters( regs, parms, &probeView, NULL );
		for ( int k = 0; k < 3; k++ ) {
			const int ri = st->color.registers[k];
			const float v = ( ri >= 0 && ri < numRegs ) ? idMath::ClampFloat( 0.0f, 16.0f, regs[ri] ) : 1.0f;
			if ( t == 0 ) {
				first[k] = v;
				rgb[k] = v;
			} else if ( idMath::Fabs( v - first[k] ) > 1e-4f ) {
				animated = true;
			}
		}
	}
	return !animated;
}

// The glow stage's texture matrix (scale / translate / rotate on the stage), evaluated at time 0 with the surface's
// parms, so the table's per-vertex st can be BAKED through it: the sample at E2 (and the debug overlay) then reads
// the texel the forward path draws. A time-scrolled matrix bakes its t=0 phase (the glow pattern is what matters
// for the flux; the live colour is E2's). Returns false (identity) when the stage has no matrix.
static bool R_RtEmissiveStageTexMatrix( const idMaterial *mat, const shaderStage_t *st, const float *entityParms, float m[6] ) {
	m[0] = 1.0f; m[1] = 0.0f; m[2] = 0.0f; m[3] = 0.0f; m[4] = 1.0f; m[5] = 0.0f;
	if ( !st->texture.hasMatrix || mat->GetNumRegisters() <= 0 ) {
		return false;
	}
	static const float defaultParms[MAX_ENTITY_SHADER_PARMS] = { 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
	float *regs = (float *)_alloca16( mat->GetNumRegisters() * (int)sizeof( float ) );
	viewDef_t probeView;
	memset( &probeView, 0, sizeof( probeView ) );
	mat->EvaluateRegisters( regs, entityParms != NULL ? entityParms : defaultParms, &probeView, NULL );
	for ( int r = 0; r < 2; r++ ) {
		for ( int c = 0; c < 3; c++ ) {
			m[r * 3 + c] = regs[ st->texture.matrix[r][c] ];
		}
	}
	return true;
}

// The debug overlay's mode-1 tint for a glow slot (rt_emissive_debug.frag dbgTint, same hash), for the census so a
// painted surface can be matched to its material by colour.
static void R_RtEmissiveDebugTint( unsigned int slot, int rgb[3], const char **name ) {
	unsigned int sh = slot * 2654435761u + 1u;
	unsigned int w = ( ( sh >> ( ( sh >> 28u ) + 4u ) ) ^ sh ) * 277803737u;
	unsigned int h = ( w >> 22u ) ^ w;
	float f[3] = { ( h & 255u ) * ( 0.75f / 255.0f ) + 0.25f, ( ( h >> 8u ) & 255u ) * ( 0.75f / 255.0f ) + 0.25f, ( ( h >> 16u ) & 255u ) * ( 0.75f / 255.0f ) + 0.25f };
	for ( int k = 0; k < 3; k++ ) { rgb[k] = (int)( f[k] * 255.0f + 0.5f ); }
	// a coarse hue word
	const float mx = Max( f[0], Max( f[1], f[2] ) ), mn = Min( f[0], Min( f[1], f[2] ) );
	if ( mx - mn < 0.12f ) { *name = ( mx > 0.7f ) ? "white/grey" : "grey"; return; }
	float hue;
	if ( mx == f[0] )      { hue = ( f[1] - f[2] ) / ( mx - mn ); }
	else if ( mx == f[1] ) { hue = 2.0f + ( f[2] - f[0] ) / ( mx - mn ); }
	else                   { hue = 4.0f + ( f[0] - f[1] ) / ( mx - mn ); }
	hue *= 60.0f; if ( hue < 0.0f ) { hue += 360.0f; }
	static const char *names[12] = { "red", "orange", "yellow", "lime", "green", "spring", "cyan", "azure", "blue", "violet", "magenta", "rose" };
	*name = names[ ( (int)( hue / 30.0f ) ) % 12 ];
}

// An entity the table may gather: a resident DM_STATIC model that is NOT produced by a callback. The game's smoke
// particle system (idSmokeParticles), FX and similar render ALL their quads through one entity whose model is a plain
// static model rebuilt every frame by renderEntity.callback - its surfaces at build time (a rocket trail, the embers
// of a burning corpse) were taken as static emitters and lingered forever (user, 2026-10-20).
static bool R_RtEmissiveEntityGatherable( const idRenderEntityLocal *def ) {
	if ( def == NULL || def->parms.hModel == NULL || def->parms.callback != NULL ) {
		return false;
	}
	const idRenderModel *model = def->parms.hModel;
	return !model->IsStaticWorldModel() && model->IsDynamicModel() == DM_STATIC && !model->IsDefaultModel();
}

// per-material census row for the build report
struct rtEmissiveCensus_t { const idMaterial *mat; int tris; float flux; bool animated; bool texMatrix; unsigned int slot; bool clipped; };

// Append every triangle of one emissive surface. modelMatrix NULL = the verts are already world space (an _areaN).

// Emit the LIGHT of one triangle (Q2RTX collect_one_light_poly): with valid, non-entire image extents the triangle is
// clipped, per texture repetition, to the st rectangle of the bright texels (Sutherland-Hodgman in st; vertices mapped
// back through the triangle's barycentrics), fan-triangulated, and each piece is an entry with the bright texels' MEAN
// radiance - a wall panel with two LEDs becomes two small lights, so the flux proposal matches the real emission. No
// extents / entire = the whole triangle. p[] are MODEL-space (or world, mm NULL) positions, st[] the baked st.
// Returns entries added.
static int R_RtEmissiveEmitTriangle( const idVec3 p[3], const float st[3][2], const float *mm, const imageEmissiveExtents_t *ext,
		float lumPerArea, unsigned int emit, unsigned int tint, unsigned int meanRG, unsigned int meanB, float scale,
		idList<rtEmissiveTriGPU_t> &out, idList<idVec3> *localOut, int &overflow ) {
	struct pv_t { float s, t; idVec3 p; };
	pv_t poly[12], clipped[12];
	int added = 0;
	auto emitPiece = [&]( const pv_t &a, const pv_t &b, const pv_t &c ) {
		if ( out.Num() >= RT_EMISSIVE_MAX_TRIS ) { overflow++; return; }
		idVec3 w0 = a.p, w1 = b.p, w2 = c.p;
		if ( mm != NULL ) { R_LocalPointToGlobal( mm, a.p, w0 ); R_LocalPointToGlobal( mm, b.p, w1 ); R_LocalPointToGlobal( mm, c.p, w2 ); }
		const float area = 0.5f * ( ( w1 - w0 ).Cross( w2 - w0 ) ).Length();
		if ( area < 1e-3f ) { return; }
		if ( mm != NULL && localOut != NULL ) { localOut->Append( a.p ); localOut->Append( b.p ); localOut->Append( c.p ); }
		rtEmissiveTriGPU_t &e = out.Alloc();
		e.p0[0] = w0.x; e.p0[1] = w0.y; e.p0[2] = w0.z;
		e.p1[0] = w1.x; e.p1[1] = w1.y; e.p1[2] = w1.z;
		e.p2[0] = w2.x; e.p2[1] = w2.y; e.p2[2] = w2.z;
		e.flux = area * lumPerArea;
		e.emit = emit;
		e.scale = scale;
		e.st0 = R_RtPackHalf2( a.s, a.t ); e.st1 = R_RtPackHalf2( b.s, b.t ); e.st2 = R_RtPackHalf2( c.s, c.t );
		e.tint = tint;
		e.meanRG = meanRG; e.meanB = meanB; e.reach = 0u; e.lightLum = 0u;
		added++;
	};
	pv_t v[3];
	for ( int k = 0; k < 3; k++ ) { v[k].s = st[k][0]; v[k].t = st[k][1]; v[k].p = p[k]; }
	// the st -> barycentric map (a 2x2 inverse); degenerate st (all three on a line) = no clipping possible
	const float ds1 = st[1][0] - st[0][0], dt1 = st[1][1] - st[0][1], ds2 = st[2][0] - st[0][0], dt2 = st[2][1] - st[0][1];
	const float det = ds1 * dt2 - ds2 * dt1;
	float minS = Min( st[0][0], Min( st[1][0], st[2][0] ) ), maxS = Max( st[0][0], Max( st[1][0], st[2][0] ) );
	float minT = Min( st[0][1], Min( st[1][1], st[2][1] ) ), maxT = Max( st[0][1], Max( st[1][1], st[2][1] ) );
	if ( ext == NULL || !ext->valid || ext->entire || idMath::Fabs( det ) < 1e-10f || ( maxS - minS ) > 64.0f || ( maxT - minT ) > 64.0f ) {
		emitPiece( v[0], v[1], v[2] );										// whole triangle (also: a surface tiling the texture 64+ times)
		return added;
	}
	const float invDet = 1.0f / det;
	auto mapBack = [&]( float sv, float tv ) {
		pv_t r; r.s = sv; r.t = tv;
		const float es = sv - st[0][0], et = tv - st[0][1];
		const float u = ( es * dt2 - ds2 * et ) * invDet, w = ( ds1 * et - es * dt1 ) * invDet;
		r.p = p[0] + ( p[1] - p[0] ) * u + ( p[2] - p[0] ) * w;
		return r;
	};
	for ( float ty = idMath::Floor( minT ); ty < maxT; ty += 1.0f ) {
		for ( float tx = idMath::Floor( minS ); tx < maxS; tx += 1.0f ) {
			// the rectangle of this repetition: 4 half-plane clips (Sutherland-Hodgman)
			const float rect[4] = { tx + ext->minST[0], tx + ext->maxST[0], ty + ext->minST[1], ty + ext->maxST[1] };
			int n = 3;
			poly[0] = v[0]; poly[1] = v[1]; poly[2] = v[2];
			for ( int side = 0; side < 4 && n >= 3; side++ ) {
				int m = 0;
				for ( int i = 0; i < n; i++ ) {
					const pv_t &cur = poly[i], &nxt = poly[( i + 1 ) % n];
					const float cv = ( side < 2 ) ? cur.s : cur.t, nv = ( side < 2 ) ? nxt.s : nxt.t, lim = rect[side];
					const bool cin = ( side & 1 ) ? ( cv <= lim ) : ( cv >= lim ), nin = ( side & 1 ) ? ( nv <= lim ) : ( nv >= lim );
					if ( cin ) { clipped[m++] = cur; }
					if ( cin != nin ) {
						const float f = ( lim - cv ) / ( nv - cv );
						clipped[m++] = mapBack( cur.s + ( nxt.s - cur.s ) * f, cur.t + ( nxt.t - cur.t ) * f );
					}
					if ( m >= 11 ) { break; }
				}
				n = m;
				for ( int i = 0; i < n; i++ ) { poly[i] = clipped[i]; }
			}
			for ( int i = 1; i + 1 < n; i++ ) { emitPiece( poly[0], poly[i], poly[i + 1] ); }
		}
	}
	return added;
}

static void R_RtEmissiveAddSurface( const srfTriangles_t *tri, const idMaterial *mat, const shaderStage_t *stage,
		const float *modelMatrix, const float *entityParms, unsigned int flags, float scale,
		idList<rtEmissiveCensus_t> &census, int &overflow,
		idList<rtEmissiveTriGPU_t> &out, idList<idVec3> *localOut ) {
	float tintRGB[3];
	const bool constant = R_RtEmissiveStageColor( mat, stage, entityParms, tintRGB );
	float tintLum = R_RtLuminance( tintRGB );
	if ( !constant ) {
		flags |= RT_EMIT_FLAG_ANIMATED;
		if ( tintLum < 0.25f ) {											// an OFF / dim phase of a flicker: keep it a candidate
			tintLum = 0.25f;
			tintRGB[0] = tintRGB[1] = tintRGB[2] = 0.25f;
		}
	}
	const idImage *img = stage->texture.image;
	// the light's colour: the mean of the BRIGHT texels when the image's glow extents are known (the light is clipped to
	// them), else the whole-image average
	const imageEmissiveExtents_t *ext = img->glowExtents.valid ? &img->glowExtents : NULL;
	float meanLin[3];
	if ( ext != NULL ) { meanLin[0] = ext->mean[0]; meanLin[1] = ext->mean[1]; meanLin[2] = ext->mean[2]; }
	else { meanLin[0] = R_RtSrgbToLinear( img->averageColor[0] ); meanLin[1] = R_RtSrgbToLinear( img->averageColor[1] ); meanLin[2] = R_RtSrgbToLinear( img->averageColor[2] ); }
	const float lumPerArea = R_RtLuminance( meanLin ) * tintLum * scale;
	if ( lumPerArea <= 0.0f ) {
		return;																// a black glow stage (a switched-off fixture skin): no emitter
	}
	if ( mat->ShouldCreateBackSides() ) {
		flags |= RT_EMIT_FLAG_TWOSIDED;
	}
	const unsigned int emit = ( (unsigned int)img->rhiHandle & 0xFFFFu ) | flags;
	const unsigned int tint = R_RtPackRGBA8( tintRGB );
	const unsigned int meanRG = R_RtPackHalf2( meanLin[0] * tintRGB[0], meanLin[1] * tintRGB[1] );
	const unsigned int meanB  = R_RtPackHalf2( meanLin[2] * tintRGB[2], 0.0f );
	float tm[6];
	const bool hasTexMatrix = R_RtEmissiveStageTexMatrix( mat, stage, entityParms, tm );
	int ci = -1;
	for ( int i = 0; i < census.Num(); i++ ) { if ( census[i].mat == mat ) { ci = i; break; } }
	if ( ci < 0 ) { rtEmissiveCensus_t c; c.mat = mat; c.tris = 0; c.flux = 0.0f; c.animated = !constant; c.texMatrix = hasTexMatrix; c.slot = (unsigned int)img->rhiHandle & 0xFFFFu; c.clipped = ( ext != NULL && !ext->entire ); ci = census.Append( c ); }

	for ( int n = 0; n + 2 < tri->numIndexes; n += 3 ) {
		const idDrawVert *vv[3] = { &tri->verts[ tri->indexes[n + 0] ], &tri->verts[ tri->indexes[n + 1] ], &tri->verts[ tri->indexes[n + 2] ] };
		idVec3 pp[3];
		float st[3][2];
		for ( int k = 0; k < 3; k++ ) {
			pp[k] = vv[k]->xyz;
			float s0 = vv[k]->st[0], t0 = vv[k]->st[1];
			if ( hasTexMatrix ) {												// bake the stage's texture matrix (draw_common.cpp RB_PrepareStageTexturing)
				const float s1 = tm[0] * s0 + tm[1] * t0 + tm[2];
				const float t1 = tm[3] * s0 + tm[4] * t0 + tm[5];
				s0 = s1; t0 = t1;
			}
			st[k][0] = s0; st[k][1] = t0;
		}
		const int before = out.Num();
		R_RtEmissiveEmitTriangle( pp, st, modelMatrix, ext, lumPerArea, emit, tint, meanRG, meanB, scale, out, localOut, overflow );
		for ( int t = before; t < out.Num(); t++ ) { census[ci].tris++; census[ci].flux += out[t].flux; }
	}
}

// A flare material: flare- or sprite-deformed with an additive stage (textures/sfx/flare is `deform flare`, light
// glows `deform sprite`). The surface the emitter-mode hide skips.
bool R_RtEmissiveIsFlareMaterial( const idMaterial *mat ) {
	if ( mat == NULL || ( mat->Deform() != DFRM_SPRITE && mat->Deform() != DFRM_FLARE ) ) {
		return false;
	}
	for ( int k = 0; k < mat->GetNumStages(); k++ ) {
		const shaderStage_t *st = mat->GetStage( k );
		if ( ( st->drawStateBits & GLS_SRCBLEND_BITS ) == GLS_SRCBLEND_ONE && ( st->drawStateBits & GLS_DSTBLEND_BITS ) == GLS_DSTBLEND_ONE ) {
			return true;
		}
	}
	return false;
}

// A light entity's flare: a model with a flare surface (a door frame's flare polygon is in the frame mesh itself)
static bool R_RtEmissiveIsFlareEntity( const idRenderEntityLocal *def ) {
	if ( def == NULL || def->parms.hModel == NULL ) {
		return false;
	}
	const idRenderModel *model = def->parms.hModel;
	for ( int sf = 0; sf < model->NumSurfaces(); sf++ ) {
		if ( R_RtEmissiveIsFlareMaterial( R_RemapShaderBySkin( model->Surface( sf )->shader, def->parms.customSkin, def->parms.customShader ) ) ) {
			return true;
		}
	}
	return false;
}

// Distance from a point to a triangle (closest point on the triangle, Ericson 5.1.5)
static float R_RtPointTriangleDistance( const idVec3 &p, const idVec3 &a, const idVec3 &b, const idVec3 &c ) {
	const idVec3 ab = b - a, ac = c - a, ap = p - a;
	const float d1 = ab * ap, d2 = ac * ap;
	if ( d1 <= 0.0f && d2 <= 0.0f ) { return ap.Length(); }
	const idVec3 bp = p - b;
	const float d3 = ab * bp, d4 = ac * bp;
	if ( d3 >= 0.0f && d4 <= d3 ) { return bp.Length(); }
	const float vc = d1 * d4 - d3 * d2;
	if ( vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f ) { const float v = d1 / ( d1 - d3 ); return ( p - ( a + ab * v ) ).Length(); }
	const idVec3 cp = p - c;
	const float d5 = ab * cp, d6 = ac * cp;
	if ( d6 >= 0.0f && d5 <= d6 ) { return cp.Length(); }
	const float vb = d5 * d2 - d1 * d6;
	if ( vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f ) { const float w = d2 / ( d2 - d6 ); return ( p - ( a + ac * w ) ).Length(); }
	const float va = d3 * d6 - d5 * d4;
	if ( va <= 0.0f && ( d4 - d3 ) >= 0.0f && ( d5 - d6 ) >= 0.0f ) { const float w = ( d4 - d3 ) / ( ( d4 - d3 ) + ( d5 - d6 ) ); return ( p - ( b + ( c - b ) * w ) ).Length(); }
	const float denom = 1.0f / ( va + vb + vc );
	const float v = vb * denom, w = vc * denom;
	return ( p - ( a + ab * v + ac * w ) ).Length();
}

// Distance from p to the nearest table triangle (1e30 when the table is empty)
static float R_RtEmissiveTableDistance( const idVec3 &p ) {
	float best = 1e30f;
	for ( int t = 0; t < s_rtEmissiveTris.Num(); t++ ) {
		const rtEmissiveTriGPU_t &e = s_rtEmissiveTris[t];
		const idVec3 a( e.p0[0], e.p0[1], e.p0[2] ), b( e.p1[0], e.p1[1], e.p1[2] ), c( e.p2[0], e.p2[1], e.p2[2] );
		const float d = R_RtPointTriangleDistance( p, a, b, c );
		if ( d < best ) { best = d; }
	}
	return best;
}
static bool R_RtEmissiveNearTable( const idVec3 &p, float reach ) { return R_RtEmissiveTableDistance( p ) < reach; }

// drop the table's GPU handles WITHOUT destroying them (a new backend owns the slots now, R_RtResetSceneState) and
// clear every CPU-side list, so the next RT frame rebuilds from nothing
// r_rtEmissiveScale as the table uses it (the clamp the cvar's range would give it)
// the seen-model list reaches entity index i, the new slots NULL (not idList::AssureSize: it leaves the slots inside
// the old capacity as they were)
static void R_RtEmissiveSeenGrow( int i ) {
	while ( s_rtEmissiveSeenModel.Num() <= i ) {
		s_rtEmissiveSeenModel.Append( NULL );
	}
}

static float R_RtEmissiveScale( void ) { return idMath::ClampFloat( 0.0f, 64.0f, r_rtEmissiveScale.GetFloat() ); }

void R_RtEmissiveForget( void ) {
	s_rtEmissiveBuf = 0;	s_rtEmissiveAddr = 0;	s_rtEmissiveTotalFlux = 0.0f;
	s_rtEmissiveRows.Clear();
	s_rtEmissiveRowAnim.Clear();
	s_rtEmissiveDbgVB = s_rtEmissiveDbgIB = 0;	s_rtEmissiveDbgIndexes = 0;
	s_rtEmissiveTris.Clear();
	s_rtEmissiveTriSrc.Clear();
	s_rtEmissiveGroups.Clear();
	s_rtEmissiveLocalVerts.Clear();
	s_rtEmissiveLabels.Clear();
	s_rtEmissiveScreenLabels.Clear();
	s_rtEmissiveFlareHidden.Clear();
	s_rtEmissiveSeenModel.Clear();
	s_rtEmissiveFlareCount = 0;
	s_rtEmissiveMovedGroups = 0;
	s_rtEmissiveMap.Clear();
}

// DUDE: the table's glow texture slots are stale (a texture reload moved them): rebuild it on the next RT frame
void R_RtEmissiveInvalidate( void ) {
	s_rtEmissiveMap.Clear();
}

void R_RtEmissiveFree( void ) {
	rhi::RHI *r = rhi::GetRHI();
	if ( r != NULL ) {
		if ( s_rtEmissiveBuf != 0 )   { r->DestroyBuffer( s_rtEmissiveBuf ); }
		if ( s_rtEmissiveDbgVB != 0 ) { r->DestroyBuffer( s_rtEmissiveDbgVB ); }
		if ( s_rtEmissiveDbgIB != 0 ) { r->DestroyBuffer( s_rtEmissiveDbgIB ); }
	}
	R_RtEmissiveForget();
}

static int R_RtCmpEmissiveCensus( const void *pa, const void *pb ) {
	const float a = ( (const rtEmissiveCensus_t *)pa )->flux, b = ( (const rtEmissiveCensus_t *)pb )->flux;
	return ( a < b ) ? 1 : ( a > b ) ? -1 : 0;
}

// Gather + flux + CDF + device buffer for the current map; prints the census (E1a) once per build.
// E2 v3 (user, 2026-10-20: "if many areas in the same surface are lit they should be computed as separate"): a WORLD
// material run holds EVERY fixture of that material on the surface - a ceiling's light panels, clipped to their bright
// rects by the gather, were ONE row: centre in the middle of the room, sample points on panels behind walls, the matched
// power divided over all of them, the floor under any one panel got a sliver (mcitya12_2, 288 triangles). Split each
// world run into spatially CONNECTED clusters of triangles (bounding boxes touching within 2 units, found through a
// 16-unit spatial hash + union-find), reorder the run so every cluster is contiguous (the GPU samples a row by its
// contiguous range) and give each cluster its own label = its own row, pairing and power. Called right after the world
// gather, and for each entity right after its surfaces (2026-10-29): a prop is NOT one fixture - the COMBAT PREP door
// frame (mars_city1) carries a strip on each side of its door, one row paired with the light BEHIND the door left this
// side dark. An entity's model-space verts (3 per triangle, read by the mover re-bake) are permuted with its triangles
// (groupFirstTri / groupFirstLocal >= 0) and the source index (s_rtEmissiveTriSrc) rides along for the re-gather.
static void R_RtEmissiveSplitRuns( int firstLabel, int groupFirstTri, int groupFirstLocal, int &runsSplit, int &fixtures ) {
	struct key_t { unsigned long long key; int tri; };
	struct cmp_t { static int Cmp( const key_t *a, const key_t *b ) { return ( a->key < b->key ) ? -1 : ( a->key > b->key ) ? 1 : 0; } };
	static idList<key_t>  keys;
	static idList<int>    parent, order;
	static idList<idBounds> boxes;
	static idList<rtEmissiveTriGPU_t> scratch;
	static idList<int> scratchSrc;
	static idList<idVec3> scratchLocal;
	const float cell = 16.0f, margin = 2.0f;
	const int nOrig = s_rtEmissiveLabels.Num();
	while ( s_rtEmissiveTriSrc.Num() < s_rtEmissiveTris.Num() ) { s_rtEmissiveTriSrc.Append( s_rtEmissiveTriSrc.Num() ); }	// new triangles: gather order
	auto find = [&]( int i ) { while ( parent[i] != i ) { parent[i] = parent[ parent[i] ]; i = parent[i]; } return i; };
	auto cellKey = [&]( const idVec3 &c ) {
		const long long ix = (long long)idMath::Floor( c.x / cell ) + 0x100000, iy = (long long)idMath::Floor( c.y / cell ) + 0x100000, iz = (long long)idMath::Floor( c.z / cell ) + 0x100000;
		return ( (unsigned long long)ix << 42 ) | ( (unsigned long long)iy << 21 ) | (unsigned long long)iz;
	};
	for ( int li = firstLabel; li < nOrig; li++ ) {
		rtEmissiveLabel_t &l = s_rtEmissiveLabels[li];
		const int n = l.numTris, first = l.firstTri;
		if ( n <= 1 ) { fixtures++; continue; }
		boxes.SetNum( n, false ); parent.SetNum( n, false ); keys.SetNum( n, false );
		for ( int t = 0; t < n; t++ ) {
			const rtEmissiveTriGPU_t &e = s_rtEmissiveTris[ first + t ];
			idBounds &b = boxes[t];
			b.Clear();
			b.AddPoint( idVec3( e.p0[0], e.p0[1], e.p0[2] ) ); b.AddPoint( idVec3( e.p1[0], e.p1[1], e.p1[2] ) ); b.AddPoint( idVec3( e.p2[0], e.p2[1], e.p2[2] ) );
			parent[t] = t;
			keys[t].key = cellKey( b.GetCenter() ); keys[t].tri = t;
		}
		keys.Sort( cmp_t::Cmp );
		// union every pair whose centroids share a hash cell or a neighbouring one and whose boxes touch (margin)
		for ( int t = 0; t < n; t++ ) {
			const idVec3 c = boxes[t].GetCenter();
			idBounds bt = boxes[t]; bt.ExpandSelf( margin );
			for ( int dz = -1; dz <= 1; dz++ ) for ( int dy = -1; dy <= 1; dy++ ) for ( int dx = -1; dx <= 1; dx++ ) {
				const unsigned long long k = cellKey( c + idVec3( dx * cell, dy * cell, dz * cell ) );
				// lower bound in the sorted keys
				int lo = 0, hi = n;
				while ( lo < hi ) { const int mid = ( lo + hi ) >> 1; if ( keys[mid].key < k ) { lo = mid + 1; } else { hi = mid; } }
				for ( int j = lo; j < n && keys[j].key == k; j++ ) {
					const int u = keys[j].tri;
					if ( u == t ) { continue; }
					if ( bt.IntersectsBounds( boxes[u] ) ) {
						const int ra = find( t ), rb = find( u );
						if ( ra != rb ) { parent[ra] = rb; }
					}
				}
			}
		}
		// components -> contiguous order: label each root, then stable-group the triangles by root
		order.SetNum( n, false );
		int clusters = 0;
		static idList<int> rootId, rootFirstSeen;
		rootId.SetNum( n, false );
		for ( int t = 0; t < n; t++ ) { rootId[t] = -1; }
		for ( int t = 0; t < n; t++ ) { const int rt = find( t ); if ( rootId[rt] < 0 ) { rootId[rt] = clusters++; } }
		if ( clusters <= 1 ) { fixtures++; continue; }
		// counting sort by cluster id (deterministic, keeps the gather order inside a cluster)
		static idList<int> start, fill;
		start.SetNum( clusters + 1, false ); fill.SetNum( clusters, false );
		for ( int k = 0; k <= clusters; k++ ) { start[k] = 0; }
		for ( int t = 0; t < n; t++ ) { start[ rootId[ find( t ) ] + 1 ]++; }
		for ( int k = 0; k < clusters; k++ ) { start[k + 1] += start[k]; fill[k] = start[k]; }
		for ( int t = 0; t < n; t++ ) { order[ fill[ rootId[ find( t ) ] ]++ ] = t; }
		scratch.SetNum( n, false );
		scratchSrc.SetNum( n, false );
		for ( int t = 0; t < n; t++ ) { scratch[t] = s_rtEmissiveTris[ first + order[t] ]; scratchSrc[t] = s_rtEmissiveTriSrc[ first + order[t] ]; }
		for ( int t = 0; t < n; t++ ) { s_rtEmissiveTris[ first + t ] = scratch[t]; s_rtEmissiveTriSrc[ first + t ] = scratchSrc[t]; }
		if ( groupFirstTri >= 0 ) {
			const int localFirst = groupFirstLocal + 3 * ( first - groupFirstTri );
			scratchLocal.SetNum( 3 * n, false );
			for ( int t = 0; t < n; t++ ) { for ( int k = 0; k < 3; k++ ) { scratchLocal[ 3 * t + k ] = s_rtEmissiveLocalVerts[ localFirst + 3 * order[t] + k ]; } }
			for ( int v = 0; v < 3 * n; v++ ) { s_rtEmissiveLocalVerts[ localFirst + v ] = scratchLocal[v]; }
		}
		// the first cluster keeps this label, the others get their own (same material: the overlay name is unchanged)
		const idMaterial *mat = l.mat;
		l.numTris = start[1] - start[0];
		for ( int k = 1; k < clusters; k++ ) {
			rtEmissiveLabel_t nl; nl.mat = mat; nl.firstTri = first + start[k]; nl.numTris = start[k + 1] - start[k];
			s_rtEmissiveLabels.Append( nl );
		}
		fixtures += clusters;
		runsSplit++;
	}
}

// self-illuminated materials the gather rejected, with the reason (the build's report)
struct rtEmissiveExcluded_t { const idMaterial *mat; const char *why; };
static void R_RtEmissiveNoteExcluded( idList<rtEmissiveExcluded_t> *excluded, const idMaterial *m, const char *why ) {
	if ( excluded == NULL || m == NULL || why == NULL || why[0] == '\0' ) { return; }
	for ( int k = 0; k < excluded->Num(); k++ ) { if ( ( *excluded )[k].mat == m ) { return; } }
	rtEmissiveExcluded_t x; x.mat = m; x.why = why; excluded->Append( x );
}

// One static entity's emissive surfaces into the table: model space x the entity matrix, the SKIN-REMAPPED material (a
// fixture's on/off state is a skin), the entity's shaderParms (its _color); split per fixture and tracked as a mover group.
// Shared by the build and R_RtEmissiveAppendEntity (an entity that appears later), so both gather identically. Returns
// the triangles added (0 = nothing glows); the caller records the slot in s_rtEmissiveSeenModel.
static int R_RtEmissiveGatherEntity( const idRenderWorldLocal *world, int i, float scale, idList<rtEmissiveCensus_t> &census,
		int &overflow, int &screens, int &runsSplit, int &fixtures, idList<rtEmissiveExcluded_t> *excluded ) {
	const idRenderEntityLocal *def = world->entityDefs[i];
	if ( !R_RtEmissiveEntityGatherable( def ) ) {
		return 0;
	}
	const idRenderModel *model = def->parms.hModel;
	float modelMatrix[16];
	R_AxisToModelMatrix( def->parms.axis, def->parms.origin, modelMatrix );
	const int groupFirstTri = s_rtEmissiveTris.Num(), groupFirstLocal = s_rtEmissiveLocalVerts.Num(), groupFirstLabel = s_rtEmissiveLabels.Num();
	for ( int s = 0; s < model->NumSurfaces(); s++ ) {
		const modelSurface_t *surf = model->Surface( s );
		if ( surf->geometry == NULL || surf->geometry->verts == NULL || surf->geometry->indexes == NULL ) {
			continue;
		}
		const idMaterial *mat = R_RemapShaderBySkin( surf->shader, def->parms.customSkin, def->parms.customShader );
		if ( R_RtEmissiveIsScreen( mat ) ) {
			screens++;
		}
		const char *why = "";
		const shaderStage_t *stage = R_RtEmissiveStage( mat, &why, def->parms.shaderParms );
		if ( stage == NULL ) {
			R_RtEmissiveNoteExcluded( excluded, mat, why );
			continue;
		}
		const int before = s_rtEmissiveTris.Num();
		R_RtEmissiveAddSurface( surf->geometry, mat, stage, modelMatrix, def->parms.shaderParms, RT_EMIT_FLAG_ENTITY, scale, census, overflow, s_rtEmissiveTris, &s_rtEmissiveLocalVerts );
		if ( s_rtEmissiveTris.Num() > before ) { rtEmissiveLabel_t l; l.mat = mat; l.firstTri = before; l.numTris = s_rtEmissiveTris.Num() - before; s_rtEmissiveLabels.Append( l ); }
	}
	const int added = s_rtEmissiveTris.Num() - groupFirstTri;
	if ( added <= 0 ) {
		return 0;
	}
	R_RtEmissiveSplitRuns( groupFirstLabel, groupFirstTri, groupFirstLocal, runsSplit, fixtures );	// one row per fixture of the prop
	rtEmissiveGroup_t &g = s_rtEmissiveGroups.Alloc();
	g.entityIndex = i;
	g.model = model;
	g.firstTri = groupFirstTri;
	g.numTris = added;
	g.firstLocalVert = groupFirstLocal;
	g.reported = false;
	memcpy( g.lastMatrix, modelMatrix, sizeof( g.lastMatrix ) );
	g.skin = def->parms.customSkin;
	g.customShader = def->parms.customShader;
	memcpy( g.shaderParms, def->parms.shaderParms, sizeof( g.shaderParms ) );
	g.dark = false;
	g.regather = false;
	return added;
}

// The device buffer from the CPU table: header (count, total flux, CDF offset, mean flux), the triangles, the flux CDF
// (unnormalised running sum; E2 draws u * totalFlux and binary-searches). Replaces any previous buffer - DestroyBuffer
// retires it after the frames in flight that read it by address. false = no buffer (the caller decides what to drop).
static bool R_RtEmissiveUploadTable( rhi::RHI *r, int *bytesOut ) {
	const int n = s_rtEmissiveTris.Num();
	const int triBytes = n * (int)sizeof( rtEmissiveTriGPU_t );
	const int cdfBytes = n * (int)sizeof( float );
	const int bytes    = RT_EMISSIVE_HEADER_BYTES + triBytes + cdfBytes;
	if ( bytesOut ) { *bytesOut = bytes; }
	byte *blob = (byte *)Mem_Alloc16( bytes );
	float *cdf = (float *)( blob + RT_EMISSIVE_HEADER_BYTES + triBytes );
	double run = 0.0;
	for ( int i = 0; i < n; i++ ) {
		run += s_rtEmissiveTris[i].flux;
		cdf[i] = (float)run;
	}
	s_rtEmissiveTotalFlux = (float)run;
	const float meanFlux = ( n > 0 ) ? (float)( run / n ) : 0.0f;
	unsigned int *hdr = (unsigned int *)blob;
	hdr[0] = (unsigned int)n;
	memcpy( &hdr[1], &s_rtEmissiveTotalFlux, sizeof( float ) );
	hdr[2] = (unsigned int)( RT_EMISSIVE_HEADER_BYTES + triBytes );
	memcpy( &hdr[3], &meanFlux, sizeof( float ) );
	memcpy( blob + RT_EMISSIVE_HEADER_BYTES, s_rtEmissiveTris.Ptr(), triBytes );
	if ( s_rtEmissiveBuf != 0 ) {
		r->DestroyBuffer( s_rtEmissiveBuf );
		s_rtEmissiveBuf = 0;	s_rtEmissiveAddr = 0;
	}
	s_rtEmissiveBuf  = r->CreateBuffer( rhi::BU_STORAGE_UPLOAD, bytes, blob );	// CPU-write-only, read by the ray passes: VRAM
	s_rtEmissiveAddr = ( s_rtEmissiveBuf != 0 ) ? r->GetBufferDeviceAddress( s_rtEmissiveBuf ) : 0;
	Mem_Free16( blob );
	if ( s_rtEmissiveBuf != 0 && s_rtEmissiveAddr == 0 ) {
		r->DestroyBuffer( s_rtEmissiveBuf );
		s_rtEmissiveBuf = 0;
	}
	return s_rtEmissiveBuf != 0;
}

static void R_RtEmissiveBuild( const idRenderWorldLocal *world, rhi::RHI *r ) {
	const int msStart = Sys_Milliseconds();
	const float scale = R_RtEmissiveScale();
	idList<rtEmissiveCensus_t> census;
	int overflow = 0, worldTris = 0, entityTris = 0, entities = 0, screens = 0;
	s_rtEmissiveNotResident.Clear();
	idList<rtEmissiveExcluded_t> excluded;									// self-illuminated materials the gather rejected, with the reason
	auto noteExcluded = [&]( const idMaterial *m, const char *why ) { R_RtEmissiveNoteExcluded( &excluded, m, why ); };

	// worldspawn: the _areaN models (verts already world space)
	for ( int a = 0; a < world->NumAreas(); a++ ) {
		const idRenderModel *model = renderModelManager->FindModel( va( "_area%i", a ) );
		for ( int s = 0; model && s < model->NumSurfaces(); s++ ) {
			const modelSurface_t *surf = model->Surface( s );
			if ( surf->geometry == NULL || surf->geometry->verts == NULL || surf->geometry->indexes == NULL ) {
				continue;
			}
			if ( R_RtEmissiveIsScreen( surf->shader ) ) {
				screens++;
			}
			const char *why = "";
			const shaderStage_t *stage = R_RtEmissiveStage( surf->shader, &why );
			if ( stage == NULL ) {
				noteExcluded( surf->shader, why );
				continue;
			}
			const int before = s_rtEmissiveTris.Num();
			R_RtEmissiveAddSurface( surf->geometry, surf->shader, stage, NULL, NULL, 0u, scale, census, overflow, s_rtEmissiveTris, NULL );
			worldTris += s_rtEmissiveTris.Num() - before;
			if ( s_rtEmissiveTris.Num() > before ) { rtEmissiveLabel_t l; l.mat = surf->shader; l.firstTri = before; l.numTris = s_rtEmissiveTris.Num() - before; s_rtEmissiveLabels.Append( l ); }
		}
	}
	// E2 v3: one row per FIXTURE, not per material run - split the world runs into connected clusters
	int worldRunsSplit = 0, worldFixtures = 0, entityRunsSplit = 0, entityFixtures = 0;
	const int worldRuns = s_rtEmissiveLabels.Num();
	R_RtEmissiveSplitRuns( 0, -1, -1, worldRunsSplit, worldFixtures );
	if ( worldRunsSplit > 0 ) {
		R_RtEmissiveReport( "r_rtEmissiveTable: %d world material runs split into separate fixtures: %d emitters from %d runs\n", worldRunsSplit, worldFixtures, worldRuns );
	}
	// static entities (func_statics, inline brush models, movers at their load pose): model space x the entity
	// matrix, the SKIN-REMAPPED material (a fixture's on/off state is a skin), the entity's shaderParms (its _color)
	s_rtEmissiveSeenModel.SetNum( world->entityDefs.Num(), false );
	for ( int i = 0; i < world->entityDefs.Num(); i++ ) {
		const idRenderEntityLocal *def = world->entityDefs[i];
		s_rtEmissiveSeenModel[i] = R_RtEmissiveEntityGatherable( def ) ? def->parms.hModel : NULL;
		const int added = R_RtEmissiveGatherEntity( world, i, scale, census, overflow, screens, entityRunsSplit, entityFixtures, &excluded );
		if ( added > 0 ) {
			entityTris += added;
			entities++;
		}
	}
	if ( entityRunsSplit > 0 ) {
		R_RtEmissiveReport( "r_rtEmissiveTable: %d entity material runs split into separate fixtures: %d emitters\n", entityRunsSplit, entityFixtures );
	}

	const int n = s_rtEmissiveTris.Num();
	if ( n == 0 ) {
		R_RtEmissiveReport( "r_rtEmissiveTable: no emissive triangles on this map (%d screen surfaces excluded)\n", screens );
		return;
	}
	// flux CDF + the device buffer (R_RtEmissiveUploadTable)
	int bytes = 0;
	if ( !R_RtEmissiveUploadTable( r, &bytes ) ) {
		common->Warning( "r_rtEmissiveTable: the table buffer (%d KB) could not be created", bytes >> 10 );
		s_rtEmissiveTris.Clear();
		s_rtEmissiveTriSrc.Clear();
		return;
	}
	const float meanFlux = s_rtEmissiveTotalFlux / (float)n;

	// E1a census: totals + the strongest materials (what E2 would sample most)
	int animatedMats = 0;
	for ( int i = 0; i < census.Num(); i++ ) { if ( census[i].animated ) { animatedMats++; } }
	qsort( census.Ptr(), census.Num(), sizeof( rtEmissiveCensus_t ), R_RtCmpEmissiveCensus );
	R_RtEmissiveReport( "r_rtEmissiveTable: %d emissive triangles (%d world, %d from %d static entities = mover-tracked groups), %d materials (%d register-driven), %d screen surfaces excluded, total flux %.1f (mean %.2f), %d KB, %d ms%s\n",
		n, worldTris, entityTris, entities, census.Num(), animatedMats, screens, s_rtEmissiveTotalFlux, meanFlux, bytes >> 10,
		Sys_Milliseconds() - msStart, overflow ? va( " - TABLE FULL: %d triangles dropped (raise RT_EMISSIVE_MAX_TRIS)", overflow ) : "" );
	// DUDE: the per-material census and the excluded-material list run tens to ~250 lines; print the
	// full breakdown only with r_rtEmissiveTableDebug. A normal build keeps just the one-line summary
	// above (plus the excluded COUNT below), which is all that matters when you are not inspecting.
	if ( r_rtEmissiveTableDebug.GetInteger() > 0 ) {
		const int show = census.Num() < 64 ? census.Num() : 64;												// every material (a level has a few dozen): each painted tint gets a name
		for ( int i = 0; i < show; i++ ) {
			int rgb[3]; const char *hue = "";
			R_RtEmissiveDebugTint( census[i].slot, rgb, &hue );
			R_RtEmissiveReport( "    %6.1f%%  %6d tris  %-46s view1 %-10s #%02X%02X%02X%s%s%s\n", 100.0f * census[i].flux / s_rtEmissiveTotalFlux, census[i].tris,
				census[i].mat->GetName(), hue, rgb[0], rgb[1], rgb[2], census[i].animated ? "  (animated colour)" : "", census[i].texMatrix ? "  (texture matrix baked)" : "",
				census[i].clipped ? "  (clipped to the glow's extents)" : "" );
		}
	}
	if ( excluded.Num() > 0 ) {
		R_RtEmissiveReport( "r_rtEmissiveTable: %d self-illuminated materials excluded%s\n", excluded.Num(),
			r_rtEmissiveTableDebug.GetInteger() > 0 ? ":" : " (r_rtEmissiveTableDebug 1 lists them)" );
		if ( r_rtEmissiveTableDebug.GetInteger() > 0 ) {
			for ( int i = 0; i < excluded.Num(); i++ ) {
				R_RtEmissiveReport( "    %-38s %s\n", excluded[i].why, excluded[i].mat->GetName() );
			}
		}
	}
	// emitter mode: the FLARE sprites of emitting fixtures (a light entity's flare model: a sprite-deformed additive
	// material, its origin within 64 units of a table triangle) - hidden while r_rtEmissiveHideFlares is on
	s_rtEmissiveFlareHidden.SetNum( world->entityDefs.Num(), false );
	s_rtEmissiveFlareCount = 0;
	for ( int i = 0; i < world->entityDefs.Num(); i++ ) {
		s_rtEmissiveFlareHidden[i] = false;
		const idRenderEntityLocal *def = world->entityDefs[i];
		if ( def == NULL || def->parms.hModel == NULL ) {
			continue;
		}
		if ( !R_RtEmissiveIsFlareEntity( def ) ) {
			continue;
		}
		if ( R_RtEmissiveNearTable( def->parms.origin, 64.0f ) ) {
			s_rtEmissiveFlareHidden[i] = true;
			s_rtEmissiveFlareCount++;
		}
	}
	R_RtEmissiveReport( "r_rtEmissiveTable: %d flare sprites sit on emitters (hidden while r_rtEmissiveHideFlares is on)\n", s_rtEmissiveFlareCount );
}

// The debug overlay's vertices for table triangles [firstTri, firstTri + numTris): xyz world, st, normal = (flux,
// scale, triangle index), tangents[0] = (glow slot, flags, 0), color = the stage tint.
static void R_RtEmissiveFillDebugVerts( int firstTri, int numTris, idDrawVert *verts ) {
	memset( verts, 0, numTris * 3 * (int)sizeof( idDrawVert ) );
	for ( int i = 0; i < numTris; i++ ) {
		const rtEmissiveTriGPU_t &e = s_rtEmissiveTris[ firstTri + i ];
		const float *p[3] = { e.p0, e.p1, e.p2 };
		const unsigned int st[3] = { e.st0, e.st1, e.st2 };
		for ( int k = 0; k < 3; k++ ) {
			idDrawVert &v = verts[ i * 3 + k ];
			v.xyz.Set( p[k][0], p[k][1], p[k][2] );
			v.st[0] = R_RtF16toF32( st[k] & 0xFFFFu );
			v.st[1] = R_RtF16toF32( st[k] >> 16 );
			v.normal.Set( e.flux, e.scale, (float)( firstTri + i ) );
			v.tangents[0].Set( (float)( e.emit & 0xFFFFu ), (float)( e.emit >> 16 ), 0.0f );
			{
				const float m[3] = { R_RtF16toF32( e.meanRG & 0xFFFFu ), R_RtF16toF32( e.meanRG >> 16 ), R_RtF16toF32( e.meanB & 0xFFFFu ) };
				for ( int ch = 0; ch < 3; ch++ ) { v.color[ch] = (byte)( idMath::ClampFloat( 0.0f, 1.0f, m[ch] ) * 255.0f + 0.5f ); }	// the MEAN radiance E2 lights with (mode 3)
			}
			v.color[3] = (byte)( ( e.tint >> 24 ) & 0xFFu );
		}
	}
}

// E2 v3: (re)build the emitter ROWS from the labels (one per surface run). Called after the build and after any
// in-place change (state: flux; movers: positions). A row is one WHOLE surface run: a cell's slots must hold DISTINCT
// emitters (chunking a big tile into pieces let one tile fill every slot of the cells under it and starved every other
// light there - the dark Marine HQ of 2026-10-20).
// The TIME-AVERAGE luminance of a light material's live stage colours (summed like RB_RHI_GatherSceneLights does) with this
// light's own parms, or 0 when it does not change over time. A flash / flicker / pulse table (lights/square_flicker2:
// flashtable[Parm4 + time * .25 * Parm3] * Parm0..2) is sampled over 32 s; sound-driven colours read silence here (no
// emitter) and count as steady. Emissive mode drops a fixtured light, so its flicker would be lost: the gather scales
// the emitter row that replaced it by live / this average - same mean brightness, the light's own flicker back.
float R_RtEmissiveLightAnimatedAverage( const idRenderLightLocal *ld ) {
	const idMaterial *ls = ld->lightShader;
	const int numRegs = ( ls != NULL ) ? ls->GetNumRegisters() : 0;
	if ( numRegs <= 0 || numRegs > 4096 ) {
		return 0.0f;
	}
	float *regs = (float *)_alloca16( numRegs * (int)sizeof( float ) );
	viewDef_t probeView;
	memset( &probeView, 0, sizeof( probeView ) );
	const int SAMPLES = 64;
	float sum = 0.0f, lo = 1e30f, hi = 0.0f;
	for ( int k = 0; k < SAMPLES; k++ ) {
		probeView.floatTime = 0.11f + 0.5003f * (float)k;			// not a multiple of a table's period
		ls->EvaluateRegisters( regs, ld->parms.shaderParms, &probeView, NULL );
		float c[3] = { 0.0f, 0.0f, 0.0f };
		for ( int st = 0; st < ls->GetNumStages(); st++ ) {
			const shaderStage_t *stage = ls->GetStage( st );
			if ( stage == NULL || regs[ stage->conditionRegister ] == 0.0f ) { continue; }
			for ( int ch = 0; ch < 3; ch++ ) { c[ch] += Max( regs[ stage->color.registers[ch] ], 0.0f ); }
		}
		const float lum = c[0] * 0.2126f + c[1] * 0.7152f + c[2] * 0.0722f;
		sum += lum;	lo = Min( lo, lum );	hi = Max( hi, lum );
	}
	const float avg = sum / (float)SAMPLES;
	if ( !( avg > 1e-3f ) || hi - lo <= 0.02f * hi ) {
		return 0.0f;													// dark under the probe (sound-driven) or steady
	}
	return avg;
}

static void R_RtEmissiveBuildRows( void ) {
	s_rtEmissiveRows.SetNum( 0, false );
	if ( s_rtEmissiveLabels.Num() == 0 || s_rtEmissiveBuf == 0 ) {
		return;
	}
	for ( int i = 0; i < s_rtEmissiveLabels.Num(); i++ ) {
		const rtEmissiveLabel_t &l = s_rtEmissiveLabels[i];
		if ( l.numTris <= 0 || l.firstTri < 0 || l.firstTri + l.numTris > s_rtEmissiveTris.Num() ) {
			continue;
		}
		rtEmissiveRow_t row;
		idVec3 c( 0.0f, 0.0f, 0.0f ), mean( 0.0f, 0.0f, 0.0f );
		float flux = 0.0f, area = 0.0f;
		bool dark = true;
		for ( int t = 0; t < l.numTris; t++ ) {
			const rtEmissiveTriGPU_t &e = s_rtEmissiveTris[ l.firstTri + t ];
			const idVec3 p0( e.p0[0], e.p0[1], e.p0[2] ), p1( e.p1[0], e.p1[1], e.p1[2] ), p2( e.p2[0], e.p2[1], e.p2[2] );
			c += p0 + p1 + p2;
			area += 0.5f * ( ( p1 - p0 ).Cross( p2 - p0 ) ).Length();
			if ( e.flux > 0.0f ) {
				dark = false;
				// the mean linear radiance of this triangle (WITHOUT its material scale: the shade applies e.scale itself),
				// weighted by its flux (the big bright triangles decide the row's colour)
				const idVec3 m( R_RtF16toF32( e.meanRG & 0xFFFFu ), R_RtF16toF32( e.meanRG >> 16 ), R_RtF16toF32( e.meanB & 0xFFFFu ) );
				mean += m * e.flux;
				flux += e.flux;
			}
		}
		c *= 1.0f / ( 3.0f * l.numTris );
		float r2 = 0.0f;
		for ( int t = 0; t < l.numTris; t++ ) {
			const rtEmissiveTriGPU_t &e = s_rtEmissiveTris[ l.firstTri + t ];
			const float *pp[3] = { e.p0, e.p1, e.p2 };
			for ( int k = 0; k < 3; k++ ) { const float d2 = ( idVec3( pp[k][0], pp[k][1], pp[k][2] ) - c ).LengthSqr(); if ( d2 > r2 ) { r2 = d2; } }
		}
		// the cone of the EMITTING face normals (cross( p2 - p0, p1 - p0 ), the side rt_emissive.glsl calls front): the
		// area-weighted mean as the axis, the widest triangle around it as the half-angle. The DI target bounds the row's
		// emission toward a receiver with it (rtEmissiveTargetRow). A tube or a bulb has no useful cone (-1: no bound).
		idVec3 nSum( 0.0f, 0.0f, 0.0f );
		for ( int t = 0; t < l.numTris; t++ ) {
			const rtEmissiveTriGPU_t &e = s_rtEmissiveTris[ l.firstTri + t ];
			const idVec3 p0( e.p0[0], e.p0[1], e.p0[2] ), p1( e.p1[0], e.p1[1], e.p1[2] ), p2( e.p2[0], e.p2[1], e.p2[2] );
			nSum += ( p2 - p0 ).Cross( p1 - p0 );
		}
		row.coneAxis.Set( 0.0f, 0.0f, 1.0f );
		row.coneCos = -1.0f;
		if ( nSum.LengthSqr() > 1e-12f ) {
			row.coneAxis = nSum;
			row.coneAxis.Normalize();
			float cmin = 1.0f;
			for ( int t = 0; t < l.numTris; t++ ) {
				const rtEmissiveTriGPU_t &e = s_rtEmissiveTris[ l.firstTri + t ];
				const idVec3 p0( e.p0[0], e.p0[1], e.p0[2] ), p1( e.p1[0], e.p1[1], e.p1[2] ), p2( e.p2[0], e.p2[1], e.p2[2] );
				idVec3 n = ( p2 - p0 ).Cross( p1 - p0 );
				if ( n.LengthSqr() <= 1e-12f ) { continue; }						// a sliver: no direction (the shade rejects it too)
				n.Normalize();
				cmin = Min( cmin, n * row.coneAxis );
			}
			// a little slack for float error: the bound must never cut off a triangle that faces the receiver
			row.coneCos = idMath::ClampFloat( -1.0f, 1.0f, cmin - 1e-3f );
		}
		row.twoSidedTris = ( s_rtEmissiveTris[ l.firstTri ].emit & RT_EMIT_FLAG_TWOSIDED ) != 0u;	// one material per row
		row.centroid = c;
		row.boundRadius = idMath::Sqrt( r2 );
		row.flux = flux;
		row.area = area;
		row.meanRadiance = ( flux > 0.0f ) ? mean * ( 1.0f / flux ) : idVec3( 0.0f, 0.0f, 0.0f );
		row.fixtureId = R_RtEmissiveFixtureId( c );	// the emitter surface's own centroid (unique per fixture, unlike a shared light origin)
		row.mat = l.mat;
		row.firstTri = l.firstTri;
		row.numTris = l.numTris;
		row.dark = dark;
		s_rtEmissiveRows.Append( row );
	}
}

// ANIMATED GLOWS (2026-11-01): a glow stage whose colour is TIME-driven (a table, a sine - RT_EMIT_FLAG_ANIMATED) was baked
// at its t = 0 colour and never moved: a pulsing panel lit the room at one constant level. Once per frame (frontend) each
// such row's stage colour is evaluated at the view's time with the surface's live parms and divided by the baked colour
// (the same t = 0 snapshot and 0.25 floor R_RtEmissiveAddSurface used); the gather multiplies the row's tint by it.

static void R_RtEmissiveUpdateAnimatedRows( const idRenderWorldLocal *world ) {
	const int n = s_rtEmissiveRows.Num();
	s_rtEmissiveRowAnim.SetNum( n, false );
	for ( int ri = 0; ri < n; ri++ ) {
		s_rtEmissiveRowAnim[ri].Set( -1.0f, 0.0f, 0.0f );
		const rtEmissiveRow_t &row = s_rtEmissiveRows[ri];
		if ( row.numTris <= 0 || row.firstTri < 0 || row.firstTri >= s_rtEmissiveTris.Num() || row.mat == NULL || tr.viewDef == NULL ) { continue; }
		const unsigned int emit = s_rtEmissiveTris[ row.firstTri ].emit;
		if ( ( emit & RT_EMIT_FLAG_ANIMATED ) == 0u ) { continue; }
		const float *parms = NULL;											// world surfaces: the default parms (as the bake)
		if ( ( emit & RT_EMIT_FLAG_ENTITY ) != 0u ) {
			for ( int gi = 0; gi < s_rtEmissiveGroups.Num(); gi++ ) {
				const rtEmissiveGroup_t &g = s_rtEmissiveGroups[gi];
				if ( row.firstTri < g.firstTri || row.firstTri >= g.firstTri + g.numTris ) { continue; }
				if ( g.entityIndex >= 0 && g.entityIndex < world->entityDefs.Num() && world->entityDefs[ g.entityIndex ] != NULL ) {
					parms = world->entityDefs[ g.entityIndex ]->parms.shaderParms;
				}
				break;
			}
		}
		const shaderStage_t *st = R_RtEmissiveStage( row.mat, NULL, parms );
		const int numRegs = row.mat->GetNumRegisters();
		if ( st == NULL || numRegs <= 0 || numRegs > 4096 ) { continue; }
		float baked[3];
		R_RtEmissiveStageColor( row.mat, st, parms, baked );				// the t = 0 snapshot the bake used
		if ( R_RtLuminance( baked ) < 0.25f ) { baked[0] = baked[1] = baked[2] = 0.25f; }	// ...and its floor
		static const float defaultParms[MAX_ENTITY_SHADER_PARMS] = { 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		float *regs = (float *)_alloca16( numRegs * (int)sizeof( float ) );
		row.mat->EvaluateRegisters( regs, parms != NULL ? parms : defaultParms, tr.viewDef, NULL );	// the view's time + global parms
		idVec3 ratio;
		for ( int k = 0; k < 3; k++ ) {
			const int r = st->color.registers[k];
			const float live = ( r >= 0 && r < numRegs ) ? idMath::ClampFloat( 0.0f, 16.0f, regs[r] ) : 1.0f;
			ratio[k] = live / Max( baked[k], 1e-3f );
		}
		s_rtEmissiveRowAnim[ri] = ratio;
	}
}

bool R_RtEmissiveRowAnimTint( int row, float ratio[3] ) {
	if ( row < 0 || row >= s_rtEmissiveRowAnim.Num() || s_rtEmissiveRowAnim[row].x < 0.0f ) { return false; }
	ratio[0] = s_rtEmissiveRowAnim[row].x; ratio[1] = s_rtEmissiveRowAnim[row].y; ratio[2] = s_rtEmissiveRowAnim[row].z;
	return true;
}

int R_RtEmissiveRows( const rtEmissiveRow_t **rows ) {
	if ( rows ) { *rows = s_rtEmissiveRows.Ptr(); }
	return ( s_rtEmissiveBuf != 0 ) ? s_rtEmissiveRows.Num() : 0;
}

// Recompute the flux CDF + header over the CPU table and upload them (after an in-place flux change).
static void R_RtEmissiveUploadCdf( rhi::RHI *r ) {
	const int n = s_rtEmissiveTris.Num();
	if ( n == 0 || s_rtEmissiveBuf == 0 ) {
		return;
	}
	static idList<float> cdf;
	cdf.SetNum( n, false );
	double run = 0.0;
	for ( int i = 0; i < n; i++ ) {
		run += s_rtEmissiveTris[i].flux;
		cdf[i] = (float)run;
	}
	s_rtEmissiveTotalFlux = (float)run;
	const float meanFlux = (float)( run / n );
	const int triBytes = n * (int)sizeof( rtEmissiveTriGPU_t );
	unsigned int hdr[4];
	hdr[0] = (unsigned int)n;
	memcpy( &hdr[1], &s_rtEmissiveTotalFlux, sizeof( float ) );
	hdr[2] = (unsigned int)( RT_EMISSIVE_HEADER_BYTES + triBytes );
	memcpy( &hdr[3], &meanFlux, sizeof( float ) );
	r->UpdateBuffer( s_rtEmissiveBuf, 0, RT_EMISSIVE_HEADER_BYTES, hdr );
	r->UpdateBuffer( s_rtEmissiveBuf, RT_EMISSIVE_HEADER_BYTES + triBytes, n * (int)sizeof( float ), cdf.Ptr() );
}

// The vanilla fixture switch folded in: the entity's glow state (skin / custom material / shader parms - a light shot
// out, a trigger, a parm7 red->green door light) changed since the slice was baked. Re-gather the entity with its
// CURRENT state: the same surfaces glowing (same triangle count) rewrite the slice in place (new glow slot, tint,
// flux - and positions, so this doubles as a move); a state that glows NOWHERE keeps the slice at zero flux with
// RT_EMIT_FLAG_DARK (the CDF never draws it, the overlay hides it) and comes back the same way; any other change
// (different surfaces glow) is a full rebuild. Returns true when the table changed.
static bool R_RtEmissiveRegatherGroup( rtEmissiveGroup_t &g, const idRenderEntityLocal *def, rhi::RHI *r ) {
	static idList<rtEmissiveTriGPU_t> scratch;
	static idList<idVec3> scratchLocal;
	static idList<rtEmissiveCensus_t> scratchCensus;
	g.regather = false;
	scratch.SetNum( 0, false );
	scratchLocal.SetNum( 0, false );
	scratchCensus.SetNum( 0, false );
	const float scale = R_RtEmissiveScale();
	float modelMatrix[16];
	R_AxisToModelMatrix( def->parms.axis, def->parms.origin, modelMatrix );
	int overflow = 0;
	const idRenderModel *model = def->parms.hModel;
	for ( int s = 0; s < model->NumSurfaces(); s++ ) {
		const modelSurface_t *surf = model->Surface( s );
		if ( surf->geometry == NULL || surf->geometry->verts == NULL || surf->geometry->indexes == NULL ) {
			continue;
		}
		const idMaterial *mat = R_RemapShaderBySkin( surf->shader, def->parms.customSkin, def->parms.customShader );
		const shaderStage_t *stage = R_RtEmissiveStage( mat, NULL, def->parms.shaderParms );
		if ( stage == NULL ) {
			continue;
		}
		R_RtEmissiveAddSurface( surf->geometry, mat, stage, modelMatrix, def->parms.shaderParms, RT_EMIT_FLAG_ENTITY, scale, scratchCensus, overflow, scratch, &scratchLocal );
	}
	g.skin = def->parms.customSkin;
	g.customShader = def->parms.customShader;
	memcpy( g.shaderParms, def->parms.shaderParms, sizeof( g.shaderParms ) );
	memcpy( g.lastMatrix, modelMatrix, sizeof( g.lastMatrix ) );
	if ( scratch.Num() == 0 ) {
		if ( !g.dark ) {														// went DARK: keep the slice, zero flux
			for ( int t = 0; t < g.numTris; t++ ) {
				rtEmissiveTriGPU_t &e = s_rtEmissiveTris[ g.firstTri + t ];
				e.flux = 0.0f;
				e.emit |= RT_EMIT_FLAG_DARK;
			}
			g.dark = true;
			if ( r_rtEmissiveTableDebug.GetInteger() > 0 ) {	// DUDE: the overlay's companion; a conveyor would print every cycle
				common->Printf( "r_rtEmissiveTable: entity %d '%s' switched OFF (%d emissive tris kept dark)\n", g.entityIndex, model->Name(), g.numTris );
			}
			return true;
		}
		return false;
	}
	if ( scratch.Num() != g.numTris ) {
		if ( Sys_Milliseconds() - s_rtEmissiveLastRebuildMs > 2000 ) {
			R_RtEmissiveLog( "r_rtEmissiveTable: entity %d '%s' changed which surfaces glow (%d -> %d tris) - rebuilding the table\n", g.entityIndex, model->Name(), g.numTris, scratch.Num() );
			s_rtEmissiveMap.Clear();											// stale next frame: a full rebuild (throttled: a flipping entity must not rebuild every frame)
		}
		return false;
	}
	// the scratch is in GATHER order, the slice in the fixture split's order (R_RtEmissiveSplitRuns): map each slot
	// through its source index (out of range = identity: a slice the split never touched)
	for ( int t = 0; t < g.numTris; t++ ) {
		const int at = g.firstTri + t;
		int src = ( at < s_rtEmissiveTriSrc.Num() ) ? s_rtEmissiveTriSrc[ at ] - g.firstTri : t;
		if ( src < 0 || src >= g.numTris ) { src = t; }
		s_rtEmissiveTris[ at ] = scratch[ src ];
		for ( int k = 0; k < 3; k++ ) {
			s_rtEmissiveLocalVerts[ g.firstLocalVert + 3 * t + k ] = scratchLocal[ 3 * src + k ];
		}
	}
	if ( g.dark ) {
		if ( r_rtEmissiveTableDebug.GetInteger() > 0 ) {	// DUDE: the overlay's companion; a conveyor would print every cycle
			common->Printf( "r_rtEmissiveTable: entity %d '%s' switched ON again\n", g.entityIndex, model->Name() );
		}
	}
	g.dark = false;
	return true;
}

// E1e: once per frame, re-bake the world positions of every entity-sourced run whose entity has MOVED since its
// last bake (doors, lifts, platforms, rotating props) and rewrite that slice of the table (and of the debug
// vertices, when built) in place. UpdateBuffer is a memcpy into the host-visible mapping, so a frame already in
// flight may read a half-updated slice: one emitter a frame off its true position - harmless for lighting (the
// visibility ray traces the real TLAS), never a crash. A still entity costs a 16-float compare. An entity whose
// slot was freed or re-used by another model is left where it was (static emitters only enter the table at build).
static void R_RtEmissiveRefreshMoversBody( const idRenderWorldLocal *world, rhi::RHI *r ) {
	static int lastFrame = -1;
	if ( tr.frameCount == lastFrame || s_rtEmissiveBuf == 0 ) {
		return;
	}
	lastFrame = tr.frameCount;
	static idList<idDrawVert> dbgScratch;
	bool cdfDirty = false, movedThisFrame = false;
	for ( int gi = 0; gi < s_rtEmissiveGroups.Num(); gi++ ) {
		rtEmissiveGroup_t &g = s_rtEmissiveGroups[gi];
		if ( g.entityIndex < 0 || g.entityIndex >= world->entityDefs.Num() ) {
			continue;
		}
		const idRenderEntityLocal *def = world->entityDefs[ g.entityIndex ];
		if ( def == NULL || def->parms.hModel != g.model || def->parms.callback != NULL ) {
			// the entity is GONE (a pickup taken, an item hidden: the game frees its render entity) or its slot now
			// holds another model: the slice goes dark (zero flux, never sampled, hidden in the overlay). If the same
			// model returns to the slot (a respawned pickup) the state compare below re-gathers it back on.
			if ( !g.dark ) {
				for ( int t = 0; t < g.numTris; t++ ) {
					rtEmissiveTriGPU_t &e = s_rtEmissiveTris[ g.firstTri + t ];
					e.flux = 0.0f;
					e.emit |= RT_EMIT_FLAG_DARK;
				}
				g.dark = true;
				g.skin = NULL; g.customShader = NULL; memset( g.shaderParms, 0, sizeof( g.shaderParms ) );
				g.regather = true;		// a return re-gathers it, whatever its state (all-zero parms would compare equal)
				r->UpdateBuffer( s_rtEmissiveBuf, RT_EMISSIVE_HEADER_BYTES + g.firstTri * (int)sizeof( rtEmissiveTriGPU_t ),
					g.numTris * (int)sizeof( rtEmissiveTriGPU_t ), &s_rtEmissiveTris[ g.firstTri ] );
				if ( s_rtEmissiveDbgVB != 0 ) {
					dbgScratch.SetNum( g.numTris * 3, false );
					R_RtEmissiveFillDebugVerts( g.firstTri, g.numTris, dbgScratch.Ptr() );
					r->UpdateBuffer( s_rtEmissiveDbgVB, g.firstTri * 3 * (int)sizeof( idDrawVert ), g.numTris * 3 * (int)sizeof( idDrawVert ), dbgScratch.Ptr() );
				}
				cdfDirty = true;
				if ( r_rtEmissiveTableDebug.GetInteger() > 0 ) {	// DUDE: the overlay's companion; a conveyor would print every cycle
					common->Printf( "r_rtEmissiveTable: entity %d '%s' is gone (%d emissive tris kept dark)\n", g.entityIndex, g.model->Name(), g.numTris );
				}
			}
			continue;
		}
		// the glow STATE: skin / custom material / shader parms (parm7 door-light colour, a scripted _color) changed?
		if ( g.regather || def->parms.customSkin != g.skin || def->parms.customShader != g.customShader
				|| memcmp( def->parms.shaderParms, g.shaderParms, sizeof( g.shaderParms ) ) != 0 ) {
			if ( R_RtEmissiveRegatherGroup( g, def, r ) ) {
				r->UpdateBuffer( s_rtEmissiveBuf, RT_EMISSIVE_HEADER_BYTES + g.firstTri * (int)sizeof( rtEmissiveTriGPU_t ),
					g.numTris * (int)sizeof( rtEmissiveTriGPU_t ), &s_rtEmissiveTris[ g.firstTri ] );
				if ( s_rtEmissiveDbgVB != 0 ) {
					dbgScratch.SetNum( g.numTris * 3, false );
					R_RtEmissiveFillDebugVerts( g.firstTri, g.numTris, dbgScratch.Ptr() );
					r->UpdateBuffer( s_rtEmissiveDbgVB, g.firstTri * 3 * (int)sizeof( idDrawVert ), g.numTris * 3 * (int)sizeof( idDrawVert ), dbgScratch.Ptr() );
				}
				cdfDirty = true;
			}
			if ( s_rtEmissiveMap.Length() == 0 ) {
				return;														// a full rebuild is queued: nothing more to patch this frame
			}
			continue;														// positions are re-baked by the regather
		}
		float mm[16];
		R_AxisToModelMatrix( def->parms.axis, def->parms.origin, mm );
		if ( memcmp( mm, g.lastMatrix, sizeof( mm ) ) == 0 ) {
			continue;
		}
		memcpy( g.lastMatrix, mm, sizeof( mm ) );
		if ( !g.reported ) {													// name each mover the first time it moves (the E1e test aid)
			g.reported = true;
			R_RtEmissiveLog( "r_rtEmissiveTable: mover re-baked: entity %d '%s' (%d emissive tris, origin %.0f %.0f %.0f)\n",
				g.entityIndex, g.model->Name(), g.numTris, def->parms.origin.x, def->parms.origin.y, def->parms.origin.z );
		}
		for ( int t = 0; t < g.numTris; t++ ) {
			rtEmissiveTriGPU_t &e = s_rtEmissiveTris[ g.firstTri + t ];
			const idVec3 *lv = &s_rtEmissiveLocalVerts[ g.firstLocalVert + t * 3 ];
			idVec3 p0, p1, p2;
			R_LocalPointToGlobal( mm, lv[0], p0 );
			R_LocalPointToGlobal( mm, lv[1], p1 );
			R_LocalPointToGlobal( mm, lv[2], p2 );
			e.p0[0] = p0.x; e.p0[1] = p0.y; e.p0[2] = p0.z;
			e.p1[0] = p1.x; e.p1[1] = p1.y; e.p1[2] = p1.z;
			e.p2[0] = p2.x; e.p2[1] = p2.y; e.p2[2] = p2.z;
		}
		r->UpdateBuffer( s_rtEmissiveBuf, RT_EMISSIVE_HEADER_BYTES + g.firstTri * (int)sizeof( rtEmissiveTriGPU_t ),
			g.numTris * (int)sizeof( rtEmissiveTriGPU_t ), &s_rtEmissiveTris[ g.firstTri ] );
		if ( s_rtEmissiveDbgVB != 0 ) {
			dbgScratch.SetNum( g.numTris * 3, false );
			R_RtEmissiveFillDebugVerts( g.firstTri, g.numTris, dbgScratch.Ptr() );
			r->UpdateBuffer( s_rtEmissiveDbgVB, g.firstTri * 3 * (int)sizeof( idDrawVert ), g.numTris * 3 * (int)sizeof( idDrawVert ), dbgScratch.Ptr() );
		}
		s_rtEmissiveMovedGroups++;
		movedThisFrame = true;
	}
	if ( cdfDirty ) {
		R_RtEmissiveUploadCdf( r );												// flux changed somewhere: the sampling CDF follows
	}
	if ( cdfDirty || movedThisFrame ) {
		R_RtEmissiveBuildRows();												// the emitter rows follow flux and position changes
	}
}

// DUDE: the per-frame mover upkeep is map-wide (any emissive mover anywhere re-bakes its slice, and any change
// regenerates the emitter rows the light grid reads), so it runs once per frame however many views draw.
static void R_RtEmissiveRefreshMovers( const idRenderWorldLocal *world, rhi::RHI *r ) {
	static int lastFrame = -1;
	if ( tr.frameCount == lastFrame ) {
		return;
	}
	lastFrame = tr.frameCount;
	R_RtEmissiveRefreshMoversBody( world, r );
}

// A group whose entity is gone: its slot was freed, or now holds another model (R_RtEmissiveRefreshMovers darkened it).
static bool R_RtEmissiveGroupGone( const idRenderWorldLocal *world, const rtEmissiveGroup_t &g ) {
	if ( g.entityIndex < 0 || g.entityIndex >= world->entityDefs.Num() ) {
		return true;
	}
	const idRenderEntityLocal *def = world->entityDefs[ g.entityIndex ];
	return def == NULL || def->parms.hModel != g.model || def->parms.callback != NULL;
}

// DUDE (2026-10-27): an entity the game hides and shows again comes back in a NEW render entity slot (Show() adds a
// fresh entityDef; FindNull hands out the lowest free one) - mc_underground's canister conveyor does it every 20-30 s.
// Its old group went dark when the slot was freed; the same model's triangles are the same, so the newcomer ADOPTS
// that group (the nearest one, if several) and the next refresh re-gathers it in place, instead of a full rebuild
// (gathering every emissive triangle of the map again, re-creating the GPU buffers: a hitch per cycle).
static rtEmissiveGroup_t *R_RtEmissiveAdoptGroup( const idRenderWorldLocal *world, int entityIndex, const idRenderEntityLocal *def ) {
	rtEmissiveGroup_t *best = NULL;
	float bestDist = idMath::INFINITY;
	for ( int gi = 0; gi < s_rtEmissiveGroups.Num(); gi++ ) {
		rtEmissiveGroup_t &g = s_rtEmissiveGroups[gi];
		if ( !g.dark || g.model != def->parms.hModel || !R_RtEmissiveGroupGone( world, g ) ) {
			continue;
		}
		const idVec3 last( g.lastMatrix[12], g.lastMatrix[13], g.lastMatrix[14] );
		const float d = ( def->parms.origin - last ).LengthSqr();
		if ( d < bestDist ) {
			bestDist = d;
			best = &g;
		}
	}
	if ( best == NULL ) {
		return NULL;
	}
	// the old slot is examined again if a model ever lands there (it no longer has a group)
	if ( best->entityIndex >= 0 && best->entityIndex < s_rtEmissiveSeenModel.Num() && s_rtEmissiveSeenModel[ best->entityIndex ] == best->model ) {
		s_rtEmissiveSeenModel[ best->entityIndex ] = NULL;
	}
	R_RtEmissiveSeenGrow( entityIndex );
	s_rtEmissiveSeenModel[ entityIndex ] = def->parms.hModel;
	best->entityIndex = entityIndex;
	best->regather = true;
	return best;
}

// True when an entity slot NO group tracks holds a static model with an emissive surface (it appeared after the
// build) that no dark group of the same model could adopt: only that needs a full rebuild. Tracked slots are skipped
// (their state compare handles them). A few hundred entities x their surfaces; the stage test evaluates material
// registers, so this runs only every 90 frames.
// An emissive static entity that APPEARED after the build (a spawned prop, a dropped item, a weapon's glowing part): its
// triangles go to the END of the table - the world and every other entity keep their slices, rows and fixture ids - then
// the GPU table is re-uploaded (the old buffer retires after the frames in flight) and the rows are rebuilt. The full
// rebuild this replaces re-gathered the whole map and re-ran the flare pass: 14-47 ms per new entity, growing with the table,
// eleven times in the demo1 timedemo (r_hitchTrace, 2026-11-02). false = the upload failed: the caller rebuilds.
static bool R_RtEmissiveAppendEntity( const idRenderWorldLocal *world, rhi::RHI *r, int i ) {
	const double t0 = Sys_MillisecondsPrecise();
	const float scale = R_RtEmissiveScale();
	idList<rtEmissiveCensus_t> census;
	int overflow = 0, screens = 0, runsSplit = 0, fixtures = 0;
	const int added = R_RtEmissiveGatherEntity( world, i, scale, census, overflow, screens, runsSplit, fixtures, NULL );
	R_RtEmissiveSeenGrow( i );
	s_rtEmissiveSeenModel[i] = world->entityDefs[i] ? world->entityDefs[i]->parms.hModel : NULL;
	if ( added <= 0 ) {
		return true;				// the scan's emissive stage gathers nothing here (clipped away / overflow): nothing to add
	}
	int bytes = 0;
	if ( r == NULL || !R_RtEmissiveUploadTable( r, &bytes ) ) {
		return false;
	}
	if ( s_rtEmissiveDbgVB != 0 || s_rtEmissiveDbgIB != 0 ) {	// the debug overlay's vertex copy: rebuilt on its next use
		if ( s_rtEmissiveDbgVB != 0 ) { r->DestroyBuffer( s_rtEmissiveDbgVB ); }
		if ( s_rtEmissiveDbgIB != 0 ) { r->DestroyBuffer( s_rtEmissiveDbgIB ); }
		s_rtEmissiveDbgVB = s_rtEmissiveDbgIB = 0;	s_rtEmissiveDbgIndexes = 0;
	}
	R_RtEmissiveBuildRows();
	const idRenderModel *model = world->entityDefs[i]->parms.hModel;
	R_RtEmissiveLog( "r_rtEmissiveTable: entity %d '%s' appeared - %d emissive tris appended (%d fixtures; %d in the table, %d KB), %.1f ms%s\n",
		i, model ? model->Name() : "?", added, fixtures, s_rtEmissiveTris.Num(), bytes >> 10, Sys_MillisecondsPrecise() - t0,
		overflow ? va( " - TABLE FULL: %d triangles dropped", overflow ) : "" );
	return true;
}

// The entity slots that hold an emissive static model the table does not track yet (newEntities); a returning entity adopts
// its dark group instead (R_RtEmissiveAdoptGroup) and is not listed.
static void R_RtEmissiveScanNewEntities( const idRenderWorldLocal *world, idList<int> &newEntities ) {
	newEntities.SetNum( 0, false );
	// the model each slot's group tracks (live or dark): a slot whose group has this very model is the refresh's - it re-gathers
	// a returning model back on (R_RtEmissiveRefreshMoversBody) - and must NOT be appended again. Without this, a slot that
	// held another model in between (the seen-model cache moved on) got a second group for the same entity, both live: double
	// emission (mfscompcell in the demo1 timedemo, 2026-11-02; the full rebuild this path replaced always started clean).
	static idList<const idRenderModel *> trackedModel;
	trackedModel.SetNum( world->entityDefs.Num(), false );
	for ( int i = 0; i < trackedModel.Num(); i++ ) {
		trackedModel[i] = NULL;
	}
	for ( int gi = 0; gi < s_rtEmissiveGroups.Num(); gi++ ) {
		const rtEmissiveGroup_t &g = s_rtEmissiveGroups[gi];
		if ( g.entityIndex >= 0 && g.entityIndex < trackedModel.Num() && g.model != NULL ) {
			trackedModel[ g.entityIndex ] = g.model;
		}
	}
	for ( int i = 0; i < world->entityDefs.Num(); i++ ) {
		const idRenderEntityLocal *def = world->entityDefs[i];
		if ( !R_RtEmissiveEntityGatherable( def ) ) {
			continue;
		}
		if ( trackedModel[i] == def->parms.hModel ) {
			R_RtEmissiveSeenGrow( i );
			s_rtEmissiveSeenModel[i] = def->parms.hModel;
			continue;
		}
		// the build examined this slot with this very model: whatever it found (emitters, tracked; or nothing) stands
		if ( i < s_rtEmissiveSeenModel.Num() && s_rtEmissiveSeenModel[i] == def->parms.hModel ) {
			continue;
		}
		const idRenderModel *model = def->parms.hModel;
		bool emissive = false;
		for ( int s = 0; s < model->NumSurfaces() && !emissive; s++ ) {
			const modelSurface_t *surf = model->Surface( s );
			if ( surf->geometry == NULL || surf->geometry->verts == NULL || surf->geometry->indexes == NULL ) {
				continue;
			}
			const idMaterial *mat = R_RemapShaderBySkin( surf->shader, def->parms.customSkin, def->parms.customShader );
			emissive = R_RtEmissiveStage( mat, NULL, def->parms.shaderParms ) != NULL;
		}
		if ( !emissive ) {
			// examined and dark, as the build records it: not tested again every 90 frames
			R_RtEmissiveSeenGrow( i );
			s_rtEmissiveSeenModel[i] = model;
		} else if ( R_RtEmissiveAdoptGroup( world, i, def ) == NULL ) {
			newEntities.Append( i );
		}
	}
}

// Keep the table in step with the live RT scene: build once per map (or when its cvars change), free when off.
// Called from R_RtWorldUpdate while the persistent scene is live for `world`.
void R_RtEmissiveUpdate( const idRenderWorldLocal *world, rhi::RHI *r ) {
	R_EmAuthLookFrame( world, tr.viewDef );			// L2 look mode (editLights): the hover pick + gizmos (no-op when off)
	if ( !r_rtEmissiveTable.GetBool() && !r_rtEmissiveLighting.GetBool() ) {		// D6: the mode implies the table (the bounce alone does not: it needs r_rtEmissiveTable)
		if ( s_rtEmissiveMap.Length() > 0 || s_rtEmissiveBuf != 0 ) {
			R_RtEmissiveFree();
		}
		r_rtEmissiveTable.ClearModified();
		r_rtEmissiveLighting.ClearModified();
		r_rtEmissiveScale.ClearModified();
		return;
	}
	const bool stale = s_rtEmissiveMap.Icmp( world->mapName ) != 0 || r_rtEmissiveTable.IsModified() || r_rtEmissiveLighting.IsModified() || r_rtEmissiveScale.IsModified();
	if ( !stale ) {
		R_RtEmissiveRefreshMovers( world, r );	// E1e: moved doors / lifts / platforms re-bake their slice
		R_RtEmissiveUpdateAnimatedRows( world );	// time-driven glow colours: this frame's live / baked ratio per row
		if ( r_rtEmissiveTableDebug.GetInteger() > 0 ) {
			R_RtEmissiveProjectLabels( tr.viewDef );	// E1d: the overlay's material names (frontend: the area stamps are current; not over the light spheres)
		} else {
			s_rtEmissiveScreenLabels.SetNum( 0, false );
		}
		// an emissive entity that APPEARED after the build (a dropped item, a respawned pickup, a spawned prop): every 90
		// frames, scan the entity slots no group tracks for a static model with an emissive surface -> APPENDED to the table
		// (R_RtEmissiveAppendEntity); a returning entity adopts its dark group instead - R_RtEmissiveAdoptGroup. Only a
		// failed append falls back to the full rebuild (rate-limited as before).
		if ( ( tr.frameCount % 90 ) == 45 ) {
			static idList<int> newEntities;
			R_RtEmissiveScanNewEntities( world, newEntities );
			bool appended = true;
			for ( int k = 0; k < newEntities.Num() && appended; k++ ) {
				appended = R_RtEmissiveAppendEntity( world, r, newEntities[k] );
			}
			if ( !appended && Sys_Milliseconds() - s_rtEmissiveLastRebuildMs > 5000 ) {
				R_RtEmissiveLog( "r_rtEmissiveTable: a new emissive entity appeared and could not be appended - rebuilding the table\n" );
				s_rtEmissiveMap.Clear();
				return;
			}
		}
		// a glow image that was not on the GPU at build time (loaded on its first draw) joins by a rebuild once it is:
		// checked every 90 frames, a handful of pointer reads
		if ( s_rtEmissiveNotResident.Num() > 0 && ( tr.frameCount % 90 ) == 0 ) {
			for ( int i = 0; i < s_rtEmissiveNotResident.Num(); i++ ) {
				if ( s_rtEmissiveNotResident[i]->rhiHandle != 0 ) {
					R_RtEmissiveLog( "r_rtEmissiveTable: glow image '%s' is now resident - rebuilding the table\n", s_rtEmissiveNotResident[i]->imgName.c_str() );
					s_rtEmissiveMap.Clear();			// stale next frame
					break;
				}
			}
		}
		return;
	}
	// the full report: a new map, a cvar the user changed, or the debug overlay; a rebuild the map asked for itself
	// (a fixture changed which surfaces glow, a new emissive entity, a glow image now resident) prints one line
	// DUDE: stay silent when the table is built but nothing consumes it - r_rtEmissiveTable pinned on
	// with Emissive Mode and the GI-bounce rows off, and no one debugging. The build still runs (so the
	// debug overlay can be toggled), it just does not print the census on every map load when emissive
	// is effectively off. The report is for when emissive is actually in use, or you are inspecting it.
	const bool emissiveInUse = r_rtEmissiveLighting.GetBool() || r_rtEmissiveBounce.GetBool() || r_rtEmissiveTableDebug.GetInteger() > 0;
	s_rtEmissiveReport = emissiveInUse && ( s_rtEmissiveReportedMap.Icmp( world->mapName ) != 0 || r_rtEmissiveTable.IsModified()
		|| r_rtEmissiveLighting.IsModified() || r_rtEmissiveScale.IsModified() || r_rtEmissiveTableDebug.GetInteger() > 0 );
	r_rtEmissiveTable.ClearModified();
	r_rtEmissiveLighting.ClearModified();
	r_rtEmissiveScale.ClearModified();
	const double buildStart = Sys_MillisecondsPrecise();
	R_RtEmissiveFree();
	s_rtEmissiveLastRebuildMs = Sys_Milliseconds();
	R_RtEmissiveBuild( world, r );
	if ( s_rtEmissiveBuf != 0 ) {
		R_RtEmissiveBuildRows();				// E2 v3: the emitter rows the scene-light gather appends
	}
	if ( !s_rtEmissiveReport && emissiveInUse ) {		// a self-triggered rebuild while emissive is in use: one timed line; silent when emissive is off
		R_RtEmissiveLog( "r_rtEmissiveTable: rebuilt in %.1f ms (%d emissive triangles)\n", Sys_MillisecondsPrecise() - buildStart, s_rtEmissiveTris.Num() );
	}
	s_rtEmissiveReport = true;				// the console commands' own output is never muted
	s_rtEmissiveReportedMap = world->mapName;
	s_rtEmissiveMap = world->mapName;		// the latch: an empty map builds nothing but is not retried every frame
}

/*
====================
R_RtEmissiveLog
====================
*/
void R_RtEmissiveLog( const char *fmt, ... ) {
	char text[MAX_STRING_CHARS];
	va_list argptr;
	va_start( argptr, fmt );
	idStr::vsnPrintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );
	if ( r_rtEmissiveTableDebug.GetInteger() > 0 ) {
		common->Printf( "%s", text );
	} else {
		common->DPrintf( "%s", text );
	}
}

unsigned long long R_RtEmissiveTableAddress( int *numTris, float *totalFlux, float *meanFlux ) {
	const int n = s_rtEmissiveTris.Num();
	if ( numTris )   { *numTris = ( s_rtEmissiveBuf != 0 ) ? n : 0; }
	if ( totalFlux ) { *totalFlux = s_rtEmissiveTotalFlux; }
	if ( meanFlux )  { *meanFlux = ( n > 0 ) ? s_rtEmissiveTotalFlux / n : 0.0f; }
	return ( s_rtEmissiveBuf != 0 ) ? s_rtEmissiveAddr : 0;
}

// The debug overlay's geometry: one idDrawVert per table vertex (xyz world, st, normal = (flux, scale, triangle
// index), tangents[0] = (glow slot, flags, 0), color = the stage tint) + a plain index list. Built on first use.
bool R_RtEmissiveDebugGeometry( unsigned int *vb, unsigned int *ib, int *numIndexes ) {
	const int n = s_rtEmissiveTris.Num();
	rhi::RHI *r = rhi::GetRHI();
	if ( n == 0 || s_rtEmissiveBuf == 0 || r == NULL ) {
		return false;
	}
	if ( s_rtEmissiveDbgVB == 0 ) {
		idDrawVert *verts = (idDrawVert *)Mem_Alloc16( n * 3 * (int)sizeof( idDrawVert ) );
		glIndex_t  *idx   = (glIndex_t *)Mem_Alloc16( n * 3 * (int)sizeof( glIndex_t ) );
		R_RtEmissiveFillDebugVerts( 0, n, verts );
		for ( int i = 0; i < n * 3; i++ ) {
			idx[i] = i;
		}
		s_rtEmissiveDbgVB = r->CreateBuffer( rhi::BU_VERTEX, n * 3 * (int)sizeof( idDrawVert ), verts );
		s_rtEmissiveDbgIB = r->CreateBuffer( rhi::BU_INDEX, n * 3 * (int)sizeof( glIndex_t ), idx );
		s_rtEmissiveDbgIndexes = n * 3;
		Mem_Free16( verts );
		Mem_Free16( idx );
		if ( s_rtEmissiveDbgVB == 0 || s_rtEmissiveDbgIB == 0 ) {
			if ( s_rtEmissiveDbgVB != 0 ) { r->DestroyBuffer( s_rtEmissiveDbgVB ); }
			if ( s_rtEmissiveDbgIB != 0 ) { r->DestroyBuffer( s_rtEmissiveDbgIB ); }
			s_rtEmissiveDbgVB = s_rtEmissiveDbgIB = 0;	s_rtEmissiveDbgIndexes = 0;
			return false;
		}
	}
	*vb = s_rtEmissiveDbgVB;
	*ib = s_rtEmissiveDbgIB;
	*numIndexes = s_rtEmissiveDbgIndexes;
	return true;
}

// E1d labels: project one label per emissive surface into WINDOW pixels for the ImGui layer (sys_imgui.cpp draws
// them over the finished frame, outlined, while the overlay is on). Anchor = the mean of the surface's triangle
// centroids read from the live table (so a mover's label follows it); culled when behind the camera, off screen,
// back-facing (the first triangle's normal, unless the material is two-sided) or farther than 700 units; nearest 48
// kept; a second label of the same material within 28 px is dropped. Runs once per frame from the overlay pass.
void R_RtEmissiveProjectLabels( const viewDef_t *viewDef ) {
	s_rtEmissiveScreenLabels.SetNum( 0, false );
	if ( viewDef == NULL || s_rtEmissiveBuf == 0 || s_rtEmissiveLabels.Num() == 0 ) {
		return;
	}
	struct cand_t { float x, y, dist; int label; };
	static idList<cand_t> cands;
	cands.SetNum( 0, false );
	const idVec3 eye = viewDef->renderView.vieworg;
	const float *mv = viewDef->worldSpace.modelViewMatrix, *pr = viewDef->projectionMatrix;
	const idRenderWorldLocal *world = viewDef->renderWorld;
	for ( int i = 0; i < s_rtEmissiveLabels.Num(); i++ ) {
		const rtEmissiveLabel_t &l = s_rtEmissiveLabels[i];
		// anchor = the centroid of the triangle nearest the surface's mean (a point ON the surface); facing = the
		// surface's AREA-WEIGHTED normal (a wrapped fixture faces where most of it faces). Dark (switched-off / gone)
		// surfaces carry no label. No occlusion trace: an in-sight emitter is ALWAYS labelled, on top of everything
		// (user, 2026-10-20) - only emitters in areas this view's portal flow never reached are left out.
		idVec3 mean( 0.0f, 0.0f, 0.0f ), nSum( 0.0f, 0.0f, 0.0f );
		bool lit = false, twoSided = false;
		for ( int t = 0; t < l.numTris; t++ ) {
			const rtEmissiveTriGPU_t &e = s_rtEmissiveTris[ l.firstTri + t ];
			const idVec3 p0( e.p0[0], e.p0[1], e.p0[2] ), p1( e.p1[0], e.p1[1], e.p1[2] ), p2( e.p2[0], e.p2[1], e.p2[2] );
			mean += p0 + p1 + p2;
			nSum += ( p2 - p0 ).Cross( p1 - p0 );								// the engine's face normal (R_DeriveFacePlanes: d2 x d1), |n| = 2 x area
			lit = lit || e.flux > 0.0f;
			twoSided = twoSided || ( e.emit & RT_EMIT_FLAG_TWOSIDED ) != 0u;
		}
		if ( !lit ) {
			continue;
		}
		mean *= 1.0f / ( 3.0f * l.numTris );
		float bestD = 1e30f;
		idVec3 c = mean;
		for ( int t = 0; t < l.numTris; t++ ) {
			const rtEmissiveTriGPU_t &e = s_rtEmissiveTris[ l.firstTri + t ];
			const idVec3 tc( ( e.p0[0] + e.p1[0] + e.p2[0] ) * ( 1.0f / 3.0f ), ( e.p0[1] + e.p1[1] + e.p2[1] ) * ( 1.0f / 3.0f ), ( e.p0[2] + e.p1[2] + e.p2[2] ) * ( 1.0f / 3.0f ) );
			const float d = ( tc - mean ).LengthSqr();
			if ( d < bestD ) { bestD = d; c = tc; }
		}
		const idVec3 toEye = eye - c;
		const float dist = toEye.Length();
		if ( dist > 1500.0f ) {
			continue;
		}
		if ( !twoSided && nSum * toEye < 0.0f ) {
			continue;															// facing away: the overlay cannot show it either
		}
		float view[4], clip[4];
		for ( int k = 0; k < 4; k++ ) { view[k] = c[0] * mv[k] + c[1] * mv[k + 4] + c[2] * mv[k + 8] + mv[k + 12]; }
		for ( int k = 0; k < 4; k++ ) { clip[k] = view[0] * pr[k] + view[1] * pr[k + 4] + view[2] * pr[k + 8] + view[3] * pr[k + 12]; }
		if ( clip[3] <= 0.01f ) {
			continue;															// behind the camera
		}
		const float nx = clip[0] / clip[3], ny = clip[1] / clip[3];
		if ( nx < -1.0f || nx > 1.0f || ny < -1.0f || ny > 1.0f ) {
			continue;
		}
		// the emitter's AREA must have been reached by this view's portal flow (portalArea_t::viewCount is stamped
		// before R_AddModelSurfaces, so it is current here in the frontend). The point is nudged off the surface
		// toward the eye; a point the BSP puts in solid (-1) is not culled.
		if ( world != NULL ) {
			idVec3 dir = toEye;
			dir.Normalize();
			const int area = world->PointInArea( c + dir * 2.0f );
			if ( area >= 0 && area < world->NumAreas() && world->portalAreas[area].viewCount != tr.viewCount ) {
				continue;
			}
		}
		cand_t cd;
		cd.x = ( nx * 0.5f + 0.5f ) * 640.0f;									// the 2D text path's VIRTUAL 640x480 space
		cd.y = ( 1.0f - ( ny * 0.5f + 0.5f ) ) * 480.0f;
		cd.dist = dist;
		cd.label = i;
		cands.Append( cd );
	}
	// nearest first (insertion sort; a view has tens of candidates)
	for ( int i = 1; i < cands.Num(); i++ ) {
		cand_t v = cands[i]; int j = i;
		while ( j > 0 && cands[j - 1].dist > v.dist ) { cands[j] = cands[j - 1]; j--; }
		cands[j] = v;
	}
	for ( int i = 0; i < cands.Num() && s_rtEmissiveScreenLabels.Num() < 64; i++ ) {
		const rtEmissiveLabel_t &l = s_rtEmissiveLabels[ cands[i].label ];
		bool dup = false;
		for ( int k = 0; k < i && !dup; k++ ) {
			if ( s_rtEmissiveLabels[ cands[k].label ].mat == l.mat
					&& idMath::Fabs( cands[k].x - cands[i].x ) < 14.0f && idMath::Fabs( cands[k].y - cands[i].y ) < 14.0f ) {
				dup = true;
			}
		}
		if ( dup ) {
			continue;
		}
		rtEmissiveScreenLabel_t &sl = s_rtEmissiveScreenLabels.Alloc();
		sl.x = cands[i].x;
		sl.y = cands[i].y;
		const char *name = l.mat->GetName();
		if ( !idStr::Cmpn( name, "textures/", 9 ) ) { name += 9; }
		else if ( !idStr::Cmpn( name, "models/mapobjects/", 18 ) ) { name += 18; }
		idStr::Copynz( sl.text, name, sizeof( sl.text ) );
	}
}

bool R_RtEmissiveHideEntity( int entityIndex ) {
	return ( r_rtEmissiveHideFlares.GetBool() || r_rtEmissiveLighting.GetBool() ) && s_rtEmissiveBuf != 0
		&& entityIndex >= 0 && entityIndex < s_rtEmissiveFlareHidden.Num() && s_rtEmissiveFlareHidden[entityIndex];
}

bool R_RtEmissiveLabelsWanted( void ) {
	return r_rtEmissiveTableDebug.GetInteger() > 0 && s_rtEmissiveBuf != 0 && s_rtEmissiveLabels.Num() > 0;
}

int R_RtEmissiveGetScreenLabels( const rtEmissiveScreenLabel_t **out ) {
	*out = s_rtEmissiveScreenLabels.Ptr();
	return s_rtEmissiveScreenLabels.Num();
}

// rtEmissiveProbe: what is under the crosshair, and what the emissive table makes of it. r_showSurfaceInfo is dead on
// Vulkan, so this is the way to name a fixture that is not painted: a trace from the last primary view's origin along
// its forward axis (world + static props + skinned models) -> the hit material's stages with their blend modes and
// images, the owning entity's model / state, and R_RtEmissiveStage's verdict with its reason.
static const char *R_RtBlendName( int bits ) {
	const int src = bits & GLS_SRCBLEND_BITS, dst = bits & GLS_DSTBLEND_BITS;
	if ( src == GLS_SRCBLEND_ONE && dst == GLS_DSTBLEND_ONE ) { return "ADD (one, one)"; }
	if ( src == GLS_SRCBLEND_ONE && dst == GLS_DSTBLEND_ZERO ) { return "opaque (one, zero)"; }
	if ( src == GLS_SRCBLEND_SRC_ALPHA && dst == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA ) { return "blend (srcAlpha, 1-srcAlpha)"; }
	if ( src == GLS_SRCBLEND_DST_COLOR && dst == GLS_DSTBLEND_ZERO ) { return "filter (dstColor, zero)"; }
	if ( src == GLS_SRCBLEND_ZERO && dst == GLS_DSTBLEND_SRC_COLOR ) { return "filter (zero, srcColor)"; }
	if ( src == GLS_SRCBLEND_SRC_ALPHA && dst == GLS_DSTBLEND_ONE ) { return "add-alpha (srcAlpha, one)"; }
	return va( "custom (src 0x%x, dst 0x%x)", src, dst );
}

void R_RtEmissiveProbe_f( const idCmdArgs &args ) {
	idRenderWorldLocal *world = tr.primaryWorld;
	if ( world == NULL || world->mapName.Length() == 0 ) {
		common->Printf( "rtEmissiveProbe: no map view\n" );
		return;
	}
	const idVec3 dir   = tr.primaryRenderView.viewaxis[0];
	const idVec3 eye   = tr.primaryRenderView.vieworg;
	// walk THROUGH translucent / deformed hits (a light's flare sprite, glass, a decal) so the fixture behind them is
	// reported too: up to 6 hits along the ray, each one named
	idVec3 start = eye;
	for ( int hit = 0; hit < 6; hit++ ) {
	modelTrace_t mt;
	if ( !world->Trace( mt, start, eye + dir * 4096.0f, 0.0f, false, true ) || mt.material == NULL ) {
		common->Printf( hit == 0 ? "rtEmissiveProbe: nothing hit within 4096 units\n" : "rtEmissiveProbe: nothing further along the ray\n" );
		return;
	}
	const idMaterial *mat = mt.material;
	common->Printf( "---- hit %d ----\n", hit + 1 );
	const float *entityParms = NULL;
	int entityIndex = -1;
	const idRenderEntityLocal *def = NULL;
	if ( mt.entity != NULL ) {
		for ( int i = 0; i < world->entityDefs.Num(); i++ ) {
			if ( world->entityDefs[i] != NULL && &world->entityDefs[i]->parms == mt.entity ) { entityIndex = i; def = world->entityDefs[i]; break; }
		}
	}
	common->Printf( "rtEmissiveProbe: hit at %.0f %.0f %.0f (%.0f units)\n", mt.point.x, mt.point.y, mt.point.z, ( mt.point - start ).Length() );
	if ( def != NULL ) {
		const idRenderModel *model = def->parms.hModel;
		entityParms = def->parms.shaderParms;
		const char *dm = ( model == NULL ) ? "none" : ( model->IsDynamicModel() == DM_STATIC ? "static" : model->IsDynamicModel() == DM_CACHED ? "cached (skinned md5)" : "continuous (particle / beam / sprite)" );
		common->Printf( "  entity %d: model '%s' (%s%s%s), skin '%s', customShader '%s', parms %.2f %.2f %.2f %.2f | parm7 %.1f\n",
			entityIndex, model ? model->Name() : "-", dm, def->parms.callback ? ", CALLBACK-built" : "", model && model->IsStaticWorldModel() ? ", world" : "",
			def->parms.customSkin ? def->parms.customSkin->GetName() : "-", def->parms.customShader ? def->parms.customShader->GetName() : "-",
			def->parms.shaderParms[0], def->parms.shaderParms[1], def->parms.shaderParms[2], def->parms.shaderParms[3], def->parms.shaderParms[7] );
		mat = R_RemapShaderBySkin( mat, def->parms.customSkin, def->parms.customShader );
		common->Printf( "  gatherable as a static entity: %s%s\n", R_RtEmissiveEntityGatherable( def ) ? "yes" : "NO",
			( entityIndex >= 0 && entityIndex < s_rtEmissiveFlareHidden.Num() && s_rtEmissiveFlareHidden[entityIndex] ) ? " | a FLARE sprite on an emitter: hidden while r_rtEmissiveHideFlares is on" : "" );
	} else {
		common->Printf( "  worldspawn surface\n" );
	}
	common->Printf( "  material '%s': coverage %s, sort %.0f%s%s%s, %d stages\n", mat->GetName(),
		mat->Coverage() == MC_OPAQUE ? "opaque" : mat->Coverage() == MC_PERFORATED ? "perforated" : "translucent", mat->GetSort(),
		mat->HasGui() ? ", GUI" : "", mat->Deform() != DFRM_NONE ? ", deformed" : "", mat->ShouldCreateBackSides() ? ", two-sided" : "", mat->GetNumStages() );
	for ( int i = 0; i < mat->GetNumStages(); i++ ) {
		const shaderStage_t *st = mat->GetStage( i );
		const idImage *img = st->texture.image;
		const char *kind = st->lighting == SL_BUMP ? "bump" : st->lighting == SL_DIFFUSE ? "diffuse" : st->lighting == SL_SPECULAR ? "specular" : "ambient";
		common->Printf( "    stage %d: %-8s %-28s image '%s'%s%s%s%s%s\n", i, kind, st->lighting == SL_AMBIENT ? R_RtBlendName( st->drawStateBits ) : "(interaction)",
			img ? img->imgName.c_str() : "-",
			img && img->rhiHandle == 0 ? " [NOT RESIDENT]" : "", img && img->type == TT_CUBIC ? " [cube]" : "",
			st->texture.texgen != TG_EXPLICIT ? " [texgen]" : "", st->texture.cinematic ? " [cinematic]" : "", st->texture.dynamic != DI_STATIC ? " [render-to-texture]" : "" );
	}
	const char *why = "";
	const shaderStage_t *stage = R_RtEmissiveStage( mat, &why, entityParms );
	if ( stage != NULL ) {
		int inTable = 0;
		for ( int i = 0; i < s_rtEmissiveLabels.Num(); i++ ) { if ( s_rtEmissiveLabels[i].mat == mat ) { inTable += s_rtEmissiveLabels[i].numTris; } }
		common->Printf( "  verdict: EMITTER (glow image '%s'); %d triangles of this material are in the table%s\n", stage->texture.image->imgName.c_str(), inTable,
			inTable == 0 ? " - NONE: the surface's entity was not gathered (see 'gatherable' above) or the table predates it" : "" );
		// E2 v3: the ROW the nearest table triangle of this material belongs to - what the gather turns into a light
		{
			int bestT = -1; float bestD = 1e30f;
			for ( int li = 0; li < s_rtEmissiveLabels.Num(); li++ ) {
				const rtEmissiveLabel_t &l = s_rtEmissiveLabels[li];
				if ( l.mat != mat ) { continue; }
				for ( int t = l.firstTri; t < l.firstTri + l.numTris && t < s_rtEmissiveTris.Num(); t++ ) {
					const rtEmissiveTriGPU_t &e = s_rtEmissiveTris[t];
					const idVec3 a( e.p0[0], e.p0[1], e.p0[2] ), b( e.p1[0], e.p1[1], e.p1[2] ), c( e.p2[0], e.p2[1], e.p2[2] );
					const float d = R_RtPointTriangleDistance( mt.point, a, b, c );
					if ( d < bestD ) { bestD = d; bestT = t; }
				}
			}
			if ( bestT >= 0 ) {
				const rtEmissiveTriGPU_t &e = s_rtEmissiveTris[bestT];
				const idVec3 a( e.p0[0], e.p0[1], e.p0[2] ), b( e.p1[0], e.p1[1], e.p1[2] ), c( e.p2[0], e.p2[1], e.p2[2] );
				// the EMITTING side (rt_emissive.glsl: a receiver P is in front when dot( cross( p1 - p0, p2 - p0 ), p0 - P ) > 0)
				const idVec3 g = ( b - a ).Cross( c - a );
				const bool eyeFront = g * ( a - eye ) > 0.0f;
				int rowIdx = -1;
				for ( int ri = 0; ri < s_rtEmissiveRows.Num(); ri++ ) { if ( bestT >= s_rtEmissiveRows[ri].firstTri && bestT < s_rtEmissiveRows[ri].firstTri + s_rtEmissiveRows[ri].numTris ) { rowIdx = ri; break; } }
				common->Printf( "  nearest table triangle %d (%.1f units from the hit): flux %.3f, emits TOWARD the eye: %s%s\n", bestT, bestD, e.flux,
					eyeFront ? "yes" : "NO (its back faces you - a one-sided emitter lights nothing on this side)", ( e.emit & RT_EMIT_FLAG_TWOSIDED ) ? " [two-sided]" : "" );
				if ( rowIdx >= 0 ) {
					const rtEmissiveRow_t &row = s_rtEmissiveRows[rowIdx];
					common->Printf( "  power match at the default reach: radiance x%.2f on the glow mean (the map's authored radiance / reach replace it)\n", R_RtEmissiveMatchedScale( row, 0.0f, 0.0f ) );
					const float sc = Max( R_RtEmissiveScale(), 1e-3f );
					const float meanLum = row.meanRadiance.x * 0.2126f + row.meanRadiance.y * 0.7152f + row.meanRadiance.z * 0.0722f;
					common->Printf( "  ROW %d: fixture %016llx, %d triangles, area %.0f units^2, effective %.0f, mean lum %.2f, bound %.0f, centroid %.0f %.0f %.0f%s\n",
						rowIdx, row.fixtureId, row.numTris, row.area, row.flux / sc, meanLum, row.boundRadius,
						row.centroid.x, row.centroid.y, row.centroid.z, row.dark ? " DARK" : "" );
				} else {
					common->Printf( "  (no row holds this triangle - the rows predate the table or the surface is dark)\n" );
				}
			}
		}
	} else if ( why[0] != '\0' ) {
		common->Printf( "  verdict: NOT an emitter - %s\n", why );
	} else {
		common->Printf( "  verdict: NOT an emitter - the material has no additive (blend add) stage: what glows is lit diffuse, not self-illumination\n" );
	}
	if ( mt.material->Coverage() == MC_OPAQUE || mt.material->Coverage() == MC_PERFORATED ) {
		return;																// solid: the ray ends here
	}
	start = mt.point + dir * 0.5f;											// see-through (flare, glass, decal): carry on behind it
	}
}
