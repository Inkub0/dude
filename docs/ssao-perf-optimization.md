# SSAO GPU-cost optimization — design

Follow-up to [ssao-gtao.md](ssao-gtao.md). The SSAO look is settled (accurate depth-normal
reconstruction on lower tiers, the normal G-buffer on High+, POM for world micro-relief, the
retuned presets). This doc is about making the **AO shader itself cheaper** — the horizon
search in [`ssao.frag`](../neo/shaders/ssao.frag) — without changing the result.

## Where the cost is (measured this session)

- The AO horizon search is **cache/ALU-bound**, and DUDE runs it at a **wide world radius**
  (`r_ssaoRadius`, 36–72). Far march steps land many pixels away in `_currentDepth`, so the
  per-step `rawDepth` fetches miss cache — the dominant cost at the higher presets (full-res +
  many steps was ~3 ms of the Nightmare frame before the retune).
- SSAO looms large in DUDE partly for structural reasons (forward renderer pays a normal
  prepass a deferred engine gets free; the 2004 base frame is very light) — see ssao-gtao.md.
  These optimizations attack the remaining *shader* cost, which is the part still worth cutting
  on High+.

All three below are **perf-only and visually identical** (verify by flag-on/off image-diff +
an in-engine look) — not opt-in look changes. They stack with everything already shipped.

Current pipeline recap ([`RhiWorld.cpp` `RB_RHI_SSAOPass`](../neo/renderer/rhi/RhiWorld.cpp)):
depth prepass → `_currentDepth` capture → **SSAO pass** (`ssao.frag`, half-res-ish) → separable
bilateral blur → optional temporal. `ssao.frag` reconstructs view-space position per tap from
`_currentDepth` via `viewPosFromRaw`.

---

## Phase 1 — Prefiltered depth mip chain (biggest leverage) — GL3 + Vulkan

**Status: BUILT (feat/ssao-depth-mip), user-tuned.** Behind `r_ssaoDepthMip` (default 1; off =
the exact prior full-res raw-depth march, for an A/B) with `r_ssaoDepthMipBias` (default **0.2**)
tuning LOD aggressiveness. **Silhouette-halo note:** a depth mip is a *point sample of a
downsampled depth*, so at any depth discontinuity one coarse texel stands in for two surfaces —
no filter (avg/min/max) can represent both, and the surviving foreground occluder still spreads
its influence across the coarse footprint (positional quantization). The max-downsample removed
the worst (phantom mid-depth) component; keeping steps on finer mips (`bias 0.2`) shrinks the
residual band to near-invisible while retaining most of the speedup (measured ~100→133 fps at
`bias 0.5`, most of it kept at 0.2). Fully eliminating the halo means not downsampling near
edges — i.e. Phase 3 compute tiling marches full-res depth from shared memory. As built: a new RHI capability — `CreateRenderTargetMipped` +
`BeginTargetMipPass` + `GetRenderTargetMipImage` (GL3: per-level `glTexImage2D` + a shared FBO
re-pointed per level; Vulkan: an `mipLevels`-deep image with a level-0 attachment view, an
all-levels sample view, and per-level single-level views + framebuffers so each coarse level is
*rendered*, not blitted). A new `ssao_depthmip` fullscreen pass linearizes `_currentDepth`
(positive view-space eye depth) into level 0 at the AO resolution; then `ssao_depthdown` fills
levels 1..N by a **max (farthest) downsample** — a box average blends fg/bg across silhouettes
into a phantom mid-depth occluder, casting radial dark halos; the conservative farthest-surface
filter removes that while level 0 stays exact (6 levels max). The per-level render passes'
external dependencies serialize the cross-frame write-after-read for free (no manual barriers).
`ssao.frag` keeps `_currentDepth` for the centre pixel + normal reconstruction (zero fidelity
change there) and reads the mip only for the cache-bound horizon occluder taps, `textureLod`ing a
coarser level as the step distance grows (`lod = clamp(log2(stepPix·bias), 0, maxMip)`); `maxMip`
and `bias` ride the free `depthTexRecip.zw` lanes (no `RenderParams` growth). **Format:** RGBA16F
with linear depth in `.r` — R16F single-channel storage is deferred to Phase 2 (below) to avoid
format-enum surgery for the first cut. Verify with `r_vkGpuTime`/`r_gl3GpuTime` A/B (expect the AO
pass to drop at High+/Nightmare where the radius is wide), a flag-on/off look (AO should be
near-identical), and check floors/ceilings on Vulkan for any y-flip.

