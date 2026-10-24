# Quake 4 on DUDE — milestone plan (draft 2026-10-15)

Status (2026-10-24): **Q0-Q3 done and on master** (Q3, world formats: merge 714881e7 - all 31
campaign maps load and quit cleanly in the null game on Vulkan and OpenGL3, `cm`/`aas`
self-checks clean, `airdefense1` user-verified on both renderers; the collision API additions
moved to Q5). **Q5 (game library) started 2026-10-24** (user: Q5 before Q4, BSE later),
plan in §5.6: Q5.0 (dhewm3 ABI) merged, Q5.1 (build + inventory) and Q5.2 (idlib layer, 4214
-> 2349 errors, 0 in idlib) done on `feat/quake4-game-library`; Q5.3 (engine interfaces) in
progress: the game library compiles and links (q4base.so); the Doom 3 + RoE smoke test is the
gate left, then Q5.4 (engine facades and the Quake 4 loader). Goal: **DUDE runs the retail Quake 4 (2005,
Raven Software, idTech4) single-player campaign natively** — its own `q4base/*.pk4` assets,
its own game logic, on DUDE's GL3 and Vulkan backends, with DUDE's enhancement tiers
(PBR, tessellation, RT) available as opt-in on top. Multiplayer is out of scope for v1 (§9).

This replaces the two-paragraph scouting note that used to be the whole plan. It rests on
three sources, in order of trust:
1. A **measured audit of the retail install** (Steam build 1.4.3, 3.0 GB, 36 `.pk4`) with the
   scripts in [tools/quake4/](../tools/quake4/README.md), diffed side-by-side against Doom 3's
   `base/pak000.pk4`. Every format delta in §1 was read from the files, not recalled.
2. The **two open-source engines that already run Quake 4** (§4): glKarin's idTech4A++ and
   themuffinator's OpenQ4. Their trees tell us the size of the engine-side work and the exact
   interface the Quake 4 game code expects.
3. The **Quake 4 SDK 1.4.2** licence text and contents (§3), verified from mirrors.

Claims that still rest on unverified research are marked **[verify]**. The four design
decisions the plan hinged on were taken by the user on 2026-10-15 and are recorded in §9.

---

## 0. TL;DR

- **Verdict: feasible; the formats are a smaller problem than the old note feared, the game
  API a bigger one, and licensing the decisive one.**
- **Formats barely moved and every change is additive.** MD5 mesh/anim/camera are
  byte-compatible (`MD5Version 10`, identical grammar across all 234 meshes). `.map` bumps
  `Version 2 → 3` only. `.proc` `mapProcFile003 → PROC "4"` adds a CRC line, an optional RGBA
  colour on vertices (12 floats instead of 8, ~10 % of vertices) and an optional portal-fade
  tuple. `.cm` `"1.00" → "3"` adds per-polygon texture coordinates and each row's map
  primitive number. AAS
  `1.07 → 1.08` adds two area fields and a tactical-features section, in five sizes per map.
  Fonts are a different (smaller) binary. Materials, GUIs and sound shaders gain a few dozen
  keywords and a macro preprocessor (`guide`). Measured lists in §1.
- **One genuinely new engine subsystem: BSE**, Raven's effects system — 1180 text `.fx`
  decls the closed engine renders. The census in §1.6 bounds it (10 particle types, 6 spawn
  domains, ~40 envelopes). **Both existing open implementations descend from a Hex-Rays
  decompile of a Quake 4 debug build**. Decision (§9): DUDE's BSE is written from the file
  grammar, the SDK interface header and the public docs; the decompile-derived code is a
  reference of last resort for hard-to-find behaviour, and anything learned from it is
  re-expressed in our own modern C++, never copied (§3, §5.5). Still the single biggest cost.
- **The game API is the real porting work.** Quake 4's `GAME_API_VERSION` is **37** (DUDE: 9).
  `idGame` has 101 virtuals vs DUDE's 44, `idGameEdit` 122 vs 55, and the engine-provided
  interfaces (render world, sound, collision, decls, UI) all grew (§5.4). idTech4A++ measures
  the engine-side footprint at **~965 `#ifdef _RAVEN` sites across ~170 engine files plus
  14.7 k new lines** (10.8 k of them BSE). Expect the same order of magnitude here.
- **One `dude` executable for every supported title** (decision, §9). A runtime title profile
  (§5.1) picks the base directory, config, decl set and the per-title game library —
  `q4base.so` beside today's `base.so` / `d3xp.so` / `d3le.so` — exactly as `fs_game` does now.
  No separate Quake 4 binary, no compile-time title macros.
- **Game logic exists in source**: SDK 1.4.2 = `game/` + `mpgame/` + `idlib/` source and
  engine headers (`bse/` ships only `BSEInterface.h`). Steam 1.4.3 differs from 1.4.2 only in
  legal text (`CHANGES_14.txt`); the shipped `gamex86.so` reports `1.4.2`.
- **Licence structure**: the SDK is a non-commercial limited-use EULA, not GPL. DUDE (GPLv3)
  keeps all engine work in-tree; SDK-derived game code lives in a **separate repository with
  the EULA attached** (the §5 redistribution clause permits that; OpenQ4 does exactly this),
  loaded across the existing `GetGameAPI` boundary. No prebuilt game binary is shipped until
  the user has decided on the legal posture (§3, §9).
- **Definition of done (v1):** `./run-q4.sh` boots to Quake 4's own main menu; the 31-map
  campaign (`airdefense1` → `core2`, Appendix C) is completable start to finish on GL3 and
  Vulkan with vehicles, cinematics, lip-sync, saves/loads and every BSE particle type
  rendering; DUDE's presets apply (Potato = faithful floor). Verified by the user playing it.

---

## 1. Evidence: the measured drift

Install: `/mnt/linux-data/SteamLibrary/steamapps/common/Quake 4/q4base`. Reproduce with
`python3 tools/quake4/q4_inventory.py` and `tools/quake4/q4_keywords.py`.

### 1.1 Summary table

