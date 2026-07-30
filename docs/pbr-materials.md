# PBR materials (roughness / metalness) — design

Design reference for an opt-in **physically based shading path** (GGX specular +
metalness workflow) on the GL 3.3 `opengl3` (RHI) backend. This document is the
**design**; sections graduate to "as-built" as the phases land (see §9 Status). Like
the rest of the enhancement suite it is built on the RHI so it ports to Vulkan rather
than being re-derived.

Like everything in the enhancement suite it is **opt-in and enhancement-gated**
(`R_BackendSupportsEnhancements()` → core profile only; nothing on the legacy ARB2
backend), and **off by default** (`r_pbr 0`). This one deserves the strongest fidelity
flag in the suite so far: an energy-conserving GGX BRDF with Fresnel visibly changes
*every lit surface*, not just adds detail on top. The vanilla LUT/Blinn paths
(`r_shading 0/1/2`) remain byte-identical and reachable; the Potato preset stays an
exact vanilla frame.

Primary code (planned): BRDF branch in
[`neo/shaders/interaction.frag`](../neo/shaders/interaction.frag), per-draw parameter
fill in [`neo/renderer/rhi/RhiWorld.cpp`](../neo/renderer/rhi/RhiWorld.cpp), per-material
data on `idMaterial` ([`neo/renderer/Material.cpp`](../neo/renderer/Material.cpp),
following the `occlusionmap` pattern), classifier tool under `tools/`, cvars in
[`neo/renderer/RenderSystem_init.cpp`](../neo/renderer/RenderSystem_init.cpp), UI in the
Enhancements tab ([`Dhewm3SettingsMenu.cpp`](../neo/framework/Dhewm3SettingsMenu.cpp)).

---

## 1. Why — and why it can't be a straight conversion

The Doom 3 material model gives the interaction pass three inputs: normal map, diffuse
albedo, and an RGB **specular color** map. The specular map answers *"how much light is
reflected"*; a modern roughness map answers *"how blurry is the reflection"*. Those are
different physical properties — chrome, brushed steel, wet mud, and polished marble can
all carry similar specular-map brightness while spanning the whole roughness range.
**There is no unique mathematical conversion** from a Doom 3 specular map to a
roughness/metalness representation (RBDOOM-3-BFG's PBR assets were hand-authored for
exactly this reason).

So this design does **not** try to invent per-texel PBR textures from the originals.
Instead it splits the problem:

1. a **GGX BRDF** in the shader that runs off *scalar* per-material parameters
   (works today, on stock assets, with conservative defaults), and
2. an **offline inference pipeline** that assigns those scalars per material from the
   semantic metadata the assets already carry — producing a *consistent* material set,
   not a physically accurate remaster, while preserving the original artistic intent.
   Original assets stay untouched; the generated table ships as an opt-in overlay.

## 2. What the assets already tell us (measured)

Surveyed `base/pak000.pk4`: **~5,148 material bodies** in the stock `.mtr` set.

- **Declared surface types are sparse but authoritative.** Only ~537 materials declare
  one (`stone` 172, `flesh` 172, `ricochet` 68, `glass` 55, `metal` 26, `cardboard` 19,
  `plastic` 16, `wood` 8, `liquid` 1). Authors only declared a surftype when the
  default didn't fit — so where present it's a **strong override signal**, but it can't
  be the primary classifier.
- **Names/paths are the primary signal.** Material paths
  (`textures/base_wall/cpuwall2b`, `textures/hell/flesh01`), directory categories
  (`metal/`, `rock/`, `skin/`…), and the *texture paths inside the material body*
  (bump/diffuse map names like `rusted_pipe_local`, `steel_grate_d`) carry consistent
  author vocabulary. A token classifier over these should land 90–95% of materials,
  with a hand-written exception list for the rest.
- **The normal maps carry roughness information for free.** The interaction shader
  already exploits mip-shortened (un-renormalized) normals; that same shortening *is*
  a normal-variance signal (Toksvig), usable both offline (per-material statistic) and
  in-shader (per-texel, §4).

