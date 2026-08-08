# CPU → GPU Offload: RHI Project Plan

**Goal.** Doom 3 / DUDE is **CPU-front-end bound** — the backend GPU frame is ~12–14 ms
(RTX 3080 Ti, nightmare) while the single-threaded front-end caps fps. Pure GPU-pass
optimization therefore has an fps *ceiling*; the only work that actually raises fps in the
common case is **moving CPU front-end work onto the GPU**. This document scopes that work:
the RHI primitives it needs, which offloads are possible on **OpenGL 3.3** vs **Vulkan-only**,
and a phased, risk-ordered build order.

Every offload here must be **output-identical** (skinning/culling produce the same pixels) and
**opt-in cvar-gated with a CPU fallback**, matching the project's existing enhancement pattern.

---

## 1. The one hard constraint — the GL 3.3 ceiling

The GL backend requests a **3.3 core** context (`glimp.cpp:450–454`; 2.1 fallback). That fixes
the entire GL-vs-Vulkan split, because the modern GPGPU toolbox is all GL 4.x:

| GPU capability | GL version | GL 3.3? | Vulkan (1.4 floor) |
|---|---|---|---|
| Compute shaders (`glDispatchCompute`) | 4.3 | ❌ | ✅ core |
| Shader Storage Buffers (SSBO) | 4.3 | ❌ | ✅ core |
| Image load/store, buffer atomics | 4.2 | ❌ | ✅ core |
| GPU-generated indirect draw | 4.0 / 4.3 | ❌ | ✅ core |
| **Transform feedback** (VS→buffer capture) | **3.0** | ✅ | ✅ (`VK_EXT_transform_feedback`) |
| **Geometry shaders** | **3.2** | ✅ | ✅ core |
| **Texture-buffer objects** (TBO) | **3.1** | ✅ | ✅ (storage/uniform buffer) |
| UBOs / instancing | 3.1 / 3.3 | ✅ | ✅ core |

**GL 3.3's offload toolbox = transform feedback + geometry shaders + TBOs + instancing.** These do
*deterministic per-vertex transforms* (skinning, shadow extrusion). They **cannot** do
data-dependent stream compaction into an indirect buffer, which is what GPU-driven culling *is*.
So the rule that falls out:

> **Deterministic vertex work → both backends (TF on GL3, compute on VK).
> Data-dependent culling/compaction → Vulkan only, by construction.**

None of these GL-3.3-legal primitives are wired in the code yet either (grep-confirmed zero
transform-feedback / geometry-shader / `glTexBuffer` / `VertexAttribDivisor` use) — so the GL path
is additive-from-scratch, just not blocked.

---

## 2. What the RHI lacks today

The RHI (`neo/renderer/rhi/RHI.h` + both backends) is a **graphics-only** abstraction. Confirmed
absent everywhere (grep-verified):

- **Compute** — no `Dispatch`, no compute `ShaderHandle`/stage. VK creates pipelines only via
  `vkCreateGraphicsPipelines` (`VulkanBackend.cpp:564,1294`); `ShaderRec` has vert/frag/tesc/tese,
  no compute (`:383–394`). GL3 `GL3_BuildProgramObject` hardcodes a 2-stage vert+frag loop
  (`GL3Shaders.cpp:290–292`).
- **Storage buffers** — `BufferUsage` is `{BU_VERTEX, BU_INDEX, BU_UNIFORM}` only (`RHI.h:29–33`);
  no `BU_STORAGE`. Every VK `CreateBuffer` alloc is `HOST_VISIBLE|HOST_COHERENT`
  (`VulkanBackend.cpp:2585`), never device-local GPU-write. No `STORAGE_BUFFER`/`STORAGE_IMAGE`
  descriptor type in either of the two set layouts (`:2116–2147`).
- **Indirect draw** — `Draw(const DrawArgs&)` is a single direct indexed draw with a literal
  `indexCount` (`RHI.h:92–109`); no count-from-buffer, no instancing.
- **Transform feedback / geometry stage** — neither exists on either backend.

**Three net-new RHI primitives underpin all offload work:** (a) a **compute lane**
(dispatch + compute shader), (b) **storage buffers** (device-local, GPU-writable), (c) **indirect
draw**. Transform feedback + a geometry stage are the GL-3.3 substitutes for (a).

---

## 3. The three offload candidates

### 3a. GPU skinning — *highest value, widest reach, both backends*

