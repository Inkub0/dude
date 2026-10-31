# Emissive mode: authored lighting and the in-game Light Editor

**Status: PLAN, 2026-10-29.** Branch `feat/rtx-light-editor`, stacked on `fix/rtx-emissive-drop-consistency` and
`feat/rtx-emissive-flicker` (today's emissive work, not merged). Replaces the automatic emissive census with authored,
per-map data and an in-game editor. Emissive mode = `r_rtEmissiveLighting`, Path Tracing tier (`dude_rtx 2`) only.

## Why

The census guessed, from geometry alone, which placed lights a glowing surface replaces. It dropped a light when a
fixture sat near its origin and paired each emitter with one light. A day of fixes showed the guess cannot be right:

| Case found 2026-10-29 | What the heuristic did |
|---|---|
| A door frame with a strip on each side (COMBAT PREP) | Paired the strip with the light behind the door |
| A ceiling wash standing for five fixtures (Marine Command Access) | Handed its whole power to one panel, and again to a kiosk screen |
| Round ceiling decals with 32-unit halo lights | The decals inherited the halo, so their light never reached the floor |
| Two lights at one fixture | One was dropped with nothing replacing it |

Power sharing then did nothing visible for the ceiling lights. The mapper's intent is not in the geometry, so it has to
be authored. User, 2026-10-29: "get rid of heuristics… an in-game light editor that can delete lights, and can create
emissive surfaces from self lit textures. It should show all lights and the user should be able to just look at them
to edit."

## Decisions (user, 2026-10-29)

1. **A map with no authored file: nothing changes.** All placed lights stay and no surface emits. Emissive mode on an
   unauthored map looks like normal mode.
2. **Deleting a light affects emissive mode only.** Normal mode keeps every vanilla light, so fidelity is untouched.
3. **One tool.** The Light Editor replaces the Emitter Editor window; `editEmitter` becomes an alias.
4. **Phase 1 also includes:**
   - an emitter that follows a light's animation (flicker / pulse);
   - a one-shot "match a deleted light's power" button;
   - editing a light's colour, brightness and radius (emissive mode only);
   - a material-wide emit toggle with per-surface exceptions.

## What stays and what goes

**Stays (the mechanism):**
- The emitter triangle table: every self-lit triangle of the world and static entities, plus mover re-bake and state
  re-gather.
- The per-fixture split (world and entity glow runs: one row per connected piece).
- Emitter rows in the scene-light buffer, area-light shading (per-sample cosine), the light grid and the DI shade.
- Flicker scaling (live / average of a light), the gradient's factor comparison, and `emitters.cfg` as the **global
  material library** (radiance scale, tint, profile, two-sided).

**Goes (the guessing), in stage L5:**
- The census drop rule.
- Triangle and row pairing (fixture / emitting side / largest reach).
- Power sharing and the keep-uninherited pass.
- The power match as an automatic rule (its formula survives as the L4 button).
- The automatic indicator rule (minimum effective area).
- Flare inference.
- The `rtEmissiveLights` diagnostic.
- The per-fixture `light <id>` lines in `emitters.cfg` (they move to the per-map file).

## Data model

One text file per map: `<fs_savepath>/<game>/emissive/<map>.cfg`, e.g. `base/emissive/game/mars_city1.cfg`. It is
written by the editor and readable by hand.

```
// DUDE emissive setup for game/mars_city1 (Light Editor; emissive mode only)
light "tim_light_3701" delete
light "tim_light_3697" color 1.00 0.90 0.80 brightness 1.2 radius 230 100 150
light @-2688,-2144,282 delete                 // a light the .map names nothing for: keyed by its spawn position
material "textures/base_light/striplight3" emit
fixture 16e9a41f488c47e1 emit scale 2.0 tint 1 1 1 profile 1 twosided 0 reach 300 follow "light_4839"
fixture 3fb7735ee5af2439 off                  // an exception to a material-wide emit
```

**Light identity.** The renderer has no entity names, and `renderLight_t` cannot change (the dhewm3 ABI is frozen).
- At the emitter-table build, the engine parses the map's `.map` (idlib `idMapFile`, only while emissive mode or the
  editor is on).