**Idea (XeGTAO "PrefilterDepths").** Build a small mip hierarchy of **linear view-space depth**
once per view, then have the horizon march sample a **coarser mip for farther steps**. Far taps
then read a small, cache-local footprint instead of scattering across full-res depth. This is
the single biggest win for a wide-radius search (the literature calls out GTAO as cache-bound).

**Why linear depth, not `_currentDepth`.** Averaging raw projection depth across a 2×2 is
meaningless (non-linear); mips must hold linear view-space depth (`viewPosFromRaw`'s `vz`).
XeGTAO stores 16-bit viewspace depth and mips it (ties into Phase 2).

**DUDE mapping:**
- New pass before `RB_RHI_SSAOPass`: linearize `_currentDepth` into a **mipped R16F/R32F
  target** (`rhiSsaoDepthMip`), then fill mips 1..N. Mip fill = a min/average reduction; XeGTAO
  uses a specific 5-tap-ish filter to avoid haloing — start with a plain 2×2 average, refine if
  edges halo.
- RHI plumbing: the render-target family has no mipped-target/downsample helper today. Options:
  (a) create the target with mips and run a small downsample fragment pass per level
  (`BeginTargetPass` per mip, sampling the previous level); (b) on GL3, `glGenerateMipmap` of a
  linear-depth texture (cheap but box-filtered). (a) is backend-portable and controls the filter.
- `ssao.frag`: replace `rawDepth(frag)` in the march with a `textureLod` of the linear-depth
  mip, LOD chosen from the step's screen-space distance
  (`lod = clamp(log2(stepPix * t / basePix), 0, maxLod)`). Near steps still hit mip 0 (full
  detail where it matters); far steps drop to coarse mips.
- Center pixel + normal reconstruction stay on mip 0.

**Gain:** the biggest of the three for the wide radius; scales with `r_ssaoRadius`.
**Effort:** moderate. **Risk:** LOD/filter tuning (haloing at silhouettes) — the bilateral blur
already downstream helps hide residual error.

---

## Phase 2 — fp16 (half precision) — storage GL3+VK, math Vulkan-only

**Idea (XeGTAO fp16).** 16-bit float math on the horizon search + 16-bit depth/AO storage.
XeGTAO reports **5–20%** with acceptable precision loss.

**Storage — BUILT (both backends).** The Phase-1 linear-depth mip is now **R16F** (new
`IF_R16F` in the RHI `ImageFormat` enum; GL3 `GL_R16F`/`GL_RED`/`GL_HALF_FLOAT`, Vulkan
`VK_FORMAT_R16_SFLOAT` with its own pipeline `passClass` 7 so the mip's pipelines build against
a format-correct render pass). Only `.r` was ever written/read (positive linear eye depth), and
the RGBA16F first cut already stored that as a half-float — so R16F is **bit-identical in `.r`
at a quarter the footprint**. Zero fidelity change; it purely shrinks the mip so more of the
horizon march's working set stays in cache — i.e. it *compounds* the Phase-1 win rather than
adding a new one. No shader edits (the linearize/downsample/march shaders already touch `.r`
only).

**AO-output storage — intentionally left RGBA8.** The doc's original "AO output as R8/RG16F"
doesn't apply: the AO buffers pack a **bent normal in `.gba`** (`ssao_blur.frag` writes
`vec4(ao, bn*0.5+0.5)`; `ambientlight.frag` and `ssao_temporal.frag` read `.gba` for directional
AO). R8/RG16F would drop the bent normal, so `rhiSsaoRT`/`rhiSsaoBlurRT`/`rhiSsaoHistRT` stay
RGBA8. The single-channel depth mip was the only AO-path buffer with dead channels to reclaim.

**fp16 math (Vulkan-only) — RESOLVED: SKIP on the GA10x dev box.** The math half would need
Vulkan + `GL_EXT_shader_explicit_arithmetic_types_float16` / `shaderFloat16`
(VK_KHR_shader_float16_int8): query `VkPhysicalDeviceShaderFloat16Int8Features` chained to the
features2 query, enable it at device creation, expose a capability flag, add a second
`ssao_fp16.frag` variant (accumulate the visibility integral `cH_pos/cH_neg` in `float16_t`, keep
position reconstruction fp32) + its own pipeline, and pick it at runtime. A sourced review
(2026-08) killed it for this hardware on two independent grounds:

