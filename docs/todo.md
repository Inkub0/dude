# TODO / roadmap

Forward-looking work for the dhewm-rt enhanced renderer. These are planned
features / improvements — not defects (see [known-bugs.md](known-bugs.md)).

## Quality presets  **[DONE — shipped 2026-07-27]**

One-click quality presets in the Enhancements tab set the enhancement cvars as a group
instead of making the user dial every slider by hand. Scale, lowest to highest:

**Potato · Low · Medium · High · Ultra · Ultra Nightmare**

**As-built** (`enhancementPresets[]` in
[Dhewm3SettingsMenu.cpp](../neo/framework/Dhewm3SettingsMenu.cpp), drawn at the top of
`DrawEnhancementsMenu()`): combo + "Apply Preset" button; a live status line reads back
"Current: <tier>" or "Custom (hand-tuned)" via `DetectEnhancementPreset()` once any slider
is touched. Separate from Video Options' vanilla `com_machineSpec` preset.

Design decisions taken:
- **Anchor: High = shipped defaults** (the GTX-1070-calibrated look), with deliberate
  deviations: **Blinn-Phong specular** (`r_shading 1`) is on for every tier above Potato, where
  the shipped cvar default is the vanilla LUT (`r_shading 0`) — effectively free (analytic
  math replacing a texture LUT fetch) — and the **rendering-pipeline tiers** exceed the
  off-by-default cvars: HDR from Medium up, PBR materials from High up, screen-space
  reflections from Ultra up (docs/ssr.md; half march resolution on Ultra, two-thirds on
  Ultra Nightmare). Two tiers scale down for weaker GL 3.3 hardware, two push modern GPUs.
- **Potato = the faithful floor**: every enhancement off *and* vanilla specular → both the
  cheapest tier *and* an exact vanilla frame, so "source-accurate" is always one click away
  (satisfies the "keep faithful reachable" note below). Playable on any GL 3.3 card.
- Presets move the **performance levers** (on/off, resolution, sample counts, budgets) plus
  the **specular look** (shading model + scale + exponent). Other artistic-calibration cvars
  (SSAO radius/intensity/floor, emissive reach/tint, shadow biases) stay at the user's tuned
  globals so all tiers share one look at different cost.

| lever | Potato | Low | Medium | **High** | Ultra | Ultra Nightmare |
|---|---|---|---|---|---|---|
| soft particles | off | on | on | on | on | on |
| smoke dark-blend | off | off | off | off | on | on |
| emissive surfaces | off | off | on | on | on | on |
| SSAO | off | off | on | on | on | on |
| baked AO maps | off | on | on | on | on | on |
| shadow mapping | off (stencil) | off (stencil) | on | on | on | on |
| specular shading | vanilla LUT | Blinn-Phong | Blinn-Phong | Blinn-Phong† | Blinn-Phong† | Blinn-Phong† |
| specular scale | 1.0 | 1.2 | 1.2 | 1.2 | 1.2 | 1.2 |
| specular exponent | (n/a, LUT) | 42 | 42 | 42 | 42 | 42 |
| HDR scene buffer | off | off | on | on | on | on |
| PBR materials (GGX) | off | off | off | on | on | on |
| screen-space reflections | off | off | off | off | on | on |
| SSR march resolution | — | — | — | — | 1/2 | 2/3 |
| SSAO res scale | — | — | 0.5 | 0.75 | 0.8 | 1.0 |
| SSAO slices / steps | — | — | 3 / 2 | 3 / 3 | 6 / 4 | 7 / 5 |
| SSAO normal G-buffer | — | — | off | on | on | on |
| shadow 2D / cube res | — | — | 512 / 512 | 1024 / 1200 | 2048 / 2048 | 2048 / 2048 |
| cube PCF taps | — | — | 5 | 6 | 8 | 12 |
| point-light budget | — | — | 16 | 64 | 96 | 128 (all) |
| shadow size-scale | on* | on* | on | on | on | on |
| size-scale pivot radius | 380* | 380* | 380 | 380 | 340 | 340 |
| emissive light cap | — | — | 16 | 24 | 32 | 48 |
| film grain / chroma | off | off | 0.04 / 0.2 | 0.04 / 0.2 | 0.04 / 0.2 | 0.04 / 0.2 |
| reflection scale | 1.0 | 1.0 | 0.7 | 0.7 | 0.7 | 0.7 |

