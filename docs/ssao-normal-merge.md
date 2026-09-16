# SSAO normal-pass merge — design

**Goal:** eliminate the standalone normal G-buffer pass (`RB_RHI_NormalPrepass`) by having
the existing depth prepass emit the view-space normal in the *same* opaque-geometry pass.
Measured (RTX 3080 Ti, docs/ssao-gtao.md §8) the normal pass is **~0.65 ms / ~69% of SSAO's
cost** — a full second opaque submission whose expense the old GPU-only timing missed. It
also feeds SSR, so the win compounds. **Vulkan first** (clean); GL3 second (blocked on the
backbuffer-depth / MSAA question).

## Current architecture (as mapped 2026-08-05)

Per fullscreen primary view, `RB_RHI_DrawView` (RhiWorld.cpp):
1. **Depth prepass** `RB_RHI_FillDepthBuffer` — renders all opaque geometry with the `zfill`
   shader into the **scene depth** (the frame target's depth on VK: `sceneDepth`), sealing
   depth with subview clip planes, perforated-surface alpha discard, and the weapon depth
   hack. Ends by capturing scene depth → `_currentDepth` (`currentDepthImage`,
   `CopyDepthbuffer` / VK `rhiCaptured`).
2. **Normal prepass** `RB_RHI_NormalPrepass` — renders the *same* opaque geometry **again**
   with the `gbuffer` shader into a **separate** target `rhiNormalRT`
   (`CreateRenderTargetColorDepth`, RGBA8 + its own depth, full-res; `colorCount 2` adds the
   SSR rough/metal attachment). Writes bump-mapped view normal (RGB) + AO/weapon mask (A).
3. **SSAO** reads `_currentDepth` (unit 0) + `rhiNormalRT` color (unit 1).
4. **Interactions** render into `sceneColor`, depth-`EQUAL` against `sceneDepth`.

The RHI render-target model (VK `RenderTarget`): each target owns its color[0..1] + optional
depth. There is **no API to render a color target against another target's depth**, which is
why the merge needs a backend change, not just a caller change.

## Approach (Vulkan)

Give the **scene frame target** an extra, prepass-only **normal color attachment** (and a
third for SSR rough/metal when `r_ssr`), reusing the existing `colorCount`-2 MRT machinery
that already backs `CreateRenderTargetColorDepth`/SSR:

- The **merged prepass** renders opaque geometry once into `{ sceneColor(ignored), normal,
  [ssrRM] } + sceneDepth`, sealing `sceneDepth` exactly as `zfill` does today **and** writing
  the normal. Because it shares `sceneDepth`, no depth copy/blit and no external-depth API is
  needed — the interactions' depth-`EQUAL` still reads the same image.
- `_currentDepth` capture is unchanged (still from `sceneDepth`).
- The **main color pass** writes only attachment 0 (`sceneColor`); the normal attachment is
  `LOAD`/don't-care there (write mask off).
- `rhiNormalRT` and `RB_RHI_NormalPrepass` are **dropped**; SSAO/SSR sample the scene target's
  normal attachment via a `GetRenderTargetImage`-style accessor.

### Shader
The prepass fragment shader must do **both** jobs: seal depth identically to `zfill`
(clip-plane `gl_ClipDistance[0]`, per-stage alpha discard for perforated surfaces, weapon
depth hack — any divergence breaks the interactions' depth-`EQUAL` invariance) **and** write
the `gbuffer` normal + mask. Simplest: extend `gbuffer.vert/.frag` to carry zfill's clip
plane + alpha handling, and run it as the prepass; retire `zfill` for the primary view (keep
it for subviews/shadow paths that don't want a normal).

### RHI change needed
Add an optional normal (+SSR) attachment to the frame-target path and a way to bind it for
sampling. Scope: VK `RenderTarget` frame-target variant gains attachment(s) + pass variants
that write them during the prepass and skip them during the color pass; a
`GetFrameTargetNormalImage()` accessor. Contained to the VK backend + the RHI interface; the
GL3 backend stubs it (keeps the separate normal pass) until its FBO/MSAA path is handled.

### Implementation notes (as-mapped 2026-08-05)
- **Scene depth is mode-dependent:** `FrameDepthImage()` resolves to the built-in `sceneDepth`
  (HDR off) or the active HDR frame target's `dsImage` (HDR on). So the merged prepass's
  framebuffer must be built against `FrameDepthImage()` and rebuilt when it changes (HDR
  toggle / resize) — cache it keyed on (normalImage, depthImage). The depth image only
  changes on mode/size change, never per-frame within a mode.
- **`rhiNormalRT` already exists** (`CreateRenderTargetColorDepth`, RhiWorld) with its own
  color+depth. The merge does *not* need a new normal image conceptually — it needs the
  normal color to be written **while sealing `FrameDepthImage()`** so the second geometry pass
  and the depth copy both disappear. The clean form: a dedicated normal color image co-attached
  with the scene depth in a prepass render pass.
- **Bring-up is feature-flagged:** a cvar `r_ssaoMergeNormal` (default 0) gates the whole path.
  Each step lands inert at default; flip the flag to test. Once verified end-to-end and the
  fps win is confirmed, the flag becomes the default (or is retired) and `RB_RHI_NormalPrepass`
  is dropped for the primary view. This keeps every intermediate commit shippable.
- **Step 1 (this commit series):** the flag + a dedicated scene-res normal color image
  (allocated only when the flag is on) + a sampleable accessor, all unused. No pass wiring, no
  visual change. Steps 2–3 add the prepass render pass/framebuffer + merged shader, then route
  SSAO/SSR and drop the standalone pass.

## Step-2 findings (as-mapped 2026-08-05) — no new shader; it's pure pass structure

- **The `gbuffer` fragment shader already seals depth equivalently to `zfill`.** Both use the
  identical `u_alphaTest.y != 0.0 && texture(...).a < u_alphaTest.x → discard`; the depth value
  is implicit from the same rasterized geometry. So the merged prepass just runs the existing
  `gbuffer` shader — **no merged shader to author.** (Clip planes are subview-only; the merge is
  primary-view-only, so they don't apply.) The remaining risk is only that the *pass* covers the
  same surfaces with the same polygon-offset / depth-hack as the depth prepass —
  `RB_RHI_NormalPrepass` already does (memory + code), so the depth it produces should equal
  `zfill`'s. **This is the invariant to verify in-engine** (opaque interactions test depth-EQUAL
  against it).
- **The depth prepass draws into the already-open main scene pass** (color-masked, depth-only) —
  it does not open its own pass. So there are two viable structures:
  - **(A) MRT-in-scene-pass:** give the scene pass a 2nd color attachment (normal); the depth
    prepass runs `gbuffer` writing depth + normal, interactions write only attachment 0. **Cost:**
    ripples a per-attachment color-write-mask into *every* scene-pass pipeline (interactions,
    ambient, translucent, fog, in-pass shadows) + the mode-dependent scene/HDR render passes.
  - **(B) separate prepass + load-depth (chosen):** run `gbuffer` into a dedicated `{ normal +
    FrameDepthImage() }` render pass *before* the scene pass, sealing scene depth; capture
    `_currentDepth` from it; the scene pass then begins with a new **clear-color / load-depth-
    stencil** variant (stencil starts 0 from the prepass clear, per-light stencil clears work as
    today); skip `zfill`. **Cost:** one new prepass render pass + one new scene-pass render-pass
    variant (mode-dependent) + a reorder; scene-pass pipelines unchanged. Isolated → chosen.

**Correctness gate is visual, not headless.** The depth-EQUAL invariant (no vanishing / z-fighting
opaque surfaces) can only be confirmed by looking at the running game. So step 2 is built behind
`r_ssaoMergeNormal` (default 0, main path untouched) and verified in a build → in-engine-check loop
with the user, not asserted from a headless run.

## Status (2026-08-11): steps 2–3 wired — awaiting user visual/perf verdict

The merge is complete behind `r_ssaoMergeNormal` (VK, `r_ssr` off, fullscreen primary view):
`RB_RHI_NormalPrepass` now returns whether it took the merged path (BeginNormalPrepass
sealed the *scene* depth via the `gbuffer` pass). `RB_RHI_DrawWorld` runs it **first**; if
it merged, `zfill` (`RB_RHI_FillDepthBuffer`) is **skipped** and `_currentDepth` is captured
from the sealed depth (`RB_RHI_CaptureCurrentDepth`, extracted from the depth prepass);
otherwise `zfill` seals depth as before. The return value is the single source of truth, and a
`BeginNormalPrepass` fallback (returns 0) cleanly leaves `zfill` to seal depth — no drift, no
double-gate. **Correctness argument:** this is behaviour-identical to the already-verified B1
(where `zfill` ran then `BeginNormalPrepass` cleared+re-sealed depth, discarding zfill's work),
minus the wasted `zfill` geometry pass — so the final scene depth and `_currentDepth` are the
same, just one pass cheaper. Headless boot is validation-clean (212 frames, ep1/e1m1). Depth-
EQUAL invariant + the fps delta are the user's visual/perf gate.

## Status (2026-09-16): velocity attachment shipped — merge now the VK DEFAULT on every tier

The last limitation fell: the merged prepass gained the **RG16F velocity attachment** (R1/A2),
so `velWants` (r_motionVectors / r_fsr — the Ultra/Nightmare configs) no longer forces the
standalone 3-MRT path. `EnsureMergeNormal/BeginNormalPrepass` take `wantVel`; attachment
order mirrors the standalone target (normal 0, mat 1, velocity 2 — `wantVel` implies the mat
attachment) so the gbuffer pipeline/passClass stay shared; velocity clears to ZERO motion
like the standalone clear; `GetRenderTargetImage3` resolves the merged handle; and `RunFsr2`
special-cases the merged pseudo-handle (it has no targetTable entry, so the old
`LookupTarget` couldn't see it). **`r_ssaoMergeNormal` default flipped 0 → 1**: every VK tier
now runs ONE opaque geometry pass (gbuffer seals scene depth + writes normal/mat/velocity)
instead of zfill + a standalone normal pass. The standalone path remains as the A/B
(`r_ssaoMergeNormal 0`) and the GL3 path. Verified: mars_city1 with the full
SSAO+SSR+HDR+PBR+MV+FSR2 config — FSR2 creates its context off the merged velocity (its
validation gate passes), ~21k frames, validation-error parity with the merge-off baseline
(the residual map-load errors pre-date this change and reproduce identically with merge off).
Pending user verify: visual parity (depth-EQUAL sacred — watch for z-fighting / vanishing
surfaces) + the fps win on Ultra/Nightmare.

## Status (2026-08-11 later): SSR MRT extension shipped — merge now engages with SSR on

`BeginNormalPrepass(w, h, clear, wantMrt)` gained an optional 2nd color attachment
(rough/metal). With `wantMrt` the merged pass renders `{normal, mat} + shared scene depth`;
`EnsureMergeNormal` rebuilds on a `wantMrt` change and uses `PassClassFor(RGBA8, true,
nColor)` so the `gbuffer` pipeline stays shared with — and render-pass-compatible with — the
standalone MRT target (`CreateRenderTargetColorDepth` colorCount 2). `GetRenderTargetImage2`
of the merged handle returns the mat image. The caller dropped the `!ssrWants` gate, passes
`ssrWants` as `wantMrt`, sets `rhiNormalMrt`, and SSR (march + composite) reads
`rhiNormalResultRT` (merged handle or standalone) instead of hardcoded `rhiNormalRT`. So the
merge now folds the normal pass away in the actual `r_ssr 1` config, not just SSR-off.
**User-verified** (VK, `r_ssr 1 + r_ssaoMergeNormal 1`): no black scene, SSR intact, **~0.3%
fps uplift** — small, as expected (the win is CPU draw-submission, mostly hidden GPU-bound).

### Earlier limitation (now lifted)
Steps 2–3 engaged only when `r_ssr` was OFF — SSR needed the 2nd MRT (rough/metal) attachment
that `BeginNormalPrepass` didn't provide, so with SSR on it fell back to the standalone pass.
The extension above provides that attachment.

## Incremental steps (each build + verify before the next)

1. **RHI + VK plumbing:** scene frame target grows the normal attachment; accessor added; no
   caller change yet (attachment unused). Verify: validation-clean, no visual change.
2. **Merged prepass shader:** author the zfill+normal prepass shader; verify depth sealing is
   bit-identical (no z-fighting / lost interactions) with `r_ssaoDebug 3` showing the normal.
3. **Route SSAO/SSR** to the frame target's normal attachment; drop `RB_RHI_NormalPrepass`
   for the primary view. Verify AO looks identical (`r_ssaoDebug 1/3`), SSR intact.
4. **Measure:** `r_vkGpuTime` + fps A/B at a real-geometry viewpoint — expect the normal
   pass's ~0.65 ms to fold away.
5. **GL3:** apply the same (shared FBO depth + MRT normal) if the MSAA-blit path allows; else
   GL3 keeps the standalone pass (documented divergence).

## Risks / invariants
- **Depth-EQUAL invariance is sacred:** the merged prepass must seal `sceneDepth` exactly as
  `zfill` (clip planes, alpha discard, polygon offset, weapon/model depth hack) or opaque
  interactions vanish / z-fight. This is the main correctness risk — step 2 gates on it.
- **MSAA:** if the scene target is ever multisampled the normal attachment must match; today
  the VK scene target is single-sample (`RGBA8 + D24S8`), so fine now.
- **Subviews / non-fullscreen views** keep the plain `zfill` path (no normal wanted).
- **SSR** rides the same attachment set — validate it in lockstep, not after.
