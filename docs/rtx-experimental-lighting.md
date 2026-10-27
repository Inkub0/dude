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

**E2 v2a built (2026-10-20, branch feat/rtx-emissive-e2).** (1) `R_ImageEmissiveExtentsRGBA` at image load (beside
averageColor, the parallel loader too): `glowExtents` (texels > 8% luminance) and `brightExtents` (> 60%), each the st
rectangle of the bright texels, their mean LINEAR colour, their coverage, `entire`. (2) `R_RtEmissiveEmitTriangle`: an
emissive triangle is clipped per texture repetition to its glow image's extents (Sutherland-Hodgman in st, vertices
mapped back through the barycentrics), fan-triangulated; every entry is a UNIFORM emitter with the bright texels'
mean. The flare-inferred fixtures use the diffuse's brightExtents the same way (a proper small light bar, no texel
read; a diffuse with nothing above 60% is not inferred). Census rows say "(clipped to the glow's extents)". (3)
`rtEmissiveSampleSolidAngle` (Arvo, Q2RTX's sample_projected_triangle) + `rtEmissiveSolidAngle`; `diShadeEmitter`
samples in solid angle: contrib = L x sqrt(cos_e) x N.L per unit solid angle, candidate pdf = flux share x 1/Omega.
No 1/d^2 anywhere. The near-field clamp went with it. Still a distance-blind proposal (v2b = the grid groups).

**Hitching (user, 2026-10-20) - fixed:** `R_RtEmissiveStage` evaluated the material registers BEFORE testing for an
additive stage, so the 90-frame scans ran a register evaluation per surface in the map; and the new-entity scan
flagged any gatherable entity without a group - an entity the build examined and found dark - as "new" every 90
frames, a full rebuild each time. Now: additive pre-check first; the build records the model it examined per entity
slot (`s_rtEmissiveSeenModel`) and the scan only looks at slots whose model changed; scan rebuilds are throttled to
one per 5 s, state-change rebuilds to one per 2 s.

**E2 v2b built (2026-10-20, branch feat/rtx-emissive-e2): the grid as the cluster list.** Emitter GROUPS (one per
table surface run: centroid, bounding radius, flux, triangle range; `R_RtEmissiveUploadGroups`, rebuilt on flux /
mover changes) are streamed by the light-grid fill: every cell keeps an EMISSIVE RECORD - the top 8 groups by
flux / max( d^2, cs^2, r^2 ) - after its light record (`lgStride( slots, slotsE )`, slotsE in the packed word's bits
24-29; `r_rtEmissiveLighting` joins the gfx-cfg signature because it re-sizes the grid). The gather's emissive
candidates come from the cell: a group by stored importance (distance-aware), a triangle inside it by flux (the CDF
restricted to the run, `rtEmissiveSampleInGroup`), a point by solid angle - pdf = q_group x p_tri|group x pdfw; a
cell without a record falls back to the global CDF. The bounce hit draws ONE emitter sample the same way from ITS
cell (rtgi_bounce.glsl, albedo x L x sqrt(cos_e) x N.L / p, one visibility ray, the glow clamp on the sample) - the GI
side of the mode: walls lit only by emitters now bounce; the standalone RTGI pass gates its glow-at-hit term too.

Test (user): round 2 setup (`r_rtEmissiveLighting 1`, dude_rtx 2, light grid on as at the PT tier). Expect the
raindrops gone or much reduced at the same candidate count; `r_rtLightGridDebug 2` still works (the light record is
unchanged); `r_vkGpuTime 2`: lightgrid.fill grows by the group stream (a few hundred groups x 131k cells: expect
~0.3-0.5 ms), restir.di.gather roughly as v2a. If the picture goes dark or the grid overlay shows every cell empty,
the stride change is the suspect (both fill and consume use lgStride( slots, slotsE )).

**E2 v2c built (2026-10-20): the per-pixel light list - the other half of the Q2RTX port.** v2b still sparkled
("frying", and the frame went 15 -> 23 ms). Diagnosis: v2b weighed whole surface RUNS at the cell centre and picked
a triangle inside by flux, blind to the pixel - a long strip light's far end drawn as often as its near end from a
pixel beside the near end. Q2RTX weighs each light POLYGON per pixel by its projected solid angle. Now:
- near cascades (`r_rtEmissiveNearCascades`, 2) store per-cell TRIANGLE lists: the fill walks its top-8 groups'
  triangles and keeps the 16 most important by flux / d^2 at the cell (count word bit 31 = tri record); far cascades
  keep group records. `r_rtEmissiveGridTris 0` = groups everywhere (the A/B).
- the gather weighs a random half of the cell's 16 triangles (8 brute-forced, Q2RTX's partition) by projected solid
  angle x mean luminance (`rtEmissiveSolidAngle`: hemisphere + facing culled), picks proportionally, Arvo-samples;
  pdf = (m / mass) / partitions x pdfw. `r_rtEmissiveClamp` (0 = off): a luminance cap on an emitter winner's
  contribution. `r_rtEmissiveBounce`: the bounce-hit sample on / off (it draws a stored triangle by importance in a
  tri cell, a group then a triangle in a group cell).
- ImGui: the group is now "Emissive Lights" with every knob: Mode (master, intensity, bounce), Sampling (samples,
  triangle lists, cascades, cap), Emitter Table (table, scale, flare inference, hide flares), Debug (views, probe).
