# R1 — the FSR2 temporal pipeline (motion vectors + jitter + FSR2 Native-AA)

Status: **COMPLETE — A0..E all built + user-verified, merged to master 2026-08-18** (`r_fsr`
Native-AA + auto-reactive deghosting, default in the Nightmare preset, backend-aware AA
menus). This is roadmap item **R1** from [rtx-shadow-roadmap.md](rtx-shadow-roadmap.md) —
the temporal pipeline that supersedes the hand-rolled-TAA plan in
[antialiasing.md](antialiasing.md). VK-only for the FSR2 dispatch; motion vectors + jitter
benefit both RHI backends. Legacy ARB is never touched. Every increment is cvar-gated so
**OFF == today's renderer, bit-for-bit**. Each increment section below carries its as-built
record above the original plan text.

## PARKED FOLLOW-UP — FSR2 optimisation wishlist (user-approved 2026-08-20, not scheduled)

Three post-R1 improvements the user wants pursued eventually — parked deliberately, do not
chase until FSR2 work reopens (limb ghosting sighting or the RT-era upscaling package):

1. **Velocity into the merged normal prepass** (~0.3–0.5 ms GPU back): `r_fsr` currently
   forces the STANDALONE 3-MRT G-buffer pass even when the merged-into-zfill normal path
   could carry a 3rd attachment. Deliberately sidestepped in A2 because the merged pass owns
   the depth-EQUAL / `RB_RHI_TessBumpForZfill` tessellation contract (the ember/acne bug
   class) — the riskiest cheap win, needs that contract re-verified.
2. **Kill the Native-AA copy-back** (~0.1 ms): needs the scene/HUD reorder below — bundle
   with the upscaling work, not standalone.
3. **Feed the engine's eye-adaptation exposure to FSR2** (drop `FFX_FSR2_ENABLE_AUTO_EXPOSURE`,
   pass the 1x1 `rbEyeExposureImg` as the `exposure` resource) so accumulation weighting
   matches the tonemap exactly. Small; only matters if brightness-transition artifacts show.

## PARKED FOLLOW-UP — sub-native upscaling (the "real FSR" fps lever)

Deliberately NOT built in R1 (the game is CPU-front-end bound today, so a GPU-side fps win
buys nothing; the customer is the RT era, where per-pixel ray budgets make 67% scale ≈ half
the ray cost). When it happens, the work is:

- **Render scale < 1.0**: scene (and its depth/velocity/SSAO/SSR chain) at render-res,
  FSR2 output at display-res. `r_fsrScale` (or quality presets: Quality 0.67 / Balanced
  0.59 / Performance 0.5), plus `fsrQuality/renderScale` preset columns (E reserved them).
- **The scene/HUD reorder** deferred out of C2: FSR2 writes a display-res HDR composite the
  HUD blends into (in HDR), tonemap last — this also kills C2's copy-back. The copy-back
  shortcut is exact ONLY at Native-AA; at any other scale the reorder is mandatory.
- Re-check `ffxFsr2GetJitterPhaseCount` (phase count grows with the scale ratio) and the
  `maxRenderSize`/`displaySize` split in `Fsr2EnsureContext` (today both == display).
- Reduced-res inputs interact with the SSAO/SSR temporal consumers (they'd march at
  render-res for free — a win — but their history buffers key on size).

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

### A0 — MRT/format plumbing (VK, self-test only)  **[DONE + user-verified 2026-08-16]**

`r_mrt3Test` PASS on the RTX 3080 Ti (`3-MRT target (RGBA8 + RGBA8 + RG16F + depth, passClass 8)
created`). Adds `IF_RG16F` + per-attachment formats + a distinct 3-MRT pass class; bit-identical
when off. Adversarial review confirmed the invariant and caught 3 dormant defects in the 3-MRT
path (blend-array size, teardown leaks) — all fixed, so A2 gets a correct foundation.

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

