# Vulkan backend audit — action plan

**Date:** 2026-10-03
**Branch:** `review/vk-correctness-lifetime-perf` (off master `7f8db038`, not pushed)
**Lens:** correctness (lifetime, synchronisation, undefined behaviour) first, then real per-frame cost. Not a style or DRY review — that is `docs/rhi-code-quality-review.md`.
**Method:** static reading only. Nothing here has been run in-game. Line numbers are approximate; re-check before editing.

## Scope rule

Only code written by this fork (Inkub0) is in scope: everything under `neo/renderer/rhi/` (all 361 commits there are ours) and the fork-added blocks in the frontend (`R_Rt*`, GPU skinning, the Vulkan image paths).

Out of scope, and not to be edited by any item below:

- original id Software / dhewm3 code (e.g. `R_CheckCvars`, `RB_SetBuffer`, `PlayerView`);
- vendored libraries (`libs/fsr2`, `libs/nrd`, `libs/vma`, `imgui`).

Where a bug is *triggered* by core code, the fix is placed on our side of the boundary.

## Already landed (commit `425aebf4`, builds, not run in-game)

- FSR2 composite target marked written after the upscale copy (one black world frame per composite creation).
- `DestroyMergeNormal` flushes the texture-set cache when it frees its sample slots.
- Swapchain `UNDEFINED → TRANSFER_DST` barrier uses the acquire-semaphore wait stage.

## Classification

- **Definite** — the defect follows from the code as written.
- **Probable** — the mechanism is certain, the trigger needs a specific configuration or timing.
- **Hardening** — no current caller reaches it.
- Perf items carry **Strongly expected** or **Benchmark required**.

---

## Phase 1 — visible or device-lost-class bugs

Do these first, one commit each, validate with `r_vkValidation 1`.

### 1.1 Texture-set cache has no real identity — Definite

`BindForDraw` (~11905) keys the cross-frame descriptor-set cache on a 64-bit hash of the 17 bound image handles and never compares the handles. The mixer is weak for small integers: 360,000 diffuse/specular handle pairs produce only 41,736 distinct keys (recomputed). A collision silently draws a surface with another material's textures until the next cache flush.

- Key the map on the 17 handles themselves (a small struct with `operator==` and a hasher); replace `boundTexKey` with the same struct compared by `memcmp`.
- The handles are already in binding order (units 0–7, then shadowCube, ssao, occlusion, parallax, shadowCubeDyn, shadowMask, decalMask, tessHeight, contactDepth), so the key array can also drive the descriptor writes.
- `DrawImmediate` uses key `0` for its all-dummy set and binds set 1 without resetting `boundTexSet`; give it the all-zero key and reset the bound-set cache there.
- Follow-up (separate commit, pure refactor): replace the 17-way `if` chain in the write loop with "live handle wins, else the binding's typed dummy" (7 → 2D shadow dummy, 8/12 → cube shadow dummy, 15 → grey, else white). This is the table-driven `BindForDraw` the earlier review deferred.
- Validate: any level, compare against GL3 for wrong-texture surfaces; watch for the pool-exhaustion warning.

### 1.2 NRD records twice per frame on portal-sky maps — Probable

RoE / Lost Mission `PlayerView` issues two full-window `RenderScene` calls per frame when a portal sky is active. Both pass the `isWindowView` gate, so `RtaoDenoise` runs twice on the same frame slot. `NrdRecordDispatches` (~6871) resets the slot's descriptor pool and restarts its constant ring at every call: the second call frees sets the first already bound in the recording command buffer and overwrites its constants.

- Reset the slot's NRD pool and ring cursor once per frame (in `BeginFrame`, after the fence wait), not per call; let the cursor continue across calls and fail gracefully when it would overflow.
- Double the ring and pool capacity in `NrdCreate` so two denoises fit.
- `NrdSelfTest` must reset the slot explicitly between its iterations.
- Not addressed here, needs your decision: both views share one NRD history, one FSR2 context and the same previous-frame matrices. Whether the portal-sky view should run temporal passes at all is a renderer-design question.
- Validate: an RoE portal-sky map at an RTX preset with validation on.

### 1.3 Animated-BLAS staging is not one-shot — Definite (mechanism)

`animStaged` is cleared only by the next `UpdateAnimCasters`, which runs only when a world view is rendered. Menu, loading-screen and PDA frames keep refitting last frame's monsters every frame — including after map unload has destroyed the vertex and index buffers their device addresses point to.

- Clear `animStaged` / `animStagedGeoms` at the end of `RefreshAnimBlas` and on its early-out.
- Validate: load a map with monsters in view, quit to menu, load another map, validation on.