(Potato/Low carry the cheap Medium SSAO/shadow sub-params under the off toggles so a
manual feature flip from those tiers stays affordable. `*` = inert under stencil shadows
but carried for determinism. `†` = dormant while PBR supersedes the specular model on
High and up; still applied for determinism. Sub-Ultra tiers carry SSR march resolution
1.0 so a hand-enabled SSR runs at the full-res default.)

Shadow size-scaling (`r_shadowMapSizeScale`) stays **on at every tier**: it distributes the
per-light resolution budget by light importance (a light at the pivot radius gets the tier's
base cube res, bigger lights get more, smaller ones less). The tiered **base resolution** is
the primary lever; the **pivot radius** (`r_shadowMapSizeScaleRadius`) is a secondary
sharpness lever — *lowering* it makes more of a map's lights count as "large" and get boosted
above base res, so only the top two tiers tighten it (Ultra 340, Ultra Nightmare 300) while
Potato→High keep the Doom-3-typical 380 default.

Original spec (kept for reference) — each preset writes the non-vanilla enhancement cvars
together, covering at minimum:

- **SSAO** — `r_ssao`, `r_ssaoResScale`, `r_ssaoSlices`, `r_ssaoSteps`, `r_ssaoRadius`,
  `r_ssaoNormalBuffer`, `r_ssaoBentNormal`.
  (e.g. Potato = off; Medium ≈ the current half-res 6-slice/1-step defaults; Ultra
  Nightmare = full-res, more slices/steps, normal G-buffer on.)
- **Shadows** — `r_shadowMapping`, `r_shadowMapPointSize`, `r_shadowMapSize`,
  `r_shadowMapPointLimit`, `r_shadowMapFaceCull`, `r_shadowMapCubePcf`.
  (e.g. Potato = low point budget + 512 cubes + 1 PCF tap; Ultra = 2048 cubes, all point
  lights, 8–16 PCF taps. Cube PCF taps trade frame time for edge softness at no VRAM cost,
  so they can soften a low-resolution tier without paying the memory of a higher one.)
- **Extras** — `r_emissiveSurfaces`, `r_useSoftParticles`, `r_gl3ReflectionScale`.

### Design notes / open questions

- A preset is a **starting point, not a lock** — it writes the cvars and leaves them
  user-overridable. Consider showing a "Custom" state once any slider is touched after
  applying a preset.
- Anchor each preset to a rough **GPU-time budget** so the scale means something; calibrate
  with `r_gl3GpuTime` on a reference scene.
- Keep **"faithful" reachable**: one preset (or an explicit toggle) should map to
  source-accurate = enhancements off / most-vanilla.

### Calibration data (2026-07-27, GTX-1070-class GPU, busy scene, 12.24 ms baseline)

Subsystem cost, from an A/B `r_gl3GpuTime` sweep — useful for budgeting the presets:

| subsystem | cost | share |
|---|---|---|
| interactions (per-light lighting) | ~5.12 ms | 42% |
| SSAO (GTAO + normal pass) | ~4.34 ms | 35% |
| base (depth prepass, ambient, shadow-gen, post) | ~2.78 ms | 23% |
| soft particles | ~0.04 ms | ~0% |

Notes: shadow **mapping** is a net win here (~1.3 ms cheaper than the stencil fallback), so
it isn't a cost to cut. SSAO is the most preset-sensitive knob — half-res 6/1 vs full-res
with more samples spans a wide range, so it should drive most of the difference between
tiers.

## Bake AO for every possible asset  **[DONE — characters/props/weapons shipped 2026-07-28]**

Shipped as three loadable pk4s in the base dir: `z_baked_ao.pk4` (characters/monsters, 346 maps),
`z_baked_ao_props.pk4` (mapobjects props, 1724), `z_baked_ao_weapons.pk4` (weapons view+world+static,
149) — ~2219 8-bit-grayscale maps, ~98 MB total. Baked via the now-multithreaded baker through
`./bake_ao.sh` (isolates fs_configpath so the headless run never clobbers video/sound cvars). The
original plan/considerations kept below for reference.


The occlusion-map baker (docs/occlusion-maps.md) is proven on individual props (file cabinet,
barrels). Next, do a **mass bake** so the generated/aomaps tree covers everything, rather than
baking assets one at a time as they come up.

