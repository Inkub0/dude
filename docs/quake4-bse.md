# Quake 4 effects (BSE) on DUDE — phase Q4

Status (2026-10-25): **plan approved; S1 (the effect decl) and S2 (the runtime and sprites)
done and on master; S3 (every drawn particle kind and the trails) built, at its gate.** Effects
play and draw every particle kind; sounds, lights, decals, physics and view effects come with
S4-S6.

Raven's BSE (their effects runtime, linked into the closed Quake 4 engine) plays the 1172
`effects/**/*.fx` decls of the retail paks: muzzle flashes, impacts, explosions, beams, ambient
steam and fire, debris, view shakes. This document is the phase plan of
[quake4-port.md](quake4-port.md) §5.5 / §6 (row Q4) worked out: the grammar as measured, the
runtime design, the stages and their gates, and the provenance of every behaviour.

The goal set by the user: **effects that look like retail's as faithfully as possible**, cheap,
and working with every renderer DUDE has (GL3, Vulkan, the legacy backend; raster and RT tiers).

---

## 1. Clean-room rules and provenance

DUDE's BSE is written from (and only from):
- the **retail files** themselves (grammar and counts below; `tools/quake4/q4_effect_census.py`);
- the SDK's **`bse/BSEInterface.h`** (game repository, first commit `94a1fdc`): the manager
  interface the game calls, view effect and category enums, the retail cvar names, and Raven's
  class names (`rvDeclEffect`, `rvSegmentTemplate`, `rvParticleTemplate`, `rvParticleParms`,
  `rvEnvParms`) — read for interoperability; DUDE's names are its own;
- **OpenQ4's written docs** (prose only):
  `docs/dev/archive/2026-10-18/quake4-bse-research-and-implementation-plan.md`, whose "Retail
  Parity Audit" lists retail behaviours OpenQ4 measured against the 1.4.2 executable. OpenQ4's
  `tools/tests/bse_retail_parity.py` pins OpenQ4's own source text, so it cannot run against
  DUDE; the behaviours it pins are the same ones that prose lists, which serves as our
  acceptance checklist instead (this settles quake4-port.md §5.5's "[verify the test data's
  derivation]");
- id's **FX editor pages** (iddevnet.dhewm3.org/quake4/: `FXEditor`, `Basic_FX_file_structure`,
  `The_Properties_of_a_segment`, `FXEditor_Tab_-_Emitter`, `Notes_about_Envelopes/Tables`,
  `Notes_about_Origins`, `Effects_Performance`);
- **retail observation**: the user's screenshots of retail Quake 4 (Proton) next to DUDE's (§5).

Decompile-derived code (OpenQ4 `src/bse`, idTech4A++ `raven/bse`, Quake4Decompiled) is read
only for a named behaviour none of the above explains; what is learned is re-expressed in DUDE's
own code and the consultation is recorded in the commit message and in the table below.

