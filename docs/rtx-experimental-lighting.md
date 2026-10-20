# Experimental lighting - emissive surfaces light the scene (assessment + plan, authored 2026-10-16, NOT started)

**The idea (user, 2026-10-16):** in the path-traced tier, disregard the game's placed lights; only emissive surfaces
emit, with real-world behaviour (inverse-square falloff, no authored radius); skies keep the sun; everything gated
under "Experimental Lighting" in the ImGui.

## Assessment

**Precedent.** Quake II RTX and merian-quake do exactly this: polygon lights from emissive textures, a sun / sky
environment, a handful of dynamic sphere lights for weapons. It works there because the content's emissive
textures ARE the light sources - Quake's data has no placed lights at all.

**Doom 3 is only half like that.** Its lighting is authored as placed light entities with projection cookies;
fixture meshes are decoration whose glow textures only roughly coincide with the lights, and many lights have
no fixture at all (fill lights, cheated bounce, coloured mood lights, most of Hell and the caves). Consequences:

1. **Dark areas.** Every area lit by a fixture-less light goes black until its emitter is authored. Expect it to
   look striking where fixtures and screens are dense (Mars City, Admin, Alpha Labs) and wrong in Hell / Caverns /
   Delta until tuned. This is a per-map calibration job, not a switch.
2. **The flashlight** has no emissive proxy and is the game's core mechanic; physically a flashlight IS a small
   emitter with a reflector, i.e. a spot light. It must stay a light, as must muzzle flashes, projectiles,
   explosions, the BFG, monster glows (their effects are additive particles, outside the TLAS, so they cannot
   emit as surfaces).
3. **Intensity.** LDR glow textures carry no absolute brightness: a monitor and a ceiling tube read alike in
   texels and differ by ~100x in reality. A per-material intensity table IS the calibration (merian's Quake plugin
   ships hue-dependent fullbright gains for the same reason).
4. **Indirect dominates.** Small emitters light rooms mostly through bounces, so multi-bounce is mandatory:
   docs/rtx-irradiance-cache.md first.
5. **Many lights, unbounded reach.** Thousands of emissive triangles plus 1/d^2 with no cull radius make the
   per-pixel exhaustive RIS impossible; a world-space light grid (ReGIR-style; merian `nee.slang` LightGrid as the
   BSD-3 reference) is REQUIRED. It was the perf item of the merian review; here it is a necessity.
6. **Exposure.** `r_hdrEyeAdaptation` exists but its relative range is capped; an emissive-only scene spans a far
   wider range (a lit screen next to a black corridor).
7. **RT tier only.** Forward tiers render one scissored volume per light; unbounded reach is not representable
   there. The mode lives in `dude_rtx 2` and nowhere else.

**Recommendation.** Build it as a MODE on top of foundations that improve the normal tier too: the cache (C),
emissive area lights ADDED to the scene (E1-E3: fixtures and screens that today do not light their surroundings
start to), the sky emitter (E4). The experimental toggle then is a small switch over that machinery - "drop the
map's placed lights, physical falloff" - and can be A/B'd room by room. That order means every stage ships
something on its own even if the pure mode never leaves "experimental".

## Decisions (user, 2026-10-16)

- **D1 - which lights survive the switch: as recommended.** First-person weapon lights (flashlight, muzzle:
  `DUDE_LIGHT_VIEWWEAPON`), parallel lights (the suns), and lights that APPEARED after level load (projectiles,
  explosions, fx), classified by the lightDef creation serial the DI gradient already tracks - no game-DLL / ABI
  change. Map-placed idLights are dropped; DUDE's own fill lights (GUI screens, item glow) are dropped, their
  surfaces become real emitters. Sub-toggle `r_xlKeepGameplayLights` for the pure A/B.
- **D2 - emissive intensity: a table + the in-game material editor, exactly as PBR.** Mirror
  `pbr/pbr_materials.cfg` + `pbr/pbr_overrides.cfg` (Material.cpp:140-360): `emissive/emissive_materials.cfg`
  (shipped, bootstrapped by a classifier script over every material with an additive stage) + `emissive/
  emissive_overrides.cfg` (dude folder, later wins). Columns `<material> <intensity> [category] [tint]`, `*` =
  inherit, `@cat <name> <intensity>` rows for the live category presets (fixture, screen, led, lava, fire,
  glow_decal, sky). The editor is a third tab of the existing "PBR Material Editor" window
  (Dhewm3SettingsMenu.cpp ~6470: crosshair pick, live preview, fork-on-edit, Save to overrides, Reload). The
  intensity maps LDR texels through merian's `ldr_to_hdr` curve, then x category x material scale.
