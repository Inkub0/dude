# Known bugs (GL3 backend)

Deferred until the rendering pipeline is complete — tracked here for later triage.
Hub: [vulkan-port.md](vulkan-port.md). Deliberate deviations (not bugs) are in
[readme-changes.md](readme-changes.md).

- **[RESOLVED 2026-07-26] Transparent surfaces (glass) rendered with wrong/iridescent tints.**
  Root cause: `environment.vert` multiplied the cube reflection by the raw geometry
  vertex colour instead of the stage colour (`u_color` + SVC mode), so glass reflections
  ran un-dimmed and washed the pane with a colour cast — the colour matched each
  material's `red/green/blue ParmN` stage colour. Fixed by mirroring `generic.vert`
  (`environment.vert` + the SVC switch in `RB_RHI_RenderTexgenStage`). Along the way two
  other real bugs were fixed: `ArbToGlsl` never initialised `fragColor` (heatHaze left the
  glass alpha undefined), and the `ArbParams` UBO put heatHaze's `u_fenv`/`u_vlocal` in the
  NVIDIA tail-read dead zone (reordered them low). A residual "glass slightly too shiny vs
  legacy" remains because the enhancement backend lights the *refracted scene* brighter than
  the original renderer; compensated with `r_gl3ReflectionScale` (default 0.7), see
  docs/readme-changes.md §1.4. Full investigation in the [[glass-tint-alpha-fix]] memory.
  Original report kept below for history:
- ~~Sometimes transparent surfaces such as windows render with an iridescent fade;
  random, relaunch usually fixes it.~~
- **Investigation 2026-07-26 (mechanism, not yet root-caused):** Doom 3 glass
  (e.g. `textures/glass/outdoor_glass1`, `breakyglass*`) draws its environment
  reflection with `blend gl_dst_alpha, gl_one` — an *additive* `env/genN` cube
  reflection, tinted by the entity colour (`red/green/blue Parm0/1/2`), **masked
  by the framebuffer's destination alpha**. A prior `maskcolor` +
  `map makealpha(...)` stage is supposed to write that per-pixel alpha mask so the
  reflection only shows in the glass pattern. When the destination alpha ends up
  `1.0` everywhere, the whole pane is washed with the (coloured) env-cube
  reflection — exactly the "various-colour iridescent tint" symptom. The GL3
  backend renders the 3D view straight to the backbuffer (framebuffer 0); the
  frame clear sets `rgba[3] = 1.0` and per-view clears leave colour/alpha alone,
  so if the mask-writing stage doesn't land its alpha on core the reflection reads
  `dst_alpha == 1.0`. Candidates for the GL3-specific / intermittent failure, not
  yet confirmed: (a) the chosen GL visual reporting 0 usable alpha bits on some
  launches (`glConfig.alphabits`; the startup log prints the actual `a%d`), (b)
  the `maskcolor` opaque alpha-write stage being skipped/mis-masked in
  `RB_RHI_RenderShaderPasses`, (c) the `makealpha()` image feeding a wrong alpha.
  Decisive next step needs the running game: read back the backbuffer alpha, or a
  RenderDoc capture of one glass surface. Reflection path is
  `environment`/`bumpyenvironment` via `RB_RHI_RenderTexgenStage`; blend factor
  `GL_DST_ALPHA` is set by `ApplyState` from the stage's `GLS_SRCBLEND_DST_ALPHA`.
