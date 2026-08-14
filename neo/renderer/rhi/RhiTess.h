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

struct drawSurf_s;
class idImage;
namespace rhi { struct RenderParams; }

// Should this surface route through the PN-triangle tessellation pipeline?
// forBlendPass relaxes the opaque-only guards (translucent + pure-emissive), which
// otherwise exclude the blended decals that the blend pass itself draws.
bool RB_RHI_TessellateSurf( const drawSurf_s *surf, bool forBlendPass );

// Fill u_tessParms (level / LOD distance / displacement / min edge) from the cvars;
// identical in every pass so the tessellated surfaces line up.
void RB_RHI_SetTessParms( rhi::RenderParams &parms );

// Copy the surface's bump-stage texture matrix into parms (so the pass builds the SAME
// bump texcoord the lit passes displace with) and return the bump image to bind on the
// displacement sampler unit. A surface with no bump stage returns flatNormalMap, which
// dudeTessDisplace reads as relief 0 (no push). Shared by the zfill prepass, the fog
// interaction pass and the blended material pass (so on-body ember/emissive stages
// displace bit-identically and hold depth-EQUAL).
idImage *RB_RHI_TessBumpForZfill( const drawSurf_s *surf, rhi::RenderParams &parms );

#endif /* !__RHITESS_H__ */
