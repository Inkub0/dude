# DUDE 0.9.2 — ray tracing grows up, GPU skinning on by default

A feature release on top of [0.9.1](https://github.com/Inkub0/dude/releases/tag/v0.9.1).
The big story is ray tracing: on a Vulkan RTX GPU, DUDE now ray-traces sun shadows,
moving-light shadows, and shadows cast by animated monsters, wrapped up in a new
**Ultra Nightmare** preset. GPU skinning becomes the default on Vulkan, and _The Lost
Mission_ now runs natively.

## Changed since 0.9.1

### Ray tracing (Vulkan + RTX)

- **New "Ultra Nightmare" preset** — the RT-gated tier above Nightmare. Enables
  ray-traced sun and moving-light shadows on capable hardware; falls back cleanly to
  the Nightmare raster path on everything else.
- **Ray-traced sun shadows** (`r_rtSunShadows`): crisp, leak-free sun/moonlight
  shadows. The expensive raster sun shadow-map render is now skipped entirely whenever
  the sun is served by ray tracing.
- **Animated monsters cast ray-traced shadows** (`r_rtMonsterShadows`): enemies and
  other skinned characters now throw correct RT shadows, including alpha-tested
  (perforated) surfaces.
- **Ray-traced shadows for moving point lights** (`r_rtMovingLights`): dynamic lights
  that move through the world cast true ray-traced shadows instead of the raster
  scratch cube.
- **RT stays cheap at rest**: the acceleration structure is only rebuilt when the scene
  actually changes, so ray tracing costs almost nothing in a static view.
- RT shadow toggles are exposed in the in-game **Shadows** settings group (shown only
  when your GPU supports ray tracing).

### Rendering

- **GPU skinning is now the default on Vulkan** (Medium preset and up). Animated
  characters are skinned on the GPU, freeing CPU frame time. Potato/Low keep the
  faithful CPU path.
- **Better tessellated characters**: MD5 mesh seams are welded on the plain
  tessellation path too (not only under GPU skinning), fixing visible cracks at arms
  and shoulders on the tessellation presets.
- **Cheaper dynamic shadows**: moving lights' scratch shadow cube renders at half
  resolution by default (`r_shadowMapSplitDynDrop`), full-resolution on Nightmare —
  a measurable cube-pass cost cut with no visible difference on movers.
- **Chromatic aberration** post effect (`r_postChromaticAberration`): a subtle
  scene-only lens fringe that runs before the HUD, so the crosshair and UI stay crisp.

### Content & UI

- **_The Lost Mission_ (d3le) runs natively** on DUDE via a built-in game library — no
  extra DLLs. Drop the Lost Mission data in and it loads by name.
- **Simpler anti-aliasing menu**: post-AA is now a single selector —
  **Off / FXAA / SMAA / FSR2 (TAA)** — instead of overlapping toggles.

## Setup (same as 0.9.1)

You need the original _DOOM 3_ / _Resurrection of Evil_ game data (`base/*.pk4`,
`d3xp/*.pk4`) from your own copy (Steam/GOG classic, **not** the BFG Edition).

- **Windows (64-bit)**: unzip and copy the **contents** of the `dude-0.9.2-win64`
  folder into your Doom 3 directory, next to `base/` (the included pk4/config files
  merge into `base/`). Or run from anywhere with `+set fs_basepath "C:\path\to\Doom 3"`.
- **Linux (x86_64)**: untar and copy the contents into your Doom 3 directory the same
  way, run `./dude`.

Renderer selection: `+set r_graphicsAPI opengl|opengl3|vulkan` or the in-game menu.
Ray tracing requires the **Vulkan** renderer and an RTX / ray-query-capable GPU.
Configs/saves live in `~/.local/share/dude` (Linux) / `Documents/My Games/dude` (Windows).

## Optional download: baked ambient-occlusion pack

`dude-0.9.2-baked-ao.zip` (~100 MB): precomputed per-material AO maps for subtle
contact shading on world surfaces, props and weapons. Extract into your Doom 3
directory the same way. Entirely optional.

DUDE is GPLv3 — source at https://github.com/Inkub0/dude.
