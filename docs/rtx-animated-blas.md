# RTX animated BLAS — feed the acceleration structure from `gpuSkinVB` (refit, not rebuild)

The payoff of GPU skinning being solid + default-on. Today RT monster casters
(`r_rtMonsterShadows`) round-trip through the CPU; this replaces that with the
GPU-resident skinned geometry we already produce, and makes animated geometry a
first-class citizen of the ray-traced world — the foundation every RT-on-dynamic
feature (reflections, GI/AO, soft shadows on monsters) stands on.

Sits under [rtx-shadow-roadmap.md](rtx-shadow-roadmap.md) as the "deform-once refit"
lane named in R2/R3 sequencing. Depends on [[gpu-skin-seam-weld]] work (gpuSkinVB).

## Where we are (the thing to replace)

`RHI::UpdateDynamicGeometry(worldPositions, numVerts, indexes, numIndexes)`
(RHI.h:476, impl in VulkanBackend + gather in tr_main.cpp ~1159–1400):

1. **CPU** walks the visible view-entity chain, takes each character's **CPU-skinned**
   `tri->verts` (always posed — the CPU position skin never strips), and bakes them to
   **world space** with `vEnt->modelMatrix` into one packed float3/int **soup**.
2. Backend **rebuilds ONE combined dynamic BLAS every frame** from that CPU array
   (full build, not refit) and appends **one identity instance** to the per-frame TLAS.

Costs: the per-frame CPU gather + world-bake, the CPU→GPU upload of the soup, and a
full BLAS rebuild every frame. And it is **shadow-only** — a throwaway soup, not real
scene geometry other RT passes can reuse.

`CreateBlas` (VulkanBackend.cpp:4376) only accepts **CPU** `positions`/`indexes`
(staged internally, freed after a synchronous build). There is **no** GPU-buffer BLAS
input and **no** refit path yet.

## Target design

**Per-entity, model-space, multi-geometry BLAS built from `gpuSkinVB`, refit per pose change,
instanced into the per-frame TLAS with the entity's `modelMatrix`.**

- **Model space, not world.** `gpuSkinVB` is model-space posed verts (the compute skin is
  `jointMat * weight` in model space; the draw applies the model matrix in its MVP). So the
  BLAS is built in model space and the **TLAS instance transform = `modelMatrix`** (3×4). This
  is the textbook layout and it splits the two kinds of motion:
  - **moving but same pose** (walking in a straight line, idle drift) → only the instance
    transform changes → **no BLAS work at all**, just the TLAS re-instance (already cheap).
  - **pose changed** (animating) → **refit** the BLAS (vertices moved, topology fixed).
- **One BLAS per entity, one geometry per surface.** A monster is ~6 `srfTriangles`, each with
  its own `gpuSkinVB` + static index buffer. A BLAS takes N geometries, so one BLAS per monster
  (N surface-geometries) → one TLAS instance per monster. Vertex data = each surface's
  `gpuSkinVB` device address (stride `sizeof(idDrawVert)`=60, xyz@0); index data = each
  surface's static index-buffer device address (the batched-zfill path already fetches indices
  by BDA, so this address exists).
- **Refit (`VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE`)** with the BLAS built once using
  `ALLOW_UPDATE`. Refit is far cheaper than rebuild and is the whole point — skinning only moves
  vertices. Rebuild only when topology changes (model reload / skin swap / LOD → surface vertex
  count changes).
- **All on the frame command buffer**, ordered by barriers (below) — no `vkQueueWaitIdle`, unlike
  the synchronous `CreateBlas`.

## Prerequisites (small, mechanical)

