# HDR render pipeline — design

Design reference for an **HDR render pipeline with SDR output** ("fake HDR") on the
GL 3.3 `opengl3` (RHI) backend, and its Vulkan port. Same enhancement framing as the
rest of Phase 3.5 (SSAO in [ssao-gtao.md](ssao-gtao.md), shadow mapping in
[shadow-system.md](shadow-system.md)): built on the RHI so it ports rather than being
re-derived, **opt-in and off by default**.

The motivation was fog banding, but the fix generalises: Doom 3 accumulates lighting
additively into an **8-bit backbuffer clamped to `[0,1]`**. Two consequences:

1. **Banding.** Wide, low-contrast gradients (fog, sky, soft particles) quantise to 256
   levels → visible bands.
2. **White-blast.** When lights sum past `1.0` the excess is destroyed *at write time*.
   No post-process can recover it — the information is already gone.

Accumulating the scene into a **half-float (RGBA16F)** target removes the clamp, which
fixes banding at the source and is the prerequisite for tonemapping / eye adaptation.

Output stays **SDR** (Rec.709/sRGB on an ordinary monitor). This is HDR *rendering*, not
HDR10/scRGB *display* output — that is a separate, later concern.

## Fidelity note

Doom 3 content was authored for the LDR clamp — blown-out lamp cores, muzzle flashes and
self-illum surfaces saturating to white at `1.0` are deliberate art. Phase A (straight
resolve, no tonemap) is a **look-neutral** change: it only replaces the 8-bit scene
buffer with a float one and copies it back 1:1, so the on-screen result is identical
except that banding is gone. Phases B and C **do** change the look and are therefore
separately gated and off by default. See the [fidelity note policy](shadow-system.md).

---

## Phase A — float scene buffer + straight resolve  *(implemented)*

Fixes banding everywhere with zero intended look change. Gate: `r_hdr` (default `0`).

**Frame flow (opengl3):**

1. `RB_GL3_ExecuteBackEndCommands` → after `BeginFrame`, if `r_hdr` and
   `R_BackendSupportsEnhancements()`, lazily create a screen-sized **RGBA16F** color
   target with a **DEPTH24_STENCIL8** attachment (stencil shadows need the stencil), and
   `SetFrameTarget()` it. All `RC_SET_BUFFER` clears and every `RC_DRAW_VIEW` (3D
   subviews, main view, 2D GUI) then accumulate into it.
2. The backend's `frameTarget` concept makes `EndPass` return to the HDR FBO instead of
   the backbuffer, so nested target passes (shadow maps, SSAO) restore correctly. When
   `frameTarget == 0` (HDR off) behaviour is bit-for-bit the old path.
3. At `RC_SWAP_BUFFERS`, `RB_RHI_HdrResolve` binds the backbuffer and draws a fullscreen
   quad sampling the HDR color texture through `shaders/hdrresolve.*`. The existing
   `RB_RHI_GammaBrightness` then runs on the resolved backbuffer unchanged, followed by
   screenshot readback and the ImGui overlay — all on the backbuffer, exactly as before.

**Dither (`r_hdrDither`, default on).** The float→8-bit resolve is the last quantization
point, so RGBA16F alone only removes the *banding from accumulation* — the resolve still
bands. `hdrresolve.frag` therefore dithers: a texture-free blue-noise-like value
(interleaved gradient noise, Jimenez 2014) remapped to a triangular PDF and scaled to ~1
LSB, added before the 8-bit write. `u_localParam0.x` carries the amount (0 = off). This is
what clears the *residual* bands RGBA16F leaves behind.

**UI.** "HDR Rendering" + a nested "Dither Resolve (steps)" slider live in the Enhancements
tab's Post-Processing section ([`Dhewm3SettingsMenu.cpp`](../neo/framework/Dhewm3SettingsMenu.cpp),
`enhancementOptions[]`); not wired into the quality presets (standalone).

**The whole post chain folds into the resolve.** FXAA, film grain and chromatic aberration
normally run as separate passes that snapshot the framebuffer into the 8-bit `_currentRender`
image, sample that, and write back. In HDR mode any such 8-bit round-trip re-quantizes the
smooth float scene into hard bands *before* the resolve dither runs — so the dither appeared to
do nothing whenever those effects were on. Fix: in HDR mode `RB_RHI_AAPass` and
`RB_RHI_PostProcess` are skipped, and the chain is rebuilt in float:

- `RB_RHI_HdrFxaa` runs FXAA as a float→float pass (`rhiHdrRT` → `rhiHdrAaRT`, a second
  RGBA16F color buffer allocated only while `r_rhiAA` is on), reusing `fxaa.frag` unchanged
  (it samples whatever is bound to unit 0; screenCorrection/texel set for an exact-size source).
- `hdrresolve.frag` then reads the AA'd buffer (or the scene buffer if FXAA is off) and applies
  chromatic aberration → film grain → dither, in that order, writing 8-bit **once**.
