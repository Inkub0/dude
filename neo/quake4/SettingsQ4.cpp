/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: the engine settings Quake 4's own menus expect (quake4/SettingsQ4.h). Values and meanings
// are retail Quake 4's (its menu labels, its shipped configs; OpenQ4's notes on the retail engine
// were read for the mix rule, none of its code used):
//
//   s_volume            Volume            0..2, linear; DUDE's s_volume_dB in step with it
//   s_speakerFraction   Ambient Volume    0.65; every positional sound but voice-over and music
//                                         (voice-over stays 3.7 dB above the rest, as retail)
//   s_musicVolume       Music Volume      0.5; sound class 3 (SOUND_CLASS_MUSICAL)
//   s_deviceName        Sound Device      the OpenAL device, DUDE's s_device in step with it
//   r_skipSky           Enable detailed sky: No - no portal sky view
//   r_forceAmbient      Force ambient light / Ambient brightness (1.4 patch): 0 off (retail's
//                       default), the menu's toggle sets 0.7 - one shadowless ambient light over
//                       the whole map (RenderWorld.cpp R_UpdateForcedAmbientLights)
//   com_clickToContinue DUDE's: 1 (retail) - a new map's loading screen waits for a click;
//                       0 goes straight in. Loaded savegames never wait.
//   s_useOpenAL, r_useSmp, in_freeLook, the multiplayer and voice chat ones:
//                       accepted so the menu works, no effect (DUDE always uses OpenAL and its
//                       own threading, mouse look is always on, multiplayer is not implemented)
//
// The main menu's resolution lists are DUDE's own video modes (r_mode), grouped by aspect ratio
// into Quake 4's 4:3, 16:9 and 16:10 lists; the aspect choice adds Auto (r_aspectRatio -1, the
// window's shape, which the Quake 4 game library understands).

#include "sys/platform.h"
#include "framework/Common.h"
#include "framework/CVarSystem.h"
#include "framework/CmdSystem.h"
#include "framework/GameTitle.h"
#include "idlib/Lexer.h"
#include "idlib/containers/StrList.h"
#include "renderer/RenderSystem.h"
#include "ui/UserInterface.h"

#include "quake4/SettingsQ4.h"

#ifndef ID_DEDICATED
#include <AL/al.h>
#include <AL/alc.h>
#endif

bool R_GetModeInfo( int *width, int *height, int mode );

static idCVar *				q4_volume = NULL;
static idCVar *				q4_speakerFraction = NULL;
static idCVar *				q4_musicVolume = NULL;
static idCVar *				q4_deviceName = NULL;
static idCVar *				q4_skipSky = NULL;
static idCVar *				q4_forceAmbient = NULL;

static float				q4_lastVolume = -1.0f;			// what s_volume / s_volume_dB were last kept at
static float				q4_lastVolumeDb = 1.0e30f;
static idStr				q4_lastDeviceName;
static idStr				q4_lastDevice;

static const float			Q4_VOLUME_SILENT_DB = -60.0f;		// s_volume 0

static void					Q4_SoundDevices( idStrList &devices );
static idStr				Q4_MenuDeviceName( const char *device );
static idStr				Q4_ChoiceValue( const char *text );