**CPU today.** Every frame, for every visible/shadowing animated MD5 entity, the CPU re-skins the
whole mesh: `idMD5Mesh::TransformVerts` (`Model_md5.cpp:255`, SIMD weighted joint-matrix sum) writes
**only xyz**, then `R_DeriveTangents` (`:357`) runs a **second** per-vertex pass to rebuild
normals/tangents/face-planes. Driven once/entity/frame from `R_EntityDefDynamicModel`
(`tr_light.cpp:1117`, gated by `dynamicModelFrameCount`).

**The leverage — "deform once, draw everywhere".** The skinned output lands in one
`tri->ambientCache` (`R_CreateAmbientCache`, `tr_light.cpp:67`) that is **already shared by 6 of the
7 consumers**: interaction, zfill/depth, ambient, 2D shadow-map, cube shadow-map, and fog all read
`tri->ambientCache`. A single GPU skin per entity feeds all six. Only the legacy **stencil** path
reads a distinct buffer (`tri->shadowCache`, a doubled xyz+w array) — see §3b.

**What a GPU skinner reads/writes.** Reads: the joint palette `ent->joints` (`idJointMat`, 3×4
row-major, 48 B each), plus the static per-mesh `scaledWeights` (`idVec4[]`) and `weightIndex`
(`int[2]`: joint byte-offset + last-weight terminator) built once at load (`Model_md5.cpp:205–217`).
Writes: deformed `idDrawVert` xyz — and, to *fully* offload, normals+tangents (else `R_DeriveTangents`
stays on CPU).

- **GL 3.3:** transform-feedback skinner — a VS reads the joint palette + weight tables via **TBO**,
  captures deformed verts to a VBO reused as `ambientCache`.
- **Vulkan:** compute skinner reading SSBO/TBO, writing a **device-local** vertex buffer used as
  `ambientCache`. Cleaner (random-access output, no rasterizer-discard dance).

### 3b. GPU stencil shadow-volume generation — *lower priority; a CPU pre-win exists*

**CPU today.** For every (shadow-casting light × dynamic entity), the CPU builds the silhouette +
volume topology (`R_CreateVertexProgramTurboShadowVolume`, `tr_turboshadow.cpp:43`) and uploads a
doubled xyz+w buffer, every frame the model animates. Note the **GPU already does the extrusion**
(`shadow.vert` projects w=0 verts to infinity) — the remaining CPU cost is the **silhouette/topology
build + streaming**, not the projection.

> **Mapping finding worth acting on first (a CPU win, not an offload):** the CPU volume *build* is
> **not gated by `r_shadowMapping`**. `CreateInteraction` always calls `R_CreateShadowVolume`
> (`Interaction.cpp:916`) and `AddActiveInteraction` always builds+uploads `global/localShadows`
> whenever `HasShadows()` — even for lights that are then **shadow-mapped and never draw stencil**
> (`useStencil` false). In a shadow-mapped scene, stencil is actually *drawn* only for parallel +
> oversize-"sun" lights, yet the volumes are built for *all* shadowing interactions. **Gating that
> build when the light won't draw stencil is a pure CPU win with no GPU work** — see Phase 0.

Because shadow maps (DUDE Phase 3.5) already displace most stencil *drawing*, and Phase 0 can kill
most of the wasted *building*, GPU shadow-volume generation is the **lowest-priority** offload.
If still wanted: geometry-shader silhouette (GL 3.2 + VK) or a compute builder (VK).

### 3c. GPU-driven culling — *biggest CPU relief, Vulkan-only by construction*

**CPU today.** Every view re-runs the entire visibility solve single-threaded from scratch: a
recursive portal flood (`FlowViewThroughPortals`), per-area linked-list walks gated by
`R_CullLocalBox` (sphere + 8-corner transform + N-plane dots) over all entity/light refs, an
`O(lights×entities)` interaction-pairing sweep (`CreateLightDefInteractions`, `tr_light.cpp:550`),
a full `qsort` of `drawSurfs`, then a **scalar backend loop** issuing one state-set + one draw per
surface (`RB_RenderDrawSurfListWithFunction`, `tr_render.cpp:268`). The GPU sees none of this
metadata — only the pre-culled, pre-sorted list replayed one draw at a time.

**GPU-driven version.** A compute shader tests persistent per-object bounds/matrices against
frustum/Hi-Z, writes a compacted `VkDrawIndexedIndirectCommand[]` + a visible count, consumed by
`vkCmdDrawIndexedIndirectCount`. This **cannot** exist on GL 3.3: it needs compute (4.3) + SSBO (4.3)
+ atomic append/compaction (4.2) + GPU-generated indirect draw (4.0/4.3). It also needs geometry in
**stable GPU-resident buffers keyed by a persistent per-object index** — the static-VBO work
([[vulkan-static-vertex-buffers]]) is a partial prerequisite, and skinned models (re-instantiated on
CPU each frame) need GPU residency, which Phase 2 provides. **Highest complexity, highest payoff,
Vulkan-exclusive.**