- **`BU_SKIN` buffer usage:** add `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` (currently gated to
  STORAGE/VERTEX/INDEX only, VulkanBackend.cpp:3469) **and**
  `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR`, gated on
  `SupportsRayQuery()` (so non-RT builds don't request the AS usage). Same for the static index
  buffers feeding the geometry.
- **Barrier — the one my batched skin barrier deliberately omits:**
  `COMPUTE_SHADER (SHADER_WRITE)` → `ACCELERATION_STRUCTURE_BUILD_KHR
  (ACCELERATION_STRUCTURE_READ)` so the AS build sees the frame's skinned verts. Extend
  `PostComputeBarrier()` to add the AS-build dst stage **only when `SupportsRayQuery()`** (the
  stage/access flags are illegal without the extension enabled). Then the existing AS-build →
  ray-query (fragment) barrier carries it into the shadow trace.
- **Scratch buffer** for refit (a persistent per-BLAS or shared scratch sized to the largest
  `updateScratchSize`); refit scratch is smaller than build scratch.

## Frame ordering (resolved during S2→S3) — refit AFTER the skin flush, not at BeginFrame

The apparent tension between S4 ("wire into `UpdateTlas`", a frontend/between-frames call) and the design
("skin → AS build/refit → TLAS → scene") resolves as follows. The backend frame is:

```
BeginFrame()            ← today records the per-frame dyn-BLAS + TLAS build here
RB_RHI_FlushSkinJobs()  ← skin compute writes gpuSkinVB (THIS frame's pose) + PostComputeBarrier
RB_RHI_FlushTessJobs()
… RC_DRAW_VIEW …        ← interactions issue the shadow rays that read the TLAS
```

So `gpuSkinVB` only holds this frame's pose *after* `FlushSkinJobs`. A refit at `BeginFrame` (where the
current CPU-soup dyn-BLAS builds) would read **last** frame's pose → a one-frame shadow lag on animated
monsters (the shipping CPU-soup path has none, because the frontend bakes current-pose world verts). To
stay lag-free, the animated **build/refit and the frame TLAS build move to a backend hook right after
`FlushSkinJobs`/`FlushTessJobs`** (`RefreshAnimBlas`). The CPU-soup dyn-BLAS (fallback) can keep building
at `BeginFrame` — it doesn't depend on the GPU skin — as long as the TLAS that references it is built in
the post-skin hook. The frontend's role shrinks to *staging* per-entity descriptors (`UpdateAnimCasters`,
between frames); the backend owns the cache + all AS recording, on the frame cb, no `vkQueueWaitIdle`.

