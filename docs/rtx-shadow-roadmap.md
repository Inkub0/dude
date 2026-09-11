# RTX shadow roadmap — the GPU-offload path to a fully GPU-resident shadow system

Synthesis of [gpu-offload-plan.md](gpu-offload-plan.md) + [shadow-research.md](shadow-research.md)
+ the shipped shadow work, answering one question: **what is still missing between today's renderer
and a well-performing RTX shadow system**, and in what order to build it.

## What we already hold (the assets)

| Asset | Status | Why it matters for RTX shadows |
|---|---|---|
| VK compute lane (`CreateComputeShader`/`Dispatch`/`BU_STORAGE`) | shipped, Phase 1 | AS builds, culling, classification passes |
| GPU skinning + **deform-once compute buffer** (`r_tessDeform`) | shipped, user-verified | **exactly the per-frame GPU-resident deformed VB a BLAS refit consumes** — the expensive half of RT-for-animated-geometry is already done |
| Indirect-draw primitive (`DrawIndexedIndirect`, `r_vkIndirectTest`) | shipped, pixel-identical | GPU-driven raster passes (Track 2) |
| GPU frustum-cull compute (`r_gpuCullTest` / `r_gpuCullLive`) | shipped, PASS on live data | TLAS instance culling; Track 2 caster culling |
| `COMPUTE→DRAW_INDIRECT` barrier site + shared frame cb | mapped (VulkanBackend.cpp:~3273) | one-line widen when consumed |
| Complete shadow-*map* system (sun maps, cube fallback, stencil last-resort, split caches, budget, Vogel PCF, normal-offset bias) | shipped | the noise-free base tier every hybrid keeps |
| Runtime shaderc GLSL→SPIR-V | shipped | compiles `GL_EXT_ray_query` shaders with no build-system change |
| VK 1.4 floor, `r_vkGpuTime` profiler | shipped | feature floor + measurement |

CPU work that still exists per frame for shadows: front-end caster-list building (interaction walk),
one draw submission per caster surface per face, plane localization, cache bookkeeping. Two tracks
can remove it — and they are **not** both needed.

---

## Track 1 — the RTX line (build this)

### R1. Temporal pipeline: motion vectors + jitter + FSR2 — the keystone · LARGE · anytime
Grown from "per-object motion vectors" into the full temporal pipeline, superseding the old
hand-rolled-TAA plan (docs/antialiasing.md):
- **Motion vectors** (prev-frame MVP per surface → velocity target during zfill; camera-derived for
  static world) — both backends benefit (SSAO temporal), and they feed everything temporal below.
- **Sub-pixel projection jitter** + the pipeline reorder: scene renders at render-res into the HDR
  buffer (already the pre-tonemap RGBA16F input FSR2 wants) → FSR2 → grain/chroma/dither/HUD at
  display res (the resolve currently folds grain in early — bounded reordering work).
- **FSR2 in Native-AA mode first** (render scale 1.0): a production-grade TAA that resolves the
  specular/normal-map shimmer SMAA structurally can't — then the **render-scale knob for free**
  (67% scale ≈ half the per-pixel ray budget when the RT tiers land; today it's banked headroom,
  since the 3080 Ti frame is CPU-front-end-bound).
- **The real integration work is reactive masks**: Doom 3 is drenched in additive particles/muzzle
  flashes, the classic temporal-upscaler ghosting case. Budget most of the effort there, not in the
  API hookup.
- **Hardware/licensing:** FSR2 is MIT-licensed pure compute — no vendor blob, no tensor cores; it
  runs on ANY GPU our Vulkan backend already supports (needs compute → **VK-only**; GL3.3 keeps
  SMAA/FXAA; the legacy backend stays byte-faithful). Gated behind a cvar + preset tier like every
  enhancement, it restricts no hardware: off = today's renderer, bit for bit. Keep the inputs
  (MVs, depth, jitter, exposure) vendor-neutral so a DLSS path could slot in later without
  committing the GPL repo to a proprietary blob.
- Also unlocks temporal RT-shadow denoising (SIGMA temporal / A-SVGF) for R5, and makes IGN-seeded
  noise usable again (the moiré lesson inverts under temporal accumulation).
- **Frame generation (FSR3.1 FG) is deliberately parked**: `com_interpolate` already renders REAL
  frames above the 60 Hz sim — better than interpolated ones on both quality and latency. FG only
  pays if the RT endgame becomes GPU-bound below display rate; revisit then, mindful of input
  latency and HUD compositing.
(Until R1 lands, the RT soft tier runs spatial-only with mild flicker — acceptable, not final.)

### R2. Ray-query foundation · LARGE · VK-only, RT-gated  **[DONE — shipped + user-verified 2026-08-18]**