Perf (open): the 23 ms of v2b wants an r_vkGpuTime 2 breakdown - lightgrid.fill (the group stream, now + the
triangle walks in the near cascades), restir.di.gather (8 candidates: CDF walks / grid reads / Arvo), the bounce ray.

**E2 v2c perf round (user numbers, EnPro, 2026-10-20).** Mode off 20.8 ms; mode on 32.4 ms: lightgrid.fill 0.40 ->
11.2 ms (the near-cascade triangle walk over whole-surface groups - a reactor cylinder is 837 triangles - times
131k cells re-reading the group list), restir.di.gather 6.1 -> 8.7 ms (8 candidates x the mass pass + Arvo). Fixes:
groups are CHUNKS of <= 16 contiguous triangles (a cell walks <= 8 x 16); the fill streams the chunk list through
SHARED memory in tiles of 64 (one global read per group per workgroup; the kernel no longer returns early - every
thread reaches the barriers); candidates default 4 (Q2RTX takes one per pixel from its weighed list). Inference
tightened: reach 6 units (a fixture's flare sits on it; walls 20 units behind a flare and a barrel's label were
inferred), bright patch < 30% coverage, the log line says why. Census: EnPro's flux = the reactor cylinders (encyl2
42%, encyl1 18%), dim_disk 17% (a 16-triangle static disk - check it in mode 1), whiteblock 11%; 585 placed lights:
365 fixtured / 189 ambience.

**E5 built (2026-10-20, branch feat/rtx-emissive-e2): the fixtured lights are dropped.** The user's finding - the
sparkle sits where a fixture carries BOTH its placed light and its emitter, and intensity 0.4 "kills the frying"
because the emitters then vanish under the still-present placed lights - made E5 the next step. The census records,
per lightDef index, the def's creation serial when the light is FIXTURED; `R_RtEmissiveLightDropped( index, serial )`
is true for exactly those while the mode is on; `RB_RHI_GatherSceneLights` marks such a light DI-covered (the forward
path does not draw it) and leaves it OUT of the scene-light buffer (DI, the bounce, volumetrics and reflections lose
it alike). Kept: ambience lights (no emitter in their volume), parallel suns, fog / blend, lights created after the
build (new serial), the weapon lights. The default intensity is now 1 (the emitters carry the fixtured rooms alone;
tune on a corridor). Overlay mode 5 draws every placed light as an 8-unit sphere: red = fixtured (dropped), green =
ambience, yellow = parallel, blue = fog / blend - the check that the drop list is the one the room needs. OPEN:
cookies / grate patterns go with the dropped light (by design); rooms whose fixture emitter is weak will read darker
than before until the D2 intensity table exists - that is the E6 calibration.

