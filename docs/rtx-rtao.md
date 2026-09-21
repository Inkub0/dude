# H4 — RTAO: ray-traced ambient occlusion (Ultra Nightmare)

Roadmap H4 ([rtx-hybrid-roadmap.md](rtx-hybrid-roadmap.md)), pulled forward past H3 on the
2026-09-17 per-pass profile: **full-res GTAO is ~4.3 ms — 40% of the 10.7 ms UN frame** —
while shadow maps (H3's target) are a solved 0.5 ms. The user's direction: UN's endgame is
ray-traced lighting, so the answer to GTAO's cost at the flagship tier is to *replace* it
there, not to optimize it (GTAO Phase 3 compute tiling: struck). Lower tiers keep the cheap
temporal-trade GTAO forever — RTAO is VK + RT-hardware only, like every RT feature.

**What RTAO buys over GTAO** besides the frame time: hemisphere rays against the TLAS see
**off-screen and behind-camera occluders** (GTAO's structural blindness), no screen-edge
halos, and its machinery is ~80% of RT indirect diffuse — the H6 GI seed (roadmap's reuse
ledger). Denoised by the **NRD REBLUR_DIFFUSE_OCCLUSION** instance the H2 framework already
hosts and validated end-to-end (docs/rtx-nrd.md).

## Design

One noisy ray per pixel, denoised temporally+spatially by NRD, composited into the SAME
RGBA8 AO buffer the lighting already samples — so ambient/interaction shaders, presets,
`r_ssaoIntensity`/`r_ssaoDirectLight` art controls and `r_ssaoDebug` all keep working
unchanged. `r_ssao` stays the master "AO on" switch; `r_rtao` swaps the *producer* when the
hardware allows.

### The frame (when r_rtao engages)

1. **Normal prepass** — unchanged (merged gbuffer pass; RTAO needs normals + velocity).
2. **H4a ray pass** (`rtao_ray.frag`, fullscreen fragment, R16F target at view res):
   reconstruct view pos from `_currentDepth` (ssao.frag math) + view normal from the
   G-buffer → world pos/normal via the inverse view matrix → one cosine-weighted
   hemisphere ray (IGN dither + golden-ratio frame phase, same jitter idiom as SSAO
   temporal) → `rayQueryEXT` closest-hit against the TLAS (`u_rtParms.xy` device-address
   idiom from ssr_rt.frag; tMin = small normal offset, tMax = `r_rtaoRadius`).
   Output = `REBLUR_FrontEnd_GetNormHitDist` encoding: `saturate(hitT / (A + |viewZ|*B))`
   on hit, 1.0 on miss/sky/weapon. A/B must MATCH the `ReblurHitDistanceParameters` set on
   the denoiser (A = 0.5*radius, B = 0.1; roughness=1 ⇒ the C term is identity).
3. **H4b guide inputs** (the H2c work, RTAO's version):
   - `IN_VIEWZ`: R16F linearize of `_currentDepth` (negative view Z — RH convention
     matching the matrices; the GTAO mip stores positive, so RTAO gets its own tiny pass).
   - `IN_NORMAL_ROUGHNESS`: fullscreen pack pass — G-buffer view normal → world, oct-encode
     to RGBA16F `(oct.xy*0.5+0.5, roughness, 0)`. Encoding-2 semantics; RGBA16F carries the
     same 0..1 values without adding an R10G10B10A2 target format to the RHI.
   - `IN_MV`: the merged prepass velocity attachment (exists at UN; zero when MV off).
4. **H4c NRD denoise**: persistent NrdCreate at view res; per-frame CommonSettings from the
   real viewDef (viewToClip = projection, worldToView = modelView, prev matrices via the
   shared temporal camera state, jitter from viewDef->jitter, frameIndex++, mvScale for the
   RG16F screen-space velocity); `NrdRecordDispatches(frameCb, REBLUR_AO)` outside any
   render pass. `CLEAR_AND_RESTART` on history breaks (map change / vid_restart).
5. **H4d composite** (`rtao_resolve.frag`): NRD's `OUT_DIFF_HITDIST` (denoised occlusion)
   → the standard AO RGBA8 buffer: `.r = pow(occ, r_ssaoIntensity)`, `.gba` = surface
   normal (bent-normal consumers degrade to the geometric normal). Point
   `rhiSsaoResultRT` at it, set `rhiSsaoAppliedThisView`, **skip the whole GTAO chain**
   (march, blur, temporal, depth mip — the SSR min-Z pyramid is separate and unaffected).
6. **H4e preset wiring** (after user verify): `r_rtao` ON in the Ultra Nightmare row
   (inert without RT hardware, like the other rt* columns).

### Cvars

- `r_rtao` (bool, 0): ray-traced AO producer (VK + ray-query hardware; needs r_ssao).
- `r_rtaoRadius` (float, 80): world-unit ray length (GTAO's 48 sees only screen geometry;
  RT rays can reach further for grounded large-scale occlusion).
- `r_rtaoDebug` (int, 0): 1 = raw noisy ray output overlay, 2 = denoised (pre-composite).
  (`r_ssaoDebug 1` shows the final composited buffer as always.)

### Phasing / verification

- **H4a** ray pass + `r_rtaoDebug 1` overlay — DONE (5db32cc2), raw output USER-VERIFIED.
- **H4b+c** guides + denoise + `r_rtaoDebug 2` — DONE (4cfd48b4 + the 4686135d jitter
  fix: NRD needs NON-jittered matrices, the engine bakes FSR2's Halton jitter into the
  projection shear terms — with r_fsr on this rejected history every frame and the output
  degenerated to drifting raw dots). Denoised output USER-VERIFIED: "looks really
  amazing".
- **H4d** composite + GTAO skip — DONE: rtao_resolve writes the STANDARD AO buffer
  (.r = pow(occ, r_ssaoIntensity) exactly like ssao.frag, .gba = the G-buffer surface
  normal so bent-normal consumers degrade to a no-op; sky/weapon unoccluded). RtaoPass
  runs BEFORE the GTAO pass, which returns immediately when RTAO composited — any RTAO
  failure falls through to GTAO unchanged. Smoke-tested validation-clean. PENDING USER:
  in-game lighting look + the r_vkGpuTime delta (expect ~10.7 → ~7–8 ms at UN).
- **H4e** preset row + docs/todo.md table — after the H4d verdict.

### Risks / notes

- **Units**: NRD hit-dist defaults assume meters; we pass Doom units consistently in
  matrices, viewZ and hit distances, and set HitDistanceParameters accordingly (A scaled to
  the radius). If ReBLUR misbehaves, the first suspect is a units mismatch between the
  shader-side normalization and the denoiser settings.
- **Sky/weapon**: normHitDist 1.0 (unoccluded) mirrors GTAO's early-outs; viewZ beyond
  `denoisingRange` marks sky for NRD.
- **TLAS coverage**: world BLAS (+ movers) is what r_rtSunShadows builds; monsters join via
  r_rtAnimBlas (default on, inert without TLAS). AO from monsters follows automatically.
- **1 spp**: if ReBLUR needs more, first lever is `hitDistanceReconstruction` mode in
  ReblurSettings, second is 2 rays half-res + checkerboard — not first-cut concerns.


## 2026-09-21 audit — "is any of the SSAO pipeline still running under RTAO?"

Checked on request. **When RTAO denoises successfully, nothing of GTAO runs:** `RB_RHI_RtaoPass` runs first
and `RB_RHI_SSAOPass` returns on `rhiRtaoDenoisedThisView` *before* any of its work — the depth-mip pyramid
(`r_ssaoDepthMip`), the horizon march, the bilateral blur and the temporal pass all live behind that gate, and
no other code path is keyed on `r_ssao` for GPU work. What remains is shared and needed: the normal prepass
(RTAO reads its normals; velocity feeds NRD) and the AO buffer's consumers.

**One real double-cost case, fixed:** when the denoise *cannot* run — no velocity MRT (`r_motionVectors` and
`r_fsr` both off) or a missing guide program — RTAO still traced its full-res ray pass, threw the result away,
and GTAO then ran as the fallback: both producers in one frame. The pass now checks the denoise prerequisites
first and returns before tracing (unless `r_rtaoDebug 1`, the only consumer of raw rays). Not the case at
Ultra Nightmare (FSR on → velocity present), so no change there.


## 2026-09-21 — guide passes folded into the ray pass (user-verified, merged)

RTAO saves less than GTAO cost because it replaces one chain with five passes (rays, view-z guide, packed-normal
guide, NRD, resolve). First trim, zero quality change: the **ray pass now writes both NRD guides itself** as
extra render-target outputs — it already fetched depth and the G-buffer normal for every pixel and computed the
view z + world normal for its ray, so the two standalone fullscreen guide passes (`rtao_viewz.frag`,
`rtao_pack.frag`: two more depth/normal fetches per pixel, two pass begins, barriers) disappear.
- *Why not into the geometry prepass (the first idea):* MRT writes there happen per rasterised fragment
  (overdraw) and would touch every gbuffer pipeline/tess variant and the merged-prepass render pass; the ray
  pass is one fullscreen draw with no overdraw and no variants.
- New RHI target `CreateRenderTargetRtaoMrt` (R16F hit distance @0, R16F view z @1, RGBA16F packed normal @2,
  VK pass class 9); `RtaoDenoiseArgs.viewzAtt/normalAtt` tell the denoiser which attachment holds each guide.
  Guides are written on every path (sky / weapon / degenerate-normal early-outs included), identical values
  to the old passes.
- **Verified 2026-09-21:** identical AO, a small GPU-time gain (~0.1–0.3 ms as predicted), several minutes of
  play without a device loss. The A/B cvar, the legacy three-pass branch and the two guide shaders
  (`rtao_viewz`, `rtao_pack`) were then removed.
- Still open for real savings: checkerboard tracing, half-res RTAO (see the 2026-09-21 discussion).

## 2026-09-21 — bright line along concave creases (fixed, user-verified)

The RTAO buffer showed a thin *bright* line exactly at wall/floor and wall/wall contacts, where GTAO is darkest.
Cause: the ray origin was lifted 1 unit along the **bump-mapped normal**; within that distance of an adjoining face
(sooner where a normal-map bevel tilts the normal toward it) the origin lands *behind* that face, world faces are
single-sided, the hemisphere looks into the void and the rays miss → unoccluded. Fix (`rtao_ray.frag`): pull the
origin back **toward the eye** (`0.25 + depth · 0.004`, plus a 0.05 normal lift against self-hits) — the camera
sees the point, so the segment back to it is unobstructed by construction and the origin can never be inside
geometry. The cosine hemisphere still uses the bump normal, so normal-map AO detail is unchanged.

## 2026-09-21 — checkerboard rays: tried, measured, REVERTED

NRD's checkerboard mode (`ReblurSettings::checkerboardMode = BLACK`; the vendored NRD already has
`NRD_SUPPORTS_CHECKERBOARD 1`) was built as `r_rtaoCheckerboard` with a menu checkbox: trace every other pixel,
noisy input packed to the left half of its texture (texel `(x', y)` = full-res pixel
`(2x' + ((y ^ frameIndex) & 1), y)`), guides still full-res. **User-measured GPU-time saving: ≈ 0** — so the
AO *rays* are not where RTAO's time goes (short 80-unit rays are cheap on this hardware); the cost is the
full-resolution denoiser + passes, which checkerboarding does not touch. Branch deleted, nothing merged.
**Do not retry.** The only lever that attacks the real cost is **half-resolution RTAO** (rays + guides + NRD on
a quarter of the pixels, depth-aware upsample in the resolve) — unscheduled.

## RTAO resolution scale (2026-09-21, `r_rtaoResScale`, default 1.0)

RTAO's cost is dominated by the **NRD denoiser**, not the rays — the earlier checkerboard experiment
(half the rays, full-res NRD) saved ~0. So the lever that actually pays is running the WHOLE chain
at reduced resolution: the ray pass, the NRD guides (view-z / normal, written by the ray pass), and
the NRD denoise all at `rw x rh = round(view * r_rtaoResScale)`, then the resolve
(`rtao_resolve.frag`, already sampling `u_rtao` at normalized uv) bilinearly upscales the denoised
occlusion to full res. Textures, geometry and direct lighting stay native - only the AO signal is
low-res (docs/rtx-rtao.md; the "decoupled signal" idea).

- **Ray pass** runs at `rw x rh`: `screenCorrection = 1/rw`, `depthTexRecip = (w/rw)/uploadW`
  (mirrors the scaled GTAO march in `RB_RHI_SSAOPass`). `rtao_ray.frag` is unchanged - it is already
  resolution-parametrised by those uniforms.
- **Velocity** for NRD's `IN_MV` is the full-res prepass velocity downsampled to `rw x rh` with a
  plain copy (`smaa_copy`); NRD's jitter is scaled by the ratio (it is in render pixels). The engine
  velocity is UV-space, so it is resolution-independent.
- **NRD** runs at `rw x rh` (`NrdCreate` sizes to it); a scale change resets its history.
- **Resolve** stays full-res and upscales the scaled NRD output for free.
- `r_rtaoResScale 1.0` -> `rw == w`: the path is byte-identical to before.

### Non-integer scale is fine here (unlike reflections)

`r_ssrResScale` / `r_rtReflResScale` **snap to integer divisors** because a reflection is a *sharp*
image reconstructed by a *temporal* upscale with per-frame jitter - a fractional ratio beats the
jitter grid into strobing stripes. AO is the opposite: a **low-frequency** signal, and the upscale
is a plain **bilinear** (spatial) filter with no jitter reconstruction, so there is nothing to beat.
That is why `r_ssaoResScale` already runs fine at 0.75, and why `r_rtaoResScale` is a **free float**
(0.25 .. 1.0), not snapped. NRD's temporal accumulation runs at the low res but its output is still
smooth low-frequency AO, so bilinear upscaling it is clean.

**Watch for:** halos / AO bleeding across depth silhouettes at aggressive scales (0.5 and below) -
the bilinear upscale isn't depth-aware. If it shows, the resolve can be upgraded to a bilateral
(depth+normal guided) upsample; it already binds both. **Pending the user's `r_vkGpuTime 2` on the
`ao (rtao/gtao)` scope + a look at 0.75 / 0.5.**
