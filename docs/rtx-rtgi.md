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

- **H6a — noisy radiance gather + debug view. DONE (feat/rt-gi-onebounce), build-verified, not user-tested.**
  New `rtgi_ray.frag` gathers one bounce into an RGBA16F target (`CreateRenderTargetRtgiMrt`); dispatched
  by `RB_RHI_RtgiPass` (RhiWorld.cpp) right after the AO passes, in its own `r_vkGpuTime 2` scope "rtgi".
  Visualised by `r_rtgiDebug 1` (`RB_RHI_RtgiDebugOverlay`, a `smaa_copy` blit of the raw radiance).
  Nothing consumes it in normal play, so `r_rtgi 0` (default) leaves play + the RTAO path byte-identical.
  Cvars: `r_rtgi`, `r_rtgiRadius` (512), `r_rtgiLightScale` (1), `r_rtgiEmissive` (1), `r_rtgiDebug`.
  Mirrors how RTAO shipped H4a (raw ray + `r_rtaoDebug`, visually inert).
- **H6b — NRD DIFFUSE denoise + reduced-res.** Switch the denoiser from `REBLUR_DIFFUSE_OCCLUSION`
  (scalar) to `REBLUR_DIFFUSE` (RGBA16F radiance + hit distance): a second NRD identifier in the
  VulkanBackend lineup, the `nrdUserViews` mapping swapped to `IN_DIFF_RADIANCE_HITDIST` /
  `OUT_DIFF_RADIANCE_HITDIST`, and the `r_rtaoResScale`-style reduced-res trace + velocity downsample +
  history reproject (all already built for RTAO). The `rtgi_ray.frag` already writes the guides and a
  normalised hit distance in alpha for this.
- **H6c — apply + reconcile the fake fills.** Add the denoised radiance to the ambient term
  (`ambientlight.frag:65`, `outRgb *= ao;` → `outRgb = outRgb * ao + gi * outRgb`), gated so **GI replaces
  RTAO** (one AO producer, not both). Then dial down / disable the fills GI now supersedes so the room
  doesn't go milky: `r_itemGlow`, `r_emissiveSurfaces`, the flat `_ambient`. This reconciliation *is* the
  work of H6c — the ray cost is the easy part. **Measure the GI-vs-RTAO delta with `r_vkGpuTime 2` on
  `mars_city1` nightmare before committing** (estimate ~1–3 ms at 0.75 res-scale).
- **H6d — one dominant-light shadow ray.** The scene-light buffer path casts no shadow ray at the hit
  yet (`ssr_rt.frag` RR16d, parked), so bounced light leaks through walls. Trace one shadow ray toward
  the nearest/dominant reaching light at the bounce hit (`r_rtgiShadow`). Cheap: one extra ray, only for
  the dominant light, not full NEE.
- **H6e — preset + perf.** Wire `r_rtgi` into the Ultra Nightmare preset (replacing `r_rtao` there),
  fold `r_rtgiIntensity`/tuning into the in-game menu, and take the final `r_vkGpuTime 2` numbers.

## Fidelity note (per policy)

GI adds bounce light Doom 3 never had and lifts blacks in lit rooms — a real departure from its inky
contrast. Opt-in, gated to the top tier, off by default; the fake fills it supersedes are dialled back
under it (H6c) so the net exposure stays close to the shipped look, not brighter-everywhere.

## Files

- `neo/shaders/rtgi_ray.frag` — the gather (setup from `rtao_ray.frag`, hit shading from `ssr_rt.frag`).
- `RhiWorld.cpp` — `RB_RHI_RtgiPass` (dispatch), `RB_RHI_RtgiDebugOverlay`, the `rhiRtgi*` statics.
- `VulkanBackend.cpp` / `RHI.h` — `CreateRenderTargetRtgiMrt` (RGBA16F radiance + the two NRD guides).
- `RenderSystem_init.cpp` / `tr_local.h` — the `r_rtgi*` cvars.
- `tr_main.cpp` — `r_rtgi` in the RT-world implication gate + the per-frame TLAS-rebuild exception.
