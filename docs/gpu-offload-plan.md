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
| **Shadow-volume *build* gating** | ✅ (CPU-only, no GPU) | ✅ | med | Phase 0 — ✅ SHIPPED (`0bf7e1dd`) |
| **Shadow-volume GPU generation** | — | — | — | ❌ STRUCK — subsumed by ray-query (Phase 4) |
| **GPU-driven culling** | ❌ impossible | ✅ compute + SSBO + indirect | **highest** | needs stable GPU residency + indirect draw |

---

## 5. Phased build order (risk-ordered)

### Phase 0 — CPU pre-win: gate the wasted stencil-volume build *(no RHI change)* — ✅ SHIPPED (`0bf7e1dd`)
Skip `R_CreateShadowVolume` in `CreateInteraction` when the light will be shadow-mapped rather than
stencil-shadowed (`r_shadowMapSkipStencilBuild`, default on) — mirroring the backend routing (shadow
mapping on, not an oversize "sun" above `r_shadowMapStencilRadius`, flashlight exempted). Gates **all**
casters, not just animated ones: the dominant cost turned out to be *world* casters whose interaction is
rebuilt every frame because their **light** moved (projectile/flicker), and the map's caster path
(`shadowMapCasters`, from `ambientTris`) shadows them independently, so it's fidelity-identical.
`BeginFrame` `FreeInteractions()` on a change to the three routing cvars prevents a cached NULL going
stale on toggle. **Measured (Vulkan):** indoor combat (all shadow-mapped) → `built ~0, skipped 3000–5900/s`
≈ 100% of per-frame volume builds removed (~46 builds/frame at peak); outdoor ~70–80% (sun lights still
stencil, correctly). User-verified shadows consistent. Readout: `r_shadowMapCacheDebug` now also prints
`shadowVol/s: built N, skipped M`. *Independent of the GPU work — shipped first, as planned.*

### Phase 1 — RHI compute lane + storage buffers *(Vulkan; the foundational primitive)* — ✅ SHIPPED (`cf18e615`)
Added to the RHI: `BU_STORAGE`, `CreateComputeShader(name, glslSrc)`, `ComputeArgs` + `Dispatch()`,
`ReadBuffer()` — all with degrading base defaults so GL3 (no compute) needed **zero changes**. Vulkan
backend: a `BU_STORAGE` host-visible+coherent buffer (RANDOM access so read-back stays cached), runtime
`shaderc_compute_shader` compile into `ShaderRec.comp` (the graphics prelude is skipped for compute), a
dedicated compute descriptor-set layout (8 storage bindings) + pipeline layout (128-byte push-const) +
FREE-able pool, a `ShaderHandle`-keyed compute-pipeline cache reusing `diskPipelineCache`, a shared
`RecordDispatch` helper, and the mid-frame `Dispatch()` (records pre-scene on the frame cb with a
COMPUTE→VERTEX/SHADER barrier + fence-retired sets — the path Phase 2 skinning will use). The graphics
queue is compute-capable, so **no new queue/sync**. **Delivered:** `r_vkComputeTest` dispatches a kernel
doubling a 256-element storage buffer, verifies the seed round-trip then `data[i]==2*i` — user-verified
`PASS` on Vulkan; adversarial-review workflow found 0 confirmed defects. **Deferred to Phase 2** (logged):
device-local storage + staged `ReadBuffer` (only needed when GPU-write bandwidth matters); a `STORAGE|
VERTEX` buffer for skinning output; precompiled `.comp.spv` (runtime shaderc is fine for now). *This is
the "compute lane the RHI lacks" that also unblocks tessellation "deform-once" and SSAO tiling.*

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

#### TBN source: why the *faithful* one lost

Two ways to get a skinned TBN, both built and A/B'd in-engine:

1. **Per-frame GPU re-derive** (was `r_gpuSkinDerive 1`; **built, lost, then removed** in `e14dfc6a`
   — the cvar and its kernel no longer exist) — a second compute pass porting
   `R_DeriveUnsmoothedTangents`. Verified to reproduce `idSIMD_SSE41::DeriveUnsmoothedTangents` to
   0.006° avg / 0.04° max. Exactly stock. **It tears animated meshes open at their seams under PN
   tessellation** — faithfully, because stock's unsmoothed path early-returns *before* the `dupVerts`
   weld (`tr_trisurf.cpp:1804` vs the weld at `:1927`), so coincident verts legitimately disagree.
   Invisible in stock's rasterizer; a hole once PN patches build control points from those normals.
2. **Pre-welded baked bind TBN + LBS** (the survivor, now the only path) — bake welds every authored
   coincident group (`dupVerts` + `mirroredVerts`, union-find, **no angle gate**) so the pair shares
   one bind normal. Coincident verts share a weight run ⇒ identical bind normal ⇒ bit-identical
   skinned normal at every pose ⇒ the seam *cannot* open. Costs a small shading divergence.

