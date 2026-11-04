# Quake 4 in a release: licence safeguards

Status 2026-11-04, for 0.9.8 (the first release meant to run Quake 4 out of the box). The
publishing decision itself is in [quake4-port.md](quake4-port.md) §3 and §9: the game library is
published the way OpenQ4 publishes its own, under the Quake 4 SDK licence and apart from the
GPL engine. This page is about doing that with as little exposure as possible. It is not legal
advice; only a written OK from the rights holders settles the questions listed at the end.

## Where the risk is

| Piece | Licence | State |
|---|---|---|
| DUDE engine, including the Quake 4 loaders and BSE | GPLv3 | Provenance-checked (below): the only text shared with the SDK is the interface the game library needs |
| `dude-q4` source (github.com/Inkub0/dude-q4) | Quake 4 SDK EULA | 14 commits carry dhewm3 fixes (dhewm3's contributors' GPL code in EULA files) |
| `q4base.so` / `.dll` | both | Raven's game code (EULA) statically linked with DUDE's idlib (GPL): the one combination neither licence allows. OpenQ4 ships the same kind of binary and states the question is unresolved |
| Retail Quake 4 data | id Software / ZeniMax | never shipped, enforced by a release gate |

## Tools

| Tool | Checks | When |
|---|---|---|
| `tools/quake4/check_no_retail_data.py` | no file in a release archive (nested pk4/zip included) has the content of a retail Quake 4 file, or replaces one by path in the same game folder | every release archive, both platforms; `package-q4.sh` runs it on the Quake 4 archive itself |
| `tools/quake4/check_provenance.py` | DUDE code written since the Quake 4 work began has no runs of code from the decompile-derived BSE trees (OpenQ4 `src/bse`, idTech4A++ `raven/bse`) or from the SDK beyond the interface | before every release; exit 1 = stop |
| `tools/quake4/q4base_licence_audit.py` | which DUDE source lines produce code in `q4base` (DWARF line tables) and whose they are (git blame) | before every release; numbers go in the notes below |
| `tools/quake4/package-q4.sh` | builds the separate `dude-q4-<ver>-<platform>` archive: library + unmodified EULA + README + BUILDINFO; refuses an uncommitted build, a changed EULA file or removed SDK copyright notices | release packaging ([releasing.md](releasing.md)) |
| `tools/quake4/build-q4game.sh` | now also writes `q4base.buildinfo` (DUDE and dude-q4 commits, compiler) next to the library | every build |

## Measured: the GPL code inside q4base (2026-11-04)

`q4base_licence_audit.py` on the library as built today (DUDE's default flags, all of every
idlib object the linker pulls in), and on a trial link with `-ffunction-sections` idlib +
`--gc-sections` (only the functions the game reaches):

| Class | Meaning | Today | gc-sections trial |
|---|---|---|---|
| id-sdk | id's GPL code whose text is also in the SDK (we hold it under the EULA too) | 22 582 (91.4 %) | 12 161 (87.2 %) |
| id-gpl | id's GPL code not in the SDK | 697 (2.8 %) | 597 (4.3 %) |
| own | DUDE author's code (a GPLv3 §7 permission can cover it) | 1 080 (4.4 %) | 1 050 (7.5 %) |
| third | dhewm3 and other contributors | 340 (1.4 %) | 140 (1.0 %) |

Substantive code lines (braces and bare `return;` counted apart). Readings:

- **third**: 340 lines from 58 dhewm3 commits; with gc-sections 140 lines from 42, the largest being dhewg's
  x86_64 heap fix (19), Daniel Gibson's `idParser` date/time fix (12) and paths-with-dots fix (12),
  then small 64-bit renames (`ReadLong` -> `ReadInt` and the like). Daniel Gibson's UTF-8
  functions (89 lines) drop out completely with gc-sections.
- **id-gpl** is mostly SIMD: the GCC paths of `Simd_SSE.cpp` / `Simd_SSE2.cpp` (id wrote them
  for the Linux port, after Raven's fork) and `Simd_Generic.cpp` signatures that differ from
  Raven's. Part of it is cosmetic (`Curve.h` `this->` for GCC templates).
- **own**: the Quake 4 interface layer and Raven-method ports in idlib. Six of its commits name
  OpenQ4 / idTech4A++; all six say "consulted, our own code" (read 2026-11-04).
- The dude-q4 repository's 14 dhewm3-carrying commits: `bcff329` (64-bit event and script
  system, +712/-680, much of it the regenerated callback table) is the substantial one; the
  rest are 1 to 48 lines.

