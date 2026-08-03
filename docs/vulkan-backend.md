# Phase 4 — Vulkan backend: milestone plan

Detailed bring-up plan for the `vulkan` backend. Hub: [vulkan-port.md](vulkan-port.md);
phase context: [port-phases.md](port-phases.md) §Phase 4. Planned 2026-08-02 at
`f7f6170c` from a fresh code survey (not the July one).

**Ground rules (unchanged from the plan):** legacy ARB and `opengl3` backends stay
untouched and the game stays playable on GL at every commit. Vulkan 1.1 baseline,
classic render passes (the `vulkan-rt` 1.3 profile is Phase 6). The backend is correct
when it produces **perceptually equivalent captures**, not matching API calls.
Validation-layer clean is part of every milestone's exit bar, not a Phase 5 afterthought.

## What Phase 4 inherits (as of 2026-08-02)

The groundwork is in noticeably better shape than when the plan was first written:

- **RHI interface** ([RHI.h](../neo/renderer/rhi/RHI.h)) — complete and battle-tested by
  the GL3 backend: buffers, geometry/uniform rings with `StreamGeneration` semantics,
  pipeline as `(stateBits, shader, vertexLayout, cullType)`, begin/end-scoped passes,
  the full offscreen render-target family (2D depth, cube depth, color+depth MRT,
  color+depth+stencil, `SetFrameTarget`), screen copies, immediate-mode debug draws.
  The GL3 backend is the executable specification.
- **Shaders** — 36 programs (72 `.vert`/`.frag` files) in `neo/shaders/`, all compiling
  clean for **both** targets today: `validate.py` runs 144 compiles (GLSL 330 and
  `-V --target-env vulkan1.0`) with 0 failures. `prelude.vk.glsl` already fixes the
  binding model: **set 0 = per-draw UBO, set 1 = combined image samplers**, unit numbers
  preserved from the ARB programs.
- **CMake** — `DHEWM3_VULKAN` option (default OFF): finds Vulkan + glslangValidator/glslc,
  bumps to C++17, defines `DHEWM3_VULKAN=1`, links `Vulkan::Vulkan`. VMA vendoring under
  `libs/vma/` was deliberately deferred to this phase.
- **ImGui** — the vendored ImGui tree already ships `imgui_impl_vulkan.{h,cpp}`; today
  only the opengl2/opengl3 impls are compiled ([CMakeLists.txt](../neo/CMakeLists.txt)
  `src_imgui`).
- **Enhancement suite** — shadow maps, SSAO, HDR, PBR, SSR, SMAA/FXAA, presets are all
  expressed in RHI terms and land here as a port (M7), not a re-derivation.

## The two structural gaps (called out honestly)

These are the real work items the original one-liner bring-up list glossed over:

1. **The RHI owns no images.** `GL3Backend::CreateImage` returns 0 by design
   ("engine images bridge via idImage until Phase 4"); every texture bind in the live
   engine goes through `idImage::Bind` — raw GL calls in
   [Image_load.cpp](../neo/renderer/Image_load.cpp). There is no GL under the Vulkan
   backend, so **image ownership must move into the RHI** during this phase: staging
   uploads, mipmaps, compressed DXT/S3TC formats (Doom 3's precompressed textures —
   BC1–BC3, universally supported on desktop Vulkan), cube maps, per-frame scratch
   uploads (cinematics), purge/reload across `vid_restart`. This is the single largest
   chunk of Phase 4 and is spread across M2–M5 below, pulled in as each pass needs it.
2. **The engine assumes a GL window + context everywhere at init.**
   [glimp.cpp](../neo/sys/glimp.cpp) always sets `SDL_WINDOW_OPENGL` and creates a GL
   context; `R_InitOpenGL` loads qgl pointers; the frontend routes into the RHI via
   `glConfig.coreProfile` checks (tr_render.cpp, tr_backend.cpp, draw_common.cpp …) and
   six call sites reference `rhi::GetGL3RHI()` directly (ImmediateMode.cpp,
   RhiBackend.cpp ×2, RhiWorld.cpp ×2, RenderSystem_init.cpp). All of that needs a
   backend-neutral seam **before** any Vulkan code is written — that's M0, a pure GL
   refactor.

