# DUDE RT GI — H6 (one-bounce global illumination)

The H6 phase of [rtx-hybrid-roadmap.md](rtx-hybrid-roadmap.md): one indirect diffuse bounce, traced,
denoised, added to the ambient term on the Ultra Nightmare tier. It is the **GI seed** the RTAO work
(docs/rtx-rtao.md) was built to grow into — the same cosine-weighted hemisphere ray, but instead of
writing an occlusion scalar it **shades the hit and gathers the incoming radiance**.

North star: this pass is the *indirect* stage of the eventual Full RT renderer
([H7 "The Dark Lord Cometh"](rtx-hybrid-roadmap.md)). Its inner-loop hit shading is what H7's ReSTIR GI
reuses; keeping it a real one-bounce gather now (not a screen-space fake) is the down-payment.

## Design: extend RTAO, don't stack on it

AO is GI with the gathered radiance forced to zero, so the two must never both run — GI **replaces**
RTAO at the RT tier. Everything RTAO already owns is reused verbatim:

- the cosine-hemisphere ray, per-frame IGN reorientation, world reconstruction and the eye-pullback
  crease-leak fix (`rtao_ray.frag`);
- the NRD guides (`IN_VIEWZ` R16F, `IN_NORMAL_ROUGHNESS` RGBA16F) written by the same pass, so the
  denoise drops in with the RTAO wiring;
- the FSR2 motion vectors, `RB_RHI_TemporalReproj`, the per-frame TLAS (`r_rtgi` joined the RT-world
  implication gate + the per-frame-rebuild exception in `tr_main.cpp`, exactly like `r_rtao`).

The hit shading is lifted from `ssr_rt.frag` (RR4/RR16): geometry-table row → interpolate st/normal →
bindless albedo → loop the shared scene-light buffer (`RB_RHI_GatherSceneLights`, `rt_light_eval.glsl`)
→ add emissive (`baseColor.a`). One implementation of "light an arbitrary world hit," shared with RT
reflections and volumetrics.

## Slice plan

(Built in the order H6a → H6c → H6b; numbered here for readability. H6a–c are done and user-verified.)

- **H6a — noisy radiance gather. DONE + user-verified.** `rtgi_ray.frag` gathers one bounce into an RGBA16F
  MRT (`CreateRenderTargetRtgiMrt`: att0 radiance+hitdist, att1/att2 the NRD guides); dispatched by
  `RB_RHI_RtgiPass` in its own `r_vkGpuTime 2` scope "rtgi". Res-scaled by `r_rtgiResScale` (0.5) — a full-res
  single sample was ~4.1 ms, 0.5 ≈ 1.2 ms (the knee; 0.25 read too grainy raw). Cvars: `r_rtgi`,
  `r_rtgiRadius` (512), `r_rtgiResScale` (0.5), `r_rtgiLightScale` (1), `r_rtgiEmissive` (1), `r_rtgiDebug`.
- **H6b — NRD `REBLUR_DIFFUSE` denoise. DONE + user-verified.** ONE NRD instance, denoiser SWAPPED by config:
  `REBLUR_DIFFUSE` added to the instance; `RtaoDenoise` gained a `giMode` flag swapping the output image
  (RGBA16F `rtgiOut*` / `GetRtgiOutputImage`), the denoiser id, and the `IN/OUT_DIFF_RADIANCE_HITDIST` enums
  (RTAO path byte-identical when off). Only one ReBLUR dispatches per frame — no resolution thrash /
  descriptor-pool conflict. **GI replaces RTAO** when on: `RB_RHI_RtaoPass` stands down, screen-space GTAO
  fills the scalar AO the ambient/reflections consume, and the GI radiance itself grounds the scene. Radiance
  is NRD-YCoCg-packed (`_NRD_LinearToYCoCg` in the gather, `_NRD_YCoCgToLinear` in the apply/debug) — feeding
  it linear caused a pink cast. Needs the velocity MRT (`r_fsr` / `r_motionVectors`) for NRD's IN_MV.
