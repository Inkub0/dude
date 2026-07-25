# Known bugs (GL3 backend)

Deferred until the rendering pipeline is complete — tracked here for later triage.
Hub: [vulkan-port.md](vulkan-port.md). Deliberate deviations (not bugs) are in
[readme-changes.md](readme-changes.md).

- **Sometimes transparent surfaces such as windows render with an iridescent fade.**
- This seems to be happening randomly and usually closing the game and 
relaunching it fixes the problem.

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
