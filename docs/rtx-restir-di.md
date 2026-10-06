# ReSTIR DI — reservoir direct lighting (the first H7 / `dude_rtx 2` piece)

> Status: **PLAN** (authored 2026-10-06). No code yet. This is the foundation plan;
> each stage below is implemented, validated, and merged on its own branch before the
> next. Part of the Full-RT end-state in [rtx-hybrid-roadmap.md](rtx-hybrid-roadmap.md);
> the direct-lighting half of H7. Build **DI first, then GI**.

## Where it lives (naming corrected 2026-10-06)

The old framing — "preset index 7, tier *The Dark Lord Cometh*" — is **obsolete**. RTX was
decoupled from the raster preset ladder into an orthogonal axis:

- `dude_preset` ([Common.cpp:105](../neo/framework/Common.cpp#L105)) is raster only, **0..5**
  (Potato … Nightmare). The old raster "Ultra Nightmare" preset (idx 6) is retired.
- `dude_rtx` ([Common.cpp:110](../neo/framework/Common.cpp#L110)) is the RT axis, **0..2**:
  `0` off, `1` hybrid RT (today's "Solid Hybrid RT"; `dude_preset 5 + dude_rtx 1` reproduces
  the retired Ultra Nightmare look), **`2` = full path tracing — H7**, already reserved in the
  cvar's own comment ([Common.cpp:107](../neo/framework/Common.cpp#L107)).

So **ReSTIR DI targets `dude_rtx 2`**, and that tier reclaims the now-free name **"Ultra
Nightmare"** (not "The Dark Lord Cometh"). Hybrid stays `dude_rtx 1`.

**Cleanup owed by the rename** (stale since the UN-preset removal, fix alongside Stage 0):
- `presetNames[]` still has a dead `"Ultra Nightmare"` at index 6 with a `preset < 7` guard
  ([Session.cpp:1161](../neo/framework/Session.cpp#L1161)) — unreachable now that `dude_preset`
  clamps to 5.
- The `dudePreset` command help still says "0 = Potato .. 6 = Ultra Nightmare"
  ([Common.cpp:2633](../neo/framework/Common.cpp#L2633)) — should read 0..5.
- Decide the `dude_rtx 2` display label in both menus (classic `mainmenu.gui` RT selector +
  the ImGui Graphics tab): "Ultra Nightmare" as the RT-axis top rung. Note it coexists with the
  **g_skill 4 "Ultra Nightmare" difficulty** (a different axis — gameplay permadeath, see
  [Session.cpp:1545](../neo/framework/Session.cpp#L1545)); the two are unrelated and that's fine.

## Why ReSTIR DI is the right first H7 piece

Doom 3 scenes routinely have dozens–hundreds of point/projected lights in view. Today's hybrid
tier (`r_rtAllLights`) traces **one visibility ray per light per pixel** — the exact cost +
stagger that killed H3 soft shadows (see [rtx-soft-shadows-h3.md](rtx-soft-shadows-h3.md)).

ReSTIR DI (Bitterli et al. 2020) importance-samples the light set into **one chosen light +
one visibility ray per pixel**, regardless of light count, and spatiotemporally reuses
neighbours' and last frame's samples to drive variance down. The single visibility ray *is* the
abandoned H3 shadow ray — reused, not rebuilt. It is the direct-lighting stage of the path
tracer, so ~100% of it carries into full ReSTIR GI afterwards.

## Substrate it rides (reuse, do not reinvent)

| Need | Where it already is |
|------|---------------------|
| **Candidate pool** (scene-light SSBO) | `RtLight[]` buffer_reference, **max 256 lights** (`RT_REFL_MAX_LIGHTS`, RhiWorld.cpp:709), struct + `dudeLightInRange`/`dudeLightDir`/`dudeLightAtten` in [rt_light_eval.glsl:15](../neo/shaders/rt_light_eval.glsl#L15); filled CPU-side by `RB_RHI_GatherSceneLights` ([RhiWorld.cpp:5424](../neo/renderer/rhi/RhiWorld.cpp#L5424)), triple-buffered + frame-cached, returns a device address. Today consumed only by RT reflections / volumetrics / RTGI-bounce — **no visibility consumer yet; ReSTIR DI is the first.** Keep it the one shared light model so DI/RTGI/volumetrics never drift. |
| Visibility ray vs TLAS | Inline any-occluder test (shadow mode 4) `interaction.frag:228-234`; standalone NaN-guarded pattern [rtshadow_ray.frag](../neo/shaders/rtshadow_ray.frag) (origin = pos + `wn·u_rtParms.z`, `tMax = min(distLight−1, u_rtParms.w)`). TLAS = `r_rtWorld` (`GetTlasAddress()`), animated geo = `r_rtAnimBlas`. Perforated-grate helper `rt_perforated.glsl`. |
| G-buffer at shade time | `RB_RHI_NormalPrepass` ([RhiWorld.cpp:4395](../neo/renderer/rhi/RhiWorld.cpp#L4395)) → MRT from `gbuffer.frag`: loc0 **view-space** normal (A = AO/weapon mask), loc1 (roughness, metalness), loc2 velocity, loc3 albedo (only on the 4-MRT RTGI prepass). **No world-position buffer** — reconstruct from `currentDepthImage` + `u_depthParms` (recipe in `rtshadow_ray.frag:98-109`). |
| Denoiser | NVIDIA NRD v4.18, H2 plumbing ([rtx-nrd-h2.md](rtx-nrd-h2.md)). ⚠ **SIGMA_SHADOW is instantiated but has no dispatch path** — only ReBLUR is actually dispatched (`RtaoDenoise`). Shadow/signal denoising for ReSTIR = net-new CPU plumbing on the existing `NrdRecordDispatches` machinery (see Stage 5). |
| Motion vectors + temporal history | MVs in `gbuffer.frag` loc2 (jitter-cancelled). Shared helper `RB_RHI_TemporalReproj` ([RhiWorld.cpp:540](../neo/renderer/rhi/RhiWorld.cpp#L540), `prevUsable` handles first-frame/teleport); variance-clip history in `ssr_temporal.frag`; NRD confidence via `rtgi_confidence.frag`. |
| RT pass template | `RB_RHI_RtaoPass` ([RhiWorld.cpp:6848](../neo/renderer/rhi/RhiWorld.cpp#L6848)) / `RB_RHI_RtgiPass` — the full recipe: `SupportsRayQuery()` + cvar gate, `GetTlasAddress()==0` bail, `LoadShader`, MRT target, TLAS addr bit-cast into `rtParms`, `BeginTargetPass → DrawFullscreen → EndPass`. |

### Infrastructure constraints (house rules — the pass must fit these)
- **Fullscreen *fragment* passes, not compute.** `.frag` + the shared fullscreen `.vert`, dispatched
  via `BeginTargetPass → RB_RHI_DrawFullscreen → EndPass`. (Compute exists — `r_vkComputeTest` — but
  every shipped RT pass is fragment; follow that unless a reservoir pass forces compute.)
- **≤2 descriptor sets, ≤16 samplers/stage, no push_constant blocks** ([shaders/README.md](../neo/shaders/README.md)).
  All per-draw scalars go through the 1056-B `rhi::RenderParams` UBO (set 0, size `static_assert`-locked);
  samplers are set 1. **The TLAS and every SSBO are reached by device address bit-cast into the
  uniform** (`u_rtParms.xy` for the TLAS) — there is no AS/SSBO descriptor binding to extend. So the
  reservoir buffers ride as `buffer_reference` SSBOs whose device addresses are passed in `RenderParams`.
- **Shaders compile offline** (`compile_spv.py` → `embed_shaders.py` → `shaders_embedded.cpp`, target
  vulkan1.4). New `restir_*.frag` files are **not built until CMake re-configures** — the `file(GLOB)`
  gotcha ([cmake-glob]); touch `neo/CMakeLists.txt` after adding them.
- New RT image targets: add `CreateRenderTarget*` in `RHI.h` + `VulkanBackend.cpp`, handle/size/ready
  statics near RhiWorld.cpp:764, and reset the handle in the context-loss path.

## Algorithm (mapped to our light model)

A per-pixel **reservoir** holds one chosen light sample and the statistics to shade it unbiasedly:

```
struct Reservoir {
    uint  y;      // chosen light index into RtLight[]  (0xffffffff = empty)
    float wSum;   // running sum of RIS weights
    float M;      // candidates seen (float: temporal/spatial M clamps aren't integers)
    float W;      // unbiased contribution weight for y
};
```

**Target function** `p̂(x)` for a candidate light `li` at shading point `p` (normal `n`, view `v`):

```
p̂ = luminance( BRDF(n, v, ldir) * RtLight[li].color * dudeLightAtten(li, p) )
```

- `ldir`, distance from `dudeLightDir`; `dudeLightAtten` already encodes radial + frustum/spot
  falloff. BRDF = our interaction BRDF. **Stage 1 uses diffuse-only `p̂`** (Lambert) for
  stability; specular is added to the target in Stage 6.
- Candidates skipped early with `dudeLightInRange` (one 16-B cull read).

**RIS** per pixel: draw `M` candidates from a source pdf, update the reservoir with
`w_i = p̂(cand) / pdf(cand)` via weighted reservoir sampling. Final unbiased weight
`W = (1/p̂(y)) · (wSum / M)`. Shaded radiance = `p̂(y) · W · visibility(y)`.

Source pdf: **Stage 1 uniform over in-range lights** (simplest, correct); upgrade to
power/solid-angle importance once the pipeline is proven.

## Staging — each shippable, validator-gated, own branch

Everything gates on `SupportsRayQuery()` + the feature cvar; raster/GL3 paths untouched.

**Stage 0 — Scaffold & decision.** This doc; `r_rtReStirDI` cvar (0 default, RT-capable only);
reservoir SSBO (ping/pong) + output image registered off the rtgi/rtao template; confirm exact
bindings; do the naming cleanup above. Interim landing decision: ReSTIR DI first ships as an
**alternative to the `r_rtAllLights` brute loop inside `dude_rtx 1`** (so it's measurable on real
maps via `r_vkGpuTime 2`), then is promoted to own the direct lighting at `dude_rtx 2`.

**Stage 1 — RIS, no reuse, no visibility.** Fullscreen fragment pass: read depth→world-pos +
view normal + material, build candidates from `RtLight[]` (cull with `dudeLightInRange`), WRS-pick
one, write the reservoir SSBO; resolve `p̂(y)·W` (unshadowed). **Validator `r_rtReStirDI_test`**
(the `r_rtWorldTest` shape — build/trace a known case, diff vs CPU, bucket FP flips, print PASS/FAIL,
self-clear): CPU reference runs the same fixed-seed WRS over N pixels and diffs the selected-light
histogram + mean `W` against the GPU, and against the brute `r_rtAllLights` *unshadowed* sum (means
must match within noise). Note `r_rtAllLights` lights via **per-light interaction draws**, not the
SSBO — so ReSTIR DI is a *different* lighting model (SSBO + reservoir) that will eventually replace it.

**Stage 2 — One visibility ray.** Trace a single shadow ray to `y` reusing the inline any-occluder
pattern (`interaction.frag:228-234` / `rtshadow_ray.frag`): origin `p + wn·offset`,
`tMax = min(dist−1, u_rtParms.w)`, `TerminateOnFirstHit`; multiply resolve by visibility. Honour the
`NOSHADOWS` flag (skip the ray); projected-light frustum gating already lives in `dudeLightAtten`;
perforated grates via `rt_perforated.glsl`. This is the H3 ray — now once/pixel regardless of light count.

**Stage 3 — Temporal reuse.** Reproject last frame's reservoir through the FSR2 MVs, combine with
the current reservoir by WRS, **clamp temporal `M`** (≈20× current) to bound stale bias — this is
what cures the H3 stagger. Reuse the existing disocclusion / normal-depth history rejection.

**Stage 4 — Spatial reuse.** k neighbour taps in a shrinking radius, combined with normal/depth
rejection (and the bias-correcting Jacobian), 1–2 passes. This is what lets one sample/pixel
effectively cover hundreds of lights.

**Stage 5 — Denoise + unify.** Feed the 1-spp result to NRD, reusing the H2 guides (packed
normal-roughness, viewZ, engine→NRD motion). ⚠ **New plumbing required:** SIGMA_SHADOW exists in the
NRD instance (`VulkanBackend.cpp:6645`) but has **no dispatch path** — only ReBLUR is wired
(`RtaoDenoise`). Add a shadow/diffuse dispatch on the existing `NrdRecordDispatches` machinery (model
it on `RB_RHI_RtaoPass`'s NRD block, RhiWorld.cpp:6982+). Decide SIGMA (shadow signal) vs ReBLUR/ReLAX
(full diffuse) by measurement. Then **ReSTIR DI replaces the `r_rtAllLights` per-light draw model as
the direct-lighting path at `dude_rtx 2`.**

**Stage 6 — Specular target + polish.** Add GGX specular to `p̂` (MIS or a separate specular
reservoir if the single reservoir gets noisy); handle VIEWWEAPON / muzzle lights; perf pass with
`r_vkGpuTime 2` scopes; set the `dude_rtx 2` "Ultra Nightmare" preset defaults.

## Non-negotiables (carried from every prior RT phase)
- `SupportsRayQuery()` + cvar gate; default off; raster/GL3/non-RT paths bit-for-bit unchanged.
- Validator-first: `r_rtReStirDI_test` diffs GPU vs a CPU reference before the effect renders.
- Runs at internal (FSR2) resolution like the other RT effects; FSR2 is the perf backstop.
- New shaders: re-run CMake configure so the glob picks them up ([cmake-glob gotcha]).

## Resolved
- **Reservoir storage = `buffer_reference` SSBO** (ping/pong), device address passed in
  `RenderParams` — matches how the TLAS + `RtLight[]` are already reached (no new descriptor set,
  respects the ≤2-set / no-push-constant baseline). Row = `{ uint y; float wSum, M, W; }` (16 B).
- **DI runs at internal (FSR2) resolution** like the other RT effects.

## Open questions (settle during Stage 0–1)
- Candidate count `M` (start 8–16) and when to move off the uniform source pdf (→ power/solid-angle).
- One combined reservoir vs split diffuse/specular (defer; Stage 6).
- Whether spatial reuse (Stage 4) stays a fragment pass or is the one place compute earns its keep.
- Whether to extend `RtLight[]`'s 256 cap for pathological light counts, or rely on reservoir reuse
  to cover overflow (prefer the latter — that's the whole point).