- **D3 - kept lights stay as they are.** Authored radius and falloff, cull sphere, cookies: untouched. Only the
  emissive emitters are "physical", which they are by construction (an area emitter has no radius; it falls off
  with distance and the cosine at the emitter). E5 therefore has no falloff switch.
- **D4 - sky: open, "it has to look good".** Two candidates, judged visually on the Mars-surface stretch before
  committing: (a) sky surfaces enter the TLAS under their own instance mask and any bounce / DI ray that reaches
  them returns the skybox radiance (cubemap sample where the material has one, else its average) x
  `r_xlSkyScale`, with the parallel light still the sun disc; (b) parallel light only, plus a flat sky-coloured
  ambient from the skybox average for surfaces that see the sky (cheap, no TLAS change). (a) is the physically
  right one (sky light is area light); (b) is the fallback if (a) reads noisy or flat. Prototype (a) first as a
  debug-only toggle, A/B against (b), decide.

## What the paper changes here (read 2026-10-16)

Alber et al.'s Quake setting IS this mode: "light lists are not available, and emissive surfaces can appear and
disappear anywhere" - lights found by hemisphere sampling, guided by Markov chains, with an irradiance cache. Two
things follow. (1) The guiding (docs/rtx-mcpg-guiding.md) is a direct-light technique in this mode, not only an
indirect one: guided rays discover emitters. (2) The paper's own limitation decides the architecture: "less
effective in scenarios with many light sources ... ReSTIR remains orthogonal" - Doom 3's fixtures, LEDs and screens
are exactly that, so the emissive triangle table + light grid (E1-E3, NEE with RIS) stays the backbone and guiding
is MIS'd on top with shares, as merian's current renderer does (`nee: resampled (RIS)`, `guiding 0.75 / bsdf 0.25 /
nee 0.1`). The cache (C) is the prerequisite of both, as the paper states.

## Stages (after C, the cache)

- **E0 feasibility.** Sky materials are not shadow casters, so `R_RtSurfCasts` leaves them out of the TLAS today
  (tr_main.cpp) - E4 adds them explicitly. Emissive classification exists: `R_RtMaterialEmitIndex` (tr_main.cpp
  ~1906) = the first additive stage with a resident image; GUI screens are dynamic images (0) and stay DUDE fill
  lights. `RtLight` (rt_light_eval.glsl:18) has `pad0/pad1` and flag bits 6-15 free for a new light KIND.
- **E1 emissive triangle table (CPU + GPU).** At `R_RtWorldUpdate` / the entity gathers: for every casting surface
  with an emissive stage, per triangle: area x mean glow luminance (texture average cached per image) x material
  scale = flux; store verts, normal, emit slot, st, flux. Morton-sorted CDF (merian `light-cdf.slang`). World
  static once; dynamic entities (plasma balls, glowing props) rebuilt when they move.
- **E2 area-light candidates in DI and at the bounce hit.** A second candidate stream in `restir_di.frag`: draw a
  triangle proportional to flux (CDF binary search), a uniform point on it, radiance = emit texture at st x scale,
  pdf = d^2 / (area x cos(theta_l)); RIS weight = pHat / pdf; the same one visibility ray. `rtgiTraceBounce`
  draws M = 4 the same way instead of its light loop.