## Provenance findings, fixed 2026-11-04

`check_provenance.py` (24-token windows, everything already in DUDE before the Quake 4 work
removed first, SDK interface text not held against the decompile-derived trees):

- **BSE** (`neo/renderer/bse/BseEffect.cpp`): five spots matched OpenQ4's decompile-derived
  `src/bse` token for token, names included (`bseCubeNormals`, `edgeY`/`edgeZ`, `planar`/`inv`,
  the spiral rotation, the emitter attenuation). Their commits had recorded the consultation,
  but the result was a copy in all but layout. Re-expressed; an equivalence test (old vs new,
  4 M fuzzed + edge cases, -O0 and -O2) gives bit-identical results.
- **idlib**: `idMath::Lerp`, `idMath::MidPointLerp` and `idDict::RandomPrefix` matched the SDK's
  text (Raven idlib methods added to DUDE's GPL idlib in Q5.2). Re-expressed with identical
  behaviour, the random generator included (20 000 picks compared).
- **Left as is**: interface declarations (signatures, struct layouts, flag values: needed to
  load the game library, the same in every port), the small inline helpers of the playback
  structs in `DeclQuake4.h`, and one-liners with only one way to write them (`FindEdgePoint`
  delegating to its centre form, `ReplaceChar`, `Cube`, `idVec3 = idVec2`).
- Not covered by the tool: comments (stripped before comparing). Comments near consulted code
  should be read by a human; the ones in the rewritten BSE spots were rewritten too.

## Release rules for Quake 4

1. **Separate download.** `dude-<ver>-<platform>` stays GPL-only and never carries the game
   library or the EULA; `dude-q4-<ver>-<platform>` carries only those (+ README, BUILDINFO).
   A takedown then touches the Quake 4 archive only.
2. **Both repositories tagged `v<ver>`**, the library built from those commits
   (`q4base.buildinfo` says which; `package-q4.sh` refuses an uncommitted build).
3. **Non-commercial** (EULA §2/§3(f)): no donation or sponsor link in the dude-q4 repository,
   its README, the Quake 4 archive or the Quake 4 part of the release notes, and no fundraising
   that mentions Quake 4. DUDE's own donation link stays on DUDE's README.
4. **No retail data, edited or whole** (`check_no_retail_data.py` on every archive), and no data
   generated from Quake 4's maps (baked AO, env probes, emissive tables): the engine makes those
   on the player's machine.
5. **Trademarks**: the product is DUDE; Quake 4 is named only to say what it runs ("runs
   Quake 4, not included"); no Quake 4 logo or art; the non-affiliation line in every README.
6. **Full retail game only** (EULA §2 excludes "any demo, test, or other version"); multiplayer
   (later) must never bypass the CD-key check (§3(m)).
7. **SDK notices kept** (EULA §3(l)): `package-q4.sh` fails if a copyright line was removed from
   `source/` since the import.

## Open

- **Shrinking the GPL code in the binary** (in order of cost): link with `--gc-sections` and a
  `-ffunction-sections` idlib (measured above: third-party code 340 -> 140 lines); a GPLv3 §7
  additional permission for the DUDE author's own code; ask the dhewm3 maintainers for
  permission covering the remaining hunks (idlib list from the audit, plus the 14 dude-q4
  commits) or rewrite them from the bug descriptions; for the SIMD id-gpl code, give the game
  only the generic SIMD processor or the engine's. Full independence (the game on the SDK's
  own idlib, "option 2") only if those leave too much.
- **Demo refusal**: needs the Quake 4 demo's file layout, not checked yet.
- **Not licence work, needed for "out of the box"**: a Windows `q4base.dll`, the install
  detection of `run-q4.sh` inside the engine, a Quake 4 entry in the menu.
- **What no step here resolves**: whether a GPL engine may load a non-GPL game library (the
  `GetGameAPI` boundary is the best available posture, not a guarantee), and whether "shall
  operate only with QUAKE 4" covers a replacement engine (read as yes: the library is a mod
  for Quake 4's data).
