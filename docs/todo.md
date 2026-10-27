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
| RT reflection trace resolution (r_rtReflResScale) | — | — | — | — | — | 1/2 (RTX) |
| volumetric lighting (r_volumetrics) | — | — | — | — | — | on (RTX) |
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
- **SHIPPED 2026-09-28 — FSR2 sub-native upscaling** (`r_fsrScale`, render scale < 1.0, the scene/HUD
  reorder that replaced the Native-AA copy-back, and a "Upscaling" menu group under Display: Quality 67% /
  Balanced 59% / Performance 50% / Custom + a mirrored RCAS sharpness slider). User-verified (1.0 pixel-
  identical, 0.67 correct) and merged to master (`fa4f6542`). Presets left untouched — it's a manual knob
  whose real customer is the RT tier's per-pixel ray budget. Full record:
  [fsr-temporal-pipeline.md](fsr-temporal-pipeline.md) § "Sub-native upscaling — `r_fsrScale`".

---

## Binary model cache  **[SHIPPED 2026-09-28 — `r_binaryModelCache`; verified, ~0.5 s. The real lever was the height fields, below]**

**Measured first (commit `79961c6d` split the load phases), and the measurement overturned the
plan below:** on hell1 the render-model parse is **6.8 s of an 11.8 s load** (551 models, ~12.4 ms
each), while the collision conversion this section was named for is **153 ms** and `.aas` parsing
42 ms. So the cache built is a **binary render-model cache** (BFG-style, the post-`FinishSurfaces`
surfaces to `generated/*.bmodel` in the dude folder, keyed on source timestamp + format/engine
version + struct sizes + welding cvars), not a collision cache — full design and format in
**[binary-model-cache.md](binary-model-cache.md)**. The collision cache is not worth building on
its own; the `.aas` sidecar and the warm-up-frame tuning (`settle=0`) are dead. Original plan
kept below for reference.

**Then the model cache verified at only ~0.5 s saved, and the next split found the real lever:** the
"model parse" was 8.5 s of *material* parsing — every bump material was generating its parallax and
tessellation height fields (a 2K decode + a 64-iteration Poisson solve, twice) *inside* the material
parse, because they registered via the immediate-generate `ImageFromFunction`. **SHIPPED 2026-09-28
(verified: hell1 cold load 11.3 s → 2.46 s, 305 height fields produced on the workers, visually unchanged):**
`ImageFromLevelGenerator` defers them to `EndLevelLoad`, produces them on the parallel-loader workers, and
purges them like file images (`r_deferLevelGenerators`). The MD5 cache is dead
(all 551 models' geometry costs 118 ms). Full account: **[load-time-decl-parse.md](load-time-decl-parse.md)**.

### Original plan: collision / model binary "generated" cache  *(superseded above)*

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

## Decals: reflections on wet blood, wet wounds on characters  **[parked 2026-09-19 — nice to have]**

The decal work of 2026-09-19 is closed and shipped on master (see
[readme-changes.md](readme-changes.md) 1.5–1.7: decal sheen control, the decal surface mask,
the wet blood film — user-verified on stock and texture-pack blood). Two follow-ups were
deliberately left out; neither is scheduled.

**1. Reflections on wet blood.** The film is *direct light only*: each blood decal is drawn
once more per overlapping light through the regular interaction path, so every glint is the
highlight of a real light source. Screen-space and RT reflections never see it. They work
from the opaque prepass's per-pixel data — depth, the normal G-buffer, the rough/metal buffer —
and a blood decal is a translucent overlay that writes none of it, so to the reflection pass a
pixel under blood is *the wall*. Worse, the decal surface mask treats blood like dirt for the
surface underneath (rougher, non-metal), so on a polished floor the reflection **fades out**
under a pool: glints from lights, but no mirror image of the room or a monster.

- *What it takes:* a small "decal G-buffer" pass after the opaque prepass that writes the
  film's **own** surface into those targets at blood texels, coverage-weighted: the relief
  normal (`RB_RHI_BloodRelief`, rotated tangent → view space) and a low roughness. Reflections
  composite at the translucent split, *after* the decals are down (`RhiBackend.cpp`, the
  `GetSort() > SS_DECAL` break in the shader-pass loop), so a reflection on blood lands on top
  of it at full brightness — no transmittance compensation needed, unlike the film's glint.
- *The trap, already hit once:* lowering roughness **without** replacing the normal makes RT
  reflections march the wall's bumped normal and draws its grooves across the splat
  (readme-changes 1.7, "Dead end"). Normal first, roughness second — or not at all.
- *Worth it?* Medium work, about the size of the decal mask pass. In Doom 3's dark rooms the
  light glints carry the wet look; mirror reflections would show mainly on large floor pools
  near something bright. The texture packs' cube-map stage (a generic environment texture, not
  the scene) already fakes it on map-placed blood, under `r_decalBloodSheen`.

