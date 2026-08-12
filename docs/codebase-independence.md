# Phase: Codebase Independence

**Goal:** let a clean DUDE build stand on its own — no reliance on third-party
community content, and a consistent DUDE identity — while keeping the deliberate
upstream *attributions* intact.

**Why:** several things that make DUDE feel finished currently lean on outside
content or still carry the `dhewm3` name. The trigger was the main-menu settings
button: it only exists inside a community widescreen mod (`zWideGuis_D3.pk4`, a
CstDoom3/"WideGuis" pak), so a mod-free build has no on-screen way into the F10
menu. This phase inventories those seams and closes the ones worth closing.

Scope note: this is *independence from third-party content and stale identity*, not
from normal libraries (SDL2, OpenAL, libcurl…) — those stay. And Doom 3 game data
(id assets) is explicitly **out of scope** (see the last item).

---

### 1. Stock-menu settings entry point (no mod)  **[interim DONE]**
*2026-08-05:* the DUDE menu (with the settings button) now always loads for any
repo/portable install: the canonical working `base/guis/mainmenu.gui` lives loose
in the repo (untracked, derivative — see item 2), the engine mirrors it into
`fs_savepath` by **content** (not mtime) at startup, and `fs_basepath` now falls
back to the executable's parent dir / cwd when the configured default is missing
(`neo/sys/linux/main.cpp`), so `build/dude` finds the repo data from any cwd.
F10 → settings already works engine-side: `sys_imgui.cpp` auto-binds `K_F10` to the
`dhewm3Settings` command on startup, independent of any GUI. But the *visible*
"DUDE Settings" button lives only in the community mod's `guis/mainmenu.gui`
(the image→text edit we made). A mod-free build shows no button.
- **Options:** (a) engine-injected button drawn over the main menu; (b) a small
  loose `guis/mainmenu.gui` override shipped by DUDE; (c) document F10 and accept
  "no button without a GUI mod".
- Starting point: [main-menu-dude-button.md](main-menu-dude-button.md).
- Fidelity: opt-in / non-destructive; must not alter the stock menu when a GUI mod
  is present.

### 2. Ship DUDE's own widescreen GUIs (drop the CstDoom3 pak dependency)  **[in progress]**
*2026-08-05:* first clean piece landed: `base/guis/_dude_anchor.pd` (DUDE-authored
anchor-constant include, values fixed by the engine enum) is git-tracked, and the
working menu now includes it instead of the pak's `_cst_anchor.pd`. The menu no
longer *loads* from `zWideGuis_D3.pk4` (loose canonical copy + startup mirror win
everywhere), but its *text* is still derivative, so it stays untracked; the pak
still supplies hud/pda/intro/cursor/restart. Clean re-authoring of the menu
screens is the remaining (multi-session, visually-verified) work.
The nice widescreen menu/HUD/PDA come from `zWideGuis_D3.pk4`. The **anchor system it
uses is already in the engine** (`cstAnchor` / `CST_ANCHOR_*` / `scaleto43` handled in
`ui/Window.cpp`, `ui/SimpleWindow.cpp`, `ui/DeviceContext.cpp`, `ui/ListWindow.cpp`,
`ui/UserInterface.cpp`; see also [GUIs.md](GUIs.md)). So the capability is native — only
the *authored* `.gui` files are third-party.
- **Blocker:** those `.gui` files are derivative of id's original menus (no clear
  license). A shippable solution needs DUDE-authored / clean overrides, not the
  community pak. Likely partial: only the elements we actually want to own.
- Engine already exposes `r_scaleMenusTo43` for the non-widescreen fallback.

### 3. Own the config / save identity  **[DONE]**
All write dirs now use `dude`, with a best-effort one-time migration that clones
the legacy `dhewm3` dir on first launch (only if the new dir doesn't exist yet),
so upgrading users keep configs/keys/saves:
- Linux save: `~/.local/share/dude` ✅ (`sys/linux/main.cpp`)
- Linux config: `~/.config/dude` ✅ (`sys/linux/main.cpp` `PATH_CONFIG`;
  `DUDE_MigrateLegacyConfig` recursively copies `~/.config/dhewm3`). **Verified**
  in-engine: identical tree cloned.
- macOS: `Application Support/dude` ✅ (`sys/osx/DOOMController.mm`; NSFileManager
  `copyItemAtPath` from the legacy dir). *Unbuilt/untested on this machine.*
- Windows: `Documents/My Games/dude` ✅ (`sys/win32/win_main.cpp` `Win_GetHomeDir`;
  `SHFileOperationA` FO_COPY of the legacy tree). *Unbuilt/untested on this machine.*
- Migration is guarded on "new dir absent AND old dir present" so it fires exactly
  once and never clobbers a live `dude` dir. Log filenames still say `dhewm3log.txt`
  (cosmetic, inside the now-`dude` dir — out of scope).
- Related gotcha: savepath > basepath override, documented separately.

### 4. Rename the settings command + F10 identity  **[DONE]**
The console command is now `dudeSettings` (`framework/Common.cpp`), with the legacy
`dhewm3Settings` kept as an **alias** (same handler) so existing binds/configs keep
working. The F10 auto-bind (`sys/sys_imgui.cpp`) now sets `dudeSettings` on fresh
installs but treats *either* name as already-bound, so a legacy F10 bind doesn't
warn or get double-bound. Menu keybind label (`framework/Dhewm3SettingsMenu.cpp`)
points at `dudeSettings`. **Verified**: both command names resolve in-engine (a
bogus name errors "Unknown command", these two don't).

### 5. Desktop / packaging IDs  **[deferred]**
Reverse-DNS `org.dhewm3.*` (`dist/linux/**`) and external refs `dhewm3-libs` /
`dhewm3-sdk` are intentionally left as upstream references for now. Revisit only if
DUDE takes on its own packaging/app identity.

### 6. Game-asset independence (id data)  **[out of scope / long-term]**
The engine still requires Doom 3 game data to run. Full libre-asset independence
(Freedoom-style) is a large separate project; noted for completeness, not planned.

---

**Kept deliberately (not "dependencies to break"):** the `fork of dhewm3`
attributions, upstream Changelog/README, and normal third-party libraries.
