/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

#ifndef __RHITESS_H__
#define __RHITESS_H__

// DUDE tessellation (docs/tessellation.md) classifier + params, shared between
// RhiWorld.cpp (the opaque depth prepass + interaction/ambient passes) and
// RhiBackend.cpp (the blended material/decal pass, so blood-overlay decals can
// follow the tessellated base).

#include "renderer/Material.h"		// shaderStage_t (anonymous typedef -- not forward-declarable)

struct drawSurf_s;
class idImage;
namespace rhi { struct RenderParams; class RHI; }

// Should this surface route through the PN-triangle tessellation pipeline?
// forBlendPass relaxes the opaque-only guards (translucent + pure-emissive), which
// otherwise exclude the blended decals that the blend pass itself draws.
bool RB_RHI_TessellateSurf( const drawSurf_s *surf, bool forBlendPass );

// Fill u_tessParms (level / LOD distance / displacement / min edge) from the cvars;
// identical in every pass so the tessellated surfaces line up.
void RB_RHI_SetTessParms( rhi::RenderParams &parms );

// Copy the surface's bump-stage texture matrix into parms (so the pass builds the SAME
// bump texcoord the lit passes displace with) and return the bump image to bind on the
// bump sampler unit. A surface with no bump stage returns flatNormalMap. (The DISPLACEMENT
// itself no longer comes from here -- see RB_RHI_TessHeightImage.) Shared by the zfill prepass, the fog
// interaction pass and the blended material pass (so on-body ember/emissive stages
// displace bit-identically and hold depth-EQUAL).
idImage *RB_RHI_TessBumpForZfill( const drawSurf_s *surf, rhi::RenderParams &parms );

// DUDE: the SIGNED height image the tess-eval stage displaces with (docs/tessellation.md).
// 0.5 = flat; >0.5 raised, <0.5 recessed. Returns the material's baked height when it has
// one, otherwise the mid-grey flat image -- NEVER NULL and never a normal map, because the
// shader computes (h - 0.5) and anything brighter than mid-grey pushes the surface outward.
// Every pass that tessellates a surface must bind THIS image for it, or that pass displaces
// differently from the depth prepass and the lit draw fails depth-EQUAL.
idImage *RB_RHI_TessHeightImage( const drawSurf_s *surf, bool tess );

// Not tessellation, but the same two-TU sharing: the PER-STAGE polygon offset bracket
// (Material privatePolygonOffset), defined in RhiBackend.cpp. Every pass that draws a
// stage's alpha-tested coverage into scene depth (RB_RHI_FillDepthBuffer, the merged
// gbuffer prepass) and the blended stage pass that later draws that stage at
// DEPTHFUNC_EQUAL must wrap the draw in Begin/End so the stage sits at ONE biased depth
// in every pass. This is what confines the burning-corpse ember to the receding edge
// (docs/burn-away-ember.md). End restores the material-level MF_POLYGONOFFSET bias, or
// clears the bias, so nothing latches into the next draw on VK's persistent dynamic bias.
bool RB_RHI_StageWantsPolyOffset( const shaderStage_t *pStage );
void RB_RHI_StagePolyOffsetBegin( rhi::RHI *r, const shaderStage_t *pStage );
void RB_RHI_StagePolyOffsetEnd( rhi::RHI *r, const shaderStage_t *pStage, const idMaterial *shader );

#endif /* !__RHITESS_H__ */
