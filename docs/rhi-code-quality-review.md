# RHI & Vulkan backend — code-quality review

**Date:** 2026-10-01
**Branch:** `review/rhi-vulkan-code-quality`
**Scope:** `neo/renderer/rhi/` — the author-written RHI and Vulkan backend files only (third-party `fsr2/`, `imgui/`, `vma/` excluded).
**Lens:** pure coding quality — coherence, leanness, DRY, efficiency, cyclomatic complexity. **Not** a correctness/bug review.
**Method:** read-only static analysis, one agent per file. Nothing built or run. Line numbers are as-reported by static reading and are approximate — verify before editing.

| File | Lines | Health |
|---|---|---|
| `vk/VulkanBackend.cpp` | 12,303 | correct-looking, superbly commented, but a monolith with hand-rolled VK boilerplate and a few god-functions |
| `RhiWorld.cpp` | 9,620 | careful and well-commented, but systematically copy-pasted feature passes; several 250–620-line functions |
| `RhiBackend.cpp` | 3,881 | works, but ~1/3 is fullscreen-pass plumbing pasted 7×; two giant stage functions |
| `RHI.h` (+ RenderParams/MaterialIR/etc.) | ~1,100 | disciplined on GPU layout & const-ref, but a ~90-method god-interface; MaterialIR has one hot spot |

**Bottom line:** the code is functionally careful and the comments carry real institutional memory. The weaknesses are structural, not statement-level: there is almost no dead code. The duplication is *faithful* (built incrementally behind opt-in cvars), which is exactly why **extraction, not rewrite** is the right lever everywhere below.

---

## Cross-cutting themes (these span multiple files — highest leverage)

### A. The GL3-vs-Vulkan split leaks through the "backend-neutral" layer — **systemic coherence issue**
The RHI layer is supposed to hide the backend, but the backend shows through as raw pokes and inline checks:
- Inline `GetActiveBackendType()==BT_VULKAN` recomputed: **~19× in RhiBackend.cpp, ~20× in RhiWorld.cpp**, sometimes twice per function under two names (`vkMode` then `vkBackend`).
- Direct GL cache pokes `gl3ActiveTexture(...)` + `backEnd.glState.currenttmu=…`: **12× in RhiBackend, 18× in RhiWorld.**
- Manual tmu-cache invalidation `tmu[].current2DMap=-1`: **27× in RhiWorld** — these exist *only* because the GL raw-bind path bypasses the tmu cache.

**Fix:** (1) cache `vkMode` once per function; (2) fold the GL raw-bind + tmu-invalidate into `RB_RHI_BindRTUnit/Image` so no caller ever repeats it. This single move deletes the whole `current2DMap=-1` class and ~6 hand-rolled G-buffer bind branches. The comments already flag this as acknowledged Phase-4 impurity.

### B. Hand-rolled Vulkan struct boilerplate with no helpers (VulkanBackend.cpp) — **biggest line-count win**
- **Image barriers:** 29 open-coded `VkImageMemoryBarrier` + ~40 `vkCmdPipelineBarrier`, 8–13 lines each, **zero helper**. → one `CmdImageBarrier(cb,image,aspect,oldL,newL,srcStage,dstStage,srcAccess,dstAccess,mip,mips,layer,layers)` deletes several hundred lines.
- **Image-handle validity idiom** `imageTable[h-1].live`: ~22–25× (densest in `BindForDraw`). A `LookupBuffer()` helper exists at :443 but no image equivalent. → `ResolveLiveImage(ImageHandle)`.
- **Texture descriptor set build+write+bind:** duplicated 3× (`RecordDispatch` 4514, `BindForDraw` 11607, `DrawImmediate` 11848). → `ResolveTextureSet(args)`; immediate path passes all-dummy args.
- **AS-build boilerplate** (`VkAccelerationStructureBuildGeometryInfoKHR`+sizes): ~9–14× across the RT methods. → `FillTlasBuildInfo()`/`FillBlasBuildInfo()`.
- **Buffer creation** (`VkBufferCreateInfo`+`VmaAllocationCreateInfo`+`vmaCreateBuffer`): 12× same recipe. → `CreateHostMappedBuffer()`/`CreateDeviceBuffer()`.
- **AS write→read `VkMemoryBarrier`:** verbatim at 2880, 5135, 5363, 5414, 5607, 6007. → `CmdAsBuildBarrier(cb)`.