## 3. Parameterization — metalness easy, roughness hard

**Metalness is effectively binary** (0 = dielectric: wood/plastic/concrete/skin/cloth;
1 = metal: steel/copper/aluminum) and maps almost directly from the name classifier +
surftype overrides. Expected accuracy: high.

**Roughness is under-determined**, so it's *estimated then clamped to a per-category
plausible range* — the category prior does most of the work; the estimate only places
the material within its band:

**Metalness correction (learned in-game 2026-07-30):** PBR metalness means *bare*
metal at the surface. Painted or coated metal is a **dielectric** — light
interacts with the paint layer, not the steel beneath — and Doom 3's station is
overwhelmingly painted panelling. The first classifier pass marked all of it
metalness 1.0 and the whole world read as gunmetal (and darkened, diffuse being
metalness-killed). Full metalness is reserved for genuinely bare-metal signals
(grates, pipes, machined steel, chrome); painted panelling gets 0.2 (a hint of
scuff-through); rust, itself a dielectric oxide, pulls rusted metal to a 0.4 mix.

| category | metalness | roughness band |
|---|---|---|
| bare steel / chrome / grates / pipes | 1.0 | 0.15–0.35 |
| painted station panelling / coated fixtures | 0.2 | 0.45–0.65 |
| rusted / corroded metal | 0.4 | 0.6–0.9 |
| concrete / stone | 0.0 | 0.8–1.0 |
| plastic / rubber | 0.0 | 0.3–0.6 |
| human heads / faces (`skin`) | 0.0 | 0.40 — tight oily sheen so facial detail pops (user request: Blinn-style face sheen, the firefly clamp preventing burn-out). Detection is **mesh-driven ground truth**: every material referenced by a `models/md5/*head*` `.md5mesh` is skin (unless its declared surftype says gear — helmets stay metal, visors glass), which catches the named cast whose faces live on full character sheets (Betruger, Campbell, Swann, `hsarge`), eyes, teeth, and zombie heads; a head/face name-prefix rule under `models/` backs it up. 83 base / 111 d3xp entries |
| body flesh / hell-growth | 0.0 | 0.5–0.6 |
| glass | 0.0 | 0.0–0.05 |
| wood / cardboard | 0.0 | 0.6–0.9 |
| cloth | 0.0 | 0.7–1.0 |

Estimate inputs, in priority order:

```
material name/path ──► category classifier ──► metalness + roughness band
        surftype ─────┘ (override)                      │
                                                        ▼
diffuse texture (rust hues, wear) ──┐
normal-map variance (Toksvig)  ─────┼──► roughness estimate, clamped to band
specular-map mean luminance  ───────┘
```

Everything is overridable by hand per material (§6).

## 4. Runtime — the GGX branch (Phase A)

New shading path in [`interaction.frag`](../neo/shaders/interaction.frag), selected by
`r_pbr 1` (a master toggle, *not* another `r_shading` value — it changes the data flow,
diffuse energy, and specular-map interpretation, not just the highlight shape):

- **Specular:** Cook-Torrance with **GGX** distribution (α = roughness²),
  **Schlick Fresnel**, and Smith/Schlick-GGX geometry-visibility term.
- **F0:** `mix(vec4(0.04), diffuse.rgb, metalness)` — dielectric base 4%, metals tint
  by albedo. The stock specular map modulates specular *intensity/tint* (it's the only
  per-texel specular data we have) rather than being discarded.
- **Diffuse:** Lambert scaled by `(1 - metalness)` and `(1 - F)` for energy
  conservation.
