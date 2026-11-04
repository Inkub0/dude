/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: game title profile - see GameTitle.h.

#ifndef ID_DEDICATED
#include "sys/sys_sdl.h"	// before idlib, whose Str.h redefines the C string functions
#endif

#include "sys/platform.h"
#include "idlib/Str.h"
#include "framework/Common.h"
#include "framework/CVarSystem.h"
#include "framework/GameTitle.h"
#include "framework/FileSystem.h"
#include "framework/Licensee.h"
#include "sys/sys_public.h"
#ifndef ID_DEDICATED
void *GLimp_GetSDLWindow( void );	// renderer/tr_local.h
#endif

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
		NULL, NULL, NULL,				// gameLibrary*: base.so ships with DUDE
	},
	{
		GAME_TITLE_QUAKE4, "quake4", "Quake 4",
		"q4base", "pak001.pk4",
		false,							// doom3Layout
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
		"https://github.com/Inkub0/dude-q4",
		"dude-q4",
		"It is built from Raven Software's Quake 4 SDK and is NOT under DUDE's licence (the GPL): "
		"it is under the Quake 4 SDK licence, included in the archive and binding on you - "
		"non-commercial use only, copies only free of charge and with that licence attached, "
		"and only with the full retail Quake 4.",
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

/*
================
GameTitle_Wrap

Word-wraps text for a message box (the X11 one does not wrap, it widens). Lines that
start with spaces (a file name, an address) are kept whole.
================
*/
static idStr GameTitle_Wrap( const char *text, int width ) {
	idStr out;
	const char *line = text;
	while ( *line != '\0' ) {
		const char *end = strchr( line, '\n' );
		const int length = ( end != NULL ) ? (int)( end - line ) : (int)strlen( line );
		if ( line[0] == ' ' ) {
			out.Append( idStr( line, 0, length ) );
		} else {
			int column = 0;
			int i = 0;
			while ( i < length ) {
				int wordEnd = i;
				while ( wordEnd < length && line[wordEnd] != ' ' ) {
					wordEnd++;
				}
				const int wordLength = wordEnd - i;
				if ( wordLength == 0 ) {	// a run of spaces
					i = wordEnd + 1;
					continue;
				}
				if ( column > 0 && column + 1 + wordLength > width ) {
					out.Append( '\n' );
					column = 0;
				} else if ( column > 0 ) {
					out.Append( ' ' );
					column++;
				}
				out.Append( idStr( line, i, wordEnd ) );
				column += wordLength;
				i = wordEnd + 1;
			}
		}
		if ( end == NULL ) {
			break;
		}
		out.Append( '\n' );
		line = end + 1;
	}
	return out;
}

#if !defined( ID_DEDICATED ) && SDL_VERSION_ATLEAST( 2, 0, 0 )
/*
================
GameTitle_DownloadDialog

A message box with the text and two buttons, "Open download page" (opens url in the
browser) and "Quit". False when no dialog could be shown (no display).
================
*/
static bool GameTitle_DownloadDialog( const char *caption, const char *text, const char *url ) {
	if ( !SDL_WasInit( SDL_INIT_VIDEO ) ) {
		return false;
	}
	// the game window is up by now: a fullscreen one would cover the dialog, and the game
	// quits after it anyway
	SDL_Window *window = (SDL_Window *)GLimp_GetSDLWindow();
	if ( window != NULL ) {
		SDL_HideWindow( window );
	}

	enum { CHOICE_QUIT, CHOICE_OPEN };
	SDL_MessageBoxButtonData buttons[2];
	int numButtons = 0;
#if SDL_VERSION_ATLEAST( 2, 0, 14 )	// SDL_OpenURL
	const SDL_MessageBoxButtonData open = { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, CHOICE_OPEN, "Open download page" };
	buttons[numButtons++] = open;
	const SDL_MessageBoxButtonData quit = { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, CHOICE_QUIT, "Quit" };
#else
	const SDL_MessageBoxButtonData quit = { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, CHOICE_QUIT, "Quit" };
#endif
	buttons[numButtons++] = quit;

	const SDL_MessageBoxData box = { SDL_MESSAGEBOX_ERROR, NULL, caption, text, numButtons, buttons, NULL };
	int choice = CHOICE_QUIT;
#if SDL_VERSION_ATLEAST( 3, 0, 0 )
	if ( !SDL_ShowMessageBox( &box, &choice ) ) {
		return false;
	}
#else
	if ( SDL_ShowMessageBox( &box, &choice ) != 0 ) {
		return false;
	}
#endif
#if SDL_VERSION_ATLEAST( 2, 0, 14 )
	if ( choice == CHOICE_OPEN ) {
		SDL_OpenURL( url );
	}
#endif
	return true;
}
#endif

/*
================
GameTitle::GameLibraryMissing
================
*/
void GameTitle::GameLibraryMissing( const char *libraryName, bool incompatible ) {
	const gameTitleProfile_t &title = Get();
	if ( title.gameLibraryProject == NULL ) {
		if ( incompatible ) {
			common->FatalError( "'%s' is not a %s game library for this engine", libraryName, title.displayName );
		}
		common->FatalError( "couldn't load game dynamic library '%s'", libraryName );
		return;
	}

	// the release this build belongs to: ENGINE_VERSION is "DUDE <version>"
	const char *version = strrchr( ENGINE_VERSION, ' ' );
	version = ( version != NULL ) ? version + 1 : ENGINE_VERSION;
#if defined( _WIN64 )
	const char *platform = "win64", *archiveType = ".zip";
#elif defined( __linux__ ) && defined( __x86_64__ )
	const char *platform = "linux-x86_64", *archiveType = ".tar.gz";
#else
	const char *platform = NULL, *archiveType = "";
#endif
	const idStr releasePage = va( "%s/releases/tag/v%s", title.gameLibraryProject, version );
	const idStr archive = ( platform != NULL ) ? va( "%s-%s-%s%s", title.gameLibraryArchive, version, platform, archiveType ) : "";
	idStr exeDir;
	if ( Sys_GetPath( PATH_EXE, exeDir ) ) {
		exeDir.StripFilename();
	}

	idStr text;
	if ( incompatible ) {
		text = va( "The %s game library, %s, is not the one for this version of DUDE (%s).\n\n",
			title.displayName, libraryName, version );
	} else {
		text = va( "DUDE found %s, but not its game library, %s.\n\n", title.displayName, libraryName );
	}
	if ( platform != NULL ) {
		text += va( "The game library is a separate download, from a separate project. For this version of DUDE it is\n"
			"    %s\nfrom the release page\n    %s\n\n", archive.c_str(), releasePage.c_str() );
	} else {
		text += va( "The game library is a separate project, with no ready-made download for this platform: "
			"build it from its source,\n    %s\n\n", title.gameLibraryProject );
	}
	text += title.gameLibraryTerms;
	text += "\n\n";
	if ( exeDir.Length() > 0 ) {
		text += va( "Extract the archive next to the DUDE executable, in\n    %s\nand start DUDE again.", exeDir.c_str() );
	} else {
		text += "Extract the archive next to the DUDE executable and start DUDE again.";
	}
	text = GameTitle_Wrap( text.c_str(), 72 );
	common->Printf( "\n%s\n\n", text.c_str() );

#if !defined( ID_DEDICATED ) && SDL_VERSION_ATLEAST( 2, 0, 0 )
	const idStr caption = va( incompatible ? "%s game library: wrong version" : "%s game library missing", title.displayName );
	GameTitle_DownloadDialog( caption.c_str(), text.c_str(), releasePage.c_str() );
#endif

	if ( platform != NULL ) {
		common->FatalError( "%s game library %s %s - download %s from %s", title.displayName, libraryName,
			incompatible ? "is for another DUDE version" : "is missing", archive.c_str(), releasePage.c_str() );
	}
	common->FatalError( "%s game library %s %s - build it from %s", title.displayName, libraryName,
		incompatible ? "is for another DUDE version" : "is missing", title.gameLibraryProject );
}
