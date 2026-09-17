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

- **H4a** ray pass + `r_rtaoDebug 1` overlay — verify: noisy but plausibly-shaped AO
  in-game, cost via r_vkGpuTime (expect ~1–2 ms full-res on the 3080 Ti).
- **H4b+c** guides + denoise + `r_rtaoDebug 2` — verify: the noise resolves, no smearing
  under camera motion (velocity reprojection working).
- **H4d** composite + GTAO skip — verify: `r_ssaoDebug 1` looks like stable AO, lighting
  identical in character to GTAO, and the frame drops by (GTAO cost − RTAO cost) ≈ 2–3 ms.
- **H4e** preset row + docs/todo.md table.

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
