/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

#ifndef __AOBAKE_H__
#define __AOBAKE_H__

// DUDE offline ambient-occlusion baker (docs/occlusion-maps.md).

class idRenderModel;
class idStr;
class idDeclSkin;

// generated/aomaps/<model sans ext>_s<surfaceIndex>.tga -- the convention the runtime
// auto-load resolves against (RhiWorld.cpp). Keyed by model + surface index so it is
// skin- and material-independent. Shared so the baker and the renderer agree on the path.
void AO_GeneratedPathForSurface( const char *modelName, int surfaceIndex, idStr &out );

// Bake one render model's self-occlusion into generated/aomaps/, one texture per surface
// keyed by model + surface index. Static models (LWO/ASE) bake their resident geometry; MD5
// characters are instantiated in their reference/bind pose first. Uses the r_occlusionMapBake*
// cvars for parameters. Returns true if it wrote at least one map. Called by the bakeAO*
// commands and, under r_occlusionMapsAutoBake, lazily by the renderer. Pure CPU + filesystem.
bool AO_BakeModelToCache( const idRenderModel *model, const idDeclSkin *skin = NULL );

#endif /* !__AOBAKE_H__ */