## Decisions (recorded 2026-08-02)

- **opengl3 default flip deferred.** `r_graphicsAPI` keeps `opengl` (legacy) as default
  through Phase 4; the parity flip (opengl3 default, legacy → `opengl-legacy`) happens
  later, on its own. Keeping legacy easily reachable during Vulkan bring-up preserves
  the reference implementation for three-way comparisons.
- **SPIR-V is compiled at build time.** A CMake custom command mirrors `validate.py`
  exactly (prepend `prelude.vk.glsl`, inject `invariant gl_Position;` for vertex
  stages, textually resolve `#include`) and emits `<name>.<stage>.spv`; install copies
  them next to the loose GLSL. `LoadShader( name )` on the Vulkan backend picks the
  `.spv` pair through the same VFS. `reloadShaders` hot-reload initially requires a
  rebuild of the .spv files (dev-only limitation; a runtime-glslang path can come later
  — do not block bring-up on it).
- **The scene always renders to an offscreen color+depth/stencil image on Vulkan**,
  blitted to the swapchain at end of frame. Swapchain images stay single-purpose
  (present + ImGui overlay). This makes `_currentRender`/`_currentDepth` copies plain
  `vkCmdCopyImage`, keeps screenshots/SMAA/gamma passes uniform with the HDR path
  (which is already offscreen on GL3), and dodges swapchain-usage/format restrictions.
  Fidelity-neutral; one extra blit per frame.
- **Clip-space conventions** (GL Y-up, −1..1 depth vs Vulkan Y-down, 0..1 depth):
  negative-height viewport (core in 1.1 via maintenance1) fixes Y without touching
  shaders; the depth-range remap is a one-matrix pre-multiply applied where MVPs are
  built for the RHI path — `RB_RHI_SpaceMvp` and the 2D ortho setup in RhiBackend.cpp —
  gated on the active backend. Shaders stay identical across GL/VK.
- **Descriptor model** (matches `prelude.vk.glsl`): set 0 = one dynamic-offset uniform
  descriptor reused for every draw (the ring slice travels as the dynamic offset —
  the same `AllocUniforms` contract); set 1 = per-image combined-image-sampler sets,
  cached per (image, sampler) and freed with the image. Per-frame descriptor pools for
  anything transient. Core 1.1 only — no push descriptors, no descriptor indexing
  (those are `vulkan-rt` material, Phase 6).
- **Frames in flight: 2.** The geometry/uniform rings are host-visible,
  persistent-mapped, partitioned per frame slot and fenced — same
  lifetime contract the GL3 ring already honours ("contents live at least until the
  frame is presented"), so `StreamGeneration` semantics carry over unchanged.
- **Pipeline cache**: keyed `(stateBits, shader, vertexLayout, renderPass)` exactly like
  GL3's program/state cache, plus a `VkPipelineCache` persisted to disk under the save
  path to kill re-run warmup stutter.
- **C++17 stays confined to `neo/renderer/rhi/vk/`**; the RHI interface and everything
  else remain C++11.

## Milestones

Each is independently verifiable, lands as its own branch/merge, and leaves GL
backends bit-identical. Order rehearsed by the GL3 bring-up (Chunks A–G).

### M0 — Seams & scaffolding (GL-only refactor, no Vulkan code)  **[DONE 2026-08-02]**
- **[done]** `rhi::GetRHI()` backend selector (+ `SetActiveBackend`/`BackendType` in
  RHI.h, implemented in RhiBackend.cpp) replacing the six direct `GetGL3RHI()` call
  sites; GL3 runs through it.
- **[done]** `glConfig.rhiBackend` flag distinct from `coreProfile`;
  `R_BackendSupportsEnhancements()` returns it. 17 frontend checks that meant "RHI
  path" flipped (tr_render ×7, tr_backend, tr_rendertools ×2, RenderSystem.cpp,
  RenderSystem_init ×3 + gate, ImmediateMode); 13 that genuinely mean "GL core
  context" stayed (glimp/qgl, fixed-function image enables, GL3's GL init, ImGui GL
  impl pick, settings-menu restart logic). Executor renamed
  `RB_GL3_ExecuteBackEndCommands` → `RB_RHI_ExecuteBackEndCommands`.
