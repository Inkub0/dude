# dhewm-rt: Vulkan Raster Port Plan

**Goal:** a faithful modern Vulkan port of the dhewm3 renderer — same look (depth prepass,
stencil shadow volumes, per-pixel ARB2-style lighting), classic Doom 3 game data, done
incrementally in this codebase with the game staying playable (on GL) at every step.

Surveyed 2026-07-20 at master `455b88e` (1.5.5).

## Non-goals

- replacing the Doom 3 material system
- converting to deferred rendering
- removing stencil shadows
- requiring Vulkan 1.2+ hardware for the compatibility renderer (baseline is 1.1)
- changing game assets

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

## Phases

### Phase 0 — Groundwork  **[DONE]**
- **[DONE]** `DHEWM3_VULKAN` CMake option (default OFF). When ON: `find_package(Vulkan
  REQUIRED)` (headers + loader + locates glslangValidator/glslc), requires a
  GLSL→SPIR-V compiler, bumps `CMAKE_CXX_STANDARD` to 17, defines `-DDHEWM3_VULKAN=1`,
  links `Vulkan::Vulkan`. Verified: default GL build unchanged/clean; `-DDHEWM3_VULKAN=ON`
  configures (found Vulkan 1.4.350 + glslc/glslangValidator) and builds.
- **[DONE]** `r_graphicsAPI` archived cvar (`opengl` default / `vulkan` / `vulkan-rt`),
  externed in tr_local.h. Startup sanity check in `R_InitOpenGL`: any non-`opengl`
  value warns and falls back to GL (distinct message for non-Vulkan builds).
- **[deferred]** VMA (VulkanMemoryAllocator) is header-only and unneeded until the
  Phase 4 backend — vendor under `libs/vma/` then.
- **[deferred → task]** F10 settings-menu selector: belongs with the window-recreate
  switch work (the `r_graphicsAPI` menu task), once a second backend actually exists.
GL build stays the default; nothing changes for a stock build.

### Phase 1 — Narrow the funnel (GL-only refactor, zero behavior change)
- **[DONE]** Replace immediate-mode drawing (debug tools, small 2D paths) with a
  batched `idImmediateMode` helper (renderer/ImmediateMode.{h,cpp}) that collects
  verts and draws them in one call; `GL_QUADS`/`GL_POLYGON` become indexed/fanned
  triangles (core-profile-ready). Verified: **0 `qglBegin` in Linux-built code**,
  clean build. Covered renderer + dmap + renderbump tools.
- **[deferred to Phase 3]** The remaining fixed-function usage — `glTexGen`, matrix
  stack, `glTexEnv`, fixed-function `glColor` current-color — lives entirely in four
  passes (`RB_STD_T_RenderShaderPasses`, `RB_T_BlendLight`, `RB_T_BasicFog`,
  `RB_PrepareStageTexturing`). These *are* the programmable path once the GLSL
  shaders (Phase 2, already written: `generic`, `blendlight`, `fog`, `environment`)
  run on a real backend. They cannot be "folded into the programmable path" before
  that backend exists — doing it on the legacy ARB/fixed-function GL now is throwaway.
  So the "confine all GL to helpers" end-state is delivered by the Phase 3 RHI (whose
  entire job is exactly that), not by a separate wrapper pass here.

### Phase 2 — Shader modernization (shared GLSL source)
Translate the ARB assembly programs to a **single GLSL source tree** compiled two
ways: GLSL 330 core for the modernized GL backend, GLSL 450 → SPIR-V (build-time,
glslang) for Vulkan. Differences (binding syntax, uniform blocks vs UBO layout,
Y/depth conventions) handled by a small preamble/macro header, the way RBDOOM-3-BFG
does it. This retires the ARB assembly path entirely once the GL 3.3 backend lands.
As the ARB programs are not just shaders, may need to add a translation layer, to also
keep mods compatibility (see "Mod compatibility scope" below).

**Verification levels for the translations** (in order of increasing strength):
1. glslang compile validation, both targets (done, automated via neo/shaders/validate.py);
2. instruction-level construction review against engine bindings (done);
3. transpiler cross-check — the ARB→GLSL transpiler is an independent second
   derivation; machine-diff it against the hand translations;
