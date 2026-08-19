# Release checklist

How a DUDE binary release is built and what it MUST contain. First done for 0.9,
overhauled for 0.9.1 (2026-08-19: portable Linux build + embedded shaders); repeat
for every release.

## 1. Version

- Bump `ENGINE_VERSION` in `neo/framework/Licensee.h` ("DUDE X.Y" — window title +
  console). Leave `BUILD_NUMBER` (id's 1305) alone: it's compatibility-relevant.
- Commit, merge, push before tagging so the release tag points at the bump.

## 2. Build both platforms

```
./build-linux-release.sh   # PORTABLE Linux -> build-linux-release/{dude,base.so,d3xp.so}
./build-win.sh             # Windows       -> dist-win/dude.exe + runtime DLLs
```

`build-linux-release.sh` builds in a rootless Ubuntu 22.04 bwrap sandbox
(self-provisions `deps-linux-release/` on first run): glibc floor 2.34,
libstdc++/libgcc/shaderc static, RUNPATH `$ORIGIN/lib`. **Never ship `build/dude`
(the dev build) in a release** — it needs the host's rolling-release glibc.

Sanity — BOTH binaries, EVERY release (a stale sandbox build once shipped without
the embedded shaders → black screen on GL3/VK):

```
strings <binary> | grep "DUDE 0\."          # must print the new version
strings <binary> | grep -c "spv/interaction" # must be >0: embedded shader table present
```

## 3. Package — every archive ships ALL of this

Staging layout (`dist/release-X.Y/`), one folder per platform, then zip/tar it:

| File | Windows zip | Linux tar.gz |
|---|---|---|
| `dude.exe` / `dude` | ✓ | ✓ |
| `base.dll`, `d3xp.dll` / `base.so`, `d3xp.so` | ✓ | ✓ |
| runtime DLLs (SDL2, OpenAL32, libstdc++, libgcc, libwinpthread, libssp) | ✓ | — (distro packages) |
| `lib/libvulkan.so.1` — CURRENT loader from the sandbox's Vulkan-Loader build (≥ the engine's VK baseline; an old loader here hard-fails VK init) | — | ✓ |
| **`base/zz_dude_menu.pk4`** — see below | ✓ | ✓ |
| `base/gamepad.cfg`, `base/gamepad-d3xp.cfg`, `base/mods-cvar-translation.cfg` | ✓ | ✓ |
| `base/pbr/pbr_materials.cfg`, `base/pbr/pbr_overrides.cfg` (else PBR classifies nothing) | ✓ | ✓ |
| `COPYING.txt`, `README.md` | ✓ | ✓ |

Shaders (GLSL + SPIR-V) are **embedded in the binary** since 0.9.1
(`shaders/embed_shaders.py`, loaders fall back VFS → dev tree → embedded) — no
shader pk4 exists and none must be added.

**Never include in the platform archives:** game pk4s (`pak000`–`pak008`, `game0x`),
`arbtool.exe`, `vulkan-1.dll` (ships with GPU drivers), the dev `base`/`d3xp` symlinks
from `dist-win/`.

**Separate optional asset — `dude-X.Y-baked-ao.zip`:** the generated AO packs
(`base/z_baked_ao.pk4`, `_props`, `_weapons`, ~100 MB) attach to every release as their
own download (the sanctioned release-attachment channel for generated assets — they
never go in the repo or the platform archives). Same drop-in layout (`base/` folder).

### zz_dude_menu.pk4 — rebuild from the repo every release

The DUDE main menu + high-res logo. Users get it merged into their `base/`:

```
cd base && zip -q ../dist/release-X.Y/zz_dude_menu.pk4 \
  guis/mainmenu.gui \
  guis/dude/mainmenu.gui \
  guis/_dude_anchor.pd \
  guis/assets/mainmenu/dude_logo.tga
```

The `zz_` prefix is load-bearing: it must sort after `pak000` and `zWideGuis_D3` so it
wins the search-path priority. If the menu ever grows new asset dependencies, add them
here — test with a clean-sandbox launch (fresh `fs_basepath` with only stock pk4s +
this pk4, fresh `fs_savepath`) before shipping.

Note on `dude_logo.tga`: copyright-protected id/Bethesda logo artwork, shipped as a
deliberate exception — see the "Known exception" paragraph in docs/publishing.md.

## 4. Release notes + publish (user action, never the agent)

Notes template: `dist/release-0.9/notes.md` (highlights, bring-your-own-game-data —
classic not BFG, per-platform setup incl. "copy the **contents** next to `base/`",
known limitations, source link + commit hash).

```
git push
gh release create vX.Y dist/release-X.Y/*.zip dist/release-X.Y/*.tar.gz \
  --title "DUDE X.Y" --notes-file dist/release-X.Y/notes.md --draft
# review in the browser, then Publish
```

(`gh repo set-default Inkub0/dude` once per clone — two remotes exist.)