**2. Wet wounds on characters.** Blood wounds on bodies are overlays
(`idRenderModelOverlay`), and the film skips them. Less is missing than it looks:

- overlay vertices already carry the base mesh's normal and tangents (`ModelOverlay.cpp`, added
  for the tessellation work) — they just never set `tri->tangentsCalculated`, which is the
  film's geometry gate. The tangents follow the *skin's* UVs rather than the wound's
  projection; for an isotropic relief nobody can tell.
- the wound materials are not "blood" to `R_DecalIsBlood`: `textures/decals/hurt02` & co.
  (`mtr_wound_flesh`) have neither the `blood` flag nor a blood-like name. They are filter
  blends, which the film already handles (mode 2). Needs an explicit list, or a flesh-wound rule.
- on tessellated characters the blend pass tessellates the overlay so it follows the displaced
  mesh, and the film refuses surfaces whose blend pass tessellates. The film draw would have to
  tessellate identically — the interaction shader has a tess variant, so it is plumbing
  (`pd.tessellate` + `RB_RHI_SetTessParms` in the film path), not new shaders.
- then judge the look: a wound is small and sits on skin that already has a wet PBR category
  (`skin`, wetness 2.0), so the gain may be slight.

---

## RT reflections: shadow rays at the hit (RR16d)  **[parked 2026-09-20 — nice to have]**

RT reflections are closed for now with RR16c (`r_rtReflLightFrustum`: each real light shading a
reflection hit is confined to its projection frustum / light box). The one remaining lighting
step, **RR16d**, is deliberately left out: per contributing light, trace hit → light against the
TLAS and zero the term when occluded, so reflected surfaces gain contact shadows and a light
behind a wall stops leaking into the reflection. It is the dominant new cost of the RR16 family
(a ray per light per reflective pixel), so it would be its own toggle (`r_rtReflShadows`, off by
default, Ultra Nightmare only at most). Reflections were judged excellent without it; the RR12
on-screen reuse already carries real shadows wherever the reflected surface is on screen. Plan:
[rtx-reflections.md](rtx-reflections.md), RR16.

---

## Fidelity backlog (no new assets)  **[proposed 2026-09-19 — nothing scheduled]**

From the fidelity review after the decal work. The reflection items became a staged plan —
RR12 lit-scene reuse at the RT hit, RR13 roughness-driven glossy for the RT tier, **RR14 specular
occlusion (DONE + user-verified 2026-09-19, `r_specOcclusion` 0.7)**, RR15 reflections on wet blood —
sequenced around the open RR5 work in
**[rtx-reflections.md](rtx-reflections.md)** ("Fidelity follow-ups"). The rest, in rough order of
gain per effort; every one is an opt-in toggle, preset-wired only after a visual pass:

