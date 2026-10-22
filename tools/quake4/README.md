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

Nothing here touches the install: the scripts only read the `.pk4` zip archives.