---

## 4. Capability matrix

| Offload | GL 3.3 | Vulkan | CPU relief | Notes |
|---|---|---|---|---|
| **Skinning** | ✅ transform feedback + TBO | ✅ compute + SSBO | high | one skin feeds 6 passes; VK path is cleaner |
| **Skinning: normals/tangents too** | ✅ (in the TF/VS pass) | ✅ (in the dispatch) | +high | else `R_DeriveTangents` stays on CPU |
| **Shadow-volume *build* gating** | ✅ (CPU-only, no GPU) | ✅ | med | Phase 0 — free win, do first |
| **Shadow-volume GPU generation** | ✅ geometry shader | ✅ compute/geom | low* | *mostly neutralized by shadow maps + Phase 0 |
| **GPU-driven culling** | ❌ impossible | ✅ compute + SSBO + indirect | **highest** | needs stable GPU residency + indirect draw |

---

## 5. Phased build order (risk-ordered)

### Phase 0 — CPU pre-win: gate the wasted stencil-volume build *(no RHI change)*
Suppress `R_CreateShadowVolume` / the `global/localShadows` build in `AddActiveInteraction` for
interactions on lights that will be shadow-mapped (not parallel, not oversize, in cube budget) — the
same technique routing the backend already computes (`RhiWorld.cpp:4586–4616`), lifted to a front-end
predicate. **Risk:** the cube-budget decision is view-dependent/backend-side, so the predicate must be
conservative (build if unsure). **Payoff:** removes per-frame silhouette+topology+upload for every
shadow-mapped dynamic interaction. **Validate:** `r_showPrimitives` `c_createShadowVolumes` drops in
mapped scenes; shadows unchanged. *Independent of the GPU work; ship first.*

### Phase 1 — RHI compute lane + storage buffers *(Vulkan; the foundational primitive)*
Add to the RHI: `BU_STORAGE`, a compute `ShaderHandle`/stage, `Dispatch(x,y,z)`, storage-buffer
binding for a dispatch, and a device-local GPU-writable buffer variant. **VK is the natural (and for
now only) home** — the graphics queue is compute-capable by spec, so a dispatch records onto the
existing per-frame cb and submits on `gfxQueue` with **zero new queue/sync** (insertion:
`VulkanBackend.cpp` dispatch window `:1676→` first `BeginPass`; new `vkCreateComputePipelines` sibling
to `GetPipeline` `:4940`; `BU_STORAGE→VK_BUFFER_USAGE_STORAGE_BUFFER_BIT` at `CreateBuffer :2571`;
`shaderc_compute_shader` kind in `CreateShaderFromGlsl :2902`). GL3 exposes these as unsupported
(returns 0). **Deliverable:** a trivial validated dispatch (e.g. a buffer fill) measured with the
existing timestamp idiom (`gpuTimerPool`, `:1686/:1971`). *This is the "compute lane the RHI lacks"
that also unblocks tessellation "deform-once" and SSAO tiling.*

### Phase 2 — GPU skinning *(both backends; highest value)*
On the Phase-1 lane (VK) and transform feedback (GL3), skin animated MD5 meshes on the GPU into a
buffer reused as `ambientCache`; write xyz **and** normals/tangents to also retire the CPU
`R_DeriveTangents` pass. Feeds interaction/zfill/ambient/2D-shadow/cube-shadow/fog from one deform.
- **VK:** compute skinner → device-local vertex buffer. Barrier `COMPUTE_SHADER→VERTEX_INPUT`.
- **GL3:** new RHI capture primitive — `glTransformFeedbackVaryings` before link
  (`GL3Shaders.cpp:325`), a rasterizer-discard capture pass near `Draw` (`GL3Backend.cpp:958`), joint
  palette via a `glTexBuffer` TBO; captured VBO feeds back through `BindVertexLayout` **unchanged**.
- Keep the CPU skinner as the fallback (`r_gpuSkinning 0`). Leave the stencil `shadowCache` on CPU
  initially (Phase 0 already trims most of it). **Payoff:** eliminates per-frame CPU skin +
  tangent-derive for every animated actor — direct front-end relief. **Risk:** medium (bit-exact
  tangent reconstruction; mirror-seam vert handling `Model_md5.cpp:344`).

