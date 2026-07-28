# Antialiasing — post-resolve AA (FXAA now, SMAA/TAA later)

## Status

- **FXAA — IMPLEMENTED** as the first filter on the post-resolve rails. Cvar `r_rhiAA`
  (0 = off, 1 = FXAA), default off, archived, opengl3/Vulkan only. Pass `RB_RHI_AAPass`
  (`neo/renderer/rhi/RhiBackend.cpp`) runs over the finished 3D view before grain/chroma and
  2D/GUI; shaders `neo/shaders/fxaa.{vert,frag}` (registered in `gl3BootPrograms[]`); menu combo in
  the Enhancements tab. Self-contained (no external LUTs), validated + builds.
- **SMAA 1x — PENDING.** Needs the AreaTex/SearchTex LUT assets; drops onto the same rails as a
  higher `r_rhiAA` value (see below).
- **TAA — PENDING.** Reuses the temporal-SSAO machinery; blocked on per-object motion vectors (below).

The sections below are the original design sketch; SMAA/TAA remain the planned upgrades.

---


## Background: what the built-in "Antialiasing" option is

The Video-options "Antialiasing" slider maps to **`r_multiSamples`** — hardware **MSAA** requested
at GL-context creation via `SDL_GL_MULTISAMPLESAMPLES` (`neo/sys/glimp.cpp`). Menu exposes
`No AA / 2x / 4x / 8x / 16x` (`Dhewm3SettingsMenu.cpp`, `1 << index`). dhewm3's only change vs. stock
Doom 3 is a **graceful fallback**: if the requested sample count fails, context creation retries lower
and writes the granted value back into `r_multiSamples` (`glimp.cpp:454/517/641/783`).

**It is NOT inert in the RHI/GL3 path.** The main lit scene renders straight to the backbuffer
(render target 0): `BeginTargetPass` is only ever called for shadow maps, the SSAO normal prepass, and
the SSAO ping-pong/history buffers (`RhiWorld.cpp`). The `currentRender` capture reads *back from* the
backbuffer (`RhiBackend.cpp:202`, `CopyFramebuffer`), confirming the scene lives there. So backbuffer
MSAA antialiases scene silhouettes in both legacy and RHI.

What MSAA does **not** fix — and never will — is the signature Doom 3 aliasing: **specular / normal-map
shimmer** (shaded-pixel aliasing crawling as the camera moves). That is the job of a post-resolve AA.

## Design: two knobs, one familiar control

- **Legacy OpenGL path:** leave `r_multiSamples` exactly as-is (backbuffer MSAA). No change, stays
  byte-faithful.
- **RHI path:** reuse the *same* `r_multiSamples` value as an AA-quality selector for an RHI-native
  post-resolve pass. `0` = off; `2x/4x/8x/16x` map to a quality preset. One menu control, better result
  on the enhanced backend. Fits the fidelity policy: legacy untouched, RHI improvement opt-in via the
  existing slider.

A new cvar `r_rhiAA` (0 = follow MSAA off, 1 = SMAA, 2 = TAA when implemented) can disambiguate later;
start by gating SMAA behind `r_multiSamples > 0 && R_BackendSupportsEnhancements()`.

## SMAA 1x — the "cheap and good, no temporal risk" starting point

Runs entirely post-resolve on the composited scene texture. No jitter, no motion vectors, no history →
no ghosting. Sharper than FXAA. Does not fix specular shimmer (that's TAA's job) but cleans edges,
including alpha-tested foliage/grate edges the MSAA path handles poorly.

### Where to hook
End of the 3D view, **after the scene composite but before the 2D/GUI pass**, so HUD/menu text stays
crisp (SMAA would otherwise soften text). Mirror `RB_RHI_SSAODebugOverlay`'s call site, which already
runs "after the scene is drawn, before the light passes overwrite it."

### Passes (reuse existing machinery)
All three are fullscreen triangles via `RB_RHI_DrawFullscreen(r, prog, parms, tex0)`, offscreen targets
via `r->CreateRenderTarget(rhi::IF_RGBA8, w, h)` + `BeginTargetPass/EndPass`, extra units bound directly
like the SSAO normal buffer.

0. **Capture:** copy backbuffer → `sceneTex` (`currentRenderImage->CopyFramebuffer`, same call the
   `currentRender` path already uses). SMAA needs the scene as a sampleable texture.
