#ifndef __MODDOWNLOAD_H__
#define __MODDOWNLOAD_H__

#include "framework/CVarSystem.h"
#include "framework/ModRegistry.h"

/*
===============================================================================

	Mod library auto-download.

	When fs_game names a mod listed in the registry whose game library is not
	installed, DUDE can fetch the library from its pinned GitHub release,
	verify it against the registry's SHA-256, and place it where the loader
	looks. This header exposes the user-consent gate; the orchestration and the
	game-library loader hook are added in a later step.
	See docs/mod-auto-download.md.

===============================================================================
*/

// 0 = never download (only report the missing library), 1 = ask the user and
// download on consent (default). Archived.
extern idCVar com_allowModDownload;

// Ask the user whether to download the given mod library. Shows a modal dialog
// naming the mod, its source release and licence, and the game data the user
// must still supply. Returns true only on an explicit yes; false if declined or
// if no display is available to ask on.
bool ModDownload_Confirm( const modRegistryMod_t &mod, const modRegistryAsset_t &asset );

// If fsGame is a registry mod whose library is missing and the user consents,
// download the library from its pinned release, verify it against the pinned
// SHA-256, and place it in fs_savepath/<fsGame>/ where the loader looks.
// Returns true only when a verified library is now present. Safe (returns
// false) for unknown mods, when disabled, or when there is no display to ask on.
bool ModDownload_EnsureLibrary( const char *fsGame );

#endif /* !__MODDOWNLOAD_H__ */
