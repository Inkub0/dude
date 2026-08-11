// DUDE: mod compatibility shim — see ModCvarTranslation.h.

#include "sys/platform.h"
#include "idlib/Str.h"
#include "idlib/containers/StrList.h"
#include "idlib/containers/HashIndex.h"
#include "framework/CVarSystem.h"
#include "framework/CmdSystem.h"
#include "framework/FileSystem.h"
#include "framework/Common.h"

#include "framework/ModCvarTranslation.h"

// Public toggle; the shim is a no-op when this is off (both aliasing and
// autoCustomRes short-circuit). Archived so users can persist their choice.
static idCVar fs_modCompatShim( "fs_modCompatShim", "1", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_BOOL,
	"apply base/mods-cvar-translation.cfg to redirect mod cvar names to their DUDE equivalents "
	"(and other small per-mod tweaks) while the active fs_game matches an entry" );

namespace {

struct ActiveShim {
	idStr			modName;
	idStrList		aliasFrom;		// mod-side cvar name (lowercased)
	idStrList		aliasTo;		// DUDE-side cvar name (verbatim)
	idHashIndex		aliasHash;
	bool			autoCustomRes;
	bool			loaded;

	void Clear() {
		modName.Clear();
		aliasFrom.Clear();
		aliasTo.Clear();
		aliasHash.Free();
		autoCustomRes = false;
		loaded = false;
	}
};

static ActiveShim shim;
static const char *CFG_PATH = "mods-cvar-translation.cfg";

// Case-insensitive equal for the "mod" block header check.
static bool StreqI( const char *a, const char *b ) {
	return idStr::Icmp( a, b ) == 0;
}

// Split a line into whitespace-separated tokens (up to 4 — more than we need).
// Strips inline `#` and `//` comments. Modifies `line` in place.
static int Tokenize( char *line, char *tokens[4] ) {
	// Strip inline comments.
	for ( char *p = line; *p; ++p ) {
		if ( *p == '#' || ( *p == '/' && *(p+1) == '/' ) ) {
			*p = '\0';
			break;
		}
	}
	int n = 0;
	char *p = line;
	while ( *p && n < 4 ) {
		while ( *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ) { *p++ = '\0'; }
		if ( !*p ) break;
		tokens[n++] = p;
		while ( *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' ) { ++p; }
	}
	return n;
}

// Parse the cfg buffer, keeping only entries for `activeMod`. Case-insensitive
// on mod name and alias-from; alias-to is written verbatim.
static void ParseInto( ActiveShim &out, const char *buf, int len, const char *activeMod ) {
	out.Clear();
	out.modName = activeMod;

	// Copy to a mutable buffer so Tokenize can NUL-terminate in place.
	idStr text( buf );
	char *cursor = &text[0];
	char *end = cursor + text.Length();

	bool inBlock = false;
	int lineNo = 0;

	while ( cursor < end ) {
		char *lineStart = cursor;
		while ( cursor < end && *cursor != '\n' ) { ++cursor; }
		if ( cursor < end ) { *cursor++ = '\0'; }
		++lineNo;

		char *tok[4] = { NULL, NULL, NULL, NULL };
		int n = Tokenize( lineStart, tok );
		if ( n == 0 ) continue;

		if ( StreqI( tok[0], "mod" ) ) {
			if ( n < 2 ) {
				common->Warning( "mods-cvar-translation.cfg:%d: `mod` needs a name", lineNo );
				inBlock = false;
				continue;
			}
			inBlock = StreqI( tok[1], activeMod );
			continue;
		}

		if ( !inBlock ) continue;

		if ( StreqI( tok[0], "alias" ) ) {
			if ( n < 3 ) {
				common->Warning( "mods-cvar-translation.cfg:%d: `alias <from> <to>` needs 2 names", lineNo );
				continue;
			}
			idStr from = tok[1];
			from.ToLower();
			int hash = out.aliasHash.GenerateKey( from.c_str(), false );
			// Duplicate-key: last definition wins (allow user to override our defaults).
			for ( int i = out.aliasHash.First( hash ); i != -1; i = out.aliasHash.Next( i ) ) {
				if ( out.aliasFrom[i] == from ) {
					out.aliasTo[i] = tok[2];
					goto next_line;
				}
			}
			out.aliasFrom.Append( from );
			out.aliasTo.Append( idStr( tok[2] ) );
			out.aliasHash.Add( hash, out.aliasFrom.Num() - 1 );
			next_line: ;
			continue;
		}

		if ( StreqI( tok[0], "autoCustomRes" ) ) {
			out.autoCustomRes = ( n >= 2 && atoi( tok[1] ) != 0 );
			continue;
		}

		common->Warning( "mods-cvar-translation.cfg:%d: unknown directive `%s`", lineNo, tok[0] );
	}

	out.loaded = true;
}

static void Reload_f( const idCmdArgs & ) {
	ModCompat::Init();
	common->Printf( "mod-cvar-translation reloaded: mod='%s', %d aliases, autoCustomRes=%d\n",
		shim.modName.c_str(), shim.aliasFrom.Num(), shim.autoCustomRes ? 1 : 0 );
}

} // namespace

namespace ModCompat {

void Init( void ) {
	shim.Clear();

	// Active mod = fs_game if set, else base. base itself doesn't need aliases,
	// so we only parse when a mod is active (keeps hot-path free of lookups
	// for stock DOOM3/RoE).
	const char *activeMod = cvarSystem->GetCVarString( "fs_game" );
	if ( !activeMod || !activeMod[0] ) {
		shim.loaded = true;
		return;
	}

	void *buf = NULL;
	int len = fileSystem->ReadFile( CFG_PATH, &buf, NULL );
	if ( len <= 0 || !buf ) {
		if ( buf ) fileSystem->FreeFile( buf );
		shim.loaded = true;
		return;
	}

	ParseInto( shim, (const char *)buf, len, activeMod );
	fileSystem->FreeFile( buf );

	if ( shim.aliasFrom.Num() > 0 || shim.autoCustomRes ) {
		common->Printf( "mod-cvar-translation: '%s' → %d aliases%s\n",
			activeMod, shim.aliasFrom.Num(),
			shim.autoCustomRes ? ", autoCustomRes" : "" );
	}

	// Command is idempotent — Init may be called multiple times (startup +
	// reload). Guarded by the cmd system's own dup-name behavior.
	static bool cmdRegistered = false;
	if ( !cmdRegistered ) {
		cmdSystem->AddCommand( "mods_translation_reload", Reload_f, CMD_FL_SYSTEM,
			"reload base/mods-cvar-translation.cfg" );
		cmdRegistered = true;
	}
}

void Shutdown( void ) {
	shim.Clear();
}

const char *ResolveAlias( const char *name ) {
	if ( !name || !shim.loaded || !fs_modCompatShim.GetBool() ) return name;
	if ( shim.aliasFrom.Num() == 0 ) return name;

	// Cheap case-insensitive hash lookup — build a lowercase key on the stack
	// for names up to 64 chars (all real cvar names fit).
	char lower[64];
	int i = 0;
	for ( ; name[i] && i < 63; ++i ) {
		char c = name[i];
		lower[i] = ( c >= 'A' && c <= 'Z' ) ? ( c + 32 ) : c;
	}
	lower[i] = '\0';

	int hash = shim.aliasHash.GenerateKey( lower, false );
	for ( int idx = shim.aliasHash.First( hash ); idx != -1; idx = shim.aliasHash.Next( idx ) ) {
		if ( shim.aliasFrom[idx] == lower ) {
			return shim.aliasTo[idx].c_str();
		}
	}
	return name;
}

bool AutoCustomRes( void ) {
	return shim.loaded && fs_modCompatShim.GetBool() && shim.autoCustomRes;
}

bool IsForeignMod( void ) {
	if ( !fs_modCompatShim.GetBool() ) return false;
	const char *g = cvarSystem->GetCVarString( "fs_game" );
	if ( !g || !g[0] ) return false;
	if ( idStr::Icmp( g, "base" ) == 0 ) return false;
	if ( idStr::Icmp( g, "d3xp" ) == 0 ) return false;
	return true;
}

const char *WarningTag( void ) {
	if ( !IsForeignMod() ) return "";
	return cvarSystem->GetCVarString( "fs_game" );
}

} // namespace ModCompat