/*
================
Q4Settings_Init
================
*/
void Q4Settings_Init( void ) {
	if ( GameTitle::Get().title != GAME_TITLE_QUAKE4 || q4_volume != NULL ) {
		return;
	}

	// created now that the title is known (idCVar registers itself once the static list is in);
	// a value the config set before is kept
	q4_volume			= new idCVar( "s_volume", "1", CVAR_SOUND | CVAR_ARCHIVE | CVAR_FLOAT, "Quake 4 menu: master volume, 0..2 linear (DUDE's s_volume_dB follows it)", 0.0f, 2.0f );
	q4_speakerFraction	= new idCVar( "s_speakerFraction", "0.65", CVAR_SOUND | CVAR_ARCHIVE | CVAR_FLOAT, "Quake 4: Ambient Volume - scales every positional sound but voice-over and music", 0.0f, 1.0f );
	q4_musicVolume		= new idCVar( "s_musicVolume", "0.5", CVAR_SOUND | CVAR_ARCHIVE | CVAR_FLOAT, "Quake 4: Music Volume - scales sound class 3", 0.0f, 1.0f );
	q4_deviceName		= new idCVar( "s_deviceName", "", CVAR_SOUND | CVAR_ARCHIVE, "Quake 4 menu: the sound device (DUDE's s_device follows it; applied by s_restart)" );
	q4_skipSky			= new idCVar( "r_skipSky", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "Quake 4: 'Enable detailed sky' off - no portal sky view" );

	new idCVar( "com_clickToContinue", "1", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_BOOL,
		"Quake 4: a new map's loading screen waits for a click to continue, as retail (0: straight into the game; loaded savegames never wait)" );
	new idCVar( "s_useOpenAL", "1", CVAR_SOUND | CVAR_ARCHIVE | CVAR_BOOL, "Quake 4 menu: sound system - DUDE always uses OpenAL" );
	new idCVar( "r_useSmp", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "Quake 4 menu: multiple CPU optimization - DUDE uses its own threading" );
	new idCVar( "in_freeLook", "1", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_BOOL, "Quake 4 menu: free look - mouse look is always on in DUDE" );
	q4_forceAmbient		= new idCVar( "r_forceAmbient", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "Quake 4: force a single ambient light throughout the level if > 0 (intensity 0..1)", 0.0f, 1.0f );

	// multiplayer and voice chat (docs/quake4-port.md Q9)
	new idCVar( "net_menulanserver", "0", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_INTEGER, "Quake 4 menu: LAN server - multiplayer is not implemented" );
	new idCVar( "net_serverMenuDedicated", "0", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_BOOL, "Quake 4 menu: dedicated server - multiplayer is not implemented" );
	new idCVar( "s_voiceVolume", "1", CVAR_SOUND | CVAR_ARCHIVE | CVAR_FLOAT, "Quake 4 menu: voice chat volume - not implemented" );
	new idCVar( "s_micInputLevel", "1", CVAR_SOUND | CVAR_ARCHIVE | CVAR_FLOAT, "Quake 4 menu: microphone level - not implemented" );
	new idCVar( "s_voiceChatSend", "0", CVAR_SOUND | CVAR_ARCHIVE | CVAR_BOOL, "Quake 4 menu: voice chat - not implemented" );
	new idCVar( "s_voiceChatReceive", "0", CVAR_SOUND | CVAR_ARCHIVE | CVAR_BOOL, "Quake 4 menu: voice chat - not implemented" );
	new idCVar( "s_voiceChatEcho", "0", CVAR_SOUND | CVAR_ARCHIVE | CVAR_BOOL, "Quake 4 menu: voice chat - not implemented" );
	new idCVar( "ui_handicap", "100", CVAR_GAME | CVAR_ARCHIVE | CVAR_INTEGER, "Quake 4 menu: multiplayer handicap - not implemented" );
}