### 1.4 Idle-skip TLAS ignores GPU-skinned casters — Probable

`R_RtRefreshInstances` treats only CPU-soup monsters as "dynamic". With RT shadows on but RTAO and RTGI both off, and no mover moving, `UpdateTlas` is skipped: BLASes are refit in place under a pinned TLAS whose instance transforms and bounds are stale, and a retired BLAS can be destroyed while that TLAS still instances it. No stock RTX preset reaches this; the separate menu checkboxes do.

- Have `R_RtStageAnimCasters` record whether it staged any caster and OR that into the dirty decision.
- Validate: `r_rtAllLights 1`, `r_rtao 0`, `r_rtgi 0`, stand still next to a walking monster.

### 1.5 Merged-prepass framebuffer lifetime — Probable

- `BeginNormalPrepass` (~11001) destroys the old framebuffer immediately while the previous frame may still use it. The trigger that is *not* covered by the settings-change idle drain: `r_hdr 0` and a film-grain / chromatic-aberration / gamma slider crossing zero, which switches the frame target. Retire it through `retiredTargets[frameIndex]` (a throwaway `RenderTarget` carrying `colorFb`).
- `DestroySceneTargets` and `FreeTargetObjects` do not reset `mergeNormalFbDepth`. The rebuild check compares raw `VkImage` handle values, so a recreated depth image that gets the same handle back keeps a framebuffer bound to a destroyed view. The same hazard is already handled for FSR2's depth view.

---

## Phase 2 — functional bugs and wasted work

### 2.1 Animated BLASes built with no RT scene — Perf, Strongly expected

`R_RtStageAnimCasters` gates on `r_rtAnimBlas` (default 1) and a real map view, not on any RT feature. On an RT-capable card with RT off, every visible GPU-skinned monster costs a BLAS build at first sight and a refit every frame, with no consumer. Gate staging on `R_RtSceneWanted()`.

### 2.2 `r_swapInterval` never reaches the Vulkan swapchain — Definite

The backend watches the cvar's modified flag, but core `R_CheckCvars` clears it earlier in the same frame. Store the interval the swapchain was built with and compare against the cvar in `BeginFrame`. Record it before the zero-extent early return so a minimised window cannot loop.

### 2.3 Captures skipped for a frame after resize under a frame target — Probable

`CopyFramebufferToImage` (~9066) tests `sceneEverWritten` even when the source is the frame target. After a resize or `vid_restart` with HDR on, every `_currentRender` / `_currentDepth` capture in the first frame returns early. Test the flag of the image actually being read, as `BeginPass` does.

### 2.4 "Free VRAM" budget is a fixed heuristic — Probable

