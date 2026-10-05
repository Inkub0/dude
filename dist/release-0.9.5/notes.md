# DUDE 0.9.5 — the performance release

A performance-focused release on top of [0.9.4](https://github.com/Inkub0/dude/releases/tag/v0.9.4).
0.9.4 was about adding heavy ray-traced features; 0.9.5 is about making the engine **start
faster and run faster** without taking any of them away. The headline is load time — levels
now load in a fraction of the time, on **every** renderer — and on the Vulkan RT tier a wave
of GPU optimizations plus FSR2 upscaling claw back the cost of the ray tracing.

Nothing was cut to get here: the look is unchanged, and every quality preset renders exactly
what it did in 0.9.4. These are faster paths to the same frames.

## Changed since 0.9.4

### Load time — much faster, on every backend

- **Level loads are dramatically faster.** The biggest cost at load was hidden inside
  material parsing: every bump material was decoding its 2K normal map and running a
  64-iteration Poisson solve to build parallax/tessellation height fields — synchronously,
  during the parse, twice per material. That work is now **deferred and run in parallel** on
  the image worker threads at the end of the level load (`r_deferLevelGenerators`, default on),
  producing bit-identical pixels. On the test machine a cold `hell1` load dropped from
  **~11.3 s to ~2.6 s (≈4.4× faster)**; the material-parse phase alone went from ~8.5 s to
  a few milliseconds. The win scales with how many parallax/tessellated materials a map uses.
- **Particle bounds** no longer simulate the same 1000-particle frame a thousand times over
  to compute a bounding box — a long-standing loop that never advanced its own RNG. One
  sample gives identical bounds (particle setup ~352 ms → ~5 ms).
- **Binary render-model cache** (`r_binaryModelCache`, default on): finished geometry is
  serialized next to the game data on first load and read back on later loads, skipping the
  re-parse and surface finishing. The cache is keyed on the source timestamp, engine version
  and the relevant cvars, and self-heals on any mismatch or corruption.
- Images that are **already resident are no longer re-uploaded** on every level load.

### Frame time on the Vulkan RT tier

- **FSR2 sub-native upscaling** (`r_fsrScale`, *Graphics → Upscaling*): render the whole
  scene — including every ray-traced pass — at a lower internal resolution and let FSR2
  upscale to your display, which cuts the cost of *everything* that runs per scene-pixel.
  Presets from **Native AA** (100%, pixel-identical to 0.9.4) through Ultra Quality (75%),
  Quality (67%), Balanced (59%) and Performance (50%), plus a custom render-scale slider, with
  a live "renders at W×H, upscaled to W×H" readout. Film grain, chromatic aberration and the
  HUD stay at native resolution so text and overlays remain crisp. Vulkan + FSR2 TAA.
- **Volumetric fast-resolve** (`r_volFastResolve`, default on): the volumetric light march
  runs at low resolution with temporal reuse and is upsampled at composite, and the default
  step count was halved (12 → 6) with no visible loss — a large cut to the volumetrics cost.
- **Cheaper RT soft shadows**: the shadow-blur ray is checkerboarded and reconstructed in the
  blur (`r_rtShadowBlurCheckerboard`), the per-light blur is skipped where its penumbra is
  sub-pixel, and the blur intensity/falloff were retuned.
- **Cheaper RT global illumination and AO**: the GI gather hoists its constant work out of
  the per-sample loop, GI resolution is pinned per tier (Ultra 2/3, Nightmare 3/4), and RTAO
  runs at a reduced resolution with a tuned NRD history window (`r_rtaoResScale`,
  `r_rtaoHistory`). GI is also now fog-aware so haze no longer washes out the bounce light.
- **Acceleration-structure storage and scratch moved to device-local VRAM**, per-frame
  scratch vectors are reused instead of reallocated, and animated-BLAS staging is skipped
  entirely when no RT scene is requested.
- **Smarter shadow-cache budgeting**: the cube-shadow cache sizes itself to actual free VRAM
  (minus a reserve) rather than a flat half, and the cache is dropped when *RT Shadows for
  All Lights* already serves every light, freeing that VRAM.
- **Single-channel maps upload as R8** (AO, parallax and tessellation height) instead of
  RGBA — a third of the VRAM and bandwidth for those textures.
- Backend washes: the draw-surface sort uses `std::sort`, and the Vulkan backend skips a
  redundant index-buffer rebind.

### Smoothness & frame pacing

- **VRR / Adaptive Sync support** (`r_vrr`, a new VSync mode): on a G-SYNC / FreeSync display
  this runs vsync-on with an automatic frame cap just below the refresh rate, for tear-free
  low-latency pacing inside the variable-refresh window. Exposed in both the in-game menus.

### Menus & presets

- **The classic (non-ImGui) System options menu was rebuilt** to mirror the Controls page —
  a left-hand category column (Video / Sound / Advanced / Post-Process) switching inline
  panels instead of the old nested pop-ups — and the whole refactor was **ported to the
  Resurrection of Evil menu** as well, so both campaigns get the same modern options screen.
- **Ray tracing is now decoupled from the quality presets** (an orthogonal `dude_rtx` axis):
  you can run any preset with RT on or off rather than it being locked to the top tier. The
  graphics config is reset once on this schema change so everyone starts from clean,
  correctly-decoupled defaults.

### Correctness & robustness

- A Vulkan correctness/hardening audit landed: robust swapchain recreate, safe NRD recording
  for two denoise passes in a frame, TLAS rebuilds for GPU-skinned shadow casters, exact
  texture-set cache keys, live `r_swapInterval` changes, real free-VRAM reporting via
  `VK_EXT_memory_budget`, and a long tail of error-path fixes. The RT tier ran validation-clean
  and stable over ~110k frames.
- Fixed: FSR2 first-frame validation errors, double vision captured at full resolution on the
  RHI backends, and staggered RT soft shadows flickering across cutscene camera cuts.

## Performance notes

Load-time numbers above are from one machine (`hell1`, developer build) and scale with
content; your mileage will vary, but the speedup is universal — it is the same work done in
parallel and deferred, on OpenGL and Vulkan alike. For frame-time, use the in-game benchmark
from 0.9.4 (`com_timeDemoRealTime 1` then `timeDemo demo1`) to measure FSR2 and the RT
optimizations on your own GPU. On the RT tier, FSR2 Quality/Balanced is the single biggest
lever you have.

## Known limitations

- FSR2 sub-native upscaling, the RT optimizations and VRR auto-capping are **Vulkan-only**;
  FSR2 needs the FSR2 TAA path selected. The OpenGL renderers get the load-time and
  draw-sort improvements but none of the RT-tier frame-time work.
- VRR auto-capping relies on the driver actually running the display in its variable-refresh
  range (confirmed on Wayland; an X11 + NVIDIA mixed-refresh desktop may clamp vsync and not
  engage VRR — a driver limitation, not DUDE's).

## Setup (same as 0.9.4)

You need the original _DOOM 3_ / _Resurrection of Evil_ game data (`base/*.pk4`,
`d3xp/*.pk4`) from your own copy (Steam/GOG classic, **not** the BFG Edition).

- **Windows (64-bit)**: unzip and copy the **contents** of the `dude-0.9.5-win64`
  folder into your Doom 3 directory, next to `base/` (the included pk4/config files
  merge into `base/` and `d3xp/`). Or run from anywhere with
  `+set fs_basepath "C:\path\to\Doom 3"`.
- **Linux (x86_64)**: untar and copy the contents into your Doom 3 directory the same
  way, run `./dude`.

Renderer selection: `+set r_graphicsAPI opengl|opengl3|vulkan` or the in-game menu.
For the full RT tier pick the **Vulkan** renderer and the **Ultra Nightmare** preset.
Configs/saves live in `~/.local/share/dude` (Linux) / `Documents/My Games/dude` (Windows).

If the **Vulkan renderer won't start** (the OpenGL renderers are unaffected), update or
reinstall your GPU driver — the Vulkan driver (the ICD the loader dispatches to) ships with it,
and no bundled loader can substitute for a missing or Vulkan-incapable driver. On Windows,
`vulkan-1.dll` is installed by your GPU driver; DUDE deliberately does not ship one, so a
missing loader there means a driver update, not a missing file. On Linux a current loader is
bundled in `lib/` as a fallback for old distro loaders, but it still needs your driver's ICD.

## Optional download: baked ambient-occlusion pack

`dude-0.9.5-baked-ao.zip` (~161 MB): precomputed per-material AO maps for subtle contact
shading on world surfaces, props and weapons. Unchanged from 0.9.4 — skip the re-download if
you already have that one. Extract into your Doom 3 directory the same way. Entirely optional,
and unused at the RT tier (GI/RTAO replace it there).

DUDE is GPLv3 — source at https://github.com/Inkub0/dude.
