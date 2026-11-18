# Parallax occlusion mapping (world surface relief) — design

Design reference for **per-pixel parallax occlusion mapping (POM)** on the Vulkan (RHI)
backend. Same enhancement framing as the rest of the M7 suite (HDR in
[hdr-pipeline.md](hdr-pipeline.md), SSAO in [ssao-gtao.md](ssao-gtao.md), SSR in
[ssr.md](ssr.md), tessellation in [tessellation.md](tessellation.md)): built on the RHI,
**opt-in and off by default**, driven by a config cvar and preset-gated.

**Complements tessellation, does not overlap it.** GPU tessellation
([tessellation.md](tessellation.md)) is gated to **skinned enemies + static props** and
reshapes their *silhouettes*. It deliberately leaves the **BSP world** — walls, floors, metal
panels, rock, grating — flat. That flat world is the majority of what the player looks at in
Doom 3's corridors, and it is exactly where parallax pays off: apparent depth on masonry,
plating and rock at grazing angles. The two features target disjoint surface sets by design.

## Why this is high-ROI here (the data already ships)

The tessellation doc states Doom 3 "ships no height/displacement maps." That is true in the
sense that no height map is *bound to a surface for rendering* — but it is **not** true that
the height data is absent. Doom 3 bakes its normal maps **from authored height maps at load
time** via the `addnormals(local, heightmap(x_h.tga, scale))` image program
([`Image_program.cpp:380`](../neo/renderer/Image_program.cpp),
[`R_HeightmapToNormalMap`](../neo/renderer/Image_program.cpp) at line 73). The height source
is consumed and discarded — but it exists on disk:

- **~2000** `heightmap(...)` references across the stock `.mtr` files.
- **~468** authored `_h.tga` / `_bmp.tga` height textures shipped in the paks (world *and*
  characters).

So parallax can be fed the **same authored height data the normal maps were baked from** — no
new art for a large fraction of surfaces. That is the unusual, decisive fact: most engines
retrofitting POM must invent height data or derive it badly from the normal map. We do not.

## The constraint — and why it is much softer than tessellation's

Interactions run with **`GLS_DEPTHFUNC_EQUAL`** against a separate depth prepass (zfill,
`DEPTHFUNC_LESS`) — see [`RhiWorld.cpp:54`](../neo/renderer/rhi/RhiWorld.cpp) and
[`RhiWorld.cpp:1012`](../neo/renderer/rhi/RhiWorld.cpp). For tessellation this was the whole
problem: moving vertices in the lit pass but not the prepass breaks depth-equal.

**POM does not move geometry.** It offsets *texture coordinates* per pixel in the fragment
shader and writes the **flat surface depth** unchanged. Therefore, in its standard form, POM
**does not touch the depth prepass at all** — zfill stays pixel-identical, `DEPTHFUNC_EQUAL`
is satisfied trivially, and no tess/patch pipeline is needed. This makes parallax a
substantially **lower-risk** feature than tessellation.

The residual constraint is *shading consistency*, not depth correctness:

- The UV offset must be applied in **every pass that shades the surface** — the per-light
  interaction pass ([`interaction.frag`](../neo/shaders/interaction.frag)) **and** the ambient
  pass ([`ambientlight.frag`](../neo/shaders/ambientlight.frag)) — or the ambient and lit
  contributions sample different points and the relief "swims" between them. Both must compute
  the same offset from the same inputs (deterministic, view-dependent is fine).
- **Do not write `gl_FragDepth`.** A "depth-correct" POM variant that offsets depth for true
  silhouettes/self-occlusion *would* diverge from the flat zfill and reintroduce the
  `DEPTHFUNC_EQUAL` break. We explicitly forgo it (see Non-goals). Silhouettes stay flat; where
  true silhouette relief matters we already have tessellation.

## What we already have in the shaders

The interaction path is nearly set up for this:

- **Tangent-space view vector already exists** — `var_TexViewVec`
  ([`interaction.vert:21`](../neo/shaders/interaction.vert), "view vector in tangent space").
  POM's ray direction is exactly this vector; no new varying or CPU plumbing for the common
  case.