- **Per-texel roughness (Toksvig), baselined + firefly clamp:** the sampled normal's
  sub-unit length (already preserved — vanilla deliberately doesn't renormalize)
  converts to a variance term that widens GGX α per texel. **Calibration saga
  (in-game A/B, 2026-07-30):** full Toksvig over-widened every highlight — on stock
  DXT5nm assets the variance carries a codec-noise floor (compressed normals decode
  short of unit even at the top mip). A first baseline of 0.5 effectively disabled
  the mechanism, whereupon **white-pixel fireflies** appeared on panel seams and
  grate lips: GGX's peak (~1/α², ≈24× at bare-metal roughness 0.32, plus the
  grazing-angle vis blow-up) is orders of magnitude above the bounded ~2.4× energy
  vanilla Blinn assets were authored against, and edge texels aligning with H clip
  to flat white — exactly the texels Toksvig exists to widen. As built:
  **baseline 0.1** (cancels the codec floor, leaves clean |N| > ~0.91 texels
  untouched, reacts to real seam/edge/mip variance) plus a **firefly clamp**
  `lobe = min(D·vis, 6.0)` (the calibrated dielectric peak ~2.2 at roughness 0.58
  is unaffected; only tight/bare-metal/grazing spikes are bounded). (The A/B ran
  via temporary `r_pbrToksvig`/`r_pbrToksvigBase` debug cvars, folded to constants
  and removed per the trim-debug-cvars policy.)
- **Defaults with no table entry:** metalness 0, roughness `r_pbrRoughness` 0.58
  with `r_pbrSpecScale` 1.5 — calibrated in-game against the enhanced-tier
  Blinn-Phong look (exp 16, `r_specularScale` 1.2) on 2026-07-30. Roughness 0.58
  is also what the analytic `α = √(2/(n+2))` power-to-roughness mapping predicts
  for exp 16; the A/B put the perceptual match between 0.5 and 0.58, and theory
  won the tie. The energy scale moved with the Toksvig tuning (see below): ×3
  matched while near-full Toksvig flattened every lobe; with the final 0.2
  baseline keeping lobes tight, ×1.5 is the match (provisional). Either way the
  physical 4%-F0 dielectric lobe needs boosting against the ×1.2 the Blinn path
  uses — Doom 3's stylized specular is overdriven relative to physical dielectrics.
  Not an exact Blinn match by design — GGX keeps its longer tail and Fresnel
  grazing response. This is the Phase A proving state: dielectric GGX everywhere,
  testable on stock assets before any classifier exists.

Parameter flow: `idMaterial` gains `pbrMetalness` / `pbrRoughness` (occlusionmap-style
non-vanilla fields, inert when off); `RhiWorld.cpp` fills a new `u_pbrParms` vec4 in
`RenderParams` (x = metalness, y = roughness, z = enable, w spare). The UBO tail is
known-good past offset 416, and `u_specularParms` is fully occupied, so a new member is
the clean move.

**HDR pairing (decided): PBR must work correctly with `r_hdr` both on and off** — it
never forces or requires the HDR buffer. On SDR the hot GGX lobe simply clamps at 1.0
per light pass (same clamp vanilla specular already lives with); the look degrades
gracefully, nothing breaks. The existing `max(…, 0.0)` floor in `interaction.frag`
already handles the HDR-target sign issue for any new term added there. HDR remains a
*recommendation* (tooltip note, and presets that enable PBR should enable HDR) because
the >1 highlight energy is what the RGBA16F buffer exists for.

**Relation to `r_shading` (decided):** when `r_pbr 1`, the GGX path **supersedes** the
specular shading model — `r_shading` (LUT/Blinn-Phong/Phong) and its tunables
`r_specularScale` / `r_specularExp` are simply not read by the PBR branch. The shader
branches on the PBR enable before the `shadingModel` switch; the cvars keep their
values untouched so switching `r_pbr` back off restores the exact previous look.

## 5. The metals-need-reflections problem (Phase C)

Metalness kills the diffuse lobe — a metal is *defined* by what it reflects — and Doom
3 has near-zero ambient; it's all dynamic lights. Two mitigations, in order:

1. **Phase A safety valve:** effective metalness is clamped
   (`r_pbrMetalnessMax` ~0.8) so metals keep a sliver of diffuse and never go fully
   black between light passes. Under Doom 3's per-light additive model with the GGX
   lobe answering every dynamic light, this reads acceptably in the game's dark
   aesthetic.