- **[done, scoped]** glimp: `GLimp_VulkanProbe()` creates a hidden
  `SDL_WINDOW_VULKAN` window (no GL context, loads the Vulkan library) and logs
  `SDL_Vulkan_GetInstanceExtensions`, SDL2 + SDL3 paths. *Scope note:* making the
  real game window Vulkan-mode moved to M1 — with no backend to present, that path
  would be untestable dead code; the probe is the verifiable M0 slice. Verified on
  the target system: `VK_KHR_surface` + `VK_KHR_xlib_surface` via sdl2-compat.
- **[done]** CMake: VMA 3.3.0 vendored at `libs/vma/`; `rhi/vk/VulkanBackend.cpp`
  skeleton (GetVulkanRHI → NULL until M1) under the option; build-time SPIR-V step
  (`shaders/compile_spv.py`, mtime-incremental, mirrors validate.py; outputs to
  `<build>/shaders/spv`, dev-fallback define `DUDE_SHADER_SPV_DIR`);
  `imgui_impl_vulkan` compiled under the option.
- **[done]** `R_InitOpenGL`: `vulkan`/`vulkan-rt` values probe + warn + fall back to
  GL (archived cvar never wedges the boot); unknown values get their own warning.
- **Verified:** stock build boots legacy (default) and opengl3 unchanged;
  `-DDHEWM3_VULKAN=ON` builds + links (72/72 .spv compiled); `r_graphicsAPI vulkan`
  probes and falls back cleanly on both builds.

### M1 — Clear screen  **[BUILT 2026-08-02 — pending the user's visual check]**
- **[done]** `VulkanBackend : rhi::RHI` (rhi/vk/VulkanBackend.cpp, ~1000 lines):
  instance (validation behind `r_vkValidation`, default on; loader chatter about
  third-party implicit layers is reported but not counted against the clean bar),
  surface via SDL, device pick (`r_vkDevice` override; auto prefers discrete —
  picks the RTX 3080 Ti over the Raphael iGPU on the dev box), queues, VMA 3.3.0.
- **[done]** Swapchain: BGRA8/RGBA8 UNORM, present mode from `r_swapInterval`
  (1+ FIFO / 0 IMMEDIATE→MAILBOX / <0 FIFO_RELAXED), recreate on resize,
  swap-interval change, OOD/suboptimal; minimized handled (skip-frames).
- **[done]** 2 frames in flight (per-slot pool/fence/acquire-semaphore,
  per-swapchain-image release semaphore); offscreen scene image RGBA8 +
  D24S8-else-D32S8, clear/load render-pass pair, EndFrame blits scene → swapchain.
  Every presented frame is defined (auto-clear if no pass ran). A
  `presentedFrames` counter prints in the shutdown summary as the M1 diagnostic.
- **[done]** Executor gate (`vkClearOnly` in RB_RHI_ExecuteBackEndCommands): under
  BT_VULKAN frames run as begin / RC_SET_BUFFER-clear / present only — no draws,
  no idImage, no GL-only helper passes, until M2.
- **[done — the M1 war story]** the frontend touches qgl in places the plan's
  "init path assumes GL" understated: `GL_CheckErrors` (EndFrame + InitOpenGL +
  vid_restart), `idImage` generation at init via `ReloadAllImages` (generator
  functions like `R_BorderClampImage` call raw qgl *beyond* GenerateImage),
  `PurgeImage`, screenshot/stencil readbacks. All now guard on NULL qgl pointers,
  and the Vulkan branch of `R_InitOpenGL` explicitly **zeroes the qgl table** so a
  GL→Vulkan `vid_restart` can't leave stale callable pointers. These guards are
  the seam list M2's image-ownership work replaces one by one.
- **Verified headless:** boots + presents (validation clean); double `vid_restart`
  on Vulkan = three clean up/down cycles (X11 teardown race did not reproduce);
  cross-backend `opengl3 → vulkan → opengl3` switching in one session works
  (29/29 GLSL programs reload after coming back); legacy + opengl3 regressions
  unchanged. **Pending:** the user's eyeball check of the clear color
  (`r_clear 1` flicker) + live resize.

### M2 — Rings, pipelines, first textures: 2D GUI/console/menu  **[BUILT 2026-08-02 — pending the user's visual check]**