- **H6c — scene-wide apply with a receiver-albedo G-buffer. DONE + user-verified.** Doom 3 has NO global
  ambient (the ambient shader runs only inside ambient-light volumes → patchy), so GI is applied by a
  FULLSCREEN additive pass, not the per-light shader. Correct per-pixel albedo needed a 4th prepass
  attachment: the VK backend was widened 3→4 color attachments (RenderTarget arrays, CreateColorTarget /
  BuildColorPasses, a new pass class, the pipeline blend array) + `GetRenderTargetImage4`. `gbuffer.frag`
  writes `out_Albedo` (discarded on the 1/2/3-att targets, the same trick `out_Material`/`out_Velocity` use);
  `RB_RHI_NormalPrepass` forces the standalone 4-att target + fills the SL_DIFFUSE stage when `r_rtgi` is on
  (merged 3-att path untouched, `r_rtgi` off byte-identical). `rtgi_apply.frag` adds
  `albedo * gathered-irradiance * r_rtgiIntensity` into `rhiHdrRT` before FSR2/tonemap (own "rtgi apply"
  scope). Reconcile: Doom 3's ambient lights (ambientLight materials) under GI follow `r_rtgiAmbient`
  (was `r_rtgiAmbientKeep`, then a hard "replace"): 0 = replace (draw nothing), 1 = FLOOR (`ambientlight.frag`
  adds only `max( 0, ambient - diffuse * RT )`, RT = the denoised GI buffer, or DI + GI at the PT tier, on unit 5),
  2 = FOLD (ambient lights become bounce-only rows behind the emitter rows - BOUNCEONLY without EMISSIVE - lighting
  every bounce hit through `rtgiAmbientAt`; the grid and DI never see them), **3 = both (default, user-verified on
  commoutside 2026-10-28)**. Carried in `u_color` (x = mode, y = RT scale, z = linear source). Ambient lights are
  rare in SP: commoutside, comm1, alphalabs2, RoE erebus6 / phobos2 - hell1 has none.

  **View-weapon leak fix (user-verified 2026-09-29).** The GI bled onto the first-person weapon (the
  world's panel/rail lines ghosting across the barrels). Two causes, both fixed:
  1. `rtgi_apply.frag` never checked the view-weapon mask (normal-buffer alpha, 0 on the weapon), so the
     reduced-res upscale pulled neighbouring world GI onto weapon pixels — the gather already skips the
     weapon (`rtgi_ray.frag` `nt.a < 0.5`), so the apply now skips it the same way.
  2. The real root cause: RTGI forces the **standalone** 4-attachment normal prepass (`wantAlbedo`), which
     goes through `BeginTargetPass` — and target passes force the full `[0,1]` depth range, dropping the
     view weapon's `SetDepthRange(0,0.5)` hack (only the *merged* prepass, `BeginNormalPrepass`, kept it —
     which is why SSAO was unaffected). So the depth-hacked weapon landed too far in the RTGI prepass and
     close world geometry won the depth test over it, writing world mask (and world normals → world GI) at
     weapon pixels. Fix: a one-shot `RHI::SetNextTargetHonorsDepthRange()` (VK `nextTargetHonorsDepthRange`,
     consumed in `EnterTargetPass`) that the standalone normal prepass requests, so it keeps the weapon
     depth hack like the merged prepass and the color pass. The weapon then wins the depth test and its
     `mask=0` covers the whole weapon. GL3 ignores it (its `glDepthRange` is global and already applies).
- **H6d — one dominant-light shadow ray. DONE.** `r_rtgiShadow` (default 1): the light loop tracks the
  BRIGHTEST reaching light at the bounce hit, then traces one shadow ray toward it (`gl_RayFlagsTerminateOnFirstHitEXT`,
  bias off the surface + toward the light); if occluded, that light's contribution is dropped. One extra
  ray per gather ray regardless of light count — not full NEE, but it kills the through-thin-wall leak.
  Menu: a "GI Shadows" checkbox.
- **H6e — preset + menu. DONE (numbers still worth a final pass).** `r_rtgi` wired into the Ultra Nightmare
  preset (new `rtgi` column in `enhancementPresets`, threaded through Apply/Detect), and that tier's
  `rtao=false` — GI replaces RTAO (the one NRD instance denoises GI; screen-space GTAO fills the scalar AO).
  Every lower tier carries `rtgi=false`, so a non-RT machine still sees UN == Nightmare AO. Menu: the
  "RT Global Illumination" toggle + Intensity / Fake-Ambient Keep / Ray Length / Resolution / GI Shadows
  sliders in the Enhancements → Ray Tracing section.

