# H2 — NRD denoiser + temporal framework (Vulkan)

The linchpin phase of [rtx-hybrid-roadmap.md](rtx-hybrid-roadmap.md): every RT effect after
it (H3 soft shadows, H4 RTAO, H5 glossy reflections, H6 GI) traces ~1 sample/pixel and is
unusable raw. NRD (NVIDIA Real-Time Denoisers) turns those noisy signals into stable images
using guide buffers we already produce. H2 itself ships **nothing user-visible** — it is the
substrate the later phases render into, proven by a synthetic-signal validator.

## Status

- **H2a — vendor + build (DONE, this commit).** NRD v4.18 vendored at
  [libs/nrd](../neo/libs/nrd/README-DUDE.md) following the FSR2 pattern: sources + prebuilt
  embedded SPIR-V (31 compute shaders), no HLSL toolchain at engine build time. Built as its
  own `nrd` static-lib CMake target (its compile definitions must exactly match the
  prebuilt-shader config and must not leak) and linked into the binary under
  `DHEWM3_VULKAN`. Compiles clean on gcc/Linux; mingw cross-build untested (build-win.sh —
  check before the next Windows release).
- **H2b — VK translation layer (DONE, this commit).** NrdCreate/NrdDestroy/NrdSelfTest in
  VulkanBackend.cpp (FSR2-section style, not a separate file — the backend class lives in
  the .cpp). `r_nrdTest 1` self-test **PASS on the 3080 Ti**: 18 compute pipelines from the
  embedded SPIR-V, 6 permanent + 10 transient pool textures, 2 immutable samplers, 960 B ×
  35-set CB ring, 29 dispatches/frame enumerated for the two denoisers. Discovery vs the
  plan: NRD's SPIR-V uses **two register spaces = two descriptor sets** (resources set 0:
  textures t20+/storage u3+; CB+samplers set 1: CB b2, samplers s0+), so the layer builds a
  shared CB/samplers set layout + per-pipeline resource layouts, and each dispatch will
  consume two sets. `VK_KHR_compute_shader_derivatives` (quads) is probed and enabled at
  device creation (`haveComputeDerivatives`); NrdCreate refuses without it. GPU dispatch
  RECORDING is deliberately not in H2b — it lands with H2d's validator, which is what can
  prove it.
- **H2c — guide-input production (REMAINING — lands with the first consumer, H3/H4).**
  World-normal/roughness prep pass, linear viewZ, MV wiring (see the table below).
- **H2d — dispatch recording + validator (DONE, this commit).** `NrdRecordDispatches`
  records a frame of denoiser work (CB ring slices, two descriptor sets per dispatch,
  pipeline + vkCmdDispatch, compute→compute barrier after each; pool images transitioned
  to GENERAL on first use; descriptor pool reset per recorded frame — single-consumer
  model, revisit if a denoiser ever records twice per frame). `r_nrdTest` now validates
  END TO END on the GPU: a 256×256 flat-plane scene (constant viewZ 100, packed flat
  normal, zero MV) with a per-pixel Bernoulli 0/1 hit-distance signal re-rolled each of 16
  frames (mean 0.5, variance 0.25) denoised by REBLUR_DIFFUSE_OCCLUSION →
  **PASS on the 3080 Ti: mean 0.498, variance 0.00027 (~900× reduction), zero NaN,
  validation-layer clean.** Gotcha found: NRD shaders declare the SPIR-V `Int16`
  capability → `VkPhysicalDeviceFeatures::shaderInt16` must be enabled (it is now);
  without it every NRD shader module fails validation and the cascade corrupts
  validation's cb state tracking into misleading downstream errors.

## How NRD integrates (the renderer-agnostic contract)

NRD is graphics-API-free. The host does all GPU work; NRD tells it *what* to run:

1. `nrd::CreateInstance(denoisers[])` → `nrd::InstanceDesc`: a list of **pipelines** (each
   with embedded SPIR-V, descriptor layout: constant buffer b-slot, samplers s-slots,
   textures t/u-slots at the register shifts s0 b2 u3 t20), **permanent + transient texture
   pools** (formats/sizes as fractions of render size), and static **samplers**.
