pep# HDR render pipeline — design

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
   `R_BackendSupportsEnhancements()` **and the frame draws a fullscreen world view**,
   lazily create a screen-sized **RGBA16F** color target with a **DEPTH24_STENCIL8**
   attachment (stencil shadows need the stencil), and `SetFrameTarget()` it. All
   `RC_SET_BUFFER` clears and every `RC_DRAW_VIEW` (3D subviews, main view, 2D GUI) then
   accumulate into it. **Worldless frames — the main menu, load/save GUI, cinematics —
   stay on the 8-bit path** (`rbHdrFrameActive` gates the whole frame,
   `RhiBackend.cpp:864`): the float resolve was lifting near-black scene detail through
   translucent 2D (the menu planet limb behind the LOAD GAME button) — fixed in `2b1f1a56`.
2. The backend's `frameTarget` concept makes `EndPass` return to the HDR FBO instead of
   the backbuffer, so nested target passes (shadow maps, SSAO) restore correctly. When
   `frameTarget == 0` (HDR off) behaviour is bit-for-bit the old path.
3. At `RC_SWAP_BUFFERS`, `RB_RHI_HdrResolve` binds the backbuffer and draws a fullscreen
   quad sampling the HDR color texture through `shaders/hdrresolve.*`. The existing
   `RB_RHI_GammaBrightness` then runs on the resolved backbuffer unchanged, followed by
   screenshot readback and the ImGui overlay — all on the backbuffer, exactly as before.

**Dither — tried, then removed (`2b1f1a56`).** An early version added a resolve-time
dither (`r_hdrDither`, default on): interleaved gradient noise (Jimenez 2014) remapped to a
triangular PDF, scaled to ~1 LSB, added before the 8-bit write to kill the *residual* bands
the float→8-bit resolve still leaves. It turned out to be a **dead-end**: once film grain and
chromatic aberration were folded into the resolve so they sample the float buffer directly
(below), the banding is gone without it. The cvar, its menu slider, and the resolve dither
code were all deleted — `hdrresolve.frag` no longer dithers, and there is no `r_hdrDither`.

**UI.** A single "HDR Rendering" toggle lives in the Enhancements tab's Post-Processing
section ([`Dhewm3SettingsMenu.cpp:1843`](../neo/framework/Dhewm3SettingsMenu.cpp),
`CVarOption( "r_hdr", … )`), alongside the standalone Film Grain / Film Grain Size /
Chromatic Aberration sliders; not wired into the quality presets (standalone).

**The whole post chain folds into the resolve.** FXAA, film grain and chromatic aberration
normally run as separate passes that snapshot the framebuffer into the 8-bit `_currentRender`
image, sample that, and write back. In HDR mode any such 8-bit round-trip re-quantizes the
smooth float scene into hard bands *before* the resolve runs — reintroducing exactly the banding
HDR exists to remove, whenever those effects were on. (This 8-bit round-trip is also what made the
old resolve dither pointless, and folding the chain in float is what let it be dropped.) Fix: in
HDR mode `RB_RHI_AAPass` and `RB_RHI_PostProcess` are skipped, and the chain is rebuilt in float:

- `RB_RHI_HdrFxaa` runs FXAA as a float→float pass (`rhiHdrRT` → `rhiHdrAaRT`, a second
  RGBA16F color buffer allocated only while `r_rhiAA` is on), reusing `fxaa.frag` unchanged
  (it samples whatever is bound to unit 0; screenCorrection/texel set for an exact-size source).
- `hdrresolve.frag` then reads the AA'd buffer (or the scene buffer if FXAA is off) and applies
  chromatic aberration → film grain → gamma/brightness, in that order, writing 8-bit **once**.
  (On Vulkan the `r_gammaInShader` correction is folded in here since the VK backend has no
  separate LDR gamma tail; GL passes identity and keeps its standalone gamma/brightness pass.)
