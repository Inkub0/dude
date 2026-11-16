#ifndef __MODREGISTRY_H__
#define __MODREGISTRY_H__

#include "idlib/Str.h"
#include "idlib/containers/List.h"

/*
===============================================================================

	Mod library auto-download registry.

	Maps a mod's fs_game name to the per-platform GitHub release asset that
	provides its game library, each pinned by a SHA-256 for integrity. Parsed
	from a small idLexer text file (hosted in the main repo, with a baked-in
	default) and used by the game-library loader to fetch a missing mod library.
	See docs/mod-auto-download.md.

===============================================================================
*/

struct modRegistryAsset_t {
	idStr		platform;	// "win64", "linux-x86_64", ...
	idStr		url;		// https download URL
	idStr		sha256;		// 64 lowercase hex chars
};

struct modRegistryMod_t {
	idStr						key;		// fs_game, e.g. "tfphobos"
	idStr						name;		// human-readable
	idStr						repo;		// "owner/name"
	idStr						release;	// pinned release tag, e.g. "v1.0.0"
	idStr						license;	// e.g. "GPLv3"
	idStr						needsData;	// note about the game data the user must supply
	idList<modRegistryAsset_t>	assets;

	const modRegistryAsset_t *	FindAsset( const char *platform ) const;
};

class idModRegistry {
public:
								idModRegistry( void ) : version( 0 ) {}

	void						Clear( void );

	// Parse registry text in idLexer syntax (see DefaultRegistryText for the
	// grammar). Replaces any current contents. Returns false on a structural
	// error, with a short reason in errMsg (if non-NULL). Does not log or touch
	// the filesystem, so it is safe to call before the engine is up.
	bool						ParseString( const char *text, const char *sourceName, idStr *errMsg = NULL );

	int							GetVersion( void ) const { return version; }
	int							NumMods( void ) const { return mods.Num(); }

	const modRegistryMod_t *	FindMod( const char *fsGame ) const;

	// Adds every mod of other that this registry does not list (by fs_game);
	// mods this registry already has are kept as they are. Returns how many
	// were added. Lets a hosted registry win per mod while the baked-in one
	// fills the gaps, so an older hosted copy cannot hide a mod this build
	// knows.
	int							AddMissing( const idModRegistry &other );

	// Asset platform key for the running build, e.g. "win64" / "linux-x86_64".
	static const char *			CurrentPlatformKey( void );

	// Baked-in registry, used as the offline fallback when the hosted copy
	// cannot be fetched or does not parse.
	static const char *			DefaultRegistryText( void );

private:
	int							version;
	idList<modRegistryMod_t>	mods;
};

#endif /* !__MODREGISTRY_H__ */
