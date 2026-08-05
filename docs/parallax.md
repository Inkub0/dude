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
| `r_parallaxScale` | `1.0` | global multiplier on per-material height scale |
| `r_parallaxMinSteps` | `8` | march steps head-on |
| `r_parallaxMaxSteps` | `32` | march steps at grazing angle |
| `r_parallaxShadow` | `0` | self-shadowing march (bool) |
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

Feature stays **off by default** (`r_parallax 0`); generation only runs when it is enabled at parse.

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
