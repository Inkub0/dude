# Ultra Nightmare difficulty

A fifth difficulty (`g_skill 4`) in the spirit of the later Doom games' Ultra-Nightmare:
**Nightmare's rules with one life.** Unlocked together with Nightmare (finishing the campaign
sets `g_nightmare 1`); until then the row shows as locked.

## Rules

| | Nightmare | Ultra Nightmare |
|---|---|---|
| damage taken, health drain to 25, no medkits, Soul Cube / Bloodstone at start | yes | yes (identical) |
| manual saves (menu, `savegame`, quicksave F5) | yes | **no** — a message box explains |
| autosaves | one `Autosave_<map>` per map, kept | **one checkpoint slot per run**, rewritten at every map start |
| death | restart menu: replay level / load / main menu | **the run is over**: the checkpoint is erased, the same restart menu offers only *Main Menu* |
| `com_disableAutoSaves` | honoured | ignored — the map-start checkpoint is part of the mode |

A run's checkpoint shows up in Load Game as `Ultra Nightmare: <map name>` (red tag). Loading it
resumes the run from the start of that map, and that file becomes the run's slot again.
Loading any other save leaves Ultra Nightmare mode; the abandoned run's checkpoint stays on
disk until that run is resumed and dies.

Cheat map loads (`devmap`) never write the checkpoint, same as vanilla autosaves.

## How it is built

**Game DLLs** (`neo/game`, `neo/d3xp`, `neo/d3le` — the last is a source copy of d3xp and must
be kept in step): every Nightmare check became `g_skill >= 3` (or `case 3: case 4:`), and
`idGameLocal::SetSkill` clamps to 4 instead of 3. Skill was already written into the savegame
(`idGameLocal::SaveGame`) so it round-trips on load. Nothing else in the DLL knows about the
mode: a third-party mod DLL that still clamps to 3 simply plays Nightmare rules while the
engine keeps enforcing the save/death rules below.

**Engine session** (`neo/framework/Session.cpp`, `Session_menu.cpp`, `Session_local.h`):

- `StartNewGame`: `g_skill >= 4` → `BeginUltraNightmareRun()` creates the slot name
  `UltraNightmare_<YYYYMMDD_HHMMSS>` (one file per run; parallel runs on one machine never
  collide). Any other new game clears the state.
- `MoveToNewMap`: in a run, the map-start autosave goes to the run's slot with the
  `^1Ultra Nightmare:^0 <map>` description instead of `Autosave_<map>`.
- `SaveGame`: refuses every non-autosave write during a run (message box + console line).
  When writing the checkpoint it adds `dude_ultraNightmare 1` to the header's persistent
  player dict; the key is removed again from the in-memory copy so it never reaches the
  game DLL.
- `LoadGame`: reads and strips that header tag; a tagged file re-enters the mode and
  becomes the slot, an untagged one leaves it. Older saves are untouched (no format change).
- `RunGameTic` `"died"`: in a run, `EndUltraNightmareRun( true )` deletes the slot's
  `.save/.txt/.tga`, then shows the game's own restart menu (`guis/restart.gui`, whichever
  variant is loaded) through `ConfigureRestartMenu( true )`: its `Restart` and `Replay` (load)
  rows are hidden and `Quit` / `Quit2` (Main Menu and its hint) move into the top row's place.
  `HandleRestartMenuCommands` also refuses `restart` / `loadGame` / `saveGame` / `deleteGame`
  while that state is set, in case a mod's menu names its windows differently. A normal death
  calls `ConfigureRestartMenu( false )`, which restores the rows (one gui instance serves both).
- `SetMainMenuGuiVars` publishes `gui::ultraNightmare` (1 while a run is live) so the
  in-game menu can lock Save Game.

**GUIs** (`base/guis/`, mirrored into `fs_savepath` at startup like the rest of the DUDE menu,
`mainmenu.gui` ships in `zz_dude_menu.pk4` — `docs/releasing.md`):

- `mainmenu.gui`: the New Game difficulty panel moved up one row (its frame is now 101 px
  tall from y=298) and gained row 5, `NGBtn5*` / `NGBtnText5` / `NGBtnText5Alt`, a clone of the
  Nightmare row that sets `gui::skill 4`; the reveal (`onTime 500/550`) and hide lists were
  extended. In-game, `SaveGameBtnText` hides and `SaveGameBtnTextUN` (greyed, error blip,
  tooltip) shows while `gui::ultraNightmare == 1`; the button frames use the grey variants.
- The death screen is not a separate file. A first version shipped a clean-authored
  `restart_ultranightmare.gui`; it was dropped after the first in-game look (solid scanline
  stripes, 4:3 pillarbox) in favour of reusing the real restart menu, which is identical to a
  normal death by construction and needs no shipped asset.

## Known limits

- **RoE / Lost Mission menus**: their main menu comes from the widescreen paks in `d3xp/`
  (not DUDE-authored), so they have no fifth row yet. The mode itself works there: start with
  `g_skill 4` + `map <first map>` from the console. This resolves itself when the DUDE
  main menu replaces those too (docs/codebase-independence.md).
- The graphics quality preset ladder also tops out at a tier called "Ultra Nightmare"
  (docs/todo.md); the two are unrelated and live in different menus.
- Finishing the campaign does not erase the run's last checkpoint (the game only sends
  `disconnect`, which is indistinguishable from quitting to the menu). It can be replayed
  from the last map start.

## Verifying (manual, in-game)

1. With `g_nightmare 1`, New Game shows five rows; the fifth starts a run and the console
   prints the checkpoint slot name. Load Game lists `Ultra Nightmare: Mars City`.
2. F5 / the in-game Save Game button / `savegame foo` all refuse with the message box.
3. A map transition rewrites the same slot (still one Ultra Nightmare entry in Load Game).
4. Die: the normal death menu appears with only *Main Menu* (in the top row); the slot is gone from Load Game
   and from `savegames/` on disk.
5. Load a normal save afterwards: saving works again.