- **Screen-space contact shadows** (GL3 + VK, raster tiers). A short depth-buffer march toward each
  light inside the interaction pass grounds what shadow maps are too coarse for — pickups, small
  props, weapon detail, feet. *Not* for Ultra Nightmare: `r_rtAllLights` already traces a hard ray
  per light there, so this is a Medium→Nightmare feature. Cost scales with lights × lit pixels
  (it runs per interaction), so budget ~8 steps and gate by light radius. Fidelity: purely additive
  darkening, but a look change → toggle.
- **Skin / flesh shading.** A `skin` PBR category already exists (wetness 2.0); give it wrap
  lighting or a cheap pre-integrated SSS term so faces and Hell flesh stop reading as painted
  plastic under GGX. Classifier-driven, no assets. Small shader change; the risk is taste, not code.
- **Volumetric light shafts** for projected/spot lights — froxel or per-light raymarch through the
  existing shadow maps (or RT rays at UN), modulated by the existing fog volumes. Biggest mood win,
  biggest departure from stock, real cost: top presets only, strictly opt-in, needs its own plan doc.
  **Plan written 2026-09-20: [volumetric-lighting.md](volumetric-lighting.md)** — two routes (quarter-res ray
  march reusing the RR16 light buffer + ray queries, vs a true froxel grid needing new RHI 3D/storage
  images). **Route A SHIPPED 2026-09-20** as `r_volumetrics` (on at Ultra Nightmare since 2026-09-20, off elsewhere) (+0.5 ms spots only, ~+1.2 ms all lights
  @1440p); froxels / gobo textures / fog boost / raster tiers remain open there.
- **RT GI (one bounce)** — already H6 in [rtx-hybrid-roadmap.md](rtx-hybrid-roadmap.md).

---

## Codebase independence  **[partly done]**

A small phase to break free from third-party content and stale identity so a clean
DUDE build stands on its own.

**Done:**
- Unified `dude` config/save dirs on all three platforms with one-time legacy-`dhewm3`
  migration, and renamed the settings command `dhewm3Settings` → `dudeSettings` (legacy
  alias kept). Commit 060f1ade, shipped in 0.9.5.
- Windows log file `dhewm3log.txt` → `dudelog.txt` (matches POSIX `dudelog.txt`).

**Menu independence — DEFERRED until after H7 ("The Dark Lord Cometh") lands.**
The DUDE Settings button already works, but it rides a `base/guis/mainmenu.gui` that is
*derived* from id's Doom 3 menu via the CstDoom3/WideGuis rework (deliberately untracked,
pak-only). Making it IP-clean = finishing the from-scratch re-author in
[base/guis/dude/mainmenu.gui](../base/guis/dude/mainmenu.gui) (screens 1–2 done). Four
points remain:
1. Author the remaining screens from scratch (Options/System pages, New Game/difficulty,
   Load, Multiplayer, Mods/Updates/Credits), referencing id assets at runtime only;
   verify each visually against the current menu via `testGUI guis/dude/mainmenu.gui`.
2. Use the canonical `exec "dudeSettings"` in the clean file (not the legacy alias).
3. Promote & swap: rename `guis/dude/mainmenu.gui` → `guis/mainmenu.gui` (git-tracked),
   drop the untracked derivative, rebuild `zz_dude_menu.pk4` from the tracked source.
4. Repeat the clean re-author + swap for RoE (`d3xp`), rebuilding `zz_dude_roe_menu.pk4`.

Also still open from the original plan: ship a DUDE-authored widescreen GUI set (the
anchor system is already in-engine; only the `.gui` text is third-party/id-derivative).

Full plan: **[codebase-independence.md](codebase-independence.md)**.

## Emissive mode performance  **[backlog 2026-10-27 — nothing scheduled]**

Context: emissive mode (`r_rtEmissiveLighting 1`) was made playable on 2026-10-27: `r_rtEmissiveSamples` default 2, the
analytic emitter RIS target on (`r_rtEmissiveCheapTarget 1`), the ReSTIR DI winner shaded once, the gather's side data
ping-ponged instead of copied, the boost picks' SH-mode target fixed, the cache resolve over touched slots only
(docs/rtx-restir-di.md and docs/rtx-irradiance-cache.md, "Perf" sections). Reference capture (user, mars_city1 1440p,
emissive on): gather 24.7 / 13.6 / 8.4 ms at 10 / 4 / 1 samples before those changes. Estimates below are estimates.

