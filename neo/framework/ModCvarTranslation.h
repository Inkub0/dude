/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: mod compatibility shim — cvar name translation + small per-mod tweaks.
//
// Some mods (e.g. tfphobos) ship stock-Doom3-era menus that reference cvars DUDE
// renamed or removed (r_bloom, in_freeLook, etc.). Rather than fork every mod's
// GUI, this table redirects cvar reads/writes at the CVarSystem layer so the
// mod's UI transparently drives DUDE's equivalents.
//
// The table is loaded from base/mods-cvar-translation.cfg — editable by users.

#ifndef __MOD_CVAR_TRANSLATION_H__
#define __MOD_CVAR_TRANSLATION_H__

namespace ModCompat {

// Load base/mods-cvar-translation.cfg into the in-memory table. Safe to call
// again to reload after the user edits the cfg (also exposed as the
// `mods_translation_reload` console command).
void Init( void );
void Shutdown( void );

// Return the DUDE-side cvar name for the currently active mod, or `name`
// itself if there's no alias (or if fs_modCompatShim is off). Never returns
// NULL; the returned pointer is stable until the next Init().
const char *ResolveAlias( const char *name );

// True if the active mod wants r_mode auto-promoted to -1 when r_customWidth/
// Height differ from r_mode's preset. Consumed by the menu "video restart"
// handler.
bool AutoCustomRes( void );

// True when a foreign mod is active (fs_game set to something other than the
// stock game dirs base/d3xp) AND the shim is enabled. Consumers gate any
// mod-only behavior on this so stock DOOM3/RoE runs untouched.
bool IsForeignMod( void );

// Non-empty active mod name (fs_game) when a foreign mod is running and the
// shim is enabled — empty string otherwise. Used to prefix engine warnings
// with "-<mod>-" so terminal output attributes issues to the mod that caused
// them.
const char *WarningTag( void );

} // namespace ModCompat

#endif // __MOD_CVAR_TRANSLATION_H__
