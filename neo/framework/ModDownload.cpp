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
#include "framework/Common.h"

#include "framework/ModDownload.h"

#include "sys/sys_public.h"

idCVar com_allowModDownload( "com_allowModDownload", "1", CVAR_SYSTEM | CVAR_INTEGER | CVAR_ARCHIVE,
	"allow DUDE to download a missing mod game library from its pinned release: "
	"0 = never (report the missing library only), 1 = ask, then download on consent" );

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