Dead ends worth not repeating: `R_WeldSeamNormals`' dot gate (default `0.7` ≈ 45°) rejects precisely
the pairs that crack — the measured offender was **49.54°**, just past it. And `r_tessWeldSeams`
welds in `UpdateSurface`, which the derive pass then overwrote, so it appeared to "do nothing"
whenever GPU skinning was on. **`r_tessWeldSeams` / `r_tessWeldThreshold` are now dead weight** —
the bake weld supersedes them entirely, and they are still listed as a blocker for retiring the CPU
tangent derive (see Milestone C below), so removing them is worth real points, not just tidiness.

Note the *normal* weld was only half the seam story: it fixed the direction coincident verts move
in, not the distance. Displacement pulled them apart anyway because the height is sampled at the
vertex UV — see `docs/tessellation.md`, "UV-seam displacement pinning".

Method note: the GPU-vs-CPU numbers in `r_gpuSkinTest` were misleading for several rounds because
`r_useDeferredTangents` (default 1) defers `R_DeriveTangents` to `R_CreateAmbientCache`, *after* the
validation hook — so the reference normals were a frame stale and ~15° of "error" was really one
frame of animation. The harness now re-derives its own reference. The choice above was still settled
by looking at an imp, not by the metric.

#### Milestone C — what GPU skinning can actually retire *(audit 2026-08-10)*

Phase 2's stated payoff ("eliminates per-frame CPU skin + tangent-derive") is only **half**
achievable, and it is worth being exact about which half. The CPU skin splits cleanly in two, and
only one part is redundant. **As shipped today, GPU skinning *adds* the compute pass on top of the
CPU one rather than replacing it.**

**Position skinning is load-bearing and has to stay.** `TransformVerts`
(`Model_md5.cpp:950` → `SIMDProcessor->TransformVerts`) writes **only `xyz`**. Three front-end
consumers read those positions every frame, and none of them can see `gpuSkinVB` — it is a GPU-side
buffer with no CPU mapping:

| Consumer | Site | Frequency |
| --- | --- | --- |
| Light culling (`R_CalcInteractionCullBits`, `R_ClipTriangleToLight`) | `Interaction.cpp:130`, `:405` | per interaction, **per light** |
| Stencil shadow volumes (`R_CreateShadowVolume`, `R_CreateVertexProgramShadowCache`) | `Interaction.cpp:947`, `tr_light.cpp:245` | per shadowing stencil light — **absent** for shadow-mapped lights |
| Surface bounds (`R_BoundTriSurf`) | `Model_md5.cpp:962` | once per surface |

Retiring those means moving culling and shadow-volume construction to the GPU as well — Phase 3
and ray-query (Phase 4 is struck) — not flipping a flag. So "switch the CPU skinner off when GPU
skinning is on" is not available: the front end is structurally position-dependent.

**And even both do not fully unpin it (recon 2026-08-10).** Phase 3 retires consumer 1 (light cull)
and ray-query retires consumer 2 (stencil volumes), but consumer 3 — `R_BoundTriSurf` surface bounds
(`tr_trisurf.cpp:783`) — has **no retirement path** short of a GPU min/max reduction or accepting a
one-frame-stale CPU bound. So "finally remove the CPU position-skin" is Phase 3 + ray-query + a bounds
solution, three pieces, not two — and on GPU-bound hardware it buys architecture (RTX alignment), not fps.

**The TBN half is the redundant part — but it is NOT the expensive half.** `TransformVerts` never
touches normals; those come from `R_DeriveTangents`, which the compute kernel duplicates and the CPU
then discards. The intuitive read is that this is the costly piece. **It is not, and the numbers are
already in** (`r_gpuSkinProfile`, commits `73a18055` / `15cb84f9`, run in a 5-enemy fight):

| | per frame |
| --- | --- |
| skinned-surface tangent derive | **~0.007 ms** |
| ambient-cache upload | **~0.025 ms** |
| **total Milestone-C prize** | **~0.03 ms** — ~0.5% of a ~6 ms frame |

The derive is cheap for a structural reason worth remembering: **MD5 meshes carry `dominantTris`**,
so `R_DeriveTangents` early-returns at `tr_trisurf.cpp:1793` into `R_DeriveUnsmoothedTangents` —
an O(verts) walk over precomputed per-vertex triangle references, *not* the general smoothed path
with its face-plane build and per-vertex accumulation. Skinned meshes never take the expensive road.

**And the bottleneck is elsewhere.** `com_speeds` on the RTX 3080 Ti in realistic combat: front-end
`rf` ~0–1 ms, backend `bk` ~0 ms, GPU **8.75 ms**. At these enemy counts the engine is **GPU-bound**,
so CPU→GPU skinning offload relieves a bottleneck that isn't there. Phase 2's "direct front-end
relief" claim holds only in a CPU-bound scene, which this hardware does not produce at these counts.

Given that, the two remaining pieces are architectural, not performance work:

