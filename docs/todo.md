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
| SSAO res scale | — | — | 1/2 | 2/3 | 3/4 | 4/5 |
| SSAO directions / steps | — | — | 2 / 4 | 3 / 6 | 4 / 8 | 5 / 10 |
| SSAO temporal trade (4/6 + temporal) | on* | on* | on | on | on | off |
| SSAO radius (world) | — | — | 48 | 48 | 48 | 48 |
| SSAO depth-mip accel | — | — | on | on | on | on |
| SSAO normal G-buffer | — | — | off (depth reconstruct) | on | on | on |
| shadow 2D / cube res | — | — | 512 / 512 | 1024 / 1200 | 2048 / 2048 | 2048 / 2048 |
| cube PCF taps | — | — | 5 | 6 | 8 | 10 |
| point-light budget | — | — | 16 | 64 | 96 | 128 (all) |
| shadow size-scale | on* | on* | on | on | on | on |
| size-scale pivot radius | 380* | 380* | 380 | 380 | 480 | 480 |
| emissive light cap | — | — | 16 | 24 | 32 | 48 |
| film grain / chroma | off | 0.05 / off | 0.05 / 0.2 | 0.05 / 0.2 | 0.05 / 0.2 | 0.05 / 0.2 |
| film grain size | 1.5* | 1.5 | 1.5 | 1.5 | 1.5 | 1.5 |
| post antialiasing | off | SMAA | SMAA | SMAA | SMAA | SMAA |
| render interpolation (com_interpolate) | off (stock 60Hz) | on | on | on | on | on |
| RT ambient occlusion (r_rtao) | — | — | — | — | — | on (RTX) |
| RT shadows for all lights (r_rtAllLights) | — | — | — | — | — | on (RTX) |
| RT shadow blur (r_rtShadowBlur) | — | — | — | — | — | on (RTX) |
| RT shadow blur staggered refresh (r_rtShadowBlurStagger) | — | — | — | — | — | on (RTX) |
| reflection scale | 1.0 | 1.0 | 0.7 | 0.7 | 0.7 | 0.7 |

(Potato/Low carry the cheap Medium SSAO/shadow sub-params under the off toggles so a
manual feature flip from those tiers stays affordable. `*` = inert on that tier (stencil
shadows in place of maps / grain intensity 0) but carried for determinism. `†` = dormant while PBR supersedes the specular model on
High and up; still applied for determinism. Sub-Ultra tiers carry SSR march resolution
1.0 so a hand-enabled SSR runs at the full-res default. The SSAO temporal trade
(`r_ssaoTemporalTrade`, docs/ssao-perf-optimization.md Phase 4) overrides the direction/step
columns with a fixed 4/6 + temporal accumulation on the tiers where it's on; Nightmare and
Ultra Nightmare keep the brute-force 5/10. Ultra Nightmare — otherwise Nightmare's column
plus the RT toggles — additionally runs the AO buffer at full resolution, vs 4/5 here.)

Shadow size-scaling (`r_shadowMapSizeScale`) stays **on at every tier**: it distributes the
per-light resolution budget by light radius (a light at the pivot radius gets the tier's base
cube/2D res, bigger lights get more, smaller ones less). The tiered **base resolution** is the
primary lever; the **pivot radius** (`r_shadowMapSizeScaleRadius`) is a secondary perf/sharpness
lever — *raising* it pushes the biggest lights down a resolution tier. Ultra/Nightmare use **480**
(vs 380 on Medium/High) to drop their largest cube + spot lights from a 2048 face to 1024 — a real
shadow-render saving in big "sun"-light areas (user-verified) at slightly softer big-light shadows.
It only bites on the 2048-base tiers: Medium/High's small bases (512 / 1200) already floor big
lights at ½× regardless of the pivot, so raising it there would be a no-op.

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
  sharp-only v1, docs/ssr.md) **[built, pending verification]** → **C.2.1**
  temporal accumulation **[built]** + glossy blur **[deferred — gated on a real
  sighting, see docs/ssr.md §5]** → **D** optional `roughnessmap`/`rmamap` keywords + lit
  redefinitions for splat decals/eyeballs/weapons.
- Pairs with `r_hdr` (GGX highlights are the >1 energy the RGBA16F buffer exists for).

---

## Antialiasing (post-resolve FXAA + SMAA shipped, TAA later)

See **docs/antialiasing.md** for status + design. Summary:

- The built-in "Antialiasing" slider = `r_multiSamples` = backbuffer MSAA. It IS active in the RHI
  path (scene renders to backbuffer 0), but only fixes silhouette edges — never the specular/normal-map
  shimmer that is Doom 3's signature aliasing.
- **FXAA — shipped** (`r_rhiAA 1` + `r_fxaaStrength`): one cheap pass whose subpixel low-pass also
  damps shimmer, at some texture softening.
- **SMAA 1x — shipped 2026-08-02** (`r_rhiAA 2`): vendored reference implementation, three passes on
  both the LDR and HDR rails; sharper pattern-classified edges, texture interiors untouched, no
  shimmer treatment (Toksvig covers that on PBR tiers). Kept alongside FXAA until TAA exists, then
  re-evaluate dropping FXAA.
