# Mod library auto-download

A way for DUDE to fetch a mod's game library (`<fs_game>.so` / `<fs_game>.dll`) on demand,
from a pinned GitHub release, when the mod is selected but its library is not installed. The
first mod it serves is Doom 3: Phobos (`tfphobos`, published at
github.com/Inkub0/dude-sdk, branch `tfphobos`), but the mechanism is a general **mod registry**, not a
Phobos special case.

It downloads mod *code* only. Game *data* (pak files) is never auto-downloaded: those assets
belong to their authors. When the paks are missing the engine tells the user to install the
mod's game data itself.

## Goals and non-goals

Goals:

- When `fs_game` names a known mod and its library is absent locally, offer to download the
  right per-OS, per-arch library and place it where the normal loader finds it.
- Make this safe: it is native code execution, so integrity and consent are load-bearing.
- Keep the engine's link-time dependencies unchanged (in particular, do not re-introduce a
  libcurl link on Linux, which broke Steam launches: see `docs` note on the Steam libcurl
  symbol conflict).
- Be a general registry so more mods can be added without an engine rebuild.

Non-goals:

- Downloading game data / assets.
- Auto-updating an already-installed library (v1 only fills a missing one).
- A package manager. This is a single-file fetch with a hash check.

## Trust model

Downloading and loading a native library is arbitrary code execution, so the design is built
around three anchors:

1. **Transport over HTTPS** with certificate validation, to a host that is pinned in the
   registry (github.com / its release CDN), never an arbitrary URL from untrusted input.
2. **A pinned SHA-256** for every downloaded library. The engine refuses to place or load a
   file whose hash does not match. This is the real integrity guarantee: it holds regardless
   of how the bytes arrived, so the choice of download transport cannot weaken it.
3. **Explicit user consent** before any download, plus a cvar to pre-authorize or disable the
   whole feature.

The registry file itself (which carries those hashes) is authenticated by the HTTPS fetch and
the pinned repository path: whoever controls the DUDE repository controls the registry, which
is the same trust boundary as the engine binary's own source. The engine also ships a baked-in
default copy of the registry, so it works offline and has a known-good fallback if the remote
fetch fails or looks malformed.

## The mod registry

A JSON file, hosted in the main DUDE repository and fetched over HTTPS, e.g.:

```
https://raw.githubusercontent.com/Inkub0/dude/master/mod-registry.json
```

A copy ships with the engine as the baked-in default (and offline fallback). The engine
fetches the remote copy when it needs to resolve a missing mod library; if the fetch fails,
times out, or does not parse, it uses the baked default.

Format (illustrative):

```json
{
  "version": 1,
  "mods": {
    "tfphobos": {
      "name": "Doom 3: Phobos game library",
      "repo": "Inkub0/dude-sdk",
      "release": "v1.0.0",
      "license": "GPLv3",
      "needsGameData": "Doom 3: Phobos (Team Future) - not downloadable",
      "assets": {
        "win64": {
          "url": "https://github.com/Inkub0/dude-sdk/releases/download/tfphobos-v1.0.0/tfphobos-win64.dll",
          "sha256": "<hash>"
        },
        "linux-x86_64": {
          "url": "https://github.com/Inkub0/dude-sdk/releases/download/tfphobos-v1.0.0/tfphobos-linux-x86_64.so",
          "sha256": "<hash>"
        }
      }
    }
  }
}
```

Notes:

- The `release` is a **pinned version**, never "latest": a future mod release could target a
  different game API, and pinning keeps the SHA-256 meaningful. Pointing DUDE at a newer mod
  library is a one-line registry edit, no engine rebuild.
- Platform keys match the engine's `D3_ARCH` / OS (e.g. `win64`, `linux-x86_64`). An entry with
  no asset for the running platform means "not available here", and the engine says so.
- Adding a mod is adding an entry. The code path is identical for every mod.

## Asset shape

Each platform asset is the **bare library file** (`tfphobos-win64.dll`,
`tfphobos-linux-x86_64.so`) published on the mod's release, and the registry pins that file's
SHA-256. One file, verify, place, no extraction step. The human-facing archives
(`<mod>-<ver>-<platform>.zip` / `.tar.gz`, which also carry COPYING and BUILDINFO) stay
as they are for manual installs. Alongside the downloaded library the engine writes a short
`LICENSE-<mod>.txt` pointing at the mod's repository and license, so the GPL notice travels
with the binary.

## Download transport

No new link-time dependency, and in particular no libcurl link on Linux:

- **Windows:** WinHTTP (a system component, linked with `-lwinhttp`). No extra runtime DLL.
- **Linux / macOS:** spawn the system `curl` (or `wget`) as a child process. If neither is
  present, fall back to the manual path (show the URL and where to put the file).