1. **The redundant ambient-cache upload — already SHIPPED** (`d07c005f`, `r_gpuSkinNoUpload`,
   opt-in, default off). `tr_light.cpp:134` allocated a fresh VK buffer and copied every vertex every
   frame for a surface that rasterizes from `gpuSkinVB` instead — never drawn. Leaving `ambientCache`
   NULL meant patching ~14 sites (every "has geometry?" gate to accept `gpuSkinVB`, every
   unconditional `Touch` null-guarded, plus `RB_RHI_CasterHash` keyed on `gpuSkinFrame` so animated
   cube shadows still invalidate). Kept behind a cvar so the OFF path stays byte-for-byte unchanged.
   Justified as RTX alignment — the GPU becomes the sole source of drawn geometry — not as fps.
2. **The tangent derive — deliberately NOT done.** It needs *per-surface gating*, not a flag.
   `tr_light.cpp:78` lists its blockers as decals (`idRenderModelOverlay`), deform materials, and the
   tess weld. **The tess-weld blocker is now gone** — `r_tessWeldSeams` was superseded by the bake
   weld — leaving two conditions that are both testable per surface. But at ~0.007 ms it buys
   ~0.1% of a frame while risking decal regressions, so the user's standing decision is to leave it.

**Where the real leverage went instead (RTX reframe).** GPU residency of skinned geometry — the thing
animated BLAS needs — was already delivered by Milestone B (`gpuSkinVB`); Milestone C adds none. The
forward step is device-local, acceleration-structure-ready geometry (`SHADER_DEVICE_ADDRESS` +
AS-build-input usage; `gpuSkinVB` and the static VBOs are host-visible today) → BLAS (static built
once, skinned refit per frame) → TLAS → ray-query shadows/AO. That also *deletes* the stencil
shadow-volume consumer in the table above, which is one of the two things pinning the CPU position
skin in place.

#### Milestone D — stripping the CPU position skin (`r_gpuSkinStripCpu`, built 2026-08-14, UNVERIFIED in-engine)

Milestone C left `TransformVerts` (position skin) + `R_BoundTriSurf` running unconditionally under GPU
skinning — so GPU skinning was **pure added work**, which is exactly why it never won a frame. Milestone D
gates that CPU work OFF for a surface when the frame proves it safe, so the GPU `gpuSkinVB` becomes the
*sole* geometry source. **The recon that struck this as blocked was wrong on two counts:** there are
**four** position pins, not three, and all four turned out tractable (not "no retirement path"):

| Pin | Reader of `tri->verts.xyz` | Retirement in Milestone D |
|---|---|---|
| Light cull | `R_CalcInteractionCullBits`/`R_ClipTriangleToLight` via `R_CreateLightTris` | It is a **pure** optimization — never clips geometry, only drops whole tris fully outside the light frustum (`Interaction.cpp` "we do not actually use the clipped triangle"). Stripped surfaces take an early **full-index** path in `R_CreateLightTris` (reference all indexes, `bounds = tri->bounds`): pixel-identical (one-sided materials still GPU-cull backfaces; exterior tris shade to nothing / fall outside the light scissor), at the cost of extra rasterization. |
| Stencil volumes | `R_CreateShadowVolume` / `R_CreateVertexProgramShadowCache` | Gated on a **per-view** flag `r_viewHasStencilShadowLights` (computed at the top of `R_AddModelSurfaces` via the shared `R_ShadowMapSkipStencilBuild`). No stencil-casting light in view ⇒ no volume is ever built ⇒ safe to strip. Matches Phase 0's domain (fully shadow-mapped scenes). |
| Bounds | `R_BoundTriSurf` (MinMax over verts) | `idMD5Mesh::CalcBoundsFast` — a joint-palette-only conservative bound, **O(joints)**. Per-joint reach (max `\|`joint-local weight pos`\|`) is precomputed in `BuildGpuSkinData`; the runtime unions `(jointOrigin ± reach)`. The AABB over those spheres contains the convex hull of all weighted vertex positions ⇒ a strict superset; every consumer only ever under-culls. **Note `idMD5Mesh::CalcBounds` is NOT this — it calls `TransformVerts` internally (re-skins), so it is not free.** |
| **Decals (the 4th pin)** | `idRenderModelOverlay::AddOverlaySurfacesToModel` reads posed `tri->verts.xyz` **every frame** (not a creation-time snapshot) | Gated per-entity on `!def->overlay` in `R_EntityDefDynamicModel`. Entities with an active blood/burn decal keep the full CPU skin (correctness first); the strip fires for the majority with none. |

**Wiring.** `R_EntityDefDynamicModel` sets a transient `r_skinStripThisModel` (cvar on + `r_gpuSkinning`
+ no overlay + `!r_viewHasStencilShadowLights`) just around `InstantiateDynamicModel`; `UpdateSurface`
ANDs in Vulkan + skin-data-ready + no MD5 skin-scale + the shader/SSBOs being ready (so `gpuSkinVB` is
guaranteed to materialize, else the stripped surface would be invisible), skips `TransformVerts` /
`R_BoundTriSurf` / `R_DeriveTangents` / the ambient upload, sets `tri->cpuSkinStripped`, and takes the
bound from `CalcBoundsFast`. `R_CreateAmbientCache` early-returns (ambientCache NULL, draw from
`gpuSkinVB` — reuses the Milestone-C no-upload gates). **HARD-GATED on `r_gpuSkinning`; OFF path is the
stock CPU skinner byte-for-byte.** `r_gpuSkinProfile` now also prints `STRIP N surf/frame, V CPU-skin
verts/frame removed` — pair with `com_speeds` `rf` for the reclaimed front-end ms.

