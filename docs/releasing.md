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

It also configures with `-DCURL=OFF`: **the Linux `dude` must not link libcurl.** Steam
starts every program (Launch Options, non-Steam shortcuts) with its runtime's
`LD_LIBRARY_PATH`, which puts a pinned `libcurl.so.4` exporting only `CURL_OPENSSL_3`
ahead of the system's; a binary built against Ubuntu's libcurl needs `CURL_OPENSSL_4` and
the loader refuses to start it (0.9.8 shipped that way). libcurl only served multiplayer
pak downloads from servers (`ID_ENABLE_CURL`, `FileSystem.cpp`).

**Always build a release with `-c` (clean) when a shader was added or renamed since the
last build.** Both scripts reconfigure CMake only when their build dir has no cache, and
the SPIR-V/embed steps key off a `file(GLOB)` captured at configure time — so an
incremental rebuild silently omits new `.comp`/`.vert`/… from the embedded table. The
binary then builds fine but runs without those shaders (e.g. the GPU-skinning kernels went
missing on Windows this way, disabling skinned motion vectors). `./build-win.sh -c` and
`./build-linux-release.sh -c` wipe and reconfigure. Verify (see below) that any new shader
is embedded: `strings <bin> | grep spv/<name>`.

Sanity — BOTH binaries, EVERY release (a stale sandbox build once shipped without
the embedded shaders → black screen on GL3/VK):

```
strings <binary> | grep "DUDE 0\."          # must print the new version
strings <binary> | grep -c "spv/interaction" # must be >0: embedded shader table present
```

Linux only, also:

