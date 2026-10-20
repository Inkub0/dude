# E1 - emissive triangle table (scope authored 2026-10-17; E1a-E1d IMPLEMENTED 2026-10-19, user validation pending)

**Status 2026-10-19:** branch `feat/rtx-emissive-table` (off master 17907505). E1a (census), E1b (the table), E1c (flux
+ CDF) and E1d (debug overlay) are built in one pass behind `r_rtEmissiveTable` (default 0 = nothing built; 1 = the
table exists but NOTHING consumes it, so the picture is unchanged either way). E1e (movers + the animated-intensity
decision) is deferred until the census and the overlay are validated. What was built, where, and how to test it: the
"Implementation" section at the end of this file.


The data foundation of the experimental emissive-lighting mode (docs/rtx-experimental-lighting.md): a GPU table of the
scene's emissive TRIANGLES, each with the flux and geometry needed to sample it as an area light. E1 builds the table
only; E2 consumes it (as a second candidate stream, fed through the light grid). Nothing it builds changes a pixel until
E2, so E1 is bit-identical behind its cvar.

Prerequisites already shipped: the irradiance cache (multi-bounce) and the **light grid** (E3's many-lights handler, so
thousands of emitters are tractable). E1 + E2 also pay off on the NORMAL tier: fixtures and screens that today only
self-illuminate on a bounce would start LIGHTING their surroundings.

## The finding that shapes E1 (recon 2026-10-17)

**The strongest emitters are NOT in the current RT scene.** An additive-only material (a neon tube, a pure light
texture) is classified `MC_TRANSLUCENT` -> `MF_NOSHADOWS` (Material.cpp:2796-2821), so `SurfaceCastsShadow()` is false
and `R_RtSurfCasts` (tr_main.cpp:1185-1190) rejects it: it is in no BLAS and no geo-table row. Only "glow-on-opaque"
surfaces (an `MC_OPAQUE`/`MC_PERFORATED` material that ALSO has an additive stage - console panels, lit readouts) are
captured today, and their glow is sampled per-pixel at a ray hit (rtgi_bounce.glsl:149-224), never as a light.

Consequence: **E1 needs its OWN emissive-surface gather**, independent of `R_RtSurfCasts` - iterate every surface whose
material has an additive stage, regardless of coverage / shadow flags. This is fine because an emitter does not need to
be a TLAS caster: E2 samples a point on the triangle and traces a visibility ray toward it against the EXISTING TLAS
(the opaque world provides the occluders). The emitter table is a light-sampling structure, parallel to the scene-light
buffer, not part of the caster set.

## Hybrid lighting is the intended mode (user, 2026-10-17)

The pure-emissive mode's weakness is Doom 3's fixture-LESS ambience lights (the red/blue/teal mood/fill washes, cheated
bounce, Hell's coloured light) that have no emissive geometry - drop them and those areas go black. The decided
direction is a **HYBRID**: emissive triangles light the glowing surfaces (fixtures, screens, lava) AND the ambience
lights that carry no emissive proxy are KEPT. It needs no new lighting path - the light grid (done) already pools
candidates, so hybrid is simply "feed the kept analytic lights + the emissive triangles into the same grid," sampled
together by ReSTIR. The only real problem is DOUBLE-COUNTING: a fixture with both a placed light and an emissive tube
would be lit twice, so a light WITH an emissive proxy is dropped (its surface is the light) and a light WITHOUT one (the
ambience) is kept. Classification signals, best -> fallback: (1) Doom 3's ambient-light material flag (fill lights are
often authored ambient); (2) a build-time geometric test - a light with an emissive surface inside its volume is
"fixtured"; (3) a per-light override (the D2 table mechanism); (4) colour saturation (ambience is usually saturated) as a
fallback only. **This refines D1 and is decided at E5; it does NOT change E1 or E2** - the emissive table + its consume
are prerequisites of hybrid and pure-emissive alike. (`r_xlKeepGameplayLights` becomes the hybrid-vs-pure A/B.)

## What E1 delivers