**Hardware reality (unchanged, user opted in anyway).** On the RTX 3080 Ti the frame is GPU-bound, so
this wins **zero or negative** fps here (the light-cull skip *adds* rasterization). The payoff is the
CPU-bound case (weak GPU / high entity counts) + RTX alignment (GPU as the sole geometry source). Built
to reclaim measurable CPU front-end time (visible in `com_speeds rf` / the strip counter), not 3080 Ti fps.
**Documented edge (accepted for v1):** the strip decision is cached with the dynamic model, so an entity
instantiated in a non-stencil primary view then re-seen in a *stencil subview* the same frame casts a
wrong stencil volume — unreachable with shadow mapping on (preset default); failure is a wrong shadow,
not a crash. Files: `Model.h` (`cpuSkinStripped`), `Model_local.h`/`Model_md5.cpp`
(`CalcBoundsFast` + joint-reach), `Interaction.cpp` (full-index lightTris + shared stencil helper),
`tr_light.cpp` (`r_gpuSkinStripCpu`, view flag, `R_ShadowMapSkipStencilBuild`, ambient early-out),
`tr_local.h` (externs).

### Phase 3 — GPU-driven culling *(Vulkan-only; biggest relief, most architecture)*

#### Phase 3.0 — the indirect-draw RHI primitive — ✅ SHIPPED (`feat/rhi-indirect-draw`, pending user A/B)
The seed the rest of Phase 3 writes into. `RHI::DrawIndexedIndirect(args, argsBuffer, argsOffset,
drawCount, stride, countBuffer, countOffset)` (no-op default, GL3 inherits it) + a Vulkan impl that
issues `vkCmdDrawIndexedIndirect` (or `vkCmdDrawIndexedIndirectCount` when `countBuffer` is set — the
GPU-count form the cull pass will use). `VulkanBackend::Draw` was refactored into a shared `BindForDraw`
(the ~200-line pipeline/descriptor/vertex-index bind) + thin `Draw`/`DrawIndexedIndirect`. `BU_STORAGE`
gains `INDIRECT` usage so a compute pass can write commands straight into a storage buffer. The
`drawIndirectCount` + `multiDrawIndirect` **features** are enabled at device creation when present (the
1.4 command floor does *not* imply them — an adversarial review caught this) and the count/multi paths
gate on the flags. Validated by `r_vkIndirectTest`: routes **every** indexed draw through the primitive
via a per-frame-in-flight indirect-command ring (mirrors the geometry rings), a pixel-identical
whole-scene A/B. Current draw site: `vkCmdDrawIndexed` at `VulkanBackend.cpp` (was `:5557`, now in
`BindForDraw`). *Additive, no-op default, OFF path byte-identical.*

#### Phase 3.1 — the cull compute pass — ✅ VALIDATED PRIMITIVE (`r_gpuCullTest`, user-verified PASS)
The GPU frustum-cull kernel (`cs_gpucull`, `tr_main.cpp`) is built and validated in isolation against the
CPU `R_CullLocalBox`, the standalone-primitive-first pattern Phases 1/2 and deform-once each used.
`r_gpuCullTest` (VK, once/sec, no draw): a deterministic synthetic object set is culled on the GPU —
8 local AABB corners transformed by the id column-major `modelMatrix` (`mat4 * vec4(p,1)` == `R_LocalPointToGlobal`,
no transpose), radius + corner reject with the exact `>=0`-is-outside sign convention and `r_useCulling`
gating — and atomic-compacted into a `VkDrawIndexedIndirectCommand[]` + count. The harness diffs survivor
**sets** against the actual renderer cull (so a wrong kernel can only FAIL), buckets straddle-box FP noise
vs genuine divergences. User-verified `PASS` (2048 objs, CPU vis == GPU vis, 0 mismatch) across view angles.
**Proves:** per-object table upload, cull-math parity, atomic compaction into the indirect buffer.