| Format | Doom 3 (DUDE today) | Quake 4 (retail 1.4.3) | Delta | DUDE touch point |
|---|---|---|---|---|
| `.md5mesh` / `.md5anim` / `.md5camera` | `MD5Version 10` | `MD5Version 10` | **none** — identical keyword set across all 234 meshes, 2233 anims, 19 cameras | `neo/renderer/Model_md5.cpp:1899` |
| `.map` | `Version 2` | `Version 3` | same `brushDef3` / `patchDef2` / `patchDef3` primitives; brush sides **drop** Doom 3's trailing `0 0 0` ints; patches carry no per-primitive key/values; **`func_group` entities stay in the file with their primitives** (418 in `airdefense1.map`) and are folded in at load (`idMapFile::Resolve`); one `.map` can hold several levels selected by an **entity filter** (`map game/process1 first` → mapDef `process1_first`, `si_entityFilter`) | `neo/idlib/MapFile.cpp:752` (`CURRENT_MAP_VERSION`), `idMapEntity::Parse`, `Session` map command |
| `.proc` | `mapProcFile003` | `PROC "4"` + map-geometry CRC line | same sections (`model`, `shadowModel`, `interAreaPortals`, `nodes`); **vertices are 8 or 12 floats** (`xyz st normal [r g b a]`, 95 k of 904 k verts in `pak002` carry colour); IAP rows gain optional `( fadeImage distanceNear distanceFar )`; the SDK header also allows an optional per-model `hasSky` int (portal sky) — **not present** in the 657 model headers of `airdefense1.proc`, so parse it as optional | `neo/renderer/RenderWorld_load.cpp:547` (`PROC_FILE_ID`), `ParseModel`, `ParseInterAreaPortals` |
| `.cm` | `CM "1.00"` | `CM "3"` (+ `"2"` on 4 pak019 models) | `collisionModel "name" <int> {` = the model's primitive count; polygon rows gain `( s t ) ( s t ) ( s t ) <primitive>` after the material; brush rows gain `<primitive>` after the contents string; block headers give counts, not bytes; Raven contents names (`"aas_solid"`, `"shotClip"`) | `neo/cm/CollisionModel_files.cpp:572` (`CM_FILEVERSION`), `ParsePolygons`, `ParseBrushes`, `ContentsFromString` |
| `.aas*` | `DewmAAS "1.07"`, one `.aas48` per map | `DewmAAS "1.08"`, **five** per map (`.aas32/.aas48/.aas96/.aas128/.aas250`, named in `def/aas.def` `aas_types`); unused sizes are **dummy files** (settings only, e.g. `airdefense1.aas250` = 628 B; `idAASFile::IsDummyFile`) | `settings` gains `generateTacticalFeatures`, `tt_*`; `areas` rows 6 → 8 (adds `ceiling`, `numFeatures`, `firstFeature`); new `features` (12-byte `aasFeature_t`: cover/corner/pinch/vantage flags for tactical AI) + `featureIndex` sections | `neo/tools/compilers/aas/AASFile.cpp:1123`; `idAASFile` layout must match the SDK's `aas/AASFile.h`; `idAASFileManager` gains `CreateNew/CreateAASSettings/CreateReachability/FromParser` |
| `.lwo` / `.ase` | LWO2 / ASE | same (962 LWO, 47 ASE) | none expected | `Model_lwo.cpp`, `Model_ase.cpp` |
| Textures | `.tga`, `dds/` precompressed | `.tga` + sibling `.dds` (8454); `progimg/` = 2401 precomputed image-program outputs (`addnormals`, `heightmap`, `makealpha`, `downsize`) | loaders unchanged; `progimg/` is an optional load-time accelerator. **One** image of 10 995 exists only as a sibling `.dds` (`shell_loader_s`, DXT1, a specular map; retail reads it with `image_usePrecompressedTextures 1`): since 2026-10-24 `R_LoadImage` decodes a same-path `.dds` on the CPU when the `.tga`/`.jpg` is missing (`LoadDDS`: DXT1/3/5, RXGB, 24/32-bit; bit-exact against Pillow, `tools/quake4/test_dds_decode.sh`), every backend - Doom 3's paks have no `.dds` outside `dds/`, so it never runs for them | `Image_load.cpp`, `Image_files.cpp` |
| Fonts | `fonts/<lang>/<face>/fontImage_<pt>.dat`, 20548 B | `fonts/english/<face>_<pt>.fontdat`, **9236 B** = 256 glyphs × 9 float32 (`imageWidth, imageHeight, horiAdvance, horiBearingX, horiBearingY, s, t, s2, t2`) + 4 float32 (`pointSize, fontHeight, ascender, descender`) + 4 B; 6 faces × 3 sizes, one image each; size picked by `textscale` (≤0.3 → 12, ≤0.6 → 24, else 48); Symbol-codepage glyph slots | different binary struct | `neo/renderer/tr_font.cpp:333` (`FILESIZE_fontInfo_t`) |
| Video | `.roq` | `.roq` only (6; the SDK's `Cinematic.h` also mentions a Windows-only WMV path, unused by the paks) | none | `Cinematic.cpp` |
| Strings | `strings/*.lang` | same (19) | none | `LangDict.cpp` |
| Sound | `.wav`/`.ogg`, `.sndshd` | same media; `.sndshd` keyword additions (§1.4) | parser | `neo/sound/snd_shader.cpp` |
| EAX/EFX | `efxs/<map>.efx` | `efxs/default.efx` (`Version 1`, 23 `reverb` presets) + per-map `maps/game/<map>.reverb` (area → preset) | new mapping file; sound API gains `SetReverb/GetReverb/GetReverbName/GetNumAreas` | `neo/sound/snd_efxfile.cpp` |
| Materials | `.mtr` | `.mtr` + `guides/*.guide` macro templates | keyword additions (§1.3) + a **guide preprocessor** DUDE lacks | `neo/renderer/Material.cpp`, `neo/framework/DeclManager.cpp` |
| GUIs | `.gui` | `.gui` (301 files, `mainmenu.gui` = 713 KB) | same window types; keyword additions (§1.5); UI API additions (§5.4) | `neo/ui/*.cpp` |
| Effects | `fx/*.fx` (`idDeclFX`), `particles/*.prt` | `effects/*.fx` — **BSE**, 1180 files | **new subsystem** (§1.6, §5.5) | none today |
| New decl types | — | `materialType` (`materials/types/types.mtt`, 19 entries), `lipSync` (87 files), `playback` (14), `playerModel` (70 in `.def`), `camera` (22 in `.def`, `anim <name> <file.md5camera>`); BSE envelope tables in `materials/tables.mtr` (77 KB) | engine registers `DECL_MATERIALTYPE`, `DECL_LIPSYNC`, `DECL_PLAYBACK`, `DECL_EFFECT`, `DECL_PLAYER_MODEL` (the SDK enum drops `DECL_FX`/`DECL_PARTICLE`); the game registers `model`, `export`, `camera`, `entityDef`, `articulatedFigure` | `DeclManager.cpp:823` registration list |
| Shader programs | ARB `.vfp` | same ARB set the materials reference (`heatHaze*.vfp`, `bumpyEnvironment.vfp`) + engine-internal `md5r*.vp` GPU skinning + 4 GLSL pairs in `glprogs/glsl/` | `glslProgram` stages are gated by `if glslPrograms == 1`; most referenced `.glsl` files are **not shipped** → define `glslPrograms = 0`, skip the stage | `neo/renderer/ArbToGlsl.cpp`, `Material.cpp:1047` |
| Game library | GPL source | `gamex86.dll`/`gamex86.so`/`game.so.bundle` in `game000–300.pk4`; 32-bit, stripped, exports only `GetGameAPI` | unusable; rebuild from the SDK | `neo/framework/Common.cpp:3078` (`LoadGameDLL`) |

### 1.2 What the install contains

36 `.pk4`, 3.0 GB. `pak001`–`pak012` are retail 1.0, `pak013+` the 1.1–1.4 patches,
`q4cmp_pak001.pk4` the competition map pack, `zpak_english*.pk4` localisation (3396 `.ogg` VO +
`.lang`). Counts by extension:

| ext | count | ext | count | ext | count |
|---|---|---|---|---|---|
| `.tga` | 11099 | `.md5anim` | 2233 | `.def` | 280 |
| `.dds` | 8454 | `.fx` (BSE) | 1180 | `.md5mesh` | 234 |
| `.ogg` | 5737 | `.cm` | 983 | `.sndshd` | 174 |
| `.lwo` | 962 | `.gui` | 296 | `.mtr` | 110 |
| `.map` / `.proc` | 102 each | `.lipsync` | 87 | `.script` | 63 |
| `.aas32/48/96/128/250` | 30 each | `.af` | 57 | `.reverb` | 54 |
| `.ase` | 47 | `.skin` | 38 | `.vp`/`.vfp`/`.fp` | 48 |
| `.md5camera` | 19 | `.fontdat` | 18 | `.playback` | 14 |

Top-level directories: `textures` 9251, `models` 8774, `sound` 5946, `gfx` 3856 (GUI art),
`progimg` 2010, `effects` 1180, `maps` 511, `guis` 296, `def` 281, `materials` 113, `lipsync` 88,
`scripts` 62, `glprogs` 58, `af` 57, `fonts` 56, `skins` 38, `strings` 19, `playbacks` 14,
`video` 6, `guides` 5, `efxs` 1. Per map: `.map`, `.proc`, `.cm`, five `.aas*`, `.reverb`.
102 maps; 31 single-player (Appendix C).

### 1.3 Material keywords Quake 4 uses that `neo/renderer/Material.cpp` does not accept

From all 110 `.mtr` + 3 `.guide` files (n = occurrences):

| keyword | n | meaning / action |
|---|---|---|
| `materialType <name>` | 475 | surface type for impacts/footsteps/BSE `impact` selection; index into the `materialType` decl table, exposed on `idMaterial` and in `trace_t` |
| `materialImage <img>` | 311 | **per-texel hit map** (`<name>_hit.tga` in 303 of 339 uses): a colour-coded image whose RGB maps to `materialType` entries, so `rvDeclMatType::GetMaterialType(tc)` can answer "what surface did this ray hit" per texel; load lazily on first trace query |
| `guide` / `inlineGuide` | ~1500 / 231 | **macro templates** from `guides/*.guide` (`generic_shader` alone 841 uses) — a preprocessor pass in the decl manager (§5.2) |
| `shaderParm <name> <src>` | 229 | GLSL-stage input binding; only inside `if glslPrograms == 1` stages → skip with the stage |
| `collision` | 130 | Raven collision-surface flag — content flag |
| `shaderTexture` | 109 | GLSL-stage texture binding → skip with the stage |
| `nomips` | 99 | stage image flag → no-mip path |
| `glass` | 72 | breakable-glass surface flag |
| `glslProgram "glsl/X.glsl"` | 55 | 1.4 GLSL stage; files mostly absent from the paks → `glslPrograms = 0`, skip |
| `notfix` | 15 | Raven flag of unknown effect — accept/ignore, log once |
| `portalImage`, `portalDistanceNear`, `portalDistanceFar` | 12 each | portal fade (pairs with the `.proc` IAP tuple and `idRenderWorld::RenderPortalFades`) |
| `snap` | 8 | `table` flag (Raven adds `snap` / `snapClamp` beside Doom 3's `clamp`); used by BSE envelope tables in `materials/tables.mtr` |
| `customLighting` | (guides) | GLSL-stage flag → skip with the stage |
| rarer, from the SDK `Material.h` / iddevnet | <2 each | `sky` (portal-sky surface), `needCurrentRender`, `highres`, content flags `vehicleclip`, `flyclip`, `largeshotclip`, `shotclip`, `projectileClip`, `sightclip`, `itemclip`, `notacticalfeatures`, surface flag `bounce`; `sort gui`; `fragmentParm` (8) / `vertexParm` (16, Doom 3 has 4); **blend modes `GL_SRC_COLOR`/`GL_ONE_MINUS_SRC_COLOR` as source and `GL_DST_COLOR`/`GL_ONE_MINUS_DST_COLOR` as destination** (illegal in Doom 3 — the RHI blend-state tables must accept them); expression ops `POTCorrectionX/Y`; decal life/spawn registers on `parm4`/`parm5`; `decalInfo` with comma-separated values |

Expression globals in `if` conditions: `glslPrograms` (30), `IsMultiplayer` (8),
`fragmentPrograms` (2, supported), `Parm5/6/7` (DUDE supports `parm0–11`; confirm case-
insensitive resolution in `ParseExpression`). Everything else (`blend`, `map`, `heightmap()`,
`addnormals()`, `vertexColor`, `texgen`, `cubeMap`, `cameraCubeMap`, `deform`, `sort`,
`spectrum`, `fogLight`, `guiSurf`, `decal_macro`, `forceOverlays`, `videoMap`, content flags)
is already accepted.

### 1.4 Sound-shader keywords DUDE does not accept

`volumeDb` (7860 — Raven's name for `volume` in dB), `shakeData <int> <string>` (6394 — a
precomputed screen-shake amplitude table, one char per sample), `frequentlyUsed` (446),
`voForPlayer` (141), `frequencyShift <min>, <max>` (96), `no_shakes` (93), `noRandomStart` (49);
flags in the SDK header also include `useDoppler`, `isVO`, `causeRumble`, `center`, `hilite`,
and `soundShaderParms_t` gains `frequencyShift`, `wetLevel`, `dryLevel`. Already
supported: `minDistance`, `maxDistance`, `description`, `onDemand`, `looping`, `global`,
`soundClass`, `no_dups`, `shakes`, `no_occlusion`, `omnidirectional`.

Found while building Q2 (`tools/quake4/q4_sound_census.py`): **distances are in map units**,
not Doom 3's metres (`maxDistance 1181` is 30 m; 600 is the commonest value) — both references
keep Raven's units; DUDE converts at the shader boundary instead, so the mixer stays metric.
**4805 of 6789 sample names have no extension** (`sound/null` is `sound/null.ogg`; a few are
WAV only). **Voice-over lives in `sound/vo_<language>/`**, English included, while shaders
say `sound/vo/…`. 539 named samples are not in the retail paks at all (cut content: `vo/mcc_2`,
`impact/bullet`, …). One retail shader is broken (`ambient.sndshd` `leadin_test`: a `volume`
with no value). Unused by the retail files, so not parsed: `useDoppler`, `causeRumble`,
`center`, `isVO`.

### 1.5 GUI keywords DUDE does not accept

From 301 `.gui`/`.guide` files: `textSpacing` (742+), `textStyle` (800+), `stopTransitions`
(1954+), `consoleCmd` (259), `backgroundHover` / `backgroundFocus` / `backgroundLine` /
`backgroundGreyed` (listDef skins), `itemHeight`, `tabTextScales`, `defineIcon`, and the
`namedEvent "x"` *script command* (DUDE has the `onNamedEvent` handler, not the command).
From the SDK/iddevnet GUI reference, also: events `onBackAction`, `onTabRelease`,
`onGainFocus`, `onLoseFocus`, `onSelChange`, relative `onTime +N`; commands `resetVideo`,
`nonInteractive`; **per-component colour variables** (`forecolor_r/g/b/w`, `matcolor_x/y/z/w`,
`backcolor_*`, `hovercolor_*`, `bordercolor_*` — `cursor.gui` sets `crossCombat_16::matcolor_x`
in its first lines) with `transition` able to target them; `alwaysThink`; and **text escapes**
`^c###` (RGB colour, e.g. the marine/strogg team colours) and `^i####` (inline icons registered
via `RegisterIcon`), which the text drawer must understand. Window types `graphDef`/`popupDef`
exist in the spec but no shipped GUI uses them (census: only the seven Doom 3 types).
Already present: `transition`, `resetTime`, `set`, `if/else`, `definevec4/definefloat`,
`onNamedEvent`, `onAction/Release`, `onEsc`, `onActivate`, `onEvent`, `choiceType/choices/
values/cvar`, `scrollbar`, `thumbShader`, `menugui`, `forceAspectHeight`, `noninteractive`,
`nowrap`, `textAlignY`, `tabVAligns`.

### 1.6 BSE effects — the census

1180 `effects/**/*.fx`, all text, grammar `effect <name> { size <r> <block>* }`:

- **Effect-level blocks:** `emitter` (2162), `spawner` (2532), `sound` (710), `decal` (164),
  `light` (72), `shake` (81), `delay` (65), `trail` (29), `doubleVision` (3), nested `effect` (3).
- **Particle types inside emitter/spawner:** `sprite` (2974), `line` (1023), `oriented` (464),
  `electricity` (138), `debris` (88), `model` (32), `linked` (4), `decal`, `light`, `sound`
  (`soundShader` 710). Emitter properties: `count`, `duration`, `locked`, `detail`, `constant`,
  `attenuateEmitter`, `attenuation`, `freqShift`, `volume`, `scale`, `density`, `particleCap`.
- **Particle properties:** `start` / `motion` / `end` envelope blocks (4953 / 4394 / 3983),
  `material`, `blend`, `gravity`, `persist`, `entityDef` (401 — debris spawns game entities),
  `generatedNormal` / `generatedOriginNormal` / `flipNormal` / `generatedLine`, `impact` (200 —
  secondary effect chosen by the hit surface's `materialType`), `fork` / `jitterRate` /
  `jitterSize` / `jitterTable` (electricity), `trailType/Time/Count/Material`, `tiling`,
  `shadows`, `specular`, `timeout`.
- **Parameters:** `size`, `fade`, `rotate`, `tint`, `position`, `velocity`, `length`, `angle`,
  `offset`, `acceleration`, `bounce`, `friction`, `remove`, `effect`. The iddevnet/OpenQ4
  spec adds rarer keys the census threshold hid: effect-level `cutOffDistance`; emitter `rate`,
  `inverseAttenuateEmitter`, `orientateIdentity`, `useMaterialColor`, `depthSort`,
  `calcDuration`, `channel`; particle `windDeviationAngle`, `trailRepeat`, `trailScale`,
  `lineHit`, `parentVelocity`, `useLightningAxis`; domain modifiers `surface`,
  `useEndOrigin`, `cone`, `relative`, `linearSpacing`, `attenuate`, `inverseAttenuate`, and a
  `model` domain. Envelopes are `idDeclTable`s (from `materials/tables.mtr`) plus rate/offset.
- **Spawn domains:** `point` (11889), `box` (11567), `line` (4881), `cylinder` (1165),
  `sphere` (453), `spiral` (14).
- **Envelopes:** `linear` (4068), `fastInSlowOut`, `cosine`, `exp_x2`, `random`,
  `linear_flicker`, `linear_nc`, plus ~30 named `table` decls (`sintable`, `fadetable`,
  `decalfade`, …) — the `table` decl DUDE already has.

Retail cvars: `bse_rateCost`, `bse_rateLimit`, `bse_scale` (1.4.2: `bse_rateLimit` default 3
"shows all shotgun pellet impacts").

### 1.7 What stays game-side (compiles from the SDK, not engine work)

`.def` files reference 90+ `spawnclass` values; Raven's are `rv*`: `rvClientAFAttachment`,
`rvVehicleTurret/Weapon/Thruster/Rigid/Static/Hoverpad/Light/Position/Spline/UserAnimated`,
`rvAITactical`, `rvSpawner`, `rvDarkMatterProjectile`, `rvMonster*`, `rvTramCar*`,
`rvSplineMover`, `rvAI*` tether/trigger helpers, `rvItemCTFFlag`, `rvViewWeapon`, `rvBurnHole`,
`rvPusher`, `rvClampDoor`, … plus the familiar `id*` set. Scripts (`scripts/*.script`,
`#include`-based, `idProgram`) are game-side too. idTech4A++'s `quake4/` game tree is 255 k
lines (ai 45 k, physics 26 k, gamesys 15 k, script 12 k, anim 10 k, vehicle 9 k, mp 8 k,
weapon 6 k, client 3 k) — this is the SDK, and none of it is engine work beyond §5.4.

---

## 2. Why this is the right next big milestone

- It is the **only** third-party idTech4 title with (a) released game source, (b) formats one
  revision from Doom 3, (c) no physics middleware (contrast Wolfenstein 2009, ruled out).
  Prey (2006) also has an SDK but its formats (portals, wall-walking, gravity) drifted further;
  Quake 4 first, and the title-profile mechanism (§5.1) is what makes Prey a follow-on.
- Every engine-side piece is **reusable infrastructure**: multi-version loaders, a guide
  preprocessor, a title profile, a generic effects runtime that later serves DUDE's own content.
- DUDE's enhancement stack (PBR classifier, parallax, tessellation, RT shadows/AO/GI, FSR2,
  presets) is content-agnostic; Quake 4's larger maps (24 MB `.proc` for `airdefense1` vs
  13 MB for `mars_city1`, 606 collision models in one file) are a better showcase than Doom 3.
- Two projects have already proven the engineering (§4): the unknowns are now *which* code can
  be reused under *which* licence, not *whether* it works.

---

## 3. Licensing and distribution strategy

Settle this before any SDK code is touched; it decides what lives in the DUDE repository.

**Facts (verified):**
- DUDE is GPLv3 (fork of dhewm3; id released Doom 3 under GPL).
- The Quake 4 *engine* source was never released. The Quake 4 *SDK 1.4.2* contains the game
  code (`game/` 90 entries, `mpgame/` 89), Raven's **idlib as source** (48 entries), and
  **headers only** for the engine (`framework/` 20 incl. `declLipSync.h`, `declPlayback.h`,
  `declMatType.h`, `DeclPlayerModel.h`; `renderer/` 9; `sound/` 1; `cm/` 1; `aas/` 2;
  `ui/` 2; `bse/` = `BSEInterface.h` only, 4.2 KB). Plus Maya import, examples, `readme.txt`,
  and `EULA.Development Kit.rtf`.
- That EULA is the **"QUAKE 4 SOFTWARE DEVELOPMENT KIT LIMITED USE LICENSE AGREEMENT"**: a
  non-exclusive, limited, **non-commercial** licence; §2 permits "New Creations" (mods) that
  "shall operate only with QUAKE 4" and forbids commercial distribution of them; §3 forbids
  reverse-engineering/decompiling the SDK and "derivative works based upon the Software" except
  as §2 allows; §5 permits distributing copies "free of charge for non-commercial purposes"
  with the agreement attached. **Not GPL, not GPL-compatible.**
- The retail assets are the user's own purchase, same posture as Doom 3's.
- **Provenance of the open BSE implementations:** jmarshall23 decompiled a Quake 4 debug build
  with Hex-Rays ("Quake4Decompiled", 2025; original account gone, re-uploaded by others).
  idTech4A++'s `raven/bse/` (10.8 k lines, Aug–Sep 2025) ports his "Quake4BSE" plus
  "Quake4Decompiled renderer source code" and carries GPL headers added by the decompiler's
  author; OpenQ4's `src/bse/` is reconstructed from Quake4BSE + Quake4Decompiled + x87
  disassembly of retail `Quake4.exe`. A GPL notice on decompiled proprietary code is not a
  licence grant; neither tree is safe to copy into DUDE.
- idTech4A++'s `LICENSE` lists `q4sdk: author: Raven Software` with no licence line and labels
  the whole project GPLv3; OpenQ4 keeps the SDK-derived game code in a **separate repository**
  (`themuffinator/openQ4-game`) under the SDK EULA with the RTF attached, documents its
  provenance (`docs/dev/game-source-provenance.md`), and states that no linking exception is
  asserted. OpenQ4's posture is the one to copy.

**Rules this plan follows:**
1. **Nothing SDK-derived is committed to the DUDE repository**: no game code, no header
   fragments, no Raven idlib (it is SDK source). The SDK interface headers are read for
   interoperability only. **FOSS sources may be used, with credit** (decision, §9): GPL code
   from idTech4A++ and OpenQ4 — loaders, fonts, GUI keywords, decl types, test tools — can be
   ported into DUDE with attribution in the commit message and in the credits section of the
   README. **Decompile-derived code** (the 2025 BSE/renderer integration in idTech4A++,
   OpenQ4's `src/bse/`, Quake4Decompiled) is **reference-only**: consulted when a behaviour
   cannot be found from the files and docs, then re-expressed in our own modern C++ — never
   copied, and the consultation is noted in the commit message so the provenance is auditable.
2. **The game library lives in a separate repository** (`dude-quake4-game`, working name):
   SDK `game/` + `idlib/` + the interface headers, DUDE's adaptation patches applied, the EULA
   RTF attached, README stating non-commercial terms. That is the §5-permitted form and the
   OpenQ4 precedent. It `#include`s DUDE's public headers and builds `q4base.so`/`.dll`
   against them. The DUDE repository ships only `tools/quake4/build-q4game.sh` that clones/
   builds it, and documentation.
3. The engine ↔ game boundary stays the existing `GetGameAPI` C ABI (`neo/framework/Game.h`),
   the plugin arrangement dhewm3 inherited and every Quake 4 mod used. It is the strongest
   available posture for a GPL host loading non-GPL game code, not a guarantee. Whether to
   ship a prebuilt `q4game` is the one item still open (default: **no**, build locally; revisit
   at Q8 packaging).
4. The "shall operate only with QUAKE 4" clause is read as: the game library is a mod for the
   Quake 4 *game data*; DUDE is an engine replacement the way Linux `quake4.x86` was. That is a
   reading, not a ruling — recorded as a risk (§8).

---

## 4. Reference implementations (what to take from where)

| Project | What it is | Status (verified 2026-10-15) | Use in this plan |
|---|---|---|---|
| **idTech4A++** (glKarin, `com.n0n3m4.diii4a`, GPLv3 engine) | dhewm3/Dante-derived idTech4 engine, Android-first with **desktop Linux/Windows CMake builds** (`bin/cmake_linux_build_doom3_quak4_prey.sh`; CI artefacts for linux_amd64 / win_x64); GLES2/3 or desktop-GL core; no Vulkan | Quake 4 since 2022-10 ("can play all levels, but some levels have bugs"); BSE working since v69 (2025-09, "still many bugs"); SP + MP + SABot bots; `q4game` built 64-bit by default | **Primary engine-side reference** for the *format* work: `#ifdef _RAVEN` sites in `RenderWorld_load.cpp` (14), `CollisionModel_files.cpp` (18), `CollisionModel_load.cpp` (36), `MapFile.cpp` (22), `AASFile.cpp` (11), `tr_font.cpp` (14), `Window.cpp` (30), `ListWindow.cpp` (24), `DeviceContext.cpp` (28), `Material.cpp` (26), `DeclManager.cpp` (18), `Session.cpp` (41); new decls in `raven/framework/` (lipsync 108 + 74 lines, playback 304 + 185, matType 79 + 53, playerModel 127 + 43); its `quake4/` tree shows which engine calls the game actually makes. Per-map fixes in its changelog seed the Q6 checklist (tram1 bridge GUI, process2 elevator GUI, process1 elevator crush, core1 Makron cameraView, waste blood pool, recomp water, airdefense1 ceiling lights, network2 beam). **Port with credit** (decision §9) everything that is format-derived GPL work; `raven/idlib/` is SDK source (EULA) → shim instead; `raven/bse/` and post-2025 renderer hunks are decompile-derived → reference-only, rewritten in our own code if consulted (§3 rule 1). |
| **OpenQ4** (themuffinator, GPL-3 engine) | Doom 3 + BFG lineage, Meson, SDL3, OpenGL default + experimental Vulkan, Win/Linux/SteamOS, VR; runs Quake 4 and *The Awakening* | created 2026-02, v0.13.2 (2026-09), "Beta", 1.6 k commits; own `src/bse/` with a **2026-10 retail-parity audit and `tools/tests/bse_retail_parity.py`**; `src/game`/`src/mpgame` under the SDK EULA | Licence-structure precedent (§3); GPL code usable with credit — in particular its **parity test tool** as BSE acceptance targets **[verify the test data's own derivation before relying on it]**; its `src/bse/` is decompile-derived → reference-only (§3 rule 1); its `docs/dev/` tree is the best written spec of the closed engine's behaviour and is safe to read: `quake4-bse-research-and-implementation-plan.md` (BSE grammar + retail quirks: 60 s particle clamp, colour byte wrap, cube-face order, `linearSpacing` extrapolation), `retail-q4-decl-parity-audit.md`, `aas-compiler.md`, `engine-capability-matrix.md`, `text-encoding-and-localization.md`, `ttf-font-system.md`, `savegame-compatibility-policy.md`, `vulkan-material-programs.md`. |
| **Quake4Doom** (jmarshall23 2021–22, re-uploads by Kasreyn / davnau) | SDK game code integrated with Doom 3 GPL engine, "visual parity except BSE" | original account gone; doors/menu bugs | Historical; shows the SDK-on-GPL-engine adaptation diff. |
| **gbQuake4** (atsb) | SDK merged into gbDoom3 | runs only as 32-bit; no BSE; menu non-functional | Nothing. |
| **Quake4Decompiled** (davnau re-upload) | Hex-Rays output of a Quake 4 debug build | "reference only, will not compile"; no licence; pre-1.1 build with known reconstruction errors (per OpenQ4) | **Reference of last resort** (decision §9): read only to resolve a specific hard-to-find behaviour, never copied; prefer OpenQ4's written audits, which already digest it. |
| **SDK 1.4.2 mirrors** | `Xanz/Quake4SDK` (CMake, Linux+Windows; Raven's edits bracketed `// RAVEN BEGIN … END`, which makes header diffs mechanical), `FriskTheFallenHuman/quake4sdk` (mirror + CMake), `skrunkgy/Quake4Linux` (+ fetches the 1.4.2 Linux binary), `glKarin/idtech4amm_quake4_sdk`; iddevnet reference pages mirrored at `iddevnet.dhewm3.org/quake4/` (`Quake4SDK`, `LinuxSDK`, `FXEditor`, `Basic_FX_file_structure`, `The_Properties_of_a_segment`, `Notes_about_Envelopes/Tables`, `ArtReference_Q4Shaders_*`, `GUIEditor`, `GUI_Variables`, `Font_Information`, `Declarations`) | id's FTP is dead; iddevnet docs mirrored at `iddevnet.dhewm3.org/quake4/` (SDK page, Linux SDK page: "compile by running scons in source/") | Source of the game code for the separate repository (§3 rule 2); interface headers for §5.4. |
| **Linux client 1.4.2** (`quake4-linux-1.4.2.x86.run`, mirrored by Lutris) | id's 32-bit native client | needs Windows game data + key | Retail reference on Linux without Proton, if the user prefers. |

---

## 5. Architecture: how Quake 4 fits into DUDE

### 5.1 Title profile (engine, GPL)

Today `BASE_GAMEDIR` is the compile-time literal `"base"` (`neo/framework/Licensee.h:69`),
used ~20 times across `FileSystem.cpp`, `Common.cpp`, `Session.cpp`, `sys/linux/main.cpp`,
plus `"d3xp"` special-cases in `FileSystem.cpp` (mod list, `pak000.pk4` probing, expansion
detection) and `Session_menu.cpp`. Quake 4's base dir is `q4base`, its config
`Quake4Config.cfg`, its cvar defaults differ (`r_varmegs`, `bse_*`, `g_playerLean`, `si_fps`
90, …), and it has no `d3xp`.

Introduce a **runtime title profile** (as built: `gameTitleProfile_t` in
`neo/framework/GameTitle.{h,cpp}`), selected by
`+set com_title quake4` or auto-detected from the pak set at `fs_basepath`
(`q4base/pak001.pk4` present → Quake 4). Raven's own constants, for reference: `BASE_GAMEDIR
"q4base"`, `BASE_MPGAMEDIR "q4mp"` (since 1.4 MP runs as `fs_game q4mp`), `CONFIG_FILE
"Quake4Config.cfg"`, `DEFAULT_BASE_PATH "/usr/local/games/quake4"`. The profile also owns the
**entity-filter map command** (`map game/process1 first` → `si_entityFilter`, passed to the game
and stored in saves) and the engine-selected loading screens (`guis/loading/generic.gui` for SP).

**As built in Q1:** the profile carries the base game folder (which is also the base game
library stem: `base.so` for Doom 3, `q4base.so` for Quake 4, so no separate name is needed),
a probe file for autodetection (`base/pak000.pk4`, `q4base/pak001.pk4`), whether the Doom 3
`base/` + `d3xp/` + `demo/` conventions apply (RoE detection, the demo fallback, the mod
list), whether a missing game library falls back to the null game (Quake 4: yes, Doom 3: no),
and the engine-owned asset names that differ per title: console charset (`textures/bigchars`
vs `gfx/2d/bigchars`), splash (`splashScreen` vs `gfx/splashScreen`), GUI fill
(`guis/assets/white.tga` vs `_white`), GUI cursor/scrollbar art (`ui/assets/` vs `gfx/guis/`),
default GUI font (`fonts` vs `fonts/chain`, OpenQ4's choice), stats-overlay font (`fonts/bank`
vs `fonts/profont`) and loading screen (`guis/map/loading.gui` vs `guis/loading/generic.gui`).
Config and saves need no renaming: both are already per game folder, so Quake 4 lands in
`~/.config/dude/q4base/dude.cfg` and `~/.local/share/dude/q4base/`. Selection happens once in
`idFileSystemLocal::Init`, before the per-OS base-path probe, which now looks for the active
title's folder. Later phases add the decl folders/types (Q2), default cvar overrides and the
"faithful floor" preset (Q7). Doom 3 keeps profile `doom3` with all current
behaviour — **zero change for existing users**. idTech4A++ does this with compile-time
`_RAVEN`/`_QUAKE4` macros and separate binaries; the user's decision (§9) is **one `dude`
binary for every title**: one executable, one settings system, the profile chosen at startup,
and only the game library differs per title (as `d3xp.so` / `d3le.so` already do).

### 5.2 Decl manager and parsers (engine, GPL)

**As built in Q2** (branch `feat/quake4-decl-parity`; everything Quake 4-only is keyed off
the title profile, so a Doom 3 run parses exactly as before):
- *Guides* — `neo/framework/DeclGuides.{h,cpp}`: templates from `guides/*.guide`, expanded in
  `.mtr` text before parsing, keeping the template's own lines (the material parser is
  line-sensitive: `qer_editorimage` skips the rest of its line, so a one-line expansion lost
  every guide material: the cause found for the first gate run's 1507 material lint failures)
  with a line map back to the file for decl line numbers; 1398 guide-defined materials. Checked offline byte for byte against an independent Python
  expansion (`tools/quake4/test_guides.sh`). The profile also turns on recursive decl folders:
  34 of the 96 `.mtr` files (all effect materials) live in `materials/<sub>/`.
- *Materials* — every keyword the retail `.mtr` files use parses
  (`tools/quake4/q4_material_census.py` lists none left): `materialType`, `materialImage`,
  `portalImage`, `portalDistanceNear/Far`, `sky`, `needCurrentRender`, stage `nomips`; content
  and surface bits at Raven's numbers (all free in Doom 3's layout). Raven's GLSL stages
  (`glslProgram` / `shaderParm` / `shaderTexture`) are parsed and dropped: each has a
  `glslPrograms == 0` fallback stage, and most of the `.glsl` files they name are not in the
  paks. `glslPrograms`, `IsMultiplayer`, `DecalLife`, `VertexRandomizer` evaluate to 0.
  Raven's image-program function `downsize( <image>, <levels> )` (404 uses, also in
  `materialImage`) loads the image unchanged — Quake 4 at its highest texture quality; the
  short `decalInfo <stay>[, <fade>]` forms, a table's trailing comma and `gl_src_color` as a
  source blend factor (`GLS_SRCBLEND_SRC_COLOR`, GL3 + Vulkan) are accepted. Left failing, as in
  retail: Raven's test materials in `test.mtr` (`POTCorrectionX`, `watermove`, `fp20Program`),
  `guisurf entity4..6` (Quake 4's own `MAX_RENDERENTITY_GUI` is 3 too).
- *Decl types* — `materialType` (`.mtt`), `lipSync`, `playback`, `effect` (brace-checked
  only; the BSE grammar is Q4) and `playerModel`, in `neo/framework/DeclQuake4.{h,cpp}`, with
  enum values appended after Doom 3's. The null game registers the game-side types and folders
  (`model`, `export`, `camera`, `def`, `af`) so `lintDecls` covers entity defs too.
- *GUIs* — Raven's script language and keywords (see §1.5), `tools/quake4/q4_gui_census.py`.
- *Sound shaders* — the keywords of §1.4; distances converted from map units to metres
  (profile `soundUnitsToMeters`); bare sample names and `sound/vo/` → `sound/vo_<sys_lang>/`
  (falling back to English for a line a language pack does not voice) under profile flags;
  `noRandomStart` honoured by the emitter; `shakeData`, `frequencyShift`, `frequentlyUsed`,
  `voForPlayer`, `no_shakes` stored on `idSoundShader` for the Q5 game library (the shake
  envelope replaces Doom 3's WAV amplitude scan, which OGG samples cannot feed).
- *EFX* — profile `reverbPresets` = `efxs/default.efx` (23 EAX presets, same grammar as Doom
  3's `.efx`) loaded for every map, plus `maps/<map>.reverb` (`idMapReverb`, area → preset)
  consulted first when the listener's area changes; `Generic` is the fallback (Quake 4's
  library has no `default`). All 50 retail tables resolve (`q4_sound_census.py`).
- *`lintDecls [type] [all]`* — parses every decl and lists the ones that fail; images and
  sound samples only register while it runs.

Open after Q2 (each lands with the phase that consumes it): the `^c###` / `^i####` text
escapes, render-window multi-model and `onTabRelease` (GUI, Q5/Q9); per-decal `DecalLife`
(renderer, Q5); `setLightColor` reaching a render light (Q5); the shake envelope and
`frequencyShift` pitch in the mixer, and what plays for the 539 missing samples (Q5/Q6); the
decl-manager API the game calls (`FindEffect`, `FindMapDef(map, filter)`, playback data) (Q5);
the settings page's engine-filled lists (`gui::4_3_choices` / `16_9` / `16_10`,
`gui::aspect_choices`, `gui::device_name`), the Doom 3 art paths the engine's menu code sets
(`loadgame_shot` = `guis/assets/blankLevelShot`, `browser_levelshot` = `guis/assets/splash/pdtempa`;
Quake 4 keeps its level shots in `gfx/guis/loadscreens/`) and the Raven-named engine cvars it binds
(`s_volume`, `s_musicVolume`, `s_speakerFraction`, `s_deviceName`, `s_useOpenAL`,
`r_aspectRatio`, `r_skipSky`, `r_forceAmbient`, `r_useSmp`, `in_freeLook`) — the game cvars
on that page (`si_*`, `ui_*`, `g_*`) come with the game library (Q5); `deform rectSprite`
billboards like `sprite` (two multiplayer materials, Q9).

Engine strings: Quake 4 ships the engine's strings as `#str_1` + Doom 3's digits
(`#str_104343` LOADING...); `idLangDict::GetString` tries that form after a miss. Measured
2026-10-24 (`tools/quake4/check_engine_strings.py`, after the Q3 map sweep warned of
`#str_04344`/`04345` on every map): of the 223 ids the engine uses, 148 read the same either way and
~30 differ only in wording (`Quake 4 is up to date…`, `LEFT ALT`, keypad keys without `KP_`), but 7
engine ids (two loading messages, network / CD-key lines) and 35 control names of DUDE's settings
menu are in neither form, and 3 renumbered ones are different messages (`#str_107232`/`07233`
"EAX Reverb" for the EAX errors, `#str_104304` a different CD-key message).
`idCommonLocal::AddEngineStringFallbacks` adds English for those after the tables load - only
where neither form exists, or under Doom 3's id for the 3 misnumbered ones - so Quake 4's own
(localised) text is used everywhere else; Doom 3 gains only the two RoE-only ids. Quake 4's
`default.cfg` binds `LEFTCTRL` / `RIGHTSHIFT` …, read as aliases of Doom 3's key names.

- **Guide preprocessor.** `guide <material> <macro>( args… )` and `inlineGuide` expand
  templates from `guides/*.guide` (up to 20 parameters) into decl text before parsing (DUDE's
  decl manager has no notion of guides; the SDK exposes it as `ParseGuides` /
  `EvaluateGuide` / `EvaluateInlineGuide`). Text expansion with argument substitution in
  `idDeclManagerLocal::ParseFile`; cache expanded text per decl for `reloadDecls`.
- **New decl types:** `materialType` (`materials/types`, `.mtt`), `lipSync` (`lipsync/`),
  `playback` (`playbacks/`), `playerModel`, `camera` (`DECL_CAMERADEF`), `effect`
  (`effects/`, `.fx` — the profile swaps Doom 3's `fx/` + `idDeclFX` mapping for this).
  Decl-manager API additions the game calls: `FindEffect`, `EffectByIndex`, `FindMapDef(map,
  filter)`, `FindType(…, noCaching)`, playback data (`GetPlaybackData`, `SetPlaybackData`,
  `FinishPlayback`).
- **Material keywords** per §1.3; `glslPrograms` / `IsMultiplayer` expression globals;
  `materialType` index on `idMaterial`, surfaced through `trace_t` (§5.4).
- **Sound shader keywords** per §1.4 (`volumeDb` → `volume`; `shakeData` → shake table).
- **GUI keywords** per §1.5 plus the UI API additions (§5.4); the `^c###` / `^i####` text
  escapes in `idDeviceContext::DrawText` and `idStr` helpers (`LengthWithoutEscapes`,
  `RemoveEscapes`) — clean-room, they are one-line regex-class rules.
- **Fonts:** `.fontdat` loader beside `.dat` (`tr_font.cpp`), selected by profile; one image
  per font. Needed on day one — the console draws with it.
- **EFX:** per-map `.reverb` (area → preset in `efxs/default.efx`) into the existing
  `idEFXFile` path; sound API `SetReverb/GetReverb/GetReverbName/GetNumAreas`.

### 5.3 World loaders (engine, GPL)

Version-tolerant parsing, both formats through one code path:
- `.map` — accept `Version 3` in `idMapFile::Parse`; nothing else changes in the parser.
  **Entity filter, as built (Q3, 2026-10-23):** `process1` and `storage1` each hold two levels;
  an entity marked `filter_first "1"` / `filter_second "1"` belongs to one (process1: 203 / 12),
  and each level has its own `info_player_start`. `map game/process1 first` (also `devmap`, the
  addon-pak reload, and a game-issued `map <next> <filter>` from `target_endLevel`'s
  `entityFilter`) sets serverinfo `si_entityFilter` - the Quake 4 game skips an entity with any
  `filter_*` key unless it names `filter_<filter>` (SDK `idGameLocal::InhibitEntitySpawn`), and the
  session finds the level's mapDef as `<map>_<filter>` (`idSessionLocal::FindMapDef`: name, load
  sizes, autosave title; autosave files get `_<filter>`). Gated on the title profile's
  `entityFilters` (Doom 3: off - no key, unchanged lookups). The null game applies the same rule to
  its dressing and its player start. Also from the mapDef: Quake 4's loading screen
  (`guis/loading/generic.gui`) gets `loading_bkgnd` = `loadimage` and `loading_levelname`, which
  DUDE never set (every Quake 4 map loaded on a blank screen); only a mapDef with a `loadimage`
  does that, so Doom 3's loading GUIs are untouched. **Quake 4 inline statics stay solid**
  (SDK `idStaticEntity::Spawn`: "inline models can be solid") - unlike Doom 3, where `Hide()`
  zeroes their contents - so terrain dmap merged into the world keeps its own `.cm` collision
  (Q5's game does this itself; a null-game floor drop that left them out sank the intro
  marines of `airdefense1` onto a buried brush and was reverted).
- `.proc` — accept `PROC "4"` + CRC; vertices with 8 **or** 12 floats (store the RGBA in
  `idDrawVert::color` — DUDE already consumes per-vertex colour for terrain blends); IAP fade
  tuple into `portal_t` (`fadeImage`, `near`, `far`) for `RenderPortalFades`. Measured
  2026-10-23: 9 of the 3527 portals in the 63 retail maps fade (`mcc_1` 6, `mcc_2` 3; near/far
  150/200 and 256/512, black) — a designer effect on chosen doorways, not a draw-distance
  system. **Assessment (2026-10-23, on the user's question whether Doom 3 should get them as a
  performance step): Quake 4 only** —
  Doom 3's maps carry no fade data, a global cut-off would black out doorways (a fidelity
  change), portal culling already drops what cannot be seen, and the RT tiers trace the whole
  world regardless of portals. **As built (Q3, 2026-10-23):** `idRenderWorldLocal::AddPortalFade`,
  called from the view flood (`FloodViewThroughArea_r`) for a visible portal with fade data:
  by the view's distance to the portal plane, transparent up to `distanceNear`, opaque from
  `distanceFar` (alpha ramps between), drawn as a translucent quad over the whole portal with the
  fade image's implicit material (`blend blend`, `colored`: alpha from `shaderParm3`; no game
  defines a `_black` material), once per view, both windings. Past `distanceFar` the flood
  stops at the portal - it is opaque, nothing behind it can show. Doom 3 portals have no fade
  data, so the path never runs for them. Behaviour as idTech4A++ (its routine is compiled out)
  and OpenQ4 read it - read for the semantics only. **Draw fixes (2026-10-23, `fix/quake4-portal-fade-draw`):**
  (1) the flood runs before `R_AddModelSurfaces`, which restarts the view's surface list, so the
  quad is recorded on the view (`viewDef_t::portalFades`) and added by `R_AddPortalFadeSurfaces`
  right after the models; (2) `_black` draws with `_portalFade`, an opaque white built-in tinted
  black by the parms (DUDE's `_black` has alpha 0, `_white` is sorted `SS_GUI` by the GUI, `_flat`
  has alpha 128 unless `image_useNormalCompression 1`); (3) every retail fade portal has a
  `sort decal` glass pane in its plane (`textures/glass/marineglass*`: alpha-mask stage, then the
  cubemap added by destination alpha), and retail shows that reflection over the darkened view
  (user screenshots at `setviewpos -1536 -71 160 90`, 175 units: half-dimmed; `-1536 -116 160 90`,
  220: a dark pane with the smudged reflection). `_portalFade` is therefore sorted `SS_DECAL - 0.5`,
  between the opaque world and the decals, below `SS_DECAL` so the decal-only paths (polygon
  offset, blood film, probe `skipDecals`) and the SSR split (`> SS_DECAL`) see it as opaque-side.
  Known limit: decals and translucents beyond a partially faded portal draw after it and are not
  dimmed; once the fade is opaque the flood no longer reaches them.
  **The pane itself never drew (2026-10-23, `fix/flat-entity-portal-plane-link`), an idTech 4 bug
  that Doom 3 shares:** the window's glass (`func_static_53722`, a zero-thickness patch) lies
  exactly in the portal plane (y 104). `PushVolumeIntoTree_r` counts distance 0 as the front side,
  so the pane was linked only into the area behind the portal (22; the viewer's corridor is 24),
  and from 24 that area is seen through a portal stack whose last plane is the portal itself, where
  `R_CornerCullLocalBox` culls a box with no corner at distance < 0 - the pane was culled even with
  the room visible, and past `fadeFar` that area is not flooded at all. `R_CreateEntityRefs` now
  gives a flat axis a quarter unit of thickness for the area links only (culling keeps the true
  bounds), so the pane is also referenced from the area it faces. Scan of the shipped maps (flat
  inline entity in the plane of an axial portal it overlaps): Quake 4 9 (the 7 fading windows of
  `mcc_1` / `mcc_2`, 2 steel panels in `dispersal`); **Doom 3 22** glass panes (alphalabs3/4, cpu,
  delta1-4) and **RoE 45** (44 in phobos2, mostly breakable `func_fracture` windows) - all culled
  from one side in vanilla too. Separately `r_decalMatte` had been stripping the reflection of
  `sort decal` glass on GL3/Vulkan (`fix/decal-matte-spares-glass`, readme-changes §1.5).
- `.cm` — **as built (Q3, 2026-10-23)**, one parser for all titles (`neo/cm/CollisionModel_files.cpp`),
  the extra fields read by file revision (`quake4FileRevision`) rather than guessed from the next
  token: `"1.00"` Doom 3; `"3"` Quake 4 maps and models — a number after the model name, two-count
  block headers (`numPolygons numPolygonEdges`, `numBrushes numBrushPlanes`, turned into the same
  byte reservation Doom 3's single number gives), per polygon three `( s t )` pairs plus a
  primitive number, per brush a primitive number (kept in `cm_brush_t::primitiveNum`, Doom 3's
  own field); `"2"` — four models of the last patch (pak019) — texture pairs only, byte-count
  headers. The trailing integer is **not** a surface/material-type id as first assumed: in
  `airdefense1` it runs 0–10233 and the number after `worldMap` is 10234, exactly the
  worldspawn + `func_group` primitive count of the `.map`. **The texture pairs** (measured
  2026-10-23, `tools/quake4/q4_cm_texcoords.py`, against the brush side each polygon's primitive
  names): the face's exact texture coordinates at three points of its plane - but the points
  themselves are not stored. For 84% (`airdefense1`) / 78% (`mcc_1`) of brush polygons they are
  three of the vertices, always a diagonal pair plus a third corner of a quad in no fixed order
  ((0,2,1), (1,3,0), (0,2,3), (1,3,2) …); for the rest they lie inside the polygon, points of a
  piece the compiler merged away. So the file cannot place a hit-map (`materialImage`) sample
  for every polygon, and Raven's closed engine is the only source of how it did (decompile-only,
  §3 rule 1). They are read past: a trace's material type is the polygon material's own
  `materialType`, as idTech4A++ does (it calls the pairs "unknown" and drops them). What that
  loses is small: of the 1.45 M polygons of the 29 campaign `.cm` files, 14 800 (1%) use a
  hit-map material - 12 056 of them one material, `textures/terminal/t_window_alpha_02` - while
  67 840 have a whole-material type and the rest none. Raven's contents names join `cm_contentsNameByIndex` after
  Doom 3's (`sightclip` … `projectileclip`, and `shotclip`: "block bullets but not players or
  monsters", baked into 22 brushes of 8 MP maps, so `shot_clip`'s material keyword now sets
  `CONTENTS_SHOTCLIP` = bit 25 too). A Quake 4 file's CRC is taken as current (Raven's tools
  wrote it; a mismatch would make the engine rebuild collision from the `.map`). Self-check: a
  two-count block header that disagrees with the rows read prints a warning naming the file
  and model (`CheckBlockCounts`; none over the retail files, and a header bumped by one in a
  copy of `airdefense1.cm` is reported).
  **Verification:** `tools/quake4/test_cm_parse.sh` runs the loader's own code (copied out of
  `neo/cm/` verbatim) with the real `idLexer` over every retail file — Quake 4 944/944,
  Doom 3 + RoE + LM 112/112, no warnings; `q4_cm_check.py` is the Python replica. A negative
  control (an earlier lookahead version of the brush trailer, which `UnreadToken` after a
  `PeekTokenString` silently broke) fails 47 of 83 Doom 3 base files, so the test catches that
  class of bug. **Doom 3 finding:** Doom 3's byte-count headers were written by 32-bit builds,
  whose `cm_polygon_t`/`cm_brush_t` are 4 bytes smaller, so on 64-bit the last rows of most
  models miss their block and were allocated one by one but never freed (`FreePolygon` /
  `FreeBrush` skipped every row of a model with a block) — ~12 MB over all Doom 3/RoE/LM
  files, up to 387 KB per map load (`le_enpro2`); fixed on its own branch
  (`fix/cm-block-fallback-leak`).
- `.aas` — **as built (Q3, 2026-10-23)**, one loader for both (`neo/tools/compilers/aas/AASFile.cpp`),
  the additions read by file revision: `DewmAAS "1.08"` adds the settings key
  `generateTacticalFeatures`, two numbers per area row (the area's run in the feature index:
  `numFeatures`, `firstFeature` - their sum is the index size and the runs follow each other in all
  28 files that have features) and the sections `featureIndex` (as the other indexes) and
  `features` (a row holds flags, height, normalx, normaly, x, y, z; Raven's 12-byte
  `aasFeature_t` adds an unstored weight; the order is what the data shows - height 72/48, normal
  bytes 0/127/254 - and what idTech4A++'s loader reads). Measured over the 150 retail files (30
  maps x `aas32/48/96/128/250`): 81 dummies (settings, every section empty), features only in the
  `.aas32` files (28), no file has a section or key beyond these. **ABI:** `aasArea_t` and the
  members Doom 3 game libraries read inline are unchanged - the feature data lives in lists
  appended after `settings` (`areaFeatures`, `featureIndex`, `features`) with inline accessors
  named as Raven's (`GetNumFeatures`, `GetFeature`, `GetNumFeatureIndexes`, `GetFeatureIndex`,
  plus `GetAreaFeatures` for the per-area run) and `IsDummyFile()` (no areas: a real file always
  has area 0; Raven's game frees a dummy at once). `generateTacticalFeatures` is appended at the
  end of `idAASSettings`, `idAASFile`'s last member. Raven's own `idAASFile` is a fully virtual
  interface with a different ABI: adapting it is Q5's. A Quake 4 file's CRC is taken as current,
  as for `.cm`. Fixed on the way: the tree-depth report walked node 1 of a file without nodes (a
  dummy). Self-check: an area run off the index, a gap, or an index entry past the features
  prints a warning. The null game loads every `aas_types` size per map and logs one line each.
  **Verification:** `tools/quake4/test_aas_parse.sh` compiles the loader's own `AASFile.cpp` +
  `AASFile_sample.cpp` with the real `idLexer`: Quake 4 150/150, no warnings (7422 features,
  21402 index entries); Doom 3 + RoE + LM 83/84 with totals identical to master's loader (the one
  refusal is `maps/testmaps/test_boxstack.aas48`, version 1.06, refused before too); a shifted
  feature run in a copy of `airdefense1.aas32` is reported.

### 5.4 Engine interfaces the game library needs (engine, GPL)

Verified from the SDK-mirroring headers in idTech4A++ (`_RAVEN` blocks) and the SDK's
`game/Game.h`:

- **Game API:** `GAME_API_VERSION = 37` (DUDE 9, Doom 3 8). `idGame` 101 virtuals (DUDE 44),
  `idGameEdit` 122 (DUDE 55). Q4-only `idGame` methods fall into: client-side effects
  (`PlayEffect`, `StartViewEffect`, `SpawnClientMoveable`, `ClientRun`, `ClientEndFrame`),
  player/menu glue (`GetPlayerInfo/Name/Clan/View`, `SetPlayerInfo`, `InCinematic`,
  `IsMultiplayer`, `MenuFrame`, `Translation`, `SetupLoadingGui`/`GetLoadingGui`,
  `FlushBeforelevelLoad`, `UpdatePlayerPostMainMenu`, `SetViewComments`), and a large MP block
  (Q4TV repeater `Repeater*`, server demos, ban list, rcon, `HTTPRequest`, `GetBotInput`,
  `ValidateDemoProtocol/ServerSettings`, debug stats) that v1 stubs. DUDE-only methods
  (`SelectTimeGroup`, `GetTimeGroupTime`, `GetBestGameType`, `GetMapLoadingGUI`) get defaults.
  Several shared methods also **changed signature** in Raven's version: `InitFromNewMap(map,
  renderWorld, isServer, isClient, randSeed)` and `InitFromSaveGame(map, renderWorld, file)` take
  no `idSoundWorld*` (Raven removed the sound world as a game-facing object — see Sound);
  `RunFrame(cmds, activeEditors, lastCatchupFrame, serverGameFrame)`; `SaveGame(file,
  saveType)` with `ST_REGULAR/QUICK/AUTO/CHECKPOINT`; `SetUserInfo` without `canModify`;
  `ServerAllowClient`, `ServerWriteSnapshot` (`dword*` PVS), `ClientReadSnapshot`,
  `ClientPrediction(…, ClientStats_t*)`. `gameImport_t` gains `rvBSEManager *bse`;
  `gameExport_t` gains `rvGameLog *gameLog`.
  **Policy:** DUDE's `idGame`/`idGameEdit` become the **union**, Q4-only entries with default
  no-op implementations, `GAME_API_VERSION` bumped; the in-tree Doom 3 libs recompile
  unchanged. Where signatures differ, DUDE keeps its own and the game patch set adjusts the
  Q4 overrides (mechanical, a dozen methods); where the *engine-provided* API differs (sound
  by `worldId`, collision by object handle, playback data), DUDE adds **Raven-style facades**
  on the engine side so the game code compiles with minimal patching. One header, one vtable.
- **Render world:** `renderEntity_t` gains `referenceSoundHandle`, `suppressSurfaceMask`,
  `shadowLODDistance`, `suppressLOD`, `overlayShader`, `weaponDepthHackInViewID`;
  `renderLight_t` gains `detailLevel`, `noDynamicShadows`, `globalLight`,
  `referenceSoundHandle`; new `renderEffect_t` (`declEffect`, `materialType`, …) and
  `idRenderWorld::AddEffectDef / UpdateEffectDef / StopEffectDef / FreeEffectDef /
  GetEffectDef / EffectDefHasSound`; `HasSkybox(area)`, `FindVisibleAreas`,
  `RenderPortalFades`, extra `Debug*` overloads, `RenderScene(view, renderFlags)`; `trace_t`
  gains `materialType`. Policy: **append**, Doom 3 libs unaffected.
- **Sound:** the game never sees an `idSoundWorld`; `idSoundSystem` takes a `worldId` and
  emitters are integer handles: `AllocSoundEmitter(worldId)`, `EmitterForIndex`,
  `GetSoundWorldFromId`, `FreeSoundEmitter(worldId, handle, immediate)`, `StopAllSounds(worldId)`,
  `PlayShaderDirectly(worldId, …)`, `Write/ReadFromSaveGame(worldId, file)`, `FadeSoundClasses(worldId, class, to, over)`, `PlaceListener(origin, axis,
  listenerId, gameTime, areaName)`, `CurrentShakeAmplitudeForPosition`, reverb
  (`SetReverb/GetReverb/GetReverbName/GetNumAreas`), `ValidateSoundShader`, `EndCinematic`,
  and a voice-chat block (`EnableRecording`, `GetVoiceData`, `BufferVoiceData`,
  `MixVoiceData`, `GetNumVoiceChannels`) that v1 stubs; `idSoundEmitter::UpdateEmitter(origin,
  velocity, listenerId, parms)`, `StartSound(shader, channel, diversity, flags)` (no
  `allowSlow`), `CurrentAmplitude(channelFlags, factorDistance)` (feeds lipsync and
  sound-reactive lights), `Handle()`. The handle-based API is a thin facade over DUDE's
  pointer-based one; `SND_CHANNEL_*` adds `POWERUP`, `POWERUP_IDLE`, `MP_ANNOUNCER`, `CUSTOM`.
- **Collision:** handles are `idCollisionModel*` objects, not `cmHandle_t` ints, and are
  map-name-scoped — `LoadModel(mapName, modelName)`,
  `PreCacheModel`, `ModelFromTrm(mapName, …)`, `TrmFromModel(mapName, …)`, `FreeMap(mapName)`,
  `DrawModel`, `ModelInfo`, `LoadMap(mapFile, forceCreateMap)`; an `idCollisionModel`
  accessor interface (`GetName/GetBounds/GetContents/GetNumPolygons/GetPolygon/
  GetPolygonMaterial/GetBrushPlane/…`); `materialType` in trace results (the 606 named
  models per `.cm` are why). **Scoped to Q5 (2026-10-23):** all of it is game-facing, so it is
  built with the game that calls it; Q3 found nothing more for the engine to read (the
  per-polygon texture pairs cannot drive a hit map, §5.3 `.cm`), so the trace's `materialType`
  is the material's own. It must not grow `contactInfo_t` for Doom 3's libraries (game DLLs
  hold `contactInfo_t` arrays for `Contacts()`): a Quake 4-only result type behind the facade.
- **UI:** `RegisterIcon(code, shader, …)` (the `defineIcon` keyword), `SetInteractive`,
  `GetMaxTextIndex`, `GetLightColor`, `SetStateVec4`, `ClearState`, `DeleteState`,
  `GetDesktop`; manager `RunAlwaysThinkGUIs`, `ClearGameGuis`, `FlushGUIs`; session-side menu
  commands `CVarStrcmp`, `main <namedEvent>`, `play <snd>`, `disconnect [menu [event]]`.
- **Render model:** `idRenderModel::GetSurfaceMask(name)` + `InstantiateDynamicModel(…,
  surfMask)` back `renderEntity_t::suppressSurfaceMask` (hide meshes by bit); `SetHasSky`.
- **Common / filesystem / sys:** localisation (`GetLocalizedString(token, langIndex)`,
  `GetNumLanguages`, `LanguageHasVO`), `GetUserCmdHz/MSec`, `IsRenderableGameFrame`; file
  helpers (`RemoveFile`, asset log); joystick events. Mostly thin.
- **Decl manager:** as §5.2.
- **idlib:** the game must compile against **DUDE's** idlib (`idStr`, `idDict`, `idList`,
  `idBounds` cross the ABI). Raven idlib extras the game uses — `rvMemSys` (map to
  `Mem_*`), `Radians`, `Interpolate`, `Pair`, `ListGame`, `AutoPtr`, `LexerFactory`,
  `TextCompiler`, `AutoCrit` — are small; shim trivially in the game repo or add clean-room
  GPL equivalents to DUDE's idlib. Also: `Mem_Alloc16(size, tag)` allocation tags (shim to
  ignore the tag), `idStr` escape helpers, `idVec4::operator int()` RGBA packing, extra
  `idVec3`/`idBounds` helpers. **No data-member changes** to `idVec*`, `idBounds`, `idDrawVert`
  — vertex and vector layouts match Doom 3. Never copy the SDK idlib into DUDE.

### 5.5 BSE — Raven's effects runtime (engine, GPL, the long pole)

Design the way `idDeclParticle` + `idRenderModelPrt` already work: a decl (`rvDeclEffect`,
parsed from the §1.6 grammar into segment templates) and a runtime (effect instances owned
by a manager; each frame builds dynamic triangle surfaces per particle type and submits them
through a `renderEntity_t` with a callback, like particle entities). Particle types map onto
three geometry kinds — camera-facing quads (`sprite`), stretched quads along a vector
(`line`, `electricity` with jittered polylines, `linked`, `trail`), oriented quads/models
(`oriented`, `model`, `debris` = model + physics callback into the game) — plus three
non-geometry kinds (`light` → `renderLight_t`, `sound` → emitter, `decal` → the existing
decal path). Envelopes are 1-D curves (named functions or `table` decls) over normalised
life. `impact` + `materialType` select secondary effects; `entityDef` on `debris` spawns a
game entity through a game callback; `shake` / `doubleVision` are view-effect requests to the
game (`StartViewEffect`). Rate limiting mirrors `bse_rateLimit`/`bse_rateCost`.

**Clean-room inputs (and only these):** the census and the 1180 files themselves; the SDK's
`BSEInterface.h` (interface for interoperability: `rvBSEManager` with `PlayEffect`,
`ServiceEffect`, `StopEffect`, `FreeEffect`, `EffectDuration`, `CheckDefForSound`,
`Begin/EndLevelLoad`, `Start/EndFrame`, `Filtered`, `CanPlayRateLimited`; `renderEffect_t`
with `declEffect, startTime, origin, axis, gravity, endOrigin, attenuation, loop, ambient,
referenceSoundHandle, shaderParms[]`; view effects `DOUBLEVISION/SHAKE/TUNNEL`; cvars
`bse_enabled, bse_render, bse_debug, bse_showBounds, bse_physics, bse_debris, bse_scale,
bse_singleEffect, bse_maxParticles`); the iddevnet effects-editor pages (segment properties,
envelopes/tables — editor-centric, no literal `.fx` samples); OpenQ4's written BSE spec and
retail-quirk notes (prose, not code); observed retail behaviour (user screenshots/video under
Proton); OpenQ4's parity test descriptions as acceptance targets **[verify the test data's
derivation]**. Segment/type names are ours. The engine hook mirrors `R_AddEffectSurfaces`:
portal areas keep an effect-reference list and the manager services live effects per frame.
Expected size: ~6–8 k lines (the decompile-derived implementations are 10–11 k).

**Escalation path (decision §9):** when an effect behaves wrongly and the grammar, docs and
retail observation do not explain why, the decompile-derived implementations may be read for
that specific behaviour (envelope evaluation order, jitter seeding, trail spacing, …). The
finding is then implemented in DUDE's own code and modern C++ (RAII, `idList`/`idStr`,
no raw `new`/`delete`, no x87 idioms), with the consultation noted in the commit message.
Nothing is pasted.

Deliverables: `neo/renderer/bse/` (decl, runtime, particle types, manager), `testEffect`
and `bseSweep` console commands, `bse_*` cvars, motion vectors for effect surfaces so FSR2
does not ghost them. **RT note:** effect geometry stays out of BLAS builds in v1 (as DUDE's
particles do today).

### 5.6 Game library (separate repository, SDK licence)

`tools/quake4/build-q4game.sh` (in DUDE):
1. Clone/update the game repository (or unpack an SDK archive the user supplies, verifying
   its hash) **outside** the DUDE tree.
2. The game repository carries DUDE's adaptation as commits/patches on top of pristine SDK
   source: 64-bit cleanliness (int/pointer casts), modern GCC/Clang/MSVC fixes, include-path
   rewrite to DUDE's headers, idlib shim, interface adaptations per §5.4, `GetGameAPI` export
   matching DUDE's `GAME_API_VERSION`. Keep them mechanical; behavioural adaptations go into
   DUDE's engine side, where they are GPL.
3. CMake: a SHARED target whose include path is DUDE's `neo/` — never the reverse.
4. Output `q4base.so`/`.dll` (named after the title's base folder, like `base.so`) next to
   `build/dude`; `LoadGameDLLbyName` already searches next
   to the binary first (how `d3le.so` is found today).

**Q5 plan (2026-10-24).** Decisions (user): start from the **pristine SDK**, take ideas from
the other ports (idTech4A++, OpenQ4) where they help, build our own port with **fidelity and
performance** as the goals; repository `~/dude-quake4-game` (local git, never pushed).

- **Source (done):** `~/dude-quake4-game` commit `94a1fdc` = the SDK 1.4.2 `source/` tree +
  Raven's `EULA.Development Kit.rtf` + `readme.txt`, unmodified, from the initial import of
  the full SDK installer in `FriskTheFallenHuman/quake4sdk` (`0682c1b8`); an independent 2015
  copy (`Reddragoon244/Quake-4-Source-Code`, `de2da6b`) matches all 718 source files byte for
  byte after line-ending normalisation. Not used: the `Xanz/Quake4SDK` mirror chosen at Q0 -
  its "formatting fixes" re-braced the whole tree and it changes maths and MP code. That
  repository's (EULA) README states licence, layout and provenance.
- **Include architecture:** every SDK game file starts with `#include "../idlib/precompiled.h"`,
  which pulls in the SDK's copies of the engine interfaces (`framework/`, `renderer/`, `sound/`,
  `cm/`, `aas/`, `bse/`, `ui/`, `sys/`) and Raven's `game/Game.h`. Those can never be what the
  library compiles against: every type that crosses the boundary (`idStr`, `idDict`, `idList`,
  `idVec*`, `idBounds`, `renderEntity_t`, `trace_t`, `usercmd_t` …) must have DUDE's layout. So
  the game repository turns its engine headers into one-line forwarders to DUDE's (`-I
  <dude>/neo`, `#include <framework/Common.h>`), replaces `precompiled.h` with an adapter
  (DUDE's idlib + a Raven idlib layer: Raven-only helpers from the SDK's own idlib where they
  do not cross the boundary, adapters where they do), and points `game/Game.h` at DUDE's
  `framework/Game.h`. SP `game/` only; `mpgame/` is Q9.
- **Engine side (DUDE, GPL, our own declarations):** the §5.4 union, driven by what the SP game
  actually fails to compile against - nothing speculative: `idGame`/`idGameEdit`/`gameImport`/
  `gameExport` (Q4 entries with defaults, MP blocks stubbed), `renderEntity_t`/`renderLight_t`
  fields and the effect calls, the sound facade (`worldId`, emitter handles), the collision
  facade (`idCollisionModel`, map-scoped; trace `materialType` = the material's own, §5.3), the
  decl accessors of Q2's decl types, UI / common / file-system extras, and a **no-op BSE
  manager** (every effect call accepted, nothing drawn) until Q4 brings the real one.
  **The dhewm3 game ABI stays frozen (user, 2026-10-24: binary compatibility with dhewm3 mods
  is DUDE's main strength).** `GAME_API_VERSION` stays 9 and no struct or interface a dhewm3
  game library compiles against changes layout, vtable order or signatures; a mod built
  against dhewm3 1.5.5's headers loads and runs unchanged. Quake 4 gets its **own** interface
  layer instead of growing the shared one: its game library receives the same `gameImport_t`,
  asks the engine for the Quake 4 interfaces through the existing extension point
  `idCommon::GetAdditionalFunction` (a new `FunctionType`; dhewm3 added that call exactly so
  the ABI need not change), and gets **facade objects** - a Quake 4 render world, sound
  system, collision manager, BSE manager, decl accessors - declared in new DUDE headers of our
  own (Quake 4-shaped `renderEntity_t` etc. live there, converted to the engine's internal
  structs by the facade; Doom 3's headers do not grow). The game's `idGame` is a subclass
  whose Quake 4 entries are appended after Doom 3's, and the engine calls them only for the
  Quake 4 title. This also means fewer patches to Raven's game code (the facades take Quake
  4's signatures as they are) - better for fidelity.
  **Audit (2026-10-24, 93 game-visible headers vs `upstream/master` = dhewm3 1.5.5):** 17
  changed; the Quake 4 work kept them ABI-safe (appended enum values, appended virtuals on
  engine-implemented classes, fields appended to engine-allocated objects), but three older
  DUDE changes break dhewm3 binaries: `shaderStage_t` grew (parallax / tessellation fields), so
  the inline `idMaterial::GetStage(i)` of a dhewm3 build strides wrongly - dhewm3's own
  `Player.cpp` PDA-video code does that and would read garbage for stage 1+; `idRenderWorld::
  Trace` gained a 7th parameter (`skipDecals`), which a dhewm3 caller never passes; and
  `glyphInfo_t` changed from int to float metrics (Q1, Raven fonts) - unused by game code but
  part of the public header. They get fixed first, and a layout/vtable diff against dhewm3
  1.5.5's headers (GCC `-fdump-lang-class`) guards it from then on.
- **Steps and gates:**
  0. **Q5.0 dhewm3 ABI** - the ABI diff tool, then the three breaks above fixed. Gate: diff
     clean; Doom 3 + RoE unchanged (user). **DONE 2026-10-24, merged (`380e99fa`):**
     `tools/check_dhewm3_abi.py` found a fourth break (`glconfig_s` mid-struct fields); all
     four fixed, 0 breaks over 543 records / 64 vtables / 851 enumerators; user-verified with
     dhewm3 1.5.5's own `base.so` / `d3xp.so` loaded into DUDE (`docs/mod-dll-compatibility.md`
     §7.4).
  1. **Q5.1 build skeleton** - `tools/quake4/build-q4game.sh` (DUDE) + CMake (game repo), the
     include architecture above, and a compile-error inventory tool (errors grouped by missing
     symbol / header), so the remaining work is a measured list. Gate: the build runs end to
     end and the inventory is recorded here.
     **Built 2026-10-24.** Game repo: `CMakeLists.txt` (sources = Raven's `game.vcproj`, 159;
     includes = DUDE's `neo/` + `build/` only; DUDE's game-library flags; `Q4SDK` as Raven's
     SDK build had it; links DUDE's `libidlib.a`) and `source/idlib/precompiled.h` replaced by
     the adapter. DUDE: `tools/quake4/build-q4game.sh` (reads `base.so`'s flags from `build/`,
     keeps only external include paths - never `neo/game` -, builds, copies `q4base.so` next to
     `build/dude`; `--inventory`) and `tools/quake4/q4game_inventory.py` (syntax-only pass over
     the compile database, errors de-duplicated across sources and grouped). **First inventory:**
     159/159 sources fail, 4214 distinct errors, no missing header. By owner:

     | Work package | Largest items (distinct errors) |
     |---|---|
     | **Raven `game/Game.h`** - game-side definitions the adapter no longer pulls in (split it: game-side part stays in the game repo, interface part comes from DUDE) | `SND_CHANNEL_*` 286, `debugInfoProc_t` 113, `demoState_t` 34, `userOrigin_t` 22, `ClientStats_t` 12; plus the Quake 4 `idGame`/`idGameEdit` entries the game defines (68 + 12 definitions without declaration) |
     | **Raven idlib layer** (Q5.2) | `RV_*` heap macros 140, `MA_*` / `MEM_*` allocation tags 128, `rvPair` 73, `rvRandom` 42, `idBitMsg` / `idBitMsgDelta` (`WriteLong`/`ReadLong` …) 152, `idVec3` 45 / `idBounds` 14 extras, template arity 82, `Lexer` (LexerFactory) 34 |
     | **Engine: common / cvars / files / network** | `idCommon` 312 (`GetLocalizedString` …), `CVAR_*` 41, `idCVarHelp` 33 (Raven's `framework/CVarSystem.h`), `idFile` 28, `idNetworkSystem` 25 |
     | **Engine: sound** (`worldId`, emitter handles, Raven channel names) | `idSoundSystem` 79, `refSound_t` 88, `SOUNDWORLD_*` 62 |
     | **Engine: collision** | `idCollisionModel` 156, `idCollisionModelManager` 18, `contactInfo_t` 28 (`materialType`) |
     | **Engine: effects / BSE** (no-op manager until Q4) | `rvClientEffect` 116, `PlayEffect` 82, `renderEffect` / `renderEffect_t` 123, `effectCategory_t` 16 |
     | **Engine: decls** (Q2's types under Raven's class names) | `rvDeclMatType` 68, `rvDeclPlayback` / `Data` 68, `rvDeclLipSync` 25, `rvDeclPlayerModel` 15, `idDeclManager` 58 |
     | **Engine: renderer / UI / input** | `RF_*` 26, `idRenderWorld` 16, `idRenderSystem` 10, `SHADERPARM_*` 15, `idUserInterface` 9, `IMPULSE_*` 46 |
     | **64-bit cleanliness** | 47 pointer-to-int casts |
     | **Cascades** (game classes that failed to parse because of the above; expected to vanish) | `idGameLocal` 89, `idClipModel` 42, MP stats (`rankedPlayers`, `statQueue`) …, 412 unclassified |

     Order: the `game/Game.h` split and the idlib layer first (they cascade the most), then the
     engine packages one by one, re-running the inventory after each.
  2. **Q5.2 idlib layer** - every idlib error gone. Gate: `idlib`-attributed errors 0.
     **Done 2026-10-24: 4214 -> 2349 distinct errors, 0 attributed to idlib; the rest are engine
     interfaces (Q5.3), 64-bit casts and their cascades.** Split by where a piece can live:
     - **Raven's methods on id's classes go into DUDE's idlib**, written for DUDE (GPL, Raven's
       code read for semantics only), all non-virtual, no data, so `check_dhewm3_abi.py` stays
       at 0 breaks: `idVec3` `ToNormal` / `ToMat3( axis )` / `Dist` / `DistXY` / `Dist2XY` /
       `DistToLineSeg` / `ProjectToLineSeg` / `ProjectOntoVector` / `IsLeftOf` / `IsZero` /
       `Random`; `idAngles` `Length` / `Random` / `Remap` / `Scale`; `idBounds` `Size` /
       `Contains` / `ShortestDistance` / `FindEdgePoint` / `FindVectorToEdge`; `idMath` `Lerp`
       (clamped) / `MidPointLerp` / `dBToScale`; `idMat3` `RotateAbsolute` / `RotateRelative` /
       `RotateArbitrary`; `idBitMsg` / `idBitMsgDelta` `*Long` (= `*Int`), `GetRemainingData`,
       `GetReadData`, `InitReading` / `InitWriting`, `*DeltaVec3` / `*DeltaMat3`; `idStr`
       `ReplaceChar`, `Filter` defaulting to case-sensitive; `idDict::RandomPrefix` with a
       default; `idList` `TypeSize` / `RemoveNull` / `StackAdd` / `StackPop` / `StackTop`;
       `idMapFile::Resolve` (+ `idMapBrush::TranslateSelf`, texture-locked).
       **Checked against Raven's own implementations** (scratch harness on `libidlib.a`, not
       committed - it holds SDK code): 200 000 random cases bit-identical, except
       `RotateArbitrary` (closed form vs Raven's five-matrix product) within 4e-7. `Resolve` over
       the 63 retail `.map` files: entity / primitive counts as expected in all 51 maps with
       func_groups; 9017 func_groups have an origin, and their primitives are stored relative to
       it (6082 of 6086 far-off patches sit near 0), so the translation Raven added is needed;
       the brush texture lock holds on every brush tried. id's `Parse()` already applies the
       worldspawn keys Raven moved into `Resolve` (`removeEntities*`, `overrideMaterial`,
       `forceEntityNames`; no retail map uses them), so DUDE's `Resolve` is the func_group step.
     - **Raven-only types, globals and macros live in the game repo** as Raven wrote them
       (`source/idlib/RavenIdlib.h/.cpp`): allocation tags + heap macros (no-ops on the PC),
       `rvRandom` (seed pinned to 32 bits: Raven's `unsigned long` is 64 on LP64 and broke its
       0..32767 range), `rvPair`, `rvSphericalInterpolate`, `rvBits` / `rvPool` /
       `rvIndexPool`, `idMsgQueue` / `idBitMsgQueue` (the engine's own `idMsgQueue` is never
       included by the game and not exported), `SignZero`, `vec4_one`, Raven's colour / icon /
       escape macros.
     - **Raven members whose behaviour has quirks the game shipped with stay Raven's code**, as
       free functions the game calls by a new name (marked patches): the `idList` heap
       (`rvListHeapAdd` / `rvListHeapPop` - the sift-down always takes the left child),
       `rvStr::Split` (takes a length for an end index inside quotes), `rvStr::RemoveEscapes` /
       `IcmpNoEscape` / `IsEscape` (drops the character after an unremoved `^`), and
       `rvListRemoveContents` (needs `EV_Remove`, so it was game-side in Raven too).
     - **Mechanical game patches:** `idMath::INT_MIN/INT_MAX` -> `INT_MIN/INT_MAX`,
       `idMath::FLOAT_EPSILON` -> `FLT_EPSILON`, allocator memory-tag template arguments dropped,
       `LexerFactory` -> `idLexer` (Raven's binary lexer only ran under `com_binaryRead`), the
       map-file resolved flag kept in `idGameLocal` (`mapFileResolved`), and `idGameEdit`'s map
       export (an editor merge workflow DUDE's `idMapFile` does not have) reports no export
       entities.
  3. **Q5.3 engine interfaces** - the union, item by item from the inventory, Doom 3 libraries
     rebuilt and unchanged in behaviour, the ABI diff against dhewm3 1.5.5 clean. Gate:
     `q4base.so` compiles and links; Doom 3 + RoE smoke test by the user.
     **As built (2026-10-24), refining the facade plan above.** Two kinds of difference, two
     mechanisms:
     - **Quake 4 asks the same interface for more** (common, cvars, files, sys, UI, network,
       render system, models, decls, AAS): the methods are added to DUDE's interfaces - inline
       where they need nothing from the engine (`GetLocalizedString`, `DebuggerCheckBreakpoint`
       over `FT_UpdateDebugger`, `ReadSyncId`, ...), appended virtuals otherwise; cvar flags and
       portal bits as appended enum values. `check_dhewm3_abi.py` stays at 0 breaks.
     - **Quake 4 reshaped a type** (render entities and lights, model traces, user commands,
       traces and contacts with a material type, sounds addressed by world id and handle,
       collision models as objects, effects, the game interface itself): `neo/quake4/` declares
       Quake 4's versions under names of their own (`renderEntityQ4_t`, `idRenderWorldQ4`,
       `idSoundSystemQ4`, `idCollisionModelManagerQ4`, `idEffectManagerQ4`, `idGameQ4`, ...),
       written from the game's call sites, and `quake4/GameNamesQ4.h` - included only by the
       game library, after every engine header - maps Raven's names onto them. Handing a Doom
       3 struct where a Quake 4 one is expected is therefore a compile error, never a silent
       layout mismatch (a shared header that only forward-declares `struct renderEntity_s`
       would otherwise accept either). Inline conversions (`R_RenderEntityQ4ToEngine`,
       `R_ModelBoundsQ4`) live in those headers so the game and the facades share them.
     - **The handshake:** instead of the `GetAdditionalFunction` route sketched above, the
       engine loads the Quake 4 library (title profile) with its own import block,
       `gameImportQ4_t` - the engine's systems where they are shared, the Quake 4 facades where
       not - and an engine-side adapter presents `idGameQ4` to the session as an `idGame`
       (Q5.4). Doom 3's `GetGameAPI` path and `GAME_API_VERSION` 9 are untouched.
     - **Engine work it took:** a real playback runtime (`idDeclPlayback`: samples carried
       over, uniform cubic B-splines for position and angles, button / impulse events with
       their time into the frame; OpenQ4's decompile-derived reading consulted for the format,
       none of its code used), `idDeclPlayerModel`, `idDeclAF::fastEval`, AAS area ceilings,
       sound-shader and lip-sync accessors, Raven's material sorts (`SS_PREGUI` = -2.5, between
       Doom 3's subview -3 and GUI -2).
     - **Recorded gaps** (interface in place, behaviour to come): `RunAlwaysThinkGUIs`, the
       script full-screen effects (medlabs' blur), `^i` text icons, multiplayer and voice
       entries (Q9), editor bits.
     Inventory: 2349 -> 1756 (shared interfaces) -> 68 (the Quake 4 layer) -> 0: the 64-bit
     cleanliness of the event and script system came from dhewm3's own x86_64 fixes of the
     same id code (`d92d4ba8`, `506ea7b3`, `ce976c47`, `db4aca78`, `4244436d`, `fb3f0cc2`,
     `76a9fdce`, `3ad384a0`), applied hunk by hunk to Raven's copies (game repo `bcff329`).
     **All 160 sources compile and `q4base.so` links** (game repo `bcff329`, DUDE this
     commit): its undefined symbols are libc and the C++ runtime only, `GetGameAPI` its
     export - `idDeclTable::TableLookup` (vehicle hover physics) moved inline for that.
     **Gate remaining: the Doom 3 + RoE smoke test** (the engine headers both game libraries
     build against changed: appended virtuals, cvar flags, decl and idlib additions).
     Silent 64-bit risks the compiler cannot see (Raven's own additions to the script system,
     `long` in its code) are a Q5.5 check.
  4. **Q5.4 load** - `q4base.so` replaces the null game under `run-q4.sh`: `GetGameAPI`,
     `Init`, main menu with the game's cvars (the sweep's `mainmenu.gui` cvar warnings gone),
     clean quit. Gate: boot + quit on both backends, no new warnings.
  5. **Q5.5 airdefense1** - spawn, HUD, weapons, AI, scripts, triggers; then VO + lipsync,
     cinematics (`md5camera`), save + load, the transition to `airdefense2`; an ASAN pass.
     Gate: the phase gate (§6).
- **Performance:** 64-bit, the engine's compiler flags (SSE4.1 baseline), no debug-only cost in
  release builds; the game's frame cost is measured at Q5.5 (`com_speeds`-style split) and
  compared with Doom 3's on a comparable map.

### 5.7 The null game (engine, GPL, small, high leverage)

A ~400-line `nullgame` library implementing `idGame` with a free-flying spectator camera and
nothing else. **As built in Q1:** `neo/nullgame/NullGame.cpp`, CMake option `NULLGAME`
(default ON), output `build/nullgame.so`. It parses the `.map` only to start the camera at the
first player start, flies with the normal movement keys (run doubles the speed), places the
sound listener, renders the session's world with a hor+ 90 degree view, and offers
`getviewpos` / `setviewpos`. Every `idGameEdit` method is stubbed because the engine calls some
outside the editors (GUI `renderDef` windows, the worldspawn lookup at map load). It loads
automatically when a title allows it and its own library is missing (Quake 4 until Q5), or on
request with `+set fs_game nullgame` on any title. Since Q3 it also loads the map's collision
(`collisionModelManager->LoadMap` in `InitFromNewMap`, `FreeMap` at shutdown, as
`idGameLocal` does): nothing traces against it yet, but every map's `.cm` goes through the
loader and its statistics land in the log.

**Map dressing (Q3 step 0, `neo/nullgame/NullGameDressing.cpp`):** a bare world says little
about whether a map renders right, so the null game adds, render-only, what the map's
entities show when they spawn — lights (Doom 3's `ParseSpawnArgsToRenderLight` plus the
`_prelight_` shadow model), static and brush models, particle models, entity GUIs, and MD5
models frozen on frame 0 of their start animation (the entity's `anim`, else `idle`, else
`af_pose`, else the bind pose), with actors' `def_head` heads on their `head_joint`. Spawn
arguments are the map's keys over the entity def's, as the game merges them; what does not
show at spawn is skipped (`hide`, `teleport`, `start_off`, triggers, player starts, inline
statics). Model defs and `.md5anim` frame 0 are parsed by the null game itself — the
animation system is game code — using only engine interfaces (the library still imports
nothing from the engine). `nullgame_dressing 0` shows the world alone. Portal skies (Quake 4's `func_camera_portalsky`, RoE's `info_portalSky`) go to the
renderer through `idRenderWorld::SetPortalSkyCamera` (appended virtual): the primary view
renders the sky camera as its own **subview** when an area it reached shows sky (the `.proc`
per-area sky number, Quake 4's `HasSkybox` rule), captures it into `_portalSky` (and
`_currentRender` for RoE's `portalSky` surfaces), clears the colour, and composites it at the
far plane right after its world (`RB_RHI_PortalSkyComposite`), so the sky fills only what the
world left empty and never reaches the temporal / RT passes. Rendering it as a second
top-level view, as the games' own player views do, fed the sky camera into FSR2 / NRD /
ReSTIR on the RT tiers (double exposure, black surfaces). Q5's `RF_PORTAL_SKY` maps onto this.
RoE's and Lost Mission's own player views (`neo/d3xp`, `neo/d3le` `PlayerView.cpp`) use it too
since 2026-10-23 (branch `fix/roe-portal-sky-subview`): their top-level sky view also took the
frame's once-per-frame RT staging (the anim-caster slot was claimed even by a non-qualifying
view; fixed separately, `fix/rt-anim-caster-frame-claim`). Offline, an emulation
of the same rules resolved every model key on seven Doom 3 / RoE maps, and all 2679 Doom 3
and 2233 Quake 4 `.md5anim` files have the layout the frame-0 reader expects. Doom 3 itself
is untouched: only the null game library changed. It validates every engine-side phase (§6) **without** the SDK — load any
title's map, render it, play effects with `testEffect`, exercise loaders and parsers, run the
smoke harness (§7) — and stays as a permanent diagnostics tool for Doom 3 itself.

---

## 6. Phases, gates, estimates

Rough person-weeks for one focused engineer plus the user's play-testing. Each phase ends at
a **gate** that is automated or a user-verified run — nothing advances on "it compiles".

| # | Phase | Scope | Gate | Est. |
|---|---|---|---|---|
| Q0 | **Preparation** — **DONE 2026-10-22** | Decisions taken (§9); reference trees cloned outside the repo to `~/quake4-refs/` as blobless sparse checkouts (idTech4A++ `ce49510` 39 MB, OpenQ4 `29c8103` 123 MB, SDK mirror `Xanz/Quake4SDK` `5077f68` 14 MB; the mirror dropped its EULA, OpenQ4 carries it in `LICENSES/QUAKE-4-SDK-EULA.rtf`); `tools/quake4/` audit. The separate game repository moves to Q5, where it is first needed: `openQ4-game` was archived on 2026-10-01 and its code now lives in OpenQ4's `src/game` / `src/mpgame` | Trees on disk | 0.5 |
| Q1 | **Title profile + boot** — **BUILT 2026-10-22, gate pending** | §5.1 profile; `.fontdat` loader (float glyph metrics + left bearing, Doom 3 fonts unchanged); `run-q4.sh` (Steam library autodetect or `Q4DIR`, then `+set com_title quake4 +set fs_basepath "<Quake 4 dir>"`); null game. Decl registration moved to Q2, where the new decl types land: Quake 4 uses the same `materials/`, `skins/` and `sound/` folders Doom 3 registers, so Q1 had nothing to change there | `./run-q4.sh` boots on both backends, lists the 36 paks, draws the console with the Quake 4 fonts, `exec default.cfg` runs without errors | 1 |
| Q2 | **Decl/material/GUI/sound parity** — **BUILT 2026-10-23; `lintDecls` gate PASSED (17085 decls, 3 retail quirks), menu verified on GL3; Vulkan + Doom 3 regression pending** | §5.2 guide preprocessor, keywords, new decl types, EFX mapping, UI API additions (as built: §5.2) | `lintDecls` (new console cmd) reports **0 failures** over every material / sound shader / effect / lipSync / playback / materialType / entity decl, except the retail data quirks listed in §1.4 and `tools/quake4/README.md` (GUIs are not decls: `q4_gui_census.py` offline, the menu run by the user); `mainmenu.gui` renders and is navigable (user screenshot); Doom 3 menu, sound and materials unchanged | 2 |
| Q3 | **World formats** | step 0: null-game map dressing (§5.7); §5.3 `.map 3` (incl. `func_group` resolve + entity filter), `.proc 4` (incl. coloured verts, portal fade data), `.cm 3`, `.aas 1.08` (incl. dummy-file detection); ~~collision API additions~~ → Q5 (game-facing; the `.cm` texture pairs cannot drive a hit map, §5.3) | Null game loads **all 31 SP maps** and renders them; `cm`/`aas` self-checks (counts match file headers); user screenshot of `airdefense1` on GL3 and VK | 1.5–2 |
| Q4 | **BSE effects** | §5.5: own implementation from grammar + docs, decompile-derived code as last-resort reference, re-expressed in our C++ | All 1180 effects parse; `testEffect` plays each particle type; `bseSweep` plays every effect once without errors; user visual check of ~20 representative effects vs retail; OpenQ4 parity data passes where applicable | **5–7** |
| Q5 | **Game library** | §5.4 interfaces (API 37 union, render/sound/collision/decl/UI additions - collision incl. map-scoped handles, the `idCollisionModel` accessors and `materialType` in traces, moved from Q3), §5.6 repo + build script, idlib shim, `playback`/`lipSync` consumers, `rvClientEffect` bridge, ASAN pass | `airdefense1` **playable**: spawn, HUD, weapons, AI, scripts, triggers, VO + lipsync, cinematic (`md5camera`), save + load round-trip; transition to `airdefense2` | 5–7 |
| Q6 | **Campaign pass** | Per-map issue log seeded from idTech4A++'s fixes (§4); vehicles (`convoy1/2`, `walker`), tram (`tram1/1b`), MCC sequences, Strogg-ification (`medlabs`), hub, `core1/2` ending; difficulty, objectives, loading screens | User completes the campaign start to finish on one backend, then the other; every logged issue closed or explicitly deferred | 3–5 |
| Q7 | **DUDE enhancements on Quake 4** | Presets (Potato = faithful); textures: Quake 4 ships 2010 precompressed DXT `.dds` (`progimg/`, `dds/`) — offer DUDE's own upload compression instead where the source image exists (dhewm3's `image_useCompression 2` = BC7/BPTC), opt-in (user note 2026-10-23); PBR classifier calibration on Raven textures; parallax/tess gating; RT: world + animated BLAS from Q4 content, RTAO/RTGI/ReSTIR DI tiers; FSR2 motion vectors for BSE; perf pass (`r_vkGpuTime`) | Ultra Nightmare runs `airdefense1` within ±25 % of the Doom 3 reference budget; fidelity-note policy applied to every new toggle; user screenshots per tier | 2–3 |
| Q8 | **Packaging** | Steam/GOG path auto-detect inside the engine (Q1's `run-q4.sh` already scans Steam libraries); ship `nullgame.so`/`.dll` with the release (`build-win.sh` already copies every `.dll`; check `build-linux-release.sh`); Windows cross-build of the engine (`build-win.sh`) and `q4base.dll` via the same script under mingw; README/docs; release notes; `dist/` layout as 0.9.x | Clean-machine install test on Linux + Windows following only the README | 1–2 |
| Q9 | **Later / optional** | Multiplayer (`mpgame`, `q4mp` paks, bots), Q4 mods (Awakening, Hardqore — OpenQ4/idTech4A++ run them), Prey (2006) via the same title profile | — | — |

Total for v1 (Q0–Q8): **~22–30 person-weeks**. Q4 (BSE) and Q5 (game library) touch disjoint
code and can run in parallel once Q3 is done. The Q1–Q3 figures assume the format loaders are
ported from idTech4A++ with credit rather than rewritten; Q4 assumes the escalation path in
§5.5 is used sparingly.

Ordering rationale: Q1–Q4 need **no SDK**; they produce visible results early (the whole
world renders by Q3) and de-risk the long pole before SDK work starts. Q5 is where the separate
game repository must already exist.

---

## 7. Verification harness — "all possible measures"

Every layer is checkable independently of the layers above it. Build these as you go:

1. **`tools/quake4/q4_keywords.py` as regression** — after each parser extension the `*`
   markers for that file type must be gone. Offline, cheap, already written. Q2 added the
   per-format checks `q4_material_census.py`, `q4_gui_census.py`, `q4_sound_census.py` and the
   guide differential test `test_guides.sh`; Q3 the loader replicas `q4_proc_check.py`,
   `q4_cm_check.py` and the real-code tests `test_cm_parse.sh` / `test_aas_parse.sh`, which run
   the engine's own `.cm` / `.aas` parse code over both titles.
2. **`lintDecls` console command** (Q2) — parse every decl of every registered type, print
   per-type error counts; headless via `+lintDecls +quit`. Goes on the Doom 3 release
   checklist too.
3. **Null-game map sweep** (Q3) — `tools/quake4/smoke-maps.sh` runs
   `dude +set com_title quake4 +map game/<x> +wait 120 +quit` over Appendix C with
   `r_vkCrashLogging 1`, grepping the log for `WARNING`/`ERROR` and loader self-checks. No
   frames are viewed (screenshot policy); the user checks visuals.
4. **BSE sweep** (Q4) — `+bseSweep +quit` plays every effect for one lifetime and reports
   parse/runtime errors; `testEffect` is the manual tool; OpenQ4 parity data where usable.
5. **Retail reference** — the user has Quake 4 on Steam; retail under Proton (or the 1.4.2
   Linux client) gives reference screenshots for effect/material parity (user-driven).
6. **Save-game round trip** (Q5) — `savegame`/`loadgame` on every campaign map.
7. **Provenance gate** — every hunk ported from a FOSS reference carries its origin (project,
   file, commit) and credit in the commit message; every consultation of decompile-derived
   code is noted the same way with the behaviour it resolved; `git blame` on the reference
   tells the two apart (idTech4A++: pre-2025 = format work, 2025-08+ BSE/renderer = decompile).
8. **ASAN + `-Wint-to-pointer-cast`** builds of engine + game for Q5; both backends, VK
   validation on for the first pass of every gate; device-loss checked against the hardware
   Xid-79 note before chasing software.

---

## 8. Risk register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| **Licence**: the EULA's "operate only with QUAKE 4" clause, or the GPL combined-work argument, makes shipping `q4game` untenable | medium | blocks binary distribution; does **not** block engine work or local builds | separate repo with EULA attached (OpenQ4 precedent); no prebuilt binary by default; revisit at Q8 |
| **Decompile taint** in available BSE/renderer code | certain (it is tainted) | copying it would put DUDE's GPL tree at risk | own BSE from grammar + docs; decompiled code consulted only as a last resort and re-expressed, never pasted (§3 rule 1, §5.5); provenance gate (§7.7) |
| **BSE fidelity** from grammar + observation rather than Raven's engine | high | some effects look wrong | census-driven coverage; parity data; known-gap list at the Q4 gate; both open projects still report "many bugs" after decompile-based work, so parity is a long tail for everyone |
| **Game API surface** (57 new `idGame`, 67 new `idGameEdit` methods; sound/collision re-indexed by handles) | high | long Q5 | union-with-defaults policy; stub the MP/Q4TV/voice/rcon blocks; idTech4A++'s `quake4/` shows which calls the SP game actually makes |
| **Hidden engine behaviour** the game assumes (shake tables, lipsync driven by `CurrentAmplitude`, playback cursor, per-texel hit maps, entity-filter levels, engine-selected loading GUIs, cvar-flag overrides for mods, `si_fps`) | high | subtle gameplay/cinematic bugs | SDK headers + iddevnet docs + OpenQ4's written audits; idTech4A++ changelog as a bug map; Q6 campaign log |
| **ABI drift**: 32-bit-era SDK on 64-bit, struct padding across `GetGameAPI` | high | crashes | ASAN; warnings as errors in the game repo; d3le precedent |
| **GUI scripting scale** (`mainmenu.gui` 713 KB) | medium | menu glitches | Q2 gate includes navigation |
| **Performance**: maps ~2× Doom 3's size, 606 collision models per map, BSE counts | medium | sub-budget on RT tiers | Q7 budget gate; BSE rate limiting; existing shadow-cache/stagger machinery |
| **Scope creep into MP** | medium | delays v1 | MP is Q9 by decision |
| **Asset path assumptions** (`run.sh` forces `fs_basepath = repo`) | low | setup confusion | `run-q4.sh` + profile auto-detect; README |

---

## 9. Decisions taken (user, 2026-10-15)

1. **One executable for all supported games.** Implemented as the runtime title profile
   (§5.1): a single `dude` binary; the per-title game library (`base.so`, `d3xp.so`,
   `d3le.so`, `q4base.so`) is selected by the profile the way `fs_game` selects it today. No
   per-title binaries, no compile-time title macros.
2. **BSE and the decompile-derived code.** DUDE writes its own BSE from the file grammar, the
   SDK interface header and the public docs. The decompile-derived implementations are used
   only when a problem is hard to find otherwise, and what is learned is made our own —
   rewritten in DUDE's current C++ style — never copied verbatim (§3 rule 1, §5.5 escalation
   path, §7.7 provenance gate).
3. **Any FOSS source may be used, with credits where needed.** GPL code from idTech4A++ and
   OpenQ4 (loaders, fonts, GUI keywords, decl types, test tools) is ported with attribution in
   commit messages and in the README credits section; the SDK-derived game code stays in the
   separate EULA-attached repository (§3 rule 2) because it is not FOSS.
4. **Multiplayer later.** Q9; `mpgame` is not built in v1.

Agreed defaults that follow from the above: branch per phase (`feat/quake4-title-profile`,
`feat/quake4-decl-parity`, …) merged to local master, never pushed; reference trees are cloned
at Q0 with source and size stated at the time (the research agent's temporary sparse clone of
idTech4A++ `doom3/neo` in the session scratchpad is not part of the repository). **Still
open:** whether a prebuilt `q4game` is ever distributed (default no; revisit at Q8).

---

## Appendix A — exact format deltas (verbatim from the files)

**`.cm` header and model line** (`maps/game/airdefense1.cm` vs Doom 3 `mars_city1.cm`):
```
Doom 3:  CM "1.00"           Quake 4:  CM "3"
         3487335092                    4236787096
         collisionModel "worldMap" {   collisionModel "worldMap" 10234 {
```
**`.cm` polygon row** — Quake 4 appends three `( s t )` pairs and the map primitive number:
```
D3: 4 ( -6169 -6166 -6163 -6171 ) ( 0 0 1 ) 26704 ( 6144 8352 26704 ) ( 7456 9472 26704 ) "textures/sfx/black"
Q4: 4 ( -19429 19438 -19433 -19437 ) ( 0 1 0 ) 9024 ( 14784 9024 -192 ) ( 25472 9024 2816 ) "textures/editor/aassolid" ( 430 6 ) ( 96.0000076294 -88 ) ( 96 6 ) 1944
```
**`.cm` brush trailer** — Raven contents names plus the map primitive number:
```
D3: } ( 6128 8336 26688 ) ( 6144 9488 27312 ) "solid,opaque"
Q4: } ( 14784 -1664 -192 ) ( 25472 9024 2816 ) "aas_solid" 1944
```
**`.proc` header, coloured vertex, portal row**:
```
D3: mapProcFile003                       Q4: PROC "4"
                                              4236787096
Q4 8-float vertex:  ( 23 3 2 0 0 -1 0 0 )
Q4 12-float vertex: ( 11079.25 -9167.34375 128 1.7081640959 7.069791317 0.4492426813 0.3344642818 0.8284410834 137 137 137 255 )
/* interAreaPortal format is: numPoints positiveSideArea negativeSideArea ( point ) ... [( fadeImage distanceNear distanceFar )] */
```
**`.aas` areas row and new sections**:
```
D3 (1.07): areas 1837 {  0 ( 0 0 0 0 0 0 ) 0 { }
Q4 (1.08): areas 1211 {  0 ( 0 0 0 0 0 0 0 0 ) 0 { }   1 ( 65 0 0 11 1 0 0 0 ) 3 {
           features 228 {  0 ( 37 72 254 127 10291 -2651 0 ) …
           featureIndex 247 {  0 ( 0 ) …
           settings { … generateTacticalFeatures = 1  tt_barrierJump = 100  tt_startCrouching = 100  tt_waterJump = 100  tt_startWalkOffLedge = 100 }
```
**`.map` header**: `Version 3`, then identical entity/primitive grammar (`brushDef3` 6293,
`patchDef2` 3509, `patchDef3` 1991 in `airdefense1.map`).

**BSE effect skeleton** (`effects/ambient/acid_react_waste.fx`):
```
effect effects/ambient/acid_react_waste {
    size 449
    emitter "bubbles2" {
        duration 1,1   count 50,50
        oriented {
            duration 1,1   blend add   material "gfx/effects/fluids_drips/bubble_additive"
            start  { position { sphere 1,-80,-80,1,80,80 }  size { point 5,5 }  tint { line 0.25098,0,0,0.501961,0.501961,0 } }
            motion { size { envelope "linear" }  fade { envelope "linear" } }
        }
    }
}
```
**Font**: `fonts/english/chain_12.fontdat` = 9236 B (= 256 × 36 + 20) vs Doom 3
`fontImage_12.dat` = 20548 B (= 256 × 80 + 68).

**Sound shader** (`sound/airdefense.sndshd`):
```
sound airdefense_intro_heartbeat { minDistance 80  maxDistance 900  volumeDb 0  soundClass 2  global
    sound/ambience/med_labs/heartbeat
    shakeData 0 aabbcbbcdceeeghijeeeihlimlknoololppvusknpqsppxnnqmplntuuotrsnnpqnmlnpoqzntoqlnmqowzxhklqrtnnprkllmoqukzluqplmqngztiflp }
```

## Appendix B — Quake 4 game-library surface (from `.def` `spawnclass`)

`idAnimated` 154, `idItem` 126, `idMoveableItem` 97, `rvClientAFAttachment` 47,
`idProjectile` 32, `rvVehicleTurret` 26, `idCameraAnim` 25, `idGuidedProjectile` 20,
`rvVehicleWeapon` 16, `idTrigger_Multi` 14, `idAFEntity_Generic` 10, `idPlayer` 10,
`rvVehicleThruster` 9, `rvVehicleRigid` 8, `rvAITactical` 7, `idAFAttachment` 7, … (90+
classes; full list from `tools/quake4/q4_keywords.py`).

## Appendix C — single-player campaign maps (31, from `def/maps.def`)

`airdefense1`, `airdefense2`, `hangar1`, `hangar2`, `mcc_landing`, `building_b`, `convoy1`,
`convoy2`, `convoy2b`, `mcc_1`, `mcc_2`, `hub1`, `hub2`, `medlabs`, `walker`, `dispersal`,
`recomp`, `putra`, `waste`, `process1_first`, `process1_second`, `process2`, `network1`,
`network2`, `storage1_first`, `storage1_second`, `storage2`, `tram1`, `tram1b`, `core1`,
`core2` (all `maps/game/`). MP: `q4dm1–11`, `q4ctf1–8`, `q4tourney1`, `q4x*`, `q4dz*`.

## Appendix D — provenance

- Install: Steam "Quake 4" 1.4.3 (`CHANGES_14.txt`: 1.4.3 = "updated legal for publishing
  under Bethesda Softworks"; 1.4.2 is the last gameplay change). `game100.pk4/gamex86.so`
  strings contain `1.4.2`; exports only `GetGameAPI` (stripped, i386).
- Doom 3 comparison files: `base/pak000.pk4` `maps/game/mars_city1.{cm,proc,aas48,map}`.
- DUDE touch points verified by grep on 2026-10-15: `PROC_FILE_ID` (`RenderWorld.h:50`),
  `CM_FILEVERSION` (`CollisionModel_files.cpp:46`), `AAS_FILEVERSION` (`AASFile.h:45`),
  `CURRENT_MAP_VERSION` (`MapFile.h:51`), `MD5_VERSION` (`Model.h:48`), `FILESIZE_fontInfo_t`
  (`tr_font.cpp:33`), decl registrations (`DeclManager.cpp:823–840`, `game/Game_local.cpp:327`),
  `GAME_API_VERSION 9` (`framework/Game.h:366`), no guide support in `DeclManager.cpp`.
- Interface deltas (§5.4) read from idTech4A++ tip `ce49510f` (v1.1.0harmattan73,
  2026-09-30): `renderer/RenderWorld.h`, `sound/sound.h`, `cm/CollisionModel.h`,
  `framework/DeclManager.h`, `ui/UserInterface.h` `_RAVEN` blocks; `quake4/Game.h`
  (`GAME_API_VERSION 37`, identical to the pristine SDK's `game/Game.h:670`).
- SDK contents and EULA: `FriskTheFallenHuman/quake4sdk` listing; `LICENSES/QUAKE-4-SDK-EULA.rtf`
  in OpenQ4; iddevnet mirror pages `quake4/Quake4SDK`, `quake4/LinuxSDK`.
- SDK header deltas (§1.1 AAS/fonts/sound details, §5.4 signatures, §5.5 interface) read from
  the `Xanz/Quake4SDK` mirror (`game/Game.h`, `bse/BSEInterface.h`, `framework/declManager.h`,
  `renderer/RenderWorld.h|Material.h|Model.h`, `sound/sound.h`, `cm/CollisionModel.h`,
  `aas/AASFile.h`, `ui/UserInterface.h`, `framework/licensee.h`, `idlib/*`) and the iddevnet
  mirror's FX/shader/GUI/font pages; cross-checked against the retail files where they make a
  claim about file contents (proc `hasSky`: absent; map brush-side ints: absent; `func_group`:
  present; `materialImage` → `_hit` maps: 303/339; dummy AAS files: present).