A device buffer `emissive triangles[]` + a flux CDF, built at `R_RtWorldUpdate` (tr_main.cpp:2654-2743), its device
address threaded into `RenderParams` exactly like the scene-light buffer (`s_rtReflLightBuf`, RhiWorld.cpp:5897-5901).
Per entry, enough to importance-sample the emitter and shade a sampled point:
- the triangle's 3 WORLD-space positions (sampling a point + its area + its normal),
- the emissive texture's bindless slot (`R_RtMaterialEmitIndex`, tr_main.cpp:1912) + a representative `st` (per-vertex or
  centroid) so E2 can read the emitted colour at the sampled point,
- `flux` (for the CDF / importance) and the material intensity scale (D2).

Plus a **flux CDF** (prefix sum over the table) so E2 can draw a triangle proportional to flux with a binary search;
Morton-ordering the triangles first (merian `light-cdf.slang`) gives the CDF spatial locality for the light grid later -
an optional refinement, not needed for a first correct E1.

## Flux = area x mean glow luminance x material scale

- **area**: world-space triangle area from the 3 transformed positions.
- **mean glow luminance**: `idImage::averageColor[3]` of the additive stage's image (Image.h:302) - the alpha-weighted
  mean RGB, already cached per image. **Caveats (recon D):** it is in GAMMA space (must be linearized for a sane flux),
  it is alpha-weighted (an approximation of area-integrated radiance, good enough to rank emitters), and it DEFAULTS TO
  WHITE for images that never went through the averaging path (dynamic / GUI / cinematic). Guard the white default: an
  emissive whose stage image is a dynamic/GUI image is a SCREEN -> it already has the legacy emissive fill-light
  (tr_light.cpp:1316-1460), so E1 skips it (consistent with D1 "GUI screens stay fill lights").
- **material scale**: the D2 per-material intensity (emissive/emissive_materials.cfg + emissive_overrides.cfg). E1
  bootstraps this to 1.0 (or a per-category default) and reads the table when D2 lands; the in-game editor is E6.

## The gather (CPU, in / beside the BLAS builders)

The per-triangle data - `tri->verts` (idDrawVert: xyz, st, normal), `tri->indexes`, and the surface's world transform -
is already in hand in the gather loops the BLAS builders use. E1 adds an emissive pass over the same surface sets, minus
the `R_RtSurfCasts` gate, plus the emissive-stage test:
- **static worldspawn (`_areaN`)**: R_RtGatherWorld / R_RtBuildWorldAreaBlas (tr_main.cpp:1238-1289, 2340-2355) -
  `tri->verts` are already WORLD space (identity). The cleanest, largest emitter source.
- **static entities (`DM_STATIC`)**: R_RtGatherEntities / R_RtBuildEntityModelBlas (tr_main.cpp:1297-1398, 2458-2473) -
  model-space verts x the entity matrix (`R_AxisToModelMatrix`, tr_main.cpp:1395).
- **CPU-skinned monsters**: R_RtGatherMonstersWorld (tr_main.cpp:1523-1581) - posed verts x `vEnt->modelMatrix`. Rare for
  emissives (and additive stages are translucent), low priority.
- **GPU-skinned monsters (blocker B)**: verts live only in `gpuSkinVB` (GPU), not CPU-readable - SKIP in E1 (monster
  glow as an area light is negligible; revisit with GPU extraction only if ever needed).

## Rebuild lifecycle (recon E)

- **Static** (world + static entities): built ONCE per map, cached by map name, at the same point the static BLASes are
  (tr_main.cpp:2678-2743). The bulk of emitters, and they never move.
- **Movers** (func_movers with emissive skins): their transform refreshes per frame in `R_RtRefreshInstances`
  (tr_main.cpp:2682) - re-bake only their triangles' world positions each frame (small count), like the TLAS instances.
- **Animated intensity caveat:** a build-time flux SNAPSHOT freezes glow that pulses / flickers (shader-register-driven
  stage colour), which the per-pixel path samples live today. Decide at E1e: (a) accept frozen flux for the table's
  IMPORTANCE (the ranking barely changes), and let E2 read the LIVE emitted colour at the sampled `st` for the actual
  radiance (so a pulsing light still pulses - only its sampling PROBABILITY is stale); (b) refresh the per-material
  scale each frame from the shader register. (a) is simpler and almost certainly enough.

## Open decisions (for when we build)