### C. Feature passes were cloned from a template instead of factored (RhiWorld + RhiBackend)
The entire SSAO / SSR / RTAO / RTGI / RT-reflection / volumetrics family was built by copy-paste. The file's own comments admit it (103 "mirrors / identical / same recipe" phrases in RhiWorld).
- **`Ensure*Target` / `Ensure*History`: 8 near-identical functions** (RhiWorld 4713–4801, 5231–5388). Same lost-context→size-match→destroy→create→validate template. → `EnsureRT(handle,&w,&h,fmt,levels)` + `EnsureHistoryPair(...)`. **~250 lines → ~40.**
- **Temporal-accumulation block: 5×** (SSAO 6717, SSR 5877, RT-refl 6148, + RTAO/RTGI). → `RB_RHI_TemporalResolve(cfg)`.
- **Fullscreen-quad post pass: 7×** in RhiBackend (SmaaDraw 267, AAPass 512, PostProcess 600, GammaBrightness 706, HdrFxaa 1010, HdrResolveSmaaFused 1146, HdrResolve 1285) — identical `quad[4]`/`idx[6]` + Alloc + `PipelineDesc{DEPTHFUNC_ALWAYS|DEPTHMASK,…}` + DrawArgs fill. `RB_RHI_SmaaDraw` already *is* this helper; generalize it. **~200 lines removed + per-frame quad allocs gone.**
- **Mip-pyramid build: 3×** (SSAO depth-mip 6625, SSR Hi-Z 5744, SSR color-down 5950). → `RB_RHI_BuildMipChain(rt,levels,shader,parms,mode)`.
- **RTAO & RTGI passes are near-twins** (6797–7052 vs 7067–7326). → one parametrized `RB_RHI_RtGatherPass`.
- **DrawArgs fill block: 11×** in RhiBackend. → `RB_RHI_FillDraw(...)`.
- **TLAS/geo device-address bit-cast into `rtParms`: ~8×** in RhiWorld. → `RB_RHI_PackRtAddr(parms.rtParms, tlas, geo)`.

### D. Per-frame allocations / churn in hot paths — **real efficiency wins**
- **VulkanBackend RT/denoise:** `UpdateTlas` allocates `std::vector` every call (5754); `BuildTlasInto` allocates per call (5503); `NrdRecordDispatches` allocates `writes`+`imageInfos` **per dispatch per frame** (6744), with a fragile `pImageInfo=&imageInfos.back()` dependency on a prior `reserve`. → reusable member scratch vectors, `clear()` not free.
- **RhiBackend particle path:** `RB_RHI_ParticleLooksLikeSmoke` copies `mat->GetName()` + up to 12 substring `Find`s **per smoke particle per frame** (2652). → classify once on the material IR.
- **RhiWorld DrawInteraction** (hottest fn, per-interaction-per-light) reads `r_pbr`, `r_skinShading`, `r_contactShadows`, `r_occlusionMaps`, `r_parallax`, `r_specularScale/Exp` etc. **every draw**. → hoist light-invariant cvars into `ictx` once per light in DrawWorld.
- Per-surface string compares every frame: `Icmp(...,"textures/decals/berserk")` (RhiBackend 3043), `bloodorb` tests (3078). → flag on material/IR at build time.

### E. God-functions (all impl files) — **cyclomatic complexity**
Behaviour-preserving decomposition targets, worst first:

| Function | File:lines | ~len | Why |
|---|---|---|---|
| `ScreenSpaceReflections` | RhiWorld 5635 | 623 | 3 stacked sub-pipelines (march+Hi-Z / glossy / RT+temporal+composite) |
| `DrawInteraction` | RhiWorld 1445 | 565 | one giant per-draw fill: vertexColor/clay/PBR/wet-film/SSS + 4-way shadow ladder + AO/occlusion/parallax |
| `DrawWorld` | RhiWorld 8767 | 513 | per-light technique ladder (rtAll→sun→2D→cube→stencil) 3–4 deep + ~25 debug counters |
| `RenderShaderPasses` | RhiBackend 2967 | 460 | berserk + helltime intercepts + 4-kind per-stage loop interleaving 4+ features |
| `NormalPrepass` | RhiWorld 4316 | 397 | want-flags matrix + merge-vs-standalone + shared surface loop |
| `RenderTexgenStage` | RhiBackend 2275 | 360 | `TG_REFLECT_CUBE` case alone ~170 lines juggling 4 bool flags |
| `CreateM2Resources` | VulkanBackend 3279 | 361 | one function doing ~8 init jobs |
| `FillDepthBuffer` | RhiWorld 2276 | 351 | surface loop + zfill-batch reorder-by-scissor |
| `GetPipeline` | VulkanBackend 10793 | 308 | 64-bit key + topology switch + 2-level stencil switch + blend lambda + vertex-layout chain |
| `RunFsr2` | VulkanBackend 7824 | 306 | long barrier walls + big dispatch-arg fill |
| `BeginFrame` | VulkanBackend 2643 | 298 | 7 self-test toggles + 6 drains + full state reset |
| `BindForDraw` | VulkanBackend 11509 | 233 | **worst branch nest:** 17-way unit if-else, each with nested validity checks |
| `DrawView` | RhiBackend 3463 | 247 | FSR2 dispatch 3-deep + opaque-split logic duplicated |

**Recommended first decompositions** (lowest risk, highest readability gain):
- `BindForDraw` → table-driven `{binding, ImageHandle DrawArgs::*, fallback}` array + `ResolveLiveImage` (also kills theme B item 2). Cuts ~120 lines and the file's worst nest.
- `ScreenSpaceReflections` → `SsrMarchChain()` / `SsrComposite()` / `RtReflPass()` (the RT path 6054–6236 is already near-standalone).
- `DrawInteraction` → `FillShadowParms()` (the 1616–1754 ladder) / `FillPbrAndFilm()` / `FillAoOcclusionParallax()`.
- `RenderShaderPasses` → `BerserkIntercept()` / `HelltimeIntercept()` + `DrawGenericStage()`; the 4 `if(si.kind==…)` become a switch.
- `GetPipeline` → `BuildVertexInput()` / `BuildStencilState()` / `MapTopology()`.
- `BeginFrame` → `MaybeRunOnToggle(cvar, memfn)` for the 7 tests + `DrainAllRetired(slot)`.

### F. RHI.h is a god-interface
- **~90 virtuals** in one abstract class spanning frames, resources, RT, compute, FSR2, NRD, readback, debug draw (RHI.h 214–709). → split into capability sub-interfaces (`IRtBackend`, `IComputeBackend`) behind `Supports*()` getters.
- **17 ray-query/AS methods** embedded in the core interface, all no-op on GL3 (528–660) → GL3 carries 17 empty vtable slots. Extract behind a `SupportsRayQuery()`-gated sub-object.
- **8 `CreateRenderTarget*` factory variants** (348–384) — factory explosion; each new target type adds a method. → one `CreateRenderTarget(const RenderTargetDesc&)`, mirroring the existing `PipelineDesc` pattern.
- **All handles are `typedef unsigned int`** (23–28) — compiler can't catch an `ImageHandle` passed where a `BufferHandle` is wanted. → `enum class : unsigned` per handle, zero-cost type safety.
- `DrawArgs` hardwires 9 named texture slots to fixed units 8–16 (116–132); `GetRenderTargetImage{,2,3,4}` positional accessors (419–426) don't scale.

---

