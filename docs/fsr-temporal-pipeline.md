# R1 — the FSR2 temporal pipeline (motion vectors + jitter + FSR2 Native-AA)

Status: **planned (implementation plan)**. This is roadmap item **R1** from
[rtx-shadow-roadmap.md](rtx-shadow-roadmap.md) — the temporal pipeline that supersedes the
hand-rolled-TAA plan in [antialiasing.md](antialiasing.md). VK-only for the FSR2 dispatch;
motion vectors + jitter benefit both RHI backends. Legacy ARB is never touched. Every
increment is cvar-gated so **OFF == today's renderer, bit-for-bit**.

Decision record + prior context: the `fsr-temporal-pipeline` memory, `render-interpolation-feature`
(why frame-gen is parked), `antialiasing-plan`, `gpu-perf-heavy-pass-profile`.

---

## What R1 is, and what already exists

FSR2 in **Native-AA mode** (render scale 1.0) is a production-grade TAA: temporal
accumulation + RCAS sharpening that resolves the specular / normal-map **shimmer** SMAA
structurally cannot touch (Doom 3's signature aliasing). It needs three inputs the engine
does not produce today: **per-object motion vectors**, **sub-pixel projection jitter**, and a
**reordered post chain** (scene at render-res → FSR2 → grain/chroma/HUD at display-res).

The roadmap's "~80% of a TAA resolve already exists" is **about half true**. Verified against
current code:

- **Exists:** a proven **camera-only** reprojection + ping-pong history + prev-view-proj
  bookkeeping — but *duplicated* across the temporal-SSAO and temporal-SSR resolves
  (`rhiSsaoPrevViewProj` [RhiWorld.cpp:361](../neo/renderer/rhi/RhiWorld.cpp#L361),
  `rhiSsrPrevViewProj` :375). Reproj recipe `inv(curView)*prevViewProj` at :4788 / :4449.
- **Absent:** per-object motion vectors, a velocity render target, prev-**model** tracking,
  sub-pixel jitter, any discontinuity / history-reset signal, and all FSR2 / compute-image
  plumbing.

So R1 builds the missing temporal *inputs*, reuses the existing temporal *machinery* as both a
consumer and a validator, then vendors FSR2 on top.

## Two decisions settled by the evidence

1. **Vendor the standalone GPUOpen FSR2 repo (MIT) and drive it via its own `ffx_fsr2_vk`
   backend** — handed raw `VkDevice`/queue/command-buffer. This sidesteps a blocking gap: the
   DUDE RHI compute lane is **SSBO-only** (zero storage images,
   [VulkanBackend.cpp:2390](../neo/renderer/rhi/vk/VulkanBackend.cpp#L2390)-2401). FSR2's VK
   backend owns its own storage images / pipelines / barriers, so we don't re-plumb the RHI
   compute lane. MIT is GPLv3-compatible (permissive → satisfies [publishing.md](publishing.md)).
2. **Backend split.** Motion vectors + jitter gate on `R_BackendSupportsEnhancements()` (both
   RHI backends); the **per-object velocity buffer and the FSR2 dispatch are VK-only.** GL3 has
   **no float-color RT path at all** — `CreateRenderTargetColorDepth` hard-rejects non-RGBA8
   ([GL3Backend.cpp:748](../neo/renderer/rhi/GL3Backend.cpp#L748)) and caps `colorCount` at 2
   (:752). So GL3 keeps the **camera-only** reprojection upgrade (A1); VK gets true per-object
   MVs feeding FSR2 *and* its own temporal-SSAO/SSR. "MVs benefit both backends" is true only
   for the camera half on GL3 — stated plainly so it isn't over-promised.

## Verified engine-convention facts that drive the design

These were confirmed against local code and each one changes the plan (several were caught by
the adversarial critique pass, not the first draft):

| Fact | Evidence | Consequence |
|---|---|---|
| Depth is **non-reversed-z, infinite-far** | `projectionMatrix[10]=-0.999`, `[14]=-2*zNear` at [tr_main.cpp:1341](../neo/renderer/tr_main.cpp#L1341)-1352 | FSR2 must set `DEPTH_INFINITE`, must **NOT** set `DEPTH_INVERTED`. Wrong flag corrupts disocclusion. |
| Legacy `r_jitter` writes **off-diagonal shear** into the projection | adds jitter to both `xmin`/`xmax` → `projectionMatrix[8]/[9]` non-zero (tr_main.cpp:1325-1338) | The shared prev-VP must store an **explicitly un-jittered** projection, or every temporal consumer smears the frame jitter turns on. Assert `[8]==[9]==0` for a symmetric frustum. |
| **No tonemap operator exists** | `hdrresolve.frag` is a straight RGBA16F→RGBA8 passthrough + optional grain/chroma/gamma; [RhiBackend.cpp:744](../neo/renderer/rhi/RhiBackend.cpp#L744) "Phase B folds exposure+tonemap" is unimplemented | The "reorder around tonemap" is really only grain/chroma/gamma. FSR2 receives **un-exposed linear HDR** → `AUTO_EXPOSURE` must be validated against D3's raw range early; fallback = a 1×1 exposure of 1.0. |
| Forcing the gbuffer prepass on for MV **replaces zfill and seals scene depth** | `mergedDepth` fork at [RhiWorld.cpp:4930](../neo/renderer/rhi/RhiWorld.cpp#L4930)-4935 | It **must** reuse `RB_RHI_TessBumpForZfill` (:816) so displaced/perforated/alpha-tested depth is bit-identical, or surfaces drop under `DEPTHFUNC_EQUAL` (the ember-tess bug class). |
| The MRT cap is a **fixed-size pass class**, not an array to widen | `PassClassFor` has no ≥3 / RG16F case; `colorRefs[2]`/`atts[3]` fixed (VulkanBackend.cpp:5284-5306) | A 3rd attachment needs a **new pass class** that cannot alias class 6, or the shipping 2-attachment SSR normal prepass silently gets the wrong render pass. |
| Only `VK_KHR_swapchain` is enabled today | [VulkanBackend.cpp:1045](../neo/renderer/rhi/vk/VulkanBackend.cpp#L1045); the VK 1.4 floor guarantees the API version, **not** optional features | FSR2's required features (subgroup ops, maybe `shaderFloat16`/`16bit_storage`) must be enabled **conditionally**; if absent, `r_fsr` is a hard no-op. |

**The #1 documented FSR2 integration bug is the motion-vector convention** (current→previous
direction; FSR2 wants top-left / +Y-down origin, D3 is +Y-up; jitter excluded). A wrong sign
smears the whole image on a camera pan. Mitigation baked into the plan: store **one canonical
velocity** (GL +Y-up UV-delta, no Y flip baked) so the SSAO/SSR consumers read it unchanged,
and apply the FSR2 top-left flip **in the dispatch wrapper** (`motionVectorScale.y` negative),
not in `gbuffer.frag`. `r_mvDebug` + an empirical pan test are the ground-truth gate before
FSR2 is ever fed.

---

## Increments

Each is independently shippable, cvar/self-test-gated, with a validator that enforces
`OFF == today` bit-for-bit (FSR2 *on* is temporal/non-deterministic → user-screenshot sign-off
per the fidelity policy).

| ID | Title | Size | Backend | Ships value alone? | Depends on |
|---|---|---|---|---|---|
| **A0** | MRT/format plumbing: `IF_RG16F` + 3rd attachment as a distinct pass class (no velocity yet) | M | VK | de-risks structure | — |
| **A1** | Shared **un-jittered** prev-view-proj + discontinuity / history-reset signal | M | both RHI | yes (correct history invalidation on cuts) | — |
| **A2** | Per-object velocity: prev-**model** cache + velocity MRT + gbuffer shaders | L | VK (GL3 camera-only) | yes (less SSAO/SSR ghosting) | A0, A1 |
| **B** | Sub-pixel Halton(2,3) projection jitter through `R_SetupProjection` | S | both RHI | with a temporal consumer | A1 |
| **C0** | FSR2 vendoring + shader-toolchain spike + **conditional** device features (build only) | M | VK | de-risks the top unknown | — |
| **C1** | Pipeline reorder as a **bit-identical passthrough** (scene/HUD split, `r_fsr` absent) | L | VK | proves the OFF path | C0 |
| **C2** | FSR2 context/dispatch in Native-AA over the C1 hook | L | VK | **the payoff** | A2, B, C1 |
| **D** | Reactive + transparency/composition masks (additive particles, weapon) | L | VK | the real quality work | C2 |
| **E** | Preset + menu wiring (Native-AA on the top VK tier) | M | VK | ships to users | C2 |

### A0 — MRT/format plumbing (VK, self-test only)
Land the riskiest structural change in isolation: a new `IF_RG16F` (→ `VK_FORMAT_R16G16_SFLOAT`)
and a **3rd color attachment as a distinct VK pass class** that cannot alias class 6. No velocity
emitted, no shader delta, no frontend change. An init-time smoke test creates a
3-MRT+depth target so a driver that rejects RG16F-as-color fails **loudly at init**, not silently
mid-frame. **Validator `r_mrt3Test`** asserts the existing normal(@0)/SSR(@1) attachments are
bit-identical to the 2-attachment path. Touch points: `RHI.h:46-52`, `VulkanBackend.cpp`
`PassClassFor`:5284 / `BuildColorPasses`:5298 / `CreateColorTarget`:5374 / framebuffer+clear
arrays :5481,:5102.

### A1 — shared un-jittered prev-VP + reset signal (both RHI)  **[built + review-verified, pending in-game A/B — branch `feat/temporal-prevvp-reset`]**
Collapse the two duplicated prev-view-proj file-statics into **one** shared per-frame struct
`{ prevViewProj, prevProjection, havePrev, historyReset }` storing an **un-jittered** projection.
Add the **discontinuity detector that does not exist today** (SSAO/SSR history only invalidates on
realloc/`vid_restart`, never on camera cuts). Source it from the game's `renderViewInterpolatable`
snap-on-cut flag ([Player.cpp:7394](../neo/game/Player.cpp#L7394)/7408) threaded onto `renderView`,
backstopped by a pose-delta threshold. Ships standalone value on **both** backends: correct
history invalidation on cuts (today's clamp only *hides* wrong cross-cut history — FSR2 won't).
**Validator:** with `r_jitter 0` + no MV, this is a pure refactor → SSAO/SSR output bit-identical;
camera cut fires `historyReset`; stored projection has `[8]==[9]==0`.

### A2 — per-object velocity (VK; GL3 stays camera-only)
The largest net-new frontend work. A persistent **prev-frame per-object matrix cache** (static
`idHashIndex` keyed by `entityDef->index` — `viewEntity_t` is frame-temporary, so copy
`modelViewMatrix` out each frame, evict despawned, discriminate mirror/subview duplicates).
`prevMvpMatrix` computed through `RB_RHI_SpaceMvp` (so the VK z-remap/Y-flip convention is
identical for cur and prev) — **preferably via a separate small UBO/push path for MV-emitting
passes only**, to avoid charging +64 B to every one of the thousands of non-MV draws the
`gl3-perf-ubo-stall` work tuned. Velocity is the **RG16F 3rd MRT** on `RB_RHI_NormalPrepass`
using A0's pass class; `gbuffer.{vert,frag,tese}` write `MV = currUV − prevUV` in the **one
canonical +Y-up convention**, zero for `weaponDepthHack` surfaces, never for `viewEntitys==NULL`
(HUD/2D). Forcing the prepass on **must** reuse `RB_RHI_TessBumpForZfill` (depth-EQUAL contract).
Feeds VK temporal SSAO (:4772) and SSR (:4441). **Validators (`r_motionVectors`, `r_mvDebug`):**
static camera → velocity ≈ 0 (hard assert); moving camera over static geo → matches A1
camera-only reproj within a perceptual threshold (not bit-exact — RG16F quantization); moving
objects → strictly less ghosting; **MV-on/SSAO-off depth == zfill depth bit-for-bit** before any
interaction pass is trusted; `r_vkBdaZfill 1` vs `2` leave velocity identical.

### B — sub-pixel Halton jitter (both RHI)
Swap the `idRandom` source in the **existing** `R_SetupProjection` jitter block
(tr_main.cpp:1300-1329) for `ffxFsr2GetJitterOffset` (Halton(2,3), phase 8 at scale 1.0), indexed
by **`tr.frameCount`** (per rendered frame — correct for `com_interpolate`, **not** per-tic, or
multiple interpolated frames share one offset and lose coverage). Store the applied pixel-space
jitter on `viewDef` as the single source handed to `FSR2.jitterOffset`. Retire the legacy
`r_jitter` as a **separate, explicitly validated** step so the two OFF conditions can't mask each
other. Skip jitter on subview/mirror/xray/env-probe/screenshot and the 2D/HUD ortho path.
**Validator:** `r_temporalJitter 0` AND `r_jitter 0` → projection matrix bit-identical
(dump+diff 16 floats).

### C0 — FSR2 vendoring + toolchain spike + conditional features (VK, build only)  **[DONE + user-verified 2026-08-16]**

**Closed:** `r_fsr2Test` PASS on the RTX 3080 Ti — `FSR2 self-test: PASS - context created
(Native-AA 2560x1440, scratch 545 KB)`. FSR2 links, sizes its scratch, builds its VK interface
against DUDE's device, and creates a full FSR2 context (all compute pipelines) with the
conditionally-enabled fp16 / 16-bit-storage features. Vendoring committed `43b41452`; smoke test
+ device features in the following commit.


**Toolchain RESOLVED (positive):** FSR2's Vulkan backend is **GLSL→SPIR-V via glslang**, not
HLSL/DXC — `vk/CMakeLists.txt` compiles `shaders/ffx_fsr2_*_pass.glsl` with
`-compiler=glslang --target-env vulkan1.1 -S comp -DFFX_GLSL=1` into `*_permutations.h`
SPIR-V byte-array headers that `ffx_fsr2_shaders_vk.cpp` `#include`s. MIT, pure compute, no
vendor blob, no runtime-shaderc dependency. So DUDE vendors *prebuilt* headers + the MIT C++
API and compiles them with **no new build-time shader step**. (The "GLSL port needed?" worry is
moot — already GLSL.) glslang compiles the shaders to valid SPIR-V on the dev box.

**Open sub-task — how to generate the ~8 permutation headers (one-time, then vendored):** the
generator (`FidelityFX_SC`) ships Windows-only; under `wine` it compiles every permutation to
valid `.spv` but the final reflection/header-aggregation step fails (exit 3, silent). Native
FFX_SC source not readily fetchable; `spirv-cross`/`spirv-reflect` not installed (but
`spirv-dis`/`glslangValidator` are). Header format (from `ffx_fsr2_shaders_vk.cpp`): per pass a
`PermutationKey` bitfield → `IndirectionTable[]` (dedup) → `PermutationInfo[]` of
`{blobData, blobSize, num{Storage,Sampled}ImageResources, numUniformBufferResources,
name/binding tables}`. Candidate paths: (A) native generator script (glslangValidator +
SPIR-V reflection → emit FFX_SC's header format); (B) fix FFX_SC under wine; (C) locate/build
native FFX_SC; (D) source pre-generated headers.

**RESOLVED → path A, built + proven.** Wine FFX_SC is a dead end (`err:seh:check_noexcept` —
MSVC exception in a `noexcept` fn during reflection; unfixable under wine). Native generator
**[neo/libs/fsr2/gen_fsr2_vk_permutations.py]** replaces FidelityFX_SC for the glslang/VK path:
per pass it compiles every permutation with the system `glslangValidator` (exact FFX_SC args),
reflects bindings from `spirv-dis` (storage `rw_*` / sampled `r_*` / uniform `cb*`; immutable
samplers `s_*` excluded — the VK backend owns those as set-0 `pImmutableSamplers`), dedups
blobs, and emits the exact `PermutationKey` union + `IndirectionTable` + `PermutationInfo`
format (self-consistent little-endian bit packing). **Proven:** all 8 headers generate clean
(0 warnings; e.g. rcas 2 blobs, accumulate 48, luminance 1); `spirv-val` passes the blobs; and
the *real* upstream `ffx_fsr2_shaders_vk.cpp` compiles cleanly against them. **Upstream FSR2
Linux patches needed when vendoring** (MSVC-isms): `-DFFX_GCC` (empties `FFX_API __declspec`),
`#include <cstddef>` (`size_t`), plus core/backend `_countof`, `<cwchar>` (`wcscmp`),
`<codecvt>`/`<locale>` (`std::wstring_convert`). Remaining C0: vendor api+vk+headers into
neo/libs/fsr2/, apply patches, wire CMakeLists (VK-guarded, imgui-style inline compile),
VulkanBackend raw-handle accessors + conditional device features, `r_fsr2Test` link/create
smoke test.

Original plan text below.

De-risk the top unknown: FSR2 kernels are authored in **HLSL** and its stock pipeline
pre-compiles them, but DUDE has only a **shaderc GLSL→SPIR-V** runtime path. Resolve the shader
production route — (a) offline DXC→SPIR-V blob vendoring (one-time build dep), or (b) a
GLSL/Vulkan-native FSR2 port (consistent with the vendored-SMAA-GLSL precedent, but fidelity must
be validated vs upstream) — and prove one permutation loads. Vendor `src/ffx-fsr2-api/` + `vk/` +
`shaders/` into `neo/renderer/rhi/vk/fsr2/`, link `ffx_fsr2_api` + `ffx_fsr2_vk` into the VK
backend only, expose raw-handle accessors. **Enable FSR2's device features conditionally** — if
absent, `r_fsr` is a hard no-op (keep SMAA); device creation must be **unchanged** for users who
never set `r_fsr`. **Validator:** builds + links; `ffxFsr2ContextCreate` succeeds against the real
device (no dispatch); on a device *without* the required feature, device creation is unchanged and
`r_fsr` reports a no-op.

### C1 — pipeline reorder as a bit-identical passthrough (VK, `r_fsr` absent)
Land the biggest OFF-path risk — the **scene/HUD target split** — as a pure passthrough with **no
FSR2 dispatch**, so the deterministic structural change is bit-checkable separately from FSR2's
non-deterministic output. Route the 3D world to a scene-only `rhiHdrRT` (no HUD); route the
`!viewEntitys` HUD/console/letterbox/menu-over-game `RC_DRAW_VIEW`s to a display-res target
composited **after** the resolve. **Enumerate every `!viewEntitys` view kind** (the
console-over-game case is the likely miss). Gate the entire split behind `r_fsr` on the same
`FrameHasWorldScene` condition that owns `rhiHdrRT`; when `r_fsr 0` execute the **literal old
single-target flow**. New statics follow `RB_RHI_Shutdown` reset discipline (not
`RB_RHI_ResetWorldTargets`, which doesn't own the HDR statics) or `vid_restart` leaves stale
handles → white frames. **Hard-gate validator:** `r_fsr 0` + reorder active is bit-identical
across HUD+world / console-over-game / PDA-over-game / letterboxed-cinematic-with-live-world /
menu frames.

### C2 — FSR2 context/dispatch, Native-AA (VK)
The convergence point. Force the offscreen **RGBA16F** scene buffer on when `r_fsr` is on even if
`r_hdr` is off (FSR2 wants HDR). New `RunFsr2` records FSR2's dispatch onto the frame command
buffer **after the scene pass closes** (the RHI `Dispatch` refuses in-render-pass and records
pre-scene). Wrap inputs with `ffxGetTextureResourceVK`: color = `rhiHdrRT` (pre-tonemap RGBA16F),
depth = scene depth aspect, motionVectors = A2's RG16F, output = a new display-res compute-write
image. **Context flags (verified):** `HIGH_DYNAMIC_RANGE` set, `DEPTH_INFINITE` set,
`DEPTH_INVERTED` **unset**, `AUTO_EXPOSURE` set, `MOTION_VECTORS_JITTER_CANCELLATION` unset.
**Dispatch fields:** `motionVectorScale = {renderW, −renderH}` (the Y-flip to FSR2's top-left
origin happens **here**), `jitterOffset` = B's stored value, `frameTimeDelta` in **ms**,
`cameraFovAngleVertical` in **radians**, `preExposure` = 1.0, `reset` = A1's `historyReset`.
Bypass FXAA/SMAA when `r_fsr` is on (RCAS replaces them). Move grain/chroma/gamma to display res
over the FSR2 output. **Validators:** `r_fsr 0` bit-identical (C1 already proved the passthrough);
`r_fsr2Test` synthetic self-test; **empirical MV-sign pan test** (violent smear on pan ⇒ Y-sign
wrong — fix before trusting); early `AUTO_EXPOSURE` check on a bright/dark scene; shimmer A/B vs
SMAA → user screenshot sign-off.

### D — reactive + transparency/composition masks (VK)
The real integration quality work: kill temporal ghosting on Doom 3's **additive-blended**
content (muzzle flashes, plasma, particles, GUI screens, heat-haze/refraction) and the weapon
view-model. V1 shortcut = FSR2's **auto-reactive** (needs an opaque-only color copy). V2 =
hand-authored R8 reactive + transparency-and-composition masks, raising the mask where additive
stages draw in the blend pass. The weapon (worst MV offender) gets reactive/stencil treatment on
top of A2's velocity zeroing. **Validator:** particle-ghosting A/B on a plasma-fire scene; no
weapon smear during rapid strafing; user screenshot sign-off.

### E — preset + menu wiring (VK)
Keep `r_fsr` a **separate** toggle (do **not** overload `r_rhiAA`, which is shared with GL3 and
consumed unconditionally in ~8 RHI sites); when `r_fsr` is on and VK, FSR wins and short-circuits
every `r_rhiAA` post-AA site. Append `fsr`+`fsrQuality/renderScale` columns to `EnhancementPreset`
and every row (positional initializers at the end), wire Native-AA (`fsr=true, scale=1.0`) into
**`PRESET_NIGHTMARE` only** and set `r_rhiAA=0` in those rows (honest detection vector, no stacked
AA). Add matching epsilon compares in `DetectEnhancementPreset` or Nightmare reads back as
"Custom". The ImGui FSR control is greyed via `BeginDisabled(!(rhiBackend && !coreProfile))`,
labelled "FSR2 (Vulkan only)". **"Ultra Nightmare" stays doc-reserved** — keeping FSR inside
Nightmare avoids widening `dude_preset`'s max (Common.cpp:104) + `PRESET_COUNT` + two combo
strings. MV + jitter are **internal infrastructure**, not user-facing preset columns — auto-enabled
whenever FSR (VK) or temporal-SSAO/SSR (both backends) is active.

---

## Sequencing — two parallelizable tracks

```
TRACK 1 (both-RHI foundation, ships value without FSR2):
  A0 (VK MRT plumbing, r_mrt3Test) ─┐
  A1 (shared un-jittered prev-VP + reset, both backends) ─┼─► A2 (VK per-object velocity) ─► B (jitter, needs A1)
                                     │
TRACK 2 (VK FSR2, no code dep on Track 1 until C2):
  C0 (vendoring + toolchain spike + conditional features) ─► C1 (reorder passthrough, r_fsr absent)
                                                                              │
        A2 + B + C1 ────────────────────────────────────────────────────────►  C2 (FSR2 dispatch, Native-AA)
                                                                                   │
                                                                     ┌─────────────┴─────────────┐
                                                                     D (reactive masks)      E (preset/menu)
```

**Recommended landing order:** `A0, A1, C0` (parallel — no interdependencies) → `A2, B, C1`
(A2/C1 parallel once their deps land) → `C2` → `D, E` (parallel).

**Do not wire FSR into any preset (E) until C2 + D are user-verified**, per the fidelity policy
and the adversarial-review-before-trust house pattern.

## Global risks (condensed)

- **MV convention** (#1 bug) — one canonical velocity in A2, flip in the C2 dispatch, `r_mvDebug`
  + pan test gate before FSR2 is fed.
- **Depth flags** — `DEPTH_INFINITE` yes, `DEPTH_INVERTED` no (D3 is non-reversed-z).
- **Un-jittered projection capture** — A1 must strip the jitter shear (`[8]==[9]==0` assert).
- **Depth-EQUAL contract** — forced gbuffer prepass reuses `RB_RHI_TessBumpForZfill` or drops
  EQUAL surfaces.
- **New MRT pass class** must not alias class 6 (isolated in A0).
- **No tonemap** — FSR2 gets un-exposed linear HDR; validate `AUTO_EXPOSURE` early, 1×1
  exposure-of-1.0 fallback.
- **GL3 has no float-color RT** — per-object velocity is VK-only.
- **HLSL→SPIR-V** — resolved in the C0 spike; if neither route is viable, FSR2 is blocked and the
  fallback is SMAA (no regression).
- **Conditional device features** — enable only when present; `r_fsr` no-ops otherwise.
- **RHI compute is SSBO-only** — drive FSR2 via its own `ffx_fsr2_vk` backend.
- **GPU-skinned/tess MV error** — rigid prev-matrix only; documented v1 limitation.
- **`vid_restart` stale handles** — new statics follow `RB_RHI_Shutdown` discipline.
- **Legacy ARB untouched** — MV/jitter gate `R_BackendSupportsEnhancements()`; velocity + FSR2
  gate `BT_VULKAN`.

## Open questions to resolve in-flight

1. **Velocity host** — force the full 3-attachment gbuffer on (writes dead normal/material
   attachments when SSAO/SSR are off) vs a dedicated RG16F velocity-only pass. Decide by
   benchmarking in A2; A0's pass-class work is reusable either way.
2. **`prevMvpMatrix` storage** — separate small UBO/push path (preferred) vs widening the shared
   928-byte block (re-verify `UBO_RING_SIZE` headroom at peak draw count).
3. **Prev-model cache key** — `entityDef->index` alone (simplest v1, main non-mirror view only)
   vs index + per-frame view discriminator for mirrors/subviews/portals.
4. **Skinned-deformation MV error** — accept rigid-only for A2 (recommended) vs store prev-frame
   skinned positions from the start.
5. **Discontinuity source** — game `renderViewInterpolatable` (authoritative intent) vs a
   renderer pose-delta threshold (backstop for scripted camera moves it misses); likely both.
6. **Shader toolchain (C0, top unknown)** — offline-DXC blob vs GLSL port.
7. **FSR2 VK feature requirements** — does the target version's `ffx_fsr2_vk` need
   `VK_KHR_16bit_storage` / `shaderFloat16` / subgroup ops beyond what's enabled? Conditional
   no-op if absent.
8. **HUD reorder strategy (C1)** — scene-only + separate composited HUD target (recommended,
   HDR-correct HUD) vs replay the `!viewEntitys` HUD draws after resolve.
9. **Reactive-mask v1** — FSR2 auto-reactive (needs opaque-only copy) then hand-authored.
10. **Render-scale exposure** — Native-AA (1.0) is R1; sub-1.0 is **banked headroom** and breaks
    the render==display assumptions (EndFrame blit filter, resolve fullscreen-quad ST) — deferred.

---

## Where this sits on the roadmap

R1 is the temporal keystone: it delivers user-visible AA now (kills the specular shimmer SMAA
can't) and **pre-pays the render-scale headroom** the RT tiers will spend (67% scale ≈ half the
per-pixel ray budget once R2/R3 land). Motion vectors also feed the eventual RT-shadow denoising
(SIGMA temporal, R5). Frame generation (FSR3.1 FG) stays **parked** behind `com_interpolate`,
which already renders *real* frames above the 60 Hz sim. Next big step after R1 is **R2 — the
ray-query foundation** ([rtx-shadow-roadmap.md](rtx-shadow-roadmap.md)).