- **Per-triangle granularity + size.** One entry per triangle is finest and matches merian, but a finely-tessellated
  emissive surface could mint many entries. Entry ~48-64 B (3 world verts + normal + st + slot + flux + scale); 10k
  emitters = ~0.6 MB, fine; a pathological level could be larger - mitigations if needed: merge coplanar/same-material
  triangles into quads, or cap by flux. Decide after measuring a real level's emissive-triangle count (E1a).
- **Store world verts vs reference the geo VB.** Direct world verts (simple, E2-friendly, re-baked for movers) vs
  storing (geo row, primitiveIndex) and fetching verts on the GPU (8 B, but indirection + the mover/skin position
  problem). Start with direct world verts.
- **Where E2 plugs in.** The emissive triangles become candidates in the light grid (E3, done): the grid's fill would
  stream the emissive table alongside the analytic lights. That is where blocker-#A's "thousands of emitters" is handled
  and where L4 (feedback + bins) earns its cost. E1 just has to produce a clean, CDF-ordered table.

## Stages

- **E1a feasibility + census.** The new emissive-surface gather (ignore `R_RtSurfCasts`, test for an additive stage with
  a resident, non-dynamic image). Count emissive triangles per level (a `common->Printf`), confirm the pure-emissive
  surfaces (neon) are now seen and the GUI screens are excluded. Confirm `averageColor` linearization + white-default
  guard on a few known emitters. No buffer yet. Decides the size question.
- **E1b the table (CPU + GPU).** Emit per-triangle entries (world verts, normal, st, emit slot) into a device buffer
  built at `R_RtWorldUpdate`; address into `RenderParams`. Static once per map. `r_rtEmissiveTable` gates it; off =
  nothing built, bit-identical.
- **E1c flux + CDF.** Per-triangle flux (area x linearized mean glow luminance x scale); prefix-sum CDF. Optional
  Morton pre-sort.
- **E1d debug view.** Overlay the emissive triangles (e.g. tint by flux, or draw the table's triangles) so the gather +
  flux are visibly correct - the L1-style validation that catches wrong surfaces / wrong flux before E2 consumes it.
- **E1e movers + animated intensity.** Per-frame transform refresh for emissive movers; the animated-intensity decision
  above.

Then E2 (a separate plan) consumes the table.

## cvars (RT tier, VK + ray query; default OFF = no table built, bit-identical)

| cvar | default | meaning |
|---|---|---|
| `r_rtEmissiveTable` | 0 | build the emissive-triangle table at level load (does nothing until E2 consumes it) |
| `r_rtEmissiveTableDebug` | 0 | overlay: 1 emitter triangles, 2 per-triangle flux (tonemapped), 3 emitter density |
| `r_rtEmissiveScale` | 1 | global multiplier on emissive flux (until the D2 per-material table lands) |

## Blockers / notes (recon 2026-10-17)

- **A (shapes the design):** pure-emissive surfaces are excluded from `R_RtSurfCasts` -> E1 needs its own gather. Done.
- **B:** GPU-skinned emissive triangles have no CPU positions -> skip (negligible).
- **C:** no per-triangle structure exists anywhere (geo table is per-surface) -> E1 is net-new, built in the BLAS-builder
  loops where verts/indexes/st/normal + transform are in hand.
- **D:** `averageColor` is gamma-space, alpha-weighted, white-default -> linearize, guard, skip dynamic/GUI images.
- **E:** static built once/map, movers refresh per frame; animated glow intensity frozen in the snapshot -> E2 reads live
  colour at the sampled st (option a).
- **F:** `RtLight` pad0/pad1 are taken (lightDef index+1, serial); bits 6-15 free. Use a SEPARATE emitter buffer (the
  `s_rtReflLightBuf` pattern), keyed by lightDef index+serial if a link is ever needed.

## Anchors (file:line)

- Emissive classification: `R_RtMaterialEmitIndex` tr_main.cpp:1912-1925; packed to geo-table flags bits 16-31
  (VulkanBackend.cpp:6541); consumed rtgi_bounce.glsl:149-224. GUI-screen fill lights: tr_light.cpp:1316-1460.
- RT world update / gather: `R_RtWorldUpdate` tr_main.cpp:2654-2743; `R_RtSurfCasts` tr_main.cpp:1185-1190; builders
  tr_main.cpp:2317-2390 (world), 2404-2521 (static entities), 1523-1581 (CPU-skinned monsters); `R_RtRefreshInstances`
  tr_main.cpp:2682. Coverage/shadow classification Material.cpp:2796-2821.
- idDrawVert layout on GPU: rtgi_bounce.glsl:11-15, 109-131 (stride 60 B = 15 uints; xyz@0, st@3, normal@5).
- Texture average: `idImage::averageColor` Image.h:302, ctor default white Image.h:363; `R_ImageAverageColorRGBA`
  Image_load.cpp:583-604.
- Buffer pattern: scene-light buffer `s_rtReflLightBuf` RhiWorld.cpp:5897-5901, address -> RenderParams RhiWorld.cpp:
  7785-7788; `BU_STORAGE` / `BU_STORAGE_DEVICE` RHI.h:31-48.
- RtLight struct + free bits: rt_light_eval.glsl:18 + 21-26 (bits 6-15 free; pad0/pad1 taken).

## References

- merian `scene/nee.slang` LightGrid + `light-cdf.slang` (the emissive-triangle CDF + the grid that consumes it);
  Quake II RTX polygon lights (the precedent this mode follows). docs/rtx-experimental-lighting.md (the E umbrella, D1-D4).

## Implementation (2026-10-19, branch feat/rtx-emissive-table)

**CPU (tr_main.cpp, the "RT emissive triangle table" block before `R_RtResetSceneState`).**
- `R_RtEmissiveStage( mat )`: the material's first additive stage (blend add: src ONE / dst ONE - the same test as
  `R_RtMaterialEmitIndex`) with a resident, non-generated, non-defaulted image; NULL for a GUI material or when that
  stage is a cinematic / render-to-texture (a SCREEN - fill-light territory). Independent of `R_RtSurfCasts`, so the
  pure light-source (additive-only, MC_TRANSLUCENT, noShadows) surfaces ARE gathered.
