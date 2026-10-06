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

**Cleanup owed by the rename** (stale since the UN-preset removal) — ✅ done in Stage 0:
- ~~`presetNames[]` dead `"Ultra Nightmare"` at index 6 + `preset < 7` guard~~ → trimmed, `sizeof`-driven
  ([Session.cpp:1161](../neo/framework/Session.cpp#L1161)).
- ~~`dudePreset` help "0 = Potato .. 6 = Ultra Nightmare"~~ → "0 = Potato .. 5 = Nightmare"
  ([Common.cpp:2633](../neo/framework/Common.cpp#L2633)).
- ~~`PATHTRACED`/"The Dark Lord Cometh" comment~~ → "Ultra Nightmare" ([Dhewm3SettingsMenu.cpp:2794](../neo/framework/Dhewm3SettingsMenu.cpp#L2794)).
- Still TODO (deferred until the tier works): the `dude_rtx 2` display label in both menus (classic `mainmenu.gui` RT selector +
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

Source pdf: a **flat power·BRDF·1/d² alias/CDF table over the 256 lights**, ~**8 candidates**
(the RTXDI `numLocalLightSamples` default — candidates are relevant enough that 8 beats Bitterli's
original 32 once importance-sampled). Uniform sampling is kept only as a validator cross-check. See
**Performance** below — at 256 lights this flat table *is* the light-tile; no ReGIR/BVH/tiling.

## Staging — each shippable, validator-gated, own branch

Everything gates on `SupportsRayQuery()` + the feature cvar; raster/GL3 paths untouched.

**Stage 0 — Scaffold & decision. ✅ DONE 2026-10-06.** Landed: this doc; the gated bring-up cvar
`r_rtReStirDI` (0 default, `CVAR_ARCHIVE|BOOL`, in `RenderSystem_init.cpp` by `r_rtAllLights`, help
notes "nothing consumes it yet → 0 leaves play byte-identical", the same way `r_rtgi` first landed);
and the UN-preset-removal naming cleanup — dead `presetNames[]` "Ultra Nightmare" + magic `< 7` guard
→ `sizeof`-driven (`Session.cpp`), `dudePreset` help "0..6" → "0..5 = Nightmare" (`Common.cpp`), and
the stale `PATHTRACED`/"The Dark Lord Cometh" comment → "Ultra Nightmare" (`Dhewm3SettingsMenu.cpp`).
Builds clean. **Deferred to the front of Stage 1:** the reservoir SSBO (ping/pong) + output target and
their exact bindings — allocated *with* the pass that consumes them (no dead GPU resources; verifiable),
read off the `RB_RHI_RtaoPass` template. Interim landing decision stands: ReSTIR DI first ships as an
**alternative to the `r_rtAllLights` brute loop inside `dude_rtx 1`** (measurable on real maps via
`r_vkGpuTime 2`), then is promoted to own direct lighting at `dude_rtx 2`.

**Stage 1a — RIS, no reuse, no visibility. ✅ DONE 2026-10-06.** Fullscreen fragment pass
`restir_di.frag` (+ `.vert`): read depth→world-pos + view normal (shared RTAO prepass), draw
`M = r_rtReStirDICandidates` (8) candidates **uniformly** from `RtLight[]`, WRS-pick one, resolve the
unbiased **unshadowed** irradiance `rad(y)·W` (diffuse, albedo = 1, Doom light model via
`dudeLightDir`/`dudeLightAtten`, frustum-gated) into a full-res RGBA16F target. `RB_RHI_ReStirDIPass`
mirrors `RB_RHI_RtaoPass`; wired after the RTGI pass under the `restir.di` GPU scope; gated on
`r_rtReStirDI` + VK + primary view + the depth/normal prepass (same dependency as `r_rtShadowBlur`:
SSAO/RTAO/motion-vectors/FSR2 on). `r_rtReStirDIDebug 1` blits the (grainy, 1-spp, expected) result
over the scene via `restir_di_debug.frag` for eyeballing against `r_rtAllLights`. Builds clean; off by
default so the scene is byte-identical. **No persistent reservoir SSBO** — with no reuse the reservoir
is a per-pixel local; the ping-pong buffer lands with temporal reuse (Stage 3). Full-res for bring-up;
internal-res scaling is a later perf lever. `r_rtAllLights` lights via **per-light interaction draws**,
not the SSBO, so ReSTIR DI is a *different* model that will replace it.

**Stage 1b — validator `r_rtReStirDITest` (NEXT).** The `r_rtWorldTest` shape (build a known case, diff
GPU vs a CPU reference, bucket FP flips, print PASS/FAIL, self-clear): a CPU weighted-reservoir reference
with a fixed seed over N pixels, checking the selected-light histogram + mean `W` against the GPU and
against the brute `r_rtAllLights` *unshadowed* sum (means must match within noise).

**Stage 2 — One visibility ray.** The initial RIS target is **unshadowed** (shadowing every
candidate is the cost we're avoiding); trace a single shadow ray only for the *chosen* sample `y`,
reusing the inline any-occluder pattern (`interaction.frag:228-234` / `rtshadow_ray.frag`): origin
`p + wn·offset`, `tMax = min(dist−1, u_rtParms.w)`, `TerminateOnFirstHit`. **Fold that visibility
into the stored reservoir** (RTXDI's `StoreVisibility`) so Stages 3–4 reuse is shadow-correct and
the shading pass can reuse it (see Performance). Honour `NOSHADOWS` (skip the ray); projected-light
frustum gating already lives in `dudeLightAtten`; perforated grates via `rt_perforated.glsl`. This
is the H3 ray — now once/pixel regardless of light count.

**Stage 3 — Temporal reuse.** Reproject last frame's reservoir through the FSR2 MVs, combine by WRS,
**cap confidence/`M` at ≈20** (RTXDI `maxHistoryLength`; range 5–30) — this bounds correlation/boiling
*and* cures the H3 stagger (it's the same lever as a low vs high cap: low reacts fast to lighting
changes, high is smoother). Reset confidence to 0 on disocclusion **from G-buffer tests only** (never
from sample contents — that biases). Turn on the **temporal visibility shortcut** (skip the
bias-correction ray when the sample carried over from last frame — RTXDI reports **>90% fewer rays**,
near-free) and the **boiling filter** (strength ≈0.2) feeding NRD. Prefer bias mode **Basic**, not
`Raytraced` (which would re-add rays).

**Stage 4 — Spatial reuse (OPTIONAL — only if NRD+FSR2 output is still noisy).** The literature is
clear this is diminishing returns at 256 lights. If needed: **1 tap at ~32 px** radius (+8
disocclusion-boost taps only on disocclusion, RTXDI defaults), normal/depth rejection, **defensive
pairwise / contribution MIS — O(M), never the O(M²) balance heuristic** — and keep bias mode
`Basic`/`Pairwise`, not `Raytraced`, to hold the one-ray/pixel budget. Can fuse with Stage 3
(spatiotemporal) rather than a separate pass.

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

## Performance (literature-calibrated, 2026-10-06)

Two independent scans of the ReSTIR literature + the NVIDIA RTXDI SDK source converge on one verdict:

> **At ≤256 analytic lights on a 3080 Ti, build ReSTIR's cheap core — not full production ReSTIR.**
> The famous 100× / "millions of lights" wins come from machinery (world-space ReGIR grids, 128×1024
> light tiles, light BVHs) that exists to tame light *counts we don't have*. The SIGGRAPH 2023 course
> says tiling doesn't pay off below ~128k lights; RTXDI's own tile budget is "≤256 unique lights per
> 16×16 screen tile" — our **entire** light set is one tile. At 256 lights, "which light?" is trivial:
> a flat power·BRDF·1/d² alias table *is* the light-tile. The only thing worth keeping from ReSTIR is
> **resampling to one shadow ray/pixel with a good, temporally-stabilised distribution.**

**The GPU-affordability answer (this is what makes it cheap enough).** Our one cost is the visibility
ray. Two near-free, low-bias tricks cut even that below one ray/pixel amortised:
- **Temporal visibility shortcut** — when the chosen sample carried over from last frame, its
  visibility was already established, so skip the temporal bias-correction ray. RTXDI: **>90% fewer
  rays.** Biggest single win; bake into Stage 3.
- **Final-shading visibility reuse** — store the shadow result in the reservoir and reuse it while
  fresh (`maxAge≈4`, `maxDistance≈16`) instead of re-tracing. Its shadow-edge brightening bias
  partially cancels reuse's darkening bias. Stage 3/5.
- DUDE-specific angle: the related "ReSTIR-sampled shadow maps" work resolves visibility from an
  existing **shadow-map cache** — which DUDE already maintains. A candidate for a *cheaper* visibility
  source on a lower RT rung later; note, don't build now.

**Starting knobs (RTXDI/course defaults — calibrate with `r_vkGpuTime 2`):**

| Knob | Start | Note |
|------|-------|------|
| Initial candidates `M` | **8** | from the flat power·BRDF·1/d² alias table; +1 BRDF sample |
| Confidence / `M` cap | **20** | range 5–30; the boiling + stagger lever |
| Spatial taps | **0–1** | 1 tap @ ~32 px only if still noisy; +8 on disocclusion |
| Bias correction | **Basic** | or Pairwise; **never Raytraced** (re-adds rays) |
| Temporal visibility shortcut | **on** | >90% ray cut |
| Final-shading visibility reuse | **on** | age 4 / dist 16 |
| Boiling filter | **on**, ~0.2 | cheap firefly guard feeding NRD |
| Reservoir row | **16 B** | `{ uint y+valid; float W, wSum; uint M+vis+age }`; analytic lights need no triangle-UV field (RTXDI's 24 B does) |

**Consciously skip at 256 lights:** ReGIR / world-space or spatial-hash grids (add build cost *and*
bias; "ray count only vital on very low-end RT hardware"); 128×1024 light tiles + per-light-type
presampling (build overhead > benefit below ~128k lights); light BVH / Stochastic Lightcuts; MCMC
decorrelation (MSE-neutral, glossy/PT-oriented); Area ReSTIR (AA/DoF/PT — owned by the raster
G-buffer + FSR2); checkerboard + custom reservoir bit-packing beyond the 16 B row (bandwidth isn't the
limiter here); Tokuyoshi statistical visibility reuse + stochastic large-kernel pairwise MIS (for
massive emissive geometry / large reuse kernels).

**Honest caveat on numbers:** there is **no published per-pass Ampere ms for ReSTIR DI**. Datapoints
are a path-tracing prototype (San Miguel 10M tris, sun/sky, <8 ms, GPU unstated) and the
candidate-generation pathology tiling fixes (25 ms — which doesn't apply to us). So the budget is set
by **our own measurement**, not a paper. Expectation: the core (initial RIS + temporal + the two
visibility cuts) should be cheaper than today's `r_rtAllLights` per-light-ray loop in light-dense
scenes, which is the whole point.

**Key sources:** SIGGRAPH 2023 course *A Gentle Introduction to ReSTIR*
(https://intro-to-restir.cwyman.org/); Wyman & Panteleev, *Rearchitecting Spatiotemporal Resampling
for Production*, HPG 2021 (https://doi.org/10.2312/hpg.20211281); NVIDIA RTXDI SDK + docs
(https://github.com/NVIDIA-RTX/RTXDI — see `Doc/Confidence.md` for the visibility shortcut); GRIS,
Lin et al. SIGGRAPH 2022 (https://doi.org/10.1145/3528223.3530158); Bitterli et al. 2020
(original ReSTIR DI). Deferred/paywalled, confirm numbers before quoting: Tokuyoshi, *Efficient
Visibility Reuse*, HPG/I3D 2024 (https://doi.org/10.1145/3641233.3664320).

## Non-negotiables (carried from every prior RT phase)
- `SupportsRayQuery()` + cvar gate; default off; raster/GL3/non-RT paths bit-for-bit unchanged.
- Validator-first: `r_rtReStirDI_test` diffs GPU vs a CPU reference before the effect renders.
- Runs at internal (FSR2) resolution like the other RT effects; FSR2 is the perf backstop.
- New shaders: re-run CMake configure so the glob picks them up ([cmake-glob gotcha]).

## Resolved (incl. by the perf scan)
- **Scope = "ReSTIR-lite":** initial RIS + temporal + the two visibility cuts; spatial reuse is
  optional, the heavy many-light machinery is skipped (see Performance).
- **Reservoir storage = `buffer_reference` SSBO** (ping/pong), device address passed in
  `RenderParams` — matches how the TLAS + `RtLight[]` are already reached (no new descriptor set,
  respects the ≤2-set / no-push-constant baseline). Row ≈ **16 B**
  `{ uint y+validbit; float W; float wSum; uint M+visibility+age }` — analytic lights need no
  triangle-UV field, so we stay under RTXDI's 24 B.
- **Initial candidates = 8** from a flat power·BRDF·1/d² alias table (no ReGIR/BVH/tiling at 256 lights).
- **DI runs at internal (FSR2) resolution** like the other RT effects.

## Open questions (settle during Stage 0–1)
- Exact candidate `M` (8 is the start) and whether a BRDF sample (`numBrdfSamples=1`) is worth it for spots.
- One combined reservoir vs split diffuse/specular (defer; Stage 6).
- If Stage 4 is ever needed: fuse spatiotemporal vs a separate spatial pass; fragment vs compute.
- Whether a later lower RT rung should source visibility from the existing shadow-map cache
  ("ReSTIR-sampled shadow maps") instead of a ray — deferred lever, measure first.