- **Mitigation shipped 2026-07-26 (`r_gl3GlassAlphaFix`, default on, pending
  validation):** the `maskcolor` alpha-write path reads as code-correct and
  deterministic (both stages draw; `GLS_COLORMASK` parsed right; `generic.frag`
  outputs alpha; `qglColorMask` managed only via `ApplyState`/`DoClear` — no
  desync found), so rather than a no-op "force the mask" change, `RB_RHI_DrawView`
  now resets the view's backbuffer alpha to 0 (`RHI::ClearViewAlpha`) before both
  the translucent and post-process shader-pass loops. Every base-game `gl_dst_alpha`
  user (glass, weapon-powerup fx, sfx) writes its own `maskcolor` mask in-surface,
  so a landing mask overwrites the reset (no change); only a *missing* mask is
  affected, and then the reflection reads `dst_alpha == 0` and simply doesn't show
  instead of washing. This neutralises the symptom regardless of the still-unknown
  intermittent trigger, and doubles as a test: **if a pane still washes with
  `r_gl3GlassAlphaFix 1`, candidate #1 is wrong** and the cause is elsewhere (the
  env cube itself, or the dst-alpha read). Validation: user is loading the game
  many times to see if the tint recurs.
- **Root cause found + fix 2026-07-26 (transpiler `fragColor` init):** live testing
  narrowed it decisively — `r_gl3GlassAlphaFix` had *no* effect, `r_skipPostProcess`
  removed both the tint and the warped-glass effect, and no enhancement toggle
  mattered. So the tint rides on the **post-process `heatHaze` glass** (the
  `_currentRender` refraction), and the user's tell was "just the *alpha* of the
  glass is getting that tint." Cause: `ArbToGlsl` declared `layout(location=0) out
  vec4 fragColor` but never initialised it, and the stock `heatHaze*` /
  `colorProcess` programs write only `result.color.xyz`, leaving `fragColor.w`
  **undefined**. heatHaze's replace blend then writes that garbage alpha into the
  framebuffer, which the later `blend gl_dst_alpha, gl_one` env-cube reflection
  reads as its mask → coloured wash. This also explains why the earlier
  view-alpha reset did nothing: heatHaze clobbered it a stage later. Fix:
  initialise `fragColor = vec4(0.0)` at the top of every transpiled fragment
  `main()` (`neo/renderer/ArbToGlsl.cpp`) — RGB is still fully written by the
  program; an unwritten alpha now reads a defined 0 (reflection contributes
  nothing) instead of garbage. Hand-written GLSL (interaction/ambient/environment)
  already write full RGBA, unaffected. Builds clean.
- **ROOT CAUSE + FIX 2026-07-26 (cube reflection ignored the stage colour):** the
  visible tint is the glass's cube-reflection stage, GL3-only, and the tint colour
  matches each material's stage colour parm (`glass1` = `0.13,0.20,0.21`,
  `outdoor_glass1` = `0.28,0.31,0.28`). Cause: `environment.vert` did
  `var_Color = attr_Color` — it multiplied the cube by the raw **geometry vertex
  colour** and dropped the **stage colour**. The ARB/fixed-function path modulates
  the reflection by the stage colour (`SVC_IGNORE` → `glColor`), which for glass is
  a dim `~0.15` factor; without it the reflection ran at full brightness and washed
  the pane with a colour tint. Fix: `environment.vert` now mirrors `generic.vert` —
  `var_Color = (attr_Color * u_vertexColorModulate + u_vertexColorAdd) * u_color` —
  and `RB_RHI_RenderTexgenStage` sets the vertex-colour mode uniforms per
  `pStage->vertexColor`. `bumpyenvironment` already matches vanilla (raw cube, no
  colour). This is the actual fix; the earlier ArbParams/fragColor changes were real
  but separate bugs. Debug scaffolding removed. **Awaiting user validation.**
- **ACTUAL root cause + fix 2026-07-26 (UBO tail-read on `ArbParams`):** the
  `fragColor` fix didn't help, and a runtime probe showed `_currentRender` IS
  copied correctly (4096×2048 = POT of the 2560×1440 view). The `arbtool`
  standalone transpiler proved the generated heatHaze GLSL is correct. That left
  the *uniforms*: heatHaze builds its `_currentRender` sample uv from
  `u_fenv[0]/[1]` (screen scale) and `u_vlocal[0]/[1]` (deform), and in the
  1536-byte `ArbParams` block those sat at byte offset **768+** — inside the
  NVIDIA "UBO tail-read" dead zone ([gl3-ubo-tail-read-bug]; RenderParams @736
  escapes it, this block didn't). Read as 0, the uv collapses to a fixed corner
  texel of `_currentRender` → a flat, camera-static, per-scene-coloured tint
  (blue-ish because the untouched default corner is RGB 16/32/48). Every stock
  transpiled program (heatHaze×3, colorProcess) uses only `env[0..1]`/`local[0..1]`,
  so the fix **reorders the `ArbParams` block** (`ArbParamsBlock.h` +
  `shaders/arbparams.glsl`) to put `u_vlocal` (@256) and `u_fenv` (@384) first —
  all heatHaze params now < 416, well inside the reliable window — and pushes the
  large unused `venv`/`flocal` arrays to the tail. Same layout, same names, no
  transpiler/fill changes. Keep the `fragColor` init too (real bug). Debug probes
  removed. **Awaiting user validation.**

- **Shadow bias needs to be different for flashlight**
as generally a shadow bias of 1 seems to work with some lights such as 
shadow casting fans across the game, the flashlight needs to be at 0.0001
to 0.005. a good value should be hardcoded for the flashlight.

## Resolved
- **World goes black around a mirror; main-menu planet disappears on approach**
  — both were the same defect: `GL3Backend::BeginPass` forced the stencil
  write-mask on before clearing stencil but never forced `depthMask`/`colorMask`
  on before the depth/color clear. `glClear(GL_DEPTH_BUFFER_BIT)` is a silent
  no-op while `glDepthMask(GL_FALSE)` is active, and a preceding subview
  (mirror reflection, the GUI-model planet view) leaves `depthMask` FALSE
  because its last draws use `GLS_DEPTHMASK` (interactions, stencil shadows,
  fog). So the following view's depth clear did nothing, its geometry failed
  the depth test, and it rendered black / dropped out — but only when a subview
  ran first (matching "only with a mirror in view"). Fixed by forcing the write
  masks on for whatever `BeginPass` clears (as legacy's `GL_State(GLS_DEFAULT)`
  does) and re-syncing pipeline state afterward. Hardens all multi-view frames
  (mirrors, portal sky, security-camera monitors, xray). Verified in base Doom 3.
- **Portal sky "culls" geometry** — non-`TG_EXPLICIT` texgen stages were all
  skipped by the Material IR, so `textures/smf/portal_sky` (`forceOpaque` +
  `sort portalSky`, a single `map _currentRender` + `screen` = `TG_SCREEN`
  stage) sealed the depth buffer but never blitted the sky, and its seal
  occluded geometry in front of it (RoE phobos1 "Phobos Labs Exterior"). Fixed
  by wiring the texgen modes through the IR (`SK_TEXGEN`): `TG_SCREEN`/`SCREEN2`
  → `portalsky`, `TG_REFLECT_CUBE` → `environment`/`bumpyenvironment`,
  `TG_SKYBOX_CUBE`/`WOBBLESKY` → `skybox`, `TG_DIFFUSE_CUBE` → `diffusecube`
  (`TG_GLASSWARP` still degrades — needs scratch-image plumbing). Because the
  original's depth-exact portal-sky sealing can't be reproduced faithfully
  without culling, the sky is drawn **at the far plane** (`z = w`), skipped in
  the depth prepass, and drawn depth-LEQUAL + no depth write — classic skybox
  behavior, a deliberate visual-fidelity-over-exactness choice (see
  [readme-changes.md](readme-changes.md)). Verified in RoE.
- **RoE grabber gun black disc** — the grabber's `_currentRender` warp rendered
  as a black circle because `RC_COPY_RENDER` was stubbed and `_currentRender`
  custom stages were skipped. Fixed in Chunk F (verified in RoE).
