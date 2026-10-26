# Quake 4 effects (BSE) on DUDE — phase Q4

Status (2026-10-26): **plan approved; S1 (the effect decl), S2 (the runtime and sprites), S3
(every drawn particle kind and the trails), S4 (lights, sounds, decals) and S5 (particle physics,
impacts, debris, nested effects) and S6 (view effects) done and on master; S7 (cvars and
tools, the Quake 4 phase gate) built on `feat/quake4-bse-cvars-tools`, its gate pending.** Effects draw every particle kind, light their
surroundings, play their sounds, leave their decals, collide with the world, throw debris, start
their child effects and shake or blur the player's view. Visual checks
go through the user's Quake 4 Effect Browser (a claude.ai page listing all 1172 effects, what
DUDE draws of each, the command that plays it, and a saved "Checked" tick per effect).

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
| That effects trace (the game's `Translation`, "small portion of physics required for the effects system"), spawn debris (`SpawnClientMoveable`; a lifetime of 0 takes the entity def's `duration`), rate-limit impact particles (`EC_IMPACT_PARTICLES`), read the owner's connected-area flag (the game sets `renderEffect_t::inConnectedArea` every frame), and the `bse_physics` / `bse_debris` cvars | the SDK (`bse/BSEInterface.h`, `game/Game_local.cpp`, `game/client/ClientEffect.cpp`, `renderer/RenderWorld.h`; Xanz/Quake4SDK = `~/quake4-refs/q4sdk`) |
| Debris hands off to a client moveable at birth; model particles trace as points; bounces come to rest below a speed limit; a missing child effect plays nothing; child, impact and timeout effects play only while the owner is in a connected area, nested ones without the end origin; debris entity defs precache their media | OpenQ4 prose (parity audit) |
| Physics (S5, §3.4): the trace over the last 0.1 s of motion from 0.1 s after the birth or bounce, against solids and render-model clip; one random impact effect at the hit point along the normal, behind the rate limit; the bounce (mirror, scale, motion restarts from the hit point, envelopes keep their clock; rest below 50 units/s off a floor facing against gravity by more than 45°); `remove`; the timeout effect at the death point along the direction of travel; debris placement (the effect's original frame, the velocity in the current one), spin (the end rotation, start not added) and lifetime 0; the nested segment's one child per window at the current origin and axis; `entityDef` on the other kinds only precached; the cvar defaults (1) | **decompile-derived, consulted 2026-10-26 (S5)**: OpenQ4 `src/bse` (`BSE_Particle.cpp` `RunPhysics` / `Bounce` / `CheckTimeoutEffect` / `rvDebrisParticle::FinishSpawn` / `EvaluatePosition` / `EvaluateVelocity`, `BSE_Particle.h`, `BSE_SegmentRuntime.cpp` `PlayEffect` / `UpdateGenericParticles`, `BSE_Segment.cpp`, `BSE_ParseParticle2.cpp` `ParseImpact` / `entityDef`, `BSE_Manager.cpp`) **cross-checked against idTech4A++ `raven/bse`** (`BSE_Particle.cpp`, `BSE_Segment.cpp`, `BSE_ParseParticle2.cpp`) - read for these behaviours only, re-expressed in DUDE's own code (`BseEffect.cpp`, `BseRender.cpp`), nothing copied. Both trees descend from the same recreation (Justin Marshall's Quake 4 BSE), so they are not independent: where they agree DUDE follows them; where they differ the user decides (below) |
| View effects (S6): that the game shows them (`StartViewEffect( type, time, scale )`; the player view's `SetShakeParms` / `SetDoubleVisionParms` take `time` as the finish time in seconds of game time, `SetTunnelParms` as a length; the game ignores shakes in multiplayer) | the SDK (`game/Game_local.cpp` `StartViewEffect`, `game/PlayerView.h`) |
| View effects (S6): one request per window; the finish time = the segment's start + its duration × the distance attenuation of §3.1 (player to the effect's bounds); the strength = the segment's `scale` × the owner's attenuation, by the same ramp over the player's distance to the effect's origin; `scale` defaults to 1 | **decompile-derived, consulted 2026-10-26 (S6)**: OpenQ4 `src/bse` (`BSE_Segment.cpp` `rvSegment::Check`, `AttenuateDuration`; `BSE_Effect.cpp` `GetAttenuation`, `GetOriginAttenuation`; `BSE_SegmentTemplate.cpp` `Init`) and idTech4A++ `raven/bse` (`BSE_Segment.cpp`, `BSE_Effect.cpp`, `BSE_Manager.cpp`, `BSE_SegmentTemplate.*`) - the two agree on all of it; re-expressed in DUDE's own code, nothing copied |
| The retail cvars - names, defaults (`bse_maxParticles` 2048, `bse_rateLimit` 3.0) and help texts - and the `bseLog` / `bseStats` commands with their help texts and output lines; `bse_debug`'s levels Off / On / Extra; `bse_rateLimit` 3 "showing all shotgun pellet impacts" (patch 1.4) | the retail executable's strings (`Quake4.exe`, read with `strings`: data, not code) and `CHANGES_14.txt` |
| That the game charges `EC_IMPACT` per hitscan impact (`bse->Filtered` / `CanPlayRateLimited`) and never calls `UpdateRateTimes` | the SDK (`game/Entity.cpp`, `game/Game_local.cpp`, `game/Game_network.cpp`) |
| Rate limits (S7): per category a budget; an impact or particle impact costs 0.1 × `bse_rateCost` (the rest free); once more than half of `bse_rateLimit` is spent, an effect plays only while cost + spent stays under a random fraction of the limit; a limit of 0.1 or less is no limit; the budgets pay back 0.1 per `StartFrame` (a game tic); `bse_singleEffect` filters by name in `PlayEffect` as well as in the game's `Filtered` | **decompile-derived, consulted 2026-10-26 (S7)**: OpenQ4 `src/bse/BSE_Manager.cpp` (`Filtered`, `CanPlayRateLimited`, `UpdateRateTimes`, `effectCosts`, `PlayEffect`) and idTech4A++ `raven/bse/BSE_Manager.cpp` (the same functions). They differ: OpenQ4 replaced retail's per-`StartFrame` payback with a wall-clock one (25/s, limit 6, deterministic refusal) because multiplayer clients never reach `StartFrame` and their buckets filled for good; idTech4A++ keeps the per-call payback and the random refusal but never fills its cost table. **The user chose (2026-10-26) retail's arithmetic with idTech4A++'s random refusal and OpenQ4's costs**, the payback counted in elapsed 60 Hz game tics (identical in single player, and right for a multiplayer client later). Also differing, chosen with it: no limit at 0.1 or less (idTech4A++; OpenQ4 0), `bse_singleEffect` matched ignoring case (OpenQ4; idTech4A++ case-sensitive) |
| Where the two references differ (S5), and the user's choice (2026-10-26): the trace contents - OpenQ4 `SOLID \| RENDERMODEL` ("the game's shot mask", listed among its corrections against the retail 1.4.2 executable), idTech4A++ `SOLID \| OPAQUE`; when a bounce comes to rest - OpenQ4 below speed² 2500 off a floor, idTech4A++ whenever the unnormalised velocity·normal is under -0.707 (every hit into a surface: no hop would survive, and its `BSE_BOUNCE_LIMIT` goes unused). **The user chose OpenQ4's for both**, to be confirmed against retail at the S5 gate (sparks hopping before they settle; blood landing on a corpse). Also differing, with no user call needed: the bounce frame (idTech4A++ and OpenQ4's own changelog: the particle's birth frame; OpenQ4's current code: the effect's current frame - DUDE keeps the birth frame, identical for effects that do not turn) | user decision, recorded per the rule that reference discrepancies go to the user |

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
  `materials/gravel.fx` and `materials/rock.fx`. Doom 3's model manager draws a missing model as
  its default box (retail's falls back to its `_default` model). **Retail never plays these three
  effects** (checked 2026-10-26: no map, entity def, script, material, effect or game source
  names them; the weapons' concrete and rock impacts are their own `impact_concrete` /
  `impact_rock` effects), so no player sees what draws there; only `testEffect` shows it.
- `explosions/surface/turret_exp` throws four debris entities (`debris_mturret_base`, `_sensor`,
  `_barrel1`, `_barrel2`) whose models, `models/mapobjects/strogg/machines/turrets/debris/*.lwo`,
  are not in the paks (the other 55 debris entity defs the effects name all have theirs). They fly
  as the engine's default model, a dark box - **as in retail**: the user spawned it there
  (`spawn func_fx`, 2026-10-26) and retail throws the same dark boxes. Retail never plays this
  effect on its own (no map, entity def, script, effect or game source names it), so only
  `testEffect` shows it.

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

### 3.3 Lights, sounds and decals (S4, in DUDE's words)

- **Lights.** A light segment makes one light per window: a particle born at the segment's start
  (a live one is not replaced). While it lives it is a point light of the effect's world: where
  the particle is, its size the radius (at least 1), oriented with the effect, its material the
  particle's (none given: the engine's default light). Its colour is the particle's colour as it
  is: retail applies neither the particle's fade nor the owner's colour or brightness, and leaves
  parm 3 at 0. Shadows and specular only with `shadows` / `specular`. A `constant` segment's light
  lives as long as the effect. DUDE updates effect lights before each scene, evaluated at the
  scene's time, so they move smoothly between game tics.