4. differential pixel harness — render identical inputs through the legacy ARB
   path (GL compatibility context; the old renderer is the reference
   implementation) and the translated GLSL 330 (core context), diff pixels;
5. Phase 3/5 in-engine parity: same scene under r_graphicsAPI toggle + RenderDoc,
   "perceptually equivalent captures" bar.

### Phase 2.5 - Material IR
Convert `idMaterial` stages into a renderer-facing Material IR containing:
- textures and resource bindings
- blend mode
- depth/stencil state
- shader permutation
- material parameters and uniforms
`idMaterial` remains the authoritative game-facing format.
Material IR is a generated intermediate representation consumed by the renderer backends.
The IR preserves original Doom 3 material semantics; it is not a replacement material language.

## Mod compatibility scope

Compatibility is defined by *what a mod ships*, not by intent:

- **Data / script / def / map / GUI mods** — already work by setting `fs_game`; the
  renderer port must not regress this. Nothing extra needed.
- **Mods with custom ARB shaders** — supported via the Phase 2 work. **Goal: a general
  ARB-assembly → GLSL transpiler that translates shaders from as many publicly
  available Doom 3 / RoE mods as possible**, so custom-shader mods run on the GL 3.3
  and Vulkan backends without per-mod hand-porting. Approach:
  - build a **regression corpus** of `.vfp`/`.vp` programs harvested from public mods
    (Phobos ×9, Sikkmod, Wulfen/Monoxead, Hexen: Edge of Chaos, The Dark Mod lineage,
    Rivensin/Ruiner, …) that the transpiler must compile;
  - ship exact hand-translations for the stock + most common shaders, transpile the
    rest, and if a program can't be transpiled, **degrade** to a plain textured/blended
    stage rather than crash;
  - keep the legacy ARB loader working on the classic GL path as fallback and as the
    reference for validating transpiler output.
- **Source-available compiled mods** — port the game source against our clean game SDK
  (API v9) → a native `<mod>.so`. Keep the SDK buildable so this stays a bounded task.
- **Binary-only compiled mods (`gamex86.dll`)** — **out of scope.** Loading a foreign
  engine's compiled game logic means reconstructing the frozen 2004 game↔engine ABI
  *plus* a Windows-PE / 32-bit / thiscall thunking layer — categorically a Wine problem
  (larger than this whole port, and Wine already does it). Even a Windows build of our
  engine can't load such a DLL (changed `GAME_API_VERSION`/ABI). Practical answer for
  these mods (e.g. Doom 3: Phobos): run **vanilla Doom 3 + the mod under Wine/Proton**.
  This is a game-code limitation, unrelated to the renderer.

### Phase 3 — RHI abstraction

**Decisions (2026-07-21):**
- **Parallel backend, not in-place rewrite**: legacy ARB path stays untouched as the
  reference implementation (pixel harness needs it); new GL 3.3 backend is new code
  behind `r_graphicsAPI opengl3` (legacy keeps `opengl` and stays default until
  parity; then opengl3 becomes default, legacy renamed `opengl-legacy`).
- **Material IR (Phase 2.5) co-developed as its own module**, lazily built + cached
  per material; unknown custom ARB programs → transpiler → on failure degrade to a
  plain generic stage.
- **Shaders load as loose files via idFileSystem** (`shaders/` dir): moddable,
  hot-reloadable (`reloadShaders`), install copies neo/shaders. Prelude prepending +
  include resolution done by the loader.
- **Uniforms**: one RenderParams/ArbParams UBO slice per draw from a ring buffer;
  C++ struct shared with GLSL layout. VAOs on attr locations 0–5 (+ shadow layout).
- `_currentRender`/`_currentDepth` stay CopyTexSubImage-style (faithful, no FBOs yet).
- RHI + GL 3.3 backend stay C++11; only the Vulkan backend requires C++17.
- **Milestone 1 = 2D/GUI/console** through the RHI, verified by eyeball + harness
  against legacy; then depth prepass → shadows+interactions → stages/fog → post →
  debug tools (rehearses the Phase 4 bring-up order).

