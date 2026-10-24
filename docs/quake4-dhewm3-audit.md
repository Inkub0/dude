# dhewm3's game-code fixes vs Raven's Quake 4 game — audit (2026-10-24, play-tested)

Raven built Quake 4's game on id's 2004–2005 Doom 3 game code, so it lacks every fix dhewm3 made
to that code since id's GPL release (`fb1609f5`). This audit went through the **112 upstream
dhewm3 commits** that touch `neo/game/` (DUDE's own commits excluded) and checked each against
Raven's copy in the Quake 4 game repository (`~/dude-quake4-game/source/game/`, single-player
`game/` only; `mpgame/` is not built). Each applied fix is its own game-repo commit, credited to
the dhewm3 commit, so mods built on the SDK can follow them (see
[quake4-port.md](quake4-port.md) 5.6 and the publication note there).

## Applied (game repository)

| dhewm3 | Raven location | Fix |
|---|---|---|
| `6d03fac3` | `Target.cpp` idTarget_WaitForButton::Think | `!oldButtons & ATTACK` precedence: the press only registered with no button held before |
| `94cd0ee5` | `Game_local.cpp` InitFromSaveGame, GetShakeSounds | `GetString()` returns `""`, never NULL: always-true checks |
| `b03fc927` | `Weapon.cpp` rvWeapon::LaunchProjectiles | `ownerBounds - projBounds` with a projectile wider than its owner (negative box, garbage start point) |
| `4bd46b3e` | `physics/Physics_Player.cpp` Restore | a save taken while a pusher rotated the player kept that clip model axis (clipping bugs after loading) |
| `4dcc7efe` | `Entity.cpp` Event_StartSoundShader | `startSoundShader( "" )` stops the channel instead of the default beep |
| `697554e5` | `Misc.cpp` idSpring | debug line drawn every frame in play (convoy2b, dispersal, network1); Restore linked ends before they were restored (load error); missing / removed ends |
| `74157006` | `ai/AI_pathing.cpp` | a path root without children steered to the world origin (Raven already had the null guard) |
| `0c84e4bf` | `anim/Anim_Import.cpp` | DLL handle truncated to int (64-bit Windows only) |
| `abd8d4ac`, `f4f14cc0` | `Game_local`, `Physics.cpp`, `Camera.cpp`, `Script_Thread.cpp`, `Player.cpp` | game time ran at 960 ms per real second (constant 16 ms frames); now 16/17 ms so 60 frames = 1000 ms, `msecPrecise` where one constant frame is needed. **Fidelity:** retail had the slow second; this follows dhewm3 and DUDE's Doom 3 |
| `befe732d` | `Pvs.cpp` | `new[]` freed with `delete` (applied earlier, game `14d44e7`) |
| 64-bit set | event / script / PVS system | `d92d4ba8` `506ea7b3` `ce976c47` `db4aca78` `4244436d` `fb3f0cc2` `76a9fdce` `3ad384a0` (Q5.3, game `bcff329`) |

## Already fixed by Raven, or not applicable

- **Raven already fixed:** `fad99572`/`57dc097f` (event strings: Raven saves event data raw),
  `c4c72363` and `2c58b2fb` (Raven computes prototype parm sizes), `a7660020` (CompileFile keeps a
  copy of the name), `a7e4eb86`/`ee4eced6` (Raven re-acquires every render light handle on
  restore), `f407b2b8` (Raven's script limits are larger: MAX_GLOBALS 589824), `0ecf60c9` (the
  assert only logs in DUDE's Quake 4 game).
- **Not applicable:** `d708fbd9` (Quake 4's HUD has no message window), `c22965bf` (Doom 3 map
  specific), `b9247b67` (TypeInfo / ID_DEBUG_MEMORY code not built), `3c96e196` (a gcc 4.5
  miscompile workaround, no source bug), `a8fc1cee` and `be5f1dac` (dead code / a typo),
  `2b9e2e51`/`3d692b58`, `6b1e27b1`/`783db18c` (see Q5.3).
- **About 40 have nothing to port:** build system, renames, version strings, platforms
  (BSD, AROS, VS, demo data), the script debugger, and dhewm3 features (`g_hitEffect`, anchored
  and 4:3-scaled GUIs, savegame build info, interactive in-game GUI).
- **About 25 are compiler-warning cleanups.** Instead of porting them, the Quake 4 game was
  compiled with `-Wall` and every remaining warning read: nothing needs a silent fix (cosmetic,
  multiplayer-only - Q9 - or unreachable).

## Found on the way, kept as retail

Real logic slips that retail Quake 4 shipped with. Fixing them would change gameplay from vanilla,
so they stay as they are (decided 2026-10-24, not even as opt-in):

- `ai/AI_Move.cpp:4298` — `!reach->fromAreaNum != myMove->myArea` (and three more) compare 0/1
  with an area number: the "edge touches the start or goal area" test is always true, so AI path
  smoothing skips extra path points more often than intended.
- `vehicle/VehicleRigid.cpp:98` — `!autoCorrectionBegin && ( ... ) || ( lengthSq < 0.01f )`
  restarts the auto-correction timer every frame while a flipped vehicle stands still, so it never
  rights itself.
- `AF.cpp:899` — `DECLAF_CONSTRAINT_*` compared with `CONSTRAINT_*` (slider 5 vs 6, spring 6 vs
  10). id's own bug, in Doom 3 and RoE too (dhewm3 left a FIXME); only reached when an AF is
  reloaded onto an existing ragdoll (the AF editor), never in play.
