# Known bugs (GL3 backend)

Deferred until the rendering pipeline is complete — tracked here for later triage.
Hub: [vulkan-port.md](vulkan-port.md). Deliberate deviations (not bugs) are in
[readme-changes.md](readme-changes.md).

- **[OPEN] Intermittent crash on repeated `vid_restart` (SDL3/X11 window teardown).**
  Independent of the backend-switch fix below — confirmed 2026-07-31 to reproduce on the
  **legacy** backend (where `RB_RHI_Shutdown` is a hard no-op), on the 2nd–3rd `vid_restart`
  with a world loaded. `GLimp_Shutdown` → `SDL_DestroyWindow` intermittently throws
  `X Error BadWindow (X_TranslateCoords)` — SDL3's X11 backend restoring the cursor against a
  window that is mid-teardown — which cascades into the game DLL's cleanup
  (`idClipModel::FreeTraceModel: tried to free uncached trace model`, then an
  `idStrPool::FreeString` assert → SIGABRT). The intermittency is the signature of an async X11
  teardown race. Pre-existing, in the windowing / game-teardown layer, **not** the renderer GPU
  teardown. Matters for the Vulkan port (heavy backend re-init). Mitigation under test: release
  relative-mouse/grab + flush pending events immediately before `SDL_DestroyWindow` in
  `GLimp_Shutdown`. Needs runtime iteration to confirm (can't be self-verified); intermittent, so
  validate by hammering `vid_restart` 10–20×.

- **[RESOLVED 2026-07-31] Garbled image / white screen when switching renderer backend (legacy ↔ opengl3).**
  Switching backends in Video Options — or any `vid_restart` while on the GL3 core backend —
  sometimes came up white or garbled. Root cause: `GL3Backend::Shutdown()` existed but was **never
  called**. On a `vid_restart` the GL context is destroyed and recreated, but the backend's
  render-target table kept dead FBO/texture names from the old context while the cached client
  handles (HDR scene buffer, SSAO, SSR, shadow maps) still pointed at them. The HDR guard detects a
  lost context via `GetRenderTargetImage(handle) == 0`, but the never-wiped table returned the
  stale *non-zero* name, so it reused a dead framebuffer and the whole scene rendered into it.
  Intermittent because a *differing* resolution across the restart tripped the `w/h` check and
  forced recreation, hiding it. **Fix:** a strong teardown `RB_RHI_Shutdown()` (RhiBackend.cpp),
  called from `R_VidRestart_f` and `ShutdownOpenGL` while the context is still current — frees the
  shadow caches, forgets every cached render-target handle (`RB_RHI_ResetWorldTargets`, RhiWorld.cpp),
  resets the HDR statics, then calls `GL3Backend::Shutdown()` to delete all GPU objects and wipe the
  table (making the existing `GetRenderTargetImage == 0` lost-context checks work as designed).
  Self-guards to a no-op on the legacy backend. Verified: the toggle no longer garbles.

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

- **[RESOLVED 2026-07-31] Shadow bias needs to be different for flashlight.**
  Reported: the world/model shadow bias (tuned for general casters like the
  shadow-casting fans) is far too large for the player flashlight, whose narrow
  cone grazes surfaces nearly edge-on and sweeps every frame — it peter-panned the
  shadow off casters and made edges swim; the flashlight wanted a bias in the
  0.0001–0.005 range instead. **Fix:** the shadow-map interaction path
  (`neo/renderer/rhi/RhiWorld.cpp`) now detects the flashlight per-light by its
  light shader name (base Doom 3 `lights/flashlight5`; a case-insensitive substring
  match on `"flashlight"` also covers D3XP/mod variants) and, for its interactions,
  substitutes a dedicated bias `r_shadowMapFlashlightBias` (new archived cvar,
  default **0.001**, range 0–0.5) in place of the receiver-based
  `r_shadowMapBias`/`r_shadowMapModelBias`. Non-flashlight lights are unchanged.
  Tunable in-game since the good value is scene-dependent within the reported
  window. See [[shadow-cache-hysteresis-stagger]], docs/shadow-system.md.

- **[RESOLVED 2026-07-31] Elevator (and any ridden mover) jittered above 60 fps (com_interpolate).**
  Reported 2026-07-31: riding an elevator, the elevator geometry visibly jittered while
  everything else stayed smooth. **Root cause:** stage 3 of the render interpolation feature
  (world entities) had never been built. With `com_interpolate` on, the first-person view is
  blended between the previous and current 60 Hz tic (`idPlayer::InterpolateRenderView`,
  gliding up to one tic in the past), but world entities still rendered at the *current* tic's
  snapshot. Riding a mover, the eye glides while the platform steps once per tic, so the
  eye↔platform offset sawtooths by one tic-step every 16.7 ms — the platform appears to
  vibrate. Only visible on entities you move *with*; static world vs an interpolated camera is
  smooth, hence "only the elevator jitters". **Fix (stage 3, world-entity interpolation):**
  `idEntity::SnapshotRenderTransform` records the render transform committed at the
  previous/current tic (rotated once per tic inside `Present`, 64-unit discontinuity snap for
  teleports) and registers the entity with `idGameLocal::RegisterRenderInterpolation`; once per
  rendered frame `idGameLocal::Draw` calls `InterpolateRenderEntities(frac)`, which re-presents
  each listed entity at a lerp/slerp of the two tic transforms (same never-extrapolate `frac`
  as the view, via a temp copy so the authoritative `renderEntity` keeps exact tic state);
  at the start of the next tic `RunFrame` re-commits the authoritative transforms so entities
  that stop moving don't rest mid-blend. `idLight` also blends its `renderLight` def (lights
  bound to movers would otherwise cast stepping light on gliding geometry); `idWeapon`
  overrides the hook to a no-op (stage 2 owns the view model in eye-relative space);
  `idSecurityCamera`'s standalone `Present` got the same snapshot hook. Excluded (own render
  defs, not worth it): `idBrittleFracture`, `idMultiModelAF`, FX/beam/shell auxiliary defs.
  Applied to both `neo/game/` and `neo/d3xp/`. See [[render-interpolation-feature]].

