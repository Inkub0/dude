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

**Status: BUILT (feat/ssao-depth-mip), pending in-engine verify.** Behind `r_ssaoDepthMip`
(default 1; off = the exact prior full-res raw-depth march, for an A/B) with `r_ssaoDepthMipBias`
(0.5) tuning LOD aggressiveness. As built: a new RHI capability — `CreateRenderTargetMipped` +
`GenerateRenderTargetMips` (GL3: per-level `glTexImage2D` + `glGenerateMipmap`; Vulkan: an
`mipLevels`-deep image with a level-0 attachment view + an all-levels sample view + a
`vkCmdBlitImage` down-chain, barriers mirroring `CopyFramebufferToImage`, with a cross-frame
write-after-read barrier since the target is read every frame and rewritten the next). A new
`ssao_depthmip` fullscreen pass linearizes `_currentDepth` (positive view-space eye depth) into
level 0 at the AO resolution; `GenerateRenderTargetMips` box-averages the chain (6 levels max).
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

**DUDE mapping:**
- **fp16 storage (both backends):** the Phase-1 linear-depth mip as **R16F** (halves its
  bandwidth — the whole point of Phase 1), and the AO output as R8/RG16F. GL3 and Vulkan both
  support 16F textures, so this half is portable and lands with Phase 1.
- **fp16 math (Vulkan-only):** GL 3.3 core has no half-float shader arithmetic (`mediump` is a
  no-op on desktop GL), so explicit fp16 math needs Vulkan +
  `GL_EXT_shader_explicit_arithmetic_types_float16` / `shaderFloat16` (VK_KHR_shader_float16_int8).
  Convert the inner horizon accumulation (`cH_pos/cH_neg`, the visibility integral) to
  `float16_t`; keep position reconstruction fp32 (precision). Gate on device support; fall back
  to fp32 (the GL3 path and non-fp16 GPUs).

**Gain:** 5–20% (modest but nearly free). **Effort:** low (storage) + low-moderate (VK math,
behind a feature check). **Risk:** minor banding from 16-bit depth (XeGTAO ships it as default).

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