| Behaviour | Source |
|---|---|
| Grammar, keywords, value counts, retail quirks (§2) | the retail files (census) |
| `count` = particles per second for an emitter; a spawner emits at once; `locked` follows the owner or bone; `density` scales the count by the domain's size; `persist` outlives the effect's stop; `attenuation` is distance level of detail; `detail` 0–1 hides a segment at lower quality | iddevnet `FXEditor_Tab_-_Emitter` |
| The first segment of the list draws on top | iddevnet `Basic_FX_file_structure` |
| No envelope means no end value | iddevnet `Notes_about_Envelopes/Tables` |
| `offset` displaces the particle, `angle` orbits it about its position, `rotate` turns it | iddevnet `FXEditor` tabs (Offset, Orbit, Rotate) |
| A table of one value reads 1.0 (missing tables default to `{ { 0 } }`) | OpenQ4 prose; DUDE's `idDeclTable::TableLookup` already does this |
| Spawn timing (fixed intervals from the segment's clock, each particle at its own birth time, 0.016 s look-ahead), the count/density/cap and `bse_scale` arithmetic, distance attenuation (camera to the effect's bounds, linear between the segment's distances), loops (window re-drawn each loop, the ambient two-loop head start), durations, the particle budget, sampler shapes (direction from the [-1,1] cube, per-axis random sphere radius, cube-face order, cylinder along x with cone taper, spiral turns), envelope evaluation (`count` = passes per life at the rate fixed at birth, `rate` = per second, `offset`), parameter defaults, spawn order and frames (generated normals, acceleration relative to travel, unlocked particles in their birth frame), the friction formula, sprite corners and texture corners, colour packing (byte wrap; additive fades through RGB), draw order within and across segments, the bounds formula | **decompile-derived, consulted 2026-10-25 (S2)**: OpenQ4 `src/bse` (`BSE_Envelopes.cpp`, `BSE_SpawnDomains.cpp`, `BSE_Segment.cpp`, `BSE_SegmentRuntime.cpp`, `BSE_SegmentTemplate.cpp`, `BSE_Effect.cpp`, `BSE_Particle.cpp`, `BSE_ParseParticle2.cpp`, `BSE_Manager.cpp`), which OpenQ4 corrected against the retail 1.4.2 executable in its 2026-10 audit - read for these behaviours only, the grammar, the docs and screenshots cannot fix them; re-expressed in DUDE's own code (`BseEffect.cpp`), nothing copied (decision quake4-port.md §9.2) |
| Particle kinds and trails (§3.2): a line's lengths (sampled first, out to the end origin, turned by a generated normal, flipped, texture repeats per `tiling` units of the start length, re-sampled while the end origin or the effect moves), the line ribbon and `generatedLine`, the strip of linked particles, an oriented quad's angles (and Quake 4's normal-to-angles conversion with its straight-down quirk), a model particle's turn, scale and material, the bolt (count, jitter walk, two-level kinks, forks, defaults, the 16384-vertex surface), motion trail ribbons and burn copies, named trail emission (child frame, interval, budget), the particle velocity formula, the 10000-vertex / 30000-index surfaces, what a missing model draws | **decompile-derived, consulted 2026-10-25 (S3)**: OpenQ4 `src/bse` (`BSE_Particle.cpp`, `BSE_Particle.h`, `BSE_Segment.cpp`, `BSE_SegmentRuntime.cpp`, `BSE_Envelopes.cpp`, `BSE_ParseParticle2.cpp`, `BSE_Effect.cpp`; `idlib/math/Vector.cpp` `ToRadians`, `Radians.cpp`) - read for these behaviours only, as for S2; re-expressed in DUDE's own code (`BseEffect.cpp`, `BseRender.cpp`), nothing copied |
| What a missing material draws, and that it is `{ map _default }` | the retail `materials/gfx.mtr` |
| Trail ribbons and the railgun strip are two-sided (so the ribbon's alternating winding shows both halves) | the retail `materials/effects/particles_shapes.mtr`, `weapons.mtr` |

## 2. The grammar (measured)

The paks hold 1180 `.fx` entries; 8 are replaced by later paks, leaving **1172 effect decls**,
one per file. All of them follow the rules below exactly — which is why the parser can be
strict (§4 S1).

```
effect <name> {
    size <radius>                       every effect; the bounds radius about its origin
    [cutOffDistance <d>]                (documented, no retail use)
    <segment type> ["<name>"] { <segment keywords> [<particle type> { <particle body> }] }
    ...
}
```

**Segments** (count): `spawner` 2525, `emitter` 2151, `sound` 707, `decal` 164, `shake` 81,
`light` 71, `delay` 65, `trail` 29, `effect` 3, `doubleVision` 3 (`tunnel`: documented, no
retail use). Particle blocks appear only in emitter / spawner / trail / decal / light segments,
at most one per segment.

| Segment keyword | Form | Used by (retail) |
|---|---|---|
| `start`, `duration`, `count`, `density`, `attenuation`, `volume`, `freqShift` | `min,max` | timing / population / distance / sound |
| `detail`, `scale`, `particleCap` | one number | quality level / view effects / cap |
| `soundShader`, `effect` | a name | sound / nested effect segments |
| `locked`, `constant`, `attenuateEmitter` (`inverseAttenuateEmitter`, documented) | flag | |

**Particles** (count): `sprite` 2964, `line` 1023, `oriented` 458, `decal` 164, `electricity`
137, `debris` 88, `light` 71, `model` 31, `linked` 4.

| Particle keyword | Form |
|---|---|
| `duration`, `gravity`, `trailTime`, `trailCount` | `min,max` |
| `fork`, `jitterRate`, `tiling` | one number |
| `jitterSize` | three numbers |
| `material`, `trailMaterial`, `model`, `entityDef`, `jitterTable` | a name (quoted or bare) |
| `trailType` | `motion`, `burn`, or the name of a `trail` segment of the same effect |
| `blend add` | the only blend form |
| `persist`, `generatedNormal`, `generatedOriginNormal`, `flipNormal`, `generatedLine`, `shadows`, `specular` | flag |
| `start { }`, `motion { }`, `end { }` | parameter blocks |
| `impact { bounce N  remove N  effect <name>... }`, `timeout { effect <name>... }` | events |

**Parameters** inside `start` / `end`: `position`, `velocity`, `acceleration`, `friction`,
`size`, `length`, `tint`, `fade`, `rotate`, `angle`, `offset`, each `{ <domain> <values>
[modifiers] }`. Inside `motion`: `{ envelope <table> [count …] [offset …] [rate …] }`. Every
retail envelope names a `table` decl (`linear`, `fastinslowout`, … live in
`materials/tables.mtr`); no `end` value appears without a `motion` envelope.

Components per parameter (the file gives exactly this many numbers per point; all 29854 retail
value lists and all 636 envelope lists comply):

| Parameter | Components |
|---|---|
| `fade` | 1 |
| `size` | sprite, oriented, decal 2; line, electricity, linked 1; light, model, debris 3 |
| `rotate` | sprite, decal (and the 2-D kinds) 1; oriented, model, debris 3 |
| everything else | 3 |

Domains (count, values): `point` 11835 (n), `box` 11531 (2n), `line` 4862 (2n), `cylinder`
1159 (6), `sphere` 453 (6), `spiral` 14 (7) — the last three for 3-component parameters
(`position`) only. Modifiers after the values: `relative`, `surface`, `useEndOrigin`,
`linearSpacing`, `cone`, `attenuate` (`inverseAttenuate`, documented). Envelope lists
`count` 317, `offset` 312, `rate` 7, one number per component.

**Retail quirks** (kept as the game shipped):
- `levels/storage/firebotspray.fx` has no `effect` word: the decl manager gives it the
  folder's type, as in retail.
- `q4cmp5/weld_sparks.fx`, `q4cmp5/shaft_sparksd.fx` end with the words `/killat0n loses.`
  after the decl: one decl-manager warning each, the effect itself is fine.
- `levels/tram/waterpipe.fx` names its material `" gfx/effects/fluids_drips/bubble_green"`
  (leading space): no such material, it draws with the default one (to compare in S2/S3).
- Names the retail data lacks (`test_effect_parse.sh` reference report): tables
  `hardflicker`, `faststart_zero_one`, `fastfadein_fadeout` (read as 1.0); effects
  `ambient/drip_ring`, `ambient/slime_splash_green`, `ambient/slime_splash_red`,
  `damage/blooddrip3`, `test/bdube/electricity`, `test/bdube/tunnelvision` (a missing child
  effect plays nothing); materials `gfx/effects/particles_shapes/jk_dirt`, `gfx/test/corona2`,
  `textures/decals/goospread`, `textures/decals/hurt03`; model `models/misc/rock_chunk.ase`;
  sound shader `effects_acid_burn2`; the trail `sparktrail` of `monsters/harvester/hit_walker`.
- `models/misc/rock_chunk.ase` is named by the model particles of `materials/concrete.fx`,
  `materials/gravel.fx` and `materials/rock.fx` (surface impacts). Doom 3's model manager draws a
  missing model as its default box: compare these impacts with retail in S3 before choosing what
  the particle draws.

## 3. Runtime design (approved 2026-10-25)

1. **Code** in `neo/renderer/bse/`; the decl (`idDeclEffect`) lives there too and
   `framework/DeclQuake4.h` includes it, so the game library sees the type as before. The engine
   code is title-agnostic; only the Quake 4 title registers the `effect` decl type, so Doom 3
   never runs it.
2. **One engine render entity per live effect** (bounds = the decl's `size` about the origin and
   end origin, grown as the owner moves), so area links, culling, view suppression and depth hacks
   reuse the entity code. (Refines §5.5's per-area effect list.)
3. **Unlit particles are built per view in frame-temporary memory** and added as draw surfaces
   from a BSE branch in `R_AddModelSurfaces` — the deform-particle / decal pattern, which every
   backend draws. Doom 3's `idRenderModelPrt` path rebuilds a heap snapshot each frame, and on
   Vulkan each snapshot block is its own RHI buffer, created and destroyed every frame. Only 3 of
   the 323 materials effects use are lit, all on model particles (10 effects: grenades in flight,
   rock chunks); an effect with lit segments gets a second render entity whose continuous model
   is a snapshot of those segments (it receives lights, casts no shadow - S3); debris becomes game
   entities (S5).
4. **Time:** spawning and physics advance on game frames (`UpdateEffectDef`); particle state is
   evaluated analytically at each view's time, so effects stay smooth with `com_interpolate`
   (the same curves, sampled more often — no fidelity change).
5. **Requests to the game** (impact / timeout child effects, debris, view effects) are queued
   and delivered at the game's `bse->StartFrame / EndFrame`, never in the middle of a render.
6. **FSR2:** translucent effect surfaces write no motion vectors (as Doom 3's particles) and
   rely on the automatic reactive mask; model particles and debris get per-entity motion
   vectors as entities. (Replaces §5.5's "motion vectors for effect surfaces".)
7. **Ray tracing:** effect geometry never enters a BLAS (`DM_CONTINUOUS`, frame-temporary).
   Effect lights are engine lights, so the RT tiers light with them — checked at S4.
8. **Fidelity:** retail quirks kept (colour bytes wrap, `linearSpacing` extrapolation, the 60 s
   particle clamp, a missing child effect plays nothing, a missing table reads 1.0). DUDE extras
   (soft particles on effects, …) are opt-in toggles.

### 3.1 Runtime semantics (S2, in DUDE's words)

The rules `neo/renderer/bse/BseEffect.cpp` implements; sources in the table of §1.

- **Effect clock.** An effect starts at its owner's start time. Each segment draws its window -
  a start time and a duration from their ranges - when the effect (or a new loop) starts. A
  segment that is not a sound makes the effect last at least to its window's end; a non-looping
  effect also lasts until its particles' longest life is over. A looping effect starts a new loop
  when that time has passed (every segment but a `constant` one draws a new window); one that
  nothing times starts over half a second after the decl's longest play. A looping effect placed
  in the map starts with two loops of history: it is already in full flow when first seen.
- **Spawning.** An emitter spawns at a fixed interval - one over its count, between the slowest
  and fastest count by `bse_scale`, divided by its distance attenuation when it has
  `attenuateEmitter` - from a clock that runs from the effect's start, up to 0.016 s past the
  time serviced and to the end of its window; each particle keeps its own birth time. A spawner
  spawns its count (between the lowest and the scaled highest, truncated) at its start. `density`
  replaces the count by density times the spawn domain's size (the sum of its extents / 100),
  capped by `particleCap`. A segment holds at most a budget of particles (its count over the
  shorter of its window and a particle's life; more when a looping effect's particles outlive a
  loop); a spawn beyond it is dropped. A segment whose `detail` is above `bse_scale` does nothing.
- **A particle's birth.** Velocity, acceleration and friction first. The position domain gives the
  position; with `generatedNormal` / `generatedOriginNormal` it also gives a normal pointing away
  from the domain's centre / the origin, which turns the velocity (authored along +x). `flipNormal`
  reverses the velocity. Acceleration is authored relative to the direction of travel, friction
  relative to the acceleration's. Gravity is the owner's world gravity times the particle's
  `gravity`, in the effect's frame. An unlocked particle stays in the frame the effect had at its
  birth (where the effect was at that instant within the frame); a `locked` one moves with the
  effect. Life: from its range, 0.002-60 s.
- **Values.** Each parameter takes a start value from its start domain and an end value from its
  end domain (`relative`: the end adds the start). Not given: colour, opacity and size start at 1,
  the rest at 0; end values are 0, but size 1. Angles and rotations are in turns. A start domain
  marked `attenuate` scales opacity or size by the distance attenuation. A `motion` envelope moves
  a value from start to end: its table read at `rate * t + offset` per component, `count n` meaning
  n passes over the life (the rate fixed at birth), `rate n` n passes per second; without one the
  value keeps its start.
- **Domains.** Point: its value. Line: between the ends, at random (or, with `linearSpacing`, at
  the spawn fraction - a spawner's i / count, an emitter's birth time, which extrapolates far
  past the line as in retail). Box: per component at random; `surface`: on a face (order -x -y -z
  +x +y +z), or a rectangle's outline. Sphere: a direction from the [-1,1] cube pushed onto the
  sphere, each axis scaled by its own random radius (`surface`: the full radius). Cylinder: along
  x, the cross-section like the sphere's, `cone` narrowing to the lowest x. Spiral: x along the
  box, the cross-section turned once per its 7th value of x. `useEndOrigin`: x runs out to the
  effect's end origin.
- **Motion.** Position at age t: start + velocity t + acceleration t²/2, plus a friction drift
  (exponential: pushes early, pulls back once t²/2 passes the life), plus the `offset` turned by
  the `angle` orbit.
- **Sprites.** Four corners at the position minus and plus the viewer's left and up axes (turned
  by the rotation, scaled by the size), the texture's corners on them in order - a sprite is its
  image turned 45°, `size` its centre-to-corner distance. Vertex colour: the owner's colour
  (shader parms 0-3) times the particle's; an additive (`blend add`) particle fades through its
  colour times the brightness (parm 6) and stays opaque, the others fade through alpha. Channels
  round and wrap past 0-1 (a byte), as retail's do. A fully faded particle still draws.
- **Draw order.** Within a segment, the particle soonest to die first (a complex segment's newest
  first); the segments in file order (the first at the back); effects farthest first.
- **Bounds.** The decl's radius by the retail formula: the largest size, offset and (lines) length
  plus half the furthest a particle's start position, velocity, acceleration with gravity and
  friction take it over eight points of its longest life. The played effect's entity is that box
  about its origin (and end origin) as it moves, plus where its live particles are.
- **Removal.** A particle dies at the end of its life; a `constant` segment's live as long as the
  effect. A stopped effect spawns nothing more; its particles go, but `persist` ones live out
  their lives.

### 3.2 Particle kinds and trails (S3, in DUDE's words)

- **Line.** Its start and end lengths are sampled before anything else; when the start length
  has `useEndOrigin` and the effect has an end origin, x moves out by the distance from the effect
  to it (an unset end length stays nothing). A generated normal turns them as it turns the
  velocity, `flipNormal` reverses them, `relative` adds the start to the end, and an `attenuate`
  start length scales both. With `tiling`, the texture repeats once per `tiling` units of the start
  length (without it, once along the line). While the end origin or the effect moves, the live
  lines of a segment that spans to the end origin take new lengths (sampled again without the
  normal, flip or attenuation). Drawn as a ribbon from the position along the length, as wide as
  the size each side, turned square to the length and the viewer; `generatedLine` lays it along
  the direction of travel (none without motion). An unlocked line's length turns with the frame it
  was born in.
- **Oriented.** A quad like a sprite's on the plane its three rotation angles give (pitch, yaw,
  roll, in turns), plus the angles of its generated normal - Quake 4's own conversion: yaw and
  pitch of the normal, pitch negated, and a normal straight down pitched by 3/4 of a half turn
  rather than 3/2.
- **Electricity.** A bolt from the position along the length (as a line's, with `generatedLine`):
  steps of about 16 units (3-200), each step's point wandering off the line by a jitter that adds
  up along the bolt (`jitterSize`, along / sideways / down) scaled by `jitterTable` at that fraction
  (default `halfsintable`, which pins both ends), each step kinked twice over into nine short
  pieces that zigzag about it. `fork` bolts (up to 16) leave from random points of the bolt towards
  20 units about the middle of the rest of its way, shorter than the bolt. The ribbon is as wide
  as the size, turned square to the bolt's length and the viewer's position; the shape holds for
  `jitterRate` seconds (0: a new one every game tic, 60 a second, as retail draws them at most). A
  segment's bolts stop at 16384 vertices, as retail's surface does.
- **Linked.** Each particle is a pair of points, the size either side of its position along the
  viewer's left axis; the segment's particles make one strip in their order, the texture running
  along it by their spawn fraction (`tiling` times over the strip).
- **Model.** The model's first surface, turned by the three rotation angles (and the birth frame
  of an unlocked particle), scaled along each axis by the size, coloured by the particle's colour,
  drawn with the particle's `material` (a missing model is the default model). A lit material
  draws through the effect's lit entity.
- **Trails.** `trailType motion`: a ribbon (with `trailMaterial`, default
  `gfx/effects/particles_shapes/motionblur`) through the particle's positions back over its trail
  time (or its age), `trailCount` steps, as wide as its size, turned square to its birth velocity
  and the viewer, fading out to the oldest - Quake 4's arithmetic with its quirks (each step draws
  the previous step's position, so the first two coincide). `trailType burn`: `trailCount` copies
  of the particle as it was over its trail time, fainter with age. `trailType <name>`: the
  particle leaves the `trail` segment of that name's particles behind it, one per that segment's
  spawn interval from its birth on, each born where the particle was then with its velocity turned
  to the particle's direction of travel; the trail segment holds as many as the segments leaving it
  need. A one-shot effect still ends at its own duration.
- **Surfaces.** A segment draws at most 10000 vertices and 30000 indexes (motion trails too):
  particles past that are not drawn, as in retail.

## 4. Stages and gates

One branch per stage, merged to master at its gate after the user's verification. The user runs
every in-game check; nothing here launches the game.

| Stage | Scope | Gate |
|---|---|---|
| **S1 Decl** — **BUILT 2026-10-25** | The grammar of §2: `neo/renderer/bse/DeclEffect.{h,cpp}`, strict value counts, references through the decl / model managers, named trails resolved | `tools/quake4/test_effect_parse.sh` (below) |
| S2 Runtime + sprites — **DONE 2026-10-25** (gate passed: user firefights in Quake 4 and Doom 3, ASAN clean on both) | Manager, effect instances behind the render world calls, timing (start, duration, count, spawner bursts, loops, delay segments, looping ambient effects already in full flow), domains and modifiers, envelopes, motion, `locked` / `attenuation` / `detail` / `density` / `particleCap`, sprites, segment draw order, `testEffect` | Offline simulation over every retail emitter (no NaN, population follows the emission rate, particles stay within the decl's `size`); the user compares 10 sprite-only effects covering every domain, modifier and envelope option with retail on GL3 and Vulkan (`moveables/barrel/debris_fly`, `monsters/scientist/obscuring_cloud`, `weapons/nailgun/guide`, `ambient/dustedge_mp`, `monsters/teleport_dropper/blast_exhaust`, `monsters/fatty/ambient_acid`, `steam/mcc_vent_slow`, `smoke/air_defense_hillsa`, `mapobjects/casket_door2_inv`, `levels/dispersal/supportbreak`); an airdefense1 fight; no FSR2 ghosting; ASAN `--q4`; Doom 3 / RoE smoke |
| S3 Particle kinds — **BUILT 2026-10-25**, at its gate | line, oriented, electricity, linked, model; `motion` / `burn` / named trails | Offline geometry test; the user compares `weapons/nailgun/impact_default`, `ambient/acid_react_waste`, `weapons/lightninggun/tube` + `impact`, `weapons/railgun/trail`, `monsters/strogg_flyer/bomb_fly`, `moveables/barrel/explode`, `weapons/nailgun/impact_glass`, `monsters/makron/blaster_fly`, `monsters/gunner/grenade_burst`; lightning gun and railgun in play |
| S4 Lights, sounds, decals | light segments as render lights; sound segments on the effect's emitter (`EffectDefHasSound`, random sample choice); decal segments projected | Rocket light, impact decals, impact sounds varying per shot; a busy fight on `dude_rtx 2` with no device loss; GL3 and Vulkan |
| S5 Physics and children | traces through `Translation`, bounce / remove, impact and timeout effects, nested effect segments, debris through `SpawnClientMoveable`, `entityDef` on other kinds, `persist` | Offline physics test; wall impacts, `wounds/blood`, harvester claw debris, turret deaths, `bse_physics` / `bse_debris`; ASAN |
| S6 View effects | `shake` scaled by distance, `doubleVision`, `tunnel` | Explosions shake by distance, `mapobjects/buzzsaw_chop`, the intro blur |
| S7 Cvars and tools | the retail `bse_*` cvars, rate limits, `bseSweep` + log summary, debug overlay, opt-in extras, performance | quake4-port.md §6's Q4 gate: `bseSweep` clean over all 1172, ~20 effects signed off against retail, OpenQ4's prose checklist, Doom 3 / RoE / LM smoke, ABI 0 breaks |

`testEffect` moves from S7 into S2: every later gate needs it.

**S2 as built.** `neo/renderer/bse/BseEffect.{h,cpp}` (the simulation of §3.1, no renderer)
and `BseRender.cpp` (the engine side). A played effect (`idBseEffect`) is an engine render
entity whose model is a shared marker (`_BSE_EFFECT`, a continuous dynamic model that draws
nothing and so never reaches a BLAS) and whose `callbackData` is the effect; `R_AddModelSurfaces`
hands such entities over and, once the view's other entities are done, `R_AddBseSurfaces` builds
each one's sprites for the view into frame-temporary vertices (`vertexCache.AllocFrameTemp`, the
deform-particle pattern: drawn by every backend, nothing kept past the frame) and adds them as
draw surfaces - effects farthest first, segments in file order. The game's `UpdateEffectDef`
services an effect at its game time (owner state, spawns, loops, deaths); its entities - with the
live particles' bounds - follow once before the next scene of its world renders (`BSE_SyncWorld`
from `RenderScene`, 2026-10-26: taking the bounds every game tic cost 86% of the effects' time,
and a skipped cinematic runs a minute of game tics without rendering one - airdefense1's effects
went from 6.6 to 0.75 ms per tic under ASAN, 3.2 to 0.4 ms at -O0). The first view of a frame
services an effect nobody has updated (a new effect, `testEffect`'s).
Particles are evaluated at the view's time. The Quake 4 render world facade
(`quake4/RenderWorldQ4.cpp`) plays the game's effects through `BSE_PlayEffect` /
`BSE_UpdateEffect` / `BSE_StopEffect` / `BSE_FreeEffect`; `EffectDuration` answers from the
played effect (or the decl's longest play). `EffectDefHasSound` stays false until S4. A freed
render world detaches its effects. The decl gained what the runtime needs (segment roles,
durations, the bounds formula, `_default` for a particle without a material) and `idDeclTable`
inline min/max accessors (ABI-neutral).

Cvars (Quake 4 title only, as `SettingsQ4.cpp`'s; Doom 3's list is unchanged): `bse_enabled`,
`bse_render`, `bse_scale` (archived, 1), `bse_maxParticles` (2048), `bse_debug` (1 logs effects
starting, looping, stopping, ending). `testEffect <effect> [once]` (Quake 4, cheat) plays an effect
80 units in front of the view facing it, where `spawn func_fx` puts one; `testEffect` alone
removes it.

Offline (`tools/quake4/test_bse_sim.sh`, the runtime with the real parsers and the retail
tables, every effect played once and looping at 60 Hz): no value that is not finite, every
non-looping effect ends, no segment over its budget. The bounds formula gives each file's own
`size` exactly for 897 effects at g_gravity 1066 and 959 at 0 (1132 of 1172 under one of the
gravities tried: the editor wrote `size` under the gravity it ran with); particles go past twice
that radius in 10 of 1124 effects (tracers and drips: the formula samples only the first 7/8 of a
life).

**S2 gate (user):**
1. The ten sprite-only effects of the table, each next to retail: in both games
   `spawn func_fx fx effects/<name> loop 1` from the same `setviewpos` (or `testEffect` in DUDE),
   GL3 and Vulkan.
2. An `airdefense1` fight: the sprite parts of the weapon and impact effects show where they
   should (lines, lights, sounds, decals are later stages), no hitch.
3. FSR2 on: no ghost trails behind moving sprites.
4. `tools/run-asan.sh --q4` for a few minutes of play: no report.
5. Doom 3 / RoE smoke (the renderer hook and `DeclTable.h` are shared).

**S2 gate result (2026-10-25, user):** Quake 4 and Doom 3 firefights fine; `tools/run-asan.sh`
runs of both titles with S2 left no report; the Quake 4 log's warnings are the known retail data
quirks only. Merged to master (`ce6be7d9`). The side-by-side retail comparisons carry over to the
S3 gate, where the effects that mix sprites with lines, oriented quads and bolts become complete.

**S3 as built.** `BseEffect.cpp` builds every drawn kind (§3.2) into a growable geometry
(`idBseGeometry`) for a viewer in the effect's frame - `BuildSegment` (the particles, with burn
copies) and `BuildMotionTrails` - and `BseRender.cpp` copies each segment's into frame-temporary
memory as one draw surface, then its motion trails as another with the trail material. Spawning
gained the line lengths, the oriented angles, trail times and counts, a seed per bolt (its shape
is a hash of the seed and the jitter period, so every view of a frame draws the same bolt), and
named trails (emitted as their particles are retired or kept each service; budgets added once
every segment's is known). An effect with lit segments plays a second render entity
(`_BSE_EFFECT_LIT`, a continuous model, `noShadow`) whose snapshot holds those segments, rebuilt
each frame like Doom 3's particle models; ray tracing never takes it (it gathers static and
skinned models only). The entity bounds now grow by lengths, model reach, bolt forks and jitter,
and where trails were. The decl adds the electricity defaults (`jitterSize` 3,7,7,
`halfsintable`; a missing jitter table falls back to it) and the default motion trail material.

Offline (`tools/quake4/test_bse_sim.sh`, now also building every segment's geometry ten times a
second for a viewer, model particles with a stand-in cube): **0 bad geometry** (vertices not
finite, indexes outside, broken triangles), no value that is not finite, every one-shot effect
ends, no segment over its budget, **all 29 named trail segments receive particles**. Most
vertices one segment took: sprites 8192 (`monsters/harvester/explode_final`), oriented 8192,
lines 2040, bolts 16384 (the berserker's `electric_shield`, at retail's cap), models 9984 (the
cube; at the 10000 cap), linked 500; motion trail ribbons up to 2448. `test_effect_parse.sh` still
passes (1172 / 0 failed, 0 mismatches). Fidelity: what the reference showed is kept, quirks
included; one choice: a bolt with `jitterRate 0` takes a new shape every 1/60 s (retail's
fastest) rather than every rendered frame, so it does not flicker faster at high frame rates.

**S3 gate (user):**
1. Each next to retail, `testEffect effects/<name>` in DUDE and
   `spawn func_fx fx effects/<name> loop 1` in retail from the same `setviewpos` (a beam:
   `testEffect` ends it 256 units to the viewer's right and prints that point; in retail add
   `endOrigin "x y z"` with it), GL3 and Vulkan: `weapons/nailgun/impact_default`, `ambient/acid_react_waste`,
   `weapons/lightninggun/tube` + `weapons/lightninggun/impact`, `weapons/railgun/trail`,
   `monsters/strogg_flyer/bomb_fly` (the grenade model, lit), `moveables/barrel/explode`,
   `weapons/nailgun/impact_glass`, `monsters/makron/blaster_fly`,
   `monsters/gunner/grenade_burst`; and S2's ten sprite-only effects.
2. In play: the lightning gun (bolts, forks, jitter), the railgun (spiral strip, trail), a gunner's
   grenades (lit model, smoke trail), concrete / rock impacts (the rock chunks: retail shows what
   for the missing `rock_chunk.ase`?).
3. FSR2 on: no ghosting behind bolts and trails.
4. `tools/run-asan.sh --q4` for a few minutes of play: no report.
5. Doom 3 / RoE smoke.

**S1 as built.** `idDeclEffect` parses into DUDE's templates (`idEffectSegment`,
`idEffectParticle`, `effectParm_t` = start domain + envelope + end domain per parameter). A
keyword the grammar does not know, a value list of the wrong length, a domain the parameter
cannot take, a second particle block, a key given twice or a block left open fails the decl
(warning with file and line; the decl becomes the default effect, which plays nothing, and
`lintDecls effect` lists it). Of the keywords retail never uses only those with an unambiguous
form are accepted (`cutOffDistance`, `tunnel`, the two inverse-attenuation flags). Tables,
materials, sound shaders and effects resolve through the decl manager at parse time (precached
with the level, as a game's `fx_` keys do), models through the model manager; `entityDef` stays a
name for the game (S5). `framework/DeclQuake4` no longer holds the old brace-only check.

**S1 gate result (2026-10-25):** `tools/quake4/test_effect_parse.sh` compiles the parser's own
source with the real `idLexer` and parses all 1172 decls: **0 failures, 0 parse warnings** (the 2
decl-manager warnings are the `q4cmp5` quirk); 29 control effects behave as designed (28 broken
ones fail, one valid one stores every value checked); **all 110 keyword counts and the number
counts and sums** (31868 statement values, 105250 domain values, 1063 envelope values) equal an
independent census of the text. Builds: Linux, the Windows cross-build, `q4base.so` against the
new header; `check_dhewm3_abi.py` 0 breaks. In the engine (user, 2026-10-25): `lintDecls effect` -
**1157 checked, 0 failed**. The other 15 are the multiplayer map pack's (`q4cmp_pak001.pk4` holds an
`addon.conf`, so the file system, like retail's, leaves it out of the search path until one of its
maps loads); the offline test covers them too. The one warning, `Couldn't load model:
'models/misc/rock_chunk.ASE'`, is the missing retail model of the reference report.

## 5. Comparing with retail

The game's own console command works the same in retail Quake 4 and in DUDE with `q4base.so`:

```
spawn func_fx fx effects/weapons/rocketlauncher/explosion start_on 1
spawn func_fx fx effects/steam/mcc_vent_slow loop 1
```

It places an `rvEffect` 80 units in front of the player at the player's yaw (the second
`func_fx` def in `def/func.def`; the first is commented out). Taken from the same `setviewpos`
in both games, the screenshots compare directly. `func_fx` also takes `endOrigin` (beams) and
`_alpha` / `_brightness`.

## 6. Follow-ups

- After the BSE particle path is done: reuse it to fix Doom 3's particle buffer churn on Vulkan
  (`idRenderModelPrt` snapshots → frame-temporary memory), as the user asked on 2026-10-25.