**As-built** (branch feat/rt-ray-query-foundation): extension trio gated behind one
`haveRayQuery` capability; RHI AS API (`CreateBlas`/`BuildTlas`/`UpdateTlas`/`DestroyRtScene`);
`r_rayQueryTest` synthetic validator + `r_rtWorldTest` world validator (GPU rays diffed vs a CPU
Möller–Trumbore reference, entity instances included); persistent `r_rtWorld` scene = one BLAS
per `_areaN` + one per unique static entity model with per-entity TLAS instances (movers
re-instanced EVERY FRAME via the per-frame TLAS lane: slot per frame-in-flight, build recorded
ahead of the frame's draws, verified with a kicked moveable casting a live moving shadow).
~60-90 ms full-map builds; rays measured ~free on the 3080 Ti @1440p. Remaining from the
original spec: BLAS compaction, animated-monster refit (below), TLAS instance culling.
The minimum stack is `VK_KHR_acceleration_structure` + `VK_KHR_ray_query` only — **skip the
RT-pipeline/SBT extension entirely** (inline queries from the existing forward interaction shader;
consensus recommendation for shadow-only workloads).
- **RHI additions:** AS build/refit API (compute-lane sibling), device-feature gating (no fallback
  in-spec: pre-Turing/pre-RDNA2 simply lack the extensions → capability cvar + preset gate).
- **Static world:** grouped BLAS (Youngblood measured a 60% BLAS-count win from grouping),
  `PREFER_FAST_TRACE` + compaction (~52% memory saved; ~25–30 B/tri on NVIDIA — a Doom 3 map is
  small by these standards).
- **Animated monsters:** per-frame BLAS **refit** (`ALLOW_UPDATE`, topology fixed) reading the
  deform-once buffer; refit ≈5–7× cheaper than rebuild, sub-ms at D3 scale; periodic/key-pose
  rebuild counters tree degradation; NVIDIA budget guide ≤ ~2 ms AS work/frame; async-compute later.
- **TLAS:** rebuilt per frame (cheap); instances culled by extended-frustum/size — reuse the GPU-cull
  machinery.
- **Validator first** (house pattern): `r_rayQueryTest` — trace a synthetic ray set, diff against a
  CPU reference before anything renders from it.

### R3. RT hard shadows — first visible RTX feature · MED · depends R2  **[SUN SHADOWS SHIPPED + user-verified 2026-08-18]**

**As-built:** `r_rtSunShadows` (opt-in, VK+RT): interaction shadow mode 4 traces one ray at the
sun (opaque + terminate-on-first-hit) in a separate `interaction_rt` program (the SPIR-V
ray-query capability fails pipeline creation on non-RT hardware, so the base shader stays
clean). Serves BOTH sun species: parallel suns AND the low oversize omnis the virtual-map fit
declines (`lightRtOnly` route — those lights render no sun map at all). TLAS address rides the
RenderParams UBO bit-cast (floatBitsToUint). The sun-map render is now skipped for EVERY
RT-served sun (both parallel and fit-declined), so RT suns render no map at all — pure ray path.

**Animated monster casters DONE + user-verified 2026-08-18** (`r_rtMonsterShadows`, default on):
each frame the view-entity chain's DM_CACHED characters are gathered (their always-posed
CPU-skinned verts baked to world by `vEnt->modelMatrix`) into one combined world-space soup →
a per-frame dynamic BLAS rebuilt on the frame cb ahead of the TLAS build, appended as one
identity instance. Key lessons: (1) PERFORATED (alpha-tested) surfaces must cast — D3 characters
are ~90% perforated and D3's own stencil treats them as solid occluders (R_RtSurfCasts admits
OPAQUE||PERFORATED); (2) walk `viewEntitys` keyed on `dynamicModel != NULL`, NOT
`dynamicModelFrameCount == tr.frameCount` — a held/idle pose isn't re-instantiated, which
flickered. Remaining: cube/point lights; alpha-tested any-hit (holes in shadows) refinement.
Inline query in the interaction path (shadow mode 4): **one ray toward the light**,
`TerminateOnFirstHit | Opaque | SkipClosestHit`; alpha-tested casters marked non-opaque, resolved in
the candidate loop (`rayQueryConfirmIntersectionEXT`). Deterministic, noise-free — **no denoiser, no
motion vectors needed.**
- **Roll out per light species, sun first**: RT deletes the sun map's two structural limits at once
  (the `r_shadowMapSunRange` reach cap and map-resolution pixelation) — pixel-exact shadows at any
  distance. Then cube lights (deletes their 6-face render + cache + budget for RT-served lights).
- RTG ch.13 measured RT *hard* shadows **beating** shadow-map rendering by 40–60% at 4+ lights
  (2080 Ti); the 3080 Ti does better. Single-light scenes favor maps ~2× — hence hybrid (R4).