- `R_RtEmissiveStageColor`: the stage's rgb registers evaluated with the owning entity's shaderParms (a fixture tinted
  via `_color`) at the build view. Non-constant registers (time / table driven = pulsing, flickering) -> the colour is
  this instant's phase, floored at 0.25 luminance so an OFF phase does not drop the emitter, and the entry is flagged
  `RT_EMIT_FLAG_ANIMATED` (E2 reads the live colour at the sampled st; the snapshot only RANKS the emitter = option (a)).
- `R_RtEmissiveBuild`: worldspawn `_areaN` surfaces (world verts) + every DM_STATIC entity model (model verts x
  `R_AxisToModelMatrix`, material SKIN-REMAPPED via `R_RemapShaderBySkin` - a fixture's on/off state is a skin). Per
  triangle: 3 world verts, flux = area x luminance(linearised `averageColor`) x luminance(stage colour) x
  `r_rtEmissiveScale`, glow bindless slot (`idImage::rhiHandle`) + flags, per-vertex st as half2, stage tint RGBA8.
  Degenerate (< 1e-3 area) and black (zero flux) triangles are skipped. Two-sided materials contribute both copies
  (both faces emit; flagged TWOSIDED). Ceiling `RT_EMISSIVE_MAX_TRIS` = 262144 (16 MB); an overflow is reported.
- Buffer: one `BU_STORAGE` (host-visible, uploaded once): header 16 B (uint count, float totalFlux, uint cdfOffset,
  float meanFlux) + count x 64 B entries + count floats of unnormalised running-sum CDF. Address via
  `R_RtEmissiveTableAddress( &n, &total, &mean )` (0 = none).
- Lifecycle `R_RtEmissiveUpdate` (called from `R_RtWorldUpdate` on the live-scene path and after a fresh build): build
  once per map, rebuild on `r_rtEmissiveTable` / `r_rtEmissiveScale` change, freed with the scene teardown and when the
  cvar goes off; `R_RtResetSceneState` drops the handles after a backend teardown. The map-name latch is set even when a
  map has no emitters (no per-frame retry).
- Census (printed once per build): triangle totals (world / entity / entities), material count (register-driven count),
  excluded screen surfaces, total + mean flux, size, ms, and the 12 strongest materials by flux share.

**GPU.** `rt_emissive.glsl` = the shared layout (RtEmissiveTri, header, CDF) + the E2 helpers written ahead so the
layout is locked: `rtEmissiveOpen` (header), `rtEmissiveFetch`, `rtEmissiveSample( u ) -> index + pdf` (CDF binary
search), `rtEmissiveUniformPoint` (Osada), `rtEmissiveST`. `rt_emissive_debug.vert/.frag` = the overlay.

**Debug overlay (E1d).** `RB_RHI_RtEmissiveDebugOverlay` (RhiWorld.cpp, called after the light-grid overlay in
RhiBackend.cpp) draws the table's triangles as real world-space geometry (idDrawVert per table vertex, built lazily
from the CPU copy by `R_RtEmissiveDebugGeometry`) through the view's world->clip matrix, depth-tested BY HAND in the
fragment stage against the sealed scene depth (there is no depth attachment at overlay time; opaque fixtures sit at
exactly the scene depth and pass, additive tubes never wrote depth and pass, walls in front hide). Modes
(`r_rtEmissiveTableDebug`): 1 a stable tint per glow texture (striped where the stage colour is animated), 2 flux vs
the table's mean on a log ramp (blue 1/16, green 1x, red 4x, white 16x+), 3 the EMITTED colour = glow texture at st x
stage tint x scale (what an E2 sample returns; too-bright / wrong-hue emitters here are calibration findings).