/*
================
Q4Settings_Frame
================
*/
void Q4Settings_Frame( void ) {
	if ( q4_volume == NULL ) {
		return;
	}

	// the master volume: whichever side changed last wins; DUDE's own setting the first time
	const float volume = q4_volume->GetFloat();
	const float volumeDb = cvarSystem->GetCVarFloat( "s_volume_dB" );
	if ( q4_lastVolume < 0.0f || ( volumeDb != q4_lastVolumeDb && volume == q4_lastVolume ) ) {
		q4_volume->SetFloat( idMath::ClampFloat( 0.0f, 2.0f, idMath::dBToScale( volumeDb ) ) );
	} else if ( volume != q4_lastVolume ) {
		// id's dB rule, 6 dB per doubling (idMath::dBToScale, the mixer's dB2Scale)
		const float db = volume > 0.001f ? 6.0f * log2f( volume ) : Q4_VOLUME_SILENT_DB;
		cvarSystem->SetCVarFloat( "s_volume_dB", db );
	}
	q4_lastVolume = q4_volume->GetFloat();
	q4_lastVolumeDb = cvarSystem->GetCVarFloat( "s_volume_dB" );

	// the device: the menu's choice (a device name as its list reads it back) to s_device, and
	// back; "" and "default" are the default device
	const char *deviceName = q4_deviceName->GetString();
	const char *device = cvarSystem->GetCVarString( "s_device" );
	if ( q4_lastDeviceName.Cmp( deviceName ) != 0 ) {
		idStr chosen = "default";
		if ( deviceName[0] != '\0' && idStr::Icmp( deviceName, "default" ) != 0 ) {
			idStrList devices;
			Q4_SoundDevices( devices );
			for ( int i = 1; i < devices.Num(); i++ ) {
				if ( devices[i].Cmp( deviceName ) == 0 || Q4_ChoiceValue( Q4_MenuDeviceName( devices[i] ) ).Cmp( deviceName ) == 0 ) {
					chosen = devices[i];
					break;
				}
			}
		}
		cvarSystem->SetCVarString( "s_device", chosen );
	} else if ( q4_lastDevice.Cmp( device ) != 0 ) {
		q4_deviceName->SetString( idStr::Icmp( device, "default" ) == 0 ? "" : Q4_ChoiceValue( Q4_MenuDeviceName( device ) ).c_str() );
	}
	q4_lastDeviceName = q4_deviceName->GetString();
	q4_lastDevice = cvarSystem->GetCVarString( "s_device" );
}

/*
================
S_Q4SpeakerFraction, S_Q4MusicVolume, R_Q4SkipSky, R_Q4ForceAmbient
================
*/
float S_Q4SpeakerFraction( void ) {
	return q4_speakerFraction != NULL ? idMath::ClampFloat( 0.0f, 1.0f, q4_speakerFraction->GetFloat() ) : 1.0f;
}

float S_Q4MusicVolume( void ) {
	return q4_musicVolume != NULL ? idMath::ClampFloat( 0.0f, 1.0f, q4_musicVolume->GetFloat() ) : 1.0f;
}

bool R_Q4SkipSky( void ) {
	return q4_skipSky != NULL && q4_skipSky->GetBool();
}

float R_Q4ForceAmbient( void ) {
	return q4_forceAmbient != NULL ? idMath::ClampFloat( 0.0f, 1.0f, q4_forceAmbient->GetFloat() ) : 0.0f;
}

/*
===============================================================================

	the main menu

===============================================================================
*/

enum { Q4_ASPECT_4_3, Q4_ASPECT_16_9, Q4_ASPECT_16_10, Q4_ASPECT_COUNT };

/*
================
Q4_AspectOf

Quake 4's aspect list for a size: 4:3 (and 5:4), 16:9, 16:10; -1 for any other (21:9, 32:9).
================
*/
static int Q4_AspectOf( int width, int height ) {
	if ( width <= 0 || height <= 0 ) {
		return -1;
	}
	const float ratio = (float)width / (float)height;
	if ( idMath::Fabs( ratio - 4.0f / 3.0f ) < 0.06f || idMath::Fabs( ratio - 1.25f ) < 0.01f ) {
		return Q4_ASPECT_4_3;
	}
	if ( idMath::Fabs( ratio - 16.0f / 9.0f ) < 0.03f ) {
		return Q4_ASPECT_16_9;
	}
	if ( idMath::Fabs( ratio - 1.6f ) < 0.03f ) {
		return Q4_ASPECT_16_10;
	}
	return -1;
}

// the current size's aspect list, for a window shape outside Quake 4's three the nearest
static int Q4_CurrentAspect( void ) {
	const int r = cvarSystem->GetCVarInteger( "r_aspectRatio" );
	if ( r >= 0 && r < Q4_ASPECT_COUNT ) {
		return r;
	}
	const int width = renderSystem->GetScreenWidth();
	const int height = renderSystem->GetScreenHeight();
	const int aspect = Q4_AspectOf( width, height );
	if ( aspect >= 0 ) {
		return aspect;
	}
	const float ratio = height > 0 ? (float)width / height : 4.0f / 3.0f;
	return ratio > 1.7f ? Q4_ASPECT_16_9 : ( ratio > 1.45f ? Q4_ASPECT_16_10 : Q4_ASPECT_4_3 );
}

