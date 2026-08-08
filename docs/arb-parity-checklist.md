# Legacy ARB → RHI parity checklist

Tracks which special-case rendering mechanisms of the original Doom 3 ARB
backend are transported into the GL3 and Vulkan RHI backends, and where each
one can be eyeballed in-game. Test each row on **GL3** and **Vulkan**, ideally
A/B'd against the **legacy ARB** backend in the same spot.

Legend: ✅ ported · ⚠️ ported but verify against ARB · ❌ missing/degraded

## Genuine gaps (degrade to "skip")

| Mechanism | GL3 | VK | Notes |
|-----------|-----|----|-------|
| GlassWarp (`TG_GLASSWARP`) | ❌ | ❌ | `MaterialIR.cpp:242`. **0 stock materials use it** — dormant SDK path, only a mod could trigger. Not a custom-ARB program (it's a texgen), so the gap-#2 runtime compiler doesn't cover it. |
| Custom (non-stock) ARB stages | ✅ transpiled | ✅ transpiled (shaderc) | **Gap #2 FIXED.** VK now runtime-compiles the transpiled ARB→GLSL via shaderc (`DUDE_RUNTIME_ARB_COMPILER`, default on). Covers stock d3xp (bloodOrb1/2/3, enviroSuit, flare, motionBlur) **and** arbitrary mod ARB shaders. Built without shaderc → degrades to skip as before. See the gap-#2 note below. |

## Verification checklist (ported — confirm it matches ARB)

| # | Effect | Stock usage | Where to test | GL3 | VK | Verdict |
|---|--------|-------------|---------------|-----|----|---------|
| 1 | Heat haze (+ mask/vertex variants) | 65 mtr | plasma/rocket fire, imp fireballs, Alpha Labs flames, Recycling steam | ✅ | ✅ | ✅ good |
| 2 | colorProcess (per-frame grade) | 8 mtr | old-film/static monitors, PDA video static, damage screen overlay | ✅ | ✅ | ✅ good |
| 3 | Environment / bumpyEnvironment (`texgen reflect`) | 7 mtr | shiny/wet metal trim, chrome pipes | ✅ | ✅ | ✅ good |
| 4 | Blend lights (`blendLight`, `RB_RHI_BlendLightChain`) | 6 mtr (all base `fogs.mtr`; d3xp reuses) | `game/comm1` (glare+pitfog), `game/enpro`/`hell1` (glare), `game/phobos1` (31 glares), `game/erebus2` (pitfog) | ✅ | ✅ | ✅ good |
| 5 | wobbleSky | 9 mtr | Hell sky (`textures/skies/hellsky2/3/4`): base `game/hell1` / `game/hellhole`, ROE `game/hell` | ✅ | ✅ | ✅ good (sky churns) |
| 6 | mirrorRenderMap | 5 mtr | Delta Labs door gutters, reflective hell stone | ✅ | ✅ | ✅ good |
| 7 | fogLight | 3 mtr | Erebus/cavern/Hell fog volumes | ✅ | ✅ | ✅ good (MD5-in-fog fixed) |
| 8 | remoteRenderMap (live cam feed) | 3 mtr (`cameraImg1/2`) | security monitor screens (Mars City / Administration) | ✅ | ✅ | ✅ good |
| 9 | Portal sky (`_currentRender` + `screen` texgen → `portalsky` builtin) | **ROE only** | `textures/smf/portal_sky`; **base D3 has ZERO `info_portalSky`** (mars_city1 is enclosed; its "portals" are visportals). ROE maps: **phobos1/2/3**, deltax, hell (`devmap game/phobos1`). Not gap #2 — ARB-program version is commented out, uses fixed-function `screen` texgen. | ✅ | ✅ | ✅ good (phobos3) |
| 10 | Soft particles | depth-fade | steam/smoke fading against geometry (Recycling) | ✅ | ✅ | ✅ good |
| 11 | Berserk vision | — | ✅ fixed (radial zoom-blur port) | ✅ | ✅ | done |

**All 11 rows verified ✅ on GL3 + Vulkan (user-confirmed).** The legacy-ARB → RHI
special-effect port is complete.

**Blend-light detail (row 4).** Three visual flavors, all defined in base `fogs.mtr`
(`RB_RHI_BlendLightChain`, `blendlight` shader), d3xp reuses them:
- `fogs/glare` / `glare2` / `glare_snd` — **additive** colored glow/haze filling a light
  volume (the common one; near every map — phobos1 has 31).
- `fogs/pitFog` / `sentest` — **alpha** colored darkening fog gradient (pits/chasms;
  base `comm1`/`mc_underground`, ROE `erebus2/3/4`).
- `fogs/filter` — `GL_ZERO, GL_ONE_MINUS_SRC_COLOR` darkening filter; **placed in ZERO
  stock maps** (defined-but-unused, like the dead `flare.vfp`) — untestable, don't chase.

## Gap #2 — custom ARB stages on Vulkan (FIXED)

d3xp/mod materials with a custom `vertexProgram`/`fragmentProgram` rendered
**invisible** on the Vulkan backend: `IR_ResolveCustomArb` returned 0 on VK (no
runtime GLSL→SPIR-V compiler) and `IR_VkBuiltinForArb` only mapped the
hand-translated `heatHaze`/`colorProcess` builtins, so every other custom `.vfp`
became `SK_SKIP`. GL3 was never affected (it transpiles + driver-compiles at
runtime).

**Fix (shipped):** a runtime GLSL→SPIR-V path on the Vulkan backend via **shaderc**
(`DUDE_RUNTIME_ARB_COMPILER`, default on; degrades to the old skip when built
without it). It reuses the existing ARB→GLSL transpiler (`arb::ToGlsl`), which
already emits backend-neutral GLSL (`SAMPLER_BINDING`/`VARY`/`UBO_BINDING` +
`#include "arbparams.glsl"`), so one path now serves both backends:

- `RHI::CreateShaderFromGlsl(name, vertSrc, fragSrc)` — new virtual. GL3 forwards
  to `GL3_FindProgramFromSource` (driver compile); Vulkan compiles via shaderc with
  the **same** preprocessing `compile_spv.py` uses (prepend `prelude.vk.glsl`,
  inject `invariant gl_Position;` for the vertex stage, textually expand `#include`,
  target `vulkan1.4`) and caches in `shaderTable` exactly like `LoadShader`.
- `IR_ResolveCustomArb` is now backend-agnostic (transpile → `CreateShaderFromGlsl`);
  the VK builtin table is still tried first, and `SK_SKIP` only remains as the
  no-runtime-compiler fallback.
- `RB_RHI_RenderCustomStage` binds `fragmentProgramImages` into `DrawArgs.textures[]`
  on Vulkan (the `qgl*` active-unit binds are no-ops there) with the `_currentRender`
  capture guard; GL3's active-unit path is unchanged.

**Covers:** the stock RoE effects — Artifact/Heart-of-Hell blood-orb (`bloodOrb1/2/3`),
hell-time directional `motionBlur`, `enviroSuit` visor warp, glass `flare` — **and**
arbitrary mod ARB shaders. The generic `ArbParams` fill already supplies every uniform
these programs read (`fenv[0]`, `fenv[1]`, `vlocal[1]`), so even motionBlur needs no
per-program plumbing. **Not covered:** `TG_GLASSWARP` (a texgen, not a custom program;
0 stock users). **Verify in-game:** ROE on the **Vulkan** backend — Artifact use
(blood-orb + hell-time blur), an enviro-suit section, glass flares around lights.

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
