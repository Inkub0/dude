# Handoff: Vulkan-only one-frame "IPN News" flash before the Mars City intro cinematic

Status: **RESOLVED 2026-09-24** (user-verified on Vulkan). Root cause and fix below; the rest of
this document is the investigation record that led there.

## 0. Resolution

- **What the flash is:** NOT the load->cinematic hand-off frame. It is the first gameplay tic's
  two *objective camera shots*: `idObjective::Event_CamShot` (`neo/game/Item.cpp` ~L716) crops
  256x256 POT (1024x512 at 2560x1440), renders the objective camera view and calls
  `CaptureRenderToFile`, which runs `R_IssueRenderCommands()` mid-frame with **no `RC_SET_BUFFER`
  and no `RC_SWAP_BUFFERS`**. The crop's `EmitFullScreen` also flushes the pending `runGui`
  redraw of `guis/movies/ipn.gui` (the hanging monitor `tim_func_mover_1` has `runGui 1`, so
  `idStaticEntity::Think` -> `StateChanged(time, true)` -> `idWindow::Redraw` paints the whole
  IPN screen into the 2D gui model every game tic; `BeginFrame` normally discards it) as a
  full-screen 2D view in front of the cam-shot view. Hence "IPN full-screen + a smaller panel
  showing a game scene".