- Bump/diffuse/specular are sampled from `var_TexBump` / `var_TexDiffuse` / `var_TexSpecular`
  ([`interaction.frag:115`](../neo/shaders/interaction.frag)). POM produces one offset that is
  added to all three (they share the surface's UV parametrisation).
- The normal map is **DXT5nm / RXGB** — the X component lives in **alpha**
  ([`interaction.frag:116`](../neo/shaders/interaction.frag)). Alpha is therefore **not**
  available to smuggle height into; parallax needs its **own** height sampler.

## Height-map plumbing (two paths, auto-capture preferred)

`R_HeightmapToNormalMap` converts height → normal and discards the height, so we must retain a
height texture as its own bound sampler. Two ways to get one:

1. **Auto-capture from `addnormals(...)` (recommended, gives stock coverage).** When a
   `bumpmap` stage's image program is `addnormals(local, heightmap(X, N))`, capture `X` (and
   `N`) during image-program evaluation and bind it as the stage's parallax height source
   automatically. This lights up the ~2000 existing references **with no material edits** —
   the same free-coverage trick that made occlusion maps inert-but-ready. Requires teaching the
   image-program parser to remember the inner `heightmap` operand instead of only emitting the
   normal.
2. **Explicit `parallaxmap` material keyword (override / new assets).** Mirror the
   `occlusionmap` shortcut ([`Material.cpp:1160`](../neo/renderer/Material.cpp)): a
   `parallaxmap <image>` stage binds a dedicated height texture, overriding auto-capture. For
   authored or DUDE-supplied relief where no `_h.tga` exists.

Height is a single channel (R). A per-material **scale** (the `N` from `heightmap(x, N)`, or a
`parallaxScale` keyword) drives apparent depth; clamp to a sane band so aggressive bakes don't
balloon.

## Shader design (POM)

In `interaction.frag` / `ambientlight.frag`, before the bump/diffuse/specular fetches:

1. `V = normalize(var_TexViewVec)` (tangent space, pointing toward the eye).
2. March the height field along `V.xy` scaled by `heightScale / V.z`, with a **step count that
   ramps by view angle** (fewer steps head-on, more at grazing) between `r_parallaxMinSteps`
   and `r_parallaxMaxSteps`.
3. Find the first intersection, then **one linear-interpolation refinement** between the last
   two samples (this is the "occlusion" in POM; a plain single-offset parallax swims and is not
   worth shipping — see Non-goals).
4. Emit `uvOffset`; add it to `var_TexBump`, `var_TexDiffuse`, `var_TexSpecular` for all
   subsequent fetches.
5. *(Optional, gated)* **Self-shadowing**: march from the intersection toward the light
   (tangent-space `var_TexLightVec`); if the height field occludes, attenuate the diffuse.
   Cheap, high payoff, separate cvar.

Distance/LOD: fade `heightScale → 0` with distance and disable the march beyond a range, so
far surfaces cost nothing and pop-free collapse to flat normal mapping.

## World-only (decided in-engine, 2026-08-04)

Parallax is applied to **static-world (BSP) surfaces only** — the render-time gate is
`entityDef->parms.hModel->IsStaticWorldModel()` in the interaction draw (mirrors the
occlusion-map path). Props and characters are excluded: POM assumes a locally flat surface
with a uniform tangent basis, and on small/curved/skinned meshes the offset smears and
deforms edges (observed on mapobjects). Those meshes are the tessellation feature's domain.
No per-material opt-in and no override cvar for now — kept deliberately simple.

## Surface gating (what parallax must skip)

- **Alpha-tested cutouts** (grates, fences, fans, foliage): simple/occlusion parallax swims
  badly on perforated surfaces and fights the alpha test. Detect via
  `Coverage() == MC_PERFORATED` / presence of an alpha-test stage and **exclude** them.
- **No height source**: surfaces whose bump map is a plain normal map (no `addnormals`
  heightmap, no `parallaxmap`) get **no** parallax — collapse to the current path. This is the
  common gate for a lot of tech/decal art and keeps behaviour identical there.
- **Deforming / scrolling / non-planar-parametrised** surfaces and skies: skip; the UV march
  assumes a stable tangent-space parametrisation.
- **Decals / projected overlays**: leave on the base surface only; do not POM the decal stage.

## Fidelity note