```
readelf -d build-linux-release/dude | grep -c curl   # must be 0 (no libcurl, see above)
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
| **`d3xp/zz_dude_roe_menu.pk4`** — the RoE menu, see below | ✓ | ✓ |
| `base/gamepad.cfg`, `base/gamepad-d3xp.cfg`, `base/mods-cvar-translation.cfg` | ✓ | ✓ |
| `base/pbr/pbr_materials.cfg`, `base/pbr/pbr_overrides.cfg` (else PBR classifies nothing) | ✓ | ✓ |
| `base/def/*.def`, `d3xp/def/*.def`, `d3le/def/*.def` — the `g_extraLights` def overrides (since 0.9.7), **loose** (see below) | ✓ | ✓ |
| `COPYING.txt`, `COPYING-QUAKE4-EXCEPTION.txt`, `README.md` | ✓ | ✓ |

Shaders (GLSL + SPIR-V) are **embedded in the binary** since 0.9.1
(`shaders/embed_shaders.py`, loaders fall back VFS → dev tree → embedded) — no
shader pk4 exists and none must be added.

**Def overrides (since 0.9.7):** the tracked `base/def`, `d3xp/def` and `d3le/def` files are
full copies of stock defs with the `g_extraLights` tags. They ship **loose**: a game folder's
loose files outrank its own paks, so they win without a `zz_` pk4 - and a `d3le/` folder must
never get a pk4 from us, or it shows up in the Mods menu for players without Lost Mission
(`ListMods` lists any folder holding a pk4). Take them from git (`git ls-files <dir>/def`), not
the working tree.

**Never include in the platform archives:** game pk4s (`pak000`–`pak008`, `game0x`),
`arbtool.exe`, `vulkan-1.dll` (ships with GPU drivers), the dev `base`/`d3xp` symlinks
from `dist-win/`.

**Separate optional assets — the baked-AO packs (decoupled since 0.9.8):** baked AO is no
longer attached to each version release. It is a standalone, independently-versioned artifact on
its own `vN` axis, re-published only when the baker changes (algorithm, parameters or format) —
one pack serves many DUDE releases. It never goes in the repo or the platform archives. Each
pack is a single drop-in `.pk4`:
- Doom 3 family — one merged `z_baked_ao.pk4` into `base/` (covers Doom 3, RoE and LM via the
  d3xp/d3le → base search fallback; one pack in `base/` means `d3le/` never gets a pk4, so it
  cannot show up as a mod for players without Lost Mission). Built by `pack_ao_d3.sh vN`.
  Published as a dedicated **Baked AO** GitHub release on `Inkub0/dude`, on the AO `vN` axis.
- Quake 4 — `z_baked_ao.pk4` into `q4base/`. Built by `pack_ao_q4.sh vN`. Published on
  `Inkub0/dude-q4`. Never attach AO (or any data) generated from Quake 4 maps to a DUDE release.

Each version's release notes link to the current recommended pack with a one-line drop-in note;
the packs are not rebuilt or re-uploaded per version.

**Create every Baked AO release with `--latest=false`** (`gh release create baked-ao-vN ...
--latest=false`), on both repositories. GitHub otherwise marks the newest release *Latest*,
and the website's download buttons (`releases/latest/download/...`, section 4) would then
point at the AO release, which has no DUDE archives.

### zz_dude_menu.pk4 — rebuild from the repo every release

The DUDE main menu + high-res logo. Users get it merged into their `base/`:

```
cd base && zip -q ../dist/release-X.Y/zz_dude_menu.pk4 \
  guis/mainmenu.gui \
  guis/dude/mainmenu.gui \
  guis/_dude_anchor.pd \
  guis/assets/mainmenu/dude_logo.tga \
  fx/teleporter.fx fx/teleporter1.fx fx/teleporter1_fly.fx \
  fx/teleporter2.fx fx/teleporter3.fx
```

The `zz_` prefix is load-bearing: it must sort after `pak000` and `zWideGuis_D3` so it
wins the search-path priority. If the menu ever grows new asset dependencies, add them
here — test with a clean-sandbox launch (fresh `fs_basepath` with only stock pk4s +
this pk4, fresh `fs_savepath`) before shipping.

**FX overrides (since 0.9.6):** DUDE's loose `base/fx/teleporter*.fx` (the dimmed,
reddened monster spawn-in flash) are packed into this same pk4 — they only override the
stock pak00x copies if they ship behind the load-bearing `zz_` prefix, so a release that
omits them silently reverts the spawn flash to stock white. The RoE copies go in the RoE
menu pk4 below.

Note on `dude_logo.tga`: copyright-protected id/Bethesda logo artwork, shipped as a
deliberate exception — see the "Known exception" paragraph in docs/publishing.md.

### zz_dude_roe_menu.pk4 — the RoE menu (since 0.9.5)

The Resurrection of Evil port of the DUDE System options menu. Ships in `d3xp/`, so
RoE players get the same modern options screen. Built from the git-tracked
`d3xp/guis/mainmenu.gui` plus a clean anchor:

```
# stage: the RoE menu + a clean copy of DUDE's own anchor under the name the
# .gui #includes (guis/_cst_anchor.pd) — so the .gui is untouched and no
# CstDoom3/fva-derivative content ships.
mkdir -p /tmp/roemenu/guis
cp d3xp/guis/mainmenu.gui   /tmp/roemenu/guis/mainmenu.gui
cp base/guis/_dude_anchor.pd /tmp/roemenu/guis/_cst_anchor.pd
cd /tmp/roemenu && zip -q <repo>/dist/release-X.Y/zz_dude_roe_menu.pk4 \
  guis/mainmenu.gui guis/_cst_anchor.pd
# then the RoE spawn-flash FX overrides (since 0.9.6), from the repo d3xp/:
cd <repo>/d3xp && zip -q <repo>/dist/release-X.Y/zz_dude_roe_menu.pk4 \
  fx/teleporter_96.fx fx/teleporter1_96.fx fx/teleporter1_vulgar.fx \
  fx/teleporter2_96.fx fx/teleporter3_96.fx
```

The RoE `.gui` also `#include`s `guis/test/grid2b.pd` + `grid3b.pd`; those are stock id
files resolved from `base/pak000.pk4` via the d3xp→base search fallback, so they need not
ship. `zz_` sorts after `zWideGuis_RoE.pk4` (lowercase `z` > uppercase `W`) so it wins the
search-path priority. Test on a clean-sandbox launch that the RoE Options screen loads.

### Quake 4 game library — its own archive (since 0.9.8)

The Quake 4 game library is under the Quake 4 SDK licence, not the GPL, so it never goes in
the platform archives above; it gets its own per platform. Rules and reasons:
[quake4-release-licensing.md](quake4-release-licensing.md).

```
# gates first (exit 1 = stop)
tools/quake4/check_provenance.py --interface ~/quake4-refs/q4sdk-frisk-initial \
    --ref 'openq4-bse!=~/quake4-refs/openq4/src/bse' \
    --ref 'idtech4a-bse!=~/quake4-refs/idtech4a/doom3/neo/raven/bse' \
    --ref q4sdk=~/quake4-refs/q4sdk-frisk-initial
# both repositories committed and tagged vX.Y, then the library from those commits, with
# the release binaries: build-linux-release.sh builds q4base.so in its sandbox (same glibc
# floor as dude) when ~/dude-quake4-game exists (Q4GAME_DIR); Windows with MinGW, like dude.exe
./build-linux-release.sh
DUDE_BUILD=build-win tools/quake4/build-q4game.sh
Q4DIR="/path/to/Quake 4" tools/quake4/package-q4.sh X.Y linux-x86_64 build-linux-release/q4base.so
Q4DIR="/path/to/Quake 4" tools/quake4/package-q4.sh X.Y win64 build-win/q4base.dll
# -> dist/release-X.Y/dude-q4-X.Y-linux-x86_64.tar.gz, dude-q4-X.Y-win64.zip
# every archive of the release, DUDE's included: nothing from retail Quake 4
python3 -I tools/quake4/check_no_retail_data.py --retail "/path/to/Quake 4" dist/release-X.Y/*.tar.gz dist/release-X.Y/*.zip
# for the notes: the GPL code inside the library (record the class table)
tools/quake4/q4base_licence_audit.py --so build/q4base.so --sdk <pristine SDK source/> --game-repo ~/dude-quake4-game
```

- The Quake 4 part of the notes and the dude-q4 release page carry **no donation link**.
- Name Quake 4 only as what DUDE runs ("runs Quake 4 — not included"); no Quake 4 logo or art.
- Never attach baked AO, env probes or other data generated from Quake 4 maps.

## 4. Release notes + publish (user action, never the agent)

Notes template: `dist/release-0.9/notes.md` (highlights, bring-your-own-game-data —
classic not BFG, per-platform setup incl. "copy the **contents** next to `base/`",
known limitations, source link + commit hash). Link the website's install guide
(https://inkub0.github.io/dude/install.html) in the setup section, and use the site link on
ModDB and other announcements: links in are what search engines rank the site on.

```
git push
gh release create vX.Y dist/release-X.Y/*.zip dist/release-X.Y/*.tar.gz \
  --title "DUDE X.Y" --notes-file dist/release-X.Y/notes.md --draft
# review in the browser, then Publish (it must end up marked Latest)
```

**On Publish, `.github/workflows/release-fixed-names.yml` adds the website's copies.** The
site's (https://inkub0.github.io/dude/) download buttons are
`https://github.com/Inkub0/dude/releases/latest/download/<fixed name>`, which GitHub redirects
to that file in the release marked *Latest*. The workflow copies the release's
`dude-X.Y-win64.zip`, `dude-X.Y-linux-x86_64.tar.gz`, `dude-q4-X.Y-win64.zip` and
`dude-q4-X.Y-linux-x86_64.tar.gz` to the same release as `dude-win64.zip`,
`dude-linux-x86_64.tar.gz`, `dude-q4-win64.zip` and `dude-q4-linux-x86_64.tar.gz`, so the site
needs no edit per release. Upload the archives under their versioned names as always (DUDE's
missing-Quake-4-library message names `dude-q4-X.Y-<platform>`); if one is missing the workflow
run fails and says which. Check the run under *Actions* after publishing; to redo it for an
existing release: *Actions* > *Release fixed-name archives* > *Run workflow* with the tag.

(`gh repo set-default Inkub0/dude` once per clone — two remotes exist.)

Quake 4 (since 0.9.8): tag `vX.Y` in the dude-q4 repository too, push it, and attach the
`dude-q4-X.Y-*` archives to the DUDE release (or a dude-q4 release) — the same user action.