- **H6f — NRD history confidence (edge-ghosting antilag). DONE (tuning).** At full res the 1-bounce GI still
  trailed lit silhouettes under small camera motion: ReBLUR's depth/normal disocclusion passes a moving lit
  edge as "same geometry" and keeps accumulating stale radiance across it. Fix per NRD's own guidance (history
  confidence is "the best way to mitigate temporal lags", and an app should not rely solely on the built-in
  antilag): a `rtgi_confidence` pass runs between the gather and the denoise, reprojects last frame's DENOISED
  GI (`GetRtgiOutputImage`, still valid until this frame overwrites it) by the motion vectors, compares its
  luminance to this frame's bounce, and writes a per-pixel `IN_DIFF_CONFIDENCE` (0..1, `isHistoryConfidenceAvailable`)
  so NRD drops history only where the lighting actually moved — no flat-area dimming like a global antilag.
  Both signals are NRD-YCoCg so `.x` is luminance. A **noise deadzone** (rel-diff below ~0.35 ignored) keeps
  the noisy 1-spp current signal from starving history on stable surfaces; a **floor** (0.1) stops edge
  "frying". Cvar `r_rtgiConfidence` (default 4, 0 = off). GI path only; RTAO byte-identical.

- **H6g — full-res checkerboard gather. SHIPPED — default on, in the H6 GI merge (0.9.4).** `r_rtgiCheckerboard`
  (**default 1**, tuned on in `0af93d43` alongside resScale 0.75 / history 6 / intensity 1.35):
  instead of `r_rtgiResScale`'s uniform downscale (which blurs silhouette edges via coarse guides + bilinear
  upscale), trace a horizontal CHECKERBOARD — half the pixels at FULL resolution, packed into the left half of
  a full-res radiance target — and let NRD's `ReblurSettings::checkerboardMode` reconstruct the rest. Sharp
  edges at ~half the rays. Pieces: the gather (`rtgi_ray.frag`) maps each packed pixel to its full-res screen
  pixel `2*gx + (row^frame) parity` and early-outs the right half (matches NRD `Sequence::CheckerBoard =
  ((x^y)^frame)&1`, constant BLACK mode → valid bit 0); a ray-free full-res `rtgi_guides` pass writes the NRD
  guides (viewZ att1, normal att2) the half-packed gather can no longer carry; `RtaoDenoise` gains
  `checkerboard` (BLACK) + points the guides at that full-res target; velocity is used full-res (no downsample).
  The v1 limitation is **resolved**: the H6f history-confidence pass was wired into checkerboard mode
  (`c326e914`), so it is no longer bypassed.

## Fidelity note (per policy)

GI adds bounce light Doom 3 never had and lifts blacks in lit rooms — a real departure from its inky
contrast. Opt-in, gated to the top tier, off by default; the fake fills it supersedes are dialled back
under it (H6c) so the net exposure stays close to the shipped look, not brighter-everywhere.

## Files

- `neo/shaders/rtgi_ray.frag` — the gather (setup from `rtao_ray.frag`, hit shading from `ssr_rt.frag`;
  YCoCg-packs radiance + writes normHitDist for NRD).
- `neo/shaders/rtgi_apply.{vert,frag}` — the fullscreen additive apply (`albedo * YCoCg-unpacked GI * intensity`).
- `neo/shaders/rtgi_debug.{vert,frag}` — the `r_rtgiDebug` overlay (YCoCg-decoded raw / denoised blit).
- `neo/shaders/gbuffer.frag` — `out_Albedo` (4th prepass attachment); `ambientlight.frag` — the `r_rtgiAmbient` floor.
- `RhiWorld.cpp` — `RB_RHI_RtgiPass` (gather + `giMode` NRD denoise), `RB_RHI_RtgiApply`, `RB_RHI_RtgiDebugOverlay`,
  the `rhiRtgi*` statics, `RB_RHI_NormalPrepass` (4-att albedo), `RB_RHI_RtaoPass` (stands down when `r_rtgi`).
- `VulkanBackend.cpp` / `RHI.h` — `CreateRenderTargetRtgiMrt`; the 3→4 color-attachment widening +
  `GetRenderTargetImage4`; `RtaoDenoise` `giMode` + `RtgiEnsureOutput` / `GetRtgiOutputImage`; `NRD_ID_REBLUR_GI`.
- `RenderSystem_init.cpp` / `tr_local.h` — the `r_rtgi*` cvars (`r_rtgi`, `r_rtgiRadius`, `r_rtgiResScale`,
  `r_rtgiLightScale`, `r_rtgiEmissive`, `r_rtgiIntensity`, `r_rtgiAmbient`, `r_rtgiDebug`).
- `tr_main.cpp` — `r_rtgi` in the RT-world implication gate + the per-frame TLAS-rebuild exception.