- It builds a table from spawn position to name for `light` entities, and matches each lightDef by its origin.
- Lights the `.map` does not name are keyed by spawn position. Runtime lights (projectiles, muzzle flashes) never match
  and are not editable.

**Surface identity.** The existing fixture id (hash of the run's world centroid, 0.1-unit quantised) and the material
name.

**Emitter brightness.** The `emitters.cfg` material radiance scale is the default. A fixture entry can override scale,
tint, profile, two-sided and reach. There is no inheritance from placed lights. "Match power" writes a scale once.

## Engine semantics (emissive mode on and an authored file loaded)

- **Emitter rows:** only for fixtures that emit, either their own `emit`, or their material's `emit` without an `off`
  exception. Brightness comes from the material library times the fixture override. The reach is authored, or the
  default reach.
- **Deleted lights:** left out of the scene-light buffer and marked DI-covered, so the forward path skips them too. This
  is today's drop path, keyed by the authored set instead of the census serial. That also fixes the "updated light comes
  back" serial bug for authored deletions.
- **Light overrides (colour / brightness / radius):** applied in `idRenderWorldLocal::UpdateLightDef`. The renderer
  keeps the game's pristine `renderLight_t` per lightDef (`idRenderLightLocal` is renderer-internal, so no ABI change)
  and derives the light from the overridden copy. An edit or a mode toggle re-runs the derive from the pristine copy.
  Normal mode never applies them.
- **Follow:** an emitter linked to a light scales by that light's live / average brightness. This is the flicker code,
  with the link authored instead of paired.

## Editor UX

**Look mode.** The `editLights` command toggles it; bind it to a key. While it is on:
- Every placed light is drawn as a small sphere:
  - white = active;
  - red = deleted;
  - yellow = overridden;
  - cyan = the one under the crosshair.
- Every self-lit surface is tinted:
  - green = emits;
  - grey = can emit;
  - orange = an `off` exception.
- A one-line label under the crosshair names what you are looking at, e.g. `light "tim_light_3701" (biground1, r 32) -
  active` or `surface striplight3, fixture 16e9…, emits x2.0`. The nearer of the light sphere (ray-sphere test) and the
  surface hit wins.
- You keep playing and looking around.

**Panel.** The bound key on a hovered object opens the ImGui panel on it (the mouse goes to ImGui). Closing the panel
returns you to look mode.

- **Light panel:**
  - name, material, origin, colour, radius;
  - **Delete in emissive mode** / Restore;
  - colour, brightness and radius sliders (live) with Reset;
  - "Make an emitter follow this light" (arms a pick).
- **Surface panel:**
  - material, fixture id, centroid;
  - **Emits** (this fixture) and **All of this material in this map**, with an exception toggle;
  - radiance scale, tint, profile, two-sided, reach;
  - Follow light (pick a light);
  - Match power of light (pick a light; sets scale and reach once).
- **Always available:** Save (per-map file), Reload, Revert all; the counts of deleted lights, overrides and emitting
  fixtures.

## Stages (one branch each, Vulkan only, GL3 untouched)

1. **L1 `feat/rtx-light-editor-data`:**
   - the per-map file (parse, apply, save), the `.map` name table, the deleted / overridden / emit data in the engine;
   - console commands to edit without UI (`lightDelete`, `lightRestore`, `emitOn`, `emitOff`, `emissiveSave`) on the
     object under the crosshair;
   - emissive mode reads only the authored data (unauthored map = no change).
   - Test: author a room by commands; the file round-trips.
2. **L2 look mode:** the gizmos (light spheres, surface tints), hover pick, and the hover label.
   - Built (`feat/rtx-light-editor-look`): `editLights [0|1]`. The frontend (`R_EmAuthLookFrame`, once per primary
     view) picks and builds the gizmos; the table overlay pass draws them (`emissive.look` GPU scope, shader modes
     6 / 7). Spheres scale with distance and show faint through walls (x-ray); lights in areas the view did not reach
     are skipped, and so are lights within 40 units of the eye (flashlight, muzzle flash).
   - The highlighted light also shows its **volume** (the edges of its frustum windings: a point light's radius box,
     a projected light's frustum); a highlighted surface shows its **reach** as three circles (user suggestion,
     2026-10-31). `lightPick` in look mode selects a light so its volume stays drawn; `lightPick` on nothing clears it.
   - The light commands act on the highlighted light (else the selected one); the emit commands keep their own
     surface pick, which is the same trace.
   - The label is drawn on the console's 2D debug pass (the emissive labels' path), not the ImGui foreground, so
     screenshots include it.
