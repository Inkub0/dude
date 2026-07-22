# Port architecture & constraints

Stable technical context for the renderer port: the code survey it started from,
the hardware baseline / backend-switch design, known risks, and prior art.
See [vulkan-port.md](vulkan-port.md) for the hub and phase status.

## Current renderer facts (from code survey)

- **Every GL call goes through `qgl*` function pointers** ([qgl.h](../neo/renderer/qgl.h)).
  125 distinct GL functions are used across 21 files — a precisely measurable surface.
  Regenerate the inventory with:
  `grep -rhoE 'qgl[A-Za-z0-9]+' neo --include='*.cpp' --exclude='qgl*' | sort | uniq -c | sort -rn`
- **Frontend/backend split already exists**: the frontend builds a command list
  (`RC_DRAW_VIEW`, …) that `RB_ExecuteBackEndCommands` in
  [tr_backend.cpp](../neo/renderer/tr_backend.cpp) executes. All porting work is
  backend-side.
- **Render state is a bitfield** (`GLS_*` in Material.h) applied via `GL_State()` —
  maps directly to a hashed `VkPipeline` key (blend, depth, stencil, mask bits).
- **Geometry flows through `idVertexCache`** (per-frame dynamic allocations + static
  VBOs) — maps to a VMA-backed per-frame ring buffer plus device-local static buffers.
- **Lighting path** ([draw_arb2.cpp](../neo/renderer/draw_arb2.cpp)): depth prepass,
  per-light stencil shadow volumes, additive interaction passes using ARB *assembly*
  programs — 16 programs in `pak000.pk4:glprogs/` plus dhewm3 extras (soft particles).
  These need translation to GLSL 450 → SPIR-V.
- **Two-sided stencil** is already used when available (`qglStencilOpSeparate`) —
  Vulkan supports native separate front/back stencil state, allowing direct translation 
  of Doom 3's two-sided stencil path.
- **Post/refraction effects** read `_currentRender` / `_currentDepth` filled via
  `CopyTexImage` — becomes `vkCmdCopyImage` (or render-to-texture) at the same points.
- **Immediate mode** (`qglBegin`/`qglVertex*`, ~500 call sites) is concentrated in
  [tr_rendertools.cpp](../neo/renderer/tr_rendertools.cpp) (debug visualization) and a
  few small 2D paths — not in the core game render loop.
- **Window/context**: SDL in [glimp.cpp](../neo/sys/glimp.cpp) →
  `SDL_Vulkan_CreateSurface` replaces the GL attribute dance.
- **ImGui overlay**: the bundled Dear ImGui already ships a Vulkan backend.
- **Cinematics (RoQ)**: plain `glTexSubImage2D` uploads → staged `VkImage` updates.

## Hardware baseline & backend switch

**Two user-facing backends**, selected by an archived `r_graphicsAPI` cvar
(`opengl` / `vulkan`) exposed in dhewm3's F10 settings menu. Switching recreates the
SDL window (GL vs Vulkan windows need different creation flags), which fits the
existing `vid_restart` flow — no full game restart.

**Vulkan target: 1.1 core, zero required extensions beyond `VK_KHR_swapchain`** —
runs on effectively all Vulkan-capable hardware (NVIDIA Kepler, AMD GCN 1.0, Intel
Gen8). 1.1 over 1.0 costs a negligible hardware sliver — on Linux/Mesa (RADV/ANV)
1.1+ reaches down to GCN 1.0 and Gen8 — in exchange for cleaner GL translation:
`VK_KHR_maintenance1` is core (negative viewport height → direct match for GL's
bottom-left origin) and `get_physical_device_properties2` is core (removes
prerequisite-extension boilerplate many features gate behind). It does **not** grant
descriptor indexing (1.2) or dynamic rendering (1.3) — those stay in the `vulkan-rt`
profile, so the two-profile split is preserved. Concrete consequences:
- Classic `VkRenderPass`/framebuffers — no dynamic rendering.
- Fixed per-frequency descriptor sets — no descriptor indexing / bindless.
- Push constants sized to the 128-byte guaranteed minimum.
- Y-flip via `VK_KHR_maintenance1` negative viewport (core in 1.1), matching GL.
- Binary semaphores + fences — no timeline semaphores.
- Depth-stencil format probed at runtime: `D24_UNORM_S8_UINT` availability is not guaranteed 
  across Vulkan implementations; probe formats and prefer `D32_SFLOAT_S8_UINT` when unavailable
  (stencil shadow volumes need the S8 either way).
- Pipeline cache persisted to disk to hide first-run hitches on old GPUs.

**Optional device features — query and gate, never assume (not guaranteed on 1.1
hardware).** These bite because several touch code that already exists:
- `fillModeNonSolid` — wireframe (`VK_POLYGON_MODE_LINE`), used by debug tools
  (`r_showTris`, `r_showTangentSpace`, `GLS_POLYMODE_LINE`). If absent: disable those
  wireframe debug views (or emulate).