- **[RESOLVED 2026-07-29] Weapon-display glow flickered while moving (com_interpolate).**
  Reported 2026-07-29. Holding a weapon with a lit display panel (machinegun ammo screen —
  any weapon with a display), the display's glow flickered while moving, worst while
  sidestepping. Reproduced even at Potato settings, so not an enhancement regression.
  **Root cause (confirmed by bisection with the user):** the "glow" is a real dynamic
  light — `lights/viewWeaponGuiLight`, a tiny **radius-3** point light on the gun's
  `guilight` joint, updated only *once per tic* in `idWeapon::PresentWeapon`. The render
  interpolation feature (`com_interpolate`) moves the *display* every rendered frame but
  left this light at its tic position, so during motion the light lagged the display by up
  to a full tic; because the light is so small, the display slipped in and out of it and
  the glow flickered. Sidestepping was worst (max lag). The interp commit only ever touched
  game-side code and this fork otherwise caps rendering to 60 fps, so `com_interpolate 0`
  hid it (the display and its light are both at the same tic pose). Diagnosis path (kept as
  a warning against theory-first fixes): first-blamed the weapon-transform-vs-eye angle
  tripping `R_PreciseCullSurface`'s back-face test — wrong (an eye-relative transform fix
  changed nothing); then a GUI-cull bypass (`r_skipGuiCull`) — no effect (content was
  redrawn but still blinked); then a coplanar z-fight vs the backing surface
  (`r_guiDepthBias` polygon offset, ±16) — no effect. The confirming test was
  `g_weaponGuiLight 0` (disable just the light): the flicker vanished.
  **Fix:** `idWeapon::PresentWeapon` now records the gui light's previous-tic origin/axis
  (with a 64-unit discontinuity snap), and `idWeapon::InterpolateViewWeapon` lerps/slerps
  the light to the same sub-tic `frac` as the weapon and re-submits it via `UpdateLightDef`
  — so the glow tracks the interpolated display. Also kept a secondary correctness
  improvement made along the way: the view weapon now interpolates in *eye-relative* space
  (interpolate the eye→weapon offset, recompose onto the interpolated eye) so it stays
  locked to the rendered view. The diagnosis-only cvars (`g_weaponGuiLight`,
  `com_interpolateApply`/`View`/`Weapon`) were removed once the fix was confirmed. Applied
  to both `neo/game/` and `neo/d3xp/`. See [[render-interpolation-feature]].

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