Minimal interface shaped by what idTech4 actually needs: Buffer, Image, Sampler,
Pipeline (state bits + shader + vertex layout), render pass begin/end, draw with
dynamic geometry, screen copy. Implement it first as the **modernized GL 3.3 core
backend** (GLSL shaders from Phase 2, VAOs, UBOs, no fixed function) and flip the
renderer to use it exclusively — game still runs on GL, now through the abstraction.
This backend is a permanent, user-selectable peer of the Vulkan one, not scaffolding.

### Phase 4 — Vulkan backend
Instance/device/swapchain via SDL_Vulkan, VMA memory, N frames in flight, descriptor
management, pipeline cache keyed by (stateBits, shader, vertex layout, pass).
Bring-up milestones, each independently verifiable:
1. clear screen → 2. 2D GUI/console/menu → 3. depth prepass → 4. stencil shadows +
interactions (the big one) → 5. translucents + `_currentRender`/`_currentDepth`
effects → 6. debug tools → 7. cinematics, screenshots, ImGui.
final milestone:
1. Mars city loads, 2. identical light/shadow behaviour, 3. no missing interactions
4. RenderDoc shows: depth pass, stencil pass, and interaction pass
Important: The Vulkan renderer is considered correct when it produces perceptually 
equivalent captures, not when individual API calls match.

### Phase 5 — Validation & polish
Validation-layer clean, RenderDoc side-by-side parity captures GL vs VK, performance
pass, mode switching/vsync, in-menu backend switching hardened (window recreate path),
and a check on a real or emulated Vulkan 1.1-class device profile (e.g. running with
a 1.1 instance and validation forbidding features above the baseline profile).

### Phase 6 — Modern Vulkan device profile (`vulkan-rt`)
Second device profile in the same backend: VK 1.3+ (dynamic rendering, sync2,
timeline semaphores, descriptor indexing, BDA). Renders the same faithful raster
frame first — this phase is profile plumbing, not new visuals.

