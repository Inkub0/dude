# RT reflections on monsters — trace the TLAS, shade the dynamic hit

The first consumer of the R3.5 animated BLAS ([rtx-animated-blas.md](rtx-animated-blas.md)). Now that
monsters are GPU-resident, model-space, per-entity TLAS instances with full `gpuSkinVB` attributes
(pos/normal/color, reachable by device address), a reflective surface can trace a reflection ray into
the TLAS and show a monster — **even one that is off-screen or screen-space-occluded**, which is
exactly what today's reflection stack cannot do.

## Where reflections stand today (the gap)

Three layers, none of which reflect off-screen dynamic geometry:
1. **Env probes** (`environment` / `bumpyenvironment`, `bakeGlassProbe`): cubemaps baked per-area.
   Static — a walking monster is never in them.
2. **SSR** (`ssr.frag` march + `ssr_composite.frag`, docs/ssr.md): dynamic, but **screen-space** —
   a reflection ray that leaves the frame or hits a screen-occluded surface finds nothing. A monster
   behind the camera, or hidden behind a wall from the camera's view but visible to the reflection,
   does not reflect.
3. (raster shadow-map path — unrelated.)

SSR already builds exactly the inputs an RT reflection pass needs: a G-buffer with view-space normal
+ PBR material (roughness/metalness) (`gbuffer.frag`, MRT), `_currentDepth`, and a Fresnel/gloss
composite (`ssr_composite.frag`). We reuse all of it.

## The mechanism (confirmed feasible)

- **In-shader TLAS trace** is already done for RT sun shadows (`interaction.frag`): the TLAS device
  address rides in as a `uvec2` push/uniform, `accelerationStructureEXT(uvec2(...))` + `rayQueryEXT`.
  Reflections trace a **closest-hit** ray (no terminate-on-first) and inspect the committed hit.
- **Identify the hit:** `rayQueryGetIntersectionInstanceCustomIndexEXT` + `…GeometryIndexEXT` +
  `…PrimitiveIndexEXT` + `…BarycentricsEXT` + `…ObjectToWorldEXT`. We tag each TLAS instance with a
  custom index → a row in a shader-readable geometry table.
- **Fetch attributes at the hit:** `GL_EXT_buffer_reference` (already used + working for the batched
  BDA zfill, `r_vkBdaZfill`) dereferences the hit geometry's `gpuSkinVB` (idDrawVert: pos/st/normal/
  tangent/color) and its 32-bit index buffer — both device-addressable since R3.5 S0. Read the 3
  triangle verts, interpolate normal + vertex color by barycentrics, transform the normal to world by
  `ObjectToWorld`.
- **Shade the hit cheaply:** reuse the RT sun direction + colour already plumbed for sun shadows —
  `diffuse = saturate(N·sunDir)·sunColor·albedo + ambient`, `albedo = vtxColor · matTint`. Optionally
  trace a second (shadow) ray from the hit to the sun for self/scene shadowing in the reflection.

No G-buffer of world materials is needed because we only fully shade the **dynamic** (monster) hit —
the case SSR/probes cannot cover. Static/world hits fall through to the existing SSR + probe layers.

## Design — hybrid, monster-focused

**RT augments the reflection stack; it does not replace SSR.** SSR stays the primary dynamic layer
(cheap, accurate, on-screen); RT contributes the pixels SSR structurally misses — dominated by
off-screen monsters. This bounds cost (RT only where it adds something) and risk (SSR still ships as
the default), and matches the ask ("reflections on monsters") precisely.

- **Reflective pixels only.** Same gate SSR uses: roughness ≤ cutoff, gloss window, metalness — from
  the G-buffer material MRT. Non-reflective pixels never trace.
- **Monster hits shade; everything else defers.** On a committed hit whose instance is a monster
  (table row has a `gpuSkinVB` address), fetch + shade. On a static/miss hit, write nothing — SSR and
  the env probe already own those pixels. (RR3 may add world-hit shading to also fill SSR's off-screen
  *static* misses, but that is not the MVP.)
- **Composite** additively into the same reflection buffer SSR feeds, weighted by the same Fresnel/
  gloss so material response and edges stay identical to SSR.

### Per-instance geometry table (the one new piece of infrastructure)

A device-local SSBO the reflection shader reads: a flat array of geometry descriptors, one per
`(instance, geometry)` pair. Each row:

```
struct RtGeoDesc { uint64_t vtxAddr; uint64_t idxAddr; uint vtxStride; uint flags; };
```

Each TLAS instance's `instanceCustomIndex` = the base row for its BLAS's geometries; the shader reads
`table[customIndex + geometryIndex]`. Monster instances (built in `RefreshAnimBlas`) fill real
`gpuSkinVB`/index addresses + `flags |= MONSTER`; static/mover instances get `vtxAddr = 0`
(`flags = 0`) → "no attributes, defer". Rebuilt each frame alongside the TLAS (cheap: a few dozen
rows). The normal transform is NOT stored — the shader gets it from `ObjectToWorldEXT`.

## Staged plan (validator-first, the way R2/R3/R3.5 landed)

- **RR0 — per-instance geometry table (no behavior change).** Add the SSBO + populate it where TLAS
  instances are built (monster rows real, static rows zero); assign `instanceCustomIndex`. Nothing
  reads it yet. Verify: row count == instance count, monster rows have non-zero addresses, no
  validation errors, non-RT/GL3 inert.
- **RR1 — reflection-ray + attribute-fetch validator (`r_rtReflTest`, one-shot).** Trace reflection
  rays at a set of surfaces; on a monster hit, fetch the interpolated world normal via the table+BDA
  and diff against a CPU reference (same triangle, same barycentrics). Proves identify + fetch +
  transform before anything renders. Mirrors `r_rtWorldTest` / `r_rtAnimBlasTest`.
- **RR2 — RT monster reflections (compute, additive) — MVP.** For reflective pixels: reconstruct
  world pos + normal from G-buffer/depth, reflect the view ray, trace closest-hit; on a monster hit
  shade (sun N·L + ambient from `gpuSkinVB` normal/colour) and write; else leave for SSR. Composite
  with the SSR Fresnel/gloss weighting. Gate `r_rtReflections`. **Visible:** monsters appear in
  reflective floors/metal, including off-screen.
- **RR3 — per-material colour, no bindless.** Replace the flat 0.55 albedo with each hit surface's
  **material average colour** (`idImage::averageColor`, already computed per texture) stored in the
  geometry table (one packed RGBA8 per row). Gives coloured monster reflections (brown zombie, grey
  sec-bot) — approximate, not per-texel, but a big step from uniform grey with zero new infra. Keeps
  the fixed key light + ambient (indoor Doom 3 has no sun; real per-light shading at a hit is RT-GI
  territory). **Verified visually 2026-09-15: RR2 geometry is pixel-perfect (user), so RR3 is a shading
  swap.**