// DUDE's video modes (r_mode 3 and up, as Doom 3's menu lists them) of one aspect list, smallest
// first
static void Q4_ModesOfAspect( int aspect, idList<int> &modes ) {
	modes.Clear();
	int width, height;
	for ( int mode = 3; R_GetModeInfo( &width, &height, mode ); mode++ ) {
		if ( Q4_AspectOf( width, height ) != aspect ) {
			continue;
		}
		int at = modes.Num();
		for ( int i = 0; i < modes.Num(); i++ ) {
			int w, h;
			R_GetModeInfo( &w, &h, modes[i] );
			if ( w * h > width * height ) {
				at = i;
				break;
			}
		}
		modes.Insert( mode, at );
	}
}

/*
================
Q4_MenuDeviceName

A device name as the menu lists it: letters, digits and single spaces. Quake 4's device choice
builds its choices and its values from the same list, and the choice window parses the two with
different lexer settings ('-' glued to the next token in values only, errors fatal in values only):
device names full of '-', '/', '(' made the two lists different lengths.
================
*/
static idStr Q4_MenuDeviceName( const char *device ) {
	idStr out;
	bool space = false;
	for ( const char *c = device; *c; c++ ) {
		if ( idStr::CharIsAlpha( *c ) || idStr::CharIsNumeric( *c ) ) {
			if ( space && out.Length() ) {
				out += ' ';
			}
			out += *c;
			space = false;
		} else {
			space = true;
		}
	}
	return out.Length() ? out : idStr( "device" );
}

/*
================
Q4_SoundDevices

"default" and the OpenAL devices (their real names).
================
*/
static void Q4_SoundDevices( idStrList &devices ) {
	devices.Clear();
	devices.Append( "default" );
#ifndef ID_DEDICATED
	if ( alcIsExtensionPresent( NULL, "ALC_ENUMERATE_ALL_EXT" ) ) {
		for ( const char *dev = alcGetString( NULL, ALC_ALL_DEVICES_SPECIFIER ); dev != NULL && *dev; dev += strlen( dev ) + 1 ) {
			if ( strchr( dev, ';' ) == NULL ) {		// ';' separates the menu's entries
				devices.Append( dev );
			}
		}
	}
#endif
}

/*
================
Q4_ChoiceValue

A string as a choice window's values list reads it back (idChoiceWindow::UpdateChoicesAndVals:
the lexer's tokens joined by spaces, a '-' glued to the next): what the window then writes to its
cvar. The device names go through it, so s_deviceName can be matched to a device.
================
*/
static idStr Q4_ChoiceValue( const char *text ) {
	idLexer src( LEXFL_ALLOWPATHNAMES | LEXFL_ALLOWMULTICHARLITERALS | LEXFL_ALLOWBACKSLASHSTRINGCONCAT | LEXFL_NOFATALERRORS | LEXFL_NOWARNINGS );
	src.LoadMemory( text, strlen( text ), "<ChoiceVals>" );
	idToken token;
	idStr out;
	bool negNum = false;
	while ( src.ReadToken( &token ) ) {
		if ( token == "-" ) {
			negNum = true;
			continue;
		}
		if ( negNum ) {
			out += "-";
			negNum = false;
		}
		out += token;
		out += " ";
	}
	out.StripTrailingWhitespace();
	return out;
}

