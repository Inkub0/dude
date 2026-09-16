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
- **H2c — guide-input production.** World-normal/roughness, linear viewZ, MV packing.
- **H2d — validator.** `r_nrdTest`: synthetic noisy signal → denoise → variance assert.

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