- **Light buffer + emissive table in VRAM** — the scene-light rows (`RB_RHI_GatherSceneLights`, a 3-slot ring) and the
  emissive table / CDF are `BU_STORAGE`: HOST_VISIBLE | HOST_COHERENT, so on a discrete GPU they may live in system memory
  and every gather / grid-fill / bounce / gradient read that misses L2 crosses PCIe. First a one-flag A/B (prefer
  device-local + host-visible, ReBAR); if it pays, stage the uploads (the mover pass's in-place slice writes then need the
  same path). Est. 0-0.5 ms, unmeasured. Effort S to test, M to stage.
- **`lightgrid.fill`** — ~2 ms on emitter-dense maps (mars_city1; 0.09 ms in hell1): one thread per cell (4 cascades x 28^3
  = 87,808) loops over EVERY row of the light buffer, placed + emitter rows, so cost = cells x rows. Ideas: cull rows per
  cascade on the CPU (a row only enters cells within its cull sphere - bin rows by cascade/region first), or a two-level
  fill (coarse bins of rows per 4^3 cell block). Must keep the deterministic top-N (the stable grid). Effort M.
- **Bounce hit: RIS-pick 1-2 of the cell's candidates** instead of shading all up-to-32 (rtgi_bounce.glsl, the combined-GI
  bounce at the hit). Est. 0.3-0.8 ms. Noisier GI, so opt-in, and a flicker A/B is required (GI half-rate flickered
  through NRD's anti-lag in 2026-10-14 and was reverted). Effort M.
- **Gradient pass** (`restir.di.grad`, 0.6-1.25 ms; was 0.44 on 2026-10-13, growth not explained) — first an A/B of
  `r_rtReStirDIGradientStratum` 3 vs 4; then a compute rewrite (one thread per gather pixel + a subgroup max, one re-shade
  per stratum) for the serial 9-pixel scans. A stratum change alters the confidence granularity: opt-in. Effort S / M-L.
- **CPU: emissive table mover upkeep** — any emissive mover move regenerates EVERY emitter row (`R_RtEmissiveBuildRows`,
  O(all triangles), plus a linear emitters.cfg / override scan per row) and any flux change re-uploads the whole CDF.
  Incremental rows (only the moved group's), a hashed emitters.cfg lookup, a CDF slice upload. No fps while GPU-bound;
  helps hitches on mover-heavy maps. Read `r_rtEmissiveTableDebug 1`'s "mover upkeep" line first. Effort S-M.
- **Denoiser knobs** (tuning, not code): `AntiFirefly` off (NRD: ~7-10% of the denoise), Atrous 2 instead of 3. Fidelity
  choices for the user.
- **Not to re-propose** (tried and rejected): GI half-rate, DI reservoir reworks (two-reservoir emitter split, K-pick
  emitter reservoir), fewer grid slots, residual stochastic slots, MCPG guiding, Q2RTX per-cell emitter lists, and the
  **cache resolve over a touched-slot list** (2026-10-27, branch `perf/rtgi-cache-touched-resolve`, NOT merged): the
  first writer of a frame appended each slot to a list and the resolve walked it instead of all 2^21 slots. `rtgi.cache`
  measured 0.48 ms at 1 sample against 0.37-0.43 ms for the sweep (user, mars_city1): the sweep's early-outs are cheap
  coalesced reads; the cost is the touched slots' own work, which a list makes scattered, plus one contended counter
  atomic per first touch in the gather. (The `rtgi.cache` GPU scope also seems to absorb the tail of the gather before
  it: the sweep read 0.4 / 0.65 / 0.87 ms at 1 / 4 / 10 samples - compare at equal settings.)
