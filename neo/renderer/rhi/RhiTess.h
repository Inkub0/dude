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
namespace rhi { struct RenderParams; }

// Should this surface route through the PN-triangle tessellation pipeline?
// forBlendPass relaxes the opaque-only guards (translucent + pure-emissive), which
// otherwise exclude the blended decals that the blend pass itself draws.
bool RB_RHI_TessellateSurf( const drawSurf_s *surf, bool forBlendPass );

// Fill u_tessParms (level / LOD distance / displacement / min edge) from the cvars;
// identical in every pass so the tessellated surfaces line up.
void RB_RHI_SetTessParms( rhi::RenderParams &parms );

#endif /* !__RHITESS_H__ */
