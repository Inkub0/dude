/*
===========================================================================
	Mod library auto-download registry parser.

	Parses the small idLexer text format described by DefaultRegistryText().
	Pure parsing only: no logging, no filesystem, no engine globals beyond
	idlib, so it can run before the engine is up and be unit-tested against
	libidlib.a. See docs/mod-auto-download.md.
===========================================================================
*/

#include "sys/platform.h"
#include "idlib/Lexer.h"

#include "framework/ModRegistry.h"

/*
===============================================================================
	parse helpers (file-local)
===============================================================================
*/

static bool MR_ReadValue( idLexer &lex, idStr &out, idStr *errMsg ) {
	idToken tok;
	if ( !lex.ReadToken( &tok ) ) {
		if ( errMsg ) { *errMsg = "expected a value"; }
		return false;
	}
	out = tok;
	return true;
}

static bool MR_ValidSha256( const idStr &s ) {
	if ( s.Length() != 64 ) {
		return false;
	}
	for ( int i = 0; i < 64; i++ ) {
		const int c = s[i];
		const bool hex = ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ) || ( c >= 'A' && c <= 'F' );
		if ( !hex ) {
			return false;
		}
	}
	return true;
}

static bool MR_ParseAsset( idLexer &lex, modRegistryMod_t &mod, idStr *errMsg ) {
	idToken tok;
	modRegistryAsset_t &a = mod.assets.Alloc();

	if ( !lex.ReadToken( &tok ) ) {
		if ( errMsg ) { *errMsg = "expected asset platform"; }
		return false;
	}
	a.platform = tok;

	if ( !lex.ExpectTokenString( "{" ) ) {
		if ( errMsg ) { *errMsg = "expected '{' after asset platform"; }
		return false;
	}

	while ( lex.ReadToken( &tok ) ) {
		if ( tok == "}" ) {
			if ( a.url.IsEmpty() || a.sha256.IsEmpty() ) {
				if ( errMsg ) { *errMsg = idStr( "asset '" ) + a.platform + "' missing url or sha256"; }
				return false;
			}
			if ( !MR_ValidSha256( a.sha256 ) ) {
				if ( errMsg ) { *errMsg = idStr( "asset '" ) + a.platform + "' has a malformed sha256"; }
				return false;
			}
			return true;
		}
		if ( tok == "url" ) {
			if ( !MR_ReadValue( lex, a.url, errMsg ) ) { return false; }
		} else if ( tok == "sha256" ) {
			if ( !MR_ReadValue( lex, a.sha256, errMsg ) ) { return false; }
		} else {
			if ( errMsg ) { *errMsg = idStr( "unexpected token in asset: " ) + tok; }
			return false;
		}
	}
	if ( errMsg ) { *errMsg = "unexpected end of file in asset block"; }
	return false;
}

static bool MR_ParseMod( idLexer &lex, idList<modRegistryMod_t> &mods, idStr *errMsg ) {
	idToken tok;
	modRegistryMod_t &mod = mods.Alloc();

	if ( !lex.ReadToken( &tok ) ) {
		if ( errMsg ) { *errMsg = "expected mod key"; }
		return false;
	}
	mod.key = tok;

	if ( !lex.ExpectTokenString( "{" ) ) {
		if ( errMsg ) { *errMsg = idStr( "expected '{' after mod '" ) + mod.key + "'"; }
		return false;
	}

	while ( lex.ReadToken( &tok ) ) {
		if ( tok == "}" ) {
			if ( mod.key.IsEmpty() ) {
				if ( errMsg ) { *errMsg = "mod has no key"; }
				return false;
			}
			return true;
		}
		if ( tok == "name" ) {
			if ( !MR_ReadValue( lex, mod.name, errMsg ) ) { return false; }
		} else if ( tok == "repo" ) {
			if ( !MR_ReadValue( lex, mod.repo, errMsg ) ) { return false; }
		} else if ( tok == "release" ) {
			if ( !MR_ReadValue( lex, mod.release, errMsg ) ) { return false; }
		} else if ( tok == "license" ) {
			if ( !MR_ReadValue( lex, mod.license, errMsg ) ) { return false; }
		} else if ( tok == "needsData" ) {
			if ( !MR_ReadValue( lex, mod.needsData, errMsg ) ) { return false; }
		} else if ( tok == "asset" ) {
			if ( !MR_ParseAsset( lex, mod, errMsg ) ) { return false; }
		} else {
			if ( errMsg ) { *errMsg = idStr( "unexpected token in mod: " ) + tok; }
			return false;
		}
	}
	if ( errMsg ) { *errMsg = "unexpected end of file in mod block"; }
	return false;
}

/*
===============================================================================
	idModRegistry
===============================================================================
*/

void idModRegistry::Clear( void ) {
	version = 0;
	mods.Clear();
}