`GetDeviceLocalMemory` relies on `vmaGetHeapBudgets`, but `VK_EXT_memory_budget` is never enabled and the allocator is created without the budget flag. VMA then reports 80 % of the heap as budget and only its own blocks as usage (swapchain and FSR2's direct allocations are invisible). The auto shadow-cache budget is built on this.

- Enable the extension when present, set the allocator flag, and refresh the budget once per frame.
- **This changes the cache size you tuned on 2026-10-03** — land it alone and A/B it.

### 2.5 Layout flags after a failed FSR2 dispatch — Probable (error path)

On failure `fsr2Out` and `fsr2Reactive` are left in `GENERAL` while their "written" flags still claim the resting layout. Clear both flags in the failure branch.

### 2.6 NRD history used without a clear after a denoiser switch — Probable

A rebuilt NRD instance clears only the denoiser passed on its first frame. Switching RTAO ↔ RTGI later at the same size runs the other denoiser in `CONTINUE` mode over never-initialised history. Track a per-denoiser "primed" flag, reset in `NrdCreate`.

---

## Phase 3 — performance

### 3.1 Acceleration structures live in host-visible memory — Benchmark required

`CreateRtBuffer` requests host-visible, host-random-access memory for every RT buffer, including AS storage and build scratch, which the CPU never touches. The vendored allocator's selection logic picks the cached system-RAM type for that request, so every ray traversal would read BVH nodes across PCIe.

- Add an access parameter: GPU-only (AS storage, scratch → device-local, unmapped) versus host-written (build inputs, instance buffer, geometry table → unchanged).
- Direction strongly expected; magnitude unknown. Measure rtao / rtgi / rtblur / inline-ray scopes with `r_vkGpuTime 2` before and after.
- Cost: the AS moves into VRAM, which reduces the auto shadow-cache budget by its size.

### 3.2 Per-frame heap churn in RT and NRD paths — Strongly expected, small

- `UpdateTlas` constructs two `std::vector`s per call → member scratch, `clear()`.
- `NrdRecordDispatches` constructs two per dispatch → hoist out of the loop.

### 3.3 Not worth doing without a measurement

- One descriptor-set alloc/update/free per compute dispatch (`RecordDispatch`).
- A one-entry memo in front of `GetPipeline`'s map lookup.
- FSR2's display-size output copy.
- Whole-cache flush in `InvalidateTextureSets`; once 1.1 stores handles per entry, a targeted erase becomes possible.

---

## Phase 4 — hardening and error paths

Small, independent, low risk. Batch into one or two commits.

- **Deferred destroys when nothing is recording.** `RetireImage` always defers; `DestroyImage`, `DestroyRenderTarget`, `RtaoDestroyOutput`, `RtgiDestroyOutput` and `Fsr2DestroyContext` test only `frameOpen`, not `skipFrame`. Between frames or in a skipped frame the slot's retire list drains one frame early. Introduce one predicate (`frameOpen && !skipFrame`) and use it at all six sites; `RetireImage` falls back to `DestroyImage`.
- **`DestroyRtScene`** skips its queue wait when only the animated-BLAS cache is live. Widen the condition to everything it destroys. (2.1 removes the main trigger.)
- **`vkCheck`** returns false for successful calls once the device is dead, so callers drop live handles and the recovery `vid_restart` can hit VMA's leak assert. Return `res == VK_SUCCESS` in the dead branch.
- **`Fsr2EnsureContext`**: two failure paths call `ffxFsr2ContextDestroy` on a malloc'd, uninitialised context. Free and null it first.
- **`FindOrAllocAnimBlas`**: a failed build strands its slot and a new one is appended every frame. Match the first loop on key alone.
- **`UpdateTlas`**: on a failed regrow of the pinned slot, `rtCurrentAddr` keeps the destroyed TLAS's address. Zero it when destroying.
- **`CreateSwapchain`**: `formats[0]` is read from a possibly empty vector.
- **`RecreateSwapchain`**: failure does not set `swapchainDirty`; the out-of-date path does not re-check for a zero extent.
- **`GpuTimerReadback`**: masks each stamp before subtracting; use `(end - begin) & mask`.
- **`CreateColorTarget`**: the two mip-chain failure branches free the views but leave their image-table slots live. Release the slots first.
- **`BeginPass`**: add `insideTargetPass` to the early-out.
- **Synchronous submits** ignore the `vkQueueSubmit` result and then wait on the fence forever on an out-of-memory failure (about ten sites).

---

## Decisions for you

1. **Vulkan floor.** Master still requires 1.4. The 1.3-floor commit `4b906e21` exists only on `feat/vulkan-1.3-floor` and was never merged, although it is recorded as the fix for the Windows black screen.
2. **Portal-sky view and temporal passes** (see 1.2).
3. **Validation callback.** It prints through `common->Warning`, which during a map load can run `PacifierUpdate` and start a nested frame from inside a Vulkan call. Deferring the print to `BeginFrame` fixes that but delays messages on a validation run.
4. **Clamp-to-zero fidelity.** Vulkan maps `TR_CLAMP_TO_ZERO` / `_ALPHA` to a transparent-black border; GL uses clamp-to-edge with forced border texels, so alpha (and RGB for the alpha variant) differ outside [0,1]. Parity is opt-in per the fidelity policy.
5. **`out_Albedo` validation warning.** `gbuffer.frag` writes location 3 unconditionally while the merged prepass never has a fourth attachment. Either compile a variant without it or accept the warning.

## Noted, no action proposed

- Pass-class aliasing: `PassClassFor` ignores attachment count for RGBA16F and puts RG16F in the RGBA8 class. No shader straddles the aliased layouts today.
- Subpass dependencies in the scene and merged-prepass render passes under-specify stages; only synchronisation validation would report it.
- In-place skin compute writes to `gpuSkinVB` are not ordered against the previous frame's reads; worst case is a one-frame torn pose.
- `DrawZfillBatch` overwrites its per-slot buffers when called twice in a frame (`r_vkBdaZfill 2`, default off).
- A fake target pass (invalid handle) does not suppress the draws issued for it; all current callers guard their handles.

## Frontend (fork-added code)

Covers our additions only: the Vulkan paths in `VertexCache.cpp`, GPU skinning in `Model_md5.cpp` and its flush in `RhiBackend.cpp`, the `R_Rt*` scene code in `tr_main.cpp`, and the Vulkan image paths. Items F1, F3, F4 and F5 were spot-checked against the source; F2, F6 and F7 are as reported by the audit and need a re-read before editing.

### F1 Per-mesh skin/tess buffer handles survive a full `vid_restart` — Probable (Phase 1 class)

`idMD5Mesh` keeps `skinGpuWeights` and its sibling handles (and the tess buffers) for the mesh's lifetime. `R_ResetBackendGpuCaches` resets the compute *shaders* but not these per-mesh *buffers*, and `EnsureSkinBuffersUploaded` returns early when the handle is non-zero. After a full backend teardown the handles are stale: skin jobs bind whatever now owns those slots, and a later `~idMD5Mesh` calls `DestroyBuffer` on them — freeing someone else's live buffers, since buffer handles carry no generation.

- Add a generation counter bumped in `R_MD5_ResetGpuShaders`; stamp it per mesh at upload. On mismatch, zero the handles without destroying and re-upload; the free functions skip `DestroyBuffer`.
- The audit states a full `vid_restart` keeps the mesh objects alive (only vertex caches are freed). Confirm that before starting; the mod-switch path deletes the models first and is fine.
- Validate: full `vid_restart` in a room with monsters, then a map change, validation on.

### F2 Skin-once-per-frame guard can drop a re-skin — Probable, low severity (reported, not re-read)

The guard keys on `tr.frameCount`. A model re-instantiated between two frames with new joints (the audit names a render-model trace) while `frameCount` is unchanged re-poses the CPU verts but skips the GPU job, leaving `gpuSkinVB` and its BLAS one tic behind. Key the guard on a serial bumped in `RB_RHI_FlushSkinJobs` instead.

### F3 Every level load re-decodes and re-uploads all referenced images on Vulkan — Perf, Strongly expected (load time)

`EndLevelLoad` decides "not loaded" from `texnum == TEXTURE_NOT_LOADED`, but the Vulkan upload paths only ever set `rhiHandle`; `texnum` is never assigned a loaded value. So nothing is ever "kept from previous": a map change, same-map reload or savegame load redoes the full decode, mip build, destroy (with a GPU-idle wait per image) and upload.

- Test `texnum == TEXTURE_NOT_LOADED && rhiHandle == 0` at the three sites in `Image_init.cpp` (~2180, 2204, 2286).
- Measure with the existing load-phase timing; this sits on top of the 0.9.5 load work.
- Check alongside: on a same-map reload the RT scene is kept while its geometry-table texture indices were baked from the old handles.

### F4 Joint palette buffer created and destroyed per skinned entity per frame — Perf, Benchmark required

`RB_RHI_FlushSkinJobs` does one `CreateBuffer` + deferred `DestroyBuffer` per visible GPU-skinned entity each frame (`r_gpuSkinning`, default on). Pack the frame's palettes into one grow-only storage buffer per frame slot and bind by offset. This is also what would let the per-dispatch descriptor sets in 3.3 be reused.

### F5 Dynamic-model caches become dedicated persistent buffers — Perf, Benchmark required

On Vulkan every non-temp `vertexCache.Alloc` creates its own `VkBuffer`, including caches rebuilt whenever a dynamic model re-instantiates (index, interaction and shadow caches) and frame-temp overflow. Count these per frame first (a temporary counter); only if the number is material, route dynamic-model blocks to the per-frame ring.

### F6 A failed or empty RT scene build retries in full every frame — Probable, edge case (reported, not re-read)

The failure path clears `r_rtWorld` and `r_rtSunShadows` but the scene stays "wanted" when RTAO, RTGI, all-lights or moving-lights is on, so the whole build (and its warning) repeats each frame. Same for a map with no casters. Remember the failed map name and skip until the map or a scene cvar changes.

### F7 Linear searches and heap churn in the per-frame RT refresh — Perf, Benchmark required (reported, not re-read)

`R_RtRefreshInstances` does a model-pointer linear search per entity, and non-casting static entities pay a full topology hash every frame to rediscover a cached "no BLAS". The monster gather and caster staging each do two `Mem_Alloc16`/`Free16` per frame. Cache the resolved BLAS (or "non-caster") per model and reuse grow-only scratch.

### Checked and found sound

Vertex-cache free/alloc (no double destroy), ownership of `gpuSkinVB` and tess buffers on surface free, the RT scene statics reset, the parallel image load (workers never touch the RHI), and the skin/tess job flush ordering.

## Suggested order

1. Phase 1 items 1.1 → 1.5, then F1, each validated.
2. 2.1 and 2.2 (both tiny).
3. F3 with load-phase timings, and 3.1 with before/after GPU scope numbers.
4. Phase 4 batch.
5. 2.4 alone, with an A/B of the shadow-cache budget.
6. Remaining Phase 2 items, 3.2, F2 and F6.
7. F4, F5 and F7 only after measuring.
