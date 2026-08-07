# Legacy ARB → RHI parity checklist

Tracks which special-case rendering mechanisms of the original Doom 3 ARB
backend are transported into the GL3 and Vulkan RHI backends, and where each
one can be eyeballed in-game. Test each row on **GL3** and **Vulkan**, ideally
A/B'd against the **legacy ARB** backend in the same spot.

Legend: ✅ ported · ⚠️ ported but verify against ARB · ❌ missing/degraded

## Genuine gaps (degrade to "skip")

| Mechanism | GL3 | VK | Notes |
|-----------|-----|----|-------|
| GlassWarp (`TG_GLASSWARP`) | ❌ | ❌ | `MaterialIR.cpp:242`. **0 stock materials use it** — dormant SDK path, only a mod could trigger. |
| Custom (non-stock) ARB stages | ✅ transpiled | ❌ skip | VK has no runtime SPIR-V compiler (`MaterialIR.cpp:86`); d3xp/mod customs invisible. Stock base+ROE customs hand-translated to builtins. |

## Verification checklist (ported — confirm it matches ARB)

| # | Effect | Stock usage | Where to test | GL3 | VK | Verdict |
|---|--------|-------------|---------------|-----|----|---------|
| 1 | Heat haze (+ mask/vertex variants) | 65 mtr | plasma/rocket fire, imp fireballs, Alpha Labs flames, Recycling steam | ⬜ | ⬜ | |
| 2 | colorProcess (per-frame grade) | 8 mtr | old-film/static monitors, PDA video static, damage screen overlay | ⬜ | ⬜ | |
| 3 | Environment / bumpyEnvironment (`texgen reflect`) | 7 mtr | shiny/wet metal trim, chrome pipes | ⬜ | ⬜ | |
| 4 | Blend lights (projected darkening cookies) | 7 mtr | light gobos, projected gradients | ⬜ | ⬜ | |
| 5 | wobbleSky | 9 mtr | Hell swirling sky, some Mars exteriors | ⬜ | ⬜ | |
| 6 | mirrorRenderMap | 5 mtr | Delta Labs door gutters, reflective hell stone | ⬜ | ⬜ | |
| 7 | fogLight | 3 mtr | Erebus/cavern/Hell fog volumes | ⬜ | ⬜ | |
| 8 | remoteRenderMap (live cam feed) | 3 mtr (`cameraImg1/2`) | security monitor screens (Mars City / Administration) | ⬜ | ⬜ | |
| 9 | Portal sky | `_currentRender` | sky through Mars City windows, Site-3 exteriors | ⬜ | ⬜ | |
| 10 | Soft particles | depth-fade | steam/smoke fading against geometry (Recycling) | ⬜ | ⬜ | |
| 11 | Berserk vision | — | ✅ fixed (radial zoom-blur port) | ✅ | ✅ | done |

## Bugs found during testing

- **[FIXED] Fan-blade shadow wrong (shadow-map projection mismatch).** ROOT-CAUSED. Fan
  lights (`lights/fanblade3`, `lights/fanlightgrate`, `lights/fanlightgrateSC`) are
  projected lights whose shadow is a rotating *gobo* (`rotate time * -1` on the
  projection stage). The rotation is baked into the projection planes by shared code
  (`tr_render.cpp:830` `RB_BakeTextureMatrixIntoTexgen`). DUDE's 2D shadow map is
  **rendered** with the *raw* light projection (`RhiWorld.cpp:1662`) but the
  interaction shader **samples** it with the *baked/rotating* projection
  (`interaction.frag:97`, `var_TexProjection`). Map written at one UV, read at a
  rotating UV → false occlusion that sweeps with the fan = "rotating dark edge".
  Only on RHI (legacy ARB never shadow-mapped this light). Confirmed: `r_shadowMapping 0`
  restores the correct gobo shadow. Cube (point-light) path is immune (direction-based).
  **Fix (shipped):** added a separate unbaked projection texgen for the 2D shadow lookup —
  `RenderParams.shadowProjectionS/T/Q` + `u_shadowProjectionS/T/Q`, a `var_ShadowProjection`
  varying (loc 12) threaded vert→tesc→tese→frag, filled in `RB_RHI_DrawInteraction` from
  `R_GlobalPlaneToLocal(vLight->lightProject[0..2])` (the same raw planes the caster renders with);
  `interaction.frag` samples the shadow map with it instead of `var_TexProjection`. No change for
  non-rotating projected lights (raw==baked); cube/unshadowed paths never read it. Builds + shader
  validate + adversarial review all clean. GL3 + Vulkan (shared GLSL).
  **Why some fans looked fine already:** the bug only fires when a fan light is projected *and*
  shadow-casting (not `noShadows`) *and* wins a 2D map slot; point-light, `noShadows`, or
  un-mapped fans (e.g. mars_city1 body-scan area) show the clean gobo on both backends.

- **[FIXED] MD5 models don't blend in fog (tessellation vs fog depth mismatch).** With
  `r_tessellation 1` (Vulkan-only), a monster/character is PN-tessellated (+ normal-displaced,
  `r_tessDisplace` default −0.25) in the zfill depth prepass and interaction passes, but the fog
  pass drew the **flat** model. The fog interaction chains run at `DEPTHFUNC_EQUAL`, so the flat
  fog fragments failed the depth test against the tessellated depth → the model rendered
  **un-fogged**, reading as a dark silhouette in bright fog (Hazardous Material Control, yellow fog).
  Confirmed: `r_tessellation 0` fixes it. The fog port itself is a faithful line-for-line copy of
  legacy `RB_FogPass` (world geometry always fogged fine). **Fix (shipped):** the fog interaction
  pass now tessellates identically to zfill — new `fog.tesc`/`fog.tese` mirror `zfill.tesc`/`.tese`
  (same `dudeTessPN`+`dudeTessDisplace`, `invariant gl_Position` → bit-identical depth), `fog.vert`
  emits the control net, `RB_RHI_FogChain` gained `allowTess` (true for the two interaction chains,
  false for the frustum-volume fill), binds the model bump on unit 2 via the shared
  `RB_RHI_TessBumpForZfill`. Inert when `r_tessellation` off and on GL3 (never tessellates).
  Rule my memory already flags: **every `DEPTHFUNC_EQUAL` pass must tessellate identically to
  zfill** — the fog pass was the one that was missed.