1. **Consumer Ampere runs scalar `float16_t` at 1:1 with FP32.** The NVIDIA GA102 whitepaper's
   own non-Tensor throughput table lists the RTX 3080 at **FP32 29.8 TFLOPS = FP16 29.8 TFLOPS**
   (contrast Turing RTX 2080: 10.6 vs 21.2 = 2:1). GA10x's 2:1 "double-speed HFMA" only fires
   when the compiler packs work into `half2` SIMD pairs; the horizon integral is a chain of
   *dependent scalar* accumulators that cannot fill both lanes, so it hits the 1:1 case. (Turing,
   RDNA/GCN, and datacenter GA100 do get real 2:1 — the calculus differs there.)
2. **The pass is bandwidth-bound, not ALU-bound.** The R16F *storage* change alone gave 72→98 fps
   — direct proof the bottleneck was the depth fetches, not the arithmetic. Halving ALU precision
   on a bandwidth-bound pass buys almost nothing.

**Expected fp16-math gain on the RTX 3080 Ti: ~0–3%** (lighter register pressure/occupancy only),
inside measurement noise — for a permanent Vulkan-only shader+pipeline+device-feature maintenance
surface. Not worth it here. **Flip conditions:** an `r_vkGpuTime` profile showing the AO pass has
become ALU/occupancy-bound (e.g. after further fetch cuts), OR restructuring the integral to
expose genuine `half2` pairs, OR targeting a 2:1-fp16 GPU (Turing/RDNA/GCN/GA100) where the pass
is ALU-bound. Sources: GA102 whitepaper v2.1; Turing architecture in-depth; A100 whitepaper; CUDA
C++ Programming Guide compute-capabilities throughput table.

**Gain:** storage compounds Phase 1's cache win (portable, shipped, measured 72→98); math ~0–3%
on GA10x. **Effort:** low (storage, done) + low-moderate (VK math, not built). **Risk:** storage
none (bit-identical `.r`); math = a VK-only maintenance surface for a near-noise local win.

---

## Phase 3 — Compute + shared-memory depth tiling (Vulkan-only, biggest raw win)

**Idea.** Rewrite the AO pass as a **compute shader**. Each workgroup (e.g. 8×8 or 16×16)
cooperatively loads its depth tile **+ an apron** into `shared` memory, then every thread runs
the horizon search reading depth from shared memory instead of re-fetching the texture. This
kills the redundant per-step fetches that dominate the cost, writes straight to a storage image
(no render-pass load/store), and opens subgroup shuffles.

**DUDE mapping (and prerequisites):**
- **The RHI has no compute path today** (confirmed — no `Dispatch`/compute stage). Phase 3 needs
  a small RHI addition: a compute pipeline + `Dispatch(x,y,z)` + storage-image binding, wired in
  the Vulkan backend only (the GL3 backend stubs it → keeps the fragment `ssao.frag`).
- Port the horizon search to `ssao.comp` with a `shared` depth tile.
- **Do Phase 1 first.** A wide radius means a large apron; with the depth-mip (Phase 1) the far
  taps read coarse mips (tiny footprint), so shared memory only needs the fine **near** tile —
  keeping the apron and LDS budget small. Without Phase 1 the apron for radius 36–72 is
  impractical.
- Optional: **async compute** — run the AO on an async queue overlapping the shadow-map / other
  graphics work, hiding its latency (Vulkan 1.4). A later refinement once the compute path exists.

**Gain:** the largest raw GPU win (eliminates redundant fetches). **Effort:** high (new RHI
compute capability + a compute shader + tiling). **Risk:** high; Vulkan-only. Do last.

---

## Phasing / ROI

1. **Phase 1 (depth mip)** — biggest cache win, portable, moderate effort. Ship first; it also
   speeds the reconstructed-normal path and could later help SSR's depth march.
2. **Phase 2 (fp16)** — storage lands with Phase 1 (portable); fp16 math rides after, Vulkan-gated.
3. **Phase 3 (compute + shared memory)** — the real GPU win, but gated on Phase 1 (apron) and a
   new RHI compute path; Vulkan-only, highest effort. Do only if High+ SSAO is still a measured
   hotspot after 1–2.

**Verification for each:** `r_vkGpuTime` A/B (expect the AO-pass ms to drop), a flag-on/off frame
image-diff to prove the AO is unchanged, and a user look. Keep each phase behind a temporary
`r_ssaoDepthMip` / `r_ssaoFp16` / `r_ssaoCompute` dev flag during bring-up, then fold into the
default path once verified (mirroring the `r_ssaoMergeNormal` bring-up discipline).

**Reality check:** on the *heavy* tiers the bigger frame-time levers are shadow maps, SSR, and
the HDR/post stack (ssao-gtao.md / the session's frame-budget analysis). These SSAO optimizations
are worth doing because the AO shader is cache-bound and the wins are clean, but profile the
whole frame first if the goal is maximum fps rather than specifically cheaper SSAO.
