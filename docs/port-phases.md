# Port phases (detail)

Detailed phase plan. Status and the short version live in the hub,
[vulkan-port.md](vulkan-port.md). Feature toggles and the post-process stack
(the "improvements" cluster + Phase 11) live in
[port-improvements.md](port-improvements.md).

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

**Chunk progress (git):** A (RHI iface + GL3 bring-up), B (program cache, UBO ring,
VAOs), C (2D/GUI/console), D (Material IR + ARB→GLSL transpile), E (3D world: depth
prepass, stencil shadows, interactions), F (fog, blend lights, `_currentRender`
post-process), G (debug tools: `idImmediateMode` core path + `RB_RenderDebugTools`,
e.g. `r_showTris` / `r_showNormals`). Texgen stages (portal sky, cube reflections,
skybox) followed F. See
[readme-changes.md](readme-changes.md) for where the GL3 path deliberately diverges
from a faithful port, and [known-bugs.md](known-bugs.md) for open issues.

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