2. **Phase C — environment specular:** a roughness-attenuated env term so metals read
   as metal even in fill light. Cheapest credible version: reuse the ambient-cubemap
   machinery from [`ambientlight.frag`](../neo/shaders/ambientlight.frag), sampled
   along the reflection vector with roughness biasing the mip (split-sum lite), scaled
   by `r_pbrEnvScale`. Full IBL (prefiltered mips + BRDF LUT) only if the cheap version
   proves insufficient. This phase decides whether the metalness clamp can be lifted.

## 6. Offline pipeline — classifier + table (Phase B)

A standalone script (`tools/pbr_classify.py`) that:

1. parses every `.mtr` in the pk4 search path (materials + surftype + stage texture
   paths);
2. classifies category via token lists over material path, directory, and stage
   texture names, with declared surftype as override;
3. estimates roughness (normal-map variance + specular luminance, computed by reading
   the pk4 textures), clamps to the category band;
4. emits a plain-text table, one line per material:
   `textures/base_wall/cpuwall2b  metal 1.0 0.32`, plus a coverage/confidence report
   for auditing.

Runtime loads the table at material-parse time (name-keyed lookup), then merges a
**hand-override file** on top (`pbr_overrides.cfg`, same format) — the artist-escape
hatch. Both ship in an **optional pk4** so the enhanced material set is a separate
opt-in download/generation, and the base game stays untouched; with no table present,
`r_pbr 1` still works on Phase A defaults. Note `fs_savepath` shadows `fs_basepath`
for pk4s (see pk4-savepath notes) — document where the table pk4 must live.

## 7. Per-texel maps — the authored future (Phase D, optional)

For authored/replacement assets: `roughnessmap` / `metalnessmap` (or a combined
`rmamap`: R = roughness, G = metalness, B = AO) stage keywords, mirroring exactly how
`occlusionmap` was added (new `stageLighting_t` entry, shortcut keyword, sampler in the
interaction pass, inert on stock assets). Per-texel values override the per-material
scalars where bound. Synthesizing per-texel roughness textures from stock specular maps
is explicitly a non-goal until the scalar pipeline proves itself.

**Phase D backlog, found during Phase B calibration** — stock assets whose
*material decls* (not the engine) block the PBR treatment; all fixable with a
modpack `.mtr` that redefines them with lit stages, zero engine work:

- **Combat blood splatter** (`textures/decals/splat1–20`, `wallsplat*`,
  `bloodyfilmred`, `bloodspray`, `drip*`, `smear01`, `handprint*`): single-stage
  filter decals with no bump — they never enter the lighting pass, so the flesh
  wetness treatment can't reach them (they do multiply-inherit the gloss of the
  surface beneath). Fix: redefine with a generic blood normal map + the existing
  splat art as diffuse/spec mask. (Curiously `gobdrip` already has full stages —
  use it as the template.)
- **Eyeballs** (`models/characters/common/left*/right*`): unlit translucent
  `deform eyeBall` filter decals — same situation; a lit redefinition would
  enable true flashlight catchlights (the `eyes` category + slider already wait
  for it; currently only teeth respond).
- **Weapon viewmodels** (`models/weapons/*`, currently excluded from the table):
  re-add via a curated per-part pass (the `models/md5/weapons/*.md5mesh` shader
  scan gives ground truth) once Phase C env specular makes metal viewmodels
  viable.

## 8. Cvars and UI

| cvar | default | purpose |
|---|---|---|
| `r_pbr` | 0 | master toggle, GGX path + table lookup |
| `r_pbrRoughness` | 0.58 | fallback roughness (no table entry); Blinn-Phong exp-16 width via `α = √(2/(n+2))`, confirmed by in-game A/B (perceptual match sits between 0.5 and 0.58) |
| `r_pbrSpecScale` | 1.5 | artistic energy scale on the GGX lobe — the PBR counterpart to `r_specularScale` (deliberately not shared with it). **Dielectric-weighted**: fades to 1 as metalness rises, because metal F0 comes from the already-bright albedo and boosting it again blew out bare-metal highlights (grate-floor finding, 2026-07-30). Calibration history: 3 matched the Blinn look while full/near-full Toksvig flattened every lobe; once the 0.2 baseline restored tight peaks, 1.5 became the perceptual match (provisional — user still testing) |
| `r_pbrMetalnessMax` | 0.8 | metal diffuse-kill clamp until Phase C env term |
| `r_pbrToksvigBase` | 0.2 | normal-variance baseline before Toksvig widening — the anti-firefly vs highlight-tightness trade (§4) |
| `r_pbrFireflyClamp` | 6 | GGX lobe ceiling — spike suppression; also the bounded core skin rides at (§4) |
| `r_pbrEnvScale` | — | Phase C env specular strength |