2. Per frame: `nrd::SetCommonSettings` (matrices, jitter, MV scale, frame index) +
   per-denoiser settings, then `nrd::GetComputeDispatches(identifiers[])` → an ordered list
   of dispatches: {pipeline, constant-buffer bytes, resource bindings (pool slot or user
   input/output), grid size}. The host records them into its command buffer with barriers.
3. Inputs/outputs are the host's images (`IN_MV`, `IN_NORMAL_ROUGHNESS`, `IN_VIEWZ`, per-
   denoiser `IN_*`/`OUT_*`); pool textures are host-allocated once per resolution.

### DUDE mapping (H2b plan)

New `neo/renderer/rhi/vk/NrdIntegration.cpp` (+ small RHI-side hooks), VK-only, gated on
`SupportsRayQuery` hardware like the rest of RT:

- **Pipelines:** one VkComputePipeline per `PipelineDesc` from the embedded SPIR-V
  (`ShaderMake::ShaderBlob` parses the `--headerBlob` arrays; NRD hands back the right
  permutation). One descriptor-set layout per pipeline from its `DescriptorRangeDesc`s;
  push-descriptor-free, classic per-dispatch sets from a dedicated pool (the RHI's
  existing per-frame descriptor machinery pattern — see rhiVkUnits — but compute).
- **Pool textures:** allocate via VMA at `CreateInstance` time (and on resolution change),
  formats mapped from `nrd::Format` to VkFormat (RGBA16F/R16F/RG16F/R8 etc. — a small
  switch). Transient pool could alias memory later; first cut allocates plainly.
- **Constant buffer:** one host-visible ring buffer (the persistent-mapped pattern the
  GL3/VK RenderParams UBO already uses), `dispatch.constantBufferData` memcpy'd per
  dispatch at a 256-aligned offset.
- **Barriers:** NRD dispatch order is already correct; between dispatches a single
  `VK_ACCESS_SHADER_WRITE|READ` image/memory barrier per written resource (compute→compute)
  suffices — mirror the batched-barrier pattern from the GPU-skin compute work
  (`ComputeArgs::deferBarrier`).
- **Compute queue:** run on the graphics queue in-line first (correct, simple); async
  compute is a later refinement (the roadmap flags it for AO/shadows overlap).

### Guide inputs (H2c plan) — what NRD needs vs what exists

| NRD input | Exists today | Gap |
|---|---|---|
| `IN_MV` (2D screen-space MV, +2D/2.5D ok) | per-object velocity MRT (R1/A2, `r_motionVectors`, drives FSR2 + SSR/SSAO temporal) | format/scale conform via `CommonSettings::motionVectorScale`; static-geo pixels carry camera reproj — velocity buffer already handles both |
| `IN_NORMAL_ROUGHNESS` | normal G-buffer (view-space RGBA8) + SSR rough/metal MRT | needs **world-space** normal + roughness packed to `NRD_NORMAL_ENCODING=2` (R10G10B10A2 oct). One fullscreen prep pass: unpack view normal → world (inverse view rotation), fetch roughness, oct-encode. |
| `IN_VIEWZ` (linear view depth, R16F/R32F) | SSAO linear-depth mip level 0 (R16F, positive) | reuse directly at AO res; full-res variant = same linearize pass at full res (cheap) |
| Per-effect `IN_*` (e.g. `IN_DIFF_HITDIST`, `IN_SHADOWDATA`) | n/a | produced by H3/H4/H5 ray passes |

The prep pass is the only new per-frame cost H2 adds when idle (~fullscreen RGBA10 write);
consider folding it into the gbuffer prepass once H3 lands (write both encodings at once).

### Validator (H2d plan)

