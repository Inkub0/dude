/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: game title profile - see GameTitle.h.

#include "sys/platform.h"
#include "idlib/Str.h"
#include "framework/Common.h"
#include "framework/CVarSystem.h"
#include "framework/GameTitle.h"

#include <stdio.h>

idCVar com_title( "com_title", "", CVAR_SYSTEM | CVAR_INIT,
	"game title to run, chosen at startup: doom3, quake4 (empty = detect from fs_basepath)" );

static const gameTitleProfile_t titleProfiles[GAME_TITLE_COUNT] = {
	{
		GAME_TITLE_DOOM3, "doom3", "DOOM 3",
		"base", "pak000.pk4",
		true,							// doom3Layout
		false,							// nullGameFallback: a missing base.so stays a fatal error
		false,							// recursiveDeclFolders: Doom 3 keeps its flat scan
		"textures/bigchars",
		"splashScreen",
		"guis/assets/white.tga",
		"ui/assets/",
		"guis/assets/",
		"fonts",						// resolves to fonts/<lang>/fontImage_<pt>.dat
		"fonts/bank",
		"guis/map/loading.gui",
	},
	{
		GAME_TITLE_QUAKE4, "quake4", "Quake 4",
		"q4base", "pak001.pk4",
		false,							// doom3Layout
		true,							// nullGameFallback: q4base.so comes from the separate game repository (Q5)
		true,							// recursiveDeclFolders: 34 of 96 .mtr files live in subfolders
		"gfx/2d/bigchars",
		"gfx/splashScreen",
		"_white",						// Quake 4 ships no guis/assets/white.tga
		"gfx/guis/",
		"gfx/guis/",
		"fonts/chain",					// resolves to fonts/<lang>/chain_<pt>.fontdat
		"fonts/profont",				// Quake 4's BankGothic-style face, the counterpart of fonts/bank
		"guis/loading/generic.gui",
	},
};

static const gameTitleProfile_t *activeTitle = &titleProfiles[GAME_TITLE_DOOM3];

/*
================
GameTitle_FileExists

Raw OS probe: runs before the filesystem is up, so no VFS. fopen accepts forward
slashes on every target, including Windows.
================
*/
static bool GameTitle_FileExists( const char *path ) {
	FILE *f = fopen( path, "rb" );
	if ( f == NULL ) {
		return false;
	}
	fclose( f );
	return true;
}

/*
================
GameTitle::Find
================
*/
const gameTitleProfile_t *GameTitle::Find( const char *name ) {
	if ( name == NULL || name[0] == '\0' ) {
		return NULL;
	}
	for ( int i = 0; i < GAME_TITLE_COUNT; i++ ) {
		if ( idStr::Icmp( name, titleProfiles[i].name ) == 0 ) {
			return &titleProfiles[i];
		}
	}
	return NULL;
}

/*
================
GameTitle::Get
================
*/
const gameTitleProfile_t &GameTitle::Get( void ) {
	return *activeTitle;
}

/*
================
GameTitle::Select
================
*/
void GameTitle::Select( const char *basePath ) {
	const gameTitleProfile_t *chosen = NULL;
	idStr why;

	const char *requested = com_title.GetString();
	if ( requested[0] != '\0' ) {
		chosen = Find( requested );
		if ( chosen != NULL ) {
			why = "com_title";
		} else {
			idStr valid;
			for ( int i = 0; i < GAME_TITLE_COUNT; i++ ) {
				valid += ( i ? ", " : "" );
				valid += titleProfiles[i].name;
			}
			common->Warning( "com_title '%s' is not a known game title (valid: %s); detecting from fs_basepath",
				requested, valid.c_str() );
		}
	}

	// Detection walks the profiles in order, Doom 3 first, so an install folder that
	// holds both games keeps today's behaviour.
	if ( chosen == NULL && basePath != NULL && basePath[0] != '\0' ) {
		for ( int i = 0; i < GAME_TITLE_COUNT && chosen == NULL; i++ ) {
			idStr probe = basePath;
			probe.AppendPath( titleProfiles[i].baseGameDir );
			probe.AppendPath( titleProfiles[i].probeFile );
			if ( GameTitle_FileExists( probe.c_str() ) ) {
				chosen = &titleProfiles[i];
				why = va( "found %s/%s", titleProfiles[i].baseGameDir, titleProfiles[i].probeFile );
			}
		}
	}

	if ( chosen == NULL ) {
		chosen = &titleProfiles[GAME_TITLE_DOOM3];
		why = "default";
	}

	activeTitle = chosen;
	com_title.SetString( chosen->name );	// show the resolved title in the console
	common->Printf( "game title: %s (com_title %s) [%s]\n", chosen->displayName, chosen->name, why.c_str() );
}