- **E3 light grid.** Camera-centred cascades (3 x 32^3 cells to start), 32 slots per cell x 7 orientation bins,
  a per-frame fill: slot importance = flux x geometry bound (sphere / cone, Conty Estevez-Kulla 2018), feedback
  atomics record measured contribution -> next frame's slot probabilities, an "even share" floor keeps exploration.
  Serves the emissive triangles AND the kept analytic lights, so it replaces exhaustive RIS for the whole tier
  (the normal tier's gather gets cheaper as a side effect).
- **E4 sky / sun.** Sky surfaces into the TLAS under mask 0x04; hit = environment radiance; parallel lights
  unchanged.
- **Cookies (2026-10-20):** the grille patterns under fixtures are the placed lights' projection textures, not
  shadows; a dropped fixtured light takes its cookie with it, and the emitter's visibility ray (E2) must be
  grate-aware (the perforated-caster walk of the RT shadow rays) so the real grille shadows instead.
- **D6 - ONE master switch (user, 2026-10-20):** "Emissive Mode" is gated by a single archived cvar,
  `r_rtEmissiveLighting` (default 0), for a simple opt-in / opt-out from the config or a future menu toggle. It
  implies `r_rtEmissiveTable`, enables E2's consumption (emitters light the scene), folds in `r_rtEmissiveHideFlares`
  and, at E5, the fixtured-light drop. The table / overlays stay usable without it (debug), and 0 = bit-identical.
- **E5 the switch.** `r_xlLighting` (+ `r_xlKeepGameplayLights`, `r_xlEmissiveScale`, `r_xlSkyScale`):
  `RB_RHI_GatherSceneLights` drops the map lights (the Stage D membership already suppresses their forward draws);
  the kept lights are untouched (D3). ImGui group "Experimental Lighting" in the Debugging tab (user-named ImGui),
  disabled below `dude_rtx 2`. Off during cinematics (authored exposure).
- **E6 calibration.** The intensity table per map, eye-adaptation range, per-map notes in this doc.

Order: C -> G (docs/rtx-mcpg-guiding.md, the current tier benefits first) -> E1 -> E2 -> E3 -> E4 (both sky
prototypes, D4 decided) -> E5 -> E6, with G5 after E2. E1-E2 are useful on their own in the normal tier (real
fixture and screen light); E3 is the big one and also the perf item; E5 is small once the rest exists.

## Risks / open

Grid + cache memory (tens of MB each); flicker of a learned grid (feedback memory, even-share floor); emissive
dynamic images (GUIs) excluded from the table; translucent / particle emitters outside the TLAS (hence D1);
thin-wall leaks in the cache; exposure range; the look departs from the authored mood by design - the toggle is
the safety net. Renderer-only: saves, mods and the game DLL are untouched.

## E2 - emitters light the scene, direct-lighting side (built 2026-10-20, branch feat/rtx-emissive-e2, NOT merged)

**What it is.** `r_rtEmissiveLighting 1` (D6, the master; implies the table, hides the fixtures' flares). In the
ReSTIR DI gather (restir_di.frag) the emissive table is a SECOND candidate stream next to the placed lights:
`r_rtEmissiveCandidates` (4) samples per pixel, each a triangle in proportion to its flux (CDF binary search) and a
uniform point on it; the candidate's area-measure pdf is p = (flux / total) / area and its RIS weight pHat / (Me p),
so the Me draws sit beside the enumerated (pdf 1) lights in the one reservoir (only with the exhaustive / grid light
streams; the uniform-M light draw is not mixed). The winner, light or emitter, is full-shaded once and traces ONE
visibility ray - `traceVisibility`, i.e. the grate-aware perforated walk when grates are on (the cookie requirement).
`diShadeEmitter` (restir_di_shade.glsl): radiance = the glow texel at the point (`rtEmissiveTexelToRadiance`: stage
tint, or the diffuse through its threshold for a flare-inferred patch) x material scale x `r_rtEmissiveIntensity`
(4); geometry = cos at the emitter (two-sided when the material is) x N.L / d^2 (d >= 2); the same apply-mode
material response as a placed light (`diMaterialResponse`, factored out of diShadeLight unchanged). The R3a boost's
extra picks can land on emitters too. `u_texGen0S` carries (table address, intensity, candidates).

**Double counting avoided.** While emitters are sampled, the bounce's per-pixel "glow at the hit" term is OFF
(pbrParms.y = 0): a bounce ray landing on an emitter is the same direct emitter->P path the stream now estimates.
Consequences for this first cut: (1) the bounce hit H receives NO emitter light yet (its light loop knows only the
placed lights), so a wall lit purely by emitters bounces nothing until E2's GI side adds emitter NEE at H; (2) the
irradiance cache learns E_dir at P from the emissive winner (teach path handles it), but at H only placed lights.
(3) An emissive winner writes row 0 to the R1 gradient side data: no antilag re-shade for emitter-lit pixels
(emitters are mostly static; fixture switches reach the cache through the state tracking's flux change).

**Units.** Placed lights: colour x Doom's linear falloff (no 1/pi); emitters: L x cos x cos / d^2 per unit area.
No exact equivalence: `r_rtEmissiveIntensity` is the calibration knob until the D2 per-material table.

**Test (user).** dude_rtx 2 (ReSTIR DI + apply on), a lit corridor (Mars City). `r_rtEmissiveLighting 1`:
1. Placed lights untouched (E5 is later), so the picture should only get BRIGHTER near fixtures: ceiling tiles and
   strips now also light the surfaces around them with soft falloff and real shadows (grilles). `r_rtEmissiveCandidates
   0` = the A/B (table on, no emitter light).
2. `r_rtReStirDIDebug 1/2` (raw / denoised DI) to see the emitter light alone in the DI buffer; noise level vs 4/8/16
   candidates; `r_vkGpuTime 2`: restir.di.gather cost delta (expect ~Me x a cheap shade + the same one ray).
3. `r_rtEmissiveIntensity` by eye: a tile should light its surroundings about as much as its placed light did.
4. Flares: gone on emitting fixtures (hidden), present on the rest.
5. Fixture shot out / door light state change: the emitter light follows (console lines).
Known for round 1: emitter-lit walls do not bounce yet; the glow at a bounce hit is off (slightly less "glow bleed").

**E2 round 1 (user, Mars City, 2026-10-20) - three findings, fixed:**
1. *Black door frames.* The flare hide skipped the whole ENTITY; Mars City's door frame models carry the flare
   polygon in the frame mesh. Now only flare SURFACES are skipped (R_AddAmbientDrawsurfs, `R_RtEmissiveIsFlareMaterial`).
2. *Fireflies / "raindrops" = the lighting.* The sample weights were ill-conditioned: pHat / (Me p) with a
   distance-blind flux proposal and a per-point texel read. A sample on a large, mostly dark glow panel (the wall
   panels, the flare-inferred frames with a diffuse-average flux share) close to the receiver was rare and huge.
   Fixes: (a) NEAR-FIELD clamp d^2 >= max( 4, area / pi ) (an area emitter cannot exceed a disc of its size);
   (b) uniform emitters light with their MEAN linear radiance (glow average x stage colour, half floats in the
   80 B entry; no texture fetch) - the texel pattern is variance, not light; the surface's own glow is the forward
   draw; (c) the flare-inferred frames get a NOMINAL flux (0.02 x area x scale) so they are drawn rarely and keep
   the thresholded texel read; (d) default candidates 8. Still distance-blind: E3's grid feed is the proper proposal.
3. Overlay mode 3 now shows the mean radiance flat (what E2 lights with).

## Q2RTX read (2026-10-20, github.com/NVIDIA/Q2RTX, GPLv2; src/refresh/vkpt) - what to port for E2 v2

Q2RTX = polygon lights from emissive textures, no placed lights: the same mode. Its sampler is the cure for round 1:
1. **Lights are built over the EMISSIVE PART of the texture only** (bsp_mesh.c `collect_one_light_poly`): at image
   load the extents (min/max texel) of the pixels with any emission and their mean colour are recorded
   (textures.c; `entire_texture_emissive` when the whole map glows); at mesh build each surface is clipped, per
   texture repetition, against that rectangle in texture space, mapped back to the surface plane and fan-
   triangulated. A wall panel with two LEDs becomes two small bright polygons with the LEDs' mean colour - the
   flux-proportional proposal then matches the real emission and the per-point texel read disappears. This is the
   structural fix for the sparse-emitter spikes (our wall panels, LED decals, the flare-inferred door frames).
2. **Per-pixel importance over a short candidate list, then SOLID-ANGLE sampling** (light_lists.h): the candidate
   list is the BSP cluster's precomputed light list (PVS-based: lights that can reach the cluster), at most 8 brute-
   forced per pixel (partitioned at random when longer); importance m = spherical-triangle area (projected solid
   angle, hemisphere + back-face culled) x luminance (x a Phong lobe share x a learned visibility factor); the pick is
   proportional to m, then the triangle is sampled UNIFORMLY IN SOLID ANGLE (Arvo 1995, `sample_projected_triangle`,
   pdfw = 1 / solid angle). The per-sample value is colour x solid angle x sqrt(cos_e) / (m / mass) ~ mass: no 1/d^2
   blow-up at all - the proposal is proportional to the integrand estimate for every candidate, so the estimate is
   nearly constant. Our round-1 spikes came from a distance-blind flux proposal + area sampling; both go.
3. **Learned visibility** (`pt_light_stats`): per cluster x light x surface orientation, counts of shadowed / unshadowed
   rays from the previous frame scale the importance (floor 0.1) - "Adaptive Shadow Testing" (Ward 1994). Our light
   grid's L4 feedback idea, confirmed in production.
4. Dynamic (sphere / spot) lights are sampled uniformly as a separate stream and MIS-combined with the polygon stream
   by luminance (one shadow ray for whichever wins); sphere irradiance capped (solid angle 0.02) at bounces.

**Port plan (E2 v2):**
- v2a (CPU + shader): emissive extents + bright-texel mean at image load (Image_load.cpp, beside averageColor); the
  table build clips each emissive triangle to the extents per repetition (Sutherland-Hodgman in st, barycentric map
  back) and fan-triangulates - every entry becomes a UNIFORM emitter with the bright mean (the flare-inferred frames
  too: their bar is the extents of the diffuse above the threshold, no texel read any more); Arvo solid-angle
  sampling with importance = spherical area x luminance over the pixel's candidates.
- v2b (the proposal): the light grid feeds emissive candidates per cell - a compact emitter-GROUP array (one per
  table surface run: centroid, radius, flux) streamed by the fill, top-K groups per cell by flux / d^2; per pixel a
  triangle inside each group by flux (the global CDF restricted to the group's contiguous range), importance x
  solid angle x luminance over the K, pick, Arvo. Q2RTX's cluster list with the grid as the cluster.
- v2c: learned visibility per cell x emitter x orientation (L4).