#### Phase 3.2a — cull the *live* surface set (real data, no draw) — ✅ SHIPPED + USER-VERIFIED (`feat/gpu-cull-live` → HEAD `97bb4cd6`, `r_gpuCullLive`; PASS on mars_city1, 0-defect adversarial review)
The Phase 3.1 kernel is now fed the **real per-frame surface set** instead of synthetic boxes, proving the
GPU cull reproduces the shipping `R_CullLocalBox` on live geometry — dynamic/animated bounds, parented
transforms, the constrained view frustum. The front-end (`R_AddAmbientDrawsurfs`, `tr_light.cpp`) records
every ambient-cull candidate — real `tri->bounds`, real `vEntity->modelMatrix`, real `tri->numIndexes`, and
the actual CPU decision — into a grow-only collector, armed at most once/sec (`R_GpuCull_ResetLive` /
`R_GpuCullLiveActive` / `R_GpuCull_RecordCandidate`, `tr_local.h`). `R_GpuCullLive` (`tr_main.cpp`, hooked
after `R_AddModelSurfaces`) replays that set through `cs_gpucull` and diffs survivor **sets** vs the CPU
decisions via the shared `R_GpuCull_RunAndReport` helper (same boundary-FP bucketing as 3.1). This de-risks
everything downstream: it confirms the GPU decision is trustworthy on real scenes *before* anything renders
from it, and starts flowing the real `numIndexes` into the draw-params (`dp.x`). Only the ambient cull site
is instrumented; the light-interaction (`Interaction.cpp:1153`) and prelight-shadow (`:1118`) sites are the
same recorder if wanted. Still no draw; still `DispatchSync` (dev-only). GL3 self-gates off.