**ImGui.** Debugging tab, new group "Experimental Lighting" (the user-named group of E5, started here): the table
toggle, the scale slider, the view combo.

**Test procedure (user; the renderer cannot be launched from the session).** Vulkan, any RT tier (the scene must be
live: `r_rtAllLights`, `r_rtgi`, `dude_rtx 2`...). `r_rtEmissiveTable 1`, then:
1. Console: the `r_rtEmissiveTable:` census lines on load. Report the totals and the top materials for 2-3 maps
   (Mars City, Alpha Labs, Hell) - this decides the granularity / size question (E1a).
2. `r_rtEmissiveTableDebug 1`: every neon tube, light tile, LED panel, lava surface should be painted; no plain wall,
   no GUI monitor / video screen. A pure light fixture that is NOT painted = a gather gap (note its material via
   r_showSurfaceInfo). Striped surfaces = pulsing / flickering stage colours.
3. `r_rtEmissiveTableDebug 2`: the big ceiling tiles / lava pools should be red-white, LEDs blue. Anything white that is
   small and dim in the game = a flux error (probably the averageColor white default slipping through).
4. `r_rtEmissiveTableDebug 3`: the emitted colour - plausibility of hue and the relative brightness across fixtures.
5. `r_rtEmissiveTable 0` and back on: no visible change to the lit picture with the overlay off (bit-identical claim);
   `r_vkGpuTime 2`: `emissive.debug` only appears with the overlay on.
Then E1e (mover refresh) and E2.

## First validation round (user, Mars City, 2026-10-19) and the fixes

Census: 5349 triangles (879 world, 4470 from 226 static entities), 54 materials, 150 screen surfaces excluded, 355 KB,
3 ms. Granularity is a non-issue at this size (per-triangle stays).

1. **The sky took 99.9% of the flux** (`textures/skies/commoutside`, 183 tris, 290M): its additive `cameraCubeMap`
   stage with `texgen skybox` passed the emitter test, and its huge portal quads drew THROUGH the comm-room wall
   (mode 3 showed the stars texture as black with white speckles). FIX: any material with a skybox / wobblesky texgen
   stage is excluded (E4 owns the sky); an additive stage with a cube image or any non-explicit texgen (glass
   `texgen reflect` environment stages: `textures/sfx/chiglass1blue`) is not emission; deformed materials (sprite
   flares: `textures/sfx/defaultflare`) are decoration, excluded.
2. **54/54 materials "register-driven"** - wrong. Doom 3 light materials take their colour from the ENTITY parms
   (`red parm0 / green parm1 / blue parm2`, `colored`), which the engine's constant-register check counts as
   non-constant, so `ConstantRegisters()` is NULL for all of them. FIX: entity parms are constant per surface; only a
   TIME-driven colour is animated, detected by evaluating the colour registers at three times (0, 0.37, 1.71 s) with the
   surface's own parms (world parm0-3 = 1) in a zeroed view.