Goal: one command (e.g. `bakeAOAll`, or `bakeAOFolder models/mapobjects`) that walks every
bakeable static model and writes `generated/aomaps/<model>_s<N>.tga` for each drawn surface.
Because keying is per model+surface (skin/material independent), one pass covers all skins and
all instances — no per-map or per-skin work.

Scope / considerations when we do it:
- **Include:** static props under `models/mapobjects/**` (`.lwo`/`.ase`). These are the win.
- **Exclude:** MD5 characters (needs bind-pose support — separate TODO), the static world BSP
  (gated off at runtime anyway), and anything with only tiling/overlapping UVs (bake is inert
  there — the modular furniture; harmless but wasted files).
- **Uppercase `.ASE` — NO fix needed (verified 2026-07-28):** an earlier note claimed
  `bakeAOFolder` misses uppercase `.ASE`. Not true — `GetFileList` matches extensions with
  `idStr::Icmp` (case-insensitive) for pk4 files, and the engine lowercases the model name, so
  `turinal.ASE` bakes fine → `turinal_s0.tga` (empirical: `bakeAOFolder models/mapobjects/washroom`
  reported "baked 14 of 14" including the one `.ASE`). Do **not** add `"ASE"` to the exts array —
  it would double-list (hence double-bake) every `.ase`/`.ASE` file.
- **Cost:** was ~10–35 s per model at 256 rays. **Baker is now multithreaded** (per-texel across
  all HW threads, `r_occlusionMapBakeThreads`, ~16.5× on a 24-thread Zen 5), so the full
  `models/mapobjects` pass at 128 rays is ~10 min, not an afternoon.
- **Shipping:** decide whether to commit/ship the generated tree (pk4) or leave it to users /
  the lazy `r_occlusionMapsAutoBake` path. A shipped tree = zero first-load hitch.
- **Dedup:** a folder may hold model variants sharing geometry; per-model keying still gives each
  its own file (fine, just disk). No correctness issue.

Not urgent — the runtime auto-loads whatever exists and falls back cleanly, so this can be a
single batch session whenever we want full coverage.

## Phase 4 — investigate Vulkan / GPU-accelerated AO baking  **[FUTURE — investigate]**

The CPU baker is now multithreaded (`r_occlusionMapBakeThreads`, ~16.5× on a 24-thread Zen 5), but
it still ray-casts on the CPU — the full props pass is ~16 min, characters were ~80 min. A GPU path
could drop that to seconds / tens of seconds and make iterative re-bakes (tuning rays/contrast/detail)
painless. Tie this to the Vulkan raster port (docs/vulkan-port.md) — do it once that backend's
device/allocator/pipeline infra exists to reuse.

Approaches, in rough order of payoff vs. effort:
- **Vulkan compute port of the current grid ray-cast.** Upload the triangle soup + uniform grid (or a
  BVH) to SSBOs, dispatch one invocation per covered texel, and port `AO_TraceNearest`, the
  cosine-weighted Hammersley hemisphere, distance falloff, contrast, and the bump-map cavity term to
  GLSL. Runs on any Vulkan 1.1 GPU. Most reuse of the existing algorithm.
- **Hardware ray tracing** (`VK_KHR_acceleration_structure` + `rayQueryEXT` in a compute shader): build
  a BVH over the model's triangles and trace the hemisphere rays in hardware. Ideal on RT-capable GPUs
  (the dev box is an RTX 30-series / GA102). Keep the compute or CPU path as the fallback for non-RT
  hardware.
- **Texture-space rasterization for the per-texel setup:** render the mesh with position = UV to emit
  object-space position + normal into a G-buffer (the classic GPU lightmap/AO-bake trick), then the
  compute/RT pass integrates visibility per texel — replaces the CPU UV rasterizer in `AO_BakeSurface`.

Why it fits Phase 4 specifically:
- Reuses the Vulkan device/allocator/pipeline plumbing from the raster port instead of standing up a
  second GPU stack.
- A Vulkan **compute** baker can run **truly headless** (offscreen compute queue, no window/surface),
  which is cleaner than today's `xvfb` + GL-client hack — and it sidesteps the config-clobber gotcha
  entirely (no full client startup writing `dude.cfg`). It could finally become the standalone
  display-free `aobake` tool that was deferred (see docs/occlusion-maps.md).

