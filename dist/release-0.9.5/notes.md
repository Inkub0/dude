# DUDE 0.9.5 — the performance release

A performance-focused release on top of [0.9.4](https://github.com/Inkub0/dude/releases/tag/v0.9.4).
0.9.4 was about adding heavy ray-traced features; 0.9.5 is about making the engine **start
faster and run faster** without taking any of them away. The headline is load time — levels
load in a fraction of the time, on **every** renderer — and on the Vulkan RT tier a wave of
GPU optimizations plus FSR2 upscaling claw back the cost of the ray tracing.

Nothing was cut to get here: every quality preset renders exactly what it did in 0.9.4. These
are faster paths to the same frames.

## Changed since 0.9.4

### Load time — much faster, on every backend

- **Level loads are dramatically faster.** The biggest load cost was hidden inside material
  parsing: every bump material decoded its 2K normal map and ran a 64-iteration Poisson solve
  to build parallax/tessellation height fields — synchronously, during the parse, twice per
  material. That work is now **deferred and run in parallel** on the image worker threads at
  the end of the load (`r_deferLevelGenerators`, default on), producing bit-identical pixels.
  A cold `hell1` load dropped from **~11.3 s to ~2.6 s (≈4.4×)** on the test machine; the win
  scales with how many parallax/tessellated materials a map uses.
- **Particle bounds** no longer simulate the same 1000-particle frame a thousand times to get
  a bounding box — a loop that never advanced its own RNG. One sample gives identical bounds
  (~352 ms → ~5 ms).
- **Binary render-model cache** (`r_binaryModelCache`, default on): finished geometry is
  serialized on first load and read back later, skipping re-parse and surface finishing. Keyed
  on source timestamp, engine version and the relevant cvars; self-heals on any mismatch.
- Already-resident images are **no longer re-uploaded** on every level load.

### Frame time on the Vulkan RT tier

- **FSR2 sub-native upscaling** (`r_fsrScale`, *Graphics → Upscaling*): render the whole scene
  — every ray-traced pass included — at a lower internal resolution and let FSR2 upscale to
  your display, cutting the cost of *everything* that runs per scene-pixel. Presets from
  **Native AA** (100%, pixel-identical to 0.9.4) through Ultra Quality (75%), Quality (67%),
  Balanced (59%) and Performance (50%), plus a custom render-scale slider and a live "renders
  at W×H, upscaled to W×H" readout. Film grain, chromatic aberration and the HUD stay at
  native resolution so text and overlays remain crisp. Vulkan + FSR2 TAA.
- **Volumetric fast-resolve** (`r_volFastResolve`, default on): the light march runs at low
  resolution with temporal reuse and is upsampled at composite, and the default step count was
  halved (12 → 6) with no visible loss — a large cut to the volumetrics cost.
- **Cheaper RT soft shadows**: the shadow-blur ray is checkerboarded and reconstructed in the
  blur (`r_rtShadowBlurCheckerboard`), the per-light blur is skipped where its penumbra is
  sub-pixel, and the intensity/falloff were retuned.
- **Cheaper RT GI and AO**: the GI gather hoists its constant work out of the per-sample loop,
  GI resolution is pinned per tier (Ultra 2/3, Nightmare 3/4), and RTAO runs at reduced
  resolution with a tuned NRD history window (`r_rtaoResScale`, `r_rtaoHistory`). GI is also
  now fog-aware, so haze no longer washes out the bounce light.
- **Acceleration structures moved to device-local VRAM** (storage + scratch), per-frame
  scratch vectors are reused, and animated-BLAS staging is skipped when no RT scene is wanted.
- **Smarter shadow-cache budgeting**: the cube-shadow cache sizes itself to actual free VRAM
  (minus a reserve) instead of a flat half, and is dropped entirely when *RT Shadows for All
  Lights* already serves every light.
- **Single-channel maps upload as R8** (AO, parallax, tessellation height) instead of RGBA — a
  third of the VRAM and bandwidth. Plus backend washes: `std::sort` for the draw-surface sort,
  and a skipped redundant index-buffer rebind on Vulkan.

### Smoothness & frame pacing

- **VRR / Adaptive Sync** (`r_vrr`, a new VSync mode): on a G-SYNC / FreeSync display, runs
  vsync-on with an automatic frame cap just below the refresh rate for tear-free, low-latency
  pacing inside the variable-refresh window. Exposed in the in-game menus.

### Gameplay

- **Ultra Nightmare difficulty** (a fifth skill level, unlocked alongside Nightmare): Nightmare
  combat rules plus **permadeath** — no manual saves, autosaves only at map starts, and death
  **wipes the run** and sends you to a GAME OVER screen. Enforced engine-side, so a third-party
  mod DLL that clamps the skill can't quietly downgrade the run's rules. (Distinct from the
  *Ultra Nightmare* graphics preset — one is how hard it plays, the other how it looks.)

### Menus & presets

- **The classic (non-ImGui) System options menu was rebuilt** to mirror the Controls page — a
  left-hand category column (Video / Sound / Advanced / Post-Process) switching inline panels
  instead of nested pop-ups — and the refactor was **ported to the Resurrection of Evil menu**,
  so both campaigns get the same options screen.
- **Ray tracing is decoupled from the quality presets** (an orthogonal `dude_rtx` axis): run
  any preset with RT on or off rather than locking it to the top tier. The graphics config is
  reset once on this schema change so everyone starts from clean, decoupled defaults.

### Correctness & robustness

- A Vulkan correctness/hardening audit landed: robust swapchain recreate, safe NRD recording
  for two denoise passes in a frame, TLAS rebuilds for GPU-skinned shadow casters, exact
  texture-set cache keys, live `r_swapInterval` changes, real free-VRAM reporting via
  `VK_EXT_memory_budget`, and a long tail of error-path fixes — validation-clean and stable
  over ~110k frames on the RT tier.
- Fixed: FSR2 first-frame validation errors, double vision captured at full resolution on the
  RHI backends, and staggered RT soft shadows flickering across cutscene camera cuts.

## Performance notes

Load-time numbers are from one machine (`hell1`) and scale with content, but the speedup is
universal — the same work, done in parallel and deferred, on OpenGL and Vulkan alike. For
frame-time, use the in-game benchmark (`com_timeDemoRealTime 1` then `timeDemo demo1`) to
measure FSR2 and the RT optimizations on your own GPU; on the RT tier, FSR2 Quality/Balanced
is the single biggest lever you have.

## Known limitations

- FSR2 sub-native upscaling, the RT optimizations and VRR auto-capping are **Vulkan-only**
  (FSR2 needs the FSR2 TAA path). The OpenGL renderers get the load-time and draw-sort wins but
  none of the RT-tier frame-time work.
- VRR auto-capping relies on the driver running the display in its variable-refresh range
  (confirmed on Wayland; an X11 + NVIDIA mixed-refresh desktop may clamp vsync and not engage
  VRR — a driver limitation, not DUDE's).

## Setup (same as 0.9.4)

You need the original _DOOM 3_ / _Resurrection of Evil_ game data (`base/*.pk4`,
`d3xp/*.pk4`) from your own copy (Steam/GOG classic, **not** the BFG Edition).

- **Windows (64-bit)**: unzip and copy the **contents** of `dude-0.9.5-win64` into your
  Doom 3 directory, next to `base/` (the included pk4/config files merge into `base/` and
  `d3xp/`). Or run from anywhere with `+set fs_basepath "C:\path\to\Doom 3"`.
- **Linux (x86_64)**: untar and copy the contents into your Doom 3 directory the same way,
  run `./dude`.

Renderer selection: `+set r_graphicsAPI opengl|opengl3|vulkan` or the in-game menu. For the
full RT tier pick the **Vulkan** renderer and the **Ultra Nightmare** preset. Configs/saves
live in `~/.local/share/dude` (Linux) / `Documents/My Games/dude` (Windows).

If the **Vulkan renderer won't start** (the OpenGL renderers are unaffected), update or
reinstall your GPU driver — the Vulkan driver (the ICD the loader dispatches to) ships with it,
and no bundled loader can substitute for a missing or Vulkan-incapable one. On Windows,
`vulkan-1.dll` comes from your GPU driver; DUDE deliberately does not ship it. On Linux a
current loader is bundled in `lib/` as a fallback for old distro loaders, but it still needs
your driver's ICD.

## Optional download: baked ambient-occlusion pack

`dude-0.9.5-baked-ao.zip` (~155 MB): precomputed per-material AO maps for subtle contact
shading on world surfaces, props and weapons. Unchanged from 0.9.4 — skip the re-download if
you already have it. Extract into your Doom 3 directory the same way. Entirely optional, and
unused at the RT tier (GI/RTAO replace it there).

DUDE is GPLv3 — source at https://github.com/Inkub0/dude.
