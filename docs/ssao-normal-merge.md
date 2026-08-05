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