3. **The overlay drew through walls.** `myGlMultMatrix( modelView, projection )` yields GL -1..1 clip z while the scene
   depth is Vulkan 0..1 (the backend remaps every MVP in `RB_RHI_SpaceMvp`), so `gl_FragCoord.z` was always smaller
   than the stored depth and the hand depth test never discarded. FIX: the overlay takes its MVP from `RB_RHI_SpaceMvp`.
   Note also: overlay triangles have no depth test among THEMSELVES (table order; world first, entities after), so two
   passing emitters that overlap on screen resolve by order - acceptable for a debug view.
4. Mode 3 showed two BLACK rectangles on the pillars at the Combat Prep doors where the game shows lit panels: material
   unidentified yet (the user names it with `r_showSurfaceInfo` next round). Candidates: the glass (`chiglass1blue`,
   now excluded) or a glow stage with a texture matrix the sample ignores (the E2 read must honour the stage's
   constant texture matrix - OPEN for E2).

## Second validation round (user, Mars City, 2026-10-19)

Census after the fixes: 4986 triangles (682 world, 4304 from 211 static entities), 46 materials (2 time-driven), total
flux 39864 (mean 8.0). Ranking: `striplight3` 32.8%, `stetile4sqlight` 20.1% (the ceiling tiles), `mcitya12_2` 11%,
`mcitydoorframeglow` 10.5%, `striplight3_break` 8.8% - the fixtures that light Mars City, as expected. The overlay now
respects walls; the black pillar panels of round 1 were the glass (excluded). Mode 1/2/3 agree on the ceiling tiles
(white, 16x+ flux), the door strips (white with orange ends in mode 3) and the wall strips.

OPEN: the Marine HQ comm terminal (the two big monitors + the console sides) IS in the table (blue + orange in mode
1, low flux in mode 2) but samples BLACK in mode 3. Two changes to pin it down: (a) the census now prints each
material's mode-1 tint (`view1 <hue> #RRGGBB`) so a painted surface can be matched to its material without
r_showSurfaceInfo (which is unusable for the user); (b) a CONSTANT stage texture matrix (scale / translate / rotate on
the glow stage) is now BAKED into the stored per-vertex st (t = 0 phase for a scrolled one; flagged "(texture matrix
baked)" in the census), so a sample reads the texel the forward path draws - the likeliest cause of a black sample
where the game shows a lit screen. If the terminal is still black after this, the remaining suspects are an entity
`_color` of 0 on its parms (then the game would show it black too - it does not) or a glow texture that is genuinely
black where the geometry's st lands (a mis-authored add map).

## Third round (user, Marine HQ, 2026-10-20) - E1 VALIDATED

