/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: what the engine's game library loader needs to load the Quake 4 game library
// (docs/quake4-port.md 5.6, Q5.4), without the Quake 4 interface types themselves.

#ifndef __GAMELOADERQ4_H__
#define __GAMELOADERQ4_H__

class idGame;
class idGameEdit;

// Hands the library's GetGameAPI the Quake 4 import block and attaches its exports as the
// engine's game and gameEdit. False when the library is not a Quake 4 game library of this
// interface version.
bool	GameQ4_Load( void *getGameAPI, idGame **game, idGameEdit **gameEdit );

// After the game's Shutdown, before the library is unloaded.
void	GameQ4_Unload( void );

#endif /* !__GAMELOADERQ4_H__ */
