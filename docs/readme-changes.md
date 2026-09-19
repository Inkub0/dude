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

### 1.4 Cube-map ("sheen") reflections dampened 30% on glass
- **Legacy:** the cube-reflection stage of glass (`textures/glass/*`,
  `blend gl_dst_alpha, gl_one`, `cubeMap env/genN`, `texgen reflect`) draws the
  environment map at its stage-colour brightness.
- **Ours:** the reflection is scaled by `r_gl3ReflectionScale` (default **0.7**,
  i.e. −30%) in `RB_RHI_RenderTexgenStage`. `1.0` restores the untouched cube.
- **Why:** the reflection itself is a faithful, correct port (same static cube,
  same stage-colour modulation, same `dst_alpha` mask — all verified). But the
  glass *also* refracts the live scene through its heatHaze stage, and the
  enhancement backend lights that scene brighter than the original renderer
  (SSAO, emissive fill lights, ambient). Against that brighter backdrop the sheen
  reads noticeably stronger than on legacy `opengl`. Rather than chase an exact
  match through the (correct) reflection path, we apply a flat compensation.
- **Fidelity impact:** glass looks marginally clearer/less mirror-y than a pure
  faithful port; tune or disable with `r_gl3ReflectionScale 1`. Only affects
  `TG_REFLECT_CUBE` stages (glass, some reflective decals).
- **Files:** `neo/renderer/rhi/RhiBackend.cpp` (`RB_RHI_RenderTexgenStage`).

### 1.5 Decal sheen control (mod-pack blood / dirt decals)
- **Legacy:** a decal material is drawn exactly as authored. Texture mod packs give
  overlay decals a `specularmap` + heightmap `bumpmap` and an additive
  `texgen reflect` cube stage, so every blood splat and dirt stain shines like chrome
  under any light, on every preset.
- **Ours:** `R_DecalSheenScale` classifies `SS_DECAL` materials. Blood (the `blood`
  content flag, or a name containing blood/splat/smear/handprint) scales **both** sheen
  sources by `r_decalBloodSheen` (default **0.35**; 1 = as authored): the interaction's
  specular modifier, and the reflect-cube stage (stage colour for `environment`,
  `u_localParam0.w` for `bumpyenvironment`, which ignores stage colour like vanilla).
  Every other *translucent* decal goes fully matte with `r_decalMatte` (default **1**).
  Opaque decals (vents, wires, posters, bullet holes) are untouched.
- **Fidelity impact:** none on the stock assets (their overlay decals have neither a
  specularmap nor a cube stage). With a mod pack the decals are deliberately less shiny
  than authored; `r_decalBloodSheen 1; r_decalMatte 0` restores the pack's look.
- **Files:** `RenderSystem_init.cpp`, `rhi/RhiWorld.cpp` (`RB_RHI_DrawInteraction`),
  `rhi/RhiBackend.cpp`, `shaders/bumpyenvironment.frag`; sliders in Debugging → Glass Reflections.

### 1.6 Decal surface mask (`r_decalMetalMask`, `r_decalRoughen`)
- **Legacy / plain PBR:** a wall's metalness and roughness are one value per material, and the
  forward renderer lights the wall before any decal blends over it — a dirt stain on a steel
  plate darkens it, but the plate still mirrors the light straight through the stain.
- **Ours:** before the light loop `RB_RHI_DecalMaskPass` draws every overlay decal
  (translucent, `SS_DECAL` or polygon-offset; stock and mod alike) into a view-sized
  *retention* buffer, multiply-blended so overlaps compound (`shaders/decalsurface.glsl`):
  - **R — metalness kept** (`r_decalMetalMask`, 1): dirt, blood and soot are dielectrics.
  - **G — smoothness kept** (`r_decalRoughen`, 1): dirt scatters; roughness climbs to 0.85,
    on metals and glossy non-metals alike.
  Coverage is whatever the decal's own unlit blend hides: alpha for `blend blend`, luminance
  for the darken blend (`gl_zero, gl_one_minus_src_color`), `1 - luminance` for filter blends;
  stage + vertex colour included, so fading game decals give the surface back. Additive and
  lit stages are skipped. The target has no depth, so `decalmask.frag` tests against
  `_currentDepth` in linear units.