- **RR4 — bindless material substrate → per-texel textured reflections. DONE + user-verified 2026-09-15
  ("looks right, with colour and all").** The real prize, and the one genuine infra lift. Landed in three
  validator-first stages:
  - **RR4a** (commit ebf886aa) — the substrate, inert. Enable the five core-1.2 descriptor-indexing
    features behind a `haveDescriptorIndexing` gate; create a set-2 layout = one variable-count
    `COMBINED_IMAGE_SAMPLER` array (cap 8192, `UPDATE_AFTER_BIND | PARTIALLY_BOUND`) + its pool + one
    persistent set; extend the graphics `pipeLayout` from 2 to 3 sets. Nothing binds/samples it yet.
    Verified validation-clean (5422 frames), rendering byte-identical.
  - **RR4b** (commit 3503a4e7) — populate + thread. `SyncBindlessSlot(handle)` points bindless slot
    `handle-1` at the image's view+sampler; hooked into `UploadTexture2DLevels` (register every disk
    material texture) and `DestroyImage`/`RetireImage` (reset the freed slot to the dummy). Add
    `texIndex` to `BlasGeometry` + `RtGeoDesc` (reusing the old `pad` word → row stays 32 B);
    `R_RtMaterialTexIndex` captures the diffuse `idImage::rhiHandle` at anim-caster staging, copied into
    the monster rows. `r_rtReflTest` extended to fetch `st` (words 3-4) and diff vs a CPU reference +
    report `texIndex`. Verified: PASS, **st max err 0.0000**, texIndex 32/32, validation-clean over a
    full level's texture load.
  - **RR4c** (commit 68acbe54) — sample. `ssr_rt.frag` enables `GL_EXT_nonuniform_qualifier`, declares
    the set-2 array, fetches `st` by barycentrics, and shades `albedo = texIndex ?
    texture(u_rtTextures[nonuniformEXT(texIndex-1)], st).rgb : baseColor` (RR3 fallback). The backend
    binds set 2 once per frame cb. Verified validation-clean with the full RT-reflection stack on
    (18583 frames) + user visual pass 2026-09-15 — textured, coloured, correct.

  This substrate is the prerequisite for RR5 and for every later RT pass (H2 denoiser, H3 soft shadows,
  H4 RTAO, H6 GI) that must evaluate a material at a hit.

  **RR4 surfaced the RR5 gap (uncanny up close).** With the monster reflection now convincingly textured,
  a reflective *floor* looks wrong when you stand near a monster: the floor's world reflection comes
  *only* from SSR (screen-space), which fails for the off-screen world at close/steep angles, so the room
  reflection vanishes while the RT monster keeps reflecting — a monster floating in a non-reflective void.
  This is not an RR4 bug; RR4 made the monster real enough to expose that RT reflects only monsters, never
  the off-screen world. **RR5 (below) is the fix** (shade world hits too); a cheap interim is an
  env-probe / ambient backdrop fill in `ssr_rt` on non-monster hits.
- **RR5 — RT WORLD reflections (replaces the snapshot glass system). SHIPPED** (RR5a-3 world BLAS
  846eb443, RR5c stage 1 glass fac2a939; still open: entity coverage + RR5c stage 2, sequenced with the
  RR12–RR15 fidelity stages in "Fidelity follow-ups" below). The user's goal: today glass /
  env reflections use baked env-probe cubemaps + `_currentRender` snapshots, which can't show anything
  dynamic or off-probe. Rebuild the **static world BLAS from full `idDrawVert`** (not positions-only, so
  `st`/normal are available) + give each world geometry a material/texture index in the table (RR4
  bindless), then shade world hits with their real texture. Reflective surfaces trace the true room —
  no probes, no snapshots. Also lets RT fill SSR's off-screen *static* misses (a true hybrid).