#### Phase 3.2b — consume the cull output via indirect draw (blocked on a prerequisite)
The remaining half — actually *drawing* from the GPU-culled command buffer — is gated by a missing
primitive the live-path recon surfaced: **there is no unified geometry buffer.** `RB_RHI_StreamAmbient`
(`RhiWorld.cpp:1487`) hands back a *different* `(vertexBuffer, indexBuffer)` per surface (per-surface
`ambientCache` / ring-streamed), but `vkCmdDrawIndexedIndirect` binds *one* vb/ib for the whole multi-draw.
So a real indirect batch first needs all batched geometry in one shared vb/ib addressed by
`firstIndex`/`vertexOffset`. On top of that, per-object state varies mid-batch and must move into an indexed
SSBO or partition the batch: **tessellate flag** (perforated skips it, opaque may not — `RhiWorld.cpp:1492`),
**cull type** (mirror views), **scissor**, **weapon/model depth-hack**, **polygon offset**, and **perforated
multi-stage** (alpha-test loops per stage with per-stage textures). And the per-surface `RenderParams` UBO
(MVP/color/alphaTest) must become an SSBO indexed by `gl_BaseInstance`/`firstInstance`. Net: 3.2b is a real
subproject — (1) unified geometry buffer, (2) per-object SSBO + zfill shader variant that indexes it,
(3) the `COMPUTE→DRAW_INDIRECT` barrier on `Dispatch` (`VulkanBackend.cpp:3273–3274` — widen `dstStageMask`
with `VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT` and `dstAccessMask` with `VK_ACCESS_INDIRECT_COMMAND_READ_BIT`;
`Draw`/`Dispatch` already share `frames[frameIndex].cb`, so no submit/fence is needed between them), (4)
`DrawIndexedIndirect` per pipeline/material bucket. Start with the depth prepass (one shader, no material
textures) over the world-static batch. GL3 keeps the CPU cull permanently. **Payoff:** attacks the per-frame
`R_CullLocalBox` sweep + the scalar draw loop — but only raises fps when CPU-bound (weak GPU / high entity
counts); on the RTX 3080 Ti the frame is GPU-bound, so this is architecture + CPU-bound-case relief, not fps
here. **Risk:** high (persistent residency for a frame-arena renderer; portal visibility is genuinely
data-dependent → a *hybrid*: CPU portal-area coarse pass feeds the GPU fine cull). Retires **one** of the
three CPU-position-skin pins (light cull, `Interaction.cpp:130/405`) once the light-interaction cull also
moves to the GPU. **Design references:** the (1)+(2) prerequisites here — one unified GPU-addressable
geometry buffer plus a per-object SSBO indexed per draw, replacing the per-surface vb/ib + `RenderParams`
UBO — are precisely the **bindless + Buffer Device Address** pattern surveyed in
[vulkan-backend.md](vulkan-backend.md) § "References" (zeux's descriptor-set ladder, "Modern Vulkan in
2025"). Read those before scoping this; they are the coherent way through the blocker, not a drop-in.

##### Modern-Vulkan path (BDA + manual vertex fetch) — dissolves the unified-buffer blocker
The "one unified geometry buffer" prerequisite above is the **legacy** framing (fixed-function
`vkCmdBindVertexBuffers` forces every sub-draw of a multi-draw to share one bound vb). The modern
path removes that constraint instead of paying it:
- **Vertex data via Buffer Device Address.** Keep the per-surface `ambientCache`/ring buffers exactly
  as they are — *no persistent unified buffer, no residency refactor of the frame-arena.* Publish each
  surface's **GPU address** (+ base vertex offset, stride) into a per-object SSBO entry. The zfill/gbuffer
  vertex shader does **manual vertex fetch** from that pointer (`GL_EXT_buffer_reference`), keyed by
  `gl_VertexIndex` + the per-draw base — so *no vertex buffer is bound at all*, and the single-bind
  constraint that blocked 3.2b simply doesn't apply. This is zeux's "manual vertex fetch from a unified
  buffer" generalized to a per-draw pointer.
- **Indices:** `vkCmdDrawIndexedIndirect` still reads one *bound* index buffer via `firstIndex`. Simplest
  hybrid — funnel indices through the existing per-frame **index ring** (`idxRing`, indices are tiny) and
  address them with `firstIndex`; vertices come from BDA. (Or go non-indexed and fetch indices via BDA too;
  the ring is less work.)
- **Per-object params:** the per-surface `RenderParams` UBO becomes an SSBO indexed by
  `gl_BaseInstance`/`firstInstance` (`gl_DrawID` under `drawIndirectCount`) — the same SSBO that carries the
  BDA pointers. **Bindless textures** (`VK_EXT_descriptor_indexing`, core 1.2) fold the material samplers
  into one global set indexed by a per-object material id, so multi-stage/perforated materials stop forcing
  a re-bind mid-batch.
- **Cost to *enable*** is small and already de-risked (see § "References" audit, 2026-08-14): `bufferDeviceAddress`
  is ~1 line on the already-chained `enabled12` struct (`VulkanBackend.cpp:1064`, right beside
  `drawIndirectCount`), + `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` on the allocator (`:1085`), +
  `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` on the addressed buffers (`CreateBuffer :2741`), + a
  `vkGetBufferDeviceAddress` wrapper in the RHI. SPIR-V/shaderc **already** target `vulkan1.4`, so no
  compiler bump; the GLSL just needs `#extension GL_EXT_buffer_reference`.

**What BDA does NOT solve** (still real 3.2b work): the *state* partitioning — tess flag, cull type (mirror
views), scissor, weapon/model depth-hack, polygon offset — still has to bucket the multi-draw by pipeline
(one `DrawIndexedIndirect` batch per pipeline/state bucket). BDA + bindless fix **geometry addressing and
per-object data**, i.e. items (1)+(2) of the blocker; item (3) the `COMPUTE→DRAW_INDIRECT` barrier and the
bucketing remain. Still VK-only, still fps-neutral on GPU-bound HW — architecture + CPU-bound-case relief.

**Recommended seeding (matches the r_vkIndirectTest / r_gpuCullTest methodology):** land a small, isolated,
headlessly-verifiable **BDA RHI primitive** first — enable the feature, add `GetBufferDeviceAddress`, and a
compute test that reads a buffer through its pointer and reports the sum back (numeric readback, like the
cull test) — *then* build the depth-prepass consume on proven plumbing. The depth prepass is the right first
consumer: one pipeline (no state buckets), no material textures, over the world-static batch.

**Status — BDA primitive ✅ USER-VERIFIED PASS (`r_vkBdaTest`, `feat/gpu-skin-cpu-unpin`, 2026-08-14).** Wired the
whole path: `bufferDeviceAddress` enabled on the device (gated on the device reporting it —
`haveBufferDeviceAddress`), the `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` allocator flag +
`VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` on `BU_STORAGE` buffers (both gated on the same flag),
`RHI::GetBufferDeviceAddress` (VK returns `vkGetBufferDeviceAddress`; GL3 returns 0), and a compute self-test
that seeds `0..N-1`, queries the buffer's address, and dispatches a kernel that sums N uints read **through the
raw pointer** (`GL_EXT_buffer_reference`, not a bound descriptor) into a separate SSBO, verifying `== N·(N-1)/2`.
Proves feature + address query + shader deref end to end. Files: `VulkanBackend.cpp` (`CreateDeviceAndVma`
feature/VMA, `CreateBuffer` usage, `GetBufferDeviceAddress` + `BdaSelfTest` by `ComputeSelfTest`), `RHI.h`.
No consumer yet — the depth-prepass consume is the next step. VK-only; SPIR-V already targets 1.4 so no
compiler bump was needed. **Verified:** in-engine `r_vkBdaTest 1` → `VK BDA self-test: PASS` (2026-08-14).

**Consume — Increment 1 ✅ USER-VERIFIED (`r_vkBdaZfill`, `feat/gpu-skin-cpu-unpin`, 2026-08-15).** The flat
world-static depth prepass now optionally fetches vertex positions from the buffer's **device address**
(`zfill_bda.vert`, `GL_EXT_buffer_reference`) instead of bound attributes — per-draw, keeping the bound index
buffer + UBO MVP. Self-contained in `VulkanBackend::Draw` (same idiom as `r_vkIndirectTest`): gated on the flat
zfill shader over an addressable persistent `BU_VERTEX` buffer, so streamed/skinned/tessellated surfaces
report address 0 and fall back to the normal draw. **Pixel-identical by construction** — same position bytes,
same `invariant u_mvpMatrix * vec4(pos,1)`, so the depth buffer is bit-identical and the whole frame with it.
Enablers: `SHADER_DEVICE_ADDRESS` extended to `BU_VERTEX` + per-buffer addresses cached at creation
(`bufferAddr`), a 16-byte vertex push-constant range on the graphics layout, and `#extension
GL_EXT_buffer_reference` in `prelude.vk.glsl` (SPIR-V-verified inert for every non-BDA shader; `zfill_bda`
compiles to `PhysicalStorageBuffer64`). **Next — Increment 2:** move address+MVP into a per-object SSBO indexed
by `firstInstance`; **Increment 3:** batch the simple opaque bucket (no depth-hack/scissor/poly-offset/tess/
subview) into one `vkCmdDrawIndexedIndirect` + the `COMPUTE→DRAW_INDIRECT` barrier. **To verify:** toggle
`r_vkBdaZfill 1` vs `0` in-world — identical image; watch world surfaces for any z-fighting or holes.
Verified 2026-08-15 on mars_city1: image identical, `VK BDA zfill: 164 draws via device address, 0 fell
back` (count tracked visible geometry as the view moved). The BDA vertex-fetch path is proven correct on live
world-static geometry — the remaining increments only change *how draws are grouped/dispatched*, not the fetch.

**Consume — Increment 2 ✅ USER-VERIFIED (`r_vkBdaZfill 2`, `feat/gpu-skin-cpu-unpin`, 2026-08-15).** The actual
GPU-driven draw: the solid-opaque depth-prepass bucket is collected in `RB_RHI_FillDepthBuffer` and drawn with
**non-indexed `vkCmdDrawIndirect`, one per distinct scissor group** (measured 3–5 draws for ~130–150 surfaces on
mars_city1). Fully bindless — each surface's vertices *and* indices are fetched through device-address pointers
(`zfill_batch.vert`, two `buffer_reference` types), so surfaces with different vb/ib batch together with no bound
geometry. Per-object `{vbAddr, ibAddr, mvp}` lives in a per-frame SSBO indexed by `gl_InstanceIndex` (= each
command's `firstInstance`; needs `drawIndirectFirstInstance`, enabled). Batchable predicate: MC_OPAQUE,
non-subview, no depth-hack/polygon-offset/tessellation, front-sided non-clip view, vb+ib BDA-addressable;
everything else stays per-surface (mode-1 BDA or normal). Pixel-identical by construction (same MVP bytes from
`RB_RHI_SpaceMvp`, same `invariant mvp*vec4(pos,1)` → bit-identical depth; color `{0,0,0,1}`, LESS, SS_ALWAYS,
CT_FRONT_SIDED all match). New infra: `VL_NONE` (empty vertex input), double-buffered `BU_STORAGE` batch buffers,
`RHI::DrawZfillBatch`(items + scissor `groups`)/`ZfillBatchEnabled`, `ApplyDynState` factored out of `BindForDraw`.

**Three bring-up bugs found + fixed (were why it silently fell back / blinked):** (1) **index buffers weren't
address-capable** — `SHADER_DEVICE_ADDRESS` had been added to `BU_STORAGE`/`BU_VERTEX` but not `BU_INDEX`, so
the fully-bindless index fetch got `ibAddr==0` and every surface fell to per-draw; (2) **per-entity scissors** —
`surf->scissorRect` is the entity's screen-bounds rect (never exactly the view rect), so the first exact-match
predicate collected nothing; fixed by **grouping surfaces by scissor** and drawing one indirect call per group
(surfaces of an entity/BSP share a scissor → 3–5 groups, not 130); (3) **multi-draw buffer clobber** — issuing a
`DrawZfillBatch` per group re-wrote the *same* SSBO between draws that only execute at submit, so every group read
the last group's data → surfaces blinked to black; fixed by **uploading all items once** and drawing per-group
over sub-ranges (`firstInstance` = global index). **Adversarial review (pre-bugs): 0 defects on the std430/index
math/lifetime/pixel-identity.** **Phase 3.2b (the batched GPU-driven DRAW) is DONE and banked here.**

#### Increment 3 (GPU-driven CULL for the prepass) — ❌ STRUCK (2026-08-15, recon-confirmed)
**Do not build (as a prepass cull).** Two recons confirmed everything to wire a GPU cull→indirect-draw
already exists — the `GPUCULL_SRC` kernel (`tr_main.cpp:713`) reproduces `R_CullLocalBox` and atomic-compacts
`VkDrawIndexedIndirectCommand[]` + count; `R_GpuCull_RecordCandidate` (`tr_light.cpp:2264`) records candidates
at the cull site; `vkCmdDrawIndexedIndirectCount` + `drawIndirectCount` are wired; only a one-line
`COMPUTE→DRAW_INDIRECT` barrier widening (`VulkanBackend.cpp` Dispatch: add `VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT`
+ `VK_ACCESS_INDIRECT_COMMAND_READ_BIT`) is missing. **But it's architecturally redundant:** `R_CullLocalBox`
runs inside `R_AddAmbientDrawsurfs` to build `viewDef->drawSurfs`, the list **every** pass consumes (ambient,
interactions, shadows) — so a prepass GPU cull re-culls what the CPU already culled for those passes = **zero
cull relief**. Retiring the CPU cull requires making *every* pass consume a GPU-produced list and dropping
`drawSurfs` (no readback anywhere) — the whole-renderer rewrite below. The GPU-driven-cull endgame is
**ray-query**, not this. The `r_gpuCullLive` primitive already banked the "validated GPU cull" milestone.

#### Nice to have (future) — "refactor renderer in GPU" (full GPU-driven path)
The genuinely-valuable version of GPU-driven culling: **every** pass (prepass, ambient, interaction, shadow)
consumes a GPU-produced draw list instead of the CPU `drawSurfs`, so the CPU does only the coarse area-level
portal flood (data-dependent, must stay CPU) and hands the GPU per-area static candidate ranges; the GPU
frustum-culls + drives all draws, no GPU→CPU readback. **Enormous, high-risk (touches lighting/shadows/
interactions), fps-neutral on current HW — not worth it for raster gains alone.** *Revisit at the RTX pivot:*
ray-query needs geometry in GPU-addressable form (BLAS build + ray pipeline), and Increment 2's **BDA geometry
addressing is already that groundwork** — a full GPU-driven path (GPU decides visibility → drives both raster
*and* ray-query) is a real RTX enabler. So: parked as a deliberate RTX-era item, not a dead end. All the
raster pieces (cull kernel, BDA geometry, indirect-count draw, the barrier one-liner) are in place to build on.

### Phase 4 — GPU shadow-volume generation — ❌ STRUCK (2026-08-10, recon-confirmed)
**Do not build.** A recon of the residual stencil cost after Phase 0 concluded a GPU stencil-volume
builder is not worth it and is a *dead-end vs ray-query*: (1) Phase 0 already removed ~100% indoor /
70–80% outdoor of the build; the residual fires only for oversize "sun" lights and is dominated by
*static* world casters (cacheable CPU-side, not a GPU job). (2) The machine is GPU-bound at realistic
counts (Milestone C), so moving the build to the GPU relieves a non-bottleneck *and* adds GPU load —
potentially net-negative fps. (3) The forward direction (ray-query shadows) **deletes** the stencil
consumer entirely, so a GPU stencil builder is throwaway code. It also needs indirect draw (Phase 3.0,
now shipped) to avoid a readback stall — but there is no independent reason to spend it here. The
stencil shadow-volume pin on the CPU position-skin (`tr_light.cpp:244`, `Interaction.cpp:947`) is
retired by **ray-query**, not by this phase.

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
| Indirect draw (`DrawIndexedIndirect`) | 3.0 | ❌ (no-op) | ✅ SHIPPED | `RHI.h` `DrawIndexedIndirect`; VK `BindForDraw`+`vkCmdDrawIndexedIndirect[Count]`; `r_vkIndirectTest` |
| Buffer device address (`GetBufferDeviceAddress`) | 3.2b | ❌ (returns 0) | ✅ VERIFIED (`r_vkBdaTest`) | `RHI.h` `GetBufferDeviceAddress`; VK device feature + VMA `BUFFER_DEVICE_ADDRESS` flag + `SHADER_DEVICE_ADDRESS` usage on `BU_STORAGE`; `vkGetBufferDeviceAddress` |

VK enablers already in place: Vulkan 1.4 floor (all core compute guaranteed, no extension gating),
runtime shaderc compiler, VMA, a timestamp-query idiom to measure any new pass. The `queues[]` array
is fixed `[2]` and scans graphics-only (`:842–863,912`) — fine for graphics-queue compute; a *dedicated
async-compute* queue (not needed until Phase 3 overlap tuning) would extend both.

---

## 7. Sequencing & dependencies

```
Phase 0 (CPU stencil-build gate) ── ✅ SHIPPED (0bf7e1dd)
Phase 1 (VK compute lane + BU_STORAGE) ── ✅ SHIPPED (cf18e615)
Phase 2 (GPU skinning, VK) ── ✅ SHIPPED (gpuSkinVB; Milestone C audited what it can retire)
   └── Phase 3.0 (indirect-draw primitive) ── ✅ SHIPPED (feat/rhi-indirect-draw)
        └── Phase 3.1 (cull kernel, synthetic validation) ── ✅ SHIPPED (r_gpuCullTest)
             └── Phase 3.2a (cull the live surface set, no draw) ── ✅ SHIPPED + USER-VERIFIED (97bb4cd6, r_gpuCullLive)
                  └── Phase 3.2b (batched indirect DRAW via BDA) ── ✅ DONE + USER-VERIFIED (r_vkBdaZfill 2; BDA dissolved
                       the "unified geometry buffer" blocker — no net-new buffer needed; verts+indices via device address)
                       └── Increment 3 (GPU-driven CULL for the prepass) ── ❌ STRUCK (redundant w/ shared drawSurfs;
                            real version = "refactor renderer in GPU", parked as an RTX-era nice-to-have)
Phase 4 (GPU shadow-volume gen) ── ❌ STRUCK (subsumed by ray-query)
Ray-query shadows (RTX pivot, after culling) ── retires pin #2 (stencil volumes); changes pixels (opt-in)
```

**Decision (2026-08-10): sequence culling now → ray-query later.** GPU-driven culling (Phase 3)
first — the indirect-draw seed is in — then the RTX/ray-query pivot that subsumes Phase 4. This keeps
both of the removable CPU-skin pins on a retirement path.

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