Keep in mind:
- **Determinism:** the CPU baker is byte-deterministic (verified: serial == 24-thread, even SSE2 vs
  `-march=native`). GPU FP + parallel-reduction ordering may not be bit-identical run-to-run or vs the
  CPU. Fine for shipped assets (bake once), but note it if reproducibility ever matters.
- **Keep the CPU baker** as the portable fallback (non-Vulkan / non-RT users, and the lazy
  `r_occlusionMapsAutoBake` path).
- **No format/keying/runtime changes:** same `generated/aomaps/<model>_s<N>.tga` output, same resolver.
  This is purely a faster *producer* — the consumer side is untouched.

Not urgent: the CPU baker already covers the shipped asset set. This is an iteration-speed (and,
via RT, potentially higher-ray-count quality) investment for when the Vulkan backend matures.

---

## PBR materials (roughness / metalness)  **[built through Phase C]**

See **docs/pbr-materials.md** for the full design + as-built state. Summary:

- Opt-in `r_pbr` GGX/Cook-Torrance path in `interaction.frag` (metalness workflow,
  Schlick Fresnel, Toksvig per-texel roughness from the already-unnormalized normals).
  Vanilla `r_shading` paths untouched; Potato stays an exact vanilla frame.
- **No per-texel conversion of stock assets** — a Doom 3 specular map can't be uniquely
  converted to roughness. Instead: per-material *scalars* from an offline classifier
  (`tools/pbr_classify.py`) over material names/paths + declared surftypes (sparse:
  ~537 of ~5,148 stock materials declare one) + normal-variance/specular-luminance
  estimates clamped to per-category bands; hand-override file on top; ships as an
  optional pk4 overlay.
- Phases: **A** GGX branch w/ defaults **[built]** → **B** classifier + table +
  per-category Developer-tab sliders **[built]** → **C.1** light-glow env floor for
  metals (`r_pbrEnvScale`) **[built]** → **C.2** screen-space reflections (`r_ssr`,
  sharp-only v1, docs/ssr.md) **[built, pending verification]** → **C.2.1** glossy
  blur + temporal → **D** optional `roughnessmap`/`rmamap` keywords + lit
  redefinitions for splat decals/eyeballs/weapons.
- Pairs with `r_hdr` (GGX highlights are the >1 energy the RGBA16F buffer exists for).

---

## Antialiasing (post-resolve SMAA now, TAA later)

See **docs/antialiasing.md** for the full sketch. Summary:

- The built-in "Antialiasing" slider = `r_multiSamples` = backbuffer MSAA. It IS active in the RHI
  path (scene renders to backbuffer 0), but only fixes silhouette edges — never the specular/normal-map
  shimmer that is Doom 3's signature aliasing.
- **Plan:** keep MSAA for legacy OpenGL; reuse the same `r_multiSamples` value as an AA-quality knob
  for an RHI-native post-resolve pass.
- **SMAA 1x first** — cheap, no temporal risk, no ghosting. 3 fullscreen passes (edges/weights/blend)
  reusing `RB_RHI_DrawFullscreen` + `CreateRenderTarget`/`BeginTargetPass`, scene captured via
  `CopyFramebuffer`, run before the 2D/GUI composite. Needs SMAA AreaTex/SearchTex LUTs + 3 new
  shaders registered in `gl3BootPrograms[]`.
- **TAA later** — the specular-shimmer fix. ~80% wired via the temporal-SSAO path (ping-pong history,
  camera reprojection, neighborhood clamp in `ssao_temporal.*`). Adds sub-pixel projection jitter +
  a color resolve pass. **Real prerequisite: per-object motion vectors** — SSAO reprojection is
  camera-only, so moving geometry/weapon will ghost without them. Keep SMAA 1x as the non-temporal
  menu alternative.

---

## Codebase independence  **[planned]**

A small phase to break free from third-party content and stale identity so a clean
DUDE build stands on its own — engine-side settings-menu entry point (no community
GUI mod), DUDE-authored widescreen GUIs (the anchor system is already in-engine),
unified `dude` config/save dirs, and a renamed `dudeSettings` command (aliased).

Full plan: **[codebase-independence.md](codebase-independence.md)**.