Opt-in enhancement, **off by default = bit-for-bit vanilla** (no height sampler bound, no march
compiled in via a shader permutation / spec constant). When on:

- **It is a departure from the original flat look** — walls and floors gain depth Doom 3 never
  showed. Deliberate, and consistent with the rest of DUDE; conservative default scale.
- **Silhouettes stay flat** (no `gl_FragDepth` write). At near-grazing angles on a straight
  edge the illusion thins; acceptable, and complemented by tessellation elsewhere.
- **Stencil shadows** are unaffected (CPU, from base geometry) — correct, since POM adds no
  geometry.
- **Interplay with SSAO / occlusion maps / SSR**: those sample the *un-offset* surface. Feeding
  them the POM-offset UV is a polish follow-up, not required for a correct first cut; note the
  minor inconsistency rather than block on it.

## cvars (proposed)

| cvar | default | meaning |
|------|---------|---------|
| `r_parallax` | `0` | master enable (bool) |
| `r_parallaxScale` | `1.0` (shipped: 0.10) | global multiplier on per-material height scale |
| `r_parallaxMinSteps` | `8` | march steps head-on |
| `r_parallaxMaxSteps` | `32` | march steps at grazing angle |
| `r_parallaxShadow` | `0` | self-shadowing march (bool) |
| `r_parallaxMinAngle` | `15` | grazing limit in degrees (built 2026-11-17, see "Pass parity") |
| `r_parallaxMaxDist` | tuned | distance past which POM fades to flat |

Preset-gated to the top tier (Ultra / "Ultra Nightmare", already reserved Vulkan-only —
see [quality-presets-plan] / [nightmare-naming-reservation]); off at and below the SMAA-default
tiers.

## Phasing

- **Phase A — height plumbing. DONE + verified.** Auto-capture the `heightmap` operand from the
  bump program (`R_ExtractHeightmapSource`), load it as an uncompressed height texture on the
  SL_BUMP stage (`shaderStage_t::parallaxImage` / `parallaxScale`), plus the `parallaxmap`
  keyword override. Gated on `r_parallax` at parse time (off = zero extra textures). Verified via
  `listParallaxMaps`: stock materials resolve the correct `_h`/`_b`/`_bmp` source with matching
  scales. No shader change yet.
- **Phase B — POM in the interaction pass. BUILT (pending in-engine verify).** Steep-parallax
  march + occlusion refinement (`parallaxUV()` in interaction.frag), offset applied to
  bump/diffuse/specular/occlusion UVs from the tangent-space `var_TexViewVec`. `r_parallax`,
  `r_parallaxScale`, `r_parallaxMinSteps`/`MaxSteps`. Height map bound on unit 11 —
  required extending the RHI sampler descriptor set from 11→12 bindings (DrawArgs.parallax,
  rhiVkUnits[12], the two Vulkan descriptor-write sites, and the `unit < 12` bind caps).
  Fragment-only, no `gl_FragDepth` → flat depth prepass and `DEPTHFUNC_EQUAL` untouched by
  construction. **Turned out backend-agnostic:** the shared shader + `SAMPLER_BINDING(11)`
  compiles on GL3 too (GL3.3 has ≥16 fragment units), so it is *not* Vulkan-locked the way
  tessellation is — see the corrected note below.
- **Phase C — gating DONE; ambient parity deferred.** Opaque-only exclusion added (alpha-tested
  grates/fences skipped: `Coverage() == MC_OPAQUE` in the interaction gate). Default
  `r_parallaxScale` settled at **0.175** (in-engine tuned). **Ambient-pass parity deliberately
  deferred:** it needs a tangent-space view varying threaded through
  `ambientlight.vert`/`.tesc`/`.tese`/`.frag` (four stages, for shader-linkage validity in the
  never-hit tessellated-ambient case), and at 0.175 the ambient-vs-direct UV mismatch is ~1% of
  a tile — invisible. Revisit only if a much higher scale ever makes the swim show. Distance LOD
  fade also deferred (perf-only; no complaint at current cost).
