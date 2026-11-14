/*
===========================================================================
	Mod library auto-download: user-consent gate and the enable cvar.

	The download itself (Sys_DownloadURL), the SHA-256 check and the loader
	hook live elsewhere; this file owns the policy cvar and the modal consent
	prompt. See docs/mod-auto-download.md.
===========================================================================
*/

#include "sys/sys_sdl.h"	// before idlib, whose Str.h redefines the C string functions

#include "sys/platform.h"
#include "idlib/Str.h"
#include "idlib/hashing/SHA256.h"
#include "framework/Common.h"
#include "framework/FileSystem.h"	// MAX_OSPATH

#include "framework/ModDownload.h"

#include "sys/sys_public.h"

#include <stdio.h>

idCVar com_allowModDownload( "com_allowModDownload", "1", CVAR_SYSTEM | CVAR_INTEGER | CVAR_ARCHIVE,
	"allow DUDE to download a missing mod game library from its pinned release: "
	"0 = never (report the missing library only), 1 = ask, then download on consent" );

static idCVar com_modRegistryUrl( "com_modRegistryUrl",
	"https://raw.githubusercontent.com/Inkub0/dude/master/mod-registry.cfg",
	CVAR_SYSTEM,
	"HTTPS URL of the mod auto-download registry; empty uses only the engine's baked-in copy" );

bool ModDownload_Confirm( const modRegistryMod_t &mod, const modRegistryAsset_t &asset ) {
	// no display (e.g. dedicated server): cannot ask, so treat as declined
	if ( !SDL_WasInit( SDL_INIT_VIDEO ) ) {
		return false;
	}

	const char *licence = mod.license.IsEmpty() ? "see the repository" : mod.license.c_str();
	idStr dataNote;
	if ( !mod.needsData.IsEmpty() ) {
		dataNote = va( "You still need the mod's own game data:\n%s\n\n", mod.needsData.c_str() );
	}

	const idStr caption = va( "Download %s?", mod.name.c_str() );
	const idStr text = va(
		"The game library for this mod is not installed.\n\n"
		"Source: github.com/%s (%s)\n"
		"File: %s\n"
		"Licence: %s\n\n"
		"%s"
		"Download it now from the official release? It will be verified against a known checksum before use.",
		mod.repo.c_str(), mod.release.c_str(),
		asset.url.c_str(),
		licence,
		dataNote.c_str() );

	enum { CHOICE_CANCEL, CHOICE_DOWNLOAD };
	SDL_MessageBoxButtonData buttons[2];
	const SDL_MessageBoxButtonData dl = { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, CHOICE_DOWNLOAD, "Download" };
	const SDL_MessageBoxButtonData cancel = { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, CHOICE_CANCEL, "Cancel" };
	buttons[0] = dl;
	buttons[1] = cancel;

	const SDL_MessageBoxData box = { SDL_MESSAGEBOX_INFORMATION, NULL, caption.c_str(), text.c_str(), 2, buttons, NULL };
	int choice = CHOICE_CANCEL;
#if SDL_VERSION_ATLEAST( 3, 0, 0 )
	if ( !SDL_ShowMessageBox( &box, &choice ) ) {
		return false;
	}
#else
	if ( SDL_ShowMessageBox( &box, &choice ) != 0 ) {
		return false;
	}
#endif
	return choice == CHOICE_DOWNLOAD;
}

/*
===============================================================================
	orchestration
===============================================================================
*/

// SHA-256 a file on disk, chunked, into lowercase hex. False on read error.
static bool MD_HashFile( const char *path, idStr &hexOut ) {
	FILE *f = fopen( path, "rb" );
	if ( f == NULL ) {
		return false;
	}
	sha256Context_t ctx;
	SHA256_Init( &ctx );
	unsigned char buf[65536];
	size_t n;
	while ( ( n = fread( buf, 1, sizeof( buf ), f ) ) > 0 ) {
		SHA256_Update( &ctx, buf, n );
	}
	const bool ok = ( ferror( f ) == 0 );
	fclose( f );
	if ( !ok ) {
		return false;
	}
	unsigned char digest[32];
	char hex[65];
	SHA256_Final( &ctx, digest );
	SHA256_DigestToHex( digest, hex );
	hexOut = hex;
	return true;
}

static bool MD_ReadTextFile( const char *path, idStr &out ) {
	FILE *f = fopen( path, "rb" );
	if ( f == NULL ) {
		return false;
	}
	out.Clear();
	char buf[8192];
	size_t n;
	while ( ( n = fread( buf, 1, sizeof( buf ) - 1, f ) ) > 0 ) {
		buf[n] = '\0';
		out += buf;
	}
	const bool ok = ( ferror( f ) == 0 );
	fclose( f );
	return ok;
}

