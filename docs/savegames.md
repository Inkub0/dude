# Savegames

DUDE loads Doom 3, Resurrection of Evil and The Lost Mission savegames written by:

| written by | header (game / version) | notes |
|---|---|---|
| retail Doom 3 / RoE 1.3.1 | `DOOM 3` / 17 | 32-bit engine; build 1304 |
| dhewm3 1.4 - 1.5.x | `dhewm 3` (`ADoom3` on AROS) / 17-18 | 32- or 64-bit; Lost Mission with the dhewm3-sdk `d3le` library |
| DUDE before revision 1 (0.9.x) | `DUDE` / 18 | |
| DUDE revision 1 on | `DUDE` / 18 + revision 1 | GUI states length-prefixed (below) |

A savegame belongs to the game it was played with: a mod's savegames need the same mod (its game
library writes its own state). Doom 3 BFG Edition savegames are another engine's and do not load.

To continue a game from another engine, copy its `.save` (and `.txt`, `.tga`) files into DUDE's
`savegames` folder of the same game folder (`base`, `d3xp`, `d3le`), under DUDE's save path
(`~/.local/share/dude` on Linux, `Documents/My Games/dude` on Windows, or `fs_savepath`). Retail
Doom 3 keeps them in `<Doom 3>/base/savegames` (the Steam copy included); dhewm3 in
`~/.local/share/dhewm3/<game>/savegames` or `Documents/My Games/dhewm3/<game>/savegames`.

## What differs between them

The game state itself is the same format: DUDE's game code saves and restores exactly as dhewm3's
(which reads retail savegames), and its Lost Mission library as the dhewm3-sdk `d3le` branch.

- **The header** names the engine; DUDE accepts all of the above (`idSessionLocal::LoadGame`).
- **DUDE's revision** rides above dhewm3's version number: DUDE writes
  `SAVEGAME_VERSION | DUDE_SAVEGAME_REVISION << 16` (`Licensee.h`), so the two numberings never
  collide, and an engine that doesn't know the revision (dhewm3, an older DUDE) restarts the map
  with the persistent player data instead of misreading the file.
- **GUI states.** Every GUI in the game (HUD, PDA, the screens in the world) saves its state dict
  and then each window's variables, scripts, registers and transitions in the order its GUI file
  declares them - nothing says how long it is or which version of the file wrote it. Read against
  another version (a widescreen or HUD mod installed or removed since, retail data under a mod) it
  used to crash the load or stop it.

## How GUI states are restored (neo/ui)

`idUserInterfaceLocal::ReadFromSaveGame`:

1. The readers check what the stream allows - each window's name, each named event's name, each
   register's name and type, counts and string lengths against the bytes left - and stop at the
   first mismatch (`GuiRestore_Failed`, `Winvar.h`) instead of reading on out of step.
2. Registers are matched by name (`idRegisterList::ReadFromSaveGame`): a saved register the file
   no longer has (a widescreen pack's `cstAnchor` on a window the stock file leaves bare) is read
   past, its size known from its type; their expression slots stay as the current file parsed them.
3. On a mismatch it rewinds and reads the state with each other copy of the GUI file
   (`FS_ReadFileVersions`): the copies the search path shadows (the stock `pak000` one under a
   mod), then those in `.pk4` files in a `savegames` folder - **a GUI mod's pak kept there is read
   for old savegames only**, never in the game. Each copy reads its `#include`s from its own pak.
   The copy that reads it whole tells where the state ends; the GUI is then laid out by the current
   file with the saved state dict over it (`RestoreValuesOnly`).
4. No copy at all: for Doom 3's player HUD the state ends where the PDA's record begins (the game
   saves them back to back, `idPlayer::Save`), taken only when the PDA's state reads whole from
   there. Anything else stops the load with a message naming the GUI file.
5. 32/64-bit: `idGameSSDWindow` (RoE's arcade on phobos2) saved a struct holding a pointer as it
   lay in memory; a GUI that reads one is tried with both layouts.

New savegames (revision 1) write each GUI's state length-prefixed (`WriteToSaveGame`), so a GUI that
changed since is skipped cleanly, its state dict kept, without needing the old file.

The console prints a `Savegame: GUI ...` line for each GUI restored any way but as saved.

## Testing

`tools/test_gui_savegame.sh` restores every GUI record of real savegames with the engine's own GUI
code (linked from `build/`, stand-in engine interfaces; never starts the game) and reports how each
went, checks Doom 3's HUD ends at its PDA, and round-trips every GUI through the current format:

```
tools/test_gui_savegame.sh base/savegames/*.save
tools/test_gui_savegame.sh -g d3xp d3xp/savegames/*.save
tools/test_gui_savegame.sh -g d3le -b d3xp d3le/savegames/*.save
tools/test_gui_savegame.sh -s ~/old/zWideGuis_D3.pk4 old-saves/*.save      # a pak kept in savegames/
D3BASE="<Quake 4>" tools/test_gui_savegame.sh -q -g q4base ~/.local/share/dude/q4base/savegames/*.save
```

Results 2026-10-28 (0 failures in every set; GUI states restored, savegames in brackets):

| savegames | as saved | otherwise |
|---|---|---|
| current DUDE: base (11), RoE (1), LM (1), sikkmod (1), Quake 4 (10) | 1609, 40, 74, 159, 714 | - |
| retail Doom 3 from Steam (4) | 647 | 4 PDAs from the stock copy (the widescreen pack's PDA installed) |
| a DUDE save with a dhewm3 header (1) | 159 | - |
| DUDE 0.1-0.9.6, most with the widescreen pack's HUD since removed: base (146), RoE (18), LM (4) | 18,551 | the 164 pack HUDs found by the PDA; with the old pak in `savegames/`, from that copy |
| retail Doom 3: Phobos (115, 32-bit) and its DUDE saves (6) | 17,426 | 16 `GameSSD.gui` in the 32-bit layout |
