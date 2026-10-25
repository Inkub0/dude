/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: the names the Quake 4 game code uses for its engine interface, mapped onto this
// interface's own (docs/quake4-port.md 5.6). Include it ONLY from the Quake 4 game library, after
// every engine header and before any game code: Doom 3's types that Quake 4 reshaped keep their
// names in DUDE's headers, so from here on the game's renderEntity_t is renderEntityQ4_t and so
// on - and anything that would hand a Doom 3 struct where a Quake 4 one is expected (or the
// reverse) fails to compile instead of corrupting memory.

#ifndef __GAMENAMESQ4_H__
#define __GAMENAMESQ4_H__

#include "quake4/GameQ4.h"

// user commands
#define usercmd_t						usercmdQ4_t

// render world
#define renderEntity_t					renderEntityQ4_t
#define renderEntity_s					renderEntityQ4_s
#define renderLight_t					renderLightQ4_t
#define renderLight_s					renderLightQ4_s
#define modelTrace_t					modelTraceQ4_t
#define modelTrace_s					modelTraceQ4_s
#define deferredEntityCallback_t		deferredEntityCallbackQ4_t
#define idRenderWorld					idRenderWorldQ4
#define rvRenderEffectLocal				idRenderEffectQ4

// sound
#define soundShaderParms_t				soundShaderParmsQ4_t
#define refSound_t						refSoundQ4_t
#define idSoundEmitter					idSoundEmitterQ4
#define idSoundSystem					idSoundSystemQ4
#define soundSystem						soundSystemQ4

// collision
#define contactInfo_t					contactInfoQ4_t
#define trace_t							traceQ4_t
#define idCollisionModelManager			idCollisionModelManagerQ4
#define collisionModelManager			collisionModelManagerQ4

// effects
#define rvBSEManager					idEffectManagerQ4

// the game interface
#define idGame							idGameQ4
#define idGameEdit						idGameEditQ4
#define gameImport_t					gameImportQ4_t
#define gameExport_t					gameExportQ4_t
#define GetGameAPI_t					GetGameAPIQ4_t
#define GAME_API_VERSION				GAME_API_VERSION_Q4

// Raven's decl classes are DUDE's Quake 4 decls (framework/DeclQuake4.h)
#define rvDeclMatType					idDeclMaterialType
#define rvDeclLipSync					idDeclLipSync
#define rvDeclPlayback					idDeclPlayback
#define rvDeclEffect					idDeclEffect
#define rvDeclPlayerModel				idDeclPlayerModel
#define rvDeclPlaybackData				idPlaybackData

#endif /* !__GAMENAMESQ4_H__ */