### Phase 7 — Uncapped framerate (fixed-tick + interpolation)
Game logic stays at fixed 60Hz ticks (`USERCMD_HZ`); the renderer interpolates
entity transforms, view origin/axis and joints between the last two ticks, BFG-style.
Simply uncapping is known-broken: dhewm3's experimental `com_fixedTic -1` was
reverted (upstream issue #261) because physics/anims/effects assume 16ms steps.
Fixed-tick + interpolation is safe at any fps (500+) since logic never sees a
variable timestep. Frontend/game-interface work, independent of the backend port.

### Phase 8 — Shadow mapping (optional, per-light)
RBDOOM-style feature set, built **on the RHI** so GL 3.3 and Vulkan share it —
deliberately not attempted on the legacy ARB backend (throwaway work) and not part
of Phase 1 (which guarantees zero behavior change):
- soft shadows via shadow maps, `r_shadowMapping` cvar + settings toggle;
  stencil volumes stay the default faithful look
- alpha-tested casters (sample material alpha in the shadow pass — impossible
  with stencil volumes)
- per-light choice of shadow maps vs stencil, freely mixed in a frame
- Poisson-disk PCF filtering as part of the soft-shadow shader
- parallel lights: single map first; cascades only if the few outdoor scenes
  visibly need them
Fits the Vulkan 1.1 / GL 3.3 hardware baseline (depth textures + FBO / render
passes, no extensions).
Additions: **simplified occluder meshes** for the depth-only caster passes
(conservative simplification; raw-geometry path kept — aggressive settings can
alter shadow silhouettes), cached in a **`generated/` directory** keyed by model
hash, **generated at load time** (pure derived-data cache, no fidelity impact).

**Occluder generation decision (chosen): load-time, not offline.** Deliberately
avoids the dmap route for now so the occlusion algorithm can be improved and tested
incrementally against the Vulkan renderer's look, without a re-bake step. The cache
is a pure optimization — following fhDOOM's key lesson, the renderer must render
correctly with occluders absent ("maps without ocl file work just fine"). An offline
dmap bake can be added later once the algorithm stabilizes. fhDOOM otherwise built
this whole feature set on GL 3.3 (mixed per-light shadow-map/stencil, Poisson soft
shadows, cascades) and also exposes `r_shading` (Blinn-Phong vs Phong) — a candidate
improvements toggle.

### Phase 9 — Parallax occlusion mapping (experimental)
Moved after the core Vulkan port (was Phase 4) — experimental fidelity feature, not
a blocker for the renderer. Doom 3 ships no height maps; derive height fields by
integrating the normal maps (Frankot–Chellappa / Poisson) at load time into the
`generated/` cache. Needs the GLSL backends (Phase 3+); best evaluated on the Vulkan
renderer. Normals baked from high-poly models aren't always integrable →
per-material opt-out required. **Fidelity: biggest look change of the improvement
set; default off.**

Reference — **fhDOOM** shipped POM on GL 3.3 Doom 3: it reads height from the
**specular map's alpha channel** (art-authored) rather than deriving it, gated by
`r_pomEnabled` (default off) + `r_pomMaxHeight`. Its author found POM "looks weird
and wrong in a lot of places" *even with real height data* — strong signal that POM
on Doom 3 art is marginal regardless of height source. Our normal-integration route
is more ambitious than fhDOOM's (stock content has no specular-alpha height at all),
so treat it as research, not a guaranteed win.

### Phase 10 — Ray tracing path
BLAS for static world + dynamic models, TLAS rebuilt/refit per frame, ray-query RT
shadows replacing stencil volumes as the first visible payoff; RT reflections after.
Full RT-pipeline/SBT work only if a use case demands it. More realistically,
development timeline could be: 1. ray query visibility test, 2. RT shadows, 
3. reflection probes, 4. glossy reflections, 5. full path tracing experiment


## "Improvements over the classic engine" settings section
A dedicated section in the F10 settings menu collecting every
fidelity-affecting toggle, each defaulting to the classic look unless noted:
- soft particles (`r_useSoftParticles` — already in dhewm3, default on)
- shadow mapping / soft shadows (Phase 8)
- parallax occlusion mapping (Phase 9, default off)
- specular model (`r_shading`, fhDOOM-style): **default = the original Doom 3
  specular-falloff LUT** (baked half-angle curve, faithful — see interaction.frag).
  Opt-in live alternatives: Blinn-Phong (close approximation of the LUT) and Phong
  (bigger departure: reflection-vector, tighter highlights, wrong for Doom 3's
  half-angle model). Both alter highlight shape/size — a look change, not free.
- uncapped framerate via tick interpolation (Phase 7)
- native-resolution console font scaling (standalone QoL; the console renders in
  virtual 640×480 coords today — scaled blurry at high res. UI-only change.)
- **film grain** (`r_postFilmGrain`, 0=off) and **chromatic aberration**
  (`r_postChromaticAberration`, 0=off): one fullscreen post pass over
  `_currentRender` after the 3D view, **before 2D/GUI — HUD unaffected**.
  Shader authored (shaders/postprocess.*), wired in Phase 3 Chunk F.
  Fidelity: look change, defaults off.

Debug aids (not fidelity-relevant, console cvars): `r_whiteWorld` renders all
diffuse maps as white to judge lighting on its own (implemented on the legacy
path; carry into the Phase 3 IR).

The section also inventories the **pre-existing dhewm3-era departures from 2004**,
so it's the single honest list of everything non-classic:
- widescreen main-menu scaling (`r_scaleMenusTo43`, default on)
- the `zWideGuis` widescreen GUI data patches (installed in base/, d3xp/, d3le/ —
  data-side, shown as detected info rather than a toggle)
- window-alpha fill for Wayland (`r_fillWindowAlphaChan`, cosmetic/compat)

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

## Effort honesty
This is a multi-week project of focused sessions. Phases 1–3 land as ordinary GL
refactors with the game fully playable at every commit; **Phase 5 is where Vulkan
actually lights up**, milestone by milestone. (Phases 2.5 and 4 — Material IR and
POM — are not plain refactors; POM in particular is experimental and could be
deferred behind the core Vulkan port.)
