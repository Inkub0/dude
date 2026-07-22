# Known bugs (GL3 backend)

Deferred until the rendering pipeline is complete — tracked here for later triage.
Hub: [vulkan-port.md](vulkan-port.md). Deliberate deviations (not bugs) are in
[readme-changes.md](readme-changes.md).

1. **Main-menu planet disappears on approach** — the 3D planet in the menu
   background vanishes once it moves close to the camera; its atmosphere effect
   remains visible but the model itself drops out. Likely a depth-range or
   near-clip issue in the RHI depth prepass for GUI-embedded 3D views.
2. **Mirrors corrupt surrounding scene** — the reflected scene inside a mirror
   renders correctly, but geometry around the mirror shows large black regions.
   Suggests the mirror's subview render is clobbering scissor, depth, or
   stencil state that the outer view depends on.

## Resolved
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