- SMAA (`r_rhiAA 2`) goes one step further when chroma is off: its neighborhood-blend pass folds
  *into* the resolve too (`hdrresolve_smaa.frag` via `RB_RHI_HdrResolveSmaaFused`), so the
  `rhiHdrAaRT` round-trip is skipped entirely — edges+weights, then a single blend+grain+gamma pass to
  the backbuffer. Chroma-on falls back to the separate-blend path above (its radial offset taps need
  the AA'd image). See [antialiasing.md](antialiasing.md).

So the correct order (scene → AA → chroma → grain → dither → 8-bit) is preserved, everything
stays half-float until the single resolve write, and the dither is the last thing before it.

**Key files:**
- Backend: `CreateRenderTargetColorDepthStencil(IF_RGBA16F,…)`, `SetFrameTarget()`,
  `frameTarget` in [`GL3Backend.cpp`](../neo/renderer/rhi/GL3Backend.cpp); GL enum guards
  in [`GL3Local.h`](../neo/renderer/rhi/GL3Local.h).
- Interface: [`RHI.h`](../neo/renderer/rhi/RHI.h).
- Orchestration: `RB_RHI_HdrBeginFrame` / `RB_RHI_HdrResolve` in
  [`RhiBackend.cpp`](../neo/renderer/rhi/RhiBackend.cpp).
- Shader: [`shaders/hdrresolve.frag`](../neo/shaders/hdrresolve.frag) / `.vert`, registered
  in [`GL3Shaders.cpp`](../neo/renderer/rhi/GL3Shaders.cpp).
- Cvar: `r_hdr` in [`RenderSystem_init.cpp`](../neo/renderer/RenderSystem_init.cpp).

**`_currentRender` is float in HDR mode (implemented).** Refraction, heat-haze and every
`SS_POST_PROCESS`-sort surface sample the `_currentRender` image, which the shared
[`idImage::CopyFramebuffer`](../neo/renderer/Image_load.cpp) snapshots from the framebuffer.
Left at RGBA8 it re-clamped and re-banded the float scene the moment a glass/refraction pane
sampled it — e.g. glass in front of fog banded even with `r_hdr` on. Fix: in an HDR frame
`CopyFramebuffer` captures into **RGBA16F**, reading `GL_COLOR_ATTACHMENT0` during the view (the
backbuffer isn't bound then) and `GL_BACK` afterwards. Off HDR (and on the legacy ARB2 backend)
it's the bit-for-bit vanilla `GL_RGB8` / `GL_BACK` path. Smoke-dark-blend and in-game camera
captures ride the same path for free.

Two separate signals keep this correct **and** cheap — `RB_RHI_HdrFrameActive()` (this is an HDR
frame, true its whole duration) picks the *format*; `RB_RHI_HdrCaptureActive()` (float FBO bound
right now, cleared at resolve) picks the *read source*. They must be separate: a format flip
forces a full-screen POT texture realloc, so if one signal drove both, any scene with a per-frame
`_currentRender` copy (heat-haze, smoke-dark-blend) *plus* the per-frame 8-bit gamma/brightness
capture would flip the format and realloc that texture **twice every frame** — a multi-ms stall
(this regressed a Potato frame from ~1.2ms to ~20ms before the split). Keying the format to the
whole frame means `_currentRender` holds one format all frame; the post-resolve gamma pass
captures the 8-bit backbuffer into the RGBA16F texture (a harmless upconvert) instead of forcing
it back. A realloc now happens only on the first HDR frame and on an `r_hdr` toggle — so toggling
still needs no `vid_restart`. Caveat: materials sampling `_currentRender.a` now read real
framebuffer alpha rather than the implicit 1.0 of an RGB8 texture — no stock material does, but
it's a behaviour change gated behind `r_hdr`.

**Known limitations (acceptable for A, addressed later):**
- **Hardware MSAA (`r_multiSamples`) is bypassed** while `r_hdr` is on — the scene renders
  into a single-sample HDR FBO, not the multisampled backbuffer. Use `r_rhiAA` (FXAA) for
  edge AA meanwhile; a multisampled RGBA16F target + resolve blit is a follow-up.
- Toggling `r_hdr` needs no `vid_restart`; the target is (re)created lazily and on resize.
- Dither runs only in the resolve, so it only helps while `r_hdr` is on. Banding on the
  legacy 8-bit path (HDR off) would need a separate dither pass — not done (HDR is the fix).

---

## Phase B — eye adaptation + tonemap  *(planned, off by default)*

Turns the float buffer into a filmic image. Gate: `r_hdrEyeAdaptation` (default `0`).

1. **Luminance measure.** Downsample the HDR color (mip chain or a small log-luma target)
   to an average scene luminance.
2. **Adaptation.** Smooth an exposure value toward that luminance over time
   (`exposure += (target - exposure) * (1 - exp(-dt / tau))`). The temporal lag *is* the
   eye-adaptation feel; expose `r_hdrAdaptSpeed`.
3. **Tonemap.** Fold exposure + a curve (Reinhard or ACES; `r_hdrTonemap`) into the resolve
   shader that already exists from Phase A — the resolve stops being a passthrough.
4. Clamp exposure range (`r_hdrExposureMin/Max`) so the reference look is preserved and dark
   rooms don't over-brighten.

**Caveat — the range problem.** Because stock materials are authored to LDR, little content
actually exceeds `1.0`, so naive adaptation just gently rescales an already-flat image.
Meaningful adaptation wants real HDR range, which motivates Phase C. Ship B with a
conservative default curve and a "reference vs cinematic" split so purists keep the stock
look.

---

## Phase C — dynamic range injection + bloom  *(planned, off by default)*

Gives adaptation something to work with, and adds the classic bright-light bleed.

- **Overbright lights / emissive boost:** optional multipliers pushing bright lights and
  self-illum surfaces above `1.0` so highlights carry real energy (`r_hdrOverbright`).
- **Bloom:** threshold the HDR buffer, blur, add back before tonemap. The Phobos bloom suite
  already transpiles (see [shaders/README.md](../neo/shaders/README.md)); reuse it.

---

## Vulkan port

Phase A ports cleanly and is arguably *more* natural on Vulkan:

- Vulkan has no default framebuffer; you already render into an explicitly created
  swapchain image and manage attachments and render passes yourself. An RGBA16F
  (`VK_FORMAT_R16G16B16A16_SFLOAT`) color attachment with a `D24_UNORM_S8_UINT` depth-stencil
  is a normal attachment description — the same object the swapchain path already builds,
  just a different format. There is no "bind FBO 0 vs an offscreen FBO" asymmetry to work
  around, so the `frameTarget` shim collapses into ordinary render-pass setup.
- The resolve becomes a second subpass/render pass sampling the HDR attachment and writing
  the swapchain image — the fullscreen-quad pattern is identical.
- MSAA is cleaner too: a multisampled RGBA16F attachment with a `resolveAttachment` is
  first-class in a Vulkan render pass, so the Phase A MSAA limitation goes away for free.
- Shaders already have a Vulkan path (`shaders/prelude.vk.glsl`); `hdrresolve.*` compile
  under it like the rest.

The only real Vulkan-specific work is format/feature checks
(`vkGetPhysicalDeviceFormatProperties` for RGBA16F color-attachment + blit/sample support,
universally available on desktop) and wiring the extra attachment into the existing
render-pass builder. The RHI methods added for Phase A (`CreateRenderTargetColorDepthStencil`,
`SetFrameTarget`) map directly onto that.

---

## Status

- **Phase A** — implemented behind `r_hdr` (default off). Needs in-engine verification
  (menus, mirrors/subviews, HUD, stencil shadows, screenshots) — headless build only so far.
- **Phase B** — planned.
- **Phase C** — planned.

### Vulkan port (M7) — as-built

Phase A now runs on the Vulkan backend too (`neo/renderer/rhi/vk/VulkanBackend.cpp`), same
`r_hdr` cvar and frontend driver (`RB_RHI_HdrBeginFrame`/`HdrResolve`). What the port added:

- `CreateRenderTargetColorDepthStencil(IF_RGBA16F)` → an RGBA16F color image + a
  D24S8/D32S8 depth-stencil (stencil shadows need it), with clear / load / clearDS render-
  pass variants mirroring the swapchain scene passes. Colour attachment ends each pass in
  `SHADER_READ_ONLY` so the resolve samples it through the ordinary descriptor path.
- Color-only `CreateRenderTarget(IF_RGBA16F/IF_RGBA8)` for the FXAA/SMAA ping (and future
  SSR buffers); `GetRenderTargetImage2` for MRT.
- `SetFrameTarget` re-routes `BeginPass`/`EndPass` into the HDR buffer; the pipeline cache
  key carries a pass-class byte so the same scene shaders get RGBA8-swapchain and
  RGBA16F-HDR pipeline variants (render-pass-incompatible colour formats).
- **Orientation:** a color render target is stored top-down (rendered like the scene),
  unlike the bottom-up M5 `_currentRender` captures, so any fullscreen pass sampling one
  (resolve/FXAA/SMAA) cancels the negative-height viewport flip. Glass/heat-haze captures
  during an HDR frame copy from the HDR buffer (with a `SHADER_READ_ONLY`↔`TRANSFER_SRC`
  round-trip); the capture is still RGBA8 (minor: 8-bit refraction sample vs GL3's RGBA16F).
- Default path unchanged: with `r_hdr 0` the frame target stays 0 and every scene/shadow
  pass behaves exactly as before M7.