- **Phase D — self-shadowing DONE (pending verify); preset wiring TODO.** `r_parallaxShadow`
  (0..1, opt-in) marches from the hit point toward the tangent-space light vector
  (`parallaxShadow()` in interaction.frag), keeps the deepest distance-weighted penetration, and
  dims the direct light (diffuse+spec) by the result. Strength lives in `u_parallaxParms2.x`.
  Preset/quality-menu wiring of `r_parallax`/`r_parallaxShadow` still to do.
- **Phase E (optional polish)** — feed offset UV into SSAO normal reconstruction / SSR / occlusion maps.

## Phase B.5 (2026-08-04, IN PROGRESS) — height from the normal map, not `_h`

In-engine, Phase B read as unconvincing: most surfaces showed little relief, and some (kitchen /
washroom floor tiles) "sank as a whole" instead of following the surface detail. Root cause,
confirmed from the material source, not speculation:

```
bumpmap  addnormals( <name>_local.tga, heightmap( <name>_h.tga, N ) )
```

Doom 3's `addnormals(A, B)` fuses a **detailed** normal map `_local` (baked from a high-poly
mesh — this is the crisp detail visible in the SSAO normal debug) with a **coarse** normal
derived from the `_h` height map. **The fine detail lives in `_local`, not in `_h`.** Auto-capture
(Phase A) correctly grabs `_h`, but marching it gives a near-uniform UV shift where `_h` is
low-contrast — hence "the whole floor sinks" rather than per-detail relief. This is a data
problem, not a tuning problem: the height field the assets ship does not contain what the player
sees.

**The fix:** derive the parallax height from the bump stage's **combined normal map** (the full
`addnormals(...)` result — same image shaded and shown by SSAO), by **integrating its tangent-space
gradients** into a scalar height field at load: `dH/dx = -Nx/Nz`, `dH/dy = -Ny/Nz`, integrated
from both directions and averaged to cancel drift, then **high-passed** (subtract a blurred copy)
so only local relief survives — which is exactly what POM's short march wants, and it discards the
low-frequency integration drift for free. Cache the result as the stage's `parallaxImage`; the
`_h` capture and explicit `parallaxmap` keyword remain as overrides/fallbacks. Everything else
(unit-11 plumbing, the POM march, cvars) is unchanged — only the *height source* changes.

> **Refined by Phase B.6 (below):** discarding `_h` outright is right only when the normal
> carries authored detail. When the normal was itself generated from `_h`, B.6 marches that
> `_h` directly instead of re-deriving a worse copy of it from the normal.

Feature stays **off by default** (`r_parallax 0`); generation only runs when it is enabled at parse.

## Phase B.6 (2026-09-23) — pick the height source per material, no blend

Phase B.5 swung the height source all the way from `_h` to a field integrated out of the
combined normal map. That fixed the "whole floor sinks" failure on materials whose relief
lives in an authored `_local` normal. An earlier B.6 tried to *blend* the authored `_h` back
under that derived field (a weighted crossfade with a tuning cvar); it was replaced because
the blend was machinery — a cvar, a slider, a bake-time generation step and a targeted-reload
command — solving a problem that only one class of material actually has.

The observation that collapses it: **a height field is only worth deriving from a normal map
when that normal map carries authored detail.** Doom 3 bump stages come in two shapes:

- `addnormals( <file>_local, heightmap(<file>_h, N) )` or a plain `_local` file — a **real
  authored normal**. The fine relief is in `_local`; integrate the height field out of the
  combined normal (B.5). ~91% of stock bump stages.
- `heightmap(<file>_h, N)` alone — the normal is **generated from the height map**. Deriving
  a height back out of it is a lossy round-trip of `_h`; the authored `_h` *is* the ground
  truth, so march it directly. ~9% of stock bump stages (and many of those are on
  models/monsters, which parallax skips — world BSP only).

So `idMaterial::ParseStage` routes per material:

```
generatedNormal = program has heightmap(...)  AND  program has no addnormals(...)

parallaxmap <file>      -> load <file>            (explicit override, unchanged)
generatedNormal + _h    -> load the authored _h   (ImageFromFile, uncompressed)
otherwise               -> R_CreateParallaxHeightImage(program)   (derive from normal, B.5)
```

