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
#include "framework/FileSystem.h"
#include "sys/sys_public.h"

#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

idCVar com_title( "com_title", "", CVAR_SYSTEM | CVAR_INIT,
	"game title to run, chosen at startup: doom3, quake4 (empty = detect from fs_basepath)" );

static const gameTitleProfile_t titleProfiles[GAME_TITLE_COUNT] = {
	{
		GAME_TITLE_DOOM3, "doom3", "DOOM 3",
		"base", "pak000.pk4",
		true,							// doom3Layout
		false,							// nullGameFallback: a missing base.so stays a fatal error
		false,							// recursiveDeclFolders: Doom 3 keeps its flat scan
		false,							// entityFilters
		1.0f,							// soundUnitsToMeters
		false,							// bareSampleNames: a Doom 3 path without .wav/.ogg stays an error (as shipped)
		NULL,							// voLanguagePrefix
		NULL,							// reverbPresets
		"textures/bigchars",
		"splashScreen",
		"guis/assets/white.tga",
		"ui/assets/",
		"guis/assets/",
		"fonts",						// resolves to fonts/<lang>/fontImage_<pt>.dat
		"fonts/bank",
		"guis/map/loading.gui",
		"guis/assets/blankLevelShot",
		"guis/assets/splash/pdtempa",
		NULL,							// fullGameFiles: no check (the GPL game code has no such terms)
	},
	{
		GAME_TITLE_QUAKE4, "quake4", "Quake 4",
		"q4base", "pak001.pk4",
		false,							// doom3Layout
		true,							// nullGameFallback: q4base.so comes from the separate game repository (Q5)
		true,							// recursiveDeclFolders: 34 of 96 .mtr files live in subfolders
		true,							// entityFilters: process1 and storage1 each hold a "first" and a "second" level
		0.0254f,						// soundUnitsToMeters: inches (DOOM_TO_METERS, SOUND_Q4_UNITS_TO_METERS), "maxDistance 1181" = 30 m
		true,							// bareSampleNames: sound/null plays sound/null.ogg
		"sound/vo_",					// voLanguagePrefix: sound/vo_english/...
		"efxs/default.efx",				// reverbPresets: 23 EAX presets, maps/<map>.reverb picks one per area
		"gfx/2d/bigchars",
		"gfx/splashScreen",
		"_white",						// Quake 4 ships no guis/assets/white.tga
		"gfx/guis/",
		"gfx/guis/",
		"fonts/chain",					// resolves to fonts/<lang>/chain_<pt>.fontdat
		"fonts/profont",				// Quake 4's BankGothic-style face, the counterpart of fonts/bank
		"guis/loading/generic.gui",
		"gfx/guis/loadscreens/generic",	// Quake 4 ships neither of Doom 3's placeholders
		"gfx/guis/loadscreens/generic",
		// fullGameFiles: the last two levels, from the two map paks (pak001, pak002) - no demo has them
		"maps/game/core2.proc maps/game/process2.proc",
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
GameTitle_TitlesIn

The titles whose probe file (a retail pak of theirs) is in folder: a bit per title.
================
*/
static int GameTitle_TitlesIn( const char *folder ) {
	int found = 0;
	for ( int i = 0; i < GAME_TITLE_COUNT; i++ ) {
		idStr probe = folder;
		probe.AppendPath( titleProfiles[i].baseGameDir );
		probe.AppendPath( titleProfiles[i].probeFile );
		if ( GameTitle_FileExists( probe.c_str() ) ) {
			found |= 1 << i;
		}
	}
	return found;
}

/*
================
GameTitle_InstallFolders

Where a game install is looked for when fs_basepath is not given, in the order the
platform code then looks for the base folder: next to the executable, its parent (the
repository layout keeps the binary in build/), and on Unix the working directory.
================
*/
static void GameTitle_InstallFolders( idStrList &folders ) {
	idStr exe;
	if ( Sys_GetPath( PATH_EXE, exe ) ) {
		exe.StripFilename();
		folders.Append( exe );
		idStr parent = exe;
		parent.StripFilename();
		if ( parent.Length() > 0 && parent != exe ) {
			folders.Append( parent );
		}
	}
#ifndef _WIN32
	// the Unix base-path search also tries the working directory (sys/linux/main.cpp)
	char cwd[ MAX_OSPATH ];
	if ( getcwd( cwd, sizeof( cwd ) ) != NULL ) {
		folders.AddUnique( idStr( cwd ) );
	}
#endif
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

	// Detection: which titles' retail paks are there - in fs_basepath when it is given, else
	// in the first install folder that holds any. Both at once is not guessed: the player
	// names the one to run.
	if ( chosen == NULL ) {
		idStrList folders;
		if ( basePath != NULL && basePath[0] != '\0' ) {
			folders.Append( basePath );
		} else {
			GameTitle_InstallFolders( folders );
		}
		for ( int f = 0; f < folders.Num() && chosen == NULL; f++ ) {
			const int found = GameTitle_TitlesIn( folders[f] );
			if ( found == 0 ) {
				continue;
			}
			if ( ( found & ( found - 1 ) ) != 0 ) {
				idStr list, choices;
				for ( int i = 0; i < GAME_TITLE_COUNT; i++ ) {
					if ( found & ( 1 << i ) ) {
						list += va( "%s%s (%s/%s)", list.Length() ? " and " : "", titleProfiles[i].displayName,
							titleProfiles[i].baseGameDir, titleProfiles[i].probeFile );
						choices += va( "%s+set com_title %s", choices.Length() ? " or " : "", titleProfiles[i].name );
					}
				}
				common->FatalError( "%s are both in %s - start with %s to choose one", list.c_str(),
					folders[f].c_str(), choices.c_str() );
			}
			for ( int i = 0; i < GAME_TITLE_COUNT; i++ ) {
				if ( found == ( 1 << i ) ) {
					chosen = &titleProfiles[i];
					why = va( "found %s/%s in %s", titleProfiles[i].baseGameDir, titleProfiles[i].probeFile, folders[f].c_str() );
				}
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

/*
================
GameTitle::RequireFullGame
================
*/
void GameTitle::RequireFullGame( void ) {
	const gameTitleProfile_t &title = Get();
	if ( title.fullGameFiles == NULL ) {
		return;
	}
	const char *next = title.fullGameFiles;
	while ( *next != '\0' ) {
		const char *end = strchr( next, ' ' );
		const idStr file( next, 0, end != NULL ? (int)( end - next ) : (int)strlen( next ) );
		next = ( end != NULL ) ? end + 1 : next + file.Length();
		if ( file.Length() > 0 && fileSystem->ReadFile( file.c_str(), NULL, NULL ) < 0 ) {
			common->FatalError( "This %s install is not the full game: %s is missing.\n"
				"DUDE runs the full retail %s only (Steam, GOG or disc); the %s demo is not supported.",
				title.displayName, file.c_str(), title.displayName, title.displayName );
		}
	}
}
