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
- **RR5 — RT WORLD reflections (replaces the snapshot glass system).** The user's goal: today glass /
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
- The `_currentRender` snapshot is **preserved** (refreshed once at the top of the RT path, VK-only): downstream
  refraction / heat-haze materials sample `_currentRender` and the RHI backend has no guaranteed per-view refresh
  after SSR (the smoke-dark capture is conditional), so skipping it risked a stale-background regression. It's a
  single cheap blit vs. the multi-ms march it replaces, so the win stands; skipping it too is a measured follow-up.

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

**Stage 2 (follow-up):** `bumpyenvironment_rt` for bumped glass (reuse its existing world-space frame +
flatten toward the vertex normal for stability); optional Fresnel; a real sky/fog-colour miss fill.

## Entity coverage — the room's props + dropped items should reflect too (PLANNED)

Today reflective floors show the worldspawn (RR5) + monsters (RR2), but **static entity props reflect as
nothing** and **runtime-dropped items don't either** — a reflection ray hits their positions-only defer row
(`flags & 1 == 0`) and returns 0. Two items, one shared mechanism: give non-monster entities *attributed*
BLASes so `ssr_rt`/`environment_rt` can shade the hit. The template is `R_RtBuildWorldAreaBlas`
(`tr_main.cpp:1847`, RR5a-3), which already builds per-surface `CreateBlasFromBuffers` BLASes for the world.

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