The detection is a case-insensitive substring test on the bump program string; every stock
`heightmap`-only stage is a plain top-level `heightmap(file, N)`, with no `smoothnormals(...)`
or nested wrapping, so the test is exact in practice. If the `_h` of a generated-normal
material is referenced but not shipped, it falls back to the derived field.

**"Custom vs stock" is free.** The authored path loads `_h` by name through the virtual file
system, so an HD pack's `_h` (a higher-priority pk4) automatically wins over the stock one;
if the pack doesn't override that texture, the stock `_h` is returned. No engine logic needed
to prefer one over the other.

**Why no blend, no `_h`-marching regression.** The B.5 "sinking" failure was specific to
`addnormals` materials, where `_h` is coarse *next to the detail in `_local`* — those keep the
derived field. A generated-normal material has no `_local`, so its `_h` isn't a coarsening of
anything; it is the source. Each material uses the one source that is actually meaningful for
it, so there is nothing to crossfade.

**Cost.** Authored-normal materials: unchanged from B.5 (one derived-field generation at level
load / `reloadImages`). Generated-normal materials: just an ordinary texture load of `_h`, no
generation pass. Nothing at draw time — the shader is untouched.

## Phase F (2026-11-17) — height in the PBR sidecar's alpha

The runtime field of B.5 is shallow by construction: `R_NormalMapToHeightMap` runs 64 Jacobi
sweeps (they spread height only ~8 texels) and then subtracts a 1/6-width box blur, so broad
panels and deep grooves keep only their edges; its [min, max] stretch and the one global depth
(`parallaxScale * r_parallaxScale * 0.015`) also make depth unrelated between materials. The
Phase D PBR sidecar ([pbr-materials.md](pbr-materials.md) sec. 7) had a free alpha channel, so
the offline bake now stores a better height there:

- **Bake** (`tools/pbr_extract_maps.py`): the same surface height field the AO bake uses
  (Frankot-Chellappa over the whole periodic tile, plus the `heightmap()` layer at the engine's
  scale), not high-passed, stretched over 0..255 between its 0.1 / 99.9 percentiles, warped into
  the diffuse's texture space. Its true depth (range in texels / width, divided by the bump
  stage's repeat when it is scaled) goes to `pbr/height/<pack>.cfg`, one per pack so the base /
  RoE / LM packs merge. `mcityc9`: baked depth 0.052 UV against ~0.009 from the runtime field at
  the default slider.
- **Detection**: `idImage::alphaVaries`, sampled with the other load-time stats
  (`R_ImageStatsRGBA`, serial, worker and `.dds` paths): the alpha is neither all 255 nor all 0.
  An RGB sidecar loads with alpha 255 and keeps the runtime field. No flag file needed, so a
  modder's hand-painted height works as is.
