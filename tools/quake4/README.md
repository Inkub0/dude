# tools/quake4 — Quake 4 asset audit scripts

Read-only probes over a retail Quake 4 install (Steam 1.4.3 / GOG / 1.4.2 patched disc)
that measure how far Raven's formats drifted from Doom 3. They were used to size the
Quake 4 milestone ([docs/quake4-port.md](../../docs/quake4-port.md)) and should be re-run
whenever a loader or parser is extended, to confirm the gap actually closed.

Both scripts default to the Steam path on the dev box; override with `Q4BASE`:

```bash
Q4BASE="/path/to/Quake 4/q4base" python3 tools/quake4/q4_inventory.py > /tmp/q4_inventory.txt
```

- `q4_inventory.py` — per-pak file counts by extension, top-level directory histogram, and
  the first ~28 lines of every format-bearing file type (`.proc`, `.cm`, `.aas*`, `.map`,
  `.md5mesh`, `.md5anim`, `.mtr`, `.gui`, `.fx`, `.sndshd`, `.lipsync`, `.playback`, ...).
- `q4_keywords.py` — keyword-level drift: harvests the leading keywords Quake 4 actually
  uses in materials, GUIs, BSE effects, sound shaders and decl files, and marks (`*`) the
  ones DUDE's parsers (`neo/renderer/Material.cpp`, `neo/ui/*.cpp`,
  `neo/sound/snd_shader.cpp`) do not accept. The BSE section is a complete census of
  segment types, per-segment properties, spawn domains and envelope names across all
  1180 effect files. Also lists every `spawnclass` the `.def` files reference (the
  game-library surface) and the decl types per file extension.

- `q4_gui_census.py` — GUI parser check: walks every Quake 4 GUI the way DUDE's window parser
  does and lists (1) window properties that would **desynchronise** the parser (multi-value
  properties DUDE does not know; the generic fallback keeps one token), (2) harmless unknown
  single values, (3) event, `onTime` and script-command usage, with the commands DUDE does not
  implement. Its "known" tables mirror `neo/ui/*.cpp`: update them together. After Q2's GUI
  work it reports zero desync sources other than three quirks in the retail files themselves.
- `test_guides.sh` (+ `check_guides.py`, `tests/guide_expand_test.cpp`) — material guides:
  compiles the engine's own `neo/framework/DeclGuides.cpp` against `build/libidlib.a`, expands
  every retail `.mtr`, and diffs the result byte for byte against an independent Python
  expansion and line map, plus structural checks (one statement per line, braces, no guide left, every guide-defined
  material present). Needs a configured `build/`.
- `q4_material_census.py <expanded materials dir>` — lists every material/stage keyword in the
  guide-expanded `.mtr` files (the `out/materials` of `test_guides.sh`) that
  `neo/renderer/Material.cpp` does not know; an unknown keyword defaults the whole material.
  Also lists image-program functions (`map downsize( x, 1 )`) that
  `neo/renderer/Image_program.cpp` does not know. After Q2 it lists none (`snap` is a false
  positive: a `table` option; `fp20Program` is in Raven's own test material).
- `q4_proc_check.py <pk4 glob>[,<glob>...]` — a replica of the engine's .proc loader
  (`neo/renderer/RenderWorld_load.cpp`): walks every file token by token in the loader's order,
  every Expect and count, for Doom 3's `mapProcFile003` and Quake 4's `PROC "4"`. Run it on both
  titles after touching the loader. (A field-by-field check passed while an area model's sky
  number - a token the loader did not expect - stopped `airdefense1` loading; only a walk in
  the loader's order catches that.)
- `test_cm_parse.sh` (+ `tests/cm_extract.py`, `tests/cm_parse_test.cpp`) — collision files:
  copies the engine's `.cm` loader out of `neo/cm/` verbatim (every function from
  `LoadCollisionModelFile` down, the contents-name tables, the block allocators), compiles it
  with the real `idLexer` from `build/libidlib.a`, and loads every `.cm` of the Quake 4 paks and
  of Doom 3 + RoE + Lost Mission (`D3PAKS` overrides). Reports failures, counts, the contents
  bits seen and how well each model's preallocated block fits its rows. Needs a configured
  `build/`. Run it after touching `neo/cm/CollisionModel_files.cpp`: a replica cannot see lexer
  mechanics (an `UnreadToken` after a `PeekTokenString` drops a token; the replica passed it,
  this test fails 47 of 83 Doom 3 files on it).
- `test_aas_parse.sh` (+ `tests/aas_parse_test.cpp`) — navigation files: compiles the engine's
  own `neo/tools/compilers/aas/AASFile.cpp` and `AASFile_sample.cpp` (only the lexer's file open
  is redirected to memory) with the real `idLexer`, and loads every `.aas*` of the Quake 4 paks
  (`DewmAAS "1.08"`) and of Doom 3 + RoE + Lost Mission (`1.07`). Reports failures and warnings
  (the loader's feature self-check included), areas, clusters, dummy files, tactical features.
  Needs a configured `build/`.
- `q4_cm_check.py <pk4 glob>[,<glob>...]` — the Python replica of the same loader: also checks
  that a Quake 4 block header's counts match the rows and that every brush contents name is
  one `cm_contentsNameByIndex` knows.
- `q4_cm_texcoords.py <q4base> [map ...]` — what a revision-3 polygon's three `( s t )` pairs
  are: inverts them through the brush side its primitive names (`.map` texture matrix) and
  reports whether the points are polygon vertices (in which order) or not, plus the polygons
  whose material has a hit map. Result (Q3): exact face coordinates at three unstored points -
  vertices in no fixed order for ~80%, merged-away interior points for the rest.
- `q4_sound_census.py [q4base] [language]` — parses every sound shader with the grammar
  `neo/sound/snd_shader.cpp` accepts for Quake 4, resolves every sample the way the engine does
  (bare names, `sound/vo/` → `sound/vo_<language>/`), and checks every `maps/**/*.reverb` area
  table against the presets in `efxs/default.efx`. After Q2: one broken retail shader
  (`leadin_test`), 539 samples absent from the retail paks, 0 reverb problems.

- `smoke-maps.sh [vulkan|opengl3|legacy] [map ...]` (+ `smoke_summary.py`) — the null-game
  map sweep (docs/quake4-port.md §7 item 3): starts `run-q4.sh` once per campaign map
  (Appendix C; `process1_first` → `map game/process1 first`), waits `WAIT` frames, quits, and
  keeps each run's console output in `logs/q4-sweep-<renderer>-<date>/`. The summary lists per
  map the load result and time, lights/models dressed, AAS areas, `.cm` header mismatches,
  warnings and errors, then every distinct warning/error with its maps. It runs the game - it is
  the user's to start. Settings come from a copy of `~/.config/dude/q4base`, so the sweep's
  `+set`s never reach the real `dude.cfg`; `SHOTS=1` adds a screenshot per map.

Nothing here touches the install: the scripts only read the `.pk4` zip archives.
