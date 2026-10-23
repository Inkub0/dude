/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: game title profile - one executable for every supported idTech4 title.
//
// DUDE runs more than one game from the same binary (docs/quake4-port.md, decision 1).
// The title is chosen once at startup, before the filesystem comes up, from
// +set com_title <name> or by looking at what fs_basepath contains. Everything
// in the engine that used to assume Doom 3 - the base game folder, the base game
// library, the console charset, GUI cursor art, the default GUI font, the loading
// screen - asks the active profile instead. The Doom 3 profile reproduces the
// historical constants exactly, so a Doom 3 run is unchanged.
//
// Game code never sees this: a game library is selected BY the profile (its stem
// is the profile's base game folder, e.g. base.so / q4base.so), so nothing here
// crosses the GetGameAPI boundary.

#ifndef __GAME_TITLE_H__
#define __GAME_TITLE_H__

typedef enum {
	GAME_TITLE_DOOM3 = 0,
	GAME_TITLE_QUAKE4,
	GAME_TITLE_COUNT
} gameTitle_t;

typedef struct gameTitleProfile_s {
	gameTitle_t		title;
	const char *	name;				// com_title value: "doom3", "quake4"
	const char *	displayName;		// "DOOM 3", "Quake 4"
	const char *	baseGameDir;		// base game folder under fs_basepath; also the base game library stem
	const char *	probeFile;			// a file only this title ships, relative to baseGameDir (autodetection)
	bool			doom3Layout;		// base/ + d3xp/ + demo/ conventions: RoE detection, demo fallback, mod list
	bool			nullGameFallback;	// run the null game when the title's own game library is not built yet
	bool			recursiveDeclFolders;	// decl files also live in subfolders (Quake 4: materials/effects/...)
	bool			entityFilters;		// "map <map> <filter>" picks one of the levels a .map holds (Quake 4: si_entityFilter, mapDef <map>_<filter>)

	// sound
	float			soundUnitsToMeters;	// sound shader min/maxDistance units: Doom 3 authors metres, Quake 4 map units
	bool			bareSampleNames;	// sound shaders name samples by path without .wav/.ogg (Quake 4: 4805 of 6952)
	const char *	voLanguagePrefix;	// "sound/vo/x" plays <prefix><sys_lang>/x (Quake 4: sound/vo_english/); NULL = Doom 3's sound/vo/<lang>/ for non-English only
	const char *	reverbPresets;		// EFX preset library picked per area by maps/<map>.reverb; NULL = Doom 3's efxs/<map>.efx keyed by area

	// engine-owned assets whose names differ per title
	const char *	charsetMaterial;	// console / small-string bitmap font
	const char *	splashMaterial;		// startup splash screen
	const char *	guiWhiteMaterial;	// GUI solid fills
	const char *	guiArtPrefix;		// GUI cursor + scrollbar art: <prefix>guicursor_arrow.tga, <prefix>scrollbarh.tga, ...
	const char *	guiListArtPrefix;	// list/edit window scrollbar art (Doom 3 keeps a second copy under guis/assets/)
	const char *	defaultFont;		// first font every GUI device context registers
	const char *	overlayFont;		// FPS / stats overlay font
	const char *	loadingGui;			// map loading screen
} gameTitleProfile_t;

// Stem of the null game library (nullgame.so / nullgame.dll): a spectator-only idGame
// that loads and renders any title's maps without gameplay. Used as the fallback for a
// title whose own game library is not built yet, and on request with +set fs_game nullgame.
#define GAME_NULL_LIBRARY		"nullgame"

namespace GameTitle {

// Resolve the active title. Called once from idFileSystemLocal::Init, after the
// fs_* startup variables and com_title have been read and before fs_basepath is
// auto-located, so basePath may be empty. Order: explicit com_title, then the
// first profile whose probe file exists under basePath, then Doom 3.
void						Select( const char *basePath );

// The active profile. Doom 3 until Select() has run.
const gameTitleProfile_t &	Get( void );

// Profile by com_title name, or NULL.
const gameTitleProfile_t *	Find( const char *name );

}

#endif /* !__GAME_TITLE_H__ */