- **Source pick** (`RB_RHI_ParallaxSource`, shared by the interaction pass and the G-buffer): a
  sidecar with a height wins; it is bound on unit 11 (it is also on unit 10 for its ORM channels)
  and `u_parallaxParms2.y = 1` tells the shaders to read `.a` and march in the **diffuse** UV.
  Depth = the baked one x `r_parallaxScale` x `PARALLAX_SIDECAR_DEPTH_GAIN` (see "Depth
  calibration" below), or the runtime formula for an unlisted (hand-made) one. The depth lists
  are read in search-path order and the first entry for a stem wins, like its image (under LM the
  RoE pack is visible too, and `_roe.cfg` would sort after `_lm.cfg`). The sidecar demand-loads on first
  bind, so the first draw before it is loaded uses the runtime field.
- **Shaders** (`parallax.glsl`, included by `interaction.frag` and `gbuffer.frag`):
  `dudeParallaxHeight` picks R or A; `dudeParallaxCarry` maps the offset from the height's
  texture space to each other stage's with the screen-derivative Jacobians
  (`J_dst * inverse( J_src )`, exact for scaled / rotated stages, untouched when the spaces
  match); `dudeParallaxAxisScale` scales the sidecar depth along v by width / height, since the
  baked depth is relative to the width (a 32 x 256 trim).
- **Behaviour change on the runtime path**: the offset is now carried into the diffuse and
  specular spaces too, where before the bump-space offset was added to them unchanged. Same
  result wherever the stages share a transform (nearly everywhere); correct on `sofloor1`-like
  materials.
- The PBR editor's **Save to texture** keeps the alpha (it forced 255 before), and the editor
  shows "A = height" for a sidecar that carries one.

- **Adaptive march + binary refinement** (first in-game test, 2026-11-17: grilles such as
  `sflgrate2` came out as stacked strips at grazing angles). The march is per pixel, but its
  step count was a fixed ramp by view angle (6 head-on to 16 at grazing). A baked grille is
  ~6x deeper than the runtime field: 16 steps over its ~136-texel grazing march jump ~8 texels,
  past whole 4-texel slot walls, so neighbouring pixels stop in different layers. Now
  `dudeParallaxSteps` sizes each pixel's march from its own length: one step per ~2 height-map
  texels or ~2 screen pixels, whichever is fewer, clamped to `r_parallaxMinSteps` ..
  `r_parallaxMaxSteps` (default 16 -> 64, range up to 128, loop cap 128). Shallow fields and
  distant surfaces get fewer steps than before. Then 5 bisections of the last bracket (relief
  mapping) and a final interpolation inside it, for sharp walls. The self-shadow march sizes
  its climb the same way, capped at half the maximum.

- **Face anchor** (second test, `mcityc5` slider 0 -> 0.1: "way too warped", panels sliding and
  tearing at corners). The march keeps one height level on the polygon; it was mid-height (0.5,
  tuned on the runtime field), but a sidecar's main face sits elsewhere (`mcityc5` 0.68, grille
  tops 0.82, `sopanel15b` 0.26), so the whole face slid by (face - 0.5) x the offset with the
  view. Now the level kept is `imageAlphaStats_t::plane`, the peak of the alpha histogram
  smoothed over +-4 levels, measured at load (`u_parallaxParms2.z`); the runtime field keeps 0.5.
  Measured: the faces are flat (peak 0.5 - 2 texels wide out of 7 - 13 texels of relief), so
  integration drift is not what bends them, and a high-pass of the bake (1/4 .. 1/16 width) only
  added halos. The remaining warping at grazing angles is POM's own: the march may shift up to
  10x the depth there (`max( z, 0.1 )`), and a recess wall has no texels of its own, so it shows
  one texel stretched across it. Now limited: see "Pass parity and the grazing limit" below.
- **Height sign verified** ("the relief caves instead of bulging"): on paper,
  `R_HeightmapToNormalMap` stores nx = -dh/dx, ny = -dh/dy (rows), which the bake and the runtime
  field integrate with the same sign, and the view vector uses the tangent / bitangent the
  (visibly correct) normal-map lighting uses; in game, a temporary flip toggle (removed) A/B'd
  and the user picked the unflipped height. Vents and grille slots are recessed in the art.
- **Depth calibration** (third test, same wall at 0.1 with ORM on vs off). Shown as baked (the
  first cut: baked x `r_parallaxScale / 0.12`), a sidecar marched ~8.5x deeper than the runtime
  field at the same slider value (`mcityc5` 0.064 vs 0.0075 UV) and read as heavily warped: the
  true relief is too deep for POM. Now the slider means the same for both sources: a sidecar of
  the median world depth (0.062, mars_city1's 259 world textures) marches as deep as the runtime
  field of a material without `heightmap()` (parallaxScale 5 x 0.015 per slider unit), i.e.
  `PARALLAX_SIDECAR_DEPTH_GAIN = 0.075 / 0.062`. The bake still sets the depths relative to
  each other; `mcityc5` marches 0.0092 at 0.1. Checked on the full bake (2026-11-17): the median world depth is
  0.062 (Doom 3, 1303 textures), 0.063 (RoE) and 0.060 (LM), so the gain holds for all three.

Still flat depth (no `gl_FragDepth`), no relief in RT shadow rays: the next steps in the
relief plan.

## Pass parity and the grazing limit (2026-11-17)

Before the depth-correct step, two fixes for what the deeper baked heights made visible.

- **Ambient pass.** Phase C left the ambient-light pass on flat UVs because the mismatch was ~1%
  of a tile at the old runtime depth. The baked heights are several times deeper on grilles and
  panels, so an ambient-lit surface showed the flat texture under the shifted one. Now
  `ambientlight.frag` runs the same march (`parallax.glsl`) and shifts the bump, diffuse and
  occlusion / sidecar reads like `interaction.frag`; `ambientlight.vert` builds the same
  tangent-space view vector (location 10, threaded through the tess stages for linkage; world
  surfaces never tessellate). No self-shadow there. `RB_RHI_FillParallaxParms` no longer
  early-outs on ambient draws, except one the RT GI replaces (`r_rtgiAmbient`, draws nothing).
- **Emissive and overlay stages.** A material's ambient stages (a light panel's glow, a screen's
  add layer) are drawn by the generic stage pass on flat UVs, so a glow sat on the flat texture
  and slid off the relief as the view moved, in every tier including Path Tracing. For a surface
  whose lit passes march, `RB_RHI_RenderShaderPasses` now draws its `SK_GENERIC` stages with
  `generic_parallax` (`generic.vert` / `.frag` under `DUDE_GENERIC_PARALLAX`; the plain `generic`
  program also draws 2D with a vertex layout that has no tangents, so it is a separate program).
  `RB_RHI_StageParallax` fills the same parms as the lit passes, puts the height's own stage
  matrix (diffuse stage for a sidecar, bump stage for the runtime field) in `u_bumpMatrix`, and
  binds the height on unit 11; the shader carries the offset into the stage's UV with
  `dudeParallaxCarry`, so scrolling or scaled glow stages follow too. Custom-ARB, builtin and
  texgen stages are left alone.