**Round (Marine HQ, 2026-10-20): drop confirmed, cost measured, two bugs found by the raw view.** Console: 211
fixtured placed lights dropped / 28 kept. GPU: fill 0.58 ms (fixed), gather 9.3 (baseline 6.1), denoise 5.5 (4.4),
total 24.3 vs 20.8. Intensity 10 matches the vanilla room's brightness (the Mars City calibration number) but the
raw DI (`r_rtReStirDIDebug 1`) was salt-and-pepper: most emitter samples FAILED. Cause: the Arvo sampler expects
the triangle wound clockwise as seen from P (cross( p1-p0, p2-p0 ) at P, o < 0 - Q2RTX's light polys); our facing
emitters wind the other way, so the routine got a mirrored triangle and its directions missed the emitter (then the
visibility ray hit the ceiling). `rtEmissiveSampleSolidAngle` now re-winds to its convention, returns nUnit pointing
at P and a `front` flag (one-sided emitters seen from the back are rejected by the callers). Second bias: the
bounce-hit emitter sample was capped by the strobe glow clamp (0.15 peak) - at intensity 10 the emitter bounce was
thrown away (rooms dark away from the tiles); the cap is now 8 x clamp x intensity. **r_rtEmissiveBounce works in
BOTH modes** (user request): with the mode off, the table + the grid's emissive record still exist and the bounce hit
samples emitters (the placed lights keep the direct term; the glow-at-hit term stays since no DI stream carries the
direct emitter light); r_rtEmissiveBounce / r_rtEmissiveTable join the gfx-cfg signature (grid re-size).

**Round (Marine HQ, 2026-10-20, after the Arvo fix).** Raw DI is a dense grain field with the real structure (the
sampler works); denoised = a plausible emitter-lit room. Cost split (hybrid tier captures, DI not applied there):
bounce-hit sample ~1.3 ms of gather, ~0.5 ms per candidate, fill 0.5 ms. Mode 5 showed a red sphere on the comm
terminal's monitors: DUDE's GUI fill light classed fixtured (the LED decal in its volume) and dropped - GUI screens
are not emitters, so fill lights are now KEPT (D1's replacement waits for GUI entries). Labels are off in mode 5.
The normal-mode bounce A/B must be done at dude_rtx 2 (at the hybrid tier the DI buffer is debug-only).

**E6 reach (user finding, EnPro, 2026-10-20).** With the bounce on, intensity 10 matches Marine HQ, but EnPro's
hall walls stay dark: a Doom 3 light falls off LINEARLY to an authored radius the mapper set to fill the hall, an area
emitter with the inverse square. Fix, faithful rather than global: the census records, per emissive triangle, the
authored radius of the placed light it sits in (`reach`, float bits in the entry; a point light's lightRadius, a
spot's frustum half-diagonal; the largest when several); past 1/4 of that radius the sample is boosted by
d / (R/4) so the far field goes ~1/d - the hall fixture lights the hall it was authored for, a small panel stays
local. `r_rtEmissiveReach` (1 = the authored radius, 0 = pure physics), `r_rtEmissiveReachDefault` (300 units) for
emitters no light covers. Applied in diShadeEmitter and the bounce-hit sample (`rtEmissiveReachBoost`).
The user also found that MORE CANDIDATES brighten the far field: the estimator is unbiased, but NRD's firefly /
fast-history clamps suppress a sparse bright signal, so a far wall lit by rare hits reads darker than its mean -
more samples densify the signal and the denoiser keeps it. Reach and samples are complementary levers.

## E2 v3 - EMITTERS AS LIGHT ROWS (decided + BUILT 2026-10-20, branch feat/rtx-emissive-rows, user test pending)

User verdict on E2 v2 (the Q2RTX port: second RIS stream, per-cell triangle lists, group records, shared-memory
tiling, bounce NEE, E6 reach boost): "I was under the assumption we would just add emitter triangles and reuse as much
as possible of our optimised architecture. When I said to look at Quake2 RTX it was for the lights, not the whole
machinery." Correct. Every v2 problem (flat glossy floor = no spec re-shade for row-0 winners, downlights dropped,
radius mismatch, fill cost, frying) traces to emitters NOT being rows in the scene-light buffer, which is the one key
the light grid, the reservoir, the SH side channel, the apply's specular re-shade, the gradient and the bounce all use.

### Design
- Each emitter GROUP (one surface run, tr_main.cpp s_rtEmissiveLabels / R_RtEmissiveUploadGroups) becomes ONE row in the
  scene-light buffer (RtLight, 128 B, rt_light_eval.glsl) appended by RB_RHI_GatherSceneLights AFTER the placed lights,
  BEFORE the fog rows, with a new flag DUDE_LIGHT_EMISSIVE (64u). pos = centroid; radius = the reach inherited from the
  replaced placed light (census; default r_rtEmissiveReachDefault); cull = (centroid, boundRadius + reach); color = mean
  radiance x r_rtEmissiveIntensity (flux proxy for the grid's importance = lum x rad^2 / d^2); the 64 B of projS/T/Q/F
  (unused by an emitter) hold firstTri, numTris, the bounding box; pad0 = 0 is NOT usable (the gradient's lightDef->row
  table): give emitter rows a key region pad0 = DI_LIGHTMAP_MAX/2 + group (DI_LIGHTMAP_MAX is 4096; cap groups to 2048)
  so temporal reuse / the gradient find "the same emitter" across the per-frame sort; pad1 = table generation.
- RB_RHI_GatherSceneLights returns TWO counts: n (placed lights, what reflections ssr_rt.frag / volumetrics
  vol_scatter.glsl / rtgi_ray.frag iterate - bit-identical for them) and nRows = n + emitters, consumed by the DI gather
  (u_color.z), the grid fill (pc.nLights) and the combined bounce (bp.nLights). RT_REFL_MAX_LIGHTS 256 -> raise the row
  capacity (buffer + static arrays + DI_LIGHTMAP bounds) to 256 + 2048 emitter rows; CPU view-cull emitter rows like the
  placed lights' off-screen pass (cull sphere within REACH of the view) and sort them by lum x rad^2 / d^2 to the eye
  when over the cap.
- diShadeLight (restir_di_shade.glsl) gets ONE branch for DUDE_LIGHT_EMISSIVE: xi from a hash of (row, pixel, frame),
  pick a triangle inside [firstTri, firstTri + numTris) via rtEmissiveSampleInGroup, Arvo point (rtEmissiveSampleSolidAngle,
  keep), contrib = meanRadiance x scale x intensity x sqrt(cos_e) x N.L / (pIn x pdfw) - i.e. the stochastic estimate of
  the group's irradiance, so the RIS target and the reservoir treat the row exactly like a point light. ldir = the direction
  to the SAMPLED point (visibility ray); the SH direction for an emitter row = the direction to the CENTROID (keeps the apply's
  specular re-shade vector long; built 2026-10-20 as ldirSh in v2, same idea). The gradient (restir_di_gradient.frag) must
  SKIP emissive rows (a fresh xi would read as a lighting change): sameLight = true, gradient 0 for DUDE_LIGHT_EMISSIVE.
- rt_light_eval.glsl dudeLightAtten / dudeLightAttenTex: return 0 for DUDE_LIGHT_EMISSIVE (a consumer that gets an emitter
  row by mistake lights nothing instead of reading the repurposed planes as a frustum).
- The bounce (rtgi_bounce.glsl light loop) handles emitter rows through the same branch (a shared helper
  rtEmissiveShadeRow in rt_emissive.glsl, included after rt_light_eval.glsl) - the bounce NEE block goes.
- Bounce in BOTH modes (user ask): a flag DUDE_LIGHT_BOUNCEONLY (128u) on emitter rows while r_rtEmissiveLighting is 0
  and r_rtEmissiveBounce is 1: diShadeLight returns false for it (the DI gather skips it), the grid fill and the bounce keep it.
- The E5 census stays (fixtured = an emitter within max(64, 0.15 x radius) of the light ORIGIN - the 2026-10-20 fix);
  R_RtEmissiveLightDropped stays. Reach: the inherited radius becomes the row's radius (linear Doom falloff via
  dudeLightAtten's radial term is NOT used - the emitter has its own estimate; keep r_rtEmissiveReach as a multiplier on
  the row's radius for the GRID importance / cull only, and drop rtEmissiveReachBoost).

### Delete (v2 machinery)
restir_di.frag: the whole emissive stream block (emTable/emDraws/emChosen/emXiChosen/emShDir, emMass/emM/emT lists,
pickBE/pickBXi, the emitter branches in the winner shade / boost / cache teach, the r_rtEmissiveClamp cap, bp.em* fields);
restir_di_shade.glsl: diShadeEmitter; light_grid_fill.comp: sGroupA/B, the slotsE block, PC fields groupLo..triMode;
rtgi_light_grid.glsl: slotsE in lgStride/LgParms/lgParmsFromVec4, lgEBase, LG_E_TRIS, lgCountE/lgIsTriRecord/lgSlotGroup/
lgSlotImpE/lgDrawGroup; light_grid_debug.frag + rtgi_ray.frag: the slotsE / bp.em* lines; rtgi_bounce.glsl: the em*
parms + the NEE block; rt_emissive.glsl: RtEmissiveGroup buffer + rtEmissiveReachBoost (keep fetch/sample/Arvo/mean);
RhiWorld.cpp: rtLightGridParms_t slotsE/groupAddr/numGroups, the grid Begin's emWanted block, Fill's pc tail, Pack's
slotsE bits, the DI pack's texGen0S/T/Q block (keep intensity: it rides the row colour now), RB_RHI_RtgiPass's
diffuseModifier[0] gate (keep: glow-at-hit off while emitters are rows); tr_main.cpp: R_RtEmissiveUploadGroups /
s_rtEmissiveGroupBuf / R_RtEmissiveGroupsAddress become a CPU-side group list (centroid, radius, flux, mean radiance,
firstTri, numTris, reach) the gather reads - no GPU group buffer; cvars r_rtEmissiveCandidates, r_rtEmissiveGridTris,
r_rtEmissiveNearCascades, r_rtEmissiveClamp (and their ImGui rows + VulkanBackend signature mixes; r_rtEmissiveLighting,
r_rtEmissiveBounce, r_rtEmissiveTable stay in ComputeGfxCfgSignature).

### Keep in the ImGui "Emissive Lights" group (user: retain the necessary knobs, remove the useless ones)
EMISSIVE MODE (r_rtEmissiveLighting), Emitter Intensity, Emitter Reach (now the row radius multiplier), Reach of uncovered
emitters, Emitters at the GI Bounce, Emissive Triangle Table, Emissive Scale, Infer Fixtures from Flares, Hide Fixture
Flares, the debug overlay mode. Add: "Emitter rows cap" (new r_rtEmissiveMaxRows, default 1024) once the capacity is in.

### Guards (memory guard-vk-device-loss.md)
Every emitter row's firstTri/numTris is bounds-checked against the table count in the shade branch; the row count the
shaders receive is the count WRITTEN this frame; r_rtEmissiveMaxRows joins ComputeGfxCfgSignature if it sizes a buffer.

### Built (2026-10-20) - deviations from the plan above
- Rows sit AFTER the fog rows ([placed n][fog][emitters]): the fog consumers index [n, n + nFog) unchanged; DI, the grid
  fill and the bounce iterate every row and skip fog rows by flag (diShadeLight / the bounce loop test DUDE_LIGHT_FOGVOLUME).
  RB_RHI_GatherSceneLights still RETURNS the placed count (reflections / volumetrics); RB_RHI_SceneRowCount() = every row.
- Capacity: RT_SCENE_MAX_ROWS = 256 + 255 + 1; emitter rows <= r_rtEmissiveMaxRows (255) per view, the strongest at the eye
  (mean lum x intensity x area / d^2) when over the cap. The 512 ceiling is the fp16 side channel (row + 1 + 0.25 x reason).
- The row is SELF-DESCRIBING: projS.w = intensity, projT.xy = table address, projQ.x/y = first triangle / count (bits),
  projQ.z = reach knee. rtEmissiveShadeRow (rt_emissive.glsl) needs nothing but the light buffer - any consumer can shade
  it. dudeLightAtten / dudeLightAttenTex return 0 for DUDE_LIGHT_EMISSIVE rows (no accidental frustum read).
- Reach boost KEPT (user-validated in v2): knee = radius / 4 with radius = inherited reach x r_rtEmissiveReach (0 = pure
  inverse square), plus a soft fade 1.5R..2R to the row's cull sphere. Row colour = mean radiance x intensity x area / R^2
  = the irradiance proxy at the reach, so the fill's lum x R^2 / d^2 ranks an emitter by what it delivers.
- Sample seed = hash( row, receiving point, frame ): the RIS loop's target and the winner's re-shade draw the SAME point
  (the reservoir weight must match the shade); a fresh point each frame for NRD. The gradient pass RETURNS (no evidence)
  when either stored row is emissive; pad0 = 0 keeps emitter rows out of the lightDef -> row map.
- Normal mode + r_rtEmissiveBounce: rows carry DUDE_LIGHT_BOUNCEONLY (128): diShadeLight skips them, the fill ranks them,
  the bounce lights with them. They take grid slots from placed lights in that mode (accepted; a test convenience).
- Deleted: the second RIS stream, diShadeEmitter, the grid's emissive record (slotsE / groups / triangle lists /
  shared-memory tiling), the bounce NEE block, rtEmissiveReachBoost, the GPU group buffer, cvars r_rtEmissiveCandidates /
  GridTris / NearCascades / Clamp (+ their ImGui rows). Added r_rtEmissiveMaxRows (ImGui "Emitter Rows / view").
- Console line (once per mode toggle / table rebuild): "emissive mode: N fixtured placed lights dropped, M placed kept,
  K emitter rows added (of T emitting surfaces; cap C)" - K < T beyond the cap or when a surface's reach cannot touch
  the view.
- Open: the within-run triangle pick is by flux (blind to the pixel: a long strip's far end as often as its near end).
  If sparkle on long tubes returns, weigh the run's triangles by solid angle inside rtEmissiveShadeRow (small runs only).
- POWER MATCH (r_rtEmissivePowerMatch, default 1; built 2026-10-20 after "still those fixtures seem dead"): the census now
  stores the replaced light's colour luminance next to its radius (rtEmissiveTriGPU_t.lightLum); the gather sets the row's
  intensity so the emitter delivers what the light did at d = R/2: L = C R^2 / (16 A) (reach boost 2 at that distance).
  A tiny decal replacing a big light burns bright, a big panel glows softly. Uncovered emitters match a light of their own
  colour at the default reach. The grid proxy = the replaced light's C (same scale as the placed rows). With it on,
  r_rtEmissiveIntensity is a TRIM around 1 (the v2 values of 10-25 are no longer meaningful).
- AUTHORED ORIGIN (built 2026-10-20 after "light at a nearly 90 degree angle makes surfaces darker"): a flush ceiling
  strip lights the wall under it at a grazing angle (receiver cosine ~0.08) where the mapper's placed light sat 20-40
  units out in the corridor (cosine ~0.3+). The census stores each triangle's light ORIGIN; a paired row's pos = that
  origin and projF.x = 1: rtEmissiveShadeRow cosines the receiver against it and the gather's SH direction points at it
  (the forward path's geometry), while the sampled surface point still drives the energy, the solid angle and the
  visibility ray. Uncovered rows keep the sampled point for both (the "centre" direction of v3's first build was wrong
  for long tubes: in the wall's plane, it zeroed the apply's cosine re-resolve).
- ROUND 2 of v3 (2026-10-20, Marine Command Access at intensity 1): the room reads like normal mode again. Console:
  "267 fixtured dropped, 89 placed kept, 255 emitter rows added (of 306; cap 255)"; GPU 1080p: gather 7.85 ms, fill 1.05,
  grad 1.1, denoise 2.9 (19.4 ms frame). Fixes: (1) the row cap is now dynamic, 511 - placed - fog (r_rtEmissiveMaxRows
  default 511); (2) INDICATORS: a surface run under r_rtEmissiveFixtureMinArea (64 units^2) never drops a light and
  never inherits power (computer LEDs / electric-box lamps were as bright as neons); (3) r_rtEmissiveMaxRadiance (64)
  caps the matched radiance. Open: the gather's 7.85 ms (an emitter candidate costs a CDF search + Arvo + a 80 B fetch;
  a cheap target for emitter rows, or fewer slots for them, is the lever if it matters); the orange wall lamp at the
  right of the Access room reads dimmer than normal mode (ask which fixture / check its pairing).
- EFFECTIVE AREA (2026-10-20, "it's an electric box, supposed to be a dim red LED"): the whole-surface geometric area let
  the box pair as a fixture and the match normalised its dim glow up to a neon. The measure is the run's FLUX / scale =
  lit area x glow luminance x stage colour ("effective emitting area"). Census pairing: >= r_rtEmissiveFixtureMinArea (8).
  Inherited power ramps 0 -> 1 from the min to r_rtEmissiveFixtureFullArea (64): Lrow = authored + s x (matched -
  authored). The grid proxy follows the same ramp. DEFERRED: several lit patches on one texture are one rect with low
  coverage (the energy is right via the mean; a connected-components split of the glow map would place the samples).
- BLOCKY DI SHADOWS IN NORMAL MODE (2026-10-20): with the table on and r_rtEmissiveBounce on (both default), the ~300
  bounce-only emitter rows competed with the placed lights for the 16 grid slots per cell, emptied cells of their placed
  lights (the DI skips bounce-only rows) and the DI changed cell to cell - cell-shaped blocks. FIX: the fill gives
  BOUNCEONLY rows a quota of 2 slots (1 at 4-7 slots, 0 below), ranked among their kind, behind the lights.
- BLOCKY SHADOWS IN EMISSIVE MODE (2026-10-20): the same cell-border steps with the emitters as real lights - 300 rows
  reaching 2 radii put far more candidates into every cell than its 16 slots; each cell kept a different top set and
  DROPPED the rest. FIX = ReGIR proper in light_grid_fill.comp: an overflowing cell keeps the strongest slotsL - kS
  deterministically and fills kS = slotsL/4 RESIDUAL slots by importance-sampled draws over the lights it cannot hold
  (fresh each frame, seed = cell + frame), stored with q = kS x imp / sumRest; the DI weighs such a candidate pHat / q,
  the bounce 1/q (capped 8). A cell that fits is bit-identical (q = 1). The per-slot float is now q, not imp.
- CULL AT ONE REACH (2026-10-20, user: "you are reinventing the wheel"): the old emissive surfaces (GUI screens, item
  glow) were plain fill point lights through the normal path - the shape v3 has. The blocks came from COUNT x REACH: a
  2 x reach cull sphere gave every cell several times the candidates of the placed lights. An emitter row now occupies
  the volume of the light it replaced (cull = bound + reach, fade 0.75R..R). The residual slots stay as the net for a
  cell that still overflows (inert otherwise).
- RESIDUAL SLOTS REVERTED (2026-10-20, "actually worse, and those pixels are frying"): a residual draw is per CELL per
  frame, so every pixel of a cell shared the same lucky pick with weight 1/q - bright cell-shaped blocks that changed
  every frame. Dead end; removed. The grid is back to deterministic top-N (per-slot float = imp). What stands against
  overflow: the cull at one reach. If blocks persist, read r_rtLightGridDebug 2 (white = full cells) before anything else.
- ROW REACH = THE LIGHT'S CULL RADIUS (2026-10-20): the inherited reach was lightRadius.Length() = the box DIAGONAL
  (173 for a 100-unit light) where the light itself culls on its frustum sphere (~87): a row claimed 8 x the volume of
  its light, the cells overflowed and rows were evicted at random cells (three identical alcoves: lit / half / dark,
  all three red in mode 5). Now reach = the frustum bounds' radius (a spot's extent, ~0.87 x a point light's half-extent).
  The bounce-only quota is 0 in emissive mode (pc.quotaB). NEXT if blocks persist: r_rtLightGridDebug 2 (white = full).
- CELL LOOKUP JITTER (2026-10-20, user: "our architecture doesn't care about the number of lights"): in COST it does
  not (O(slots) per pixel); in QUALITY a cell with more lights that matter than r_rtLightGridSlots (default 9!) drops the
  dimmest, and a dropped light is a cell-shaped step - the L4 follow-up never landed. RTXDI's answer, now built:
  r_rtLightGridJitter (default 1) jitters every pixel's (and bounce hit's) lookup by up to half a cell, so the steps
  become per-pixel noise NRD averages. r_rtLightGridSlots 16 is the other lever for emissive mode.
- SLOTS ARE THE LIMIT (2026-10-20, user: "Rank by Light Falloff makes those lights visible; lowering candidates/cell makes
  them go away"): the earlier grey grid-debug shot misled me. r_rtLightGridRankLinear stays on. LG_MAX_SLOTS 16 -> 32;
  new r_rtEmissiveGridSlots (16, 1..32) = the grid's slots while emissive mode is on (the larger of it and
  r_rtLightGridSlots, which stays 9 for normal mode); in ComputeGfxCfgSignature. Why more candidates in emissive mode:
  two rows per multi-surface fixture, uncovered emitters at r_rtEmissiveReachDefault (300), a row's cull = surface
  bound + reach. r_rtReStirDIDebug 8 (chosen light per pixel) exists now for the next such question.
- RULE (user, 2026-10-20): "the only thing that seems to help is disabling Rank by Light Falloff" in emissive mode ->
  r_rtLightGridRankLinear is forced OFF while r_rtEmissiveLighting is on (RB_RHI_LightGridFill), the checkbox greyed.
- ROUND 2026-10-20 (late): all three alcoves lit (inverse-square rank, rule in place). Candidates/cell uninfluential 9..32
  (+1 ms at 32) -> r_rtEmissiveGridSlots default 9 (= the grid's; "1" in the user's test was 9, the max of the two).
  Electric box fine at r_rtEmissiveFixtureMinArea 32 (now the default); past the threshold the matched LED lights its own
  housing at point-blank range (the near field of a radiance-64 patch) - indicators must stay below. OPEN: the ceramic
  floor under the info terminal (a GUI fill light, i.e. a placed light): grooves brighter than tile tops - DI bump
  handling, not emitters; needs the hybrid-tier comparison of the same spot before touching restir_di_apply.frag.
- ONE ROW PER FIXTURE (2026-10-20, "those ceiling fixtures are still not shining anything on the floor"): the probe
  showed the panel is WORLD material mcitya12_2 with 288 table triangles in one run = one row (centre mid-room, samples on
  panels behind walls, matched power divided over all panels). R_RtEmissiveSplitWorldRuns: a world run is split into
  spatially connected clusters (boxes touching within 2 units; 16-unit spatial hash + union-find), the run reordered so
  each cluster is contiguous (the GPU samples a contiguous range), one label = one row per cluster. World runs only
  (entity slices keep their gather order for the re-gather). Console: "N world material runs split into separate
  fixtures: F emitters from R runs". Rows per view may now exceed the 511 - placed cap: the strongest at the eye win.
- ACCESS HALL (2026-10-20, new test map): floor-edge 45-degree tubes go DARK when they become fixtures (their placed
  lights dropped, the rows deliver nothing). Built: rows inherit the replaced light's noShadows (flag 4: no visibility
  ray - a tube behind its cover); rtEmissiveProbe now prints the nearest table triangle (emits toward the eye?), its ROW
  (area, effective area vs min/full, mean lum, centroid) and the pairing (reach, lum, origin + on the EMITTING side?,
  noShadows). Next probe on those tubes decides: back-facing winding vs blocked ray vs origin behind the emitter.
- ROW CEILING 4096 (user, 2026-10-20 "widen more up to 4096"): the DI gather MRT's att3 (SH1 + chosen row id) and its
  side copy are RGBA32F now (IF_RGBA32F, pass class 15 for the copy target); RT_EMIT_MAX_ROWS 4096, r_rtEmissiveMaxRows
  default 4096; the per-view pick is a partial sort (std::nth_element) when over the cap. Cost: +8 B/px at gather res.
- 2026-10-21 STATUS: "it all looks reasonably good now". GPU (Access Hall, 1080p): normal 16.35 ms / emissive 16.45 ms;
  gather 7.50 / 7.61, fill 0.63 / 0.63, sidecopy 0.16 / 0.17 - emissive mode is FREE per pixel. r_rtLightGridRankLinear
  default 0 (a diagnosis knob; hurt in emissive mode, untested in normal). OPEN for the next pass: light-level CONTRAST
  between areas is exaggerated in emissive mode. Hypothesis: the match equates the fixture to its light at R/2 only;
  nearer, the solid-angle estimate (inverse square until the knee R/4) is brighter than Doom's linear law, farther it is
  dimmer (fade 0.75R..R vs Doom's linear tail), and uncovered emitters carry the 300-unit default. Levers: the knee
  (R/4 -> R/8 flattens the near field), the match reference distance, r_rtEmissiveReachDefault. Also open: ceramic floor
  grooves vs tops under a placed fill light (hybrid comparison still pending).
- FLOOR UNDER CEILING SPOTS (2026-10-21, "Chosen light" view: the whole floor picked the kiosk fill): a row's reach was
  the light's bounds RADIUS, centred halfway down a spot's cone - half the drop to the floor; the row faded out above the
  tiles. Reach is now the farthest corner of the light's bounds FROM THE EMITTING TRIANGLE (a point light's box: the
  half-diagonal as before; a ceiling spot: the full drop). Mode 8 is the tool for "what lights this surface".

## E2 physical - life-like emitters + the emitter editor (2026-10-21, branch feat/rtx-emissive-physical)
User goal: real-life illumination; the compatibility layer (reach / knee / fade / power match at R/2) only reproduces
the placed lights' non-physical look. Built:
- r_rtEmissivePhysical (0): no reach, no boost, no fade; a row's cull radius = sqrt( L A / r_rtEmissiveCullIrradiance )
  (0.003), the grid proxy 16 L A / d^2 (projQ.w = 1 tells the shade: no fade).
- emitters.cfg (save path): the per-MATERIAL table - "name scale r g b profile twoSided indicator". A material with an
  entry lights at its AUTHORED radiance x scale x tint (no power match) in BOTH modes; profile 0 sqrt(cos) / 1 Lambert
  / 2 uniform rides RT_EMIT flag bits 22-23; indicator = never a row. Commands: rtEmissiveBakeTable (seed every
  emitting material without an entry with the power match's area-weighted multiplier; indicator materials flagged),
  rtEmissiveSaveTable, rtEmissiveReloadTable. R_RtEmissiveMatchedScale is the ONE power-match formula (gather + bake).
- The editor (ImGui "Emissive Lights" > "Emitter Editor"): pick the material under the crosshair (through flares), edit
  radiance x / tint / profile / two-sided / indicator, Apply (live: table rebuild), Remove, Save, Bake defaults, Reload.
- r_rtEmissiveAutoExposure (1): R_HdrEyeAdaptationWanted() = r_hdrEyeAdaptation || (emissive && physical && this) - the
  existing eye adaptation with its own knobs (user's choice over a second mechanism).
Workflow: Emissive Mode on -> Bake defaults -> Physical on -> walk the map with the editor; the probe prints the entry.
- EMITTER EDITOR = STANDALONE WINDOW (user, 2026-10-21): moved out of the Emissive Lights group into its own ImGui
  window like the PBR editor (D3_ImGuiWin_EmitterEditor bit; Com_DrawEmitterEditor / Com_OpenEmitterEditor). Open it
  from the group's "Open Emitter Editor" button, or bind the `editEmitter` command to a key (e.g. `bind m editEmitter`)
  to open it on the material under the crosshair. The group keeps only the launcher + "Bake defaults".
- HANGAR LIGHTS GO BLACK ON EDIT (user, 2026-10-21): flagging a shared light material as INDICATOR (emitters.cfg /
  editor) dropped every placed light of that material (the census marked them fixtured on geometry alone) while the
  indicator rows are kept OUT of the light buffer - no emitter, no placed light. FIX: the census treats an emitter
  flagged RT_EMIT_FLAG_INDICATOR exactly like a sub-area indicator - it never makes its placed light fixtured, so the
  light stays. The low-radiance case already self-corrects (flux -> effArea below the floor -> light kept). LESSON: the
  drop and the "is this a light row" test must use the SAME indicator rule, or a fixture goes black twice over.
- PER-FIXTURE (LIGHT ID) OVERRIDE (user, 2026-10-21: "same fixture used as a floodlight or a small ceiling light"): a
  second table in emitters.cfg, keyed by a stable fixture id = FNV hash of the paired light's origin (or the cluster
  centroid for an uncovered emitter), quantised to 1 unit so it survives reloads. Line: `light <idhex> scale r g b
  profile two ind`. Resolution: the material entry (or power match) is the base; R_RtEmissiveApplyLightOverrides rewrites
  the overridden cluster's TRIANGLES after the census (scale/tint REPLACE the material's, profile/indicator set), so the
  gather and shader see it with no further plumbing. row.fixtureId + row.tableEntry (either source pins the radiance).
  Editor: a "This material (all) / This light (override)" radio; "Pick under crosshair" captures both the material and
  the fixture id (R_RtEmissivePickFixture). Two-sided override is best-effort (back geometry only exists if the material
  is two-sided). Open: the apply pass rebuilds the whole table per edit (fine for a small table).
- OVERRIDE ID = CENTROID, not light origin (user, 2026-10-21: landing bay, lights bundled): the fixture id keyed off the
  paired light's origin collided when several placed lights share one spot - every emitter lit by that bundle got the
  same id, so one override hit them all. Now the id is the emitter surface's own CENTROID (unique per fixture), quantised
  to 0.1 unit (deterministic centroid -> stable key). MIGRATION: any `light <id>` lines saved before this no longer match
  (old = origin+1-unit); re-create those overrides.
- PICK HIT textures/common/collision (user, 2026-10-21): world->Trace prefers COLLISION surfaces (a model with clip
  geometry traces only against them), so the editor pick got the collision brush's material, not the fixture's. FIX:
  the pick (R_RtEmissivePickFixture / PickMaterial) now uses the emitter TABLE at the hit POINT - the nearest emitter
  row within 24 units - and returns that ROW's material (row.mat, new field), walking the ray past collision / flare /
  non-emitter hits (up to 8). mt.material is no longer trusted.
- "LIGHTS LOST / BLINK WHILE EDITING" (user, 2026-10-21, landing bay = bundled fixtures): NOT the per-view cap (~4000).
  It is the LIGHT GRID per-cell top-N: a cell in a dense bundle holds more fixtures than r_rtEmissiveGridSlots, so the
  weak ones are never sampled (missing); editing a fixture changes its grid importance (flux), which evicts a neighbour
  from the kept set (blink). Mitigation: r_rtEmissiveGridSlots default 9 -> 16 (up to 32); smaller r_rtLightGridCellBase
  also helps (fewer lights per cell). PROPER FIX (not done): temporal accumulation of the overflow is unbiased only with
  a 1/f weight that spikes and fries (the reverted residual slots); a biased deterministic rotation (weight 1) is stable
  but under-counts the rotating weak lights - acceptable for eye-tuning, a candidate if 16/32 is not enough.
- "LIGHTS TURN ON AFTER I LOOK AT THEM" (user, 2026-10-21): NOT the importance calc (the grid was not red). Glow
  textures upload LAZILY (first draw), so a fixture not yet seen has rhiHandle 0 and R_RtEmissiveStage skipped it; the
  periodic residency rebuild (every 90 frames) added it ~1.5 s after it was first drawn - the pop-in. FIX: R_RtEmissiveStage
  force-loads every candidate glow texture at build (ActuallyLoadImage(true,false); frontend context; idempotent), so all
  emissive glows are resident from map load (user: VRAM is plentiful, GPU upload is the low-latency path). The load also
  computes the bright extents + averageColor the mean radiance reads. s_rtEmissiveNotResident is now a rare safety net.
- DISTANT LIGHTS SWITCH OFF / BLINK WHILE MOVING (user, 2026-10-21, circling among lights): the per-view emitter cull
  dropped any row whose centroid was > 2*radius + 2048 (~2648) from the CAMERA, so distant fixtures popped on/off as the
  player moved across that line. FIX: the cutoff is now the LIGHT GRID's own half-extent + the row's reach (so a fixture
  the grid can place in a cell is never culled). CONFIG LEVER (the user's hunch): the grid extent = r_rtLightGridCellBase
  x 2^(cascades-1) x res/2 ~ 3072 at defaults; raise r_rtLightGridCellBase (24->48 doubles it to ~6144) or cascades to
  keep more distant lights, at more grid fill cost. The cull is REQUIRED (the grid fill iterates every buffer row per
  cell); beyond the grid a far surface lights exhaustively. If the edge pop is still visible on a huge map, a fade band
  at the boundary is the next step.
- FAR EMITTERS READ DIM vs the placed light (user, 2026-10-21; emissive OFF correct, grid OFF no help, reach no help):
  the emitter is UNDER-SAMPLED. A placed light is a point the reservoir picks reliably; an area emitter spreads its
  energy, so at distance one Arvo sample is rarely the winner and NRD clamps the sparse spikes -> far dimmer. FIX:
  rtEmissiveShadeRow now AVERAGES r_rtEmissiveSamples (default 4) area samples of the emitter's outgoing radiance toward
  P; ndl / ldir / SH stay the centroid's (bump re-resolve still cancels), only the radiance is smoothed. Carried in the
  DI pass's free u_texGen0S.x; the bounce uses 1. A placed light still costs 1 sample (the emitter's own cost). LESSON:
  the far-field area-light problem is sampling variance + the denoiser clamp, not falloff or culling.
  2026-10-27 (user, mars_city1, emissive mode on): `restir.di.gather` 24.7 / 13.6 / 8.4 ms at 10 / 4 / 1 samples (~1.75 ms
  per sample: every emitter CANDIDATE of a pixel's list ran them all, not only the winner), and 10 / 4 / 1 looked the same
  -> the default is 1 (`tune/emissive-samples-default`); a saved value is kept, so set it by hand where one is archived.
