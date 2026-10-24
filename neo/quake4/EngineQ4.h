/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: the engine's side of the Quake 4 game interface (docs/quake4-port.md 5.6, Q5.4). The
// Quake 4 game library is loaded with gameImportQ4_t; what it asks of the engine differently from
// Doom 3 goes through the facades declared here, each a thin translation onto the engine's own
// system (sound worlds, the collision model manager, the render world), and the game itself
// reaches the session through an adapter that presents idGameQ4 as an idGame.
//
// Engine-only: the game library never includes this.

#ifndef __ENGINEQ4_H__
#define __ENGINEQ4_H__

#include "quake4/GameQ4.h"

class idSoundWorld;
class idGame;
class idGameEdit;

// the facades, created on first use and kept for the run
idSoundSystemQ4 *			SoundSystemQ4( void );
idCollisionModelManagerQ4 *	CollisionModelManagerQ4( void );
idEffectManagerQ4 *			EffectManagerQ4( void );

// the Quake 4 face of a render world: one per engine render world, made on first use; all of
// them go with FreeRenderWorldsQ4 (the game library's unload)
idRenderWorldQ4 *			RenderWorldQ4( idRenderWorld *renderWorld );
void						FreeRenderWorldsQ4( void );

// the engine's emitter behind a SOUNDWORLD_GAME handle (render entities' and lights'
// referenceSoundHandle), NULL for none
idSoundEmitter *			SoundEmitterForHandleQ4( int handle );

// the game's descriptions as the engine's (render world and sound facades)
void						R_RenderLightQ4ToEngine( const renderLightQ4_t &src, renderLight_t &dst );
void						S_SoundParmsQ4ToEngine( const soundShaderParmsQ4_t &src, soundShaderParms_t &dst );

#endif /* !__ENGINEQ4_H__ */