Consequence for first-sight builds: they must also record on the frame cb (after this frame's skin), not
via the synchronous `CreateBlasFromBuffers` — a synchronous build mid-frame would `vkQueueWaitIdle` on
*previously submitted* work and read stale/garbage `gpuSkinVB` (this frame's skin is recorded but not yet
executed). So `RefreshAnimBlas` records both BUILD (first sight / topology change) and UPDATE (refit) on
the frame cb, with one persistent per-BLAS scratch sized to the (larger) build scratch.

## Staged plan (validator-first, the way R2/R3 landed)

- **S0 — prereqs (no behavior change):** `BU_SKIN`/index usage flags + the RT-gated
  `PostComputeBarrier` AS-build stage. Nothing consumes it yet. Verify: existing scenes byte-
  identical, no validation errors, non-RT + GL3 unaffected.
- **S1 — RHI API:** `CreateBlasFromBuffers(geoms[], allowUpdate)` (each geom = vtx addr/stride/
  count + idx addr/count) recording on the frame cb, and `RefitBlas(blas, geoms[])`
  (UPDATE mode). Keep the synchronous CPU `CreateBlas` for the static world + as fallback.
- **S2 — validator (`r_rtAnimBlasTest`, once/sec, no visible change):** build one monster's BLAS
  from its `gpuSkinVB` and, against the SAME monster's CPU-soup BLAS, diff a small traced ray
  grid (or compare per-triangle AABBs read back). Proves the GPU-fed build + refit produce the
  same geometry as the shipping path before anything renders from it. Mirrors `r_rayQueryTest` /
  `r_rtWorldTest`.
- **S3 — per-entity BLAS lifecycle:** cache a BLAS per render entity (keyed like `gpuSkinVB` is,
  by the model + surface set); build with `ALLOW_UPDATE` on first sight or topology change; refit
  each frame the pose changed (`gpuSkinFrame` bumped); retire on entity free / model change. This
  is the genuinely new state to get right — see Risks.
- **S4 — wire into the TLAS:** replace the combined world-soup identity instance with one
  model-space instance **per monster** (`transform = modelMatrix`) in `UpdateTlas`. Gate on
  `gpuSkinVB` present; **fall back to the CPU-soup `UpdateDynamicGeometry`** for any caster
  without a `gpuSkinVB` (gpuSkinning off, or a surface that didn't skin on GPU).
- **S5 — retire the redundant CPU path** for GPU-skinned casters: no world-bake, no soup upload,
  no per-frame full rebuild for those. The CPU soup remains solely as the fallback.
- **S6 — verify + measure:** RT sun/monster shadows pixel-equivalent to today; per-frame CPU +
  GPU cost down (refit ≪ rebuild, gather gone); no device-loss across a heavy fight; then the
  payoff features (RT reflections / R5 soft shadows) can consume the now-resident monsters.

## Results (measured 2026-09-14, RTX 3080 Ti)

S0–S4 shipped behind `r_rtAnimBlas` (default 0) and verified end to end:
- **Correctness:** `r_rtAnimBlasTest` PASS on zfat / z7 (static, 0.00000 delta) / cacodemon — build
  AND refit trace-identical to the read-back `gpuSkinVB`. Live A/B (`r_rtAnimBlas 0` vs `1`) under a
  sun light: monster shadows visually identical.
- **Lifecycle at scale:** build-once → refit-every-frame → clean retire. `live` tracks the on-screen
  monster count exactly (no leak); refits = live × fps; builds only on first sight; retires only on
  leave. No VK validation errors / device loss across repeated toggles + a fight.
- **Perf: neutral** — a 4-monster sun-lit scene ran ~18.7 ms GPU with the CPU soup vs ~18.8 ms with
  the animated path: identical within frame-to-frame noise. Expected at Doom 3 scale — the per-frame
  monster AS-build (rebuild *or* refit) is sub-millisecond either way, lost in a GPU-bound ~18 ms
  frame, and the CPU gather/bake/upload saved doesn't show while GPU-bound. **No regression.**

**Takeaway:** the payoff of this phase is architectural, not FPS — monsters are now optional
GPU-resident, model-space, per-entity TLAS instances (the reusable layout RT reflections / GI / R5
soft shadows need), and the CPU-soup throwaway is bypassed for them. Because it is perf-neutral and
device-loss territory, `r_rtAnimBlas` stays **opt-in (default 0)** until a consuming feature needs it
— flip the default then (that is S5; the redundant CPU path for `gpuSkinVB` casters is already skipped
whenever the cvar is on).

## Risks & mitigations (extra care — this is device-loss territory)

- **Sync hazards** (skin compute → AS build → ray read). Mitigation: the explicit RT-gated
  barrier in S0; S2 validator catches a wrong/torn build before it ships; keep everything on one
  queue/cb ordered as skin → AS build/refit → TLAS → scene (ray reads in the interaction pass).
- **BLAS lifecycle leaks / dead handles.** Fence-retire AS destroys (as `DestroyBlas` already
  does), key the per-entity cache so a freed/re-instantiated entity can't refit a stale BLAS,
  and skip TLAS instances referencing dead BLAS (UpdateTlas already does).
- **Refit staleness.** A refit assumes fixed topology; if `gpuSkinVB` was reallocated (vertex
  count changed) we must full-rebuild, not refit — tie the rebuild trigger to the same condition
  that reallocates `gpuSkinVB`.
- **Fallback correctness.** Any caster without a `gpuSkinVB` (gpuSkinning off / non-GPU-skinned
  surface / eye-deform sub-meshes) must still cast via the CPU soup — never silently drop a
  shadow caster. S4 keeps both paths live.
- **Non-RT / GL3 / non-gpuSkin:** every new path gates on `SupportsRayQuery()` **and**
  `gpuSkinVB != 0`; all of it is inert otherwise. Ultra Nightmare (the only RT preset) has
  gpuSkinning on, so the fast path is the one that actually runs there.

## Not in scope (follow-ups this unlocks)
RT reflections over dynamic geometry, RT GI/AO, R5 soft shadows on monsters — all become
possible once monsters are GPU-resident TLAS instances. Alpha-tested any-hit refinement (holes
in monster shadows) is an independent R3 refinement, orthogonal to this.