**As-built notes:** per-slot host-visible VMA rings (GL3 sizes ×2 slots); SPIR-V
module cache (`shaders/spv` VFS + `DUDE_SHADER_SPV_DIR` dev fallback); pipelines
keyed (GLS bits, shader, layout, cull) with the GL3 `ApplyState` decode; set 0 =
per-slot dynamic-UBO descriptor (range 2048 covers ArbParams' 1536), set 1 =
per-draw 9-sampler set from a per-frame pool with a 1×1 white dummy in empty
slots; `idImage` bridge: `rhiHandle` + `GenerateImage`→`CreateTexture2D`
(RGBA8 + CPU `R_MipMap` chain + sampler from filter/repeat), `Bind()` demand-loads
only, executor passes handles via `DrawArgs::textures[0]`. Clip conventions as
decided: z-remap in `RB_RHI_SpaceMvp`, negative-height viewport, GL-rect→VK-rect
conversion; the Y-flip makes idTech4's CW winding equal VK's CLOCKWISE front, so
the legacy cull mapping carries over unchanged. Deliberate M2 gaps: texgen stages
skipped (need cube images, M3+), custom-ARB stages degrade to generic (**no
runtime SPIR-V compiler** — transpiled materials need glslang/shaderc at runtime,
planned with M5), cinematics show black (M5), precompressed .dds auto-skips via
`textureCompressionAvailable=false` (BC formats M3), gamma-in-shader tail still
GL-only (M5/M6). "Vertex attribute not consumed" pipeline-creation warnings are
benign (shared vertex layout vs generic.vert) and left visible.

Original plan:
- Host-visible persistent-mapped vertex/index/uniform rings, per-slot partitioning,
  `StreamGeneration` bumps (`AllocVertices`/`AllocIndices`/`AllocUniforms`).
- `LoadShader` → `.spv` modules; pipeline layout per the descriptor model; pipeline
  cache keyed as decided; `GLS_*` state bits → VkPipeline state translation
  (blend/depth/stencil/mask/cull — the same bits GL3's `ApplyState` decodes).
- **Image ownership begins:** `CreateImage`/`UpdateBuffer`-style upload path via VMA
  staging, RGBA8 + DXT formats, mip chains, samplers derived from idImage filter/repeat
  state; the idImage bridge routes 2D textures (GUI, fonts, loading screens) through
  the RHI on this backend.
- `Draw` with `DrawArgs::textures[]` live (the GL3 8-unit loop that is inert today
  becomes the real bind path here).
- **Verify:** main menu, console, PDA/GUI overlays fully rendered on Vulkan — the same
  eyeball-vs-legacy bar Chunk C used on GL3.

### M3 — Depth prepass  **[BUILT 2026-08-02 — pending the user's in-game look]**
- **[done]** `RB_RHI_DrawWorld` runs under Vulkan up to the depth prepass and then
  returns (lights/shadows/SSAO/normal-prepass are the M4 slice); the ambient shader
  passes that follow depth-test `EQUAL` against the real prepass. `RB_RHI_BindUnit`
  gained the VK handle route (`rhiVkUnits[]` + `RB_RHI_VkTextures`); zfill's
  alpha-tested and solid draws carry their textures through `DrawArgs`.
- **[done]** `SetDepthRange` joined the RHI (GL: `glDepthRange`, VK: viewport
  min/max depth) — the weapon/model depth hacks route through it on RHI backends.
- **[done]** third render-pass variant `passClearDS` (color LOAD + depth/stencil
  CLEAR): the world-view begin clears ds only and must keep the frame's color —
  the clear-everything variant would have wiped the 2D layer under it.
- **[done]** `shaderClipDistance` device feature enabled (zfill.vert always writes
  `gl_ClipDistance[0]`; zero plane when unused, no enable needed on VK).
- **[done]** M5/M6/M7 paths gated under VK: soft particles (spuriously armed —
  the VK GenerateImage path records upload sizes, making `_currentDepth` look
  captured; also its program lookup was a direct `GL3_FindProgram`, now
  backend-neutral), fog/blend lights, `_currentRender` copy, film-grain/AA tail,
  debug tools (`RB_RenderDebugTools` does GL state setup before any cvar check).
- **Verified headless** (r_vkDumpNextFrame): Mars City runs on Vulkan (1100+
  frames, validation clean); the intro Traffic Monitoring GUI renders
  pixel-perfect; the hangar world view shows correctly-occluded emissive
  fixtures over a dark world — `EQUAL`-tested ambient stages surviving is
  direct evidence the prepass depth is right. GL3 map regression clean.

### M4 — Stencil shadows + interactions (the big one)  **[BUILT 2026-08-02 — pending the user's in-game look]**
- **[done]** Stencil state entered the RHI as a compact enum (`rhi::StencilState` in
  `PipelineDesc`): disabled / always / shadow-test (GEQUAL 128) / volume
  preload / z-pass / z-fail, plus mirror variants that swap the faces like the
  GL path's firstFace/secondFace swap. GL3 ignores the field (RhiWorld keeps its
  literal, now NULL-guarded qglStencil* calls); Vulkan bakes it into the pipeline
  key (bits 56..59) as two-sided `VkStencilOpState`. Empirically the CCW
  front-face setup preserves GL's facing, so the ops translate literally.
- **[done]** `SetPolygonOffset` (VK dynamic depth bias, always enabled with (0,0)
  as "off") and `ClearStencilBuffer` (`vkCmdClearAttachments` clipped to the
  current scissor — the per-light stencil clear) joined the RHI.
- **[done]** `CreateTextureCube` (six RGBA8 faces + CPU mips, clamp-to-edge) and the
  `GenerateCubeImage` bridge — the normalization/ambient cube maps sample for real.
- **[done]** Descriptor set 1 grew to 11 bindings (units 0-7, shadow cube 8, SSAO 9,
  occlusion 10 — interaction/ambientlight declare them all statically). Typed
  dummies for empty slots: white 2D, white cube, and 1×1 D32 depth-compare
  dummies for `sampler2DShadow`/`samplerCubeShadow` (units 7/8, real maps at M7).
- **[done]** Light loop runs under VK: stencil volumes (`VL_SHADOW` input was already
  in the pipeline path), interactions, translucent interactions. Shadow *maps* +
  SSAO/SSR/normal prepass stay gated to M7 (LogOnce; every light takes stencil).
  `RB_CreateSingleDrawInteractions`' scissor now routes through the RHI.
- **War story — the RXGB swizzle:** first lit frames were bright-left/dark-right
  with washed speculars. CPU-side interaction inputs dumped bit-identical across
  backends, and a temporary shader visualization proved the tangent-space light
  vectors matched too. The culprit: the shaders decode normal maps as RXGB
  unconditionally (`bump.x = bump.a`) because the GL upload path swaps red into
  alpha "even on tga normal maps"; the VK RGBA8 bridge uploaded plain TGA data, so
  every normal's x read as +1 (alpha=255) and all lighting leaned +X. The bridge
  now mirrors the swizzle for `TD_BUMP`.
- **Verified headless:** alphalabs1 A/B at lockstep 60fps — GL3 (stencil path) vs
  Vulkan mean abs pixel diff **0.87/255** with stencil shadows on, band means
  identical; Carmack's reverse (default) and z-pass both exercised; Mars City runs
  validation-clean (3000+ frames). GL3 regression clean.

### M5 — Translucents, fog, texgen, screen copies  **[BUILT 2026-08-03 — pending the user's in-game look]**
- **[done] Screen captures.** The RHI grew `CreateCaptureImage` / an 8-arg
  `CopyFramebufferToImage` / `RetireImage` / `UpdateTexture2D` (GL3 keeps its
  literal qglCopyTexSubImage2D path; the defaults no-op). On Vulkan,
  `idImage::CopyFramebuffer/CopyDepthbuffer/UploadScratch` route to them with
  the same POT-oversize bookkeeping as GL. **Orientation contract:** the scene
  image is top-down (negative-height viewport keeps GL matrices), so *color*
  captures blit with a vertical flip into GL's bottom-up memory layout —
  explicit-texcoord consumers (the player-view `_scratch` warps: double vision,
  berserk, tunnel vision) sample byte-identically to GL. gl_FragCoord-based
  consumers add `u_windowCoord.w` (or `u_localParam1.w`) to the row term — 0 on
  GL, `vidHeight/h` on VK (portalsky, heathaze_mask/maskvertex, colorprocess,
  softparticle's smoke-dark tap). *Depth* captures stay native top-down
  (their only consumers address by fragCoord, which is native per backend; and
  `RB_RHI_SpaceMvp`'s `z' = 0.5(z+w)` remap makes VK window depth bit-identical
  to GL, so softparticle's depth constants hold unchanged). Copies run mid-pass:
  the render pass suspends (color already sits in TRANSFER_SRC between passes,
  depth round-trips through TRANSFER_SRC), `EnsureScenePass` resumes.
  `idImage::rhiCaptured` distinguishes "really captured" from demand-load
  leftovers (which set uploadWidth under VK).
- **[done] Mid-frame image lifetime:** `RetireImage` + per-slot retired-image
  lists (the retiredRings pattern) — capture/cinematic reallocation never
  stalls and never frees views this frame's recorded descriptor sets still use.
- **[done] Cinematics:** first frame / size change recreates the texture
  (blocking upload — new image, nothing references it); steady-state frames
  restage through a new per-slot TRANSFER_SRC ring (2 MB, grows) with in-cb
  barriers ordering against earlier samples. Cube-map cinematics warn once
  (unused by stock content). `RB_RHI_BindStageImage` runs the full
  `ImageForTime` → `UploadScratch` flow under VK.
- **[done] Fog + blend lights ungated:** `RB_RHI_FogAllLights` runs under VK —
  the chains already drove VK-aware `RB_RHI_BindUnit`; they now fill
  `DrawArgs` via `RB_RHI_VkTextures` and the raw stencil enable/disable is
  qgl-guarded (VK pipelines carry SS_DISABLED anyway).
- **[done] Texgen stages ungated:** skybox/wobblesky/diffuse-cube/reflect-cube
  bind their cube images through `DrawArgs::textures[0]` (the descriptor
  writer uses each image's own view, cube views included; M4's
  `CreateTextureCube` supplies them), bumpyenvironment adds the bump map on
  unit 1. TG_SCREEN got a correctness fix **on both backends**: mirror/xray
  stages (`mirrorRenderMap` sets TG_SCREEN + `texture.dynamic`) now sample
  their own subview capture (`_scratch`) as legacy did, not `_currentRender`.
- **[done] Subviews/mirrors:** `RC_COPY_RENDER` ungated (mirror/remote/xray
  captures land through the VK copy), and the `CropRenderSize`/`UnCrop` M4
  no-op gates are removed — cropped re-renders + captures work for real, which
  restores the damage double-vision warp (and berserk/tunnel vision) on Vulkan.
- **[done] Custom ARB via hand-translated builtins:** no runtime SPIR-V yet;
  instead the stock programs map to their Phase-2 hand translations
  (crossdiff-verified 120/120): heatHaze/WithMask/WithMaskAndVertex →
  `heathaze*`, colorProcess → `colorprocess`. New `SK_BUILTIN_ARB` stage kind +
  `RB_RHI_RenderBuiltinArbStage` (RenderParams model: vertexParms →
  u_localParam0/1, fragmentProgramImages → units, capture images must be
  rhiCaptured). Covers the base game; d3xp customs (enviroSuit, flare,
  motionBlur, glasswarp, bloodOrb, portalSky) still skip with a warning —
  runtime SPIR-V arrives with the d3xp backend flip.
- **[done] Soft particles ungated** (the `_currentDepth` capture is real now);
  the smoke-dark blend samples its GL-layout color capture through the same
  flip term.
- **Verified headless:** alphalabs1 lockstep A/B vs GL3 structurally identical
  (5.2/255 vs a lossy-JPEG reference + lamp-flicker phase; M4's TGA metric was
  0.87); mars_city1 runs validation-clean 5300+ frames with video screens and
  cinematics live; GL3 renders normally after the shared-path edits (flip
  terms are inert at w=0 on GL). **Pending: the user's in-game look.**

### M6 — Debug tools, screenshots, ImGui
### M6 — Debug tools, screenshots, ImGui  **[BUILT 2026-08-03 — pending the user's in-game look]**
- **[done] Screenshots:** `ReadPixelsRGB` reads a rect of the last completed frame out
  of the scene image (it persists between frames as `TRANSFER_SRC`) into the exact
  `glReadPixels(GL_RGB)` contract the callers expect — GL window coords, bottom-up rows,
  4-byte row padding — via a synchronous queue-idle + `vkCmdCopyImageToBuffer`. Wired
  into the swap-capture path (`RB_RHI_CaptureNextSwap` serviced after `EndFrame`),
  `CaptureRenderToFile`, and `R_ReadTiledPixels` (envshots/probes). **The glass room
  probes are re-enabled on Vulkan** now that captures work (the M5 gate is gone).
- **[done] ImGui** via the vendored `imgui_impl_vulkan`: a LOAD render pass on the
  swapchain image (initialLayout `TRANSFER_DST` after the scene blit → finalLayout
  `PRESENT`), per-image views/framebuffers rebuilt on swapchain recreation (the pass
  object survives; a surface-format change recreates it + `CreateMainPipeline`). The
  glue lives in [VulkanImGui.h](../neo/renderer/rhi/vk/VulkanImGui.h) (impl in
  VulkanBackend); [sys_imgui.cpp](../neo/sys/sys_imgui.cpp) grew a `useVulkanBackend`
  branch (`InitForVulkan` + glue) and init moved to `R_InitOpenGL` after the backend is
  up (glimp defers when `windowIsVulkan`). `RC_SWAP_BUFFERS` → `ImGuiHooks::EndFrame`
  hands the draw data over; the backend renders it **between the scene blit and present**,
  so menus stay out of screenshots (which read the scene image), exactly like GL.
- **[done] `DrawImmediate`** (24-byte `imVert_t`, locations 0/1/5): new `VL_IMMEDIATE`
  vertex layout + per-call primitive topology baked into the pipeline key (a GL primMode;
  -1 = triangle list for every normal draw). Non-indexed draw through the generic
  program, fixed alpha-blend/depth-LEQUAL pipeline. `GL_LINE_LOOP` → line strip (no VK
  analogue; open loop, cosmetic).
- **[done] Debug tools ungated on VK:** `RB_RenderDebugTools` runs the
  idImmediateMode-based subset (game debug lines/polygons + `r_showPortals`); the
  surface-indexed views (`r_showTris`/`r_showNormals`/…) stay GL-only until the core
  `RB_DrawElements` path — the **same gap the GL3 core backend has** — with a one-time
  notice. Shared helpers made qgl-NULL-safe (`GL_State`/`GL_Cull` no-op,
  `RB_SimpleWorldSetup`/`SurfaceSetup` route scissor through the RHI, the enabled debug
  functions guard their bare qgl) so the debug path can't crash without a GL context.
- **Verified headless:** in-game screenshot pixel-correct (right orientation, full
  frame); ImGui init + settings-window render + `vid_restart` cycles validation-clean;
  the `r_showPortals` outline renders through `DrawImmediate` (validation clean); GL3
  regression clean. **Pending: the user's in-game look** (F10 menu interaction,
  screenshot, glass with probes re-enabled).

### M7 — Enhancement suite port (together, as planned)
- The render-target family on VMA images: 2D depth (projected shadow maps), cube depth
  (point-light shadows), color+depth (+MRT — SSAO normal G-buffer, SSR
  roughness/metalness), color+depth+stencil (HDR scene buffer), `SetFrameTarget`
  re-routing, `BeginCubeFacePass`.
- Then the suite in dependency order: shadow maps → SSAO/GTAO → HDR pipeline → PBR →
  SSR → SMAA/FXAA → film grain/chroma/gamma — same cvars, same Enhancements tab, same
  presets; shaders already compile to SPIR-V.
- **Verify:** each feature A/B'd against its GL3 counterpart (RenderDoc side-by-side);
  presets Potato→Nightmare apply and detect correctly on Vulkan.

**As-built so far:**
- **Shadow maps [BUILT]** — 2D depth (projected/spot) + cube depth (point) targets,
  one shared depth-only render pass, LEQUAL-compare sampler; `r_shadowMapping` no longer
  `!vkMode`-gated (`RhiWorld.cpp`), alpha-tested casters feed unit-0 coverage.
- **HDR pipeline [BUILT, pending in-engine check]** — the color render-target family on
  VMA images: `CreateRenderTargetColorDepthStencil` (RGBA16F + D24S8/D32S8 scene buffer),
  color-only `CreateRenderTarget` (RGBA16F FXAA/SMAA ping, later SSR buffers),
  `GetRenderTargetImage2`, and `SetFrameTarget` re-routing the whole scene into the HDR
  buffer. The pipeline cache key gained a "pass class" byte (bits 24-31) so scene shaders
  build render-pass-compatible variants for the RGBA8 swapchain path (class 0) and the
  RGBA16F HDR path (class 2); `BeginPass`/`EndPass`/`GetPipeline` route by the active
  destination. Fullscreen post passes that *sample* a color target (resolve/FXAA/SMAA)
  cancel the negative-height flip — those targets are stored top-down, unlike the
  bottom-up M5 captures. `_currentRender`/`_currentDepth` captures follow the active frame
  target (glass-in-HDR works; the capture is still RGBA8, an 8-bit refraction sample vs
  GL3's RGBA16F — a minor fidelity gap to close later).
- **SSAO/GTAO [BUILT, pending in-engine check]** — `CreateRenderTargetColorDepth` (RGBA8
  color + depth, +MRT) via the same color-target machinery; normal G-buffer prepass + the
  ssao/ssao_blur/ssao_temporal fullscreen passes un-gated on Vulkan. The multitexture the
  post passes bind by raw GL on GL3 (normal/material/history buffers) is routed through a
  new `RB_RHI_BindRTUnit` → `rhiVkUnits` (extended to 11) → `RB_RHI_VkTextures`; DrawArgs
  gained `ssao`/`occlusion` fields bound at descriptor units 9/10 (were dummies). The AO
  buffer is recorded once before the light loop and rides every interaction/ambient draw.
  `r_ssaoDebug 1/2` works on Vulkan. Benign validation warning: gbuffer.frag always writes
  the SSR MRT output, discarded on the SSAO-only (1-attachment) target — same as GL.
- **SSR [explicitly deferred on Vulkan]** — `RB_RHI_ScreenSpaceReflections` early-returns on
  BT_VULKAN (LogOnce) and NormalPrepass skips its MRT there. It was never `vkMode`-gated —
  it used to bail because its targets returned 0; once `CreateRenderTargetColorDepth` worked
  it proceeded into its raw-GL march/composite binds and crashed. Porting it = un-gate +
  route those binds through `RB_RHI_BindRTUnit` like SSAO. PBR (`r_pbr`) rides the
  interaction shaders and already runs on Vulkan (verified crash-free with dude_preset 5).

**Phase exit = the plan's final milestone:** Mars City loads; identical light/shadow
behaviour; no missing interactions; RenderDoc shows depth, stencil, and interaction
passes; perceptual-equivalence bar met. Then Phase 5 (validation & polish, backend
switching hardening, 1.1-profile check) takes over.

## Risks / watch list

- **Image-ownership sprawl** — Image_load.cpp's generator paths accumulated 20 years of
  special cases (border clamp emulation, scratch images, partial uploads). Mitigation:
  bridge per-category as each milestone pulls it in (M2 2D → M4 cube/falloff → M5
  scratch/cinematic), never "port Image_load wholesale".
- **Pipeline permutation warmup** — first-use `vkCreateGraphicsPipelines` stutter where
  GL3 pays a cheaper program+state switch. Mitigation: disk-persisted VkPipelineCache
  (decided) + optional prewarm of the known `(stateBits, shader)` set at level load.
- **The X11 teardown race** ([known-bugs.md](known-bugs.md), mitigated 2026-08-01) —
  Vulkan's heavier re-init exercises exactly that path; re-verify under repeated
  `vid_restart` early (M1), not at the end.
- **sdl2-compat surface quirks** — this fork runs SDL2 API over SDL3;
  `SDL_Vulkan_CreateSurface` goes through the compat layer. Works, but if it misbehaves,
  the native-SDL3 move that was deferred "to Vulkan" is the fallback — flag it early.
- **Stencil format portability** — D24S8 isn't universal on Vulkan (notably missing on
  AMD Windows drivers historically); the backend must fall back to D32S8
  (`IF_DEPTH24_STENCIL8` already documents "backend may substitute D32S8").