**Per-category live values** (same Developer-tab section): the generated table
tags each entry with its category column, and for the five main classes the
values come from cvars at draw time — so these sliders retune the bulk of the
game live. Hand-written `pbr_overrides.cfg` entries are exempt (explicit values
always win); the long-tail categories (wood, glass, cloth, liquid, rust…) keep
their baked table numbers.

| cvar | default | category |
|---|---|---|
| `r_pbrSkinRoughness` | 0.4 | human heads/faces |
| `r_pbrSkinWetness` | 1.0 | specular-energy boost on skin **and eyes/teeth**, modelling the sweat/water film. Deliberately **not metalness** (metallic skin tints and darkens like bronze — wrong physics); a wet look = this raised + skin roughness lowered |
| `r_pbrEyesRoughness` | 0.15 | eyes + teeth (`eyes` category: `left*`/`right*`/`teeth*` from the head meshes + eye-prefixed names under `models/`). **Caveat found in-game:** stock eyeballs are unlit `deform eyeBall` filter decals with no interaction stages, so this effectively drives **teeth only** (plus any lit monster-eye materials); a true eye glint needs Phase-D edits to the eye materials themselves |
| `r_pbrFleshRoughness` | 0.55 | body flesh / gore / hell-growth |
| `r_pbrFleshWetness` | 1.0 | flesh-only specular boost: the slime/gore film on demons, viscera, hell-growth — **and blood**: blood/gore/gib-named materials (including decals, rescued from the skip list) route to flesh so stains glisten. Since most blood decals and all gibs ship with **no specular map** (which would zero the lobe), organic categories (flesh/skin/eyes) with a black spec image get an **organic spec fallback**: the material's own diffuse binds as the spec mask, so the gleam follows the blood shape and density and tints dark red |
| `r_pbrMetalRoughness` | 0.32 | bare metal (metalness 1) |
| `r_pbrPaintedRoughness` | 0.55 | painted station panelling (walls) |
| `r_pbrPaintedMetalness` | 0.2 | scuff-through on painted metal; shared by walls and floors |
| `r_pbrCeramicRoughness` | 0.45 | the `ceramic_sheen` category (renamed from `painted_floor` when the washroom tiles joined): painted floors (`base_floor`/`recyc_floor`) **plus ceramic tile on floors and walls** (`washroom`). Tighter than the wall panelling so lights streak — the wet-floor / glazed-tile look; grate floors stay bare metal |
| `r_pbrRustRoughness` | 0.78 | rusted/corroded metal (named rust/oxid/corro/dirty/stain) |
| `r_pbrRustMetalness` | 0.4 | patchy bare-metal/oxide mix (rust itself is dielectric) |
| `r_pbrStoneRoughness` | 0.9 | rock / concrete / brick |

Weapons caveat: `models/weapons/*` is deliberately **excluded from the table**
(no entries at all) — the viewmodel is the closest, most-stared-at surface in
the game, mostly painted/polymer, and the early bare-metal prior read badly
with no environment reflections at point-blank view. Weapons render on the
global dielectric fallback until Phase C makes metal viewmodels viable
(barrels/receivers would then be natural env-specular showcases).

Rust caveat: the asset vocabulary is sparse (~29 named-rust materials in base) —
most of Doom 3's rusty *look* is painted into the diffuse art of neutrally-named
panels, so the **Painted Metal** sliders are the mass lever for the grungy-panel
appearance; the Rust sliders drive the explicitly-named set. Getting the truly
rusty panels into the rust category automatically would need the Phase-B.2
diffuse-texture analysis (rust-hue detection) from the original pipeline sketch.