- **Consumers:** `interaction.frag` (sampler unit 14, gated by `u_pbrParms.z == 2`), and the
  SSR / RT-reflection passes through `RB_RHI_DecalResolveMaterial`, which resolves the
  G-buffer's rough/metal attachment through the mask once per view (`decalmat.frag`,
  texel-exact) so reflections and highlights agree about the same stain.
- **Scope:** PBR only (`r_pbr`), primary fullscreen view.
- **Fidelity impact:** non-vanilla; refines surfaces only under decals. Both at 0 restore
  plain PBR.
- **Files:** `rhi/RhiWorld.cpp`, `shaders/decalmask.*`, `shaders/decalmat.*`,
  `shaders/decalsurface.glsl`, `shaders/interaction.frag`, `rhi/RHI.h` +
  `rhi/vk/VulkanBackend.cpp` (texture binding 14); sliders in Debugging → Glass Reflections.

### 1.7 Wet blood film (`r_decalWetBlood`)
- **Legacy:** blood is a matte darkening stain. Texture packs fake wetness per material
  (specular + heightmap bump + a cube stage) — but only on map-placed decals: every splat the
  game *spawns* is a projected decal (`idRenderModelDecal`), which the frontend never lights
  and which carried no tangent space at all, so no material can make those shine.
- **Dead end, recorded:** wetness as a property of the wall underneath (lower its roughness /
  level its normal under the blood). The glint then traces the *wall's* relief, RT reflections
  draw its grooves across the splat, and a levelled film under Doom 3's point lights shows
  nothing at all — a flat mirror only glints at a light's exact mirror angle. Blood reads wet
  because its **own** relief always has some facet catching a light.
- **Ours:** every blood decal (`R_DecalIsBlood`; darken or filter blend) is lit as a surface
  of its own. `RB_RHI_DecalMaskPass` collects them; after each light's translucent chain
  `RB_RHI_DrawFilmInteractions` culls them against the light and submits a synthesized
  specular-only interaction (`RB_CreateFilmDrawInteractions`) through the regular
  interaction path, so shadow maps, RT shadows and PBR all apply:
  - **relief** = the material's authored bump stage when it has one, else
    `smoothnormals(heightmap(<cover image>, 4))` (inverted for filter blends) generated with the
    engine's image programs; `r_decalWetBloodRelief` (0.17, 0..1) scales the slope in the shader.
  - **film** = dielectric GGX at `r_decalWetBloodRough` (0.18), strength `r_decalWetBlood`
    (0.057, 0..0.5).
  - **coverage + gain** come from the decal's own cover image on the diffuse unit
    (`interaction.frag` film mode, `u_channelMask`): coverage is shaped into an even film that
    feathers in above `r_decalWetBloodThreshold` (0.3) — splat textures trail off into a mist
    too faint to see, and without that floor the bare floor around a splat glints as if wet —
    and the glint is pre-divided per channel by the blend's transmittance —
    it is drawn in the light loop, *before* the decal's blend multiplies the framebuffer.
  - projected decals get a tangent frame at creation (`R_DecalWindingFrame`), and the draw
    uses the ambient pass's polygon offset to win depth-LESS against its coplanar wall.
  - when a map-placed decal's material authors its own specular (texture packs), the film
    replaces that highlight; the pack's cube sheen stays on `r_decalBloodSheen`.
- **Scope:** PBR only, primary fullscreen view; not overlays on characters (no tangent space),
  not decals whose blend pass tessellates. Up to 96 decals per view, one draw per decal per
  overlapping light.
- **Defaults:** all four values are the project owner's in-game tuning (2026-09-19).
- **Fidelity impact:** non-vanilla and visibly changes stock blood. **On by default, but only
  where PBR runs** (High preset and up) — Potato..Medium keep the matte, faithful blood.
  `r_decalWetBlood 0` turns it off.
- **Files:** `ModelDecal.cpp`, `tr_render.cpp`, `rhi/RhiWorld.cpp`, `shaders/interaction.frag`;
  sliders in Debugging → Glass Reflections.

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