- SMAA (`r_rhiAA 2`) goes one step further when chroma is off: its neighborhood-blend pass folds
  *into* the resolve too (`hdrresolve_smaa.frag` via `RB_RHI_HdrResolveSmaaFused`), so the
  `rhiHdrAaRT` round-trip is skipped entirely — edges+weights, then a single blend+grain+gamma pass to
  the backbuffer. Chroma-on falls back to the separate-blend path above (its radial offset taps need
  the AA'd image). See [antialiasing.md](antialiasing.md).

So the correct order (scene → AA → chroma → grain → gamma → 8-bit) is preserved, and everything
stays half-float until the single resolve write.

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
- Banding on the legacy 8-bit path (HDR off) is unaddressed — HDR is the fix, so turning it
  on is the remedy; there is no separate LDR dither pass.
- **Negative interaction output is floored** (`interaction.frag:435`): the RGBA16F target
  doesn't clamp, so a negative N·L used to *subtract* warm light and blue-shift models under
  HDR; the shader now floors to 0 to match the old 8-bit fixed-point clamp (`2b1f1a56`).

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
- **Bloom:** ✅ **BUILT** (`r_hdrBloom`, default off). Threshold the bright HDR scene at half res
  (`bloomthreshold.frag`, soft-knee, hue-preserving) → downsample chain (`bloomdown.frag`, 5-tap
  dual-filter) → tent-upsample-and-combine chain (`bloomup.frag`, 3×3 tent + in-shader add of each
  level's downsample) → the resolve bilinear-upsamples the half-res glow and adds it into the linear
  scene *before* the tonemap (so it exposes/rolls off like real light). `RB_RHI_Bloom` runs at the
  end of the primary 3D view (scene-only, like eye adaptation), on two single-level target chains
  (`rhiBloomD`/`rhiBloomU`) — deliberately NOT the mip-render path, and it combines in-shader because
  VK `BeginTargetPass` always clears (no additive load-preserve). `r_hdrBloomThreshold` sets the
  bloom cut-in — lower it to bloom fire/lava directly, which can stand in for the emissive overbright.
  opengl3 + Vulkan, tunable in Post-Processing.

### C-lite — additive self-illum overbright  *(BUILT, the minimal first slice)*

The smallest slice of Phase C that gives B0's tonemap and B1's eye-adaptation real range to
work on, without bloom or a light-injection overhaul. Rationale: stock content is authored to
LDR, so nothing exceeds `1.0` and adaptation just rescales a flat image — this creates the
bright anchors it needs.

- **What:** `r_hdrOverbright` (float, default `3.0`, range `1..8`, archive; `1` = off) scales
  **additive** material stages (`GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE` — the self-illum blend:
  lamps, monitor screens, fire, glares) so their RGB exceeds `1.0` in the float scene buffer.
  Active only with a tonemap curve on (`r_hdrTonemap>=1`, like `r_hdrExposure`), so the faithful
  mode-0 look is never touched. ImGui slider sits under the Tonemap combo (greys out with it).
- **Supersedes the legacy flare haze:** when overbright is active, `R_FlareDeform` forces the
  `r_flareSize` flare/glare deform ("the old light haze") to its `0`/off size — the lights are
  genuinely bright now, so the faked sprite double-counts. Restored when overbright is off.
  (Paired default retune: `r_hdrExposure` 2.33, `r_hdrOverbright` 3.0.)
- **Where:** one guard at the `parms.color` set in `RB_RHI_RenderShaderPasses`
  ([RhiBackend.cpp](../neo/renderer/rhi/RhiBackend.cpp)). No shader edit — `generic.vert`
  already multiplies `var_Color` by `u_color` (= `parms.color`), so scaling the stage colour
  scales the additive output. Both backends free (the pass is shared GL3+VK).
- **Faithfulness / gating:** applied only while `rbHdrActiveThisFrame` (the float target is
  bound — on the 8-bit path `>1` would clip to white); RGB only, alpha untouched; the `>1`
  guard makes `r_hdrOverbright 1` skip entirely, so the default and the legacy path are
  bit-identical. Non-vanilla, opt-in, not preset-wired.
- **Deliberately excluded (keeps it minimal):** no uniform light/diffuse multiply (that is just
  exposure, no *range*); no bloom; soft-particle, custom-ARB and texgen stages take an earlier
  branch and stay unboosted (a follow-up if stock/mod emissive customs need it).
- **Known tuning item:** additive *particles* (sparks, muzzle glares) also boost — usually
  desirable, but a bright spark field can blow out; a material-name gate (like
  `RB_RHI_ParticleLooksLikeSmoke`) is the escape hatch if needed.

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

## HDR *display* output (HDR10 / scRGB) — separate future feature, NOT a phase

**Everything above is HDR *rendering* with SDR *output*** — the float scene is always tonemapped
back down to Rec.709/sRGB and written to an 8-bit backbuffer, so **Phases A/B/C are fully visible
on an ordinary monitor** (banding gone, adaptation, bloom — all SDR-visible). None of them need an
HDR panel.

An **actual HDR display is only required for a distinct fourth capability**: sending the >1.0
values to the panel directly instead of compressing them into SDR — i.e. presenting into a
swapchain in a PQ (HDR10) or scRGB colorspace so a 1000-nit highlight is *emitted* as 1000 nits.
This is deliberately out of scope for the pipeline above; it is tracked here only so the design is
recorded.

**Why it's separate, not "Phase D":** it changes the *presentation/encode*, not the rendering. It
is also the **only** HDR work here that is hardware-gated (needs an HDR monitor + OS HDR mode) and
**Vulkan-only** — GL 3.3 has no portable path.

**Requirements when/if it's built:**
- **Swapchain colorspace.** `VK_EXT_swapchain_colorspace`, then either
  `VK_COLOR_SPACE_HDR10_ST2084_EXT` with a 10-bit format (`VK_FORMAT_A2B10G10R10_UNORM_PACK32`,
  PQ-encoded) or `VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT` (scRGB) with
  `VK_FORMAT_R16G16B16A16_SFLOAT`. Enumerate via `vkGetPhysicalDeviceSurfaceFormats2KHR` and fall
  back to the current sRGB swapchain when unsupported / OS HDR off.
- **A different resolve tail.** Instead of the sRGB clamp+gamma in `hdrresolve.frag`, encode to the
  target space: PQ (SMPTE ST 2084) with a nits mapping, or scRGB linear with SDR white pinned at
  1.0. Peak-luminance / paper-white cvars (`r_hdrDisplayPeakNits`, `r_hdrDisplayPaperWhite`).
- **No SDR tonemap** on this path (or a much higher-peak one) — the whole point is to *not* crush
  the range.

**Fork point: Phase B's tonemap.** B is where the float buffer first meets a tone curve, so that
is the clean branch — share the float scene + exposure up to that point, then split
`tonemap → SDR/sRGB` vs `encode → PQ/scRGB` as two resolve-tail variants. Building B with that
split in mind (exposure/curve separated from the final encode) keeps this path cheap to add later.

---

## Status

- **Phase A** — ✅ **DONE + USER-VERIFIED** behind `r_hdr` (default off) on **both GL3 and
  Vulkan**. User runs with HDR on and is happy with it in-engine. Got there via in-engine
  iteration: the worldless-frame 8-bit gate and the negative-interaction floor (`2b1f1a56`)
  were both fixes for artifacts observed running (menu planet-limb lift, blue-shifted models);
  the originally-planned resolve dither was tried and **removed** as a dead-end. Only open
  item is the deferred MSAA-in-HDR follow-up (use `r_rhiAA`/FXAA meanwhile).
- **Phase B0** (static tonemap) — ✅ **DONE + MERGED** (`r_hdrTonemap` 0..4 Off/Reinhard/ACES/
  AgX/Khronos-PBR-Neutral + `r_hdrExposure`, default 1.25, folded into both resolve paths;
  `neo/shaders/tonemap.glsl`; ImGui combo + exposure slider). Mode 0 stays bit-identical.
  GL3 + Vulkan. Pending only the user's final look-calibration of the curves.
- **Phase B1** (eye adaptation) — ✅ **BUILT** (`r_hdrEyeAdaptation`, default off; needs r_hdr +
  a tonemap curve). Pipeline: reduce the HDR scene to a 1×1 geometric-mean luminance via an
  `IF_R16F` log-luma mip chain (`hdrluma`/`hdrlumadown`, reusing the SSAO `CreateRenderTargetMipped`
  + `BeginTargetMipPass` infra) → ease an adapted exposure toward `r_hdrExposure * 0.18 / L`,
  clamped to `[r_hdrExposureMin, r_hdrExposureMax]`, into a **1×1 RGBA16F ping-pong** with an
  `exp(-dt/τ)` lag (`hdrexpose`, τ = 1/`r_hdrAdaptSpeed`; ping-pong cleared on creation for VK) →
  the resolve samples that 1×1 (`hdrresolve.frag` binding 1) instead of the static exposure. Runs
  at the top of `RB_RHI_HdrResolve` (SMAA fusion disabled when adaptation is on so only the plain
  path needs the sampler). ImGui: toggle + Adapt Speed / Exposure Min / Max under the Tonemap combo.
  GL3 + Vulkan, opt-in. Now that C-lite overbright pushes lights past 1.0, adaptation has real
  range to measure. **Pending user in-engine verify** (esp. VK, the swap-time offscreen passes).
  **Cinematics never adapt (2026-09-18).** While the main world view is a cinematic camera the
  pass stands down (static exposure, exactly as if it were off) and resumes by itself when
  gameplay returns - `r_hdrEyeAdaptation` is never touched, so there is nothing to restore and
  nothing to leak into the config if the game quits mid-cutscene. Detected from the view itself
  (`RB_RHI_CinematicView`, latched in `RB_RHI_DrawView`): `viewID == 0` (not a first-person eye)
  AND a near plane below 2 (`idGameLocal::SetCamera` sets `r_znear` to 1 for the duration of a
  cinematic, in every SDK-derived game DLL) - so it works with mods and needs no game-side
  signal. The adapted exposure stays in the pass's history, so gameplay resumes from where it
  was. Expect a one-frame exposure step at the cut into / out of a cutscene.
- **Phase C** — ✅ **C-lite BUILT** (`r_hdrOverbright`, additive self-illum overbright, default
  off; see the "C-lite" section above). Full Phase C (light injection + bloom) still planned.
- **HDR display output** (HDR10/scRGB) — separate future feature, **not started**; the only
  hardware-gated (needs an HDR panel), Vulkan-only part. Forks off Phase B's tonemap. See the
  "HDR *display* output" section above.

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