All of the above are live per-draw uniforms, tunable in-game from the
**Developer tab → "PBR Materials (GGX)"** section (sliders + a Reload PBR Table
button); values apply instantly, no `reloadShaders` needed.

(The Phase-A-only `r_pbrMetalness` debug global was removed when the Phase B
table landed, per the trim-debug-cvars policy; per-material metalness now comes
from the table, correctable per material via the override file.)

Console: **`reloadPbrTable`** re-reads both table files and re-applies to every
parsed material — override entries tune live in-game, no restart. Pair with
`r_showSurfaceInfo 1` to read the material name under the crosshair.

Enhancements tab: one "PBR materials" toggle (+ advanced sliders in Developer section).
When it's on, the superseded controls grey out via the existing
`ImGui::BeginDisabled()` pattern (see the AA-quality selector in
[`Dhewm3SettingsMenu.cpp`](../neo/framework/Dhewm3SettingsMenu.cpp)): the "Specular
Shading Model" combo (`r_shading`) plus "Specular Scale" / "Specular Exponent"
(`r_specularScale` / `r_specularExp`) in `enhancementOptions[]`, with a tooltip note
that PBR replaces the specular model. Their values are preserved, not overwritten —
toggling PBR off restores the previous look exactly. The HDR toggle stays independent
(never forced); the PBR tooltip recommends it.

Preset placement: off through High (faithful-ish look preserved); candidate for
Ultra/Ultra Nightmare once Phase B/C land and the look is calibrated. Per
trim-debug-cvars policy, calibration-only cvars get folded once tuned.

## 9. Status

- **Phase A — GGX BRDF branch** (scalar params, Toksvig per-texel roughness, defaults
  on stock assets): **built 2026-07-30, pending in-game verification.** As built:
  GGX branch in `interaction.frag` (Schlick Fresnel, Smith-Schlick-GGX combined
  visibility `G/(4·N·L·N·V)`, Toksvig variance folded into α², π-free convention so
  metalness-0 diffuse matches vanilla brightness exactly); `u_pbrParms` appended to
  `RenderParams` (784 → 800 bytes, sizeof-propagated); cvars per §8 including the
  Phase-A-only `r_pbrMetalness` debug global; Enhancements-tab toggle with the three
  superseded specular controls greyed via `BeginDisabled`, values preserved. Vanilla
  path only reordered (the `light` product moved after the branch, identical
  multiplication order); GLSL validated with `glslangValidator` both stages.
- **Phase B — classifier + table + overrides**: **built 2026-07-30, pending in-game
  verification.** As built: `tools/pbr_classify.py` (signals per §6, plus
  substring-token matching for compound names like `sopanel`/`rustpanel`, and
  `models/mapobjects/<sub>` fixture priors) emits `base/pbr/pbr_materials.cfg`
  (3,284 entries: 422 bare metal / 1,982 painted metal @0.2 / 17 rust / 388
  stone / 381 flesh / ~100 other; ~800 unclassified fall back to the globals)
  and a base+d3xp merged `d3xp/pbr/pbr_materials.cfg` (3,639). The first cut
  marked all station panelling metalness 1.0 — "world of gunmetal", corrected
  per the §3 painted-metal note. Runtime: name-keyed
  `idHashTable` in `Material.cpp` loads table + `pbr_overrides.cfg` (override
  wins) lazily at first material parse; `idMaterial` carries
  `pbrMetalness/pbrRoughness` (-1 = no entry); the interaction fill uses them
  with global fallbacks; `reloadPbrTable` re-applies live. `r_pbrMetalness`
  debug global retired.
- **Phase C — environment specular for metals**: **sketched**; decides the metalness
  clamp's fate.
- **Phase D — per-texel map keywords / authored packs**: **optional, deferred**.

Sequencing rationale: A first because it's testable immediately and de-risks the BRDF;
B is pure data work that's meaningless until the BRDF it feeds exists; C is the
open-ended piece and benefits from A+B being observable in-game first.
