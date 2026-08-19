# DUDE — Doom3 Unified Development Engine

**DUDE** is a fork of [dhewm3](https://github.com/dhewm/dhewm3) (itself the GPL source
port of _DOOM 3_) that modernizes the renderer while keeping the classic _DOOM 3_ look
and gameplay faithful. The stock renderer's ARB assembly shaders are translated to a
shared GLSL source tree serving two new backends — **OpenGL 3.3 core** and **Vulkan**
(1.4, with hardware ray tracing where available) — selectable via the `r_graphicsAPI`
cvar, with the original ARB renderer kept intact as the faithful reference. All visual
enhancements are **opt-in**, driven by in-game quality presets whose floor is the
unmodified classic look.

**License note:** DUDE is a modified version of [dhewm3](https://github.com/dhewm/dhewm3),
based on id Software's Doom 3 GPL source release. All DUDE-authored source in this
repository — including every file under `neo/shaders/` and files without an explicit
license header — is licensed under the GNU GPLv3 (see COPYING.txt). Third-party
components (Dear ImGui, VMA, FSR2, SMAA, stb) keep their original licenses, stated in
their files. **No game assets are included or redistributable here**: the `base/*.pk4`
game data is proprietary id/Bethesda content — bring your own Doom 3 installation.

## Features

**Renderers** (`r_graphicsAPI opengl | opengl3 | vulkan`)
- Legacy **OpenGL/ARB** path (`opengl`, the default) — untouched, the faithful reference.
- **OpenGL 3.3 core** — the stock look re-expressed in GLSL, plus the enhancement suite.
- **Vulkan 1.4** — same enhancement suite, plus Vulkan-only features below. VMA-managed
  memory, persistent scene buffers, GPU timestamps (`r_vkGpuTime`).

**Opt-in enhancement suite** (GL3 + Vulkan; presets *Potato → Nightmare* in the classic
System menu, "Potato" = unmodified classic rendering)
- Shadow maps: cube shadows with rotated-Vogel PCF, sun/parallel-light shadow maps
  (stencil shadows remain the fallback and the faithful default).
- HDR pipeline, SSAO, PBR (GGX) with screen-space reflections, parallax occlusion
  mapping, soft particles, baked per-material AO maps.
- Anti-aliasing: SMAA 1x / FXAA, and **AMD FSR2** in Native-AA mode (motion vectors +
  jittered projection) on Vulkan.
- Smaller touches: depth-of-field on weapon reload, self-lit pickup glow, emissive GUI
  surfaces casting light, smoke-in-darkness blending, hi-res GUI font atlases.

**Vulkan-only**
- **Ray-traced sun shadows** (`r_rtSunShadows`) via `VK_KHR_ray_query` on RT-capable GPUs.
- **GPU tessellation** for characters/monsters (PN triangles + displacement), GPU skinning.
- Parallel image decode on level load, buffer-device-address batched indirect depth
  prepass, and other GPU-driven experiments (see [docs/gpu-offload-plan.md](docs/gpu-offload-plan.md)).

**Engine quality-of-life**
- `com_interpolate` — render above 60 fps while the game simulates at its native 60 Hz.
- `com_maxFPS` frame limiter, SSE4.1 SIMD baseline, runtime ARB→SPIR-V compilation so
  classic mod shaders work on the new backends, cvar-translation shim for old mods.

## Future development — the RTX pipeline

The long-term goal is to progressively **replace the raster rendering pipeline with ray
tracing** on RT-capable hardware. The foundation is already shipped: acceleration-structure
infrastructure over the live game scene, `VK_KHR_ray_query` shading, ray-traced sun
shadows, a GPU compute lane, and per-frame GPU-resident deformed geometry for animated
models. From there the roadmap ([docs/rtx-shadow-roadmap.md](docs/rtx-shadow-roadmap.md))
climbs tier by tier:

1. **Ray-traced shadows for all lights** — replacing shadow maps and stencil volumes
   with per-pixel traced visibility (the sun tier already works).
2. **Ray-traced reflections** — replacing screen-space reflections and their
   off-screen blind spots.
3. **Ray-traced ambient occlusion and indirect lighting**, converging on a fully
   ray-traced (path-traced) lighting pipeline in the spirit of the classic-game RTX
   remasters.

As with everything else in DUDE, the RTX path is additive: the faithful raster
renderers remain, both as the default look and as the path for GPUs without ray
tracing support.

## Supporting the project

If you enjoy DUDE, you can support development at
**[buymeacoffee.com/Inkub0](https://buymeacoffee.com/Inkub0)** ☕ — donations go
toward new hardware needed to build and test **real HDR output support** and the
**RTX pipeline** above.

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

Dependencies: CMake, GCC or Clang (C++17 for the Vulkan backend), SDL2 (SDL3 works as a
fallback), OpenAL; Vulkan headers + `glslangValidator` for the Vulkan backend; optional
`shaderc` for runtime mod-shader compilation on Vulkan.

## Running

DUDE needs the original _DOOM 3_ / _Resurrection of Evil_ game data (`base/*.pk4`,
`d3xp/*.pk4`) from your own copy of the game (the Steam/GOG classic version, not the
BFG edition). Point the engine at it with `+set fs_basepath /path/to/doom3`, or place
the pk4s next to the executable. Writable data (configs, saves, screenshots) lives in
its own "dude folder" (`~/.local/share/dude` on Linux, `Documents/My Games/dude` on
Windows).

# Upstream: dhewm3

DUDE is built on **dhewm3**; everything documented there (game data setup, original
compile instructions, mod support, FAQ) applies to DUDE's faithful baseline too:

- Project + README: https://github.com/dhewm/dhewm3
- Homepage: https://dhewm3.org — mods: https://dhewm3.org/mods.html
- FAQ: https://github.com/dhewm/dhewm3/wiki/FAQ

DUDE-specific documentation lives in this repo under [docs/](docs/) —
[port architecture](docs/port-architecture.md), [Vulkan backend](docs/vulkan-backend.md),
and per-feature plans.