- **Why VK only:** GL swaps only in the frontend's `RC_SWAP_BUFFERS` case, so a swap-less
  command list never reaches the screen. The Vulkan backend presented on **every** `EndFrame`,
  so both cam-shot issues were blitted to the swapchain and shown for a frame each. (The same
  bug glitched manual saves on VK: `SaveGame`'s 320x240 preview capture is the same path.)
- **Fix:** `rhi::RHI::SetFramePresent(bool)` (default no-op; GL3 untouched). The frontend
  (`RB_RHI_ExecuteBackEndCommands`) scans the command list for `RC_SWAP_BUFFERS` and tells the
  backend; `VulkanBackend::BeginFrame` acquires a swapchain image only for presenting frames and
  `EndFrame` closes + submits a capture-only issue without blit/ImGui/present (the readback
  `ReadPixelsRGB` waits on the queue, so `CaptureRenderToFile` still captures correctly).
- **Why the earlier evidence misled:** the 1980-surface IPN view at the hand-off frame (10
  settle-tic redraws flushed by `StartWipe`'s crop) is real but harmless: `RC_SET_BUFFER` clears
  it before the world draws, on both backends. Skipping that view (diag experiment) also skipped
  the pre-`SET_BUFFER` cam-shot views, which is why it "worked". `r_clear` never tinted the flash
  because the cam-shot issue carries no `RC_SET_BUFFER`. The stock wipe is a no-op on every
  backend (`wipe2Material` samples the all-zero `_black`), so `com_wipeSeconds` was irrelevant.
- **Diagnostic that found it:** a cvar-gated per-frame command summary (one line per
  `RB_RHI_ExecuteBackEndCommands` call) plus a VK BeginPass/EndPass/present trace armed on the
  frame carrying an IPN 2D view; the giveaway was two command lists in one `tr.frameCount` with
  no `SETBUF`/`SWAP`, each ending in a VK present. All hooks were removed after verification.

Engine: DUDE (fork of dhewm3 / Doom 3 GPL), multiple render backends selectable via
`r_graphicsAPI`: `opengl` (legacy/classic), `opengl3` (GL3, RHI), `vulkan` (RHI), `vulkan-rt`.
The `opengl3` and `vulkan` backends share a common RHI **frontend** (`neo/renderer/rhi/RhiBackend.cpp`,
`RhiWorld.cpp`) and differ only in the **backend executor** (`neo/renderer/rhi/vk/VulkanBackend.cpp`
for VK; a GL3 executor for opengl3).

---

## 1. Symptom

Starting a New Game (Mars City / `maps/game/mars_city1`), during the opening intro sequence —
"the intro cinematic starting with the landing monitor display, then Betruger watching out of a
window" — there is a **single-frame flash of the full-screen "IPN News Network" broadcast screen**
(a readable IPN news graphic filling most of the screen, with a smaller rectangle/panel that shows a
game scene) immediately at the hand-off into the cinematic.

- **Vulkan only.** `opengl3`, `opengl` (classic), and upstream stock dhewm3 are all clean — verified
  by the user switching backends.
- Deterministic: always at that same transition. It is **one frame** ("split second").
- The user is willing to accept an ad-hoc "black it out" workaround if no clean fix is found.

---

## 2. DEFINITIVELY IDENTIFIED (what and when)

Via cvar-gated `common->Printf` logging in the frontend (see §5 for the technique), the flash frame
was captured precisely. Example capture (frame counter `fc`), the flash is **fc=3431** here (a
different run put the same event at fc=386 / fc=3624 — the frame number varies, the structure does not):

```
fc=3429  2D vp=2560x1440@0,0 surfs=8            <- trivial transition/loading frame
fc=3430  2D vp=2560x1440@0,0 surfs=8            <- trivial transition/loading frame
fc=3431  2D vp=2560x1440@0,0 surfs=1980         <- THE FLASH: full-screen IPN broadcast GUI
fc=3431  [ipn] mat=video/video_ipn_news        2D scissor=0,0..2559,1439 vp=2560x1440@0,0  (x~10)
fc=3431  [ipn] mat=guis/assets/marscity/ipn    2D scissor=0,0..2559,1439 vp=2560x1440@0,0  (x~10)
fc=3431  2D vp=2048x1024@0,0 surfs=8            <- a capture/offscreen render this frame
fc=3431  3D vp=2560x1440@0,0 surfs=37 (id=1)    <- a 3D view
fc=3431  2D vp=2560x1440@0,0 surfs=27           <- HUD/2D
fc=3432  2D vp=2560x1440@0,0 surfs=6            <- back to trivial frames
fc=3433  2D vp=2560x1440@0,0 surfs=6
```

Conclusions, each backed by a test:

- The flash content is **`guis/movies/ipn.gui`** rendered **full-screen** as a 2D worldless-style
  view. Its materials are `video/video_ipn_news` and `guis/assets/marscity/ipn`
  (defined in `base/pak000.pk4 :: guis/movies/ipn.gui`; `video.mtr` defines `video_ipn_news`).
- It is drawn full-screen **exactly once** in an entire run → it is a **transient transition frame**,
  not an intended multi-second interstitial.
- It is **genuinely drawn**, not a stale/leaked buffer: setting `r_clear "1 0 1"` (magenta clear)
  does NOT tint the flash → the pixels are painted by a draw, not left-over clear background.
- It is **not the RoQ video**: `r_skipROQ 1` does NOT remove it (so it's effectively the static
  IPN imagery / GUI, independent of video playback).
- It is **not the mirror/subview path**: the flash views log `sub=0`, and `r_skipSubviews 1` only
  removes the toilet mirror, not this.
- The RHI frontend emits this frame on **both** GL3 and VK (shared code). Therefore the divergence is
  in the **VK backend's presentation of this transitional frame** — VK presents it, the GL path does
  not (it is swallowed/covered by the transition on GL).

The neighbouring frames (surfs 6–8) look like the load/transition GUI (`guiLoading`/`ExecuteMapChange`),
so fc=3431 is the game rendering one full frame of the just-set-up scene (the IPN broadcast) in the
middle of the load→cinematic hand-off.

---

## 3. RULED OUT (with the exact test that eliminated each)

All of these were tried as code changes and/or console tests; none fixed it; all code was reverted.

1. **Render interpolation** (`com_interpolate`, a DUDE feature): `com_interpolate 0` does not change
   the flash. Also tried forcing >=1 game tic before the first render after spawn — no effect.
2. **All temporal-history features**: **Potato preset also flashes**, and Potato has no HDR, no FSR2,
   no RTGI/RTAO/NRD, no volumetrics. `r_fsr 0` (disable FSR2) — no change. So not FSR2/TAA/denoiser
   history and not preset-specific.
3. **Persistent scene-buffer leak (sceneColor)**: VK renders every frame into one persistent
   `sceneColor` image (`VulkanBackend.cpp`, blitted to the swapchain in `EndFrame`). Forcing a
   per-frame color clear of `sceneColor` did nothing — because `RC_SET_BUFFER` already clears the
   active target every frame (`r_clear` default "2"; see `RhiBackend.cpp` `RC_SET_BUFFER` handler).
4. **HDR/post buffer leak (`rhiHdrRT`)**: clearing `rhiHdrRT` once per frame in
   `RB_RHI_HdrBeginFrame` — no effect (and Potato doesn't use `rhiHdrRT` anyway).
5. **`RHI_SKIP_STAGE_IMAGE`** (VK deliberately skips a draw that samples a never-captured
   `_currentRender`/`_scratch`): instrumented — this path never fires during the intro. Not it.
6. **The screen WIPE** (`idSessionLocal::StartWipe` / `DrawWipeModel`, `neo/framework/Session.cpp`):
   - `com_wipeSeconds 0.01` vs `1` — no difference in the flash → not the wipe fade duration.
     (`com_wipeSeconds 0` crashes; irrelevant.)
   - `StartWipe` uses `CropRenderSize(640,480,true)` which POT-rounds the physical viewport DOWN to
     2048x1024 — the same bug documented in `idPlayerView::BerserkVision`
     (`neo/game/PlayerView.cpp` ~L590-610). Applied the same "capture full-res on RHI, no crop" fix
     to `StartWipe` — no effect.
   - Fully skipping the wipe on VK (`ClearWipe(); return;` in `StartWipe` when
     `r_graphicsAPI`==vulkan) — **no effect**. So the wipe is NOT the cause, and the `_scratch`
     2048x1024 crop-capture seen at some flash frames (which IS `StartWipe`) was a **red herring**.
7. **Subview leftover clear**: forcing a color clear on the first main view after any `isSubview`
   render — did not fix it (the flash views are `sub=0`) and **broke the toilet mirror**. Reverted.
8. **`_scratch` capture leftover clear**: clearing `sceneColor` after any `RC_COPY_RENDER` to
   `globalImages->scratchImage`/`scratchImage2` on VK — no effect.

---

## 4. LEADING REMAINING HYPOTHESIS

The frontend renders one transitional frame (the full-screen IPN broadcast GUI, `guis/movies/ipn.gui`)
during the load→cinematic hand-off. On **GL** this frame is either not presented or is covered by the
transition; on **VK** it is presented for one frame. The mechanism is a **presentation/pacing
difference during the transition** (likely inside `idSessionLocal::ExecuteMapChange` /
`UpdateScreen` / the first frame after `mapSpawned`), NOT any single render subsystem (all the obvious
ones are ruled out above).

Notably the flash frame ALSO performs a 2048x1024 offscreen capture (the `2D vp=2048x1024@0,0` view)
— i.e. it is a capture/setup frame. A promising angle: on GL a capture/setup `UpdateScreen` during the
transition does not become the visible frame, but on VK it does. Worth confirming whether fc=3431 is
inside `ExecuteMapChange` (`insideExecuteMapChange`) or the first post-spawn frame, and whether VK is
presenting an `UpdateScreen` that GL treats as non-visible.

Related known VK issues in this engine (same family — VK present/scissor/subview quirks), see the
project memory files: `vk-scissor-effflipy-bug`, `mirror-subview-merge-spill`,
`gl3-to-vk-stale-state-bug`.

---

## 5. KEY CODE LOCATIONS

- `neo/renderer/rhi/RhiBackend.cpp`
  - `RB_RHI_DrawView()` (~L3394): sets viewport/scissor per view; 3D view clears depth/stencil (not
    color); 2D view `BeginPass(NULL)`.
  - `RB_RHI_RenderShaderPasses()` (~L2905): per-surface stage draw; `surf->material`,
    `surf->scissorRect` available here (used for the `[ipn]` material logging).
  - `RB_RHI_HdrBeginFrame()` (~L845): routes world frames into `rhiHdrRT`; worldless frames early-out
    to the backbuffer. `SetFrameTarget(rhiHdrRT)` ~L954.
  - `RC_SET_BUFFER` handler (~L3682): the per-frame `r_clear` color clear of the active target.
  - `RC_COPY_RENDER` handler (~L3727): `cmd->image->CopyFramebuffer(...)` (the `_scratch`/`_currentRender` captures).
  - `RB_RHI_BindStageImage()` (~L1737) + `RHI_SKIP_STAGE_IMAGE` (~L1735).
- `neo/renderer/rhi/vk/VulkanBackend.cpp`
  - `BeginFrame`/`EndFrame` (~L2588 / ~L2984): `EndFrame` blits the persistent `sceneColor` to the
    acquired swapchain image and presents.
  - `BeginPass()` (~L2867): render-pass variant selection; `wantColorClear = (clear&&clear->color) || !everWritten`.
    `sceneEverWritten` only resets on target (re)create.
  - `CopyFramebufferToImage()` (~L8504): suspends the render pass, copies, next draw resumes LOAD variant.
  - `UpdateTexture2D()` (~L8681) / `CreateTexture2D()` (~L8075): cinematic scratch upload paths (barriered, correct).
- `neo/framework/Session.cpp`
  - `StartWipe()` (~L499), `CompleteWipe()` (~L522), `DrawWipeModel()` (~L2360), `ClearWipe()` (~L589).
  - `StartWipe` callers: `ExecuteMapChange` ~L1575 (`"wipeMaterial"`, hold + CompleteWipe) and ~L1739
    (`"wipe2Material"`). `ExecuteMapChange` loading loop ~L1729-1735 (repeated `UpdateScreen`).
  - `mapSpawned = true` ~L1772; `lastGameTic = latchedTicNumber = com_ticNumber` ~L1749.
- `neo/renderer/RenderSystem.cpp`: `CropRenderSize()` (~L850), `UnCrop()` (~L930),
  `CaptureRenderToImage()` (~L960, emits `RC_COPY_RENDER`).
- Content: `guis/movies/ipn.gui`, `guis/marscity/transport_intro.gui` (the landing-monitor gui with a
  `gui/marscity/monoflyby` video + a `guis/assets/marscity/flybystatic` overlay), `materials/video.mtr`
  (`video/video_ipn_news`, `gui/marscity/*`), `textures/mcity/ipnnews` in `materials/base_wall.mtr`.
  All in `base/pak000.pk4`. Map `maps/game/mars_city1.map` uses `func_cameraview` +
  `marscity_cinematic_*` entities for the intro; `char_betruger` for the window shot.

---

## 6. DIAGNOSTIC TECHNIQUE THAT WORKED (reproduce/extend)

The engine writes **no persistent log file**; console output goes to stdout (the terminal running
`run.sh`) and `condump <file>` dumps the console buffer to `fs_savepath`. The reliable capture flow:

1. Add a `static idCVar` gate in `RhiBackend.cpp` and `common->Printf(...)` lines at the point of
   interest (per-view in `RB_RHI_DrawView`, per-capture in the `RC_COPY_RENDER` handler, per-surface
   material in `RB_RHI_RenderShaderPasses`).
2. Build: `cmake --build /home/peppe/dhewm-rt/build --target dude -j$(nproc)`.
3. Run and tee stdout to a file the assistant can read directly off disk:
   `./run.sh +set r_graphicsAPI vulkan +set <yourcvar> 1 2>&1 | tee /tmp/dude.log`
4. Play through the flash, quit, then grep `/tmp/dude.log`.

Useful fields per view: `viewDef->viewEntitys` (3D vs 2D), `viewport`, `scissor`,
`viewDef->numDrawSurfs`, `isSubview`/`isMirror`/`isXraySubview`, `viewDef->renderView.viewID`,
`viewDef->projectionMatrix[14]` (< -4 ≈ normal near plane; > -4 ≈ cinematic `r_znear=1`), and
`tr.frameCount`. For captures: `cmd->image->imgName`, rect. For materials: `surf->material->GetName()`.

Constraints in this project: the assistant must NOT launch or view the running game (build + static
analysis only; the user runs it and shares logs). `run.sh` launches `build/dude` (override with
`DUDE_BIN=...`). Old release binaries exist under `dist/release-0.9.x/` for bisecting (the flash is
present at least back to 0.9.1 / Aug 2026).

---

## 7. AD-HOC WORKAROUND (endorsed by the user, NOT yet implemented)

On Vulkan, skip drawing the full-screen 2D IPN-broadcast materials so the transient frame renders
black/empty instead of the IPN screen. In `RB_RHI_RenderShaderPasses` (VK path only), skip the surface
when `viewDef->viewEntitys == 0` (2D), the view is full-screen, and
`surf->material->GetName()` is `video/video_ipn_news` or `guis/assets/marscity/ipn`. This reliably
kills the flash and does not affect in-world IPN monitors (drawn on 3D geometry / small scissors).
Downside: hardcodes Mars City asset names in the renderer — a workaround, not a general fix.

A cleaner target, if the present-timing root can be found: prevent VK from presenting this one
transitional/capture frame during `ExecuteMapChange`/the hand-off, matching GL behavior.