**PRODUCTION half DONE + user-validated 2026-08-16 (branch `feat/temporal-prevvp-reset`).** The
velocity buffer ships (`r_motionVectors`, `r_mvDebug` 1 dir / 2 mag + `r_mvDebugScale`); consumption
(feeding temporal SSAO/SSR) is the follow-up. **As-built deltas from the plan below:** (a) stores the
prev MVP by **growing `RenderParams` one mat4 (`prevMvpMatrix`, 928→992)**, NOT the "separate small
UBO/push" the plan preferred — `uboAlign=256` so 928 and 992 round to the same 1024-byte ring slice,
making the "+64 B/draw" cost literally zero, while a 2nd UBO binding is layout surgery on every
pipeline. (b) **Standalone 3-MRT path forced when velocity is wanted** (`wantMerge && !velWants`),
which *sidesteps* the `RB_RHI_TessBumpForZfill`/merge/depth-EQUAL contract entirely — scene depth
stays zfill's, untouched, so "MV-on depth == zfill depth" holds by construction. (c) cache is a flat
`idList` keyed `[0]=worldSpace`, `[entityDef->index+1]=entities` (not `idHashIndex`; index is dense
and the primary-fullscreen-view-only prepass means no mirror/subview dupes). (d) velocity is
**per-pixel** — vert/tese emit cur+prev clip, frag divides each by its own `w`. **Validation:** static
camera → grey (≈0); **strafe → walls tint by depth (near>far parallax); walk forward → radial
expansion field** (the geometry-tracking + sign confirmation); a camera pan concentrates velocity at
the edges (perspective `sec²`), a poor geometry test. Rigid-only: skinned limb motion isn't captured
(v1 limit → increment D). **NEXT: A2-consumption** = temporal SSAO/SSR sample the velocity
(`historyUV = currentUV − velocity`) instead of the camera-only reproj matrix, with a fallback when
`r_motionVectors` is off.

Original plan text:

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

### B — sub-pixel Halton jitter (both RHI)  **[DONE + user-verified 2026-08-16]**