### Phase 3 — GPU-driven culling *(Vulkan-only; biggest relief, most architecture)*
Add indirect draw to the RHI (`vkCmdDrawIndexedIndirect[Count]`, swapping `vkCmdDrawIndexed :5557`).
Upload persistent per-object bounds + matrices + sort keys; a compute pass frustum/Hi-Z-culls and
compacts a `VkDrawIndexedIndirectCommand[]` + count. Requires stable GPU-resident geometry keyed by a
persistent object index (static-VBO work is a partial prereq; Phase-2 skinning gives dynamic models
GPU residency). GL3 keeps the CPU cull permanently. **Payoff:** attacks the actual bottleneck — the
per-frame `R_CullLocalBox` sweep + scalar draw loop. **Risk:** high (persistent residency for a
frame-arena-oriented renderer; portal visibility is hard to fully GPU-port — likely a *hybrid*: CPU
portal-area coarse pass feeds GPU fine cull).

### Phase 4 *(optional)* — GPU shadow-volume generation
Only if Phase 0 leaves meaningful stencil-build cost. Geometry-shader silhouette (GL 3.2 + VK) or a
compute builder (VK) writing the doubled `shadowCache` + cap-sorted indices. Low priority.

---

## 6. Consolidated RHI API additions

| Addition | Phase | GL 3.3 | Vulkan | Insertion |
|---|---|---|---|---|
| `BU_STORAGE` + device-local buffers | 1 | ❌ (n/a) | ✅ | `RHI.h:29`; `CreateBuffer` VK `:2571`, VMA `:2585` |
| Compute `ShaderHandle` + stage | 1 | ❌ | ✅ | `ShaderRec :383`; `LoadShader :2691`; `CreateShaderFromGlsl :2902` |
| `Dispatch(x,y,z)` + storage bind | 1 | ❌ | ✅ | new compute pipeline `~:4940`; record `:1676→` |
| Transform-feedback capture pass | 2 | ✅ | (VK uses compute) | `glTransformFeedbackVaryings` `GL3Shaders.cpp:325`; discard pass near `GL3Backend.cpp:958` |
| TBO bind (joint palette) | 2 | ✅ `glTexBuffer` | ✅ SSBO/UBO | GL3 texture loop `:969–987` |
| Geometry stage (opt) | 4 | ✅ | ✅ | GL3 2-stage loop `GL3Shaders.cpp:290`; VK stage assembly `:4980` |
| Indirect draw + instancing | 3 | ❌ | ✅ | `DrawArgs`/`Draw` `RHI.h:92`; VK `:5557`; GL3 `:989` (instancing only) |

VK enablers already in place: Vulkan 1.4 floor (all core compute guaranteed, no extension gating),
runtime shaderc compiler, VMA, a timestamp-query idiom to measure any new pass. The `queues[]` array
is fixed `[2]` and scans graphics-only (`:842–863,912`) — fine for graphics-queue compute; a *dedicated
async-compute* queue (not needed until Phase 3 overlap tuning) would extend both.

---

## 7. Sequencing & dependencies

```
Phase 0 (CPU stencil-build gate) ── independent, ship first, de-risks Phase 4
Phase 1 (VK compute lane + BU_STORAGE) ── blocks Phase 2-VK and Phase 3
   ├── Phase 2 (skinning): VK on Phase 1; GL3 on a parallel transform-feedback primitive
   │        └── gives dynamic models GPU residency ── prereq for Phase 3
   └── Phase 3 (GPU culling, VK-only): needs Phase 1 + indirect draw + Phase 2 residency
Phase 4 (GPU shadow-volume gen) ── optional, only if Phase 0 leaves cost
```

The **GL3 transform-feedback primitive** (Phase 2-GL3) is independent of the **VK compute lane**
(Phase 1) — they're two separate substitutable back-ends for the same "deform once" capability, so
Phase 2 can land VK-first and GL3-later (or vice-versa) behind one `r_gpuSkinning` cvar.

## 8. Decisions to make before starting

1. **Backend scope for skinning** — VK-only first (fewer moving parts, compute is cleaner), then
   backfill the GL3 transform-feedback path? Or both together behind one cvar? (Recommend VK-first.)
2. **How far to chase culling** — full GPU-driven (Phase 3, VK-only, large) vs stop after skinning
   (Phase 2 gives most of the *portable* CPU relief). Culling is the biggest win but the biggest lift
   and Vulkan-exclusive.
3. **Phase 0 now?** — it's a cheap CPU win independent of everything else; worth landing regardless of
   how far the GPU-offload project goes.

Related: [[vulkan-gpu-offload-interest]], [[tessellation-plan]] (compute "deform-once" shares Phase 1),
[[gpu-perf-heavy-pass-profile]], [[vulkan-static-vertex-buffers]] (Phase 3 residency prereq),
`docs/vulkan-backend.md`.