- This is where the reserved **"Ultra Nightmare"** preset tier finally means something: RT-gated,
  VK-only, opt-in per the fidelity policy.
  > **SHIPPED 2026-08-20** — `PRESET_ULTRA_NIGHTMARE` (index 6) = Nightmare + `r_rtSunShadows` +
  > `r_rtMovingLights`. First real content for the reserved name. Degrades to == Nightmare on
  > GL3/non-RT (the RT cvars gate on `SupportsRayQuery`). Wired in both the ImGui combo and the
  > classic `mainmenu.gui`. Alongside it, **TLAS dirty-flag** (`r_rtTlasDirty`) removes the
  > ~0.2 ms/frame per-frame TLAS rebuild when nothing moves — RT is now ~free when idle, which
  > is what makes leaving the tier's RT features on affordable.

### R4. Hybrid scheduling · MED · depends R3
The AMD FidelityFX Hybrid Shadows model, adapted: keep shadow maps as the noise-free base where they
are cheap (single/small lights, cached static cubes), trace where they are weak or wrong —
classifier rejects fully-lit and fully-shadowed pixels from the map, **ray-interval reduction**
shortens rays to the ambiguous band, budget-capped. The shadow-map system we just finished is not
throwaway: it is the hybrid's raster tier and the non-RT fallback, permanently.

### R5. RT soft shadows — the endgame · MED–LARGE · depends R3 (+R1 for final quality)
1 ray/pixel with **STBN** (spatiotemporal blue-noise) light sampling — free, a texture lookup — +
**analytic penumbra from occluder hit-distance** (PCSS geometry, no cone sampling) + **NRD SIGMA**'s
penumbra-guided spatial denoise (~0.4 ms @1440p on a 4080; ~0.5–0.7 ms est. on the 3080 Ti). Runs
without motion vectors (mild flicker); R1 upgrades it to SIGMA's temporal stabilization.
Explicitly ruled out: ReSTIR (crossover ~50+ overlapping shadowed lights — a genre change).

**End state:** stencil exists only on Potato/Low (faithful floor); shadow maps serve non-RT hardware
+ the hybrid's cheap tier; RT-capable cards run hard-RT (R3) → hybrid (R4) → soft (R5) shadows with
near-zero CPU shadow work — caster lists, caches, and per-face passes all deleted for RT-served
lights; the CPU's only residual job is AS orchestration.

---

## Track 2 — the raster line (build only what RT doesn't obsolete)

### T1. Unified geometry buffer + GPU-driven caster pass · LARGE · VK-only
Our validated cull + indirect primitives applied to the shadow caster pass: persistent per-light
caster table → GPU cull per face/frustum (`cs_gpucull` reuse) → compacted
`VkDrawIndexedIndirectCommand[]` → one indirect draw per face. Needs the **unified geometry buffer**
(the Phase-3.2b blocker) plus a `shadow_sm` variant taking WORLD planes + a per-instance transform
SSBO (today's per-surface CPU plane localization can't feed a multi-draw).
- **The shadow pass is the ideal pilot** for the unified buffer: one pipeline, depth-only, no
  material textures for opaque casters — far simpler than the zfill case.
- **BUT: ray query deletes this pass for RT-served lights.** Build T1 only if (a) non-RT hardware
  perf becomes a priority, or (b) the unified buffer is being built anyway for main-view GPU-driven
  drawing (Phase 3.2b) — then the caster pass is the cheap first customer, not a goal in itself.

### T2. Cube update scheduling (Nth-frame refresh, screen-size update frequency) · LOW–MED
Frame-pacing smoothing for the raster tier; survives into the hybrid as the map tier's scheduler.
Worth doing opportunistically; not on the RTX critical path.

---

## Sequencing

```
R1 temporal pipeline (MVs + jitter + FSR2) ──┐  (parallel track — TAA-class AA now,
                                             ▼   render-scale headroom for the RT tiers)
R2 ray-query foundation ──► R3 RT hard shadows ──► R4 hybrid ──► R5 soft (STBN+SIGMA)
      (AS API, BLAS/TLAS,        (sun first,           (maps stay     (temporal once
       deform-once refit,         then cubes;           as base        R1 lands)
       r_rayQueryTest)            "Ultra Nightmare")    tier)

FSR3.1 frame generation — PARKED behind com_interpolate; revisit only if R5-era GPU-bound
T1 unified-buffer caster pass — DEFERRED: only as Phase-3.2b pilot or for non-RT perf
T2 cube scheduling — opportunistic, low priority
```

**Recommended next big steps: R2 or R1, either order.** R2 is the item everything RTX hangs off
(deform-once removes its hardest prerequisite; validator-first lets it land without touching a
pixel). R1 now carries immediate user-visible value of its own (FSR2 Native-AA beats SMAA on the
shimmer Doom 3 actually suffers from) and pre-pays the render-scale headroom the RT tiers will
spend — a legitimate first pick if AA quality is the itch.