- **RR6 — quality + verify + preset.** Roughness-driven glossy (SSR colour-mip blur), temporal reuse
  (`ssr_temporal` history + motion vectors), seam softening across the SSR↔RT boundary; A/B + perf;
  wire into Ultra Nightmare, `r_rtReflections` default per the measured cost.

  **RR6a — temporal RT reflections (the firefly fix). BUILT, user visual gate pending.** The residual
  fireflies after RR5 were the RT layer compositing **one sharp ray straight onto the scene**: wherever
  SSR *persistently* misses (grout lines the screen-space march can never reach), RT legitimately fills
  the pixel, but a single point-sampled ray has no way to resolve, so it aliased into a static grid of
  dots that stood out against SSR's temporally-smoothed neighbours. RR6a gives the RT layer the **same
  temporal treatment SSR gets**:
  - The RT contribution (`ssr_rt.frag`, unchanged shade path — RGB + hit-mask in alpha) now renders into
    its own target `rhiRtReflRT` (sized to the SSR march resolution, so RT and SSR share a grain and
    scale together with `r_ssrResScale`) instead of directly onto the scene.
  - `r_rtReflTemporal` (default 1) then accumulates that target across frames through the **exact
    `ssr_temporal` pipeline** SSR uses — camera + per-object velocity reprojection (`RB_RHI_TemporalReproj`,
    the R1 motion-vector MRT), a history ping-pong (`rhiRtReflHistRT[2]`), variance clipping + hit-aware
    feedback. The shared `rhiTemporalCam` advance-once-per-frame invariant already handles RT as a third
    consumer after SSAO/SSR.
  - `ssr_rt.frag` gained a small per-frame **ray jitter** (`r_rtReflJitter`, sub-degree cone, golden-ratio
    phase) — *the* piece that makes the accumulation **resolve** the grid rather than smear a static image
    (same reason SSR's march jitter lets its temporal resolve the march grain). Jitter is off unless
    temporal is on.
  - Finally the smoothed result is composited additively onto the scene (`smaa_copy`, 1:1). `r_rtReflTemporal 0`
    restores the exact pre-RR6 look (at `r_ssrResScale 1` the new dedicated-target path is byte-identical to
    the old direct-to-scene draw), giving a clean runtime A/B.

  Landed on `feat/rtx-bindless-materials`; builds clean (shader recompiled + embedded + relinked).
  **Gate result (user, 2026-09-15):** `r_rtReflTemporal 1` mitigates the grid, but a residual is *still
  visible standing perfectly still* — temporal killed the motion/flicker component but not the spatial one.

  **RR6b — spatial blur of the RT contribution (the static-residual fix). BUILT, gate pending.** Diagnosed
  the standing-still residual: RT's shaded radiance is bounded (~albedo·(0.28 + N·L) ≤ ~1.3, ×weight ≤ ~0.64),
  so it is **not** an HDR brightness spike — a firefly *clamp* (like `ssr_composite`'s `r_ssrFireflyClamp`,
  threshold 3) would never even fire on it. The residual is a **spatial** step: RT fills SSR's isolated
  single-pixel blind spots (grout) with a flat key-light approximation that differs from SSR's screen-space
  reflection at the neighbours. That ~0.1–0.2 per-pixel step **clips toward white and vanishes in LDR but is
  preserved by HDR** and tonemapped into a visible dot (the user's HDR hunch, correctly explained), and being
  spatially static neither temporal nor jitter can dissolve it. Fix = a box blur of the RT target before the
  additive composite (`ssr_rt_composite.{vert,frag}`, replacing the plain `smaa_copy` recomposite): it spreads
  each fill into a low-frequency haze and dims ISOLATED speckles by peak/N² while large off-screen fills (every
  tap contributing) keep their brightness — so the marquee off-screen-monster case survives while the grout
  grid dissolves. `r_rtReflBlur` (0..1, default 0.6) is the strength; 0 = the sharp RR6a look. New shader files
  → cmake reconfigure done. **Gate result (user):** the shimmer is "almost gone" and the rest is manageable
  via SSR temporal — but exaggerating reflectivity exposed a new artifact: the reflection is **"striped"**, and
  the user localised it precisely — *only at `r_ssrResScale 0.75`* (full res is clean), and *only with
  `r_rtReflections` on* (it's the RT layer, not SSR).

  **RR6c — temporal UPSCALE of the RT reflection (the striping fix). BUILT, gate pending.** Root cause: a
  **non-integer upscale ratio**. The composite bilinear-upsamples the fractional-res RT buffer, and at 3/4 res
  the interpolation weights cycle with period 4 output pixels — over sharp RT content that beats into a regular
  horizontal stripe. (Integer ratios like 0.5 upsample cleanly; full res has no upscale.) It's RT-only because
  RT is the *sharp* layer — SSR's march dither + glossy pyramid pre-soften its content so nothing survives to
  beat. Fix = eliminate the fractional bilinear upscale by **temporally upscaling** the RT reflection:
  - The ray-query **trace stays at `ssrRes`** (cheap — the whole point of the slider), but the temporal
    **history is promoted to full res** (`RB_RHI_EnsureRtReflHistory(fullW, fullH)`). `ssr_temporal` bilinear-
    upsamples the `ssrRes` trace into the reprojected full-res history each frame; the composite then reads a
    full-res buffer 1:1 — **no fractional upscale, no beat**.
  - The RR6a ray-cone jitter was replaced by a **sample-GRID jitter**: `ssr_rt` offsets its whole reconstruction
    by a per-frame Halton(2,3) sub-texel amount (`r_rtReflJitter`, ±0.5 trace-texel at 1 = exactly one
    interpolation cycle), so the residual per-frame beat lands at a different phase every frame and the full-res
    history averages it out — while accumulating sub-pixel-shifted samples toward genuine detail (mini-TAAU).
  - Cost is small and *cheaper than tracing at full res*: trace unchanged, only the fullscreen resolve/composite
    + history go full-res (**~+0.1 ms, ~15–30 MB**). Directly answered the user's "unless it's much more
    expensive" condition and their FSR2-for-reflections idea (a dedicated FSR2 instance was assessed as heavier
    + quality-risky: it expects a full frame with matching MVs/depth, but a reflection's MVs/depth describe the
    reflector, not the reflected content).
  - **Honest limit:** `ssr_temporal`'s variance clip clamps history to the current (bilinear) neighbourhood, so
    the recovered sharpness is "good bilinear+", not full native — stripe-free + stable, pushable sharper later
    by loosening the clip. `r_rtReflTemporal 0` = the pre-RR6 look (stripes return).
  - **Quarter-res shimmer fix (2026-09-20, user-verified).** At a Quarter trace the reflection shimmered
    because the variance-clip's 3×3 taps were spaced one *output* pixel apart: over a 4×-upscaled trace all
    nine fall inside one bilinear cell, the deviation collapses, the clip box pins history to the current
    frame, accumulation stops and the per-frame grid jitter shows through. `ssr_temporal` now steps the taps
    by the **trace texel** (`u_localParam1.y` = output px per trace texel; 1 for plain SSR, whose history
    matches its march) and reads the trace **un-jittered** (`u_localParam1.zw`) — the "true un-jittered TAAU"
    once listed as remaining. Shimmer largely gone; a residue survives only on near-mirror surfaces with
    tiled normal maps (one bump normal stands in for a 4×4 footprint) — accepted. Un-jitter made no visible
    difference over the tap spacing alone but is the correct read and free; `r_rtReflJitter` stays 0.5.
    Makes **Quarter a usable setting** (1/16 of the rays).

  **Gate:** at `r_ssrResScale 0.75`, user confirms the stripes are gone with `r_rtReflTemporal 1` and it holds
  up in motion (watch for moving-monster ghosting from the wider full-res history). **Remaining RR6:** perf
  measure, optional sharper reconstruction (loosen the clip / true un-jittered TAAU), preset wiring
  (`r_rtReflections` into Ultra Nightmare).

## RR11 — shader micro-optimisation (mathematically equivalent, cheaper)

A literature-informed audit pass (real-time blur/TAA/Fresnel references) for **same output, fewer
resources**. Every change is either bit-exact or exact-in-intent-and-more-accurate — no visual change is
intended; the RT-reflection look is frozen at the RR6–RR10 result. Deliberately NOT taken: the Karis
spherical-gaussian Fresnel (an *approximation*, not equivalent) and YCoCg/k-DOP variance clipping (a
*quality* change to `ssr_temporal`, not a cost reduction). Both are noted as future options.

- **A — `ssr_rt_composite` box blur: 25 point taps → 8 bilinear taps + reused centre.** The RR6b blur is
  a contiguous 5×5 box (shipped tap spacing = 1 texel, `RhiWorld.cpp` `localParam0.y = 1`). A box is
  separable, and the RT target is sampled **LINEAR + CLAMP_TO_EDGE** (`VulkanBackend.cpp` render targets:
  `GetSampler(TF_LINEAR,…)`), so a bilinear fetch at the **midpoint** of two adjacent texels returns their
  exact 50/50 average. Grouping each axis `{-2,-1},{0},{+1,+2}` → `{pair, centre, pair}` with per-axis
  weights `{2,1,2}/5` reproduces the 25-texel equal average **bit-for-bit**: corner taps average a 2×2
  (weight 4/25), edge taps a 1×2 (2/25), centre is the lone texel (1/25, reuse the already-fetched `c`).
  ~3× fewer texture fetches on the full-res composite. Exact at spacing 1; a dilated spacing degrades to an
  equivalent smooth box (not the sparse dilated one), which is fine — spacing is not exposed as a cvar.
- **B — Schlick Fresnel `pow(1−c, 5.0)` → `m²·m²·m` (3 muls)** in `ssr_rt`, `ssr_composite`, `ssr`. glslang
  emits a real `Pow` (exp2∘log2) for a constant integer exponent; the multiply form is **exact and *more*
  accurate** (no transcendental round-trip) as well as cheaper, per reflective pixel across three shaders.
- **C — `ssr_rt` hit-vertex fetch CSE.** Hoist the per-vertex base word index `i·s` into `b0/b1/b2` and the
  shared third barycentric `w0 = 1−bc.x−bc.y` (used by both the normal and texcoord interpolation). Pure
  common-subexpression elimination — bit-exact; the compiler likely already folds it, so this is mostly
  intent/readability plus a belt-and-suspenders guarantee for shaderc.
- **D — `ssr_composite` view direction drops the depth divide.** This pass reconstructs a view-space `P` only
  to form `V = normalize(−P)` for the Fresnel `NdotV`. Since `P = depth · dir` with `depth > 0`, `normalize` is
  invariant to the positive depth: `V = normalize(−dir)`, `dir = (ndc.x/proj00, ndc.y·ySign/proj11, −1)`. So the
  linear-eye-z divide `1/(raw·c.x + c.y)` is dead work here (only `ssr`/`ssr_rt`/`ssr_temporal`, which use `P`'s
  *magnitude* to march/offset/reproject, still need it). Mathematically exact; FP-wise it differs only by the
  rounding of one `normalize`, invisible in a smooth reflectance term. Removes a divide per reflective pixel on
  the full-res composite (and the now-unused `depth_consts`).

**Verify:** build clean (`SPIR-V: 4 compiled … 0 failures`). A/B the GPU cost with `r_vkGpuTime` on a
reflective, monster-in-frame scene (the composite + `ssr_rt` are the touched hot paths); the reflection
image should be pixel-indistinguishable before/after. `ssr_temporal` and the `ssr` march are left as-is —
already near-optimal for their algorithms (the march's `projectToFrag` was folded to ~5 muls earlier).

## #4a — skip the dead SSR march when RT replaces the composite (the real perf lever)

This is *fetch-elimination*, not equivalence shader-math, and the one change that actually moves `r_vkGpuTime`
in the RT-on preset (Ultra Nightmare). Post-RR7, `ssr_rt` traces every reflective pixel itself and reads **none**
of the SSR march's output, and the SSR composite is already gated off when `rtWillRender`. Yet the whole
ssr-res chain — Hi-Z build, scene snapshot, the 64-step **march**, `ssr_temporal`, and the glossy pyramid —
still ran and was thrown away. That is a full dependent-fetch-bound pass of pure waste under RT.

`RB_RHI_ScreenSpaceReflections` now **hoists `rtWillRender` above the march** (all its inputs — `rtWants`, the
TLAS/geo device addresses, `ssrW/H`, the invertible view — are known pre-march) and wraps the entire march
chain **+** composite in `if ( !rtWillRender ) { … }`, following the exact non-reindent guard style the composite
already used. When RT renders, that block is skipped wholesale; when RT can't run, the full SSR path executes
exactly as before (safe fallback). Two knock-on details:
- The RT pass's `u_ssr` slot (unit 0) is **unused** since RR7, but it used to bind `resultRT` — which is now
  scoped inside the skipped block. It's rebound to the always-valid **normal G-buffer** as a harmless dummy.
- The `_currentRender` snapshot was originally **preserved** here (a safety net for downstream refraction /
  heat-haze materials, which sample `_currentRender`), on the reasoning that the backend has no guaranteed
  per-view refresh after SSR. **Item 6 (a later change) then dropped it under RT** — no pass in the
  `rtWillRender` block reads it and those materials re-capture on demand — reclaiming the full-res blit
  (see the `item 6` comment in `RhiWorld.cpp`'s RT block). **Consequence for RR12:** the lit scene is *not*
  snapshotted under RT, so RR12 samples the **live** HDR scene-colour target rather than re-adding a blit
  (RR12b′ in the fidelity section).

Only the VK + `r_rtReflections` + ray-query path is affected; SSR-only presets (Nightmare and below) are byte-
identical. **Verify:** `r_vkValidation 1` soak in Ultra Nightmare (watch for layout/barrier errors on the RT
targets), confirm RT reflections still look identical, check refraction/heat-haze surfaces are unchanged, and
A/B `r_vkGpuTime` with `r_rtReflections 0↔1` — the march/temporal/glossy cost should vanish when RT is on.

## RR5c — glass on RT reflections (drop the cube in the RT tier)

The user's original goal for the whole RR5 arc: replace the baked env-probe/cube glass reflections with the
real ray-traced room. Glass is **translucent** — it renders after the `SS_DECAL` split and is *not* in the
G-buffer, so `ssr_rt` (which runs on opaque G-buffer pixels) can't reach it. Instead glass gets its **own
forward RT shader**. The enabling fact: the shared scene TLAS already holds the fully-shadeable, textured
world + monsters (RR5 world rows set `flags = RT_GEO_MONSTER` — bit 0 = "has attributes → shade the hit" — so
`ssr_rt.frag`'s "static hit → SSR/probes own it" comment is stale; world hits *do* shade). So glass reuses
`ssr_rt`'s exact trace-and-shade, just fed a ray from the glass surface.

**Stage 1 (unbumped `environment` glass — the windows: `glass2`, `outdoor_glass1`, `mc_scannerglass`):**
- New `environment_rt.{vert,frag}`. The vert reconstructs the glass **world** position (`M·localPos` via
  `u_modelMatrixRow0-2`, full vec4 dot for the translation) + world normal + world view vector (rotate the
  local vectors by `M`, matching `bumpyenvironment.vert`), and folds the stage colour into `var_Color` exactly
  as `environment.vert` (so the RT reflection carries the material's dimming tint). The frag reflects off the
  glass normal (flipped to the viewer-facing side), traces the TLAS from `worldPos + N·2`, and shades the hit
  through the RR4 bindless substrate — a copy of `ssr_rt`'s hit block. On a **miss** (ray escapes to sky, or a
  positions-only defer row) it fills with a dim sky/ambient tone (`RT_SKYFILL`), never black. Output is
  `refl · var_Color` in the same `dst_alpha` blend slot the cube used.
- Backend (`RB_RHI_RenderTexgenStage`, `TG_REFLECT_CUBE`): `rtGlass` engages when `vkMode &&
  r_rtReflections && SupportsRayQuery() && !GetBumpStage()` and the TLAS/geo table/program are all present.
  It sets `u_rtParms` (TLAS + geo addresses), **skips the probe swap + cube bind** (`environment_rt` samples no
  unit-0 cube — only the bindless set-2, reachable from forward draws since RR4a made the graphics layout 3
  sets), and overrides `pd.shader` to `environment_rt`. The switch is **per-draw at runtime**, so toggling the
  preset flips glass between cube and RT with no `vid_restart`. `r_gl3ReflectionScale` still dims `var_Color`.
- **Gating:** keyed on `r_rtReflections`, already Ultra-Nightmare-only. Nightmare and below keep the vanilla
  cube untouched — the faithful default. Fidelity: UN glass genuinely changes (live traced reflections with
  off-screen geometry vs the baked cube), which is the intended upgrade.
- **Verified** (user, RTX 3080 Ti): `r_vkValidation` clean, interior + exterior (`comm1`/`alphalabs2`
  `outdoor_glass1`) panes reflect the real room correctly, sky/ambient fill reads right on sky-facing panes.

**Stage 2 (BUILT 2026-09-21, user gate pending):** `bumpyenvironment_rt.{vert,frag}` for bumped glass
(chrome / textured panes — the `GetBumpStage() != NULL` case stage 1 excludes). The vert merges
`environment_rt.vert`'s world-position/tint fold with `bumpyenvironment.vert`'s tangent→global rows; the
frag builds the reflection normal exactly as `bumpyenvironment.frag` (sample bump, rotate to world) then
**flattens it toward the flat geometric normal** (`RT_BUMP_FLATTEN 0.5`) for ray stability, biases the ray
origin along the stable flat normal, and reuses the stage-1 trace-and-shade + sky/grazing miss fill. Backend
(`RB_RHI_RenderTexgenStage`, `TG_REFLECT_CUBE`): a parallel `rtGlassBump` path engages on the same RT gating
when a bump stage is present, binds the bump map on **unit 0** (the cube is skipped, so unit 0 is free —
avoids a novel unit-1-without-0 descriptor) and overrides `pd.shader` to `bumpyenvironment_rt`. Per-draw
runtime flip like stage 1; SPIR-V confirms bump at set 1/binding 0 + bindless at set 2. Optional Fresnel /
real fog-colour miss fill remain future polish.

## Entity coverage — the room's props + dropped items should reflect too (BUILT 2026-09-21, user gate pending)

Today reflective floors show the worldspawn (RR5) + monsters (RR2), but **static entity props reflect as
nothing** and **runtime-dropped items don't either** — a reflection ray hits their positions-only defer row
(`flags & 1 == 0`) and returns 0. Two items, one shared mechanism: give non-monster entities *attributed*
BLASes so `ssr_rt`/`environment_rt` can shade the hit. The template is `R_RtBuildWorldAreaBlas`
(`tr_main.cpp:1847`, RR5a-3), which already builds per-surface `CreateBlasFromBuffers` BLASes for the world.

**BUILT** (`feat/rt-refl-entity-coverage-bumped-glass`): both items landed as designed below.
`R_RtBuildEntityModelBlas` (Item 1) builds the map's static props attributed into `s_rtModelBlas` +
`s_rtEntityVB/IB`, called right after the world areas in `R_RtWorldUpdate`; `R_RtBuildScene` gained a
`modelsPreBuilt` flag so the static anchor instances those pre-built attributed BLASes (validator stays
honest). `R_RtBuildOneEntityModelBlas` + the first-sight cache in `R_RtRefreshInstances` (Item 3) build a
dropped item's attributed BLAS the frame it appears (synchronous — a brief one-time stall), keyed by model
+ a topology signature that guards model-pointer reuse; no mid-map destroy (a first-sight entry lives until
the map's `DestroyRtScene`, dedicated buffers freed by `R_RtFreeWorldGeo`). Build + shader-validator clean;
the reflect/shadow "does it look right" gate is the user's.

### Item 1 — `func_static` / map-placed props (the foundation)

**Current state.** `R_RtGatherEntities` (`tr_main.cpp:1243`) gathers `DM_STATIC`, casting entities as a
**positions-only** soup (3 floats/vert); `R_RtBuildScene`'s model loop (`~1413`) builds a positions-only
`CreateBlas` per unique model, registers it in `s_rtModelBlas`, and instances it. `UpdateTlas` gives each a
single zero-attr defer row → `ssr_rt.frag:125` returns 0.

**Change.** Make the per-model BLASes **attributed**, mirroring `R_RtBuildWorldAreaBlas`:
1. Add dedicated device buffers `s_rtEntityVB/IB` (like `s_rtWorldVB/IB`), freed in `R_RtFreeWorldGeo`.
2. Gather **full `idDrawVert`** (model space) per *unique model* into `s_rtEntityVB/IB`, and build a per-surface
   `RHI::BlasGeometry` for each casting surface: `vertexAddress = vbAddr + vOff*sizeof(idDrawVert)`,
   `vertexStride = sizeof(idDrawVert)`, `indexAddress = ibAddr + iOff*4`, surface-local 32-bit indices,
   `texIndex = R_RtMaterialTexIndex(surf->shader)`, `baseColor = R_RtMaterialBaseColor(surf->shader)` — exactly
   as the world path (`1898-1915`), but keyed per model, not per area.
3. Build each unique model's BLAS via `CreateBlasFromBuffers(geoms, cnt, /*allowUpdate*/false)` (chunk at
   `MAXG=64` surfaces), store `model→blas` in `s_rtModelBlas`.

**What already works unchanged** (this is why it's contained): the per-entity instancing +
**`R_RtRefreshInstances`** (`1544`) re-instances `s_rtModelBlas` by model pointer with the *current* pose every
frame (so movers/doors track — a model-space BLAS + per-frame transform is REQUIRED; a world-space bake would
freeze the pose), and **`UpdateTlas`** (`VulkanBackend.cpp:5510`) fills one attributed row per geometry
(`flags = has-attr`, `texIndex`, addrs) for any stored-descriptor BLAS. At a hit, `ssr_rt` applies
`mat3(ObjectToWorld)` (the instance transform) to the interpolated model-space normal — same as monsters.

**Recommended structure:** a new `R_RtBuildEntityModelBlas(r, world)` that mirrors `R_RtBuildWorldAreaBlas`
(per unique model instead of per area, filling `s_rtModelBlas` with attributed handles), called right after it
in `R_RtWorldUpdate` (`~1966`). Then the positions-only entity path in `R_RtGatherEntities` +
`R_RtBuildScene` can retire for the persistent build (keep it only if the `r_rtWorldTest` validator still wants
a positions-only entity trace — attributes are transparent to the CPU geometry trace, so the validator can use
either). This keeps the diff parallel to the proven world path and avoids churning the `R_RtBuildScene`
signature/callers (`~1969` persistent, `~2121` validator).

**Verify:** `r_vkValidation` soak; a map with `func_static` props (crates/consoles) + a reflective floor under
Ultra Nightmare — the props should now appear in reflections (were invisible). `r_rtWorldTest` still PASS.

### Item 3 — runtime-dropped items (builds on item 1)

The persistent scene builds once per map (`s_rtWorldMap` check at `1948`); items **dropped at runtime**
(`idItem`/moveables spawned after that — the zombie's chainsaw) aren't in `entityDefs` at build time, and
`R_RtRefreshInstances` only re-instances the *existing* `s_rtModelBlas`. Fix: **build-on-first-sight** — in the
per-frame refresh, when a `DM_STATIC` casting entity's model isn't in `s_rtModelBlas` yet, build its attributed
model BLAS then (reusing item 1's `R_RtBuildEntityModelBlas` machinery) and cache it by model, with a
retire-after-grace lifecycle mirroring the animated-monster BLAS cache (`RefreshAnimBlas`, `ANIM_RETIRE_GRACE`).
The model geometry is static, so only the per-frame transform updates. **Verify:** drop a chainsaw near a
reflective floor under RT — it should appear in the reflection.

## Fidelity follow-ups — RR12–RR15, sequenced around the open RR5 work (PLANNED 2026-09-19)

**Where the arc stands.** RR0–RR11, #4a and RR5c stage 1 are shipped: under Ultra Nightmare every
reflective opaque pixel and every unbumped glass pane traces the real, textured room + monsters. Entity
coverage (Item 1 props, Item 3 dropped items) and RR5c stage 2 (bumped glass) are now **BUILT** (2026-09-21,
`feat/rt-refl-entity-coverage-bumped-glass`, user visual gate pending) — see the sections above. This
section adds four fidelity stages and slots them around those, so nothing gets built — or visually gated —
twice. No new assets anywhere; everything reads data the engine already has.

**What the RT tier still gets wrong** (read off `ssr_rt.frag` / `ssr_rt_composite.frag` as of 76bb37ec):

1. **The reflected scene is lit by a fake light.** `ssr_rt.frag:156-161` shades every hit as
   `albedo · (ambient + N·L_key)` — a fixed key direction, no real lights, no shadows, no specular, no
   decals, no emissive. A red-lit corridor reflects as a neutrally lit one; a dark corner reflects bright;
   a glowing GUI screen or light fixture reflects as a dull textured panel. With the geometry now
   pixel-perfect this is the largest remaining error in the image.
2. **Ultra Nightmare lost glossy reflections.** `r_ssrGlossy`'s colour pyramid lives inside the SSR chain
   that #4a skips when RT renders; the RT composite only has the fixed 5×5 `r_rtReflBlur` box. So at UN a
   polished floor and brushed metal reflect equally sharp — roughness only *dims* (the gloss window), it
   never *blurs*. The tier below (Ultra/Nightmare, SSR) handles this better than the top tier.
3. **Reflections ignore occlusion.** Neither composite reads the AO term, so crevices, the undersides of
   props and the contact line under a monster's feet reflect at full strength — reflective surfaces "glow"
   exactly where the ambient term was just darkened by RTAO/GTAO.
4. **Wet blood reflects lights but not the room** (parked in [todo.md](todo.md), "Decals: reflections on
   wet blood").

### Order, and why

| # | Stage | Depends on | Tier | Size |
|---|---|---|---|---|
| 1 | **Entity coverage Item 1** (props) — **BUILT 2026-09-21** | — | UN | M |
| 2 | **RR12** — lit-scene reuse at the hit (+ RR12b emissive flag) | best judged after 1 | UN | S–M |
| 3 | **Entity coverage Item 3** (dropped items) — **BUILT 2026-09-21** | 1 | UN | S |
| 4 | **RR5c stage 2** (bumped glass) — **BUILT 2026-09-21** (own hit-shade copy; RR12a factor still TODO) | RR12's shared hit-shade include | UN | S–M |
| 5 | **RR13** — roughness-driven glossy for the RT tier | 2 (tune blur on correctly-lit content) | UN | M |
| 6 | **RR14** — specular occlusion on reflections | — (independent; can jump the queue) | Ultra+ (SSR **and** RT), GL3+VK | S |
| 7 | **RR15** — reflections on wet blood (the parked decal G-buffer) | RR13 | Ultra+ | M, optional |

The rule behind the order is **coverage → shading → filtering → occlusion**: every stage's visual gate is
judged by eye (the user's), and a gate on an incomplete image tunes against the wrong thing. Props that
return 0 (`ssr_rt.frag:132`) read as holes in the reflection; RR12 changes the brightness and colour of
everything RR13 will blur; RR13's blur radius decides how visible RR14's contact darkening is. RR14 has no
code dependency and is the smallest, so it is the natural filler if a bigger stage is blocked on a gate.
RR5c stage 2 moves *behind* RR12 on purpose: `environment_rt.frag` carries a copy of `ssr_rt`'s hit block,
and a third copy (`bumpyenvironment_rt`) should not be made before the block is factored out.

### RR12 — lit-scene reuse at the hit. BUILT 2026-09-19 (core), user visual + `r_vkValidation` gate pending.

The standard hybrid trick, inverted from how SSR uses it: RT finds the hit **exactly** (no march, no
thickness heuristics), then asks "is that world point on screen?" If so, the frame already contains its
true radiance — real lights, real shadows, decals, wet blood, PBR specular, emissive stages.

**What shipped (commit pending on `feat/rt-specular-occlusion`):** the core RR12b + RR12b′ in `ssr_rt.frag`
+ backend. The live-target read landed *clean, with no new barriers* — the HDR frame target already rests
in `SHADER_READ_ONLY` when its scene pass is suspended for the RT pass (VulkanBackend `FrameColorBetweenLayout`),
exactly how `RB_RHI_Fxaa` samples `rhiHdrRT`. A new RHI accessor `GetFrameColorSampleImage()` returns that
image (or 0 on the swapchain path / non-HDR, so RR12 self-disables where the scene isn't sampleable). The hit
view-pos is computed directly as `P + Nr·2 + Rv·t` (rigid ray → world t == view t), so **no matrix inverse**.
Cvar `r_rtReflSceneReuse` (default 1). **Deferred:** RR12a (factor the shade into a shared `rt_hit_shade.glsl`
— done inline for now; glass `environment_rt` keeps the key-light shade until that factoring), RR12c (the
emissive off-screen flag), and the `r_rtReflDebug` confidence view. **Gate risk to watch:** one write-after-read
on the scene image between the trace pass (samples it) and the composite (writes it two passes later) — the
intervening temporal pass gives slack and the resume carries a layout transition, but confirm under
`r_vkValidation 1` (sync); if it flags, the fix is a scoped dependency on the scene-pass resume.

- **RR12a — factor the hit shade.** Move the attribute fetch + shade (`ssr_rt.frag:134-161`, duplicated in
  `environment_rt.frag`) into a shared `rt_hit_shade.glsl` include. No behaviour change; build must stay
  byte-identical in output (A/B by eye is not needed — it is a textual move; validation soak only).
- **RR12b — reuse, cheap-path first.** At a committed, attributed hit compute `hitWorld = origin + dir·t`
  (from the ray `t`, **no** geometry fetch yet) → view space (forward view = the rigid inverse of
  `u_modelViewMatrix`, transpose-rotate; no new matrix uniform) → screen uv via the projection scalars
  already in `u_localParam0.xy`. **Confidence** = on-screen (edge fade of a few percent, like SSR's) ×
  depth agreement (`|sceneViewZ(uv) − hitViewZ| < k·|hitViewZ|`, k ≈ 1–2 %; rejects "the hit is behind
  something the camera sees") × not-the-view-weapon (normal-buffer alpha) — all from **one depth fetch**,
  before any attribute read. Then branch:
  - **confidence ≥ HIGH (on-screen):** `radiance = sceneColour(uv)` — one colour fetch and **return**,
    skipping the BDA geometry fetch + bindless diffuse sample entirely. This is the common case for a
    reflective floor facing the room, and it is *cheaper* than today's RR4c shade.
  - **confidence < HIGH:** run the factored fallback (`rt_hit_shade` — the RR4c geometry fetch + bindless +
    key light) and `radiance = mix( fallback, sceneColour(uv), confidence )`, the colour fetch predicated
    away when confidence is 0 (a genuinely off-screen hit pays only the fallback + the one depth fetch).
    The expensive dependent BDA read runs only where the screen cannot answer.
  Apply `r_ssrFireflyClamp` to the sampled radiance (HDR; the key-light path never needed it).
- **RR12b′ — the scene colour is the LIVE target, not a re-added blit (this corrects the earlier draft).**
  The first draft claimed the `_currentRender` snapshot is "already refreshed at the top of the RT path."
  It is **not** — #4a deliberately *drops* that snapshot under RT (item 6 of the `rtWillRender` block in
  `RhiWorld.cpp`) to save the full-res blit. The proper solution keeps that saving: bind the **live HDR
  scene-colour target** (the one the RT composite already writes into) as a sampled input on **unit 0** —
  the `DrawFullscreen` image arg that has been a throwaway normal-buffer dummy since RR7 — with a layout
  transition COLOR_ATTACHMENT → SHADER_READ before the `ssr_rt` trace pass, and back for the additive
  composite. No copy. The ordering is a clean read-then-write across two passes (Pass A *samples* the lit
  opaque scene; the composite, two passes later, *adds* onto it), so there is no same-pass hazard and no
  feedback loop: a reflective pixel reflects *other* pixels, which carry this frame's lit-but-pre-reflection
  colour — single-bounce, as intended. `environment_rt` (glass) forward-renders after the opaque scene is
  complete, so the same live target is valid there through the shared include.
- **RR12c — emissive fallback for off-screen hits.** One more bit in `RtGeoDesc.flags` (bit 0 is "has
  attributes"): "unlit/self-illuminated" — set at BLAS staging when the material has no lit stages (GUI
  surfaces, light fixtures, `_emission`-style additive-only materials). The fallback then shades
  `albedo · 1` instead of `albedo · (ambient + N·L)`. Off-screen light fixtures stop reflecting as grey panels.
- **Known, accepted error:** view-dependent terms in the reused colour (specular highlights, Fresnel) were
  evaluated for the camera, not for the reflected ray. SSR has always had exactly this error and nobody has
  flagged it; diffuse-dominant Doom 3 surfaces make it minor.
- **Monte-Carlo (Full-RT) alignment — where the effort should sit.** The screen-reuse path is a hybrid
  *accelerator* and explicitly transitional: the path tracer retires it, because a reflection is a specular
  bounce whose hit must be shaded by next-event estimation (sample a light → trace a visibility ray →
  accumulate + denoise), never by reading the screen — the hit is often off-screen, and the screen colour
  carries the camera-view-dependent terms noted just above (the very reason PT must shade at the hit). So
  keep the reuse path cheap to write *and cheap to delete*, and build nothing durable on it. The
  **fallback** hit-shade is the seam that grows into NEE: today a fake key light + ambient; tomorrow a
  sampled real light + a shadow ray (the H3/H5 shadow-ray machinery) evaluated against the **RR4 bindless
  material** at the hit — which is the actual Full-RT down-payment (the reuse-ledger "materials + geometry
  table" row, carried 100%; see [rtx-hybrid-roadmap.md](rtx-hybrid-roadmap.md)). Concretely: route the
  fallback through `rt_hit_shade` (RR12a) with the material fetch intact, and treat the confidence gate +
  screen reuse as removable scaffolding. Per that ledger reflections are low Full-RT leverage — RR12 is
  worth building for image quality now, and its one lasting piece is the material-shaded fallback, not the
  reuse. (This is also why RR13's stochastic-GGX + NRD-specular is deferred to land *with* H6, not here: the
  denoised-radiance path is the same one NEE will feed.)
- **Seam risk:** where confidence falls to 0 (screen edge, occluded hits) the reflection hands over to the
  key-light shade, which has a different brightness → a visible boundary that moves with the camera. Two
  mitigations, in order: the confidence ramp + the existing temporal accumulation smear it; if that is not
  enough, **calibrate the fallback** — scale the key-light shade by a slowly-adapting ratio of
  (mean reused luminance / mean fallback luminance) measured on confident pixels (the eye-adaptation
  reduction machinery already exists). Do not start with the calibration; gate first.
- **Cvar** `r_rtReflSceneReuse` (default 1 once gated; 0 = today's look for A/B) + a debug view
  (`r_rtReflDebug`: confidence as green→red) so the user can see *where* reuse engages without guessing.
- **Cost (with the cheap-path structure):** the trace (the dominant cost) is unchanged; RR12 only changes
  the shade. An on-screen hit is one depth + one colour fetch and *skips* the BDA geometry fetch + bindless
  sample today's shade always does → in an on-screen-dominant view (a reflective floor facing the room) a
  wash-to-modest **win**; an off-screen-heavy view (reflecting behind-camera geometry — where RT earns its
  keep over SSR) pays the full fallback + one wasted depth fetch → a slight loss. Net: scene-dependent,
  roughly a wash, *not* the flat add the naive structure implied, and with **no re-added blit** (the
  live-target bind, RR12b′). One scattered HDR fetch is the only new bandwidth. A/B `r_vkGpuTime` on
  `r_rtReflSceneReuse`.

### RR16 — real lighting at the reflection hit (the NEE seam, chosen 2026-09-19)

**Why.** RR12's gate proved the screen-reuse limitation the plan warned about: reuse can only shade a hit
whose reflected surface is *on screen*, so an off-screen-reflected surface drops to the fixed key light —
a soft-edged "overlay" of correct lighting that hugs the on-screen geometry, with the rest of the
reflection flat/dim. The user chose the proper fix: **shade the hit with the scene's real lights**, so a
reflection is lit correctly whether or not the reflected surface is on screen. This is the fallback the
Monte-Carlo note already named as "the NEE seam" — building it here is a direct Full-RT down-payment (it
*is* the path tracer's direct-lighting stage evaluated at a reflection bounce), not a transitional pass.

**Why it needs new infrastructure.** RT shadows (`r_rtAllLights`) trace their ray *per light, inside the
raster interaction loop*, where that one light's data is already bound — there is no all-lights buffer a
fullscreen pass can iterate. The reflection pass is fullscreen and hits arbitrary surfaces, so it needs
the view's lights gathered into a **device buffer it can loop** — read via `buffer_reference`, exactly
like the RR4/RR5 geo table (address bit-cast into a param, no descriptor).

**Staged (validator-first, opt-in, VK+RT only, gate each). RR16a+b BUILT 2026-09-19 (commit f7f6657a, gate
pending); RR16c BUILT + user-verified 2026-09-20 (`r_rtReflLightFrustum`) — this closes reflections for now;
RR16d parked as nice-to-have ([todo.md](todo.md)); RR16e deferred.**
- **RR16a — light buffer. BUILT.** Each view, gather `viewDef->viewLights` into a device buffer: per light
  `{ vec3 worldOrigin, float radius, vec3 color, uint flags }` (32 B). `worldOrigin = globalLightOrigin`;
  `color` from `vLight->shaderRegisters` × the light shader's colour stage (mirror `RhiWorld.cpp`'s light
  colour eval ~8117-8124); `radius = lightDef->parms.lightRadius.Length()`. Create per-view (host-visible,
  pre-filled) + retire fenced (the temp-buffer idiom); pass its device address + count in a free param
  (`color`/`diffuseModifier` free up once the key light goes). Validator `r_rtReflLightTest`: count ==
  view light count, addresses non-zero, no validation errors, inert on GL3/non-RT.
- **RR16b — diffuse point-light shade (no shadows).** In `ssr_rt.frag`, at the attributed hit compute the
  **world** hit (`wp + wN·2 + wr·t` — the world vectors already exist) and loop the light buffer:
  `L = origin − hitWorld`, `atten = sat(1 − |L|/radius)²` (Doom-ish inverse falloff), add
  `color · atten · max(N·L,0) · albedo`, plus a small ambient. This **replaces the fixed key light** as
  the shade — reflections now carry real light colour/direction on and off screen. Keep `r_rtReflSceneReuse`
  as the *exact* on-screen path over the top (the reuse↔real-shade seam is now tiny — both ≈ correct — so
  judge whether to keep the hybrid or default reuse off for one consistent method). Gate: off-screen
  reflections lit by the right lights; the RR12 overlay boundary gone.
- **RR16c — projected-light reach (faithfulness).** A point approximation over-lights: a Doom spot/box
  light reaches only inside its frustum. Add the 4 world-space `lightProject` planes (S/T/Q/falloff) to the
  buffer and gate the contribution on the hit projecting inside `[0,1]` (uniform-within-frustum; the
  projection/falloff *textures* — the gobo pattern — are a later bindless refinement, like RR4). Restores
  which surfaces a light may touch. Fidelity: opt-in, UN only.
  **BUILT 2026-09-20** (`r_rtReflLightFrustum`, default 1 inside the already opt-in `r_rtReflRealLights`; 0 =
  the RR16b sphere for A/B). The light-buffer row grows 32 → 96 B with the four global-space `lightProject`
  planes. In `ssr_rt.frag` the world hit is projected `s = S·p/Q·p, t = T·p/Q·p, f = F·p` and the term
  windowed to `[0,1]` with a soft 15 % border (every Doom light texture fades to black at its edge, so a
  hard box would be *less* faithful than a soft one). It applies to **every** light, not just spots: a Doom
  point light is a *box* (`lightProject` S/T span X/Y, falloff spans Z), so the RR16b `lightRadius.Length()`
  sphere over-reached past the box faces too. Point/parallel lights keep the RR16b radial falloff × the box
  window (core brightness unchanged, so `r_rtReflLightScale` needs no retune); spots (flags bit1) drop the
  radial term — they do not use `lightRadius` — for an axial `1 − f` start→end falloff, and hits behind the
  apex (`Q·p ≤ 0`) are rejected.
  **Perf lesson:** the first cut copied the whole 96 B row per light per reflective pixel and cost **+1.5 ms** —
  the loop is memory-bound, not ALU-bound. Fix: the row leads with the light volume's bounding sphere
  (`frustumTris->bounds`; 112 B total), the shader rejects on those 16 B, reads the other fields individually
  and skips the plane reads for back-facing hits; the CPU gather culls by the same sphere. Verified: frustum
  on is now *cheaper* than the RR16b sphere (`r_rtReflLightFrustum 0`), and looks right.
- **RR16d — shadow rays (r_rtReflShadows). PARKED 2026-09-20 — nice to have** (reflections judged excellent
  as-is; see [todo.md](todo.md)). Per contributing light, trace hit→light against the TLAS;
  zero the term if occluded. Reflected surfaces gain contact shadows. The dominant new cost (a ray per
  light per reflective pixel), so it is its own toggle; start off. This is literally NEE visibility — the
  H3/H5 shadow ray at a reflection bounce.
- **RR16e — reconcile + preset.** Decide reuse-on (hybrid) vs reuse-off (pure real-shade) by eye; specular
  term at the hit (GGX using the hit roughness) if worthwhile; perf; wire into Ultra Nightmare.

**Cost.** Diffuse loop is a few lights × a few ALU per reflective pixel — cheap. Shadow rays (RR16d) are
the real cost and stay opt-in. The trace that finds the hit is unchanged. **Retires** the fixed key light
and shrinks the reuse to an optional accelerator; in Full RT this shade *is* the specular-bounce direct
lighting, kept whole.
- **Side effect worth noting:** this delivers "blood / decals *in* reflections" for free whenever the
  decal is on screen — distinct from RR15, which is reflections *on* blood.

### RR13 — roughness-driven glossy for the RT tier

Restore at UN what `r_ssrGlossy` gives the SSR tier, on the RT buffer.

- Build the `ssr_colordown` pyramid over the **temporal-upscaled RT result** (the full-res history that the
  composite reads today) into a mipped target, and have `ssr_rt_composite` sample it at
  `LOD = glossyMaxLod · r_ssrGlossyScale · f(roughness)` — the same mapping `ssr_composite` uses
  (`RhiWorld.cpp` stage 2.5, `localParam1[3]`), so the two tiers agree on what a given roughness looks like.
  Reuse `r_ssrGlossy` / `r_ssrGlossyScale`; no new user-facing knobs.
- **Alpha matters here and not in SSR's pyramid:** the RT buffer is *weighted radiance + hit mask in
  alpha*, and `ssr_colordown` is a plain 4-tap average. Averaging hit and miss texels darkens every
  reflection edge at high LOD. The buffer is effectively premultiplied, so the fix is in the composite,
  not the downsample: `rgb / max(a, ε) · smoothstep(a)` — renormalise by the blurred coverage, then fade by
  it. Verify on a reflection with a hard miss boundary (sky through a window) before anything else.
- The RR6b box blur **stays** as the LOD-0 floor (its job is the static firefly grid, not glossiness); the
  RR8 full-res material mask stays last, unchanged — it is what keeps a blurred reflection from haloing
  past its surface, and it matters more once the blur is wide.
- **RR13b — contact hardening (optional, after the gate).** Real glossy reflections are sharp at the
  contact point and blur with distance. Pack the hit distance into the RT target's alpha (alpha is only
  tested `> 0` today — confirm in `ssr_temporal`'s hit-aware feedback before repurposing it) and scale the
  LOD by `saturate( t / refDist )`. A monster's feet reflect crisp, its head soft. Cheap, and the single
  biggest "this is ray traced" cue after RR12.
- **Deliberately not now:** stochastic GGX rays + NRD ReBLUR-specular. It is the physically right answer
  and a genuine Full-RT down-payment (see [rtx-hybrid-roadmap.md](rtx-hybrid-roadmap.md)), but it needs
  hit-distance + spec-MV guides, multiplies the denoiser cost, and re-opens the device-lost surface H4
  just closed. Revisit it *with* H6 (GI), which needs the same NRD radiance plumbing; the pyramid is
  retired then, not before.
- **Cost:** the pyramid is ~6 successively-halved passes, but over a **full-res** source (SSR's runs at
  `ssrRes`), so do not assume SSR's number carries over — measure with `r_vkGpuTime`, A/B on `r_ssrGlossy`.
  If it is too dear, start the chain from a half-res first level (glossy content has no full-res detail).

### RR14 — specular occlusion on reflections. DONE + user-verified 2026-09-19 ("0.7 is fine").

- Scale the reflection by a roughness-aware specular-occlusion term (Lagarde, *Moving Frostbite to PBR*:
  `saturate( pow( NdotV + ao, exp2( −16·rough − 1 ) ) − 1 + ao )`) read from the frame's finished AO
  buffer (`rhiSsaoResultRT` — GTAO or the RTAO resolve, whichever ran), blended by `r_specOcclusion`
  (0..1). A near-mirror's tight lobe is barely occluded (exponent → 0.5, forgiving); a rough-glossy lobe
  is occluded like the diffuse hemisphere (exponent → 0, term → `ao`). Mirror in a crevice still mirrors;
  rough-glossy floors/metal lose the reflected "glow" in corners and under props.
- **Where it's applied (differs from the first sketch, which said `ssr_rt_composite`).** The term needs
  `NdotV` + roughness, which the RT *composite* (`ssr_rt_composite.frag`) does not have — it only sees the
  finished RT radiance + the material buffer. So the two tiers apply it where those values already exist:
  - **SSR tier:** `ssr_composite.frag` — has N, V, roughness; term folded into the final
    `refl · (F · gloss · intensity · specOcc)`.
  - **RT tier:** `ssr_rt.frag` (the trace+shade pass, *not* the composite) — has `Nr`, V, roughness; term
    folded into `weight` **after** the `weight < 0.002` discard gate (so coverage is unchanged) and
    **before** the temporal/blur passes, so the occlusion is accumulated and blurred with the reflection.
  Both take the strength in `localParam1.x` and sample AO on **unit 4** (`SAMPLER_BINDING(4) u_aoBuffer`,
  R channel). The backend (`RhiWorld.cpp`) binds `rhiSsaoResultRT` there when `rhiSsaoAppliedThisView`,
  else `globalImages->whiteImage` (ao = 1 → term = 1) and forces strength 0 — doubly inert with no AO.
  SSR path mirrors the Hi-Z unit-4 idiom (GL3 tmu-cache invalidation); the RT path is VK-only.
- **Scope, stated honestly:** *reflections only* — the indirect specular. Direct specular from the
  interaction pass is **not** AO'd (that would be physically wrong, and it is the wet-blood glint path the
  user just tuned). Cube-map `environment` stages on opaque surfaces + glass (`environment_rt`) are
  possible later consumers; left out until the gate.
- Works on GL3 + VK, SSR and RT tiers alike (the only stage here not UN-only). Inert when AO is off this
  view or `r_specOcclusion 0` (the default) — every current preset stays byte-identical.
- **Default + exposure (post-gate).** `r_specOcclusion` default is **0.7** (user-approved). It is a global
  reflection tuning knob, **not** preset-wired — exactly like `r_ssrIntensity` / `r_rtReflBlur`: the presets
  own the on/off masters (`r_ssr`, `r_rtReflections`, `r_ssao`) and this rides them (inert wherever
  reflections or AO are off, so it self-activates at Ultra via GTAO and Ultra Nightmare via RTAO). A
  **"Specular Occlusion"** slider sits in the Debugging tab → *Screen-Space Reflections (SSR)* group,
  beside Reflection Intensity / Roughness Cutoff (the other knobs shared by both reflection tiers); the
  group disables with `r_ssr`, which gates the whole reflection pipeline.
- **Verify (build side):** clean build; `SPIR-V … 0 failures`; `spirv-dis` confirms `u_aoBuffer` at
  Binding 4 in the recompiled `ssr_composite`/`ssr_rt` blobs. A/B any time with `r_specOcclusion 0`.

### RR15 — reflections on wet blood (optional)

The design is already written up in [todo.md](todo.md) (decal G-buffer pass: **normal first, roughness
second**, per the dead end recorded in readme-changes 1.7). It belongs *after* RR13 because a blood pool
is a rough-glossy reflector — without roughness-driven blur it would mirror like chrome, which is the
look the decal work spent a day getting away from. Stays parked unless the user pulls it.

### Verification pattern (same for every stage)

Build + static checks only on this side: clean build, `SPIR-V … 0 failures`, **re-run cmake when a new
shader file is added** (new files are not dependencies until reconfigure), confirm `build/dude` mtime.
Every visual and runtime gate — `r_vkValidation 1` soak, `r_vkGpuTime` A/B, and all "does it look right"
judgements — is the user's; each stage ships a cvar that restores the previous look exactly so the A/B is
one console line. No stage is preset-wired before its gate passes.

### Not reflection work — tracked in todo.md, not here

The same fidelity review also produced items outside this document's scope: screen-space **contact
shadows** for the raster tiers (pointless at UN, where `r_rtAllLights` already traces every light),
**volumetric light shafts**, and **skin/flesh shading**. RT GI is H6 in
[rtx-hybrid-roadmap.md](rtx-hybrid-roadmap.md). See [todo.md](todo.md), "Fidelity backlog".

## Risks & mitigations

- **Shading an off-screen hit with no material texture.** MVP shades from `gpuSkinVB` vertex colour ×
  a tint + sun/ambient — crude but recognisably the monster, lit consistently. Sampling the actual
  diffuse texture at the hit (bindless + st fetch) is an RR3+ refinement, not required for the win.
- **Cost.** Reflective pixels are a minority (gated by roughness/metalness), and RT runs only where
  SSR can't help. Trace at half-res + temporal (RR3) if needed. Measured in RR4; `r_rtReflections 0`
  reverts.
- **BDA / lifetime.** The table holds device addresses valid for the frame; monster `gpuSkinVB`/index
  addresses are stable per surface (rebuild the row on topology change, same trigger as the animated
  BLAS rebuild). Static rows are zero (never dereferenced). Guard every fetch on `vtxAddr != 0`.
- **Sync.** The reflection trace reads the TLAS (built in `RefreshAnimBlas`, already barriered to the
  fragment/compute ray-read stage) and the `gpuSkinVB` (written by the skin compute, barriered by the
  R3.5 S0 compute→AS-build/…→shader-read scope). The reflection compute pass slots after those, before
  the composite.
- **Non-RT / GL3 / SSR-off:** every path gates on `SupportsRayQuery()` + `r_rtReflections`; SSR and
  probes are untouched and remain the default.

## Not in scope (follow-ups this unlocks)
Full RT GI (indirect bounce off the same table), RT reflections of *world* dynamic movers with proper
material shading, denoised many-bounce reflections. The geometry table + hit-shading built here is the
shared substrate for those.
