/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// What a game library's frame returns and how it answers connects and ESC - the same for every
// title's game interface (framework/Game.h, quake4/GameQ4.h), so both include them from here.

#ifndef __GAMERETURN_H__
#define __GAMERETURN_H__

#include "idlib/Lib.h"

typedef struct {
	char		sessionCommand[MAX_STRING_CHARS];	// "map", "disconnect", "victory", etc
	int			consistencyHash;					// used to check for network game divergence
	int			health;
	int			heartRate;
	int			stamina;
	int			combat;
	bool		syncNextGameFrame;					// used when cinematics are skipped to prevent session from simulating several game frames to
													// keep the game time in sync with real time
} gameReturn_t;

typedef enum {
	ALLOW_YES = 0,
	ALLOW_BADPASS,	// core will prompt for password and connect again
	ALLOW_NOTYET,	// core will wait with transmitted message
	ALLOW_NO		// core will abort with transmitted message
} allowReply_t;

typedef enum {
	ESC_IGNORE = 0,	// do nothing
	ESC_MAIN,		// start main menu GUI
	ESC_GUI			// set an explicit GUI
} escReply_t;

#endif /* !__GAMERETURN_H__ */
