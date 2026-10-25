/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: the engine settings Quake 4's own menus expect (docs/quake4-port.md). Quake 4's engine
// had settings Doom 3's never had - a linear master volume, music and ambient volumes, the sound
// device by name, the detailed sky, a resolution list per aspect ratio - and its main menu binds
// to them. Registered only when the title is Quake 4, so Doom 3's cvars and config are unchanged.
// Engine-only: the game library never includes this.

#ifndef __SETTINGSQ4_H__
#define __SETTINGSQ4_H__

class idUserInterface;
class idCmdArgs;

// after the file system is up (the title is known), before the config files run
void	Q4Settings_Init( void );
// once a frame: the settings that mirror one of DUDE's own (the master volume, the device)
void	Q4Settings_Frame( void );

// the main menu's lists and states (resolutions per aspect, aspect choices, sound devices)
void	Q4Settings_SetMenuGuiVars( idUserInterface *gui );
// a main menu command only Quake 4's menu sends (fixup_mode); false when it is not one
bool	Q4Settings_HandleMenuCommand( const char *cmd, const idCmdArgs &args, int &icmd );
// a choice window changed (or re-read) its cvar: Quake 4's menu scripts read the value back as
// gui::<cvar>
void	Q4Settings_MirrorChoice( idUserInterface *gui, const char *cvarName, const char *value );

// the mix Quake 4 plays (1 when the title is not Quake 4): ambient - every positional sound but
// voice-over and music - and music
float	S_Q4SpeakerFraction( void );
float	S_Q4MusicVolume( void );
// Quake 4's "Enable detailed sky: No"
bool	R_Q4SkipSky( void );
// Quake 4's "Force ambient light" level, 0..1 (0: off, and always for Doom 3)
float	R_Q4ForceAmbient( void );

#endif /* !__SETTINGSQ4_H__ */
