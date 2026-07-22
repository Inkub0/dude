# GL3 backend — deliberate divergences from a faithful port

The DUDE GL 3.3 / RHI backend aims to reproduce the classic ARB2 renderer's
output. Most of it is a state-bit-for-state-bit port of the legacy code. This
file lists the places where it **deliberately does not** replicate the legacy
math exactly — because exact replication would be wrong/impossible on a core
profile, or as a conscious visual/practical trade-off.

Each entry: what the legacy renderer does → what we do instead → why → the
fidelity impact.

> Scope: this covers the fog/blend-light/post (Chunk F) and texgen-stage work.
> Faithful ports (2D/GUI, depth prepass, stencil shadows, interactions, the
> fog *math* itself) are intentionally **not** listed here — they match legacy.

---

## 1. Visual / behavioral divergences

### 1.1 Sky drawn at the far plane (portal sky & skybox)
- **Legacy:** the portal-sky surface (`textures/smf/portal_sky`, `forceOpaque`,
  `sort portalSky`, a lone `map _currentRender ; screen` stage) and skybox
  surfaces write depth at their true geometry position. The portal sky "seals"
  the level so you can't see the void behind it.
- **Ours:** sky is pinned to the far plane in the vertex shader
  (`gl_Position.z = gl_Position.w`), **skipped in the depth prepass**, and drawn
  **depth-LEQUAL with no depth write**. Classic skybox behavior: it sits behind
  all geometry and fills only empty (far-depth) pixels.
- **Why:** on the GL3 path the depth-exact seal made the sky occlude geometry in
  *front* of it (RoE phobos1, "Phobos Labs Exterior" — corridor and exterior
  pipes were culled by the sky). Reproducing the seal faithfully reproduces the
  bug; the far-plane model cannot cull geometry.
- **Fidelity impact:** the sky loses its level-**sealing** role. If a map has a
  gap the sky was meant to seal, the void could show through (rare). Otherwise
  the visual result matches.
- **Files:** `neo/shaders/portalsky.vert`, `neo/shaders/skybox.vert`;
  `RhiWorld.cpp` (`RB_RHI_FillDepthBuffer` sky skip); `RhiBackend.cpp`
  (`RB_RHI_RenderTexgenStage`).

### 1.2 Blend-light texture matrix not applied
- **Legacy:** `RB_BlendLight` applies a (possibly scrolling/animated) texture
  matrix to the light projection via `RB_LoadShaderTextureMatrix`.
- **Ours:** logged once and drawn **without** the matrix.
- **Why:** scoped out; stock Doom 3 / RoE content essentially never puts a
  texture matrix on a blend light. It would be folded into the projection
  texgen planes (à la `RB_BakeTextureMatrixIntoTexgen`) if a case appears.
- **Fidelity impact:** a blend light with a scrolling projection texture would
  not scroll. No known stock case.
- **Files:** `RhiWorld.cpp` (`RB_RHI_BlendLight`).

### 1.3 `TG_GLASSWARP` not implemented (degrades)
- **Legacy:** glass-warp stages sample a downscaled `_currentRender`
  (`scratchImage`/`scratchImage2`) through `FPROG_GLASSWARP` + screen texgen.
- **Ours:** the stage is skipped (`SK_SKIP`).
- **Why:** needs extra scratch-image plumbing; rare, and glass-warp surfaces are
  translucent, so dropping the stage removes the distortion without breaking the
  scene (unlike the opaque portal sky).
- **Fidelity impact:** glass-warp surfaces render without their heat-haze-style
  distortion.
- **Files:** `neo/renderer/rhi/MaterialIR.cpp`.

---

## 2. Equivalent result, different mechanism

### 2.1 Skybox / wobblesky texcoords computed in-shader
- **Legacy:** `R_SkyboxTexGen` / `R_WobbleskyTexGen` compute per-vertex 3D
  texcoords on the CPU each frame and upload them as a separate dynamic vertex
  stream (`surf->dynamicTexCoords`).
- **Ours:** the vertex shader computes `texcoord = wobble · (position −
  localViewOrigin)` from uniforms (`u_localViewOrigin`, and the wobble matrix in
  `u_modelMatrixRow0..2`); no second vertex stream / custom vertex layout.
- **Why:** avoids a parallel dynamic vertex buffer and an extra VAO layout; the
  result is numerically identical (the wobble matrix replicates
  `R_WobbleskyTexGen` exactly; identity for a plain skybox).
- **Fidelity impact:** none (same math).
- **Files:** `neo/shaders/skybox.vert`; `RhiBackend.cpp` (`RB_RHI_WobbleMatrix`,
  `RB_RHI_RenderTexgenStage`).

---

## 3. Omitted legacy pass (inert on this profile)

### 3.1 `RB_STD_LightScale` not ported
- **Legacy:** after interactions, multiplies the whole color buffer by
  `backEnd.overBright` when the brightest light color exceeds
  `backEndRendererMaxLight`.
- **Ours:** not implemented.
- **Why:** the ARB2-class renderer sets `backEndRendererMaxLight = 999`, so
  `RB_DetermineLightScale` always yields `overBright = 1.0` and the pass is a
  no-op. (The depth-fill's `1/overBright` subview down-modulate stays `1.0` too.)
- **Fidelity impact:** none in practice; would matter only if
  `backEndRendererMaxLight` were lowered.
- **Files:** `RhiBackend.cpp` (`RB_RHI_DrawView`).

---

## 4. Additions (not in the legacy renderer at all)

### 4.1 Post-process pass — film grain + chromatic aberration
- New DUDE "improvements over the classic engine" effects
  (`r_postFilmGrain`, `r_postChromaticAberration`), **default off** (strength 0
  is an exact passthrough). One fullscreen pass over `_currentRender` after the
  3D view and **before** 2D/GUI, so the HUD is never affected.
- **Fidelity impact:** none by default; a look change only when enabled.
- **Files:** `neo/shaders/postprocess.*`; `RhiBackend.cpp` (`RB_RHI_PostProcess`).

---

## 5. Implementation bridges (same output, non-final plumbing)

These are **not** behavioral divergences — the output matches legacy — but they
use direct GL where the future Vulkan backend will need proper RHI
abstractions. Marked `TODO(RHI)` in the code.

- `_currentRender` / `_currentDepth` copies (`RC_COPY_RENDER`, the
  post-process-surface copy, the film-grain snapshot) call
  `idImage::CopyFramebuffer` directly instead of an RHI copy.
- Polygon offset, stencil op/func, and `GL_STENCIL_TEST` enable are direct
  `qgl*` calls.
- Engine textures bind via `idImage::Bind()` rather than `DrawArgs::textures`.

---

## Appendix — corrections *toward* legacy (not divergences)

Noted so a reader auditing these areas isn't surprised:

- **Fog "enter" fade restored.** `fog.vert` from Phase 2 had collapsed the
  fog-enter coordinate to a constant (`u_localParam0.xy`), dropping the soft
  enter/exit transition. It was restored to the per-vertex plane, matching the
  fixed-function `RB_T_BasicFog`. (Required adding `texGen1T` to the shared
  uniform block.)

## Appendix — pre-existing divergence (not authored by this work)

- **Degenerate-depth GUI models.** For cropped GUI-renderDef 3D model views
  (e.g. the main-menu planet), interactions use `DEPTHFUNC_ALWAYS` instead of
  `EQUAL`, because the content-authored projection is depth-degenerate and
  `EQUAL` bands the lighting into stripes. Listed for completeness; it is a
  separate change currently under retest, not part of the work above.
