# Depth linearisation follows the view's projection (cinematics lower the near plane)

## The bug

Every screen-space shader that reads `_currentDepth` turns the stored window depth into a view
distance. They all did it with a hard-coded pair:

    const vec2 depth_consts = vec2( 0.33333333, -0.33316667 );     // 1/vz = raw * x + y

That pair is not a universal constant - it is Doom 3's projection evaluated for `r_znear = 3`
(`R_SetupProjection`, tr_main.cpp: `P[10] = -0.999`, `P[14] = -2 * zNear`). **While a cinematic
camera is active the game sets `r_znear` to 1** (`idGameLocal::SetCamera`, all three game DLLs and
every SDK-derived mod: "so that transitioning into/out of the player's head doesn't clip through
the view"; restored to 3 afterwards). With the play-time pair, every view distance computed during
a cutscene came out **3x too large**. (`renderView.cramZNear` would quarter the near plane again,
but nothing in the game code sets it. *An earlier version of this note blamed cramZNear and said
4x - wrong mechanism, same fix: the pair must come from the projection.*)

Found 2026-09-18 through `r_rtShadowBlur`: in the intro cinematic the blurred ray-traced shadows
lost real shadows and gained stray soft patches, and a staggered light flickered between its mask
and the inline fallback ray (docs/rtx-shadow-blur.md). That shader was fixed first and
user-verified; this change applies the same fix everywhere.

## The fix

General form, from the projection matrix `P` (column-major GL layout):

    z_ndc = -P10 - P14 / vz,   raw = ( z_ndc + 1 ) / 2
    =>   1/vz = raw * ( -2 / P14 ) + ( 1 - P10 ) / P14

- New `RenderParams::depthParms` / `u_depthParms` (vec4, appended LAST - block 1008 -> 1024 bytes,
  static_assert updated): `.xy` = the pair for this view.
- `rhi::FillDepthParms( parms, proj )` (RenderParams.h) fills it; called at all 12 sites that set
  up a depth-reading pass (RhiWorld.cpp x10, RhiBackend.cpp x2), from `backEnd.viewDef`.
- Shaders use `DUDE_DEPTH_CONSTS()` (renderparms.glsl): the uniform when provided, else the
  play-time pair - so a pass that was missed behaves exactly as before, never worse.
- Switched: ssao, ssao_blur, ssao_temporal, ssao_depthmip, rtao_ray, rtao_viewz, ssr, ssr_rt,
  ssr_temporal, ssr_depthmin, environment_ssr, softparticle, rtshadow_ray (which moves from its
  private `u_depthTexRecip.zw` copy to the shared uniform).

In play the computed pair equals the old constants to within one float ulp
(`-2 / -6`, `( 1 + 0.999 ) / -6`), so gameplay rendering is unchanged; only cutscenes change.

## What was wrong in cutscenes before (expected, from the code - not individually verified)

- **SSAO / GTAO**: the scene read 3x larger, so the world-space AO radius covered a third of the
  intended neighbourhood - weaker, tighter AO.
- **RTAO**: rays started 3x too far along the view ray - behind the surfaces.
- **SSR / RT reflections**: ray origins and depth comparisons in a 3x-scaled space, reprojected
  with the true matrix.
- **Soft particles**: scene-vs-particle distance 3x too large, so the soft fade was 3x shorter.

## Not changed

- Raw-depth thresholds such as the sky cut `raw >= 0.9994` (about 30000 units in play, about 10000
  with the cinematic near plane - still far beyond any cutscene set).
- The legacy ARB soft-particle program (stock dhewm3 path) keeps its constants.

## Verify

Doom 3 intro cinematic and the RoE intro, Vulkan and GL3, with SSAO (or RTAO), SSR and soft
particles on: AO on the actors should now look like gameplay AO; then any gameplay scene to
confirm nothing changed there. Builds; SPIR-V validates; `validate.py` shows the same six
Vulkan-only GL failures as master. **USER-VERIFIED 2026-09-18 ("it all looks right")**, together
with eye adaptation standing down in cinematics (docs/hdr-pipeline.md).