3. **L3 panels** - built as a **right-click menu** (user, 2026-10-31: "pointing at a light and right clicking, should
   open a menu with all the available options to that light"), branch `feat/rtx-light-editor-menu`:
   - In look mode a right-click in game never reaches the game's binding (sys/events.cpp -> `R_EmAuthMouseButton`). It
     freezes what the crosshair points at and opens an ImGui window at the crosshair, with the cursor placed on it.
   - **Light:** name, material, origin, radius, colour; Delete in emissive mode / Restore; "Make a surface follow this
     light..." (the next right-click on a surface links it); Keep its volume shown; Copy name.
   - **Surface:** material, fixture id, state; this surface as its material / emits / off; every surface of the
     material emits; Brightness and Reach sliders (live) with Reset; Follow a light... (the next right-click on a
     light) / Stop; Clear this surface's settings; Copy fixture id.
   - **Nothing under the crosshair:** the map's file and counts, Recover / Discard the recovery copy, Leave look mode.
   - Every menu: Save / Reload / Close, and "Turn it on" when emissive mode is off. Esc, a click outside (a left
     click there closes the menu instead of firing) or a lost focus closes it.
   - **Appearance** (the surface menu, from the old Emitter Editor - user, 2026-10-31): Physical radiance + Radiance x,
     Tint, Emission profile, Faces (one- / two-sided), for the material or this surface only (its own value wins, else
     the material's, else the default). Live: they ride on the emitter row (rt_emissive.glsl projS.xyz = tint,
     projT.z = profile, projT.w = two-sided; the radiance is the row's intensity), no table rebuild, no Apply.
     Without a radiance a surface's brightness is power-matched to its reach; with one it is physical. Turning
     physical on starts from the matched value, so the look holds. The per-surface Brightness multiplies on top.
   - **emitters.cfg and the Emitter Editor are gone** (user, 2026-10-31: "dump the emitters.cfg file altogether and make
     all the changes converge in the new system"): the table bakes no per-material values any more, the bake / save /
     reload commands are removed, `editEmitter` is an alias of `editLights`, and the settings menu's Emissive section
     has an "Enter look mode" button instead. Per-map file syntax: `material "<m>" [emit] [look]`,
     `fixture <id> [emit|off] [scale x] [reach r] [follow "<light>"] [look]`, look = `[radiance <x> | radiance matched]
     [tint r g b] [profile tube|lambert|uniform] [twosided 0|1]`.
   Original plan text: the ImGui Light Editor replaces the Emitter Editor: the light and surface panels, the material-wide
   toggle, live edits, save / reload.
4. **L4:** follow-a-light, match-power, and the light colour / radius overrides through `UpdateLightDef`.
   Built on `feat/rtx-light-editor-overrides` (2026-10-31):
   - **Light overrides** (light menu, "Colour, brightness, radius"): colour replaces shaderParms 0-2, brightness
     multiplies them, radius replaces a point light's lightRadius. `idRenderWorldLocal::UpdateLightDef` keeps the
     game's parms as `idRenderLightLocal::dudePristine` (renderer-internal: no ABI change) and derives from an
     overridden copy (`R_EmAuthOverrideLight`); `RenderScene` re-derives every affected light when the data or the
     mode changes (`R_EmAuthRefreshLightOverrides`). Emissive mode only; lights keep their key (the game's origin).
     A light material whose stages ignore the colour parms shows no colour change.
   - **Match a light's power** (surface menu, then right-click a light): reach = the light's radius, radiance = the
     power match against its luminance (physical), Brightness reset. One-shot.
   - **New emitters are physical** (user, 2026-10-31): emitOn / emitMaterial / the menu / Ins on a surface with no
     radiance in effect sets the radiance to its current power match - same look, physical from then on.
   - **Keys in look mode** (user, 2026-10-31): Del deletes the highlighted light (again: restores it) or stops the
     highlighted surface emitting; Ins makes it emit. The **wheel** changes the highlighted surface's radiance by 10 %
     a step (Ctrl: its material's) or the highlighted light's brightness; with nothing highlighted the wheel stays the
     game's. Taken in sys/events.cpp only in game (no menu, no console, ImGui not capturing).
5. **L5 cleanup:**
   - delete the heuristics listed above (and their cvars: `r_rtEmissiveShareExponent`, `r_rtEmissiveFixtureMinArea`
     and `FullArea`, `r_rtEmissivePowerMatch`, `r_rtEmissiveFlareInfer`, `r_rtEmissiveReach` and `ReachDefault` if
     unused), plus the `rtEmissiveLights` diagnostic;
   - migrate the `light <id>` lines of `emitters.cfg`;
   - docs.
   Done on `feat/rtx-light-editor-cleanup` (2026-10-31):
   - **Removed:** the light census (fixtured / ambience classification, per-triangle light pairing, the drop list),
     power sharing, the paired-light power inheritance and its reach, the effective-area "indicator" rule and the
     power match's area ramp, the `rtEmissiveLights` diagnostic, overlay modes 4 (light coverage) and 5 (placed lights
     by census class - look mode shows the lights), the cvars `r_rtEmissiveShareExponent`, `r_rtEmissivePowerMatch`,
     `r_rtEmissiveFixtureMinArea`, `r_rtEmissiveFixtureFullArea`, and the row fields they fed.
   - **Kept:** the power match as a plain formula (`R_RtEmissiveMatchedScale( row, reach, lightLum )`: a surface
     without a radiance, Match a light's power, the start of physical), `r_rtEmissiveMaxRadiance` (its cap),
     `r_rtEmissiveReach` (the reach multiplier / knee) and `r_rtEmissiveReachDefault` (both used), flare inference
     (`r_rtEmissiveFlareInfer`: it only makes flare-lit fixtures CANDIDATES the editor can switch on - it decides
     nothing), flare hiding, the fixture split.
   - **Behaviour:** normal mode's bounce-only rows (`r_rtEmissiveBounce`) now use the authored set too (an unauthored
     map: none). A surface still power-matched (no radiance) that is small is no longer held at its painted glow by the
     area ramp: it now matches like any other (capped by `r_rtEmissiveMaxRadiance`).
   - The `light <id>` lines of `emitters.cfg` needed no migration: the user's file had none (folded and removed with
     the file).
6. **Merge:** the whole stack (flicker, today's fixes, the editor) after the user's play test.

## Risks

- **Light identity.**
  - A light the game moved before the build, or a savegame that restores a moved light, has no name match. It falls
    back to the position key, which then differs. Static lights (nearly all) are fine.
  - Same-position duplicates (two lights at one spot): the name decides when present, else the spawn order.
- **ImGui input.** The panel takes the mouse; look mode must not. Hover runs with the game's own view and the label
  draws on the ImGui foreground list.
- **Savepath shadowing.** Per-map files live in the save path (the `pk4-savepath-shadow` gotcha). The editor shows the
  path it writes.
- **Rebuilds.** Edits must not rebuild the whole table. Emit / scale / delete are per-row or per-light flags applied at
  the next gather. Only a material-library change re-bakes triangles, as today.
- **Perf.** Fewer rows than today (only authored emitters), so the gather and grid get cheaper. Light-radius overrides
  re-derive one light.
