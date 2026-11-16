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

int idModRegistry::AddMissing( const idModRegistry &other ) {
	int added = 0;
	for ( int i = 0; i < other.mods.Num(); i++ ) {
		if ( FindMod( other.mods[i].key.c_str() ) == NULL ) {
			mods.Append( other.mods[i] );
			added++;
		}
	}
	return added;
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
	// as the <branch>-vX.Y.Z release assets of Inkub0/dude-sdk (the DUDE Mod
	// SDK, one branch per mod).
	return
		"modRegistry {\n"
		"\tversion 1\n"
		"\tmod \"tfphobos\" {\n"
		"\t\tname      \"Doom 3: Phobos game library\"\n"
		"\t\trepo      \"Inkub0/dude-sdk\"\n"
		"\t\trelease   \"tfphobos-v1.0.0\"\n"
		"\t\tlicense   \"GPLv3\"\n"
		"\t\tneedsData \"Doom 3: Phobos game data (Team Future) - not downloadable\"\n"
		"\t\tasset \"win64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/tfphobos-v1.0.0/tfphobos-win64.dll\"\n"
		"\t\t\tsha256 \"17daac0538b3d5f5d07512d2227dc8d7dd93f2bf1f85695f7567685c39047bd0\"\n"
		"\t\t}\n"
		"\t\tasset \"linux-x86_64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/tfphobos-v1.0.0/tfphobos-linux-x86_64.so\"\n"
		"\t\t\tsha256 \"8d5ff722221279f2ba42b215d0569e6376c0ac39c16e2a129e7e31475eda98c1\"\n"
		"\t\t}\n"
		"\t}\n"
		"\tmod \"cdoom\" {\n"
		"\t\tname      \"Classic Doom 3 game library\"\n"
		"\t\trepo      \"Inkub0/dude-sdk\"\n"
		"\t\trelease   \"cdoom-v1.0.0\"\n"
		"\t\tlicense   \"GPLv3\"\n"
		"\t\tneedsData \"Classic Doom 3 game data (Flaming Sheep) + Doom 3 - not downloadable\"\n"
		"\t\tasset \"win64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/cdoom-v1.0.0/cdoom-win64.dll\"\n"
		"\t\t\tsha256 \"97860127bc51ab735042370b7e5f365320c4101e55975589bcd061d0bde7536c\"\n"
		"\t\t}\n"
		"\t\tasset \"linux-x86_64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/cdoom-v1.0.0/cdoom-linux-x86_64.so\"\n"
		"\t\t\tsha256 \"0d663c93ac0b26045f3cd43ff293382208da843dfaa6de4fc180ea916563279d\"\n"
		"\t\t}\n"
		"\t}\n"
		"\tmod \"bloodmod\" {\n"
		"\t\tname      \"Blood Mod game library\"\n"
		"\t\trepo      \"Inkub0/dude-sdk\"\n"
		"\t\trelease   \"bloodmod-v1.0.0\"\n"
		"\t\tlicense   \"GPLv3\"\n"
		"\t\tneedsData \"Blood Mod game data (ModDB) + Doom 3 - not downloadable\"\n"
		"\t\tasset \"win64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/bloodmod-v1.0.0/bloodmod-win64.dll\"\n"
		"\t\t\tsha256 \"fb2b5ec5f2bb55a4243c17824c6a5e55ab9a2339ba660d23425c813073a330b5\"\n"
		"\t\t}\n"
		"\t\tasset \"linux-x86_64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/bloodmod-v1.0.0/bloodmod-linux-x86_64.so\"\n"
		"\t\t\tsha256 \"148a699fd071b8867be5e628bd311d7053255eb5fb17facdfaf54e4cdade7592\"\n"
		"\t\t}\n"
		"\t}\n"
		"\tmod \"bloodmod_roe\" {\n"
		"\t\tname      \"Blood Mod game library (Resurrection of Evil)\"\n"
		"\t\trepo      \"Inkub0/dude-sdk\"\n"
		"\t\trelease   \"bloodmod-v1.0.0\"\n"
		"\t\tlicense   \"GPLv3\"\n"
		"\t\tneedsData \"Blood Mod game data (ModDB) + Doom 3 + Resurrection of Evil - not downloadable\"\n"
		"\t\tasset \"win64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/bloodmod-v1.0.0/bloodmod_roe-win64.dll\"\n"
		"\t\t\tsha256 \"a48bc851125ea8629c7d2157593e1b50d0b70b0c90fec905b21f4774b6b69496\"\n"
		"\t\t}\n"
		"\t\tasset \"linux-x86_64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/bloodmod-v1.0.0/bloodmod_roe-linux-x86_64.so\"\n"
		"\t\t\tsha256 \"2aefbe82f0f9fd284600e657f0f5fe8cee6f3feafa23527a1082ff72eacefa9b\"\n"
		"\t\t}\n"
		"\t}\n"
		"\tmod \"dentonmod\" {\n"
		"\t\tname      \"Denton's Enhanced Doom 3 game library\"\n"
		"\t\trepo      \"Inkub0/dude-sdk\"\n"
		"\t\trelease   \"dentonmod-v1.0.0\"\n"
		"\t\tlicense   \"GPLv3\"\n"
		"\t\tneedsData \"Denton's Enhanced Doom 3 game data (ModDB); the folder must be named dentonmod/ (lowercase) + Doom 3 - not downloadable\"\n"
		"\t\tasset \"win64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/dentonmod-v1.0.0/dentonmod-win64.dll\"\n"
		"\t\t\tsha256 \"6e54fa12ac6bcfd760b64efe658ba7969ba9727e3057d76c5f08e6e2a1fb72bf\"\n"
		"\t\t}\n"
		"\t\tasset \"linux-x86_64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/dentonmod-v1.0.0/dentonmod-linux-x86_64.so\"\n"
		"\t\t\tsha256 \"2551095044efba509ffe84b774d7a15c2b877db2552e7be60b705f1459f458d3\"\n"
		"\t\t}\n"
		"\t}\n"
		"\tmod \"desolated\" {\n"
		"\t\tname      \"Desolated - The Crying Fate game library\"\n"
		"\t\trepo      \"Inkub0/dude-sdk\"\n"
		"\t\trelease   \"desolated-v1.0.0\"\n"
		"\t\tlicense   \"GPLv3\"\n"
		"\t\tneedsData \"Desolated game data (ModDB); the folder must be named desolated/ (lowercase) + Doom 3 - not downloadable\"\n"
		"\t\tasset \"win64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/desolated-v1.0.0/desolated-win64.dll\"\n"
		"\t\t\tsha256 \"06f67bb9964ede1d950071dde21b9ef305c2b07f74825de3b99f7a793172ae5b\"\n"
		"\t\t}\n"
		"\t\tasset \"linux-x86_64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/desolated-v1.0.0/desolated-linux-x86_64.so\"\n"
		"\t\t\tsha256 \"87767c5e68818f9d19379384381a3ef771b13938e32b588a0378a866e2dbfe1f\"\n"
		"\t\t}\n"
		"\t}\n"
		"\tmod \"doom3cc\" {\n"
		"\t\tname      \"Doom3[CC] (closed captions) game library\"\n"
		"\t\trepo      \"Inkub0/dude-sdk\"\n"
		"\t\trelease   \"doom3cc-v1.0.0\"\n"
		"\t\tlicense   \"GPLv3\"\n"
		"\t\tneedsData \"Doom3[CC] game data (see dhewm/dhewm3-sdk issue 12) + Doom 3 - not downloadable\"\n"
		"\t\tasset \"win64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/doom3cc-v1.0.0/doom3cc-win64.dll\"\n"
		"\t\t\tsha256 \"597288d303f86e164e2d2504a1e46b78898ec3b79d60dc66397b42eb9198d85c\"\n"
		"\t\t}\n"
		"\t\tasset \"linux-x86_64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/doom3cc-v1.0.0/doom3cc-linux-x86_64.so\"\n"
		"\t\t\tsha256 \"1c9312769f307035e18461d0995353045c1435661bb6880e2aa7b95fb9233876\"\n"
		"\t\t}\n"
		"\t}\n"
		"\tmod \"doom3ccXP\" {\n"
		"\t\tname      \"Doom3[CC] (closed captions) game library (Resurrection of Evil)\"\n"
		"\t\trepo      \"Inkub0/dude-sdk\"\n"
		"\t\trelease   \"doom3cc-v1.0.0\"\n"
		"\t\tlicense   \"GPLv3\"\n"
		"\t\tneedsData \"Doom3[CC] game data (see dhewm/dhewm3-sdk issue 12) + Doom 3 + Resurrection of Evil - not downloadable\"\n"
		"\t\tasset \"win64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/doom3cc-v1.0.0/doom3ccXP-win64.dll\"\n"
		"\t\t\tsha256 \"73bd9554282627b1f7cf1c3496cc0aa583c661c4bd6de5885fa2ccf9078a4841\"\n"
		"\t\t}\n"
		"\t\tasset \"linux-x86_64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/doom3cc-v1.0.0/doom3ccXP-linux-x86_64.so\"\n"
		"\t\t\tsha256 \"4a1e60da150b38e3e5ce90dbd31e94acb7d87cadc0e2130b893fe2740a18f352\"\n"
		"\t\t}\n"
		"\t}\n"
		"\tmod \"eldoom\" {\n"
		"\t\tname      \"ELDOOM game library\"\n"
		"\t\trepo      \"Inkub0/dude-sdk\"\n"
		"\t\trelease   \"eldoom-v1.0.0\"\n"
		"\t\tlicense   \"GPLv3\"\n"
		"\t\tneedsData \"ELDOOM game data (ModDB) + Doom 3 - not downloadable\"\n"
		"\t\tasset \"win64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/eldoom-v1.0.0/eldoom-win64.dll\"\n"
		"\t\t\tsha256 \"9454e88b8a83b981067cbf7dbcb34215d0716bdbf3ed6c024c075ef67fb07ddd\"\n"
		"\t\t}\n"
		"\t\tasset \"linux-x86_64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/eldoom-v1.0.0/eldoom-linux-x86_64.so\"\n"
		"\t\t\tsha256 \"e5ae72989db7f97962713af5e995d7dc7a441b677b3d13bfc62caacc85d0a114\"\n"
		"\t\t}\n"
		"\t}\n"
		"\tmod \"eoc\" {\n"
		"\t\tname      \"HeXen: Edge of Chaos game library\"\n"
		"\t\trepo      \"Inkub0/dude-sdk\"\n"
		"\t\trelease   \"eoc-v1.0.0\"\n"
		"\t\tlicense   \"GPLv3\"\n"
		"\t\tneedsData \"HeXen: Edge of Chaos game data, dhewm3 edition (ModDB) + Doom 3 - not downloadable\"\n"
		"\t\tasset \"win64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/eoc-v1.0.0/eoc-win64.dll\"\n"
		"\t\t\tsha256 \"6cc67fd04b7e9c50a9e214ff634766130ebf082070415ab80a108456a83b99a1\"\n"
		"\t\t}\n"
		"\t\tasset \"linux-x86_64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/eoc-v1.0.0/eoc-linux-x86_64.so\"\n"
		"\t\t\tsha256 \"b3d5e3d5de5b4ce612b664f61db2e0c2aa7fe8c0b272127dc25711e49cde1350\"\n"
		"\t\t}\n"
		"\t}\n"
		"\tmod \"fitz\" {\n"
		"\t\tname      \"Fitz Packerton game library\"\n"
		"\t\trepo      \"Inkub0/dude-sdk\"\n"
		"\t\trelease   \"fitz-v1.0.0\"\n"
		"\t\tlicense   \"GPLv3\"\n"
		"\t\tneedsData \"Fitz Packerton game data (teddydief.itch.io/fitz) - not downloadable\"\n"
		"\t\tasset \"win64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/fitz-v1.0.0/fitz-win64.dll\"\n"
		"\t\t\tsha256 \"b3f96177b69633d27e43638e521dbc6323c315e1011a1f7b0b0b741d1c18675c\"\n"
		"\t\t}\n"
		"\t\tasset \"linux-x86_64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/fitz-v1.0.0/fitz-linux-x86_64.so\"\n"
		"\t\t\tsha256 \"debf4b2f5013c5c5b6d9addcae48fe229812af81a909340cae6ce96679b6f640\"\n"
		"\t\t}\n"
		"\t}\n"
		"\tmod \"grimm\" {\n"
		"\t\tname      \"Grimm: Quest for the Gatherer's Key game library\"\n"
		"\t\trepo      \"Inkub0/dude-sdk\"\n"
		"\t\trelease   \"grimm-v1.0.0\"\n"
		"\t\tlicense   \"GPLv3\"\n"
		"\t\tneedsData \"Grimm game data (ModDB) + Doom 3 - not downloadable\"\n"
		"\t\tasset \"win64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/grimm-v1.0.0/grimm-win64.dll\"\n"
		"\t\t\tsha256 \"b9a2d55dec8bf6fd28b2ed83342f492f2d85a8bb37c65afffe6da5b8ad7c5d7b\"\n"
		"\t\t}\n"
		"\t\tasset \"linux-x86_64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/grimm-v1.0.0/grimm-linux-x86_64.so\"\n"
		"\t\t\tsha256 \"5bb4c46ecee403d329ebd204b12462870e36a51303a5ad437528f90f2db85149\"\n"
		"\t\t}\n"
		"\t}\n"
		"\tmod \"hardcorps\" {\n"
		"\t\tname      \"Hard Corps game library\"\n"
		"\t\trepo      \"Inkub0/dude-sdk\"\n"
		"\t\trelease   \"hardcorps-v1.0.0\"\n"
		"\t\tlicense   \"GPLv3\"\n"
		"\t\tneedsData \"Hard Corps game data (ModDB) + Doom 3 - not downloadable\"\n"
		"\t\tasset \"win64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/hardcorps-v1.0.0/hardcorps-win64.dll\"\n"
		"\t\t\tsha256 \"de54d1d94d0be422cc51e8b98d8bf4af5e276d8d2ff6b8536ff9a64dd80e8f7a\"\n"
		"\t\t}\n"
		"\t\tasset \"linux-x86_64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/hardcorps-v1.0.0/hardcorps-linux-x86_64.so\"\n"
		"\t\t\tsha256 \"0054035af90651cb82a736892b90fcdd0a4ab4343e9ca5a13f626041c27bed09\"\n"
		"\t\t}\n"
		"\t}\n"
		"\tmod \"librecoop\" {\n"
		"\t\tname      \"LibreCoop game library\"\n"
		"\t\trepo      \"Inkub0/dude-sdk\"\n"
		"\t\trelease   \"librecoop-v1.0.0\"\n"
		"\t\tlicense   \"GPLv3\"\n"
		"\t\tneedsData \"LibreCoop game data (ModDB) + Doom 3 - not downloadable\"\n"
		"\t\tasset \"win64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/librecoop-v1.0.0/librecoop-win64.dll\"\n"
		"\t\t\tsha256 \"5742ec39bbba0fa72f71eacc228821a231b2aa35ed752d169d85ef7e0e43ae48\"\n"
		"\t\t}\n"
		"\t\tasset \"linux-x86_64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/librecoop-v1.0.0/librecoop-linux-x86_64.so\"\n"
		"\t\t\tsha256 \"ecacf1bb1af1ddfc685e03f9a942405a4e30972d6eae3eda8dca6e79e8f1f6de\"\n"
		"\t\t}\n"
		"\t}\n"
		"\tmod \"librecoopd3xp\" {\n"
		"\t\tname      \"LibreCoop game library (Resurrection of Evil)\"\n"
		"\t\trepo      \"Inkub0/dude-sdk\"\n"
		"\t\trelease   \"librecoop-v1.0.0\"\n"
		"\t\tlicense   \"GPLv3\"\n"
		"\t\tneedsData \"LibreCoop game data (ModDB) + Doom 3 + Resurrection of Evil - not downloadable\"\n"
		"\t\tasset \"win64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/librecoop-v1.0.0/librecoopd3xp-win64.dll\"\n"
		"\t\t\tsha256 \"d9997e8430b8869a2181f638b220b5e81ed38f3dd5024577c42553a44042234c\"\n"
		"\t\t}\n"
		"\t\tasset \"linux-x86_64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/librecoop-v1.0.0/librecoopd3xp-linux-x86_64.so\"\n"
		"\t\t\tsha256 \"444e78ef914306b1f1d5fd2d75ce0af9b606290f319c18b4d1d1a3d06f84e8ef\"\n"
		"\t\t}\n"
		"\t}\n"
		"\tmod \"perfected\" {\n"
		"\t\tname      \"Perfected Doom 3 game library\"\n"
		"\t\trepo      \"Inkub0/dude-sdk\"\n"
		"\t\trelease   \"perfected-v1.0.0\"\n"
		"\t\tlicense   \"GPLv3\"\n"
		"\t\tneedsData \"Perfected Doom 3 game data (ModDB); folders must be named perfected/ and perfected_roe/ (lowercase) + Doom 3 - not downloadable\"\n"
		"\t\tasset \"win64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/perfected-v1.0.0/perfected-win64.dll\"\n"
		"\t\t\tsha256 \"737409e5320537fbf59591ca0b532ac4957d5f6cfd9c27ea517d69c4db2703b1\"\n"
		"\t\t}\n"
		"\t\tasset \"linux-x86_64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/perfected-v1.0.0/perfected-linux-x86_64.so\"\n"
		"\t\t\tsha256 \"076de8612759c7f562c72d7e29432e572068c92d23a29eb9d2651898261bad5d\"\n"
		"\t\t}\n"
		"\t}\n"
		"\tmod \"perfected_roe\" {\n"
		"\t\tname      \"Perfected Doom 3 game library (Resurrection of Evil)\"\n"
		"\t\trepo      \"Inkub0/dude-sdk\"\n"
		"\t\trelease   \"perfected-v1.0.0\"\n"
		"\t\tlicense   \"GPLv3\"\n"
		"\t\tneedsData \"Perfected Doom 3 game data (ModDB); folders must be named perfected/ and perfected_roe/ (lowercase) + Doom 3 + Resurrection of Evil - not downloadable\"\n"
		"\t\tasset \"win64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/perfected-v1.0.0/perfected_roe-win64.dll\"\n"
		"\t\t\tsha256 \"658c95b1b0b0fe6ab89b418c3bfd4289cc16dda177f8cfb9e8e7a42c41d39bfc\"\n"
		"\t\t}\n"
		"\t\tasset \"linux-x86_64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/perfected-v1.0.0/perfected_roe-linux-x86_64.so\"\n"
		"\t\t\tsha256 \"874f56806c1a70cc7c4dcc4554926a962ddefb065b4c08b522fbef7a3a3b7002\"\n"
		"\t\t}\n"
		"\t}\n"
		"\tmod \"realgibs\" {\n"
		"\t\tname      \"Real Gibs game library\"\n"
		"\t\trepo      \"Inkub0/dude-sdk\"\n"
		"\t\trelease   \"realgibs-v1.0.0\"\n"
		"\t\tlicense   \"GPLv3\"\n"
		"\t\tneedsData \"Real Gibs game data (ModDB) + Doom 3 - not downloadable\"\n"
		"\t\tasset \"win64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/realgibs-v1.0.0/realgibs-win64.dll\"\n"
		"\t\t\tsha256 \"010fd1fbea3d1e156964fb6b42650d1615ded4c0502055c5d63e885020fef67c\"\n"
		"\t\t}\n"
		"\t\tasset \"linux-x86_64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/realgibs-v1.0.0/realgibs-linux-x86_64.so\"\n"
		"\t\t\tsha256 \"b41990b2be6931cea5d8c02ef31e759c46bd9be578d0014069b234df5fa53f66\"\n"
		"\t\t}\n"
		"\t}\n"
		"\tmod \"rivensin\" {\n"
		"\t\tname      \"Scarlet Rivensin: The Ruiner game library\"\n"
		"\t\trepo      \"Inkub0/dude-sdk\"\n"
		"\t\trelease   \"rivensin-v1.0.0\"\n"
		"\t\tlicense   \"GPLv3\"\n"
		"\t\tneedsData \"Scarlet Rivensin game data (ModDB, its dhewm3/rivensin folder) + Doom 3 - not downloadable\"\n"
		"\t\tasset \"win64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/rivensin-v1.0.0/rivensin-win64.dll\"\n"
		"\t\t\tsha256 \"47ef7cfb2a1d8519e654f3c3e368ce88c13f405f6f97558a0916ed0fb0a478a0\"\n"
		"\t\t}\n"
		"\t\tasset \"linux-x86_64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/rivensin-v1.0.0/rivensin-linux-x86_64.so\"\n"
		"\t\t\tsha256 \"36a04b81fb1c8a70bf8c821134615459f7db28793987ef4853a227f4b96bef75\"\n"
		"\t\t}\n"
		"\t}\n"
		"\tmod \"sikkmod\" {\n"
		"\t\tname      \"Sikkmod game library\"\n"
		"\t\trepo      \"Inkub0/dude-sdk\"\n"
		"\t\trelease   \"sikkmod-v1.0.0\"\n"
		"\t\tlicense   \"GPLv3\"\n"
		"\t\tneedsData \"Sikkmod game data (ModDB: v1.2, and v1.1 for RoE) + Doom 3 - not downloadable\"\n"
		"\t\tasset \"win64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/sikkmod-v1.0.0/sikkmod-win64.dll\"\n"
		"\t\t\tsha256 \"fac92dfbd8287f85cbf0d9f53e180cee009b033d10f5a83757fb84fc22b6c353\"\n"
		"\t\t}\n"
		"\t\tasset \"linux-x86_64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/sikkmod-v1.0.0/sikkmod-linux-x86_64.so\"\n"
		"\t\t\tsha256 \"f104ce5f7acc6d920edccf84099b6ad3bfa302323bd3bafd2c9e97912205a7df\"\n"
		"\t\t}\n"
		"\t}\n"
		"\tmod \"sikkmodd3xp\" {\n"
		"\t\tname      \"Sikkmod game library (Resurrection of Evil)\"\n"
		"\t\trepo      \"Inkub0/dude-sdk\"\n"
		"\t\trelease   \"sikkmod-v1.0.0\"\n"
		"\t\tlicense   \"GPLv3\"\n"
		"\t\tneedsData \"Sikkmod game data (ModDB: v1.2, and v1.1 for RoE) + Doom 3 + Resurrection of Evil - not downloadable\"\n"
		"\t\tasset \"win64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/sikkmod-v1.0.0/sikkmodd3xp-win64.dll\"\n"
		"\t\t\tsha256 \"31246f8e85270261505c65e4c2d12768ac216b21583b2503d6c12d18e7cc7d97\"\n"
		"\t\t}\n"
		"\t\tasset \"linux-x86_64\" {\n"
		"\t\t\turl    \"https://github.com/Inkub0/dude-sdk/releases/download/sikkmod-v1.0.0/sikkmodd3xp-linux-x86_64.so\"\n"
		"\t\t\tsha256 \"91ff0e03a98a2eeb6a964c2e5bb60ee9dcdb5e2f914cae137cbe0d5eb72e18ae\"\n"
		"\t\t}\n"
		"\t}\n"
		"}\n";
}