The census tints resolved the comm terminal: the big "blue screen" is `textures/base_light/sterlightdecal` (azure
#4474B9, 124 tris map-wide) - a nonsolid, alpha-tested LIGHT DECAL laid over the terminal bezel, whose `_add` map is
black except for a few LED spots. Mode 3 shows exactly those spots (the small green and orange dots on the dark
terminal), so the sample is CORRECT; the decal quad merely paints over the monitors in modes 1/2 because the overlay
has no depth between its own triangles. The monitors themselves (the "COMMUNICATIONS" GUIs) are GUI surfaces,
excluded BY DESIGN (D1: GUI screens keep the r_emissiveSurfaces fill lights; their content is a live render, there is
no texture to sample). The user's reading ("the Doom 3 screens GUI") is right.

Two consequences for E2:
- **Sparse emitters.** A decal / panel whose add map is mostly black (sterlightdecal, mcitya12_2, mcityc29) gets a
  uniform point on a large quad, so most samples land on black texels: correct in expectation, but high variance for
  its (small) flux share. Mitigation if it shows as noise: build the triangle's flux from the add map's luminance over
  the triangle's st footprint (not the whole-image average), or sub-divide bright regions; defer until E2 shows it.
- **GUI screens as emitters** (optional, later): the fill-light path already derives a per-screen average colour from
  the GUI render (tr_light.cpp emissiveReq tint); a GUI surface could enter the table with a flat colour entry
  (emit slot 0 = "use tint") instead of a texture. Not needed for E2's first cut.

E1 is validated: gather, exclusions, flux ranking, depth-tested overlay, texture-matrix bake. Next: E1e (mover
transform refresh), then E2.

## E1e - movers (built 2026-10-20, branch feat/rtx-emissive-movers) + the E5 light census

**Movers.** Every entity-sourced run of the table is a `rtEmissiveGroup_t` (entity index, model, table slice, its
model-space vertices, the model matrix it was baked with). Once per frame (`R_RtEmissiveRefreshMovers`, from
`R_RtEmissiveUpdate` while the table is current) every group compares its entity's current matrix with the baked one
(16 floats; a still entity costs that compare) and, on a change, re-bakes its triangles' WORLD positions and
`UpdateBuffer`s that slice in place - and the debug vertices' slice when the overlay geometry exists. Area, hence
flux and the CDF, is invariant under the rigid move, so only positions change. `UpdateBuffer` is a memcpy into the
host-visible mapping, so a frame in flight may read a half-updated slice: one emitter a frame off its true position,
harmless for lighting (the visibility ray traces the real TLAS). An entity whose slot was freed or re-used by another
model keeps its last bake. Skin changes (a fixture switching off) and emitters that appear after load (dropped
props) are NOT tracked - static emitters enter the table at build only.

Test: Mars City, `r_rtEmissiveTable 1`, `r_rtEmissiveTableDebug 1`, open a door whose frame glows
(`mcitydoorframeglow`, 76 tris, on the door models): the painted strips must travel with the door and stay in place
when it stops. A lift / platform with a light fixture likewise.

**Animated intensity (decided): option (a).** The flux is a build-time snapshot (time-driven colours at their t = 0
value, floored at 0.25 luminance); E2 reads the LIVE colour at the sampled st. Only the sampling probability of a
pulsing light is stale, never its brightness.

**E5 light census (`R_RtEmissiveLightCensus`, printed after the build).** The hybrid's double-counting question: a
placed light with an emissive triangle INSIDE its volume is FIXTURED (its surface becomes the light - dropped when the
emitters light the scene, or that fixture is lit twice); a light with no emitter in its volume is AMBIENCE (the fill /
mood washes with no proxy - kept). Point-in-frustum of every triangle centroid against the light's six global planes
(1 unit of slack: a tube flush with its own light's face), frustum bounds as the cheap reject. Parallel and fog /
blend lights are listed as kept. The ambient-material flag is counted alongside as the second signal. The per-light
override (the D2 table mechanism) and the saturation fallback remain E5's; this census is what E5's drop list is
built from.

**Overlay mode 4 - light coverage (added 2026-10-20).** The census also flags every triangle whose centroid lies
inside a placed light's frustum (`RT_EMIT_FLAG_INLIGHT`, bit 19; the table is re-uploaded after the census).
`r_rtEmissiveTableDebug 4` paints an emitter GREEN when it sits inside a placed light's volume (that light is
fixtured and would be dropped) and RED when no placed light covers it (this emitter would be the only light there).
A level that reads mostly green has its fixtures backed by placed lights, as Doom 3 is authored; red emitters are
the surfaces that would start lighting rooms no placed light lights today (decals, glow panels in dark corners).

**E1e validated (user, 2026-10-20):** a swinging ceiling fixture in Marine Command Access keeps its painted emitter
attached through its motion. Mars City's door PANELS carry no glow (the frame glow / door lights are frame parts, so
the door test was a wrong target); the console's "mover re-baked" lines name every moving emitter as it first moves.
Merged to local master. E1 + E1e are complete; E2 next (docs/rtx-experimental-lighting.md, the table as a second
candidate stream through the light grid).

