# DUDE: Doom3 Unified Development Engine

![DUDE logo](https://inkub0.github.io/dude/assets/img/logo.png)

**Website, downloads and install guide: https://inkub0.github.io/dude/**

**DUDE** is a fork of [dhewm3](https://github.com/dhewm/dhewm3) (itself the GPL source
port of _DOOM 3_) that modernizes the renderer while keeping the classic look and
gameplay faithful. It runs _DOOM 3_, _Resurrection of Evil_, _The Lost Mission_ and, since
0.9.8, the retail _Quake 4_ campaign, all from one executable.

The stock renderer's ARB assembly shaders are translated to a shared GLSL source tree
serving two new backends, **OpenGL 3.3 core** and **Vulkan 1.4**, selectable via the
`r_graphicsAPI` cvar, with the original ARB renderer kept intact as the faithful reference.
On ray-tracing GPUs the Vulkan backend goes all the way to a **path-traced lighting tier**.
All visual enhancements are **opt-in**, driven by in-game quality presets whose floor is the
unmodified classic look.

**License note:** DUDE is a modified version of [dhewm3](https://github.com/dhewm/dhewm3),
based on id Software's Doom 3 GPL source release. All DUDE-authored source in this
repository, including every file under `neo/shaders/` and files without an explicit
license header, is licensed under the GNU GPLv3 (see COPYING.txt), with an additional
permission for linking with the Quake 4 SDK game code (COPYING-QUAKE4-EXCEPTION.txt). Third-party
components (Dear ImGui, VMA, FSR2, NRD, SMAA, stb) keep their original licenses, stated in
their files. **No game assets are included or redistributable here**: the game data is
proprietary id/Bethesda content; bring your own copy of the game.

## Features

**Renderers** (`r_graphicsAPI opengl | opengl3 | vulkan`)
- Legacy **OpenGL/ARB** path (`opengl`, the default): untouched, the faithful reference.
- **OpenGL 3.3 core**: the stock look re-expressed in GLSL, plus the enhancement suite.
- **Vulkan 1.4**: the same enhancement suite, plus ray tracing and the Vulkan-only features
  below. VMA-managed memory, persistent scene buffers, GPU timestamps (`r_vkGpuTime`).

**Quality presets.** Two independent axes, in the classic System menu and the F10 settings
menu:
- **Quality** (`dude_preset`): *Potato → Low → Medium → High → Ultra → Nightmare*. "Potato"
  is the unmodified classic rendering.
- **Ray tracing** (`dude_rtx`, Vulkan on an RT-capable GPU): *RTX Off*, *RTX On* (hybrid:
  ray-traced shadows, reflections, AO and GI over the raster renderer) and *Path Tracing*.

**Ray tracing** (Vulkan, `VK_KHR_ray_query`)
- **Path Tracing tier**: direct light from every light in the scene, sampled with **ReSTIR
  DI** and a **ReGIR light grid**, denoised with **NVIDIA NRD** (ReLAX), with per-texel bump
  detail restored after the denoise.
- **Ray-traced global illumination** with a multi-bounce **irradiance cache**; Doom 3's
  ambient lights add only what the traced bounce falls short of (`r_rtgiAmbient`).
- **Ray-traced shadows** for every light (soft, with grates and fences casting their pattern),
  **ray-traced reflections** and **ray-traced AO**.
- Animated and GPU-skinned characters are in the acceleration structures every frame,
  with per-bone motion vectors for the denoiser and FSR2.
- **Volumetric lighting** (`r_volumetrics`): light shafts and lit haze from the scene's
  real lights.
- **Emissive Mode** (experimental, off by default): glowing surfaces become real area lights,
  set up per map in the in-game **Light Editor** (`editLights`).

**Raster enhancement suite** (GL3 + Vulkan)
- Shadow maps: cube shadows with rotated-Vogel PCF, sun/parallel-light shadow maps,
  contact shadows (stencil shadows remain the fallback and the faithful default).
- HDR pipeline, GTAO/SSAO, PBR (GGX) with screen-space reflections, parallax occlusion
  mapping, subsurface skin and flesh shading, soft particles, baked per-material AO maps.
- Anti-aliasing and upscaling: SMAA 1x / FXAA, and **AMD FSR2** (native AA or sub-native
  upscaling, `r_fsrScale`) on Vulkan.
- **GPU tessellation** for characters and monsters (Vulkan), GPU skinning.
- Smaller touches: depth-of-field on weapon reload, self-lit pickup glow, emissive GUI
  surfaces casting light, smoke-in-darkness blending, hi-res GUI font atlases, anisotropic
  filtering.

**Quake 4**
- The retail single-player campaign on the GL3, Vulkan and RT tiers, with DUDE's presets.
- Quake 4's own formats (maps, collision, AAS, materials, GUIs, sound shaders, fonts) read by
  the engine, and **BSE**, Raven's particle and effects system, reimplemented from the effect
  files and the SDK interface.
- The game code is a separate library from a separate project (see
  [Running Quake 4](#running-quake-4)).

**Games and mods**
- _Resurrection of Evil_ and _The Lost Mission_ with their own game libraries; the stock
  HUDs stay correctly proportioned on wide screens without any pack.
- Binary compatibility with dhewm3 1.5.5 mod libraries, runtime ARB→SPIR-V compilation so
  classic mod shaders work on the new backends, and a cvar-translation shim for old mods.
- Savegames from retail Doom 3, dhewm3 and every DUDE 0.9.x release load.

**Gameplay**
- **Ultra Nightmare** difficulty (unlocked with Nightmare): Nightmare rules plus permadeath:
  one checkpoint per run written at map starts only, no manual saves, erased when you die
  ([docs/ultra-nightmare.md](docs/ultra-nightmare.md)).
- **Playable third person** (`pm_thirdPerson 1`, Doom 3 and Quake 4): the HUD stays up, the
  crosshair sits where the shots land, and the player model casts and receives shadows like
  any other character (see [Configuration.md](Configuration.md)).

**Engine quality-of-life**
- `com_interpolate`: render above 60 fps while the game simulates at its native 60 Hz.
- `com_classicTics`: Doom 3's original 62.5 Hz game tic (16 ms, as 1.3.1 ran) instead of
  60 Hz, for speedrunners; pair it with `com_interpolate` (see [Configuration.md](Configuration.md)).
- VRR / adaptive sync (`r_vrr`), `com_maxFPS` frame limiter, SSE4.1 SIMD baseline.
- Fast level loads: parallel image and sound decoding, a binary model cache.
- In-game benchmark: `com_timeDemoRealTime 1` then `timeDemo demo1` reports average and
  percentile frame times with a CPU/GPU split ([docs/benchmark.md](docs/benchmark.md)).

## Ray tracing: where it stands

The long-term goal was to **replace the raster lighting pipeline with ray tracing** on
RT-capable hardware, tier by tier. That path is now complete up to the Path Tracing tier:
shadows, reflections, AO, global illumination and finally direct lighting are traced and
denoised ([docs/rtx-hybrid-roadmap.md](docs/rtx-hybrid-roadmap.md),
[docs/rtx-restir-di.md](docs/rtx-restir-di.md)). The current work is making it faster and
finishing **Emissive Mode**, where a level's glowing fixtures light the scene themselves
instead of the placed lights.

As with everything else in DUDE, the ray-traced path is additive: the faithful raster
renderers remain, both as the default look and as the path for GPUs without ray
tracing support.

## Supporting the project

If you enjoy DUDE, you can support development at
**[buymeacoffee.com/Inkub0](https://buymeacoffee.com/Inkub0)** ☕. Donations go
toward new hardware needed to build and test **real HDR output support** and the
**ray-tracing pipeline** above.

## Building

Linux (native):
```
./build.sh              # GL3 + legacy renderers  -> build/dude
./build.sh --vulkan     # + the Vulkan backend (needs Vulkan headers + glslang)
./run.sh                # launch
```

Windows (cross-compiled from Linux with mingw-w64):
```
./build-win.sh          # GL3 + Vulkan            -> dist-win/dude.exe (+ DLLs, self-contained)
./build-win.sh --no-vulkan
```

Quake 4 game library (needs a checkout of [dude-q4](https://github.com/Inkub0/dude-q4), by
default in `~/dude-quake4-game`), built against the DUDE build above:
```
tools/quake4/build-q4game.sh                      # -> build/q4base.so
DUDE_BUILD=build-win tools/quake4/build-q4game.sh # -> q4base.dll (MinGW)
```

Dependencies: CMake, GCC or Clang (C++17 for the Vulkan backend), SDL2 (SDL3 works as a
fallback), OpenAL; Vulkan headers + `glslangValidator` for the Vulkan backend; optional
`shaderc` for runtime mod-shader compilation on Vulkan.

## Running

DUDE needs the original game data from your own copy of the game.

**Doom 3.** _DOOM 3_, _Resurrection of Evil_ and _The Lost Mission_ (`base/*.pk4`,
`d3xp/*.pk4`, `d3le/*.pk4`) from the Steam/GOG classic version, not the BFG edition. Point the
engine at it with `+set fs_basepath /path/to/doom3`, or place the pk4s next to the executable.
Writable data (configs, saves, screenshots) lives in its own "dude folder"
(`~/.local/share/dude` on Linux, `Documents/My Games/dude` on Windows).

### Running Quake 4

The same executable runs the retail _Quake 4_ from your own install
([docs/quake4-port.md](docs/quake4-port.md)). The Quake 4 game library (`q4base.so` /
`q4base.dll`) is a separate download, `dude-q4-<version>-<platform>`, from a separate project,
[dude-q4](https://github.com/Inkub0/dude-q4): it is built from Raven Software's Quake 4 SDK and
is under the SDK's licence, not the GPL - non-commercial, free copies only, the licence file
always attached. No Quake 4 game data is included anywhere. Without the library DUDE stops with a
message naming the archive for your version and platform and where to extract it, with a
button that opens the download page. The library links parts of DUDE's idlib; the DUDE author's code
in it is covered by the section 7 permission in COPYING-QUAKE4-EXCEPTION.txt. Licence details:
[docs/quake4-release-licensing.md](docs/quake4-release-licensing.md).

Setup: copy the contents of the DUDE archive and of the `dude-q4` archive into your Quake 4
folder (the one that contains `q4base/`) and start `dude` there; DUDE sees Quake 4's paks and
runs Quake 4. Each archive holds a single folder (`dude-<version>-<platform>/`,
`dude-q4-<version>-<platform>/`): copy what is inside it, so that `dude` and the `q4base`
library end up right next to Quake 4's own executable, not in subfolders. Never copy Quake 4's
files into DUDE's or Doom 3's `base/`: DUDE reads them from `q4base/` where they are installed,
and Quake 4 paks in `base/` stop DUDE with a message saying so (they share pak001-pak008's names
with Doom 3, so a copy over Doom 3's `base/` replaces Doom 3's own). From anywhere else, start it
with `+set fs_basepath "/path/to/Quake 4"`; on Linux `./run-q4.sh` finds a Steam install by
itself. A folder that holds both Doom 3's and Quake 4's game data stops with a message: start
with `+set com_title doom3` or `+set com_title quake4` to choose.

**Full retail Quake 4 only.** DUDE needs the full game (Steam, GOG or disc) and refuses the
Quake 4 demo, or an install missing its map paks: the Quake 4 SDK licence lets the game library
run with the full version of Quake 4 only. Multiplayer is not supported yet.

Quake and Quake 4 are trademarks of their owners (id Software / ZeniMax Media). DUDE is an
independent, non-commercial project, not affiliated with, endorsed or sponsored by id Software,
ZeniMax Media, Microsoft, Raven Software or Activision.

# Upstream: dhewm3

DUDE is built on **dhewm3**; everything documented there (game data setup, original
compile instructions, mod support, FAQ) applies to DUDE's faithful baseline too:

- Project + README: https://github.com/dhewm/dhewm3
- Homepage: https://dhewm3.org (mods: https://dhewm3.org/mods.html)
- FAQ: https://github.com/dhewm/dhewm3/wiki/FAQ

DUDE-specific documentation lives in this repo under [docs/](docs/):
[port architecture](docs/port-architecture.md), [Vulkan backend](docs/vulkan-backend.md),
[Quake 4 port](docs/quake4-port.md), [Quake 4 effects (BSE)](docs/quake4-bse.md), and
per-feature plans.