1. **Edge detection** (`smaa_edges.frag`): sample `sceneTex` luma → `edgesRT` (RG8 is enough).
2. **Blend-weight calc** (`smaa_weights.frag`): sample `edgesRT` + the two precomputed SMAA LUTs
   (`AreaTex`, `SearchTex`) → `weightsRT` (RGBA8). Ship the LUTs as two images loaded once
   (they're the standard SMAA constant textures).
3. **Neighborhood blend** (`smaa_blend.frag`): sample `sceneTex` + `weightsRT`, write the resolved
   image back to the **backbuffer** (`BeginTargetPass(0, ...)`).

### New files
- `neo/shaders/smaa_common.glsl` (or inline), `smaa_edges.{vert,frag}`, `smaa_weights.{vert,frag}`,
  `smaa_blend.{vert,frag}`.
- Register the three program names in `gl3BootPrograms[]` (`GL3Shaders.cpp`), next to the `ssao*`
  entries.
- Two LUT textures (`AreaTex`, `SearchTex`) added to the image manager.
- `r_rhiAA` cvar + menu wiring in `Dhewm3SettingsMenu.cpp` (or fold into the existing MSAA slider).

### Cost
Two RGBA8 half-ish targets + one backbuffer-sized copy + 3 fullscreen draws. Cheap; comparable to the
existing SSAO chain. Quality preset from `r_multiSamples` can scale edge threshold / search steps.

## TAA — add later (the specular-shimmer fix)

TAA is the only cheap option that actually resolves the specular/normal-map shimmer, and **we are
~80% wired for it already**: the temporal-SSAO path (`ssao_temporal.{vert,frag}`, `RhiWorld.cpp:2388+`)
already implements the exact machinery — a **ping-pong history buffer** (`rhiSsaoHistRT[2]`), a
**camera reprojection matrix** (view→world→previous-clip), and a **neighborhood clamp** to kill
ghosting. TAA reuses all of it, one layer up (color instead of AO).

### What TAA adds on top of the SSAO temporal pattern
1. **Camera jitter:** offset the projection matrix by a sub-pixel Halton(2,3) sequence each frame
   (a per-frame translation on `projectionMatrix`). The SSAO path already advances a golden-ratio
   jitter phase — same idea, applied to projection.
2. **History resolve pass** (`taa_resolve.frag`): blend current `sceneTex` with the reprojected
   history, neighborhood-clamped in YCoCg, feedback ~0.9. Mirror `ssao_temporal.frag` almost verbatim,
   swapping the AO scalar for RGB color.
3. **Ping-pong color history:** two backbuffer-sized RGBA (ideally RGBA16F) targets, same
   `rhiSsaoHistRT[writeIdx/readIdx]` bookkeeping.

### The one real prerequisite: motion vectors
The SSAO temporal pass reprojects with a **camera-only** matrix because the world is static for AO
purposes. That is fine for a static scene but will **ghost/smear on moving geometry** (monsters, the
player weapon, movers/elevators) under color TAA. Proper TAA needs **per-object motion vectors**:

- Add a velocity output — either a small extra render target written during the main pass (prev-frame
  MVP vs. current per surface), or reconstruct camera motion from depth for static geometry and accept
  weapon/monster ghosting as a known limitation of a v1.
- The weapon view-model is the worst offender (always moving in screen space); consider excluding it
  from history (velocity-based or a stencil bit) if full motion vectors are deferred.

### Decision record
- **SMAA 1x first** (no temporal risk, drop-in, cleans edges) — do this before TAA.
- **TAA second**, gated on building the motion-vector pipeline. Keep SMAA 1x as the non-temporal
  alternative in the menu for users who dislike TAA softening/ghosting.
- Fidelity: both are post effects that never touch lighting math → clean opt-in toggles. TAA is the
  only one that slightly softens frame-to-frame; ship it as a toggle, not a default.

### Sizing / gotchas
- If we ever move the scene into a **multisampled offscreen** target instead (true MSAA-in-RHI),
  `CopyFramebuffer` and the SSAO depth reads need an explicit MSAA→single-sample **resolve** first.
  Post-resolve SMAA/TAA avoid this entirely by operating on the already-single-sampled backbuffer —
  which is a big reason they're the cheaper "good-looking" win here.
- Run AA **before** the 2D/GUI composite so HUD/menu text isn't blurred.
- Reuse `RB_RHI_ForgetTexBinds()` after the direct-bound units, exactly as the SSAO passes do, so the
  following passes re-issue their binds.
