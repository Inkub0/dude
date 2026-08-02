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

**Remaining GL3 perf work (measurement-driven only).** Core-GL modernization is
already done — program cache, RenderParams/ArbParams UBOs, VAOs, redundant-state
dedup (`ApplyState` diff mask + `boundProgram`/`boundVBO`/`boundLayout` caches),
static vertexCache VBOs, and the persistent-mapped UBO ring (the one measured win so
far, a ~10× NVIDIA per-draw stall — see the divergence catalog). The rest of the old
`opengl3_renderer_improvements.md` checklist is either already covered above or
actively wrong for a faithful port (texture atlasing breaks the material system and
mod compat; instancing/16-bit indices don't fit idTech4's drawSurf replay; async
texture streaming isn't a frame bottleneck — Doom 3 loads up front). What's left is a
short, evidence-gated list; **do not chase these without a profile showing they
matter:**

1. **Profiling first — DONE.** `r_gl3GpuTime` wraps each frame's command stream in a
   ring of GL_TIME_ELAPSED queries (ARB_timer_query, core 3.3, optional-loaded like
   buffer_storage), reads each result back a ring later so it never stalls the GPU,
   and prints averaged GPU ms once per second
   ([GL3Backend.cpp](../neo/renderer/rhi/GL3Backend.cpp), `BeginGpuTimer`/`EndGpuTimer`).
   This is the gate: measure with it before touching anything below.
2. **Dedup the per-draw UBO bind — dropped after inspection.** Every draw is preceded
   by its own `AllocUniforms`, so the ring offset advances each draw and the
   (buffer, offset, size) triple is unique — a redundancy guard would essentially
   never fire. Not worth the code under the current one-slice-per-draw design; only a
   different uniform strategy (dynamic-offset rebinds) could change that, and that's
   speculative until the profiler says UBO binds cost anything.
3. **Dedup texture binds — deferred to Phase 4.** The 8-unit loop in `Draw` is inert
   today: `DrawArgs::textures` is never populated (RHI owns no images yet —
   `CreateImage` returns 0), and the live engine binds go through `idImage::Bind`,
   which *already* dedups via `backEnd.glState.tmu[].current2DMap`. Revisit only when
   Phase 4 moves image ownership into the RHI.

### Phase 3.5 — OpenGL 3.3 enhancement suite (pre-Vulkan) **[NEW — added 2026-07-24]**
Intermediate phase: implement and validate **all GL 3.3-feasible non-vanilla
enhancements on the `opengl3` backend *before* starting Vulkan**, so the Vulkan port
(Phase 4) carries the whole suite over in one pass (SPIR-V equivalents) instead of
re-deriving each feature. Everything is opt-in via the ImGui **Enhancements** tab,
defaults to vanilla behavior, and is gated off on the legacy ARB2 path via
`R_BackendSupportsEnhancements()`.

Rationale: **fhDOOM proves every one of these runs on a GL 3.3 core context** (it
dropped ARB/fixed-function entirely), so none need GL 4.x. Iterating on GL3 is faster
than on Vulkan, and a stable GL3 reference gives the Vulkan versions something to be
validated against (RenderDoc side-by-side, Phase 5).

**Already shipped (Enhancements tab, this branch):**
- Soft particles (`r_useSoftParticles`) — GL3-gated, removed from the legacy path.
- Film grain (`r_postFilmGrain`) + chromatic aberration (`r_postChromaticAberration`).
- Gamma/brightness in shader (`r_gammaInShader`).
- **Specular tuning** (`r_shading` 0 vanilla-LUT / 1 Blinn-Phong,
  `r_specularScale`, `r_specularExp`) — new `u_specularParms` UBO member@736;
  `interaction.vert`/`.frag` updated. Defaults reproduce vanilla specular
  exactly. Legacy ARB2 untouched. (A classic-Phong mode 2 shipped initially but
  was removed 2026-08-01 as redundant with Blinn-Phong; the tangent-space
  view-vector varying it added survives for the PBR path.)

**To implement on GL 3.3, ranked by value/effort:**
1. **Specular tuning** — **DONE** (see above).
2. **Shadow mapping** (large, high payoff) — full technique design in **Phase 8**
   below; already specified as an RHI feature ("GL 3.3 and Vulkan share it").
   Default stays stencil (faithful); soft shadow maps are the opt-in. **Includes
   alpha-tested casters** (specifically wanted — perforated shadows from
   grates/fences/foliage), which is only possible on the shadow-map path, not with
   stencil volumes.
3. **Parallax occlusion mapping** (marginal) — full design in **Phase 9** below.
   Height from specular-alpha; near-useless on stock art, needs HD packs. Default off.

**Stays Vulkan-only:** curved-geometry tessellation (Phase 9.5) needs GPU tessellation
shaders (GL 4.0+), which the 3.3 core context doesn't have.

**Handoff to Phase 4:** each feature lands once here against the RHI, so the Vulkan
backend inherits the cvars + Enhancements-tab UI unchanged and only needs SPIR-V shader
equivalents and pipeline wiring — the suite moves to Vulkan together.

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

**Enhancement suite port:** the Phase 3.5 features (shadow mapping, specular tuning,
POM, plus the already-shipped soft particles / film grain / chromatic aberration /
gamma-in-shader) come across to Vulkan **together** here — same cvars and
Enhancements-tab UI, only new SPIR-V shader equivalents + pipeline wiring. Because
they were built once on the RHI in Phase 3.5, this is a port, not a re-derivation.

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
**[Scheduled in Phase 3.5 — implemented on the GL 3.3 `opengl3` backend first, then
ported to Vulkan in Phase 4. This section is the detailed technique *design*; for what
actually shipped on GL3 (projected 2D + point cube maps, adaptive resolution, static
cache, perforated casters, oversize→stencil fallback, full cvar list) see the as-built
reference [shadow-system.md](shadow-system.md).]**

RBDOOM-style feature set, built **on the RHI** so GL 3.3 and Vulkan share it —
deliberately not attempted on the legacy ARB backend (throwaway work) and not part
of Phase 1 (which guarantees zero behavior change):
- soft shadows via shadow maps, `r_shadowMapping` cvar + settings toggle;
  stencil volumes stay the default faithful look
- **alpha-tested casters** *(specifically wanted — grates/fences/foliage cast
  correctly perforated shadows)*: sample the material's alpha/coverage in the
  depth-only shadow pass and `discard` below the alpha test threshold, mirroring the
  main-pass alpha test. **Impossible with stencil volumes** (they extrude the opaque
  silhouette), so this capability rides entirely on the shadow-map path — it is not a
  standalone toggle. Inherent to shadow-mapped lights; no separate cvar needed
  (optionally `r_smAlphaTestedShadows` to force-disable for perf). Cost: bind the
  diffuse/coverage texture in the shadow pass for alpha-tested materials only.
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

**First milestone (in progress) — one projected/spot light, hard edges.** Control
surface starts as a **global mode only** (`r_shadowMapping` cvar + Enhancements-tab
row: Stencil default / Shadow Maps); per-light material-keyword override deferred.
In Shadow-Maps mode, projected lights take the SM path and point/parallel lights
**fall back to stencil** until implemented — that automatic fallback *is* the
free-mixing behavior. *(As-built update: point lights are now cube-mapped; parallel
lights and deliberately-oversize point lights are what fall back to stencil — see
[shadow-system.md](shadow-system.md).)* Build order:
1. **Minimal depth render-target in the RHI** (currently absent — `CreateImage`
   returns 0, only `BeginPass` on the default framebuffer + `CopyFramebufferToImage`
   exist). Depth-only offscreen target: create depth texture + FBO, `BeginPass` able
   to target it, bind it as a sampler for a later pass. Foundational — Vulkan and any
   future FBO post-process reuse it.
2. Light view-projection matrix for projected lights (derived from existing
   `light_target/right/up/start` data).
3. Depth-only caster shader (`shadow_sm.vert/.frag`).
4. Interaction-shader shadow term: sample the map, depth-compare, multiply the light
   contribution. Start with hardware `sampler2DShadow` 2×2 PCF; Poisson soft tier and
   alpha-tested casters follow.
5. `r_shadowMapping` cvar + Enhancements-tab row; per-light branch in
   `RB_RHI_DrawWorld`.

**Two texture-architecture guardrails (decided 2026-07-24)** so shadow work advances
the texture/material system rather than boxing it in for Phase 4:
- **The depth target is the first RHI-owned image.** Route it through the same
  `ImageHandle` + sampler abstraction future material textures (HD packs, RHI-owned
  images) will use — this *starts* the Phase 4 image-ownership story cleanly instead
  of being a one-off. Shadowing and normal mapping stay orthogonal layers in the
  interaction shader (`visibility × normal-mapped shading`), so shadow maps neither
  disturb nor depend on the bump/normal path.
- **The caster pass carries a material-texture bind hook from day one** (milestone 1
  is opaque-only, but the hook is present): alpha-tested/perforated shadows need the
  diffuse/coverage texture bound in the depth pass, so leaving the seam in makes that
  an extension, not a redesign.

**Occluder generation decision (chosen): load-time, not offline.** Deliberately
avoids the dmap route for now so the occlusion algorithm can be improved and tested
incrementally against the Vulkan renderer's look, without a re-bake step. The cache
is a pure optimization — following fhDOOM's key lesson, the renderer must render
correctly with occluders absent ("maps without ocl file work just fine"). An offline
dmap bake can be added later once the algorithm stabilizes. fhDOOM otherwise built
this whole feature set on GL 3.3 (mixed per-light shadow-map/stencil, Poisson soft
shadows, cascades) and also exposes `r_shading` — a candidate
improvements toggle.

### Phase 9 — Parallax occlusion mapping (experimental)
**[Scheduled in Phase 3.5 — implemented on the GL 3.3 `opengl3` backend first, then
ported to Vulkan in Phase 4. This section is the detailed technique design.]**

Experimental fidelity feature, not a blocker for the renderer. Doom 3 ships no height
maps; derive height fields by
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

### Phase 9.5 — Curved-geometry tessellation (Vulkan / GPU) **[DEFERRED here 2026-07-24]**
Smooth low-poly silhouettes (character heads, rounded props) via **PN-triangle /
Phong tessellation** — curve each triangle using its per-vertex normals. Exposed as
a **tessellation-factor slider** in the "Enhancements" tab (opengl3/Vulkan gate,
default off / factor 1 = no change).

**Why not on the opengl3 (GL 3.3) backend:** hardware tess (TCS/TES + `GL_PATCHES`)
needs **GL 4.0**; the core context is 3.3 ([glimp.cpp:329](../neo/sys/glimp.cpp)).
CPU subdivision *is* possible on 3.3 but was rejected: it doesn't help the marquee
cases (the main-menu "planet" is a 2D GUI element, not model geometry; character
heads are **skinned MD5** meshes that would need per-frame CPU re-subdivision or
joint-weight interpolation onto a densified mesh — heavy + invasive), and the CPU
code is **throwaway** vs the GPU path. Only the cvar + slider UI would carry over.

**Vulkan plan:** SPIR-V tessellation-control/evaluation stages; skin in the vertex
shader, then tessellate — so skinned characters get subdivided **on-GPU** for free.
Scope: **character (MD5) + static model** surfaces only. Explicitly skip the BSP
world (flat, wastes perf, cracks lightmaps/shadows), GUI, particles, decals.
Watch-outs: UV/normal seams crack under tessellation → per-material opt-out; hard-
surface/mechanical models can look wrong when rounded. **Fidelity: opt-in, default
off.** Reuse the Enhancements-tab slider cvar (e.g. `r_tessFactor`) across backends.

### Phase 10 — Ray tracing path
BLAS for static world + dynamic models, TLAS rebuilt/refit per frame, ray-query RT
shadows replacing stencil volumes as the first visible payoff; RT reflections after.
Full RT-pipeline/SBT work only if a use case demands it. More realistically,
development timeline could be: 1. ray query visibility test, 2. RT shadows, 
3. reflection probes, 4. glossy reflections, 5. full path tracing experiment

---

## "Enhancements" tab — non-faithful graphical toggles (GL3/Vulkan only)

Goal: an ImGui **Enhancements** settings tab, shown **only on GL3/Vulkan** backends,
housing non-vanilla graphical toggles. The **legacy ARB2 path stays vanilla-faithful**.

Key finding: this fork (`DUDE 0.1`) is based on an **older dhewm3 that predates the
GLSL backend**, so upstream's `r_useShadowMapping` shadow mapping **does not exist
here** — bringing it in is a fresh port into the RHI (see Follow-up below), not a
relocation. The one non-vanilla feature that currently *does* run in the legacy path
is **soft particles** (`r_useSoftParticles`, default ON, ported from TDM #3878).

### Backend gating
- `glConfig.coreProfile` is `true` only for the GL3 core backend and is already used
  in `Dhewm3SettingsMenu.cpp`. Wrap it in a helper `R_BackendSupportsEnhancements()`
  (returns `glConfig.coreProfile` for now) so Vulkan can OR-in its own flag in one
  place later.

### Change set
1. **New Enhancements tab** — `Dhewm3SettingsMenu.cpp` tab bar (after "Video
   Options"): `if ( R_BackendSupportsEnhancements() && ImGui::BeginTabItem(...) )`.
   On legacy the tab isn't created; optionally add a greyed note in Video Options.
2. **Move existing non-vanilla toggles** Video → Enhancements: `r_useSoftParticles`
   (+ `r_enableDepthCapture` companion), `r_postFilmGrain` (add slider), 
   `r_postChromaticAberration` (add slider), `r_gammaInShader` (the in-shader
   reimplementation; `r_gamma`/`r_brightness` sliders stay in Video — gamma is
   vanilla). Add a disabled **"Shadow Mapping — coming soon"** placeholder row.
3. **Gate soft particles to non-legacy backends** — add `&& glConfig.coreProfile`
   (via the helper) at the two decision points: `tr_light.cpp:1447` and the
   `getDepthCapture` decision in `draw_common.cpp:562`. Legacy then renders vanilla
   particles (no depth-capture pass). Cvar keeps its `"1"` default (harmless when
   gated); code stays in place for the RHI path.
4. **Helper wiring** — declare `R_BackendSupportsEnhancements()` in `tr_local.h`,
   define in `RenderSystem_init.cpp`; ensure post-FX cvars are reachable (`extern`)
   from the menu.

### Explicitly NOT touched
`com_interpolate` (sim-level, backend-agnostic); `r_gamma`/`r_brightness` sliders
(vanilla); ARB2 rendering math (only the soft-particle gate); shadow mapping.

### Files
`neo/framework/Dhewm3SettingsMenu.cpp`, `neo/renderer/tr_local.h`,
`neo/renderer/RenderSystem_init.cpp`, `neo/renderer/tr_light.cpp`,
`neo/renderer/draw_common.cpp`.

### Follow-up — now part of Phase 3.5
The Enhancements tab is the delivery surface for the **Phase 3.5** OpenGL 3.3
enhancement suite. Remaining work, in order:
1. **Specular tuning** sliders (`r_specularScale`, `r_specularExp`, `r_shading`) —
   quick, low-risk shader-uniform toggles (fhDOOM reference).
2. **Shadow mapping** (Phase 8 design) on the GL3 RHI — flip the "Shadow Mapping
   (coming soon)" placeholder row into a live per-light toggle; stencil stays default.
3. **Parallax occlusion mapping** (Phase 9 design) — opt-in, default off, marginal
   without HD texture packs.

Each is built once on the RHI so Phase 4 ports the whole suite to Vulkan together.
Tessellation (Phase 9.5) stays Vulkan-only (needs GL 4.0+ tess shaders).
