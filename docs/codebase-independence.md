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

### 1. Stock-menu settings entry point (no mod)  **[TODO]**
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

### 2. Ship DUDE's own widescreen GUIs (drop the CstDoom3 pak dependency)  **[TODO]**
The nice widescreen menu/HUD/PDA come from `zWideGuis_D3.pk4`. The **anchor system it
uses is already in the engine** (`cstAnchor` / `CST_ANCHOR_*` / `scaleto43` handled in
`ui/Window.cpp`, `ui/SimpleWindow.cpp`, `ui/DeviceContext.cpp`, `ui/ListWindow.cpp`,
`ui/UserInterface.cpp`; see also [GUIs.md](GUIs.md)). So the capability is native — only
the *authored* `.gui` files are third-party.
- **Blocker:** those `.gui` files are derivative of id's original menus (no clear
  license). A shippable solution needs DUDE-authored / clean overrides, not the
  community pak. Likely partial: only the elements we actually want to own.
- Engine already exposes `r_scaleMenusTo43` for the non-widescreen fallback.

### 3. Own the config / save identity  **[TODO]**
The write dirs are half-renamed:
- Linux save: `~/.local/share/dude` ✅ (`sys/linux/main.cpp:94`)
- Linux config: `~/.config/**dhewm3**` ❌ (`sys/linux/main.cpp:235,237`)
- Windows: `Documents/My Games/**dhewm3**`; macOS: `Application Support/**dhewm3**`
- **Task:** unify on a `dude` dir across platforms, with a one-time migration / read
  fallback so existing configs and keys aren't orphaned. (Related gotcha:
  savepath > basepath override, documented separately.)

### 4. Rename the settings command + F10 identity  **[TODO]**
`dhewm3Settings` console command (registered in `framework/Common.cpp`, auto-bound in
`sys/sys_imgui.cpp`) still carries the old name. Rename to `dudeSettings` and keep a
`dhewm3Settings` **alias** so existing binds/configs keep working. (Low effort; do it
with item 3.)

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