bool idModRegistry::ParseString( const char *text, const char *sourceName, idStr *errMsg ) {
	Clear();

	if ( text == NULL ) {
		if ( errMsg ) { *errMsg = "null registry text"; }
		return false;
	}

	idLexer lex;
	lex.SetFlags( LEXFL_NOERRORS | LEXFL_NOWARNINGS | LEXFL_NOFATALERRORS | LEXFL_NOSTRINGESCAPECHARS );
	if ( !lex.LoadMemory( text, (int)strlen( text ), sourceName ? sourceName : "modRegistry" ) ) {
		if ( errMsg ) { *errMsg = "could not load registry text"; }
		return false;
	}

	idToken tok;
	if ( !lex.ExpectTokenString( "modRegistry" ) || !lex.ExpectTokenString( "{" ) ) {
		if ( errMsg ) { *errMsg = "expected 'modRegistry {' at the start"; }
		return false;
	}

	while ( lex.ReadToken( &tok ) ) {
		if ( tok == "}" ) {
			return true;
		}
		if ( tok == "version" ) {
			idToken v;
			if ( !lex.ReadToken( &v ) ) {
				if ( errMsg ) { *errMsg = "expected version number"; }
				return false;
			}
			version = v.GetIntValue();
		} else if ( tok == "mod" ) {
			if ( !MR_ParseMod( lex, mods, errMsg ) ) {
				return false;
			}
		} else {
			if ( errMsg ) { *errMsg = idStr( "unexpected token: " ) + tok; }
			return false;
		}
	}

	if ( errMsg ) { *errMsg = "unexpected end of file (missing closing '}')"; }
	return false;
}

const modRegistryMod_t *idModRegistry::FindMod( const char *fsGame ) const {
	if ( fsGame == NULL || fsGame[0] == '\0' ) {
		return NULL;
	}
	for ( int i = 0; i < mods.Num(); i++ ) {
		if ( mods[i].key.Icmp( fsGame ) == 0 ) {
			return &mods[i];
		}
	}
	return NULL;
}

const modRegistryAsset_t *modRegistryMod_t::FindAsset( const char *platform ) const {
	if ( platform == NULL || platform[0] == '\0' ) {
		return NULL;
	}
	for ( int i = 0; i < assets.Num(); i++ ) {
		if ( assets[i].platform.Icmp( platform ) == 0 ) {
			return &assets[i];
		}
	}
	return NULL;
}

const char *idModRegistry::CurrentPlatformKey( void ) {
#if defined(_WIN32) || defined(WIN32)
	// the DUDE Windows build is 64-bit; keep a stable key matching the release asset
	return "win64";
#elif defined(MACOS_X) || defined(__APPLE__)
	return "macos-" D3_ARCH;
#else
	return "linux-" D3_ARCH;
#endif
}

const char *idModRegistry::DefaultRegistryText( void ) {
	// Baked-in fallback, used when the hosted registry cannot be fetched or
	// parsed. The hosted copy (in the main repo) is authoritative; keep this in
	// sync with it. Hashes are the SHA-256 of the exact library files published
	// as the dude-phobos v1.0.0 release assets.
	return
		"modRegistry {\n"
		"\tversion 1\n"
		"\tmod \"tfphobos\" {\n"
		"\t\tname      \"Doom 3: Phobos game library\"\n"
		"\t\trepo      \"Inkub0/dude-phobos\"\n"
		"\t\trelease   \"v1.0.0\"\n"
		"\t\tlicense   \"GPLv3\"\n"
		"\t\tneedsData \"Doom 3: Phobos game data (Team Future) - not downloadable\"\n"
		"\t\tasset \"win64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-phobos/releases/download/v1.0.0/tfphobos-win64.dll\"\n"
		"\t\t\tsha256 \"17daac0538b3d5f5d07512d2227dc8d7dd93f2bf1f85695f7567685c39047bd0\"\n"
		"\t\t}\n"
		"\t\tasset \"linux-x86_64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-phobos/releases/download/v1.0.0/tfphobos-linux-x86_64.so\"\n"
		"\t\t\tsha256 \"1325a8849ac4bace7a90c58daf1b965cf63a6449ec019afb326bd650a7b58b32\"\n"
		"\t\t}\n"
		"\t}\n"
		"\tmod \"cdoom\" {\n"
		"\t\tname      \"Classic Doom 3 game library\"\n"
		"\t\trepo      \"Inkub0/dude-classicdoom3\"\n"
		"\t\trelease   \"v1.0.0\"\n"
		"\t\tlicense   \"GPLv3\"\n"
		"\t\tneedsData \"Classic Doom 3 game data (Flaming Sheep) + Doom 3 - not downloadable\"\n"
		"\t\tasset \"win64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-classicdoom3/releases/download/v1.0.0/cdoom-win64.dll\"\n"
		"\t\t\tsha256 \"97860127bc51ab735042370b7e5f365320c4101e55975589bcd061d0bde7536c\"\n"
		"\t\t}\n"
		"\t\tasset \"linux-x86_64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-classicdoom3/releases/download/v1.0.0/cdoom-linux-x86_64.so\"\n"
		"\t\t\tsha256 \"28e57e1a709555cdd30d06a92d0676ca9f8913dac9b36bf97b57cf1f78b86c32\"\n"
		"\t\t}\n"
		"\t}\n"
		"}\n";
}
