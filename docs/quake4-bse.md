# Quake 4 effects (BSE) on DUDE — phase Q4

Status (2026-10-25): **plan approved; S1 (the effect decl) built** on `feat/quake4-bse-decl`.
Effects still do not play: the render world declines every effect until S2.

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
| Decompile consultations | none so far |

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
   the 323 materials effects use are lit (model particles); lit model particles and debris become
   real render entities (lighting, shadows, motion vectors).
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

## 4. Stages and gates

One branch per stage, merged to master at its gate after the user's verification. The user runs
every in-game check; nothing here launches the game.

| Stage | Scope | Gate |
|---|---|---|
| **S1 Decl** — **BUILT 2026-10-25** | The grammar of §2: `neo/renderer/bse/DeclEffect.{h,cpp}`, strict value counts, references through the decl / model managers, named trails resolved | `tools/quake4/test_effect_parse.sh` (below) |
| S2 Runtime + sprites | Manager, effect instances behind the render world calls, timing (start, duration, count, spawner bursts, loops, delay segments, looping ambient effects already in full flow), domains and modifiers, envelopes, motion, `locked` / `attenuation` / `detail` / `density` / `particleCap`, sprites, segment draw order, `testEffect` | Offline simulation over every retail emitter (no NaN, population follows the emission rate, particles stay within the decl's `size`); the user compares 10 sprite-only effects covering every domain, modifier and envelope option with retail on GL3 and Vulkan (`moveables/barrel/debris_fly`, `monsters/scientist/obscuring_cloud`, `weapons/nailgun/guide`, `ambient/dustedge_mp`, `monsters/teleport_dropper/blast_exhaust`, `monsters/fatty/ambient_acid`, `steam/mcc_vent_slow`, `smoke/air_defense_hillsa`, `mapobjects/casket_door2_inv`, `levels/dispersal/supportbreak`); an airdefense1 fight; no FSR2 ghosting; ASAN `--q4`; Doom 3 / RoE smoke |
| S3 Particle kinds | line, oriented, electricity, linked, model; `motion` / `burn` / named trails | Offline geometry test; the user compares `weapons/nailgun/impact_default`, `ambient/acid_react_waste`, `weapons/lightninggun/tube` + `impact`, `weapons/railgun/trail`, `monsters/strogg_flyer/bomb_fly`, `moveables/barrel/explode`, `weapons/nailgun/impact_glass`, `monsters/makron/blaster_fly`, `monsters/gunner/grenade_burst`; lightning gun and railgun in play |
| S4 Lights, sounds, decals | light segments as render lights; sound segments on the effect's emitter (`EffectDefHasSound`, random sample choice); decal segments projected | Rocket light, impact decals, impact sounds varying per shot; a busy fight on `dude_rtx 2` with no device loss; GL3 and Vulkan |
| S5 Physics and children | traces through `Translation`, bounce / remove, impact and timeout effects, nested effect segments, debris through `SpawnClientMoveable`, `entityDef` on other kinds, `persist` | Offline physics test; wall impacts, `wounds/blood`, harvester claw debris, turret deaths, `bse_physics` / `bse_debris`; ASAN |
| S6 View effects | `shake` scaled by distance, `doubleVision`, `tunnel` | Explosions shake by distance, `mapobjects/buzzsaw_chop`, the intro blur |
| S7 Cvars and tools | the retail `bse_*` cvars, rate limits, `bseSweep` + log summary, debug overlay, opt-in extras, performance | quake4-port.md §6's Q4 gate: `bseSweep` clean over all 1172, ~20 effects signed off against retail, OpenQ4's prose checklist, Doom 3 / RoE / LM smoke, ABI 0 breaks |

`testEffect` moves from S7 into S2: every later gate needs it.

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