Why this avoids the libcurl conflict we hit before: the Steam launch failure came from DUDE
being *linked* against a distro libcurl whose versioned symbols (`CURL_OPENSSL_4`) are absent
in the libcurl Steam injects via `LD_LIBRARY_PATH`, so the dynamic loader killed the process at
startup. Here **DUDE never links or dlopens libcurl at all** - it runs a separate `curl`
process. That child has its own address space, and we launch it with a **sanitized
environment** (clearing `LD_LIBRARY_PATH` and `LD_PRELOAD`) so even the child resolves the real
system libraries instead of Steam's injected ones. On Windows, WinHTTP has nothing to do with
curl.

Because integrity rests on the SHA-256 pin, the transport only affects whether the download
*succeeds*, never whether a wrong or tampered library could be loaded.

## Flow

In `idCommon::LoadGameDLL` (neo/framework/Common.cpp), after the existing local search
(`fs_gameDllPath`, then `fs_savepath/<fs_game>`, then `fs_basepath/<fs_game>`) fails to find
`<fs_game>` + the platform library suffix, and before the fallback to the base-game library:

1. If `fs_game` is not in the registry, or `com_allowModDownload` is 0, keep the current
   behaviour (fallback / error). No change for unknown mods.
2. Resolve the registry (remote fetch over HTTPS, else baked default) and look up `fs_game`
   and the current platform. No asset for this platform -> clear message, stop.
3. Consent dialog via `SDL_ShowMessageBox` (the game menu does not exist this early):
   name the mod, the source (`repo` + `release`), the size, the license, and that the user
   still needs the mod's own game data. Yes / No.
4. On Yes: HTTPS GET the pinned asset to a temporary file.
5. Compute SHA-256 and compare to the pinned value. Mismatch -> delete, error, stop. Never
   place or load unverified bytes.
6. Move the verified library into `fs_savepath/<fs_game>/` and write `LICENSE-<mod>.txt`.
7. Retry the normal load. It now finds the library through the ordinary search path, so the
   download path and the load path stay decoupled.

## Consent and cvars

- `com_allowModDownload` (CVAR_ARCHIVE, default ask): `0` disables auto-download entirely
  (engine only reports the missing library and the manual URL); `1` allows it after the
  per-time consent dialog. A value that means "allowed, do not ask again" can be added once the
  dialog exists.
- The consent dialog always states the source, version, size and license, and that game data
  is separate. No silent downloads.

## Code vs game data

The fetched library is the mod's GPL (or otherwise licensed) *code*. The engine separately
detects missing mod *paks* (the normal "game data not found" condition) and instructs the user
to install the mod's data. Team Future's Phobos assets, Raven's Quake 4 assets and the like are
never fetched. For `tfphobos` specifically, the consent dialog and the data-missing message
both say the player needs their own Doom 3: Phobos install.

## Failure handling

Every failure path leaves the user informed and installs nothing unverified:

- Registry fetch fails or is malformed -> use baked default; if that lacks the mod, report it.
- No asset for this platform -> say so, give the repo URL.
- Download fails (network, no `curl`/`wget` on Linux) -> show the asset URL and the target
  directory for a manual install.
- SHA-256 mismatch -> delete the file, report a verification failure, do not retry silently.
- User declines -> report the missing library and the manual URL, then fall back / error as
  today.

## Registry entries

Mod libraries are built and released from the DUDE Mod SDK (github.com/Inkub0/dude-sdk), which mirrors
dhewm3-sdk: one branch per mod, releases tagged `<branch>-vX.Y.Z`. Current entries (21 libraries, 16
branches, all `<branch>-v1.0.0`): every mod dhewm3 has ported plus Doom 3: Phobos, Resurrection of
Evil variants included (`bloodmod_roe`, `doom3ccXP`, `librecoopd3xp`, `perfected_roe`,
`sikkmodd3xp`, which `FS_KnownModBase` loads on top of `d3xp`). `mod-registry.cfg` and the baked
copy in `ModRegistry.cpp` must stay identical; the Linux libraries are built in DUDE's Ubuntu 22.04
sandbox (glibc 2.27+), the Windows ones with mingw-w64 (they import `libwinpthread-1.dll`, which
DUDE's Windows package ships).


`tfphobos` -> Inkub0/dude-sdk, release `tfphobos-v1.0.0`, assets `win64` and `linux-x86_64`, GPLv3,
game data from Team Future. This requires adding bare-library assets
(`tfphobos-win64.dll`, `tfphobos-linux-x86_64.so`) to the tfphobos release and recording
their SHA-256 in the registry.

## Open items / future

- macOS and other arches: add assets and platform keys as the DUDE Mod SDK branches build them.
- "Allowed, do not ask again" consent state once there is UI for it.
- Optional: verify the registry's own signature (not just HTTPS) if the trust boundary ever
  needs to be tighter than "whoever controls the DUDE repo".
- Other mods: any mod published as a GPL (or redistributable) library with per-platform release
  assets can be added as a registry entry with no engine change.