`r_nrdTest` console command (pattern: `r_rtAnimBlasTest`): fill a synthetic `IN_VIEWZ` +
`IN_NORMAL_ROUGHNESS` (flat plane) + a noisy `REBLUR_DIFFUSE` hit-distance signal (checker
of 0/1 occlusion + white noise), run N frames of denoise with a static camera, read back
the output, assert (a) spatial variance collapsed below a threshold, (b) mean preserved
within tolerance, (c) no NaNs. PASS/FAIL to console — runnable headless by the user like
the other RT validators.

## Denoiser lineup (per roadmap)

- **SIGMA_SHADOW** — H3 soft shadows (penumbra from area-sampled shadow rays).
- **REBLUR_DIFFUSE_OCCLUSION** — H4 RTAO (hit-distance only, cheapest ReBLUR mode).
- **REBLUR_SPECULAR** (or ReLAX if it fights the art) — H5 glossy reflections.
- The validator instantiates REBLUR_DIFFUSE_OCCLUSION (closest to first consumer H4;
  SIGMA needs light geometry the synthetic can't fake as easily).

## Risks / notes

- **Quad intrinsics:** shaders built with `NRD_SUPPORTS_QUAD_INTRINSICS=1` →
  `VK_KHR_compute_shader_derivatives` must be enabled at device creation when available
  (3080 Ti: yes). Add to the extension gate next to the RT extensions; if unavailable at
  runtime we must rebuild the vendored SPIR-V with the flag off (documented in
  libs/nrd/README-DUDE.md).
- **Encoding lock-in:** `NRD_NORMAL_ENCODING=2` / `ROUGHNESS=1` are baked into the vendored
  SPIR-V; the C++ target's defines mirror them (CMake comment enforces). The prep pass must
  encode to exactly this.
- **mingw:** NRD upstream supports MSVC/clang/gcc; cross-build needs a check before the
  next Windows release (build-win.sh).
- **Fallback:** if NRD integration stalls, the roadmap's fallback is a hand-rolled
  SVGF-style denoiser on the existing ssr/ssao temporal machinery. Keep NRD.

## NRD improvements across consumers (investigation, 2026-10-11)

The ReSTIR DI review ([rtx-restir-di.md](rtx-restir-di.md), *Review against public ReSTIR* + *Plan R0-R6*) produced a set
of NRD-side improvements. This section asks which of them carry to the OTHER NRD consumers, and how. Read against
`VulkanBackend::RtaoDenoise` (the one shared dispatch path), `RB_RHI_RtaoPass`, `RB_RHI_RtgiPass`, the ray shaders and
NRD v4.18's headers/README.

### Who uses NRD today

| Consumer | Denoiser | Cvar / history | Res | Notes |
|---|---|---|---|---|
| RTAO (H4) | `REBLUR_DIFFUSE_OCCLUSION` | `r_rtao`, `r_rtaoHistory` **45** frames | `r_rtaoResScale` | 1 cosine ray; weapon masked to 0 (`rtao_ray.frag:88`); no confidence |
| RTGI (H6) | `REBLUR_DIFFUSE` | `r_rtgi`, `r_rtgiHistory` **14**, `r_rtgiConfidence` 1.8 | 0.75 + checkerboard | 1 bounce; weapon masked to 0 (`rtgi_ray.frag:107`); luminance confidence (`rtgi_confidence.frag`); stands down under DI |
| ReSTIR DI (H7) | `REBLUR_DIFFUSE` / `_SH` | `r_rtReStirDI`, history 16 | 0.75 + checkerboard | covered by Plan R0-R6; in combined mode (`r_rtReStirDICombineGI`, the `dude_rtx 2` default) the GI bounce rides DI's denoise, so R0-R6 reach it automatically |
| SIGMA_SHADOW | `SIGMA_SHADOW` | - | - | **in the instance (`NrdCreate` table) but never dispatched** since H3 was abandoned; NRD still allocates its share of the permanent + transient pools |

Not NRD (own temporal, out of scope here): SSR / RT reflections (`ssr_temporal`), volumetrics, the RT shadow blur, FSR2.

All three live consumers share one code path (`RtaoDenoise`), one velocity source (the gbuffer `R1/A2` RG16F attachment,
9 read sites in RhiWorld.cpp), one normal encoding (`R10_G10_B10_A2_UNORM`, materialID in `.w`, always 0) and the same
reset rule (`da.reset = !prevValid`, i.e. first frame / interruption only). That is the lever: most of the items below
are ONE change in the shared path, not three.

### Applicability matrix

| Improvement (from R0-R6) | RTAO | RTGI (standalone, hybrid tier) | DI (+ combined GI) | How |
|---|---|---|---|---|
| Camera-cut NRD reset (`rhiTemporalCam.historyReset`) | **yes - missing** | **yes - missing** | yes (R0) | one line in `RtaoDenoise`: `a.reset \|\| historyReset` -> all three |
| NRD validation overlay + history-length view | yes | yes | yes (R0) | shared hook (`cs.enableValidation`, bind `OUT_VALIDATION`); a debug value per feature |
| History in seconds (`GetMaxAccumulatedFrameNum` + 0.2 s low-pass FPS) | **yes** - 45 frames = 0.31 s @144 Hz but 0.75 s @60 Hz | **yes** | yes (R0) | shared helper; each `*History` cvar keeps its number, a companion `*HistoryMs` (0 = frames) switches the unit. Matters more since VRR / `com_maxFPS` caps vary the frame rate |
| Fast history / clamp sigma / stabilization exposed | **yes - NRD: `fastHistoryClampingSigmaScale` "1.1 is a safe value for occlusion denoising"**, NRD-Sample occlusion mode uses `historyFixFrameNum 2`; RTAO sits at 2.0 / 3 | yes (NRD-Sample: 1.5 "works well even for dirty signals") | yes (R0) | make the giMode-only overrides in `RtaoDenoiseArgs` generic (`fastFrames`, `fastClampSigma`, `stabilizedFrames`); RTAO gets a `r_rtaoFastClamp` defaulting to today's 2.0 for the A/B - the expected win is snappier AO under moving occluders (NPCs, doors) at no cost |
| 2.5D motion vectors (`.z = viewZprev - viewZ`) | **yes** | **yes** | yes (R4) | one gbuffer change (`out_Velocity` gains a channel, or a companion R16F attachment only when an NRD consumer is on) + `motionVectorScale[2] = 1` in the shared path; FSR2 keeps reading `.xy`. NRD: "2D MVs can lead to massive history reset on moving objects" - the NPC AO/GI trail |
| Weapon material ID + alternate history-fix stride | **yes** | **yes** | yes (R4) | RTAO/RTGI write a ZERO hole under the weapon (`nt.a < 0.5`) with valid guides, so NRD blurs the hole into the world at the silhouette and the world the weapon uncovers has no history - the "fried halo" DI hit and solved by shading the weapon unmasked. Tagging the hole `materialID 1` + `minMaterialForDiffuse 0` stops the spatial mixing; `historyFixAlternatePixelStrideMaterialID` is NRD's documented knob for "protagonist's weapon". Both ray shaders + `rtgi_guides.frag` pack `.w` |
| Gradient confidence (reservoir / RNG replay) | **no** - AO has no lighting to change; geometry disocclusion is NRD's own test | **partly** | yes (R1) | RTGI's `rtgi_confidence` has the same noise-vs-change weakness as DI's (hence its deadzone 0.35 + floor 0.1); it survives because the bounce is low-variance. Combined mode: R1 covers it for free. Standalone RTGI: a Q2RTX-style RNG replay is feasible (the bounce direction is IGN + a CPU-hoisted per-frame rotation/offset - pass last frame's, re-trace 1/9 of the pixels at the reprojected surface, compare to last frame's RAW bounce) but only worth it if hybrid-tier GI trails are reported; Remix's cheaper route (invalidate GI history from DI gradients) needs DI, which the hybrid tier lacks |
| ReLAX | **no** - NRD-Sample: "RELAX doesn't support AO / SO denoising" | optional, low | yes (R2) | Remix does use ReLAX for indirect (250 ms history), but NRD positions ReBLUR for dirty 1-spp input, which the 0.75-res checkerboard bounce is. Once R2 adds the `denoiser` arg the RTGI A/B costs nothing to try |
| SH / SG resolve | no (scalar occlusion) | **yes** | yes (R5) | RTGI at 0.75 + checkerboard loses bump N·L exactly as DI did before its SH mode; NRD: SH is "a must-have when NRD produces a lower-resolution output". `rtgi_bounce.glsl` already returns `bounceDir`; `rtgi_apply.frag` gets the SG resolve. Cost ~+0.4-0.8 ms (NRD table: REBLUR 2.55 -> 3.40 ms for DIFFUSE_SPECULAR). Hybrid tier only (under DI the combined buffer is already SH) |
| hitT hygiene | ok (miss -> 1.0 = unoccluded, correct) | ok (`thit` normalized, miss -> 1) | R4 | nothing to do |
| Spatial reuse / disocclusion boost | n/a | n/a (no light selection to reuse) | R3 | RTGI's disocclusion answer stays NRD's history fix; ReSTIR GI-style reuse is a separate phase (roadmap) |
| SIGMA | - | - | - | **dead weight**: drop it from the `NrdCreate` table (fewer pipelines, smaller pools, faster `NrdCreate`), or keep it for the one experiment it is good at: per-light penumbra from occluder distance, lag-free (5-frame stabilization, 0.40 ms/light per NRD's table) as an alternative to the screen-space `r_rtShadowBlur` on the hybrid tier's hard RT shadows (5 lights/frame, +2.27 ms today). H3 was abandoned over ray cost + stagger, not SIGMA's quality, so this is unproven either way. Low priority |

### Plan N0-N3 (folded into the R stages where they share code; NOT started)

- **N0 - with R0 (shared path, zero-risk) - IMPLEMENTED 2026-10-12 with R0 (branch `feat/rtx-restir-di-r0-diagnostics`, user test pending):** cut reset reaches RTAO / RTGI / DI through `RB_RHI_NrdHistoryReset`; `RtaoDenoiseArgs` gained generic `fastFrames / fastClampSigma / stabilizedFrames / antilagOff / returnHistoryLength / validation`; cvars `r_rtaoHistoryMs / r_rtaoFastClamp / r_rtaoHistoryFix / r_rtgiHistoryMs / r_rtgiFastClamp / r_nrdSigma` (SIGMA now OUT of the instance by default). Validation / history views are DI-only so far. Original scope: cut reset, validation / history-length views, seconds-based history, the
  generic fast/clamp/stabilize fields with per-feature cvars at today's values; RTAO A/B `r_rtaoFastClamp 1.1` +
  `r_rtaoHistoryFix 2` (the NRD occlusion preset) for AO trails under moving occluders. Measure with `r_vkGpuTime 2`;
  acceptance = byte-identical at defaults, no AO/GI ghost after a cinematic cut or teleport.
- **N1 - with R4:** 2.5D velocity for all consumers (one gbuffer + one shared-path change), weapon material ID in
  `rtao_ray.frag` / `rtgi_ray.frag` / `rtgi_guides.frag`. Acceptance = NPC under a static light shows no history reset on
  its body in the validation view (RTAO + RTGI), no dark halo at the weapon silhouette in AO / GI debug views.
- **N2 - after R1/R2 (hybrid-tier RTGI only, on report):** RNG-replay gradient for standalone RTGI; RTGI ReLAX A/B via
  R2's `denoiser` arg. Both opt-in cvars, both trimmed if they lose.
- **N3 - optional:** RTGI SH mode + SG resolve for the hybrid tier (R5's resolve code reused); SIGMA removal, or the
  per-light SIGMA penumbra experiment against `r_rtShadowBlur`.

Non-negotiables as everywhere: VK + `SupportsRayQuery()`, default-off / default-today cvars, GL3 and non-RT untouched,
new shaders need the CMake re-configure.
