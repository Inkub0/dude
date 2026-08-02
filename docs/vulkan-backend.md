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

### M0 — Seams & scaffolding (GL-only refactor, no Vulkan code)
- `rhi::GetRHI()` backend selector replacing the six direct `GetGL3RHI()` call sites;
  GL3 keeps working through it.
- `glConfig.rhiBackend` (or equivalent) flag distinct from `coreProfile`: "frontend
  routes through the RHI", true for opengl3 **and** vulkan.
  `R_BackendSupportsEnhancements()` switches to it. The scattered
  `glConfig.coreProfile` frontend checks that actually mean "RHI path" flip to the new
  flag; the ones that genuinely mean "GL core context" (glimp, qgl setup) stay.
- glimp split: when `r_graphicsAPI vulkan`, create the window **without**
  `SDL_WINDOW_OPENGL` (SDL_WINDOW_VULKAN), skip GL context creation and qgl loading,
  query `SDL_Vulkan_GetInstanceExtensions`. Both SDL2 and sdl2-compat/SDL3 paths.
  `vid_restart` teardown ordering mirrored from the GL path (mind the mitigated X11
  teardown race in [known-bugs.md](known-bugs.md) — re-verify it here).
- CMake: vendor VMA under `libs/vma/`; add `rhi/vk/` sources under the option; add the
  SPIR-V build step; compile `imgui_impl_vulkan` under the option.
- `R_InitOpenGL` accepts `r_graphicsAPI vulkan` when built with `DHEWM3_VULKAN`
  (falls back with a warning otherwise, as today).
- **Verify:** stock GL build byte-for-byte behaviour; `-DDHEWM3_VULKAN=ON` build links;
  `r_graphicsAPI vulkan` reaches a clean "backend not yet implemented" stub without
  touching GL.

### M1 — Clear screen
- `VulkanBackend : rhi::RHI` skeleton: instance (validation layers behind a
  `r_vkValidation` cvar, default on for dev builds), surface via
  `SDL_Vulkan_CreateSurface`, physical-device pick (`r_vkDevice` index override),
  device + graphics/present queues, VMA initialisation.
- Swapchain: format/colorspace pick, present mode from `r_swapInterval`
  (0 → IMMEDIATE, 1 → FIFO, −1/adaptive → FIFO_RELAXED, MAILBOX where it fits the
  menu's Disabled/Enabled/Adaptive triple), recreate on resize/`vid_restart`/out-of-date.
- 2 frames in flight: per-slot command buffer, fence, acquire/present semaphores.
- The offscreen scene image (color + D24S8-or-D32S8) + its render pass; `BeginFrame` /
  `BeginPass(clear)` / `EndPass` / `EndFrame` → blit to swapchain → present.
- **Verify:** boots to the clear color on `r_graphicsAPI vulkan`; window resize,
  `vid_restart`, and clean shutdown all validation-clean.

### M2 — Rings, pipelines, first textures: 2D GUI/console/menu
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

### M3 — Depth prepass
- World traversal already reaches the backend through `RB_RHI_DrawWorld`; bring up the
  zfill path: `zfill.vert/.frag`, depth-only pipelines, alpha-tested prepass surfaces
  (needs the diffuse bind from M2), portal/subview scissors, the MVP clip-space
  pre-multiply from the Decisions section.
- **Verify:** RenderDoc depth-buffer capture matches an opengl3 capture of the same
  scene (Mars City start).

### M4 — Stencil shadows + interactions (the big one)
- Complete image ownership for the interaction inputs: normal/specular/diffuse stages,
  light projection + falloff textures, cube maps (`VL_SHADOW` layout, six-face uploads).
- Shadow volume pipelines: two-sided stencil (core), Carmack's-reverse depth-fail
  preserved exactly; incr/decr wrap; the `GLS_*` stencil bits translate directly.
- Interaction pass: `interaction.vert/.frag` (already SPIR-V-clean), one pipeline per
  state permutation, scissored per light like GL3.
- **Verify:** the Phase 4 exit criteria from the plan largely live here — Mars City
  lighting/shadow behaviour indistinguishable from opengl3; RenderDoc shows the
  depth → stencil → interaction pass structure; no missing interactions.

### M5 — Translucents, fog, texgen, screen copies
- Blend/translucent stages (`generic`), fog + blend lights, texgen stages (skybox,
  portal sky, cube reflections/`environment`, `bumpyenvironment`), heatHaze family.
- `CopyFramebufferToImage` = `vkCmdCopyImage` out of the offscreen scene image
  (`_currentRender`/`_currentDepth`); cinematic scratch textures (per-frame
  `UploadScratch` → ring-buffered staging upload).
- Subviews/mirrors (nested view rendering into the same scene image with scissor,
  as GL3 does).
- **Verify:** glass, fog volumes, sky, mirrors, in-game video screens all correct;
  three-way spot checks vs opengl3/legacy.

### M6 — Debug tools, screenshots, ImGui
- `DrawImmediate` (the 24-byte imVert_t contract) for `r_showTris`/`r_showNormals`/…
- Screenshots: copy scene image to a host-visible buffer, feed the existing capture
  path (`RB_RHI_CaptureNextSwap` analogue).
- ImGui via the vendored `imgui_impl_vulkan` (render into the swapchain pass after the
  scene blit), init/teardown wired in [sys_imgui.cpp](../neo/sys/sys_imgui.cpp) beside
  the GL impls.
- **Verify:** settings menu (incl. Enhancements tab UI) usable on Vulkan; screenshot
  command produces correct images; debug cvars draw.

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