**Overlay labels (added 2026-10-20, fixed the same day).** Every mode writes the material name over each painted
surface: the overlay pass projects one anchor per emissive surface (the mean of its triangle centroids, read from
the live table so a mover's label follows it) into the 2D VIRTUAL 640x480 space - culled behind the camera, off
screen, back-facing (the engine's face normal, (v3-v1) x (v2-v1); the first cut had the sign flipped, which hid the
ceiling tiles and showed far-side walls), beyond 700 units, or OCCLUDED (a CPU `idRenderWorld::Trace` from the eye
to 2 units in front of the anchor, world + static props); nearest 48 kept, same-material duplicates within 14
virtual px dropped. Console.cpp `SCR_DrawEmissiveLabels` draws them (debug TTF font, black halo + white) on the
console's 2D pass - the FPS counter's path - because the ImGui layer composites AFTER the screenshot capture and
its text never reached a screenshot. Prefixes "textures/" and "models/mapobjects/" stripped.

**Decals excluded + the exclusion list (2026-10-20).** The alphabet / marking decals (`textures/decals/*`, the
DECAL_MACRO: `sort decal`) put their artwork on an additive stage so it stays legible in the dark - legibility, not
light - and were painted as emitters (user: the door's alphabet sign). Any `SS_DECAL`-sorted material is now
excluded. `sterlightdecal` (the LED light decal) is NOT sort-decal and stays. The census now ends with every
self-illuminated material the gather REJECTED and why (gui screen / deformed / decal / sky / video screen / cube map
or reflection texgen / glow image not resident / no image), so a lit fixture that is not painted can be looked up by
name - e.g. the tube over the Mars City entrance door, unpainted in the same screenshot, is the first case to read
off that list.

**Conditional stages + late-resident glow images (2026-10-20).** Two-state fixtures (`textures/base_door/doorlight`:
`if ( parm7 == 0 )` red stage / `if ( parm7 == 1 )` green stage; broken-light skins) are gathered through the stage
condition evaluated with the surface's own parms at time 0, so the stage actually drawn is the one in the table and
an OFF state is excluded ("switched off by its condition"). A glow image that is not yet on the GPU at build time
(loaded on its first draw) is remembered; every 90 frames the live path checks the list and rebuilds the table once
one becomes resident. The user's observation that a fixture keeps a small lit bar with the placed lights off is the
engine's own design: the glow stage is unconditional self-illumination, the placed light does the lighting; vanilla
turns a fixture OFF with a skin or a parm7 condition, never by swapping the glow texture.

**Vanilla fixture switching folded in (2026-10-20).** Every tracked entity group remembers the glow STATE it was
baked with - skin, custom material, the 12 shader parms - next to its model matrix. The per-frame refresh compares
them; on a change `R_RtEmissiveRegatherGroup` re-gathers the entity with its current state (skin remap, stage
conditions, tint) into a scratch list: the same triangle count rewrites the slice in place (new glow slot / tint /
flux + positions), a state that glows nowhere (a light shot out: the `_off` skin) keeps the slice at zero flux with
`RT_EMIT_FLAG_DARK` (the CDF never draws it; the overlay discards it) and comes back the same way, and a change in
WHICH surfaces glow queues a full rebuild (rare). Any flux change re-uploads the CDF + header
(`R_RtEmissiveUploadCdf`). Console lines name the entity on OFF / ON. A scripted `_color` fade re-gathers its entity
every frame it changes (one entity's surfaces + a 5k-float CDF: negligible); the placed light the game removes with
the fixture is E5's business (fixtured lights are dropped anyway).

**Freed entities + late arrivals (2026-10-20).** A pickup taken (the game frees its render entity: the armor-shard
`_fx` emitters lingered), an item hidden, or a slot re-used by another model darkens the group's slice (zero flux,
`RT_EMIT_FLAG_DARK`); the same model returning to the slot (a respawn) re-gathers it on through the state compare.
Every 90 frames `R_RtEmissiveNewEntityAppeared` scans the untracked entity slots for a static model with an emissive
surface (a dropped / spawned item) and queues a full rebuild when one exists.

**D5 (user, 2026-10-20) - the visible glow must read as a light.** The "ugly square": a fixture's glow stage drawn
by the forward path as a flat, unlit rectangle (plainest with the placed lights off). Decided: once the emitters
light the scene, an emitter surface's visible glow is drawn at its EMITTED radiance - the same per-material
intensity (D2 table x r_rtEmissiveScale) that the table uses - so it blooms and reads as a source rather than a
pasted square, and the look and the light come from one number. Belongs to E2's apply / E6 calibration (the forward
additive draw of an emitter surface scaled by its intensity); a dark fixture is untouched. Not built yet.
