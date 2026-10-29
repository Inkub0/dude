# RT shadows with NRD SIGMA - research and plan

**Status: PLAN, 2026-10-29. No code.** Branch `feat/rtx-sigma-plan` (docs only). Decision pending (user).
Goal: more stable, better-looking ray-traced shadows for the same or less GPU time than DUDE 0.9.7.
Prior attempt: H3 soft shadows (commits `2ab43dbc` code, `80a8d764` docs/rtx-soft-shadows.md, both off master),
abandoned 2026-09-18 for stagger and an 11.4 ms toll in light-dense rooms, not for SIGMA's quality.

## Summary

- **SIGMA is a per-light shadow denoiser.** It takes one light's 1-ray-per-pixel visibility, the occluder hit
  distance and that light's size. It denoises with a penumbra-sized spatial blur (16x16 tiles, at most 32 px) and a
  short temporal stabilisation (at most 7 frames). It is not a radiance denoiser and has no multi-light mode: the
  experimental multi-light helpers were removed in NRD v4.8.0. NRD's author, in NVIDIA-RTX/NRD#99: "SIGMA is per
  light only, unfortunately. I tried, I failed."
- **Option A (RTX tier, SIGMA in place of `r_rtShadowBlur`) does not meet the brief.**
  - Correction to the brief: at Ultra and Nightmare + RTX, `r_rtAllLights` is on, so every shadow-casting light
    traces rays. "Cached maps for the rest" holds only at Medium and High + RTX.
  - Our build cannot limit SIGMA to a light's screen rect (NRD's viewport-offset support is compiled out), so every
    light pays a full-screen denoise (about 0.5-0.6 ms per light at 1440p on the 3080 Ti, scaled from NRD's own table).
  - The tile-bounded blur averages about 0.09 ms per light in a busy 12-light scene. A SIGMA swap costs several ms more
    there, and is roughly even only with 1-2 lights.
  - Today's blur is already deterministic and history-free, so it is stable. SIGMA only looks better if we also switch
    to area-sampled rays (real penumbrae), which is exactly what H3 did.
  - Verdict: park A. Its plumbing still gets built, as the shared core of B1.
- **Option B as posed (one SIGMA over the ReSTIR winner's visibility, "B2") does not work.** The research is
  unambiguous:
  - DUDE's DI lights are points, so each light's visibility is deterministic. The only noise in a pixel's visibility
    is which light won the pick.
  - SIGMA sizes its blur from the occluder distance and goes hard at contact by design, so at contact that selection
    noise passes straight through. With light size 0, SIGMA reads the pixel as back-facing.
  - The H3 lesson "never feed SIGMA re-rolled noise that is not penumbra-shaped" applies directly.
- **The hybrid "B1: dominant-light lanes inside the DI" fits SIGMA's design and the brief.**
  - The 1-2 lights that dominate the view leave the ReSTIR pick. Each gets its own per-pixel shadow ray, a SIGMA
    denoise, and analytic unshadowed shading in the DI apply. That is the NRD sample's sun recipe and the NRD#100
    torch recipe ("No temporal lags noticeable"), applied per view to Doom's local lights.
  - Their shadows then follow casters with at most SIGMA's few-frame stabilisation, instead of ReLAX's radiance
    history. The remaining lights stay in ReSTIR + ReLAX.
  - The smear the boost fights is mostly the dominant light's. That should let `r_rtReStirDIBoostHistory` /
    `r_rtReStirDIBoostConfidence` go back toward 0.9.6 values (boost 2.88 -> 0.96 ms).
  - Estimate: about +0.6 ms per lane against up to -1.9 ms of boost. Net about -1.3 ms with one lane and -0.7 ms
    with two, if the boost can come down. That "if" is measurement M1, which needs no code.
  - B1 is built inside the DI pass, not by routing lights forward, so it also covers **emissive mode**: a lane can be
    an emitter row (a fixture's triangles), and area-sampled visibility toward the triangles is exactly the penumbra
    signal SIGMA is built for. Emissive lanes come last, after emissive mode's own open bugs (some lights do not light
    at all, probably those on non-world geometry).
- **Recommended order:**
  1. M1, the boost-knob split (no code). It sizes the prize.
  2. M2, a "who dominates" survey (no code).
  3. S1, SIGMA plumbing plus a validator.
  4. S2, a one-lane debug probe with costs.
  5. S3, the lane composite.
  6. S4, the boost retune.
  7. S5, emissive lanes.
  8. Option A only on request.

## 1. Baseline to beat (0.9.7)

3080 Ti, Vulkan, 2560x1440, `timeDemoQuit demo1`, `com_timeDemoRealTime 1`, Path Tracing Nightmare (dude_rtx 2,
preset 5), `r_vkGpuTime 3`. Memory: perf-097-vs-096-pt-regression.

| scope | 0.9.6 | 0.9.7 | note |
|---|---|---|---|
| whole frame | 12.39 | **14.02** | -9.3 % fps at PT; raster flat |
| restir.di.boost | 0.96 | **2.88** | the whole regression: BoostHistory 2 -> 4, BoostConfidence 0 -> 0.3 |
| restir.di.gather | 2.46 | 2.69 | |
| restir.di.denoise (ReLAX) | 1.57 | 1.42 | |
| restir.di.grad | - | 0.74 | |
| restir.di.apply | - | 0.16 | |

At PT Nightmare the DI runs at `r_rtReStirDIResScale` 0.75 with checkerboard off, so the DI and any DI-side SIGMA run
at **1920x1080**. Ultra PT runs 0.75 with checkerboard. Unmeasured: each boost knob's share (M1).

RTX-tier reference (2026-10-03, Nightmare + RTX, busy scene, 12.4 ray-traced lights; it predates
`r_rtShadowBlurMinPenumbra`, so re-measure in M0):

| scope | ms | per light |
|---|---|---|
| rtblur.ray | 0.96 | 0.077 |
| rtblur.tiles | 0.47 | (2 passes per light) |
| rtblur.blurH | 0.36 | |
| rtblur.blurV | 0.33 | |
| rtblur total | 2.12 | 0.17 |
| interaction + inline rays (unscoped) | ~2.21 | 0.18 |

Full-screen light, 2026-09-21: ray 0.32, tiles 0.14, blurH 0.12, blurV 0.16 ms. The tile pass has been about 0.07 ms
cheaper since FastTiles; the ray is about 35 % cheaper since the checkerboard ray.

## 2. What SIGMA needs and what it outputs

Vendored NRD v4.18.0 (`neo/libs/nrd`, commit e06852bd) is the newest release. Upstream master has three post-tag
commits, all REBLUR/RELAX: "fixed tile classification to handle viewZ scaling" and "avoided uninitialized SMEM reads"
(#113) touch ReLAX, which DI uses. **No SIGMA change since the tag.**

**Denoisers:**
- `SIGMA_SHADOW`: IN_PENUMBRA (R16F) -> OUT_SHADOW_TRANSLUCENCY (R8).
- `SIGMA_SHADOW_TRANSLUCENCY`: adds IN_TRANSLUCENCY (RGBA8 in, RGBA8 out). The output's `.yzw` carries fractional
  transmission. This is the variant that keeps DUDE's grate and fence coverage (`r_rtPerforatedShadows`): plain
  SIGMA_SHADOW builds its signal as `IsLit(penumbra)`, a binary.

**Guides:** IN_VIEWZ and IN_NORMAL_ROUGHNESS always. IN_MV only for temporal stabilisation (`maxStabilizedFrameNum > 0`).

**Encoding** (NRD.hlsli 985-1003):
- Directional light: `PackPenumbra(dOcc, tanAngularRadius)` = `0.5 * dOcc * tan`.
- Local light: `PackPenumbra(dOcc, dLight, lightSize)` = `0.5 * lightSize * dOcc / (dLight - dOcc)`.
- **Lit** = `NRD_FP16_MAX` (65504). **0 = back-facing.** So every light needs a non-zero size, or its contact
  shadows read as back-faces.
- Output decode: `SIGMA_BackEnd_UnpackShadow(s) = s*s`.

**Ray rules** (README): from the surface toward the light; **no** accept-first-hit-and-end-search, because the
occluder distance must be the nearest one. Write 0 for N.L <= 0, the hit distance on a hit, and FP16_MAX on a miss.
DUDE's DI visibility ray uses `TerminateOnFirstHit` today, so its `occDist` is "an" occluder, not the nearest.
`rtshadow_ray.frag` (RTX tier) already traces closest-hit; the closest-hit premium was measured at about 0 there.

**Passes** (`Sigma.cpp`, `Sigma_Shadow.hpp`):
- Classify tiles (16x16), then smooth tiles.
- Copy (history, only when stabilising).
- Blur, then post-blur: 12 Poisson taps each since v4.18, radius `min(r, 2)`..32 px. The penumbra estimate favours
  smaller penumbrae (`SIGMA_PRESERVE_SHADOWS_IN_PENUMBRA`).
- Temporal stabilisation:
  - Reprojects by the **receiver's** motion vectors.
  - Clamps history to a 5x5 mean +- sigma (sigma 3 -> 1 as history grows).
  - Anti-lag `1 - sqrt(|clamp delta|)`.
  - At most 7 frames; default 5 = 0.084 s.
  - No confidence input; the only reset is `CommonSettings::accumulationMode`.
- Fully lit and fully umbra tiles early-out of the blurs and the stabilisation.

**Settings** (`SigmaSettings`):
- `lightDirection`: inert in v4.18, because screen-space sampling is on.
- `planeDistanceSensitivity` 0.02.
- `maxStabilizedFrameNum` 5 (0..7; 0 = spatial only).
- `checkerboardMode`: new in v4.18; untested in DUDE. H3's checkerboard caused a ~1 s settle.

**History:** the only persistent state is the previous OUT (the user's texture) plus a permanent R32_UINT history
length per denoiser identifier. So a temporally stabilised light needs its own identifier and output, keyed by a
stable light identity. With `maxStabilizedFrameNum = 0` one identifier can serve any number of lights per frame,
since nothing persists. The README calls this the light-count-independent option ("less stable").

**Cost** (NRD README, RTX 4080, 1440p native, v4.18): SIGMA_SHADOW **0.40 ms**, SIGMA_SHADOW_TRANSLUCENCY **0.45 ms**
(v4.8 without stabilisation: 0.24 / 0.30). Memory at 1080p: 31.88 MB working set (transient, shared between
identifiers) + 8.44 MB persistent per SIGMA_SHADOW.
- DUDE's own data point (H3, 3080 Ti, 1080p, area-sampled ray + SIGMA): **~0.55 ms per light**.
- Planning numbers used below for the 3080 Ti, assuming it is 1.3-1.5x slower than the 4080 (an assumption, to be
  measured in S2):
  - per full-screen light at 1440p: 0.52-0.68 ms;
  - per lane at 1920x1080: 0.29-0.38 ms (translucency variant: 0.33-0.38).

**What NRD and others do with many lights:**
- **NRD sample:** SIGMA for the sun only. The sun's unshadowed light goes out separately and is subtracted from the
  path tracer's radiance; composite `L * shadow`. Local and emissive lights stay in the REBLUR/RELAX radiance.
- **RTXDI and RTX Remix:** ReLAX/REBLUR on demodulated DI radiance, gradient confidence (RTXDI Confidence.md: the
  stated target is "shadows lagging behind the objects that cast them"), no SIGMA. Remix denoises direct and indirect
  in separate ReLAX instances, with the direct one tuned short (250 ms, fast history 1, prepass 0).
- **NRD#99 (the author):** two instances (direct + indirect), confidence inputs, and SIGMA as a "must have for
  sun/moon shadows". **NRD#100:** a mod traced a torch's shadow separately with SIGMA and reported no lag; in the
  author's own gradient work the sun is left out of the gradients "since shadows are denoised by SIGMA".
- **Heitz et al. 2018 (ratio estimator):** analytic unshadowed `U` x denoise(S) / denoise(U), with S and U from the
  same rays through the same filter. It denoises before dividing (never the ratio itself); cost is per light per pixel.
- **UE5 MegaLights (2025):** one denoiser on demodulated radiance for all lights; per-light shadow denoising was
  rejected because it caps the light count.

## 3. Where DUDE's shadows come from today

### 3.1 RTX tier (dude_rtx 1)

**Routing** (`RB_RHI_SelectLightShadowTechnique`):
- `r_rtAllLights`: every shadow-casting light with interactions -> `lightRtPoint` (inline ray, no map, no stencil).
- Otherwise:
  - the sun and oversize lights -> `lightRtOnly` when `r_rtSunShadows`;
  - moving point lights (pose changed within 30 views) -> `lightRtPoint` when `r_rtMovingLights`;
  - the rest -> cube or 2D maps.

**Presets** (`rtAugment[]`):

| tier + RTX | sun | moving | all lights | blur | stagger | MinPenumbra |
|---|---|---|---|---|---|---|
| Medium | 1 | 1 | 0 | 0 | 0 | - |
| High | 1 | 1 | 0 | 1 | 0 | 1.0 |
| Ultra | 1 | 1 | **1** | 1 | **1** | 4.0 |
| Nightmare | 1 | 1 | **1** | 1 | 0 | 2.0 |

**Blur chain, per light, inside the light loop** (scene pass suspended; `RB_RHI_RtShadowBlurLight`):
1. `rtblur.ray`:
   - Full-res, deterministic **centre** ray, closest hit, jitter-exact unprojection, far bias.
   - Static checkerboard; the H pass rebuilds the holes.
   - Output: RGBA16F (vis, half-width x, view distance, half-width y) + an RG8 summary.
2. `rtblur.tiles` x2 (8x8 tiles).
3. `rtblur.blurH`, then `rtblur.blurV` (24-tap cap, depth-aware) -> R16F mask.
4. The interactions read the mask as **mode 5** (unit 13, Vulkan only).
5. Translucent (depth-LESS) and depth-hacked surfaces fall back to the inline **mode 4** ray. Staggered far lights
   reproject an old RGBA8 slot mask (**mode 6**, Ultra only).

**Penumbra width:** `halfW = blurScale * g^curve` with `g = dOcc / (dL - dOcc)`. `blurScale` =
`3 * r_rtShadowBlurIntensity` (x maxAxis/256 for point lights; a 1-degree cone for parallel lights). The user-tuned
defaults are Intensity 0.13 and Curve 0.5; curve 0.5 is not physical. Doom lights have no physical size: this
`blurScale` is the only stand-in, and the DI's soft grate coverage shares it.

Today's chain is stable by construction: deterministic input, no history. Its weak points are the fake penumbra shape
(no contact hardening from a real area, overlapping occluders not combined) and the Ultra-only stagger reprojection.

### 3.2 Path Tracing tier (dude_rtx 2, ReSTIR DI)

Per frame, in `RB_RHI_ReStirDIPass` (before the light loop, world-only depth):
lightgrid.fill -> restir.di.gather -> young -> boost -> grad / gradfilter / gradconf / disoccmix -> denoise (ReLAX)
-> later restir.di.apply (additive into the HDR scene). RTAO and RTGI stand down; the GI bounce rides inside the
DI's ReLAX signal.

**Gather:**
- Exhaustive RIS over the pixel's light-grid cell (32 slots at PT, so effectively exhaustive in a Doom room), with
  the cheap unshadowed-irradiance target.
- **One visibility ray for the winner only.** It aims at the light's `pos`: Doom lights are **points**, and emitter
  rows aim at their authored origin or centroid while their radiance is area-sampled. Flags `Opaque |
  TerminateOnFirstHit`.
- So every DI shadow from a solid caster is **hard and deterministic per light**. The only softness is the grate
  coverage footprint.
- The noise in a pixel's visibility is **selection** noise: which light won.

**MRT:** att0 = radiance + hitDist (the DI occDist, = dL when lit); att1 = viewZ (RG32F) + gradient luminance;
att2 = normal; att3 = SH1 + row id. The **4-attachment cap is full** (the 8-attachment raise of R3b.0 left with the
scrapped R3b).

**Boost:**
- A pixel is young when its NRD history < BoostHistory (4), last frame's confidence < BoostConfidence (0.3), it was
  revealed, or it is held.
- Young pixels with at least 5 of their 3x3 neighbours young re-run the **whole** gather shader with K = 2 picks,
  K rays and a second GI bounce.
- Cost grows with the young-pixel count. Shadow motion feeds the gradient, which feeds confidence, which feeds young
  pixels.

**Gradient:** re-shades last frame's samples at the stored points. Its luminance includes `vis`, so moving shadows
drive the confidence drops, ReLAX resets (IN_DISOCCLUSION_THRESHOLD_MIX) and boosts.

**Denoise:** RELAX_DIFFUSE_SH on albedo-free irradiance (+ GI demodulated). The apply rebuilds the diffuse with the
albedo and a GGX lobe from SH1.

**Not covered by the DI** (they stay on the forward path with the inline mode-4 hard ray): translucents, blood films,
the first-person weapon, subviews, mirrors and the portal sky. Weapon lights (`DUDE_LIGHT_DIFORWARD`) are lit forward
and have **no** bounce.

**Not jitter-exact:** the DI unprojection (`diSurfaceAt`, the apply's spec-reshade) ignores the FSR2 jitter terms
`proj[8]/[9]`, unlike `rtshadow_ray.frag`. It is self-consistent, but world points shift by up to the jitter.
Pre-existing; relevant to any new DI-side ray.

## 4. Option A - SIGMA per already-ray-traced light (RTX tier)

### Design (if built)

- **Ray pass:** `rtshadow_ray.frag` gains a SIGMA output: PackPenumbra (local form) or the lit sentinel, written
  full-screen. Outside the light rect = lit: a full clear, about 0.01 ms.
  - **A-det:** keep the deterministic centre ray. SIGMA becomes a smarter blur. To keep the user's tuned look, feed
    an effective occluder distance d' that makes SIGMA's linear radius equal `blurScale * g^curve`.
  - **A-area:** a stochastic point on an emitter sphere of radius ~`blurScale`, blue-noise jitter as the README
    recommends (DUDE's blue-noise texture went with H3). This gives real penumbrae.
- **Denoise:**
  - **A-spatial:** `maxStabilizedFrameNum = 0`, one shared identifier, dispatched per light in the loop. No per-light
    history, so no slots, no stagger interaction and no light-identity bookkeeping. Output = the mode-5 mask (squared
    in the shader).
  - **A-temporal:** H3-style slots with an identifier and persistent output per light, keyed by `dudeSerial` (not the
    lightDef index, which `r_rtShadowBlurStagger` slots still use), so incumbents hold their slots.
- **NRD instance:** RTGI (Ultra/Nightmare + RTX) and RTAO run at a scaled resolution, and one NRD instance has one
  resolution. A size change is a `vkDeviceWaitIdle` rebuild, so per-frame mixing would thrash. Full-res SIGMA needs
  a second instance: a "shadow context", as H3 pass 2's NrdState/NrdSelect.
- **Budgets:**
  - `NRD_FRAME_PASSES = 2` today. The CB ring and descriptor pools must grow to `2 + lights x SIGMA dispatches`;
    on overflow, fall back to the blur, not skip.
  - Add explicit headroom per extra identifier (H3 gotcha: NRD sizes pools per unique dispatch template).
- **Interaction side:** unchanged (mode 5; modes 4 and 6 fallbacks).

### Cost against the RTX-tier numbers (estimates; S2/M0 measure them)

| case | today (tiles + H + V) | SIGMA (A-spatial) | delta |
|---|---|---|---|
| 1 full-screen light, 1440p | ~0.35 | 0.52-0.68 | +0.2-0.3 |
| High + RTX (sun + moving, 0-3 lights) | 0-1.0 | 0-1.9 | +0-0.9 |
| Nightmare + RTX busy (12.4 lights) | 1.16 | 12.4 x 0.15-0.6 = 1.9-7.4 | **+0.7-6** |

- The SIGMA floor per small light (classify + copy + early-out blurs over a full screen of tiles) is unmeasured.
  0.15 ms is a guess.
- The rect-bounded passes are why today's chain averages 0.09 ms per light. NRD can take a rect only with
  `NRD_SUPPORTS_VIEWPORT_OFFSET=1` and regenerated SPIR-V, and a rect that moves every frame breaks history unless
  stabilisation is off.
- Extra losses:
  - A-temporal loses the static checkerboard (SIGMA's own checkerboard is unproven; a separate fill pass cost
    0.31 ms), so +35 % on rtblur.ray.
  - A-area traces incoherent rays.
- Memory:
  - A-spatial: ~57 MB at 1440p, once.
  - A-temporal: +~15 MB history + 3.7 MB output per light.

### Verdict A

**Does not meet "same or less GPU time"** at Ultra/Nightmare + RTX, where most RT shadows are. The stability gain is
small (today's mask has no history to lag). The look gain needs A-area, which brings back H3's noise and its
never-diagnosed strafing stagger.

Worth building only as an opt-in "SIGMA soft shadows for the top-K lights" quality option. The user rejected mixing
blurred and unblurred lights in a view in 2026-09 (though SIGMA-vs-blur with a matched size model would differ less).
**Recommendation: park.** Its core (S1: second identifier set, sizing, validator) is shared with B1 anyway.

## 5. Option B - Path Tracing tier

### B2 - one SIGMA over the ReSTIR winner's visibility: rejected

- The appeal: with pick probability proportional to the unshadowed target, `E[V_winner] = sum(pHat_i V_i) /
  sum(pHat_i)`, which is exactly Heitz's shadowed/unshadowed ratio. With the 32-slot grid, `sum(pHat_i)` is close to
  exact unshadowed irradiance. So "exact U times denoised V" looks natural.
- It fails on SIGMA's design. The V noise is selection noise between point lights. SIGMA's blur radius comes from the
  occluder distance (0 at contact, and "back-facing" when light size is 0), so selection noise at contact shadows of
  overlapping lights is not filtered. The 5-frame stabilisation leaves sigma ~0.2 at a 50/50 overlap.
- Inventing a "selection-noise penumbra" to force blur would soften every contact shadow in multi-light areas.
- NRD tried and dropped multi-light SIGMA (helpers removed in v4.8.0; README "This solution is limited": overlapping
  shadows, mixed brightness, bias toward the strongest light).
- Splitting the GI bounce out of att0 (needed so V is not applied to GI) costs a second ReLAX.
- Emissive mode makes it worse: more rows means more selection noise.

### B1 - dominant-light lanes inside the DI (recommended)

**Idea:** per view, up to N lights (default 1, max 2-4) become **lanes**. A lane light leaves the RIS pick and gets:
1. its own per-pixel shadow ray;
2. a SIGMA denoise (its own identifier, so its own short history);
3. analytic unshadowed shading in the DI apply.

Everything else stays as today. The lane light's direct term never enters ReLAX, so its shadows carry no radiance
history: they follow casters within SIGMA's 1-5 frame stabilisation. This is NRD's own recipe (sample: sun; #100:
torch), applied per view.

**Frame (all at DI resolution, PT Nightmare = 1920x1080):**
1. **Lane selection (CPU, per view, sticky):**
   - Rank covered lights by an on-screen contribution estimate, or better, by GPU feedback: the gather accumulates
     each row's sum of pHat over the frame into a 4096-entry buffer, read back FRAMES_IN_FLIGHT later.
   - Hysteresis: an incumbent keeps its lane unless a challenger beats it by 1.5x for 0.5 s.
   - Key by `dudeSerial` (placed lights) or `fixtureId` (emitter rows); never by row or lightDef index.
2. **Gather / boost / gradient:**
   - A new light flag (a free bit 8-15; bits 16-31 hold the grate blurScale) makes the DI pick and the gradient skip
     lane rows.
   - The **GI bounce keeps them.** It must not be `DUDE_LIGHT_DIFORWARD`, which also drops the bounce under
     `skipForwardLights`.
   - The gradient no longer sees the lane light's shadow motion, so fewer confidence drops and fewer young pixels:
     the boost gets cheaper even before its knobs change.
3. **`sigma.ray` (new fullscreen fragment pass):**
   - One **closest-hit** ray per lane per pixel, jitter-exact unprojection (`rtshadow_ray.frag`'s, not
     `diSurfaceAt`'s), grate-aware walk.
   - Writes IN_PENUMBRA (R16F) and IN_TRANSLUCENCY (RGBA8, grate coverage) per lane.
   - 2 attachments per lane, so 2 lanes per pass under the 4-attachment cap.
   - Guides: reuse the gather's att1/att2 (viewZ, normal), so SIGMA and ReLAX agree on geometry.
4. **`sigma.denoise`:** SIGMA_SHADOW_TRANSLUCENCY per lane in the **existing** NRD instance (same resolution as the DI
   ReLAX, so no rebuild).
   - Identifiers `NRD_ID_SIGMA_BASE + lane` replace the dead `r_nrdSigma` slot. `NRD_FRAME_PASSES` becomes 2 + lanes,
     with pool headroom per extra identifier.
   - IN_MV = the existing `nrdMvScratch` copy.
   - A lane changing hands runs its first frame with `maxStabilizedFrameNum 0` (H3's per-denoiser reset; avoids a
     second SetCommonSettings in the frame).
5. **Apply:**
   - Per lane: unshadowed diffuse + GGX for that light (exact for a point light: the same `diShadeLight` path at
     full res), times `UnpackShadow(mask)`, joint-bilaterally upsampled like the DI.
   - AO is applied as on the DI direct term.
6. **Handover crossfade:**
   - A light entering or leaving a lane fades over ~0.25-0.5 s: its RIS share is scaled by (1 - w) and its lane
     contribution by w (per-row weight in the light buffer).
   - Precedent: `93b60891` ("crossfade the forward <-> DI handover (blinking lights)"), removed by `70fb8393` when
     forward became weapon-only. The fading-out light needs a lane while it fades, so allocate N + 1 lanes during a
     handover.

**Penumbra size (fidelity decision):**
- SIGMA needs a non-zero light size, so lane shadows get a penumbra. DI shadows of the other lights are hard today,
  and stock Doom 3 shadows are hard.
- Choices:
  - (a) near-hard (a size giving SIGMA's 2 px minimum);
  - (b) match the RTX tier's soft look (`blurScale`, intensity/curve as an effective-distance warp);
  - (c) physical sphere size, area-sampled.
- Recommend (a) first. It isolates "stability" from "look", and a soft dominant light next to hard secondary lights
  would show. Per the fidelity policy, a softer look is a separate opt-in.

**Emissive mode (the target):**
- A lane can be an **emitter row** (a fixture = a run of triangles). Its ray samples a point on the row's triangles
  (Arvo, as the radiance already does), and `lightSize` = the row's projected extent. Real area-light penumbrae are
  what SIGMA is built for.
- The lane's unshadowed radiance must then be low-noise without ReLAX: more area samples for lane rows only (a few
  rows, so affordable), or an analytic polygon irradiance per triangle for the diffuse part (Lambert edge integral;
  exact for constant radiance, approximate for textured glow).
- Gated on: emissive mode's broken lights (suspected: emitters on non-world geometry) fixed first, and B1 proven in
  normal mode. Lanes are keyed by `fixtureId`.
- Note: today the emitters' own DI visibility ray aims at the authored origin, which keeps the ray, the SH and the
  bump re-resolve consistent. Area-sampled visibility for non-lane emitter rows is a separate decision (more noise
  into ReLAX).

**What stays forward:** translucents, blood films and the weapon keep mode 4 for every covered light, lanes included;
subviews and mirrors keep the forward path; DIFORWARD weapon lights are unchanged.

### Cost of B1 (estimates against the 0.9.7 baseline)

| scope | 0.9.7 | B1, 1 lane | B1, 2 lanes | basis |
|---|---|---|---|---|
| restir.di.gather | 2.69 | ~2.69 | ~2.69 | lane rows skipped in RIS (~0) |
| restir.di.boost | 2.88 | **0.96-2.88** | 0.96-2.88 | 0.96 = 0.9.6 knobs; needs M1 + S4 |
| restir.di.grad (+filter/conf) | 0.74 | <= 0.74 | <= 0.74 | lane rows skipped |
| restir.di.denoise | 1.42 | 1.42 | 1.42 | unchanged |
| sigma.ray (new) | - | 0.15-0.25 | 0.30-0.50 | rtblur.ray full-screen 1440p 0.32 ms, scaled to 2.07 Mpx, + incoherence |
| sigma.denoise (new) | - | 0.33-0.38 | 0.66-0.76 | NRD table x 0.56 (pixels) x 1.3-1.5 (GPU); H3 ray + SIGMA 0.55 @1080p |
| restir.di.apply | 0.16 | ~0.20 | ~0.24 | + one BRDF + mask fetch per lane |
| **frame delta** | 14.02 | **-1.4 ... +0.6** | **-0.9 ... +1.2** | best case: boost back to 0.96 |

Memory per lane at 1080p: ~8.4 MB persistent + ~12 MB inputs (R16F penumbra + RGBA8 translucency) + ~8 MB RGBA8
output; the ~32 MB transient pool is shared.

### C - the non-SIGMA control

What RTXDI, Remix and NRD#99 recommend for this symptom, without SIGMA:
- **Split the DI direct and the GI bounce into two ReLAX instances.** The direct one runs short (Remix: 250 ms, fast
  history 1, prepass 0) because exhaustive RIS keeps it low-noise.
- **Keep the gradient confidence** (DUDE already has it).

It costs one more ReLAX (~+1.0-1.4 ms) and only pays if the boost can then mostly go. It is listed so B1 has a
benchmark. Not recommended first: it adds a fixed cost, and it does not use SIGMA.

## 6. Risks

| risk | A | B1 | handling |
|---|---|---|---|
| **H3 strafing stagger** (never root-caused; SIGMA stabilisation, FSR2 and checkerboard all exonerated; suspects: route flips, scissors, stochastic input) | high (A-area) | medium | B1 lanes have no per-frame route flips (hysteresis) and start near-hard (deterministic input). The strafe test is part of every visual check (section 7). |
| **Multi-light** | n/a (per light) | by design | B2 rejected; B1 lanes are single-light. Non-lane lights keep today's behaviour. |
| **Handover pops** (light enters or leaves a lane) | n/a | medium | crossfade + N+1 lanes; parity test: lane on/off at a static view must not change brightness. |
| **Shading parity** (lane analytic shade vs DI shade) | n/a | medium | the lane uses the DI's own `diShadeLight` + apply GGX/AO path, not the forward interaction shader. |
| **Translucents** | mode 4, as today | mode 4, as today | SIGMA masks describe the opaque depth surface only. |
| **Weapon depth hack** | mode 4, as today | weapon is not in the DI depth; mode 4 | unchanged. |
| **Checkerboard** | static-cb ray needs a fill, or SIGMA's new checkerboard (H3: ~1 s settle with the old one) | Nightmare PT has no DI checkerboard; Ultra PT does | trace lanes at full DI res; SIGMA checkerboard only after an A/B. |
| **FSR2 jitter** | ray already jitter-exact | new lane ray must be jitter-exact; the DI's own unprojection is not (pre-existing) | NRD gets the jitter; the gather/lane mismatch is <= 1 px; watch shadow edges with FSR2 on/off. |
| **Motion vectors** | receiver MVs only (2D) | same | the moving-caster lag is SIGMA's 1-5 frames (anti-lag clamp); entity MV bugs fixed 2026-10-28. |
| **Stagger (`r_rtShadowBlurStagger`, Ultra + RTX)** | incompatible with A-temporal (history from 2-3 frames ago, reprojected by 1-frame MVs) | n/a | A-spatial only, or stagger off for SIGMA lights. |
| **Light identity** | slots by lightDef index today | lanes | key by `dudeSerial` / `fixtureId`. |
| **NRD plumbing** | second instance (resolution) | same instance | budgets (`NRD_FRAME_PASSES`, pools); Debug-build asserts in `InstanceImpl.cpp` on edge values (2026-10-28 crash). |
| **Device loss** | per-light dispatch arrays | lane index vs live lane count | bounds-check lane and row indices on the GPU; lane count, lane targets and SIGMA identifiers join `ComputeGfxCfgSignature`. |
| **Emissive bugs** | n/a | emitter lanes | S5 only after the "lights that never light" bug is fixed. |
| **Smear is not mostly the dominant light's** | n/a | the saving shrinks | M2 checks this before S3; if secondary-light smear needs the 0.9.7 boost knobs, B1 does not pay. |

## 7. Measurement plan

**Method (every A/B):**
- Two clean sandboxes, alternating runs (1 warm-up + ABBA).
- `timeDemoQuit demo1` with `com_timeDemoRealTime 1`, fixed display settings (2560x1440 borderless, vsync/VRR off,
  `com_maxFPS 0`).
- Per-pass `r_vkGpuTime 3`. GPU clocks logged (the 3080 Ti throttles). Diff the report's cvar dumps.
- The user runs everything; I never launch the game.

**Visual checks (screenshots / user verdict):**
1. NPC walking past a wall under the room's main light (smear).
2. Strafing past a static furniture shadow (H3 stagger).
3. Flashlight.
4. A multi-light room with contact shadows.
5. Grates.
6. A camera cut in a cinematic.
7. Lane handover while walking between rooms (pops).

### Before any code

- **M0 - RTX-tier re-baseline** (for A only, skip if A stays parked): Ultra + RTX and Nightmare + RTX demo1, scopes
  `rtblur.*` with calls/frame, which gives lights blurred per frame.
- **M1 - boost knob split** (the 4-variant A/B proposed after 0.9.7):
  - Variants: BoostHistory 2/4 x BoostConfidence 0/0.3 at PT Nightmare. Watch restir.di.young/boost.
  - Visual: check 1 per variant.
  - Answers how many of the 1.92 ms each knob costs, and which one the smear needs. That is the ceiling of what B1
    (or C) can win back.
- **M2 - who dominates:**
  - `r_rtReStirDIDebug 8` (chosen light per pixel) screenshots in 4-5 typical rooms + the smear spots.
  - Is one light most of the pixels where the smear shows? Two? Many comparable lights?
  - Decides the lane count, or kills B1 if the answer is "many comparable".

### Build stages (each its own branch, default-off cvar, Vulkan only, GL3 untouched)

1. **S1 `feat/rtx-sigma-core`:**
   - SIGMA identifiers on demand in the NRD layer (replaces `r_nrdSigma`).
   - CB-ring/pool sizing beyond `NRD_FRAME_PASSES`, headroom per identifier.
   - An RHI entry for SIGMA (penumbra + translucency in, mask out).
   - `r_nrdTest` gains H3's two-slot mirrored-edge SIGMA phase (code in `2ab43dbc`).
   - Exit: validator PASS + validation-clean; no visual change.
2. **S2 `feat/rtx-sigma-lane-probe`:**
   - One lane, placed lights only, CPU-picked brightest covered light, near-hard size.
   - `sigma.ray` + `sigma.denoise` with **debug view only**: `r_rtSigmaDebug` 1 = mask, 2 = raw visibility.
   - Measure the two scopes in demo1.
   - Go/no-go: lane total <= 0.8 ms at 1080p DI res; the mask holds still when strafing (check 2).
3. **S3 `feat/rtx-sigma-lanes`:**
   - Lane flag (DI pick + gradient skip, bounce keeps), apply composite, crossfade, GPU-feedback ranking, N lanes
     (`r_rtSigmaLanes` 0..4, default 0).
   - Exit: brightness parity on/off; checks 1, 2, 4, 7 better or equal.
4. **S4 `tune/rtx-sigma-boost`:**
   - With lanes on, retest M1's variants. Keep the cheapest that passes check 1 for the non-lane lights.
   - Decision rule: ship lanes (preset-owned at PT Nightmare/Ultra) only if the demo1 frame <= 14.02 ms **and** the
     user calls the shadows more stable. Otherwise revert.
5. **S5 `feat/rtx-sigma-emissive-lanes`:** emitter rows as lanes (area-sampled visibility, row extent as light size,
   low-noise unshadowed term). After the emissive bug fixes.
6. **S6 (optional) `feat/rtx-sigma-rtx-tier`:** Option A-spatial for the top-K blurred lights, on request only.
7. **S7:** trim losers and diagnostic cvars; refresh this doc and the cvar tables.

## 8. Decisions

User, 2026-10-29:
1. **Track: B1** (dominant-light lanes). C stays a reference only.
2. **M2 first** (debug-8 screenshots of named spots, normal mode), **then M1** as an automated timedemo A/B.
3. **Lane penumbra: near-hard**, to be judged visually when S2/S3 run.
4. **Option A: parked** ("thesis corroborated by data").
5. Emissive lanes (S5) stay after the emissive bug fixes ("some lights plain won't work", suspected emitters on
   non-world geometry; tracked separately).