`r_temporalJitter` (default 0, ARCHIVE). **As-built deltas:** (a) **hand-rolled `R_Halton(index,base)`**
in tr_main.cpp, NOT `ffxFsr2GetJitterOffset` — keeps backend-agnostic core from including FSR2/VK
headers; FSR2 only needs the applied jitter fed back via `jitterOffset` (any consistent sequence
works; Halton(2,3) phase 8 matches its default). (b) Legacy `r_jitter` **left as an independent
source** (not retired) — both default 0, so OFF == un-jittered, bit-identical. (c) **Velocity
jitter-cancellation done HERE** (the A2 coupling): the projection jitter is a depth-independent
uniform screen shift, so one per-view `(jitter_cur − jitter_prev)/viewport` (+Y-up UV) added to the
velocity in gbuffer.frag via the spare `u_localParam1.xy` cancels it exactly — cheaper than an
un-jittered MVP, no RenderParams growth. Prev jitter tracked in `rhiPrevJitter` (advanced once/frame
in the prepass, reset with the MV cache). `viewDef->jitter[2]` (pixels, +Y-up) stores it for C2.
Skips subview/env-probe/screenshot (2D/HUD doesn't hit R_SetupProjection). **User-verified:** static
camera + `r_temporalJitter 1` + `r_mvDebug 1` stays flat grey (jitter cancelled from the velocity);
`r_temporalJitter 0` = unchanged. Payoff is C2 (alone it just shimmers — no resolve).

Original plan text:

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

### C1 — add `r_fsr` + force the HDR scene path  **[DONE + user-verified 2026-08-16]**

**RE-SCOPED from the original "passthrough reorder" plan (below) after a user test.** The reorder was
built and reverted: resolving the scene to LDR before the 2D overlays made the HUD slightly DIMMER,
because with `r_hdr` on the old flow composites scene+HUD **together** in `rhiHdrRT` and blends
translucent HUD panels over the **un-clamped HDR** scene. **Key insight: a passthrough reorder has
nowhere correct to composite the HUD** — keeping scene+HUD in the HDR buffer and resolving at swap
IS the old flow; the HUD-in-HDR fix needs a display-res **HDR composite** buffer, which is only
needed once FSR2 actually inserts between the scene and the HUD. **So the reorder moved to C2.** C1
is now just: `r_fsr` (VK-only, default 0, ARCHIVE) + `HdrBeginFrame`'s `wantHdr |= (r_fsr && VK)`
forcing the RGBA16F scene path = **bit-identical to `r_hdr` on** (HUD stays in HDR, unchanged).
User-verified: `r_fsr 1` indistinguishable from `r_fsr 0`. Original plan text below.

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

**BUILT (branch `feat/fsr2-dispatch`, pending in-game verify). As-built deltas from the plan
below:**

- **COPY-BACK, not the reorder.** The scene/HUD reorder (separate display-res composite
  buffer, HUD retargeted into it) was NOT built. Instead `RunFsr2` (VulkanBackend) writes
  FSR2's output to an internal RGBA16F storage image and **copies it back over `rhiHdrRT`'s
  color attachment** — exact at Native-AA (same size/format, one ~0.1 ms copy), and the
  entire frame after the dispatch is structurally untouched: HUD still composites in HDR
  into `rhiHdrRT`, eye-adapt/bloom meter the (now stabilised) scene, tonemap/grain at the
  resolve are unchanged. The reorder's only real payoff is render-res ≠ display-res, so it
  moves to the future upscaling increment. This kills the reorder's whole OFF-path /
  vid_restart risk class.
- **Dispatch site**: end of the primary fullscreen world view, after `RB_RHI_DepthOfField`,
  BEFORE the eye-adapt/bloom measurement (RhiBackend.cpp) — adaptation and bloom read the
  FSR2-resolved scene, so bloom stops shimmering too.
- **`separateDepthStencilLayouts` (core 1.2) now enabled when supported**: the vendored FSR2
  VK backend barriers the sampled depth with a depth-ONLY aspect, which a combined
  D24S8/D32S8 image only permits with the feature. `RunFsr2` refuses (one warning) without
  it. The scene depth is sampled through a backend-owned depth-only view of `rhiHdrRT`'s
  depth-stencil image, round-tripped ATTACHMENT_OPTIMAL → SHADER_READ_ONLY → back; all
  inputs are declared in the states they already rest in, so FSR2's own barriers are
  same-layout no-ops.
- **Jitter Y is negated at the dispatch** (`jitterOffset = {+jx, −jy}`), NOT passed raw as
  planned: the same +Y-up → top-left flip reasoning that produced the MV scale applies to
  the jitter too (FSR2's convention applies its jitterY as `proj[2][1] -= 2·jy/h`).
- **Auto-enable**: `r_fsr` implies the velocity gbuffer (RhiWorld `velWants`) and the
  temporal Halton jitter (tr_main) on VK — MV/jitter are infrastructure, not user toggles
  (per section E). It also bypasses FXAA/SMAA at the HDR resolve (`rbFsrRanThisFrame`).
- **New cvar `r_fsrSharpness`** (0..1, default 0.8, 0 = off) drives RCAS. FSR/RCAS controls
  in the ImGui FSR debug group.
- **Reset**: `RB_RHI_TemporalFsrReset` (RhiWorld) reuses A1's shared camera-cut classify and
  stages every frame even with temporal SSAO/SSR off; first dispatch of a context forces
  reset. `frameTimeDelta` = wall-clock ms between dispatches, clamped [0.1, 200].

Original plan text:

The convergence point. **C2 also inherits the scene/HUD REORDER deferred from C1** (see C1's
re-scope note): FSR2 reads the scene (`rhiHdrRT`) before the 2D overlays and writes a **display-res
HDR (RGBA16F) composite** buffer; the HUD/console/menu then composite into THAT buffer **in HDR**
(so translucent panels keep the pre-tonemap brightness the old whole-frame path gave them — the
thing C1's LDR reorder got wrong), and grain/chroma stay scene-only while gamma/tonemap is the
final pass over the whole composite at swap. FSR2 forces the RGBA16F scene buffer on (already done
in C1) even if `r_hdr` is off (FSR2 wants HDR). New `RunFsr2` records FSR2's dispatch onto the frame
command buffer **after the scene pass closes** (the RHI `Dispatch` refuses in-render-pass and records
pre-scene). Wrap inputs with `ffxGetTextureResourceVK`: color = `rhiHdrRT` (pre-tonemap RGBA16F),
depth = scene depth aspect, motionVectors = A2's RG16F, output = a new display-res compute-write
image. **Context flags (verified):** `HIGH_DYNAMIC_RANGE` set, `DEPTH_INFINITE` set,
`DEPTH_INVERTED` **unset**, `AUTO_EXPOSURE` set, `MOTION_VECTORS_JITTER_CANCELLATION` unset.
**Dispatch fields:** `motionVectorScale = {−renderW, +renderH}` — **NOT `{renderW, −renderH}`**
(corrected by the A2 adversarial review, verified against the vendored FSR2 shaders). A2 stores
`currUV − prevUV` in **+Y-up** UV; FSR2 wants `prevUV − curUV` in **top-left +Y-down** UV
(`fReprojectedUv = fUv + fMotionVector`, then samples history — `ffx_fsr2_reproject.h`,
`_depth_clip.h`, `_reconstruct_dilated_velocity...h`). The scale must therefore do TWO things
at once: reverse the direction (negate **both** axes) and flip +Y-up→+Y-down (negate Y). On X
only the reversal applies → **−renderW**; on Y the reversal and the coordinate flip **cancel** →
**+renderH**. The naive "just flip Y" (`{renderW, −renderH}`) reprojects backwards on both axes =
full-image smear. (The in-engine temporal SSAO/SSR consumers use `historyUV = currentUV − velocity`
in the engine's own +Y-up space and are unaffected — this correction is FSR2-dispatch-only.)
`jitterOffset` = B's stored value, `frameTimeDelta` in **ms**,
`cameraFovAngleVertical` in **radians**, `preExposure` = 1.0, `reset` = A1's `historyReset`.
Bypass FXAA/SMAA when `r_fsr` is on (RCAS replaces them). Move grain/chroma/gamma to display res
over the FSR2 output. **Validators:** `r_fsr 0` bit-identical (C1 already proved the passthrough);
`r_fsr2Test` synthetic self-test; **empirical MV-sign pan test** (violent smear on pan ⇒ Y-sign
wrong — fix before trusting); early `AUTO_EXPOSURE` check on a bright/dark scene; shimmer A/B vs
SMAA → user screenshot sign-off.

### D — reactive + transparency/composition masks (VK)

**V1 (auto-reactive) BUILT (branch `feat/fsr2-dispatch`, pending user verify).** As-built:
`Fsr2CaptureOpaque` snapshots the scene color at the opaque/translucent split of the primary
view (right after the SSR composite — reflections live on opaque surfaces — and before
particles/blends/fog draw; a straight same-orientation `vkCmdCopyImage`, NOT the y-flipped M5
capture). `RunFsr2` then runs `ffxFsr2ContextGenerateReactiveMask` (opaque-vs-final, AMD
reference constants: threshold 0.2 → binary 0.9, per-channel max, tonemapped compare) into a
context-owned R8 mask fed to the dispatch as `reactive`. New cvars `r_fsrReactive` (default 1)
+ `r_fsrReactiveScale` (default 1.0); ImGui FSR group has both. The snapshot is one-frame data
(invalidated each BeginFrame). The weapon keeps A2's zeroed velocity as its treatment. The
hand-authored R8 + transparency-and-composition V2 below stays open if auto-reactive proves
insufficient. Fog draws after the split, so fog regions read as reactive — acceptable
(low-frequency content, nothing to shimmer).

Original plan text:

The real integration quality work: kill temporal ghosting on Doom 3's **additive-blended**
content (muzzle flashes, plasma, particles, GUI screens, heat-haze/refraction) and the weapon
view-model. V1 shortcut = FSR2's **auto-reactive** (needs an opaque-only color copy). V2 =
hand-authored R8 reactive + transparency-and-composition masks, raising the mask where additive
stages draw in the blend pass. The weapon (worst MV offender) gets reactive/stencil treatment on
top of A2's velocity zeroing. **Validator:** particle-ghosting A/B on a plasma-fire scene; no
weapon smear during rapid strafing; user screenshot sign-off.

### E — preset + menu wiring (VK)

**BUILT (branch `feat/fsr2-dispatch`).** As-built: `fsr` column appended to
`EnhancementPreset` + matching `DetectEnhancementPreset` compare; **Nightmare only** gets
`fsr=true`. **Deviation from the plan below: `rhiAA` STAYS SMAA (2) in the Nightmare row**,
not 0 — the HDR resolve already bypasses FXAA/SMAA whenever FSR2 actually resolved the frame
(`rbFsrRanThisFrame`), so there is no stacked AA, and keeping SMAA gives GL3 (where `r_fsr`
is inert) its post-AA fallback on the same preset. The `fsrQuality/renderScale` columns wait
for the upscaling increment (Native-AA has no scale). Menu wiring shipped earlier and beyond
the plan: the classic System-menu AA row is backend-aware (MSAA legacy / FXAA+SMAA GL3 /
+FSR2 VK via the `dude_aa` bridge), and the ImGui FSR2 controls (toggle + RCAS + reactive)
live in Graphics → Antialiasing, with the Debug-tab FSR group keeping only the MV
infrastructure/debug views. "Ultra Nightmare" stays doc-reserved.

Original plan text:

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