// Fill reg from the hosted registry if it can be fetched and parsed, otherwise
// from the engine's baked-in default.
static void MD_ResolveRegistry( idModRegistry &reg ) {
	const char *url = com_modRegistryUrl.GetString();
	if ( url != NULL && url[0] != '\0' ) {
		const idStr tmp = idStr( cvarSystem->GetCVarString( "fs_savepath" ) ) + "/mod-registry.download";
		if ( Sys_DownloadURL( url, tmp.c_str() ) ) {
			idStr text, err;
			if ( MD_ReadTextFile( tmp.c_str(), text ) && reg.ParseString( text.c_str(), "mod-registry(remote)", &err ) ) {
				remove( tmp.c_str() );
				return;
			}
			common->Printf( "mod registry: hosted copy unusable, using the built-in list\n" );
			remove( tmp.c_str() );
		}
	}
	idStr err;
	if ( !reg.ParseString( idModRegistry::DefaultRegistryText(), "mod-registry(baked)", &err ) ) {
		common->Warning( "mod registry: built-in list failed to parse: %s", err.c_str() );
		reg.Clear();
	}
}

bool ModDownload_EnsureLibrary( const char *fsGame ) {
	if ( fsGame == NULL || fsGame[0] == '\0' ) {
		return false;
	}
	if ( com_allowModDownload.GetInteger() == 0 ) {
		return false;	// feature disabled by the user
	}

	idModRegistry reg;
	MD_ResolveRegistry( reg );

	const modRegistryMod_t *mod = reg.FindMod( fsGame );
	if ( mod == NULL ) {
		return false;	// not a known downloadable mod; leave it to the caller
	}

	const char *platform = idModRegistry::CurrentPlatformKey();
	const modRegistryAsset_t *asset = mod->FindAsset( platform );
	if ( asset == NULL ) {
		common->Warning( "%s has no downloadable library for this platform (%s); get it from github.com/%s",
			mod->name.c_str(), platform, mod->repo.c_str() );
		return false;
	}

	if ( !ModDownload_Confirm( *mod, *asset ) ) {
		common->Printf( "%s library download declined\n", mod->name.c_str() );
		return false;
	}

	// place it where the loader looks: <fs_savepath>/<fsGame>/<fsGame><libsuffix>
	const idStr dir = idStr( cvarSystem->GetCVarString( "fs_savepath" ) ) + "/" + fsGame;
	Sys_Mkdir( dir.c_str() );

	char libName[MAX_OSPATH];
	sys->DLL_GetFileName( fsGame, libName, sizeof( libName ) );
	const idStr finalPath = dir + "/" + libName;
	const idStr partPath = finalPath + ".part";

	common->Printf( "downloading %s from %s ...\n", mod->name.c_str(), asset->url.c_str() );
	if ( !Sys_DownloadURL( asset->url.c_str(), partPath.c_str() ) ) {
		common->Warning( "download failed; install %s manually from github.com/%s", mod->name.c_str(), mod->repo.c_str() );
		return false;
	}

	idStr gotHash;
	if ( !MD_HashFile( partPath.c_str(), gotHash ) || gotHash.Icmp( asset->sha256.c_str() ) != 0 ) {
		remove( partPath.c_str() );
		common->Warning( "the downloaded %s library failed checksum verification and was discarded", mod->name.c_str() );
		return false;
	}

	remove( finalPath.c_str() );
	if ( rename( partPath.c_str(), finalPath.c_str() ) != 0 ) {
		remove( partPath.c_str() );
		common->Warning( "could not move the downloaded library into place at %s", finalPath.c_str() );
		return false;
	}

	// a licence/provenance note beside the library
	const idStr notePath = dir + "/" + "LICENSE-" + fsGame + ".txt";
	FILE *nf = fopen( notePath.c_str(), "wb" );
	if ( nf != NULL ) {
		fprintf( nf, "%s\nLicence: %s\nSource: github.com/%s (%s)\n"
			"Downloaded by DUDE and verified against a pinned SHA-256.\n",
			mod->name.c_str(),
			mod->license.IsEmpty() ? "see the repository" : mod->license.c_str(),
			mod->repo.c_str(), mod->release.c_str() );
		fclose( nf );
	}

	common->Printf( "%s library installed and verified at %s\n", mod->name.c_str(), finalPath.c_str() );
	return true;
}