## Leanness / dead scaffolding (per the project's "trim debug cvars once confirmed" policy)
- **VulkanBackend:** ~700 lines of `*SelfTest` scaffolding (`ComputeSelfTest` 4642, `BdaSelfTest` 4732, `RayQuerySelfTest` 6095, `NrdSelfTest` 7188 ~305 lines, `Fsr2SelfTest` 7493, `Mrt3SelfTest` 8134) compiled into every build, reachable only via `r_*Test` cvar edges. → gate behind a `DUDE_VK_SELFTESTS` compile flag, or remove the ones whose features shipped. 14 diagnostic cvars (97–143) are retirement candidates.
- **VulkanBackend:** `-Wunused-parameter`/`-Wunused-variable` globally suppressed for the whole TU (50–51) — hides genuinely unused locals rather than fixing them.
- **RhiWorld:** ~207 distinct `r_*` cvars read in this one file; many are bring-up/debug (`r_ssaoTemporalTrade`, `r_rtaoDebug`, `r_rtgiDebug`, `r_mvDebug`, `r_shadowMapCacheDebug/Debug`). `r_shadowMapPointRangeScale` (3268) is self-labelled "TEMP live-tuning knob" yet ships as a live cvar.
- **RhiBackend:** unused param `const float *regs` in `RB_RHI_BindStageImage` (1767) — drop it. `RB_RHI_LogOnce` (1377) matches via substring `Find`, so a short tag that prefixes a longer one suppresses the longer message, and the `logged` list grows unbounded.
- **RHI.h:** dead comment block at 463–465 referencing a declaration that isn't there.
- **MaterialIR.cpp:** `IR_Build` (156–281, ~125 lines) is the one hot spot — stage iteration + 3-attempt ARB resolution + currentRender detection + 6-case texgen switch; extract `BuildNewStage()` + `BuildTexgenStage()`. The builtin-load block is pasted verbatim at 192–200 and 207–217.

## Positives worth keeping
- Large structs are passed by const-ref throughout the interfaces; arrays by pointer — no value-copy issues found.
- GPU-layout discipline: std140 structs guarded by size `static_assert`s; `GatherSceneLights` frame-cached with `_alloca`/static scratch (no heap churn).
- Comment density is high and genuinely useful; error handling (LoadShader/null-target → early return) is uniform.
- No commented-out dead-code blocks; scratch `idList`s reuse capacity across frames.

---

## Suggested action plan (risk-ranked)

**Tier 1 — mechanical, behaviour-preserving, high leverage (do first):**
1. `CmdImageBarrier()` helper → route all 29 VK barriers (theme B).
2. `ResolveLiveImage()` + table-driven `BindForDraw` (themes B, E).
3. Generalize `RB_RHI_SmaaDraw` → one fullscreen-pass helper + static quad VB/IB (theme C, ~200 lines + allocs).
4. Collapse the 8 `Ensure*Target/History` → 2 helpers (theme C, ~210 lines).
5. Fold GL raw-bind + tmu-invalidate into `BindRTUnit/Image`; cache `vkMode` once per function (theme A).
6. Trivia: drop `regs` param; delete RHI.h:463 dead comment.

**Tier 2 — medium effort, clear win:**
7. Reuse member scratch vectors in `UpdateTlas`/`BuildTlasInto`/`NrdRecordDispatches` (theme D — genuine perf, low risk).
8. `RB_RHI_TemporalResolve`, `BuildMipChain`, `RtGatherPass`, `FillDraw`, `PackRtAddr` extractions (theme C).
9. Decompose the god-functions listed in theme E.
10. Hoist per-draw cvar reads into per-light `ictx` (theme D).

**Tier 3 — structural, needs a design decision:**
11. RHI.h: capability sub-interfaces, `RenderTargetDesc`, typed handles (theme F) — touches GL3 + VK + all call sites.
12. Split the 12K-line `VulkanBackend.cpp` into a header + themed TUs (swapchain / RT-AS / NRD / FSR2 / targets / draw).
13. Gate/retire the ~700 lines of self-tests + shipped-feature diagnostic cvars.
14. RenderParams.h: core + optional feature UBO blocks to cut per-draw bandwidth (large).

**Constraint:** this is perf-critical, behaviourally-sensitive render code. Every item above is extraction, not rewrite — but each still needs a build + visual verification (I can't run the game), and Tier 3 items (11, 12) carry real churn across backends.