/*
================
Q4Settings_SetMenuGuiVars
================
*/
void Q4Settings_SetMenuGuiVars( idUserInterface *gui ) {
	if ( q4_volume == NULL || gui == NULL ) {
		return;
	}

	// the resolution lists, one per aspect; a custom size (r_mode -1) leads each
	static const char *listNames[Q4_ASPECT_COUNT] = { "4_3", "16_9", "16_10" };
	for ( int aspect = 0; aspect < Q4_ASPECT_COUNT; aspect++ ) {
		idList<int> modes;
		Q4_ModesOfAspect( aspect, modes );
		idStr choices = "Custom";
		idStr values = "-1";
		for ( int i = 0; i < modes.Num(); i++ ) {
			int width, height;
			R_GetModeInfo( &width, &height, modes[i] );
			choices += va( ";%d x %d", width, height );
			values += va( ";%d", modes[i] );
		}
		gui->SetStateString( va( "%s_choices", listNames[aspect] ), choices );
		gui->SetStateString( va( "%s_values", listNames[aspect] ), values );
	}

	// the aspect choice (Auto = the window's shape); which list shows follows the size's aspect
	gui->SetStateString( "aspect_choices", "Auto;4:3;16:9;16:10" );
	gui->SetStateString( "aspect_values", "-1;0;1;2" );
	gui->SetStateInt( "r_aspectRatio", Q4_CurrentAspect() );

	// the sound devices; s_deviceName holds the name as the list reads it back
	idStrList devices;
	Q4_SoundDevices( devices );
	idStr list;
	for ( int i = 0; i < devices.Num(); i++ ) {
		if ( i ) {
			list += ";";
		}
		list += i == 0 ? devices[i] : Q4_MenuDeviceName( devices[i] );
	}
	gui->SetStateString( "device_name", list );
	const char *device = cvarSystem->GetCVarString( "s_device" );
	for ( int i = 0; i < devices.Num(); i++ ) {
		if ( devices[i].Icmp( device ) == 0 ) {
			q4_deviceName->SetString( i == 0 ? "" : Q4_ChoiceValue( Q4_MenuDeviceName( devices[i] ) ).c_str() );
			break;
		}
	}
}

/*
================
Q4Settings_HandleMenuCommand

fixup_mode <aspect>: the aspect choice changed - keep r_mode if it is in that aspect's list (or
custom), else the mode of that list closest in size.
================
*/
bool Q4Settings_HandleMenuCommand( const char *cmd, const idCmdArgs &args, int &icmd ) {
	if ( q4_volume == NULL || idStr::Icmp( cmd, "fixup_mode" ) != 0 ) {
		return false;
	}
	const int aspect = icmd < args.Argc() ? atoi( args.Argv( icmd++ ) ) : -1;
	const int mode = cvarSystem->GetCVarInteger( "r_mode" );
	int width, height;
	if ( aspect < 0 || aspect >= Q4_ASPECT_COUNT || mode < 0 || !R_GetModeInfo( &width, &height, mode ) ) {
		return true;
	}
	if ( Q4_AspectOf( width, height ) == aspect ) {
		return true;
	}
	idList<int> modes;
	Q4_ModesOfAspect( aspect, modes );
	int best = -1;
	int bestDiff = 0;
	for ( int i = 0; i < modes.Num(); i++ ) {
		int w, h;
		R_GetModeInfo( &w, &h, modes[i] );
		const int diff = idMath::Abs( w * h - width * height );
		if ( best < 0 || diff < bestDiff ) {
			best = modes[i];
			bestDiff = diff;
		}
	}
	if ( best >= 0 ) {
		cvarSystem->SetCVarInteger( "r_mode", best );
	}
	return true;
}

/*
================
Q4Settings_MirrorChoice
================
*/
void Q4Settings_MirrorChoice( idUserInterface *gui, const char *cvarName, const char *value ) {
	if ( q4_volume == NULL || gui == NULL || cvarName == NULL || value == NULL ) {
		return;
	}
	// the aspect choice picks which resolution list shows: Auto shows the size's own
	if ( idStr::Icmp( cvarName, "r_aspectRatio" ) == 0 && atoi( value ) < 0 ) {
		gui->SetStateInt( cvarName, Q4_CurrentAspect() );
		return;
	}
	gui->SetStateString( cvarName, value );
}