- **Grazing limit** (`r_parallaxMinAngle`, default 15 degrees, 6..45; Advanced > Parallax >
  Grazing Limit). The march's view height is floored at sin(limit) instead of 0.1 (~5.7
  degrees): the shift grows as 1 / that height, so at the old floor it reached 10x the depth and
  stretched recess walls over many texels. Flatter views now march as if seen from the limit;
  the shift tops out near 3.9x the depth at 15 degrees and stays continuous at the limit. The
  step count follows the shorter march, so grazing pixels also get cheaper. Fidelity: changes
  how the (opt-in, non-vanilla) relief looks at grazing angles; 6 restores the old behaviour.
- **Default depth** `r_parallaxScale` 0.12 -> **0.10** (the user's pick after testing the baked
  heights with the fixes above). An archived cvar: an existing config keeps its value until reset.

## Non-goals (initial)

- **Depth-correct POM** (`gl_FragDepth` write for true silhouettes/self-occlusion) — breaks
  `DEPTHFUNC_EQUAL` against the flat prepass; forgone deliberately.
- **Single-offset parallax mapping** (no occlusion march) — swims; not shipped.
- **Tessellated displacement of the world** — that is tessellation's domain and far more
  expensive; POM is the right tool for flat world surfaces.
- ~~**opengl3 backend** — same GL-version framing as tessellation; Vulkan-only.~~ **Corrected
  (Phase B):** unlike tessellation (which needs a GL4 feature level), POM is a plain fragment
  technique. The shared interaction shader and the `SAMPLER_BINDING(11)` height sampler compile
  and bind on the GL3 backend as well, so the feature is genuinely cross-backend. The backend
  gate is `R_BackendSupportsEnhancements()` (GL3 + Vulkan), same as SSAO/occlusion/PBR. Whether
  to expose it on GL3 or keep it a Vulkan-tier-only toggle is a product decision, not a
  technical limit.

## Risks

1. **Shading-consistency, not depth** is the real risk: interaction and ambient passes must
   apply an identical offset or the relief crawls between lights. Mitigated by sharing one
   offset function/include across both fragment shaders.
2. **Alpha-tested surfaces** are numerous in Doom 3 and look *worse* with naïve POM — the
   exclusion gate (Phase C) is mandatory, not optional.
3. **Height-map quality varies**; some `_h.tga` are low-contrast (baked for subtle normals).
   Per-material scale clamp + global `r_parallaxScale` handle this; a few materials may want the
   explicit `parallaxmap` override.
4. **Auto-capture parser change** touches the image-program evaluator
   ([`Image_program.cpp`](../neo/renderer/Image_program.cpp)) — the one non-trivial CPU change;
   keep it additive (capture-and-continue) so vanilla normal baking is unchanged when
   `r_parallax 0`.