- **TAA — SHIPPED as FSR2 Native-AA (R1, 2026-08-18)**: `r_fsr` on Vulkan (temporal accumulation +
  RCAS + auto-reactive deghosting), default in the Nightmare preset, SMAA kept as the GL3/opt-out
  fallback. Full record: [fsr-temporal-pipeline.md](fsr-temporal-pipeline.md).
- **PARKED — FSR2 sub-native upscaling** (render scale < 1.0, `fsrQuality` presets, the scene/HUD
  reorder that replaces the Native-AA copy-back): pointless while the game is CPU-bound; the
  customer is the RT era's per-pixel ray budget (67% scale ≈ half the rays). Work list in
  [fsr-temporal-pipeline.md](fsr-temporal-pipeline.md) § "PARKED FOLLOW-UP".

---

## Collision / model binary "generated" cache  **[parked 2026-08-18 — next load-time lever]**

With `r_parallelImageLoad` shipped (image phase ~10x faster, user-verified), the biggest
remaining load-time chunk is the entity-populate **cacheMedia** slice (~2.8s on hell1):
per-prop collision models and the render models they drag in. Two deterministic,
re-done-every-load costs:

- **Runtime CM conversion** (`idCollisionModelManagerLocal::LoadRenderModel`,
  CollisionModel_load.cpp): stock props ship no `.cm`, so every ASE/LWO moveable is
  converted at load — per-tri winding + vertex/edge hash weld + axial BSP build — and the
  result is discarded at map end.
- **Text parsing everywhere**: ASE/LWO render models and even the map's cached `.cm` are
  idLexer text parses.

**Plan (BFG-style, engine-only):** (1) measure the split first — accumulating timers
(render-model parse vs CM conversion) printed with the existing `load phases (msec)` line;
(2) serialize built `cm_model_t` (and, if the split says so, binary render models) to the
writable dude folder, CRC/timestamp-gated exactly like the map `.cm` already is.
Parallelizing this phase was investigated and declined (Increment 3, thread-safety of
renderModelManager/CM manager/allocators for ~2-3x); the disk cache gets more for less.
Expectation: attacks the ~2.8s slice; the ~4.4s spawnDefs floor remains.

---

## GPU skinning: recover the CPU-skin win via skin-on-demand  **[parked 2026-08-18 — low priority]**

**Why it's parked:** the CPU-position-skin strip (`r_gpuSkinStripCpu`, "Milestone D") was
GPU skinning's real CPU win — but it was *removed* (commit 4ab52781) because it broke monster
hit detection. A living monster's player-hit collision **is its animated render mesh**
(`idActor` combat clip = render-model handle, `CONTENTS_RENDERMODEL`; player attacks trace
`MASK_SHOT_RENDERMODEL` → `idClip::TraceRenderModel` → `idRenderWorldLocal::ModelTrace` →
`R_LocalTrace` reads `tri->verts[].xyz` directly, tr_trace.cpp:43). Stripping left those verts
at bind pose → invulnerable monsters. So the CPU position skin now always runs, and with GPU
skinning on it runs **redundantly** with the GPU skin (CPU for the trace/bounds/fallback, GPU
for the draw) — the per-frame CPU-skin cost is back.

**The recovery (proper design): skin on demand.** Let the GPU own the per-frame skin again
(re-strip the CPU skin for the *draw*), but skin a monster's verts **just-in-time inside
`ModelTrace`** when a hit is actually tested — hits are far rarer than every-frame-every-monster,
so this recovers the strip's savings while keeping correctness. Sketch: keep the `cpuSkinStripped`
signal; in `R_EntityDefDynamicModel`/`ModelTrace` (RenderWorld.cpp:~1083), when the fetched
dynamic model is stripped, run `TransformVerts` for the traced surface(s) into scratch (joints
are on `renderEntity.joints`) before `R_LocalTrace`. The strip cvar/menu were retired (9896868c);
this phase would reintroduce a *safe* strip gated on the on-demand path existing. Vestigial infra
kept for it: `idMD5Mesh::CalcBoundsFast` + the per-joint reach precompute + `R_GpuSkinProfileAddStrip`.

**Worth it only for weak-CPU targets.** The strip's measured benefit was ~+3% fps *and only when
CPU-bound* (weaker GPU / low presets); on a strong CPU + discrete GPU it's in the noise. GPU
skinning stays valuable as **infrastructure** regardless (GPU-resident posed geometry for
deform-once tessellation and future animated-RT-BLAS refits), just not as a CPU-time win until
this lands. Full context: memory [[gpu-skinning-monster-invuln-bug]].

---

## Codebase independence  **[planned]**

A small phase to break free from third-party content and stale identity so a clean
DUDE build stands on its own — engine-side settings-menu entry point (no community
GUI mod), DUDE-authored widescreen GUIs (the anchor system is already in-engine),
unified `dude` config/save dirs, and a renamed `dudeSettings` command (aliased).

Full plan: **[codebase-independence.md](codebase-independence.md)**.
