/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: null game map dressing - what a map places that can be drawn without a game.
//
// The null game spawns no entities, so on its own it shows only the world geometry the
// .proc carries. Dressing adds, render-only, what the map's entities would show at the
// moment they spawn: lights, static and brush models, MD5 models frozen on frame 0 of their
// start animation (the entity's "anim", else "idle", else "af_pose", else the bind pose),
// actors' heads, and entity GUIs. Nothing moves, thinks or collides.
//
// It reads spawn arguments the way Doom 3's game code does (idGameEdit::
// ParseSpawnArgsToRenderLight / ParseSpawnArgsToRenderEntity, idLight::Spawn,
// idActor::SetupHead, idGameEdit::ANIM_CreateAnimFrame), merged with the entity's
// entityDef, and parses model defs and frame 0 of .md5anim files itself: the animation
// system lives in a game library. Only engine interfaces are used - a game library
// cannot call engine free functions.

#ifndef __NULLGAMEDRESSING_H__
#define __NULLGAMEDRESSING_H__

class idDict;
class idMapFile;
class idRenderWorld;

namespace NullGameDressing {

// Adds the map's entities to the render world. Call once the session has loaded the
// map's render world (idGame::InitFromNewMap). entityFilter: the serverinfo si_entityFilter.
void		Spawn( const idMapFile &mapFile, idRenderWorld *renderWorld, int gameTime, const char *entityFilter );

// Quake 4's entity filter (idGameLocal::InhibitEntitySpawn): with a filter chosen ("map
// game/process1 first" sets si_entityFilter), an entity that names any filter_* key spawns only
// if it names filter_<filter>. Entities without such keys, and all of them when no filter is
// chosen (every Doom 3 map), spawn.
bool		EntityFilteredOut( const idDict &spawnArgs, const char *entityFilter );

// Removes what Spawn added. With renderWorld NULL the render world has already been
// reset (a new map was loaded into it), so only the bookkeeping is dropped.
void		Clear( idRenderWorld *renderWorld );

// listDressing: the MD5 entities placed, their animation frame and heads.
void		List( void );

}

#endif /* !__NULLGAMEDRESSING_H__ */