- `wideLines` / `largePoints` — `lineWidth`/`pointSize` > 1.0, used by debug line/point
  drawing (`r_debugLineWidth` up to 10). If absent: clamp to 1.0 or quad-emulate.
- `depthClamp` — **shadow-volume z-fail caps depend on this** (see Known Risks). Query
  it; near-universal on desktop but not guaranteed — no cap trick without it.
- `samplerAnisotropy` — anisotropic filtering; gate, fall back to trilinear.
- `textureCompressionBC` — DXT/BC incl. RXGB (=DXT5) normal maps, core to the look;
  near-universal on desktop, query anyway.
- `geometryShader` / `tessellationShader` — **not used by the faithful renderer; must
  not be assumed present.** For Phase 8 shadow-map cubemap/cascade layers use
  `multiview` (core in 1.1) instead of a geometry shader.

**Minimum guaranteed limits the design must live within:**
- `minUniformBufferOffsetAlignment` up to 256 B → the per-frame UBO ring must align
  each draw's slice to the device value (query at init); same for storage buffers.
- `maxPushConstantsSize` ≥ 128 B → RenderParams stays a UBO; push only tiny data.
- `maxUniformBufferRange` ≥ 16 KiB → per-draw UBO blocks stay well under this.
- `maxBoundDescriptorSets` ≥ 4 → keep the set layout ≤ 4 (we use 2: UBO + samplers).
- `maxPerStageDescriptorSampledImages` ≥ 16 → interaction uses 7 units; safe.

**1.1-core features actively relied on:** `maintenance1` (Y-flip), `multiview`
(shadow-map layers), `get_physical_device_properties2`.

**Modernized GL target: OpenGL 3.3 core.** Every Vulkan-capable GPU exceeds this
comfortably, and it avoids cutting off older GL-only cards dhewm3 currently supports.

### Third renderer: modern Vulkan + ray tracing

`r_graphicsAPI` gains a third value, `vulkan-rt`: modern Vulkan (1.3+) with the RT
pipeline. This is **not a third backend** — it is the same Vulkan backend running a
different *device profile*, plus an RT render path on top:

- **Baseline profile** (`vulkan`): VK 1.1 core as described above.
- **Modern profile** (`vulkan-rt`): VK 1.3+ — dynamic rendering, synchronization2,
  timeline semaphores, descriptor indexing, buffer device address — plus
  `VK_KHR_acceleration_structure` / `VK_KHR_ray_query` (RTX 20xx+, RDNA2+, Arc).

Guardrails the baseline work must respect so this stays cheap to add:
- The RHI's render-pass abstraction is begin/end-scoped, not VkRenderPass-shaped, so
  the modern profile can implement it with dynamic rendering.
- Buffer/geometry abstractions keep vertex/index data identifiable and device-local
  so BLAS/TLAS builds can consume them later; no GL-style interleaving assumptions.
- Descriptor layout code lives in one place, so the modern profile can swap fixed
  sets for descriptor indexing without touching render paths.
- Sync is expressed as high-level dependencies in the backend, not raw barriers
  sprinkled through render code.

The RT path itself starts with **ray query in fragment shaders** (RT shadows
replacing stencil volumes, RT reflections) before any full RT-pipeline/SBT work —
far less machinery, same hardware, and it reuses the raster frame structure.

## Known risks
- **Clip-space conventions**: GL −1..1 vs Vulkan 0..1 depth and flipped Y — handled in
  projection matrix + viewport, but every place the engine does depth trickery
  (polygon offset, shadow volumes, depth bounds) must be re-checked.
- **Shadow volume caps** rely on depth-clamp behavior. Vulkan `depthClampEnable`
  provides it *only if* the optional `depthClamp` device feature is present (query it,
  see Hardware baseline) — near-universal on desktop but not guaranteed. Verify parity
  on z-fail paths, and decide a fallback if a target GPU lacks the feature.
- Windows-only MFC editors are stubs on Linux — out of scope.

## Prior art
- **fhDOOM** (eXistence/fhDOOM): GL 3.3 core modernization of Doom 3 — ARB/fixed
  function fully replaced with GLSL, per-light mixed shadow-map/stencil soft shadows
  (Poisson), cascade maps, POM, dmap-baked `.ocl` occluders. Closest precedent to our
  GL-side work (Phases 2–4, 9); differs in that it did not keep an ARB path or aim at
  Vulkan/RT. Mod friction: pure content mods work sans custom ARB2, game DLLs need
  recompilation.
- **RBDOOM-3-BFG**: the shared-GLSL-source + SPIR-V approach and shadow-mapping
  reference; based on BFG data rather than classic (see Non-goals).