- **Sounds.** A sound segment plays its shader on the effect's emitter (the game allocates one when
  `EffectDefHasSound` says the decl has sound segments): on channel `SCHANNEL_ONE` + the segment's
  index, a random sample choice, the volume and pitch picked from `volume` / `freqShift` (no
  `volume`: the shader's own; no `freqShift`: 1), the emitter moved to the effect on every service.
  A one-shot plays again each loop; a looping shader starts once and stops with the effect. Without
  `duration`, the segment lasts as long as its sound (1 s when not known), which keeps a one-shot
  effect - and the emitter the game frees with it - alive until its sound ends; a looping sound
  does not.
- **Decals.** A decal segment projects one decal at its start: a square of the particle's size
  (half width), turned by its rotation, onto what lies within 8 units behind the effect along its
  x axis (impacts play along the surface normal), with the particle's material.

### 3.4 Physics, impacts, debris, nested effects (S5, in DUDE's words)

- **Physics.** A particle with an `impact` block collides with the game's world when `bse_physics`
  is on (1): each service traces its position 0.1 s ago (or at its birth or last bounce, if later)
  to its position now, as a point, through the game's `Translation` against solids and models
  that clip by their render model. Until 0.1 s have passed since the particle moved off it is not
  traced, and at 60 Hz the first trace starts a frame after the motion did - a fast particle
  can cross a wall in that slice and go on through it, as in retail. A particle at rest no longer
  traces.
- **A hit.** One of the particle's impact effects, picked at random, plays at the hit point facing
  out of the surface - behind the game's rate limit for impact particles, and only while the
  owner is in an area connected to the player's. A non-zero `bounce` then mirrors the particle's
  velocity in the surface and scales it; the motion starts again from the hit point, while its
  colour, size and other envelopes keep their clock. Slower than 50 units a second off a floor (a
  surface facing against gravity by more than 45°), the particle comes to rest there. `remove 1`
  ends the particle at the hit.
- **Timeout.** A particle that dies of age plays one of its timeout effects where it is, facing
  its direction of travel, while the owner is in a connected area.
- **Debris.** A debris particle becomes a game entity (a client moveable of its `entityDef`) at
  birth when `bse_debris` is on (1), and takes no particle slot: placed as an unlocked particle is
  born (locked or not) but in the effect's original frame, moving with its velocity in the
  effect's current frame, spinning by its end rotation in radians per second, living as long as
  the entity def says. `entityDef` on any other particle only precaches that entity def's media
  with the level.
- **Nested effects.** An `effect` segment plays its effect once per window, at the effect's origin
  and axis now, not looping and without the end origin, while the owner is in a connected area.
- **To the game.** Child effects and debris are requests, delivered at the game's next effect frame
  bracket (`bse->StartFrame` / `EndFrame`), at most 256 effects and 128 debris a bracket (the
  most one retail effect asks for in one game frame: 100 and 19). A missing child effect plays
  nothing.
- **Persist.** Particles that persist live out their lives after the effect stops (S2), bouncing,
  colliding and timing out as before.

### 3.5 View effects (S6, in DUDE's words)

- A `shake`, `doubleVision` or `tunnel` segment asks the game for that view effect once per window,
  at its start: lasting the segment's duration shortened by the player's distance to the effect
  (the §3.1 ramp over the distance to its bounds), as strong as its `scale` (1 when not given)
  shortened by the same ramp over the distance to its origin, both times the owner's attenuation.
  The game's player view keeps the last one asked for; the game shows no shake in multiplayer.
- Retail's view effects: 81 shakes, 3 double visions, no tunnel. Most shakes ramp from 200 to 600
  units; the buzzsaw effects' (`attenuation 1,1`) act only within two units of their origin,
  where the game plays them on the player; `cinematics/intro/blur` and `levels/core/doors` have no
  distances and act anywhere.
- Requests reach the game at its effect frame bracket like child effects (§3.4), at most 64 a
  bracket.

### 3.6 Cvars and tools (S7, in DUDE's words)

- **Cvars** (Quake 4 only; help texts retail's): `bse_enabled`, `bse_render`, `bse_debug` (1: plays,
  loops, stops, frees in retail's words; 2: also, once a second, the effects, particles, traces,
  requests and rate-limited effects), `bse_showBounds` (1: each effect's bounds, 2: and its live
  particles'), `bse_physics`, `bse_debris`, `bse_scale` (archived), `bse_singleEffect` (only effects
  whose name contains it play - checked when any effect plays and in the game's filter),
  `bse_maxParticles` 2048, `bse_rateLimit` 3, `bse_rateCost` 1. DUDE's own: `bse_softParticles`
  (0; effect sprites and oriented quads fade where they meet the scene, over the segment's average
  particle size, through the soft particle path Doom 3's particles use).
- **Rate limits**: hitscan impacts (the game asks) and particle impact effects (BSE physics) each
  spend from their own budget; the arithmetic is in §1's S7 row. In single player the budget pays
  back every game tic, so one impact a tic is free; a burst plays its first ~15 impacts for
  certain, the next ones ever more rarely, none past ~30.
- **Particle lives** are clamped to 60 s, retail's (no retail particle asks for more).
- **bseLog**: every effect played since the game started, with its plays and loops, and how many of
  the registered effects that is. **bseStats [all]**: the loaded effect decls' segment counts, how
  many were never referenced, segments per effect, the share of segments with particles, and the
  most particles one play of such a segment has alive (DUDE's reading of retail's lines); `all`
  parses every effect first.
- **bseSweep [filter] [quit] | stop** (DUDE): plays every effect (or those whose name contains the
  filter), twelve at a time in a 4 x 3 grid in front of the view, each once and for at most 2.5 s;
  after each batch it lists the effects that played while the engine gave warnings (their text is
  just above in the log), and at the end the effects that did not play or parse and the most
  particles one effect had alive. `quit` quits when done.

## 4. Stages and gates

One branch per stage, merged to master at its gate after the user's verification. The user runs
every in-game check; nothing here launches the game.

| Stage | Scope | Gate |
|---|---|---|
| **S1 Decl** — **BUILT 2026-10-25** | The grammar of §2: `neo/renderer/bse/DeclEffect.{h,cpp}`, strict value counts, references through the decl / model managers, named trails resolved | `tools/quake4/test_effect_parse.sh` (below) |
| S2 Runtime + sprites — **DONE 2026-10-25** (gate passed: user firefights in Quake 4 and Doom 3, ASAN clean on both) | Manager, effect instances behind the render world calls, timing (start, duration, count, spawner bursts, loops, delay segments, looping ambient effects already in full flow), domains and modifiers, envelopes, motion, `locked` / `attenuation` / `detail` / `density` / `particleCap`, sprites, segment draw order, `testEffect` | Offline simulation over every retail emitter (no NaN, population follows the emission rate, particles stay within the decl's `size`); the user compares 10 sprite-only effects covering every domain, modifier and envelope option with retail on GL3 and Vulkan (`moveables/barrel/debris_fly`, `monsters/scientist/obscuring_cloud`, `weapons/nailgun/guide`, `ambient/dustedge_mp`, `monsters/teleport_dropper/blast_exhaust`, `monsters/fatty/ambient_acid`, `steam/mcc_vent_slow`, `smoke/air_defense_hillsa`, `mapobjects/casket_door2_inv`, `levels/dispersal/supportbreak`); an airdefense1 fight; no FSR2 ghosting; ASAN `--q4`; Doom 3 / RoE smoke |
| S3 Particle kinds — **DONE 2026-10-26** (gate passed: all 20 comparisons ticked, play checks, ASAN clean) | line, oriented, electricity, linked, model; `motion` / `burn` / named trails | Offline geometry test; the user compares `weapons/nailgun/impact_default`, `ambient/acid_react_waste`, `weapons/lightninggun/tube` + `impact`, `weapons/railgun/trail`, `monsters/strogg_flyer/bomb_fly`, `moveables/barrel/explode`, `weapons/nailgun/impact_glass`, `monsters/makron/blaster_fly`, `monsters/gunner/grenade_burst`; lightning gun and railgun in play |
| S4 Lights, sounds, decals — **DONE 2026-10-26** (gate passed: 12 effects ticked, play checks, no device loss, ASAN clean) | light segments as render lights; sound segments on the effect's emitter (`EffectDefHasSound`, random sample choice); decal segments projected | Rocket light, impact decals, impact sounds varying per shot; a busy fight on `dude_rtx 2` with no device loss; GL3 and Vulkan |
| S5 Physics and children — **DONE 2026-10-26** (gate passed: all 16 effects, play checks, ASAN clean) | traces through `Translation`, bounce / remove, impact and timeout effects, nested effect segments, debris through `SpawnClientMoveable`, `entityDef` on other kinds, `persist` | Offline physics test; wall impacts, `wounds/blood`, harvester claw debris, turret deaths, `bse_physics` / `bse_debris`; ASAN |
| S6 View effects — **DONE 2026-10-26** (gate passed: the view effects, the intro blur after two renderer fixes) | `shake` scaled by distance, `doubleVision`, `tunnel` | Explosions shake by distance, `mapobjects/buzzsaw_chop`, the intro blur |
| S7 Cvars and tools — **BUILT 2026-10-26** (gate pending) | the retail `bse_*` cvars, rate limits, `bseSweep` + log summary, debug overlay, opt-in extras, performance | quake4-port.md §6's Q4 gate: `bseSweep` clean over all 1172, ~20 effects signed off against retail, OpenQ4's prose checklist, Doom 3 / RoE / LM smoke, ABI 0 breaks |

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
80 units in front of the view facing it, where `spawn func_fx` puts one, looping (or once) for 4
seconds or its whole play when that is longer; then it stops spawning, its particles die out and it
is freed (2026-10-26, at the user's request: it used to stay until removed). `testEffect` alone
removes it at once.

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

**S5 as built.** The runtime (`BseEffect.cpp`) does §3.4 through four new engine hooks:
`BSE_Trace` (the game's `Translation`, Quake 4's `CONTENTS_SOLID | CONTENTS_RENDERMODEL`; a
contact without a plane - the collision model's "huge translation" - is no hit),
`BSE_CanPlayImpactEffect` (the game-facing rate limit, open until S7), `BSE_RequestEffect` and
`BSE_RequestDebris`. A particle keeps the time its motion runs from (its birth or last bounce) and
whether it is at rest; positions and velocities evaluate from that time. The engine side
(`BseRender.cpp`) queues the requests, bounded, per render world (a freed world drops its own),
and `BSE_DeliverRequests` hands them to the game from the effect manager facade's `StartFrame` /
`EndFrame` (`quake4/RenderWorldQ4.cpp`): `game->PlayEffect` (once, no end origin, white) and
`game->SpawnClientMoveable` (lifetime 0). Traces never run while a view is being built (an effect
serviced for its first view skips them; the next service's 0.1 s window covers the gap). The decl
precaches every particle's `entityDef` through a hook the Quake 4 title sets
(`effectPrecacheEntityDef` -> `game->CacheDictionaryMedia`, only while a map loads or runs, so
`lintDecls` does not load debris models). `quake4/EngineQ4.h` gained `GameQ4()` and
`GameQ4_MapActive()`. New Quake 4 cvars `bse_physics` and `bse_debris` (1). No game library change;
the dhewm3 ABI is untouched (`check_dhewm3_abi.py`: 0 breaks).

Offline (`test_bse_sim.sh`): every effect now plays inside a closed room - a box the stand-in trace
collides with as the collision model's point traces do (one-sided, stopping 0.25 short) - and the
effects it asks the game for are played back as the game would (once, where asked, up to three
deep), with the real retail decls as children. The child decls are all made before any decl parses,
so impact, timeout and nested effects resolve to the effects themselves; the 6 names the data
lacks are defaulted and play nothing. The looping run ends with a stop and must then end within
65 s. Result: 0 values not finite, 0 bad requests (origin, axis, velocity, spin, entity def), **0
particles crossing a wall while a trace covered them** (2350 samples went through retail's untraced
slice: `explosions/surface/rock_exp`, `explosions/air/airburst_03`), 683517 traces, 25640 hits,
15591 bounces, 5520 particles come to rest; 3573 impact, 341 timeout and 45 nested effect requests
and 3437 debris (all 59 entity defs), at most 100 effects (`monsters/slash`) and 19 debris in one game
frame; 3706 children played back, none deeper than three, at most 164 alive in one run, all ended;
every stopped looping effect ends. The S2-S4 checks are unchanged (0 bad geometry, lights,
decals; 29/29 trails fed). `test_effect_parse.sh` 1172 / 0 failed, 0 mismatches; Linux and Windows
builds; `q4base.so` rebuilt.

Fidelity: what both references agree on is kept, retail's untraced slice included. The bullet
impacts S4 signed off (`weapons/*/impact_concrete`) now bounce their sparks off the floor instead of
letting them fall through it. Child effects start through the game, which adds them on its next
think: up to one game tic (16 ms) later than retail, whose effects called the game in the middle of
their own service.

**S5 gate (user):** on the Effect Browser, the stage selector set to stage 5, the "Stage 5 gate"
effects (16):
1. With `testEffect` near a floor or wall: the bullet impacts `weapons/machinegun/impact_concrete`,
   `weapons/shotgun/impact_concrete`, `weapons/nailgun/impact_concrete` and `sparks/generic`
   (sparks hop and settle), `moveables/barrel/explode` (bouncing chunks), `monsters/gib` (chunks
   splat and vanish), `wounds/blood` (drips that splat on the floor once they time out),
   `ambient/single_drip` (a drop splashing), `monsters/spiderhead/spit_impact` (an acid pool);
   debris: `monsters/harvester/claw_metal`, `monsters/harvester/claw_dirt`,
   `explosions/surface/turret_exp`, `vehicles/explosion_tank`, `monsters/strogg_flyer/explosion`;
   nested: `monsters/turret/dropturret_death` (a fire column after 0.3 s; debris too),
   `levels/convoy/ob_all` (its explosion after 1.05 s).
2. Retail side by side (the two reference discrepancies, §1): sparks of a bullet impact on a floor -
   do they hop a few times before settling? Blood or sparks falling onto a corpse - do they land on
   it or pass through?
3. In play: sparks off walls and floors, blood on the floor after a hit, gibbing, turret and
   harvester deaths (debris flying, fading); `bse_physics 0` / `bse_debris 0` turn them off.
4. A busy fight on GL3 and Vulkan and on `dude_rtx 2`: no device loss, no hitch from debris.
5. `tools/run-asan.sh --q4` for a few minutes of play; Doom 3 / RoE smoke (the scene hook and the
   game adapter are shared; Doom 3 never runs BSE).

**S6 as built.** `idBseSegment::Check` handles the three view segments (§3.5) through one more
engine hook, `BSE_RequestViewEffect`; `idBseEffect::OriginAttenuation` is the strength ramp (the
duration uses the existing `SegmentAttenuation`). The request queue (`BseRender.cpp`) gained a
kind per request (effect, debris, view; 256 / 128 / 64 a bracket) and delivers view effects as
`game->StartViewEffect` with Quake 4's `VIEWEFFECT_*` numbers. The decl's default `scale` is 1, as
retail's (it was 0, which would have left the 29 shakes without a `scale` - the rocket and grenade
impacts among them - still).

Offline (`test_bse_sim.sh`): 1076 shake and 12 double vision requests over the run, none bad (end
time finite and not before the effect's start, scale finite and not negative), strongest 8
(`scale 8`), longest 4.98 s; the rocket impact's shake (no `scale`, attenuation 200-600) gives 1, 0.5
and 0 at 100, 400 and 700 units, for both the strength and the duration. The S2-S5 results are
unchanged. `test_effect_parse.sh` 1172 / 0 failed; ABI 0 breaks; Linux and Windows builds;
`q4base.so` rebuilt.

**S6 gate (user):** on the Effect Browser, stage 6:
1. With `testEffect` (80 units in front): `weapons/rocketlauncher/impact`,
   `weapons/grenadelauncher/impact` (a short shake, weaker from further away: `setviewpos` back
   400 units halves it, 600 ends it), `monsters/camerashakelarge` (a strong shake, from 500 units
   on weaker), `monsters/gladiator/footstomp`, `levels/core/doors` (a shake at any distance),
   `cinematics/intro/blur` (double vision for 5 s; `g_doubleVision 1`).
2. In play: rocket and grenade explosions near and far, the airdefense1 intro (the blur), a
   buzzsaw hit on the player (shake and double vision - `testEffect` cannot show those, they act
   only at the effect's origin).
3. Retail side by side for one of them (the strength of a near rocket impact's shake).

**S7 as built.** `neo/renderer/bse/BseTools.cpp` (new): `bseLog`, `bseStats`, `bseSweep`.
`BseRender.cpp`: the cvars of §3.6, the rate limiter and filter (`BSE_Filtered`,
`BSE_CanPlayRateLimited`, `BSE_UpdateRateTimes`, `BSE_ResetRateTimes`), the name filter in
`BSE_PlayEffect`, retail's debug lines ("BSE: Play %d: %s at %g", "BSE: Stop: %s", "BSE: Free: %s"),
`bse_showBounds` drawn before each scene, the `bse_debug 2` counters, soft particles on effect
surfaces, and `testEffect`'s placement as `BSE_StartTestEffect` (which `bseSweep` reuses). The Quake
4 facade (`quake4/RenderWorldQ4.cpp`) answers the game's `Filtered` / `CanPlayRateLimited` from it
and pays the budgets back at `StartFrame`; a new map resets them (`quake4/GameQ4.cpp`). The runtime
counts an effect's loops for `bseLog` and the segment's average sprite size for soft particles;
particle lives are clamped to retail's 60 s (the code said 300, the docs 60: no retail particle
differs). `framework/Common` gained `Com_NumWarnings()` (a free function: the `idCommon`
interface is unchanged) for the sweep's report. Offline tests unchanged (the sim's output is
identical); parse test 1172 / 0; ABI 0 breaks; Linux and Windows builds; `q4base.so` rebuilt.

**OpenQ4's prose checklist** (its "Retail Parity Audit", §1) against DUDE:

| Audit item | DUDE |
|---|---|
| Spawn domains: retail sampler table, cube-face order, rectangle outlines, spirals, the bounds literals | S2 (§3.1) |
| Model spawn domains | no retail effect uses one (§2): not built |
| Segment costs from retail's base-cost table; the `bse_debug 2` cost overlay | not built: `bse_debug 2` prints DUDE's counters instead |
| Segment bounds, spawn volume, furthest distance with gravity | S2 (the bounds formula) |
| Particle life clamped to 60 s, trail counts truncated | S2 / S3; the clamp fixed to 60 s in S7 |
| Model particles trace as points; debris precaches its entity def | S5 |
| A missing child effect plays nothing | S5 |
| Segment timing (`Check`): fast-forward, constant emitters, spawner truncation, one-shot segments | S2 |
| Sounds: volume / pitch before start, one-shot per loop, looping latched, random diversity | S4 |
| Child effects only in a connected area, without the end origin | S5 |
| 0.016 s look-ahead; interval attenuation's direction; lights initialised only when fresh | S2 / S4 |
| Decals sized from `size.x`, turned by `rotate.x` | S4 |
| Electricity: surface from the live bolts, bend points, winding, fork width | S3 |
| Loop / ambient latched; two loops of history for ambient effects | S2 |
| Spawn order, generated normals, friction as a vector, end-origin resampling | S2 / S3 |
| Line, linked, debris overrides; debris hands off at birth | S3 / S5 |
| Bounces keep their frame and come to rest; traces include render models; impact and timeout effects respect the connected area | S5 |
| Envelopes at the rate fixed at spawn | S2 |
| Vertex colours: additive fades through RGB, bytes wrap; faded particles still draw; generated lines collapse without velocity | S2 / S3 |
| Light particles: raw tint, unsigned radius clamp, default light shader | S4 |
| `TableLookup` 1.0 for one-value tables; effects drawn farthest first | S2 |
| The client effect owns its sound emitter | the game library (SDK 1.4.2 code) |
| `bseStats` / `bseLog`; `bse_showBounds` an integer; `bse_scale` archived | S7 |
| Retail quirks: `linearSpacing` extrapolation, colour byte wrap | S2 |
| OpenQ4's deliberate divergences (time-based limiter, presentation clock, service budgets, soft particles, …) | not taken, except soft particles as an opt-in (`bse_softParticles` 0) |

**S7 gate (user) = the Quake 4 phase gate** (quake4-port.md §6, row Q4):
1. `bseSweep` in a map (e.g. `map game/airdefense1`, then `bseSweep`; about 100 batches, a few
   minutes): the summary line with nothing that did not play or parse; the batches that gave
   warnings, and their warning text, to look at together (known: the data's missing names, §2).
2. The Effect Browser's "Stage 7 gate": 20 effects next to retail (`testEffect` in DUDE,
   `spawn func_fx fx <effect> loop 1` in retail from the same `setviewpos`).
3. The cvars: `bse_singleEffect rocket` (only rocket effects play), `bse_showBounds 1` / `2`,
   `bse_debug 1` / `2`, `bseLog`, `bseStats all`, `bse_softParticles 1` (smoke and fire sprites
   soften against walls and the floor, GL3 and Vulkan); a shotgun at a wall (every pellet's impact
   shows, as retail 1.4's limit promises).
4. `tools/run-asan.sh --q4` during a sweep; Doom 3 / RoE / Lost Mission smoke (the session's
   warning counter and the scene hook are shared); ABI 0 breaks (done).

**S6 gate result (2026-10-26, user): passed, merged to master.** Shakes ticked on the Effect
Browser (rocket and grenade impacts by distance, `camerashakelarge`, the gladiator's footstomp,
`levels/core/doors`). The intro blur (`cinematics/intro/blur`) found two renderer bugs in views the
game renders into a crop - Quake 4's double vision renders the player's view into
`CropRenderSize( 512, 256 )`, which DUDE's own Doom 3 code no longer does on GL3/Vulkan, so no gate
had exercised it:
- the portal sky was black: `R_PortalSkyRender` served window-sized views only (`fix(renderer)`
  40e86b95: a view rendered while a crop is active, `viewDef_t::isCropView`, qualifies);
- on Vulkan the fire effects' heat haze (`gfx/effects/energy_sparks/warp_mask`) drew discs of one
  row of `_currentRender`: Vulkan puts a crop on the target's bottom rows, and its screen lookups
  assumed a view starts at the top (`fix(vulkan)` 51adf8ea: the transpiled ARB `fragment.position`,
  the screen texgen / sky composite row offset and the soft particles' depth lookup use the
  view's own rows).
After both, the blur is right on Vulkan (user). Left as a follow-up (§6): with `r_hdr` the blur's
frames skip the HDR tone map - a frame goes through the HDR buffer only when it has a full-window
world view, and the double vision frame's only world view is the crop - so bright fire looks
hotter for the blur's 5 s (the untone-mapped, retail look); RoE / Lost Mission with
`g_lowresFullscreenFX 1` likewise.

**S5 gate result (2026-10-26, user, ASAN build): passed, merged to master.** 15 of the 16 stage-5
effects ticked on the Effect Browser, and the 16th passed as well (below); impacts, bounces, drips, gibs, acid pools, debris (`harvester/claw_dirt` and the others) and
both nested effects look right; no ASAN report. The one left unticked, `explosions/surface/turret_exp`,
throws dark boxes: its debris models are missing from the retail data, and retail spawned from the
console throws the same dark boxes (§2) - a pass. The timeout direction (the particle's own world direction, not OpenQ4's
current-frame reading) is the user's choice. `vehicles/explosion_tank` under `testEffect` costs
frame time: per play 10 debris rigid bodies live 5 s, each trailing `fire/spot_32_trail_bigsmoke`
(six emitters, about 30 smoke sprites of radius 50-120), and `testEffect` loops the effect for 4 s
80 units from the eye - two or three plays at once, about 1000 large overlapping smoke quads plus
the game's debris physics, on an ASAN build. The same per play in retail, which plays it once per
tank death; the effect has no impact blocks, so S5's traces are not part of it. Open: a non-ASAN
look at that cost (`testEffect effects/vehicles/explosion_tank once`), and the two retail
side-by-sides of §1.

**S4 as built.** The runtime (`BseEffect.cpp`) handles the three segment kinds of §3.3 through
engine hooks, so the offline test keeps running it: `BSE_StartSound` / `BSE_UpdateSound` /
`BSE_StopSound` reach the game's emitter through the Quake 4 sound facade
(`SoundSystemQ4()->EmitterForIndex`, the calls retail's effects make), `BSE_ProjectDecal` calls
`ProjectDecalOntoWorld`, and `EvaluateLight` gives a light segment's light, which `BSE_SyncWorld`
adds, updates and frees as a render light before each scene. `idSoundShader::GetTimeLength` (new,
non-virtual: the dhewm3 ABI is untouched) gives a sound's length. `EffectDefHasSound` now answers
from the decl. `testEffect` allocates an emitter of its own (freed with the effect, letting its
sounds finish) and puts an effect that leaves a decal on the surface in view. The decl's default
`freqShift` is 1 (was 0). Effect lights go through the engine's light path like the game's own
muzzle flashes; the ray-traced tiers' light table caps itself (256) and drops the rest.

Offline (`test_bse_sim.sh`, now also evaluating every light ten times a second and checking every
decal's projection and every sound's parameters): 0 bad lights, 0 bad decals; about 4900 light
samples, 9800 sounds started and 4200 decals over the run. Parse test, ABI check (0 breaks), Linux
and Windows builds, `q4base.so`.

**S4 gate (user):** on the Effect Browser, the stage selector set to stage 4, the "Stage 4 gate"
effects - their particles are stage 3's; this stage adds the light, the sound and the decal:
1. With `testEffect` (an impact goes on the wall in view; sounds play): the rocket's flight
   (`weapons/rocketlauncher/fly`: an orange light that follows it, the looping rocket sound), the
   rocket and grenade impacts (`weapons/rocketlauncher/impact`, `weapons/grenadelauncher/impact`:
   explosion sounds, scorch), the bullet impacts (`weapons/machinegun/impact_concrete`,
   `weapons/shotgun/impact_concrete`, `weapons/nailgun/impact_default`,
   `weapons/hyperblaster/impact_concrete`: a mark on the wall, a sound that varies between shots),
   `weapons/grenadelauncher/trail` and `monsters/gunner/grenade_fly` (a light that stays with the
   effect), `monsters/strogg_flyer/bomb_burst` and `monsters/scientist/concussion_burst` (sound,
   light and scorch together), `monsters/bossbuddy/charge2` (lights casting shadows).
2. In play: the rocket launcher (light following the rocket, its sound stopping at the impact),
   machine gun and shotgun on walls (marks, sounds varying per shot), a grenade fight.
3. A busy fight on `dude_rtx 2` (Path Tracing): no device loss; GL3 and Vulkan.
4. `tools/run-asan.sh --q4` for a few minutes of play; Doom 3 / RoE smoke (`sound.h` and the
   scene hook are shared).

**S4 gate result (2026-10-26, user):** the 12 effects ticked "Checked" for stage 4 on the Effect
Browser; rockets, bullet impacts and grenade fights right in play; the demanding fights showed no
device loss. One crash in a busy hangar1 fight, not in the effects: a ragdoll's start-in-solid test
(`idAF::TestSolid`) touching another trace model read the engine's trace-model brush, whose material
id never set - an uninitialised contact material the Quake 4 collision facade dereferenced
(`ContactToQ4`). Fixed in the engine (`fix(cm)` 68009680: the brush takes the trace model's
material, as its polygons), which covers Doom 3, RoE and Lost Mission too; the fight replayed under
ASAN without a crash or report. Merged to master with that fix.

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

**S3 gate result (2026-10-26, user):** all 20 comparisons look like retail and are ticked
"Checked" on the Effect Browser; the play checks are fine. The concrete / rock item fell away:
those effects are never played by retail (§2), so nothing in play triggers them. The gate's ASAN
runs found no memory error but stopped on a debug `assert` (Doom 3's 10000-unit limit in
`BoundsInAreas`, passed by an effect bound to airdefense1's convoy truck, whose animation spans
55000 units - retail data; fixed for Quake 4's facade only) and showed a minute-long hitch when
skipping the intro (fixed: effect bounds taken per rendered scene, not per game tic - §4 S2 as
built). Found and fixed on the way: a failed `assert` now leaves a backtrace (crash handler takes
SIGTRAP; `run-asan.sh` reports it with a symbolized stack), Ctrl+V pastes in the console (it never
did with SDL2), and `testEffect` shows an effect for 4 s or its whole play, then frees it. Merged to
master with those fixes.

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

- **HDR for views rendered into a crop** (queued 2026-10-26, user): Quake 4's double vision frames
  (and RoE / Lost Mission's with `g_lowresFullscreenFX 1`) have no full-window world view, so
  `RB_RHI_FrameHasWorldScene` keeps them off the HDR path - no tone map or `r_hdrExposure` for the
  blur's seconds, a visible exposure jump on HDR setups. Option: let a cropped world view route the
  frame through the HDR buffer; to work out first: the order of the 2D double vision composite and
  the HUD against the tone map resolve, eye adaptation (measure the crop or hold), FSR2 (built for
  one full-window view). Own branch and test round.

- After the BSE particle path is done: reuse it to fix Doom 3's particle buffer churn on Vulkan
  (`idRenderModelPrt` snapshots → frame-temporary memory), as the user asked on 2026-10-25.
