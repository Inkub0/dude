# DUDE 0.9.7 — a closer look at Doom 3

A fidelity and bug-fix release on top of [0.9.6](https://github.com/Inkub0/dude/releases/tag/v0.9.6).
0.9.6 brought the Path Tracing tier; 0.9.7 goes back over Doom 3 itself. It fixes the three
open GitHub issues, sharpens the path-traced shadows of moving characters, makes the stock HUD
widescreen-correct without any pack, and corrects a long list of places where DUDE's GL3 /
Vulkan renderers or id's own code did not do what Doom 3 intended — ambient lights, window
glass, portal skies, soft particles, savegames and more.

Many of those fixes were found the same way: by running a second idTech 4 game through the
engine and chasing every difference back to its cause (see *Under the hood* below).

## Changed since 0.9.6

### Reported issues

- **Graphics corruption on Path Tracing with a mirror in view**
  ([#2](https://github.com/Inkub0/dude/issues/2)). Two causes, both fixed: props drawn in more
  than one batch in a frame (the mirror, sinks, dispensers, pipes) lost their motion vectors
  after the first batch, so the denoiser and FSR2 dragged old frames across them — the veil and
  the streaks behind pipes; and ReSTIR direct lighting divided by the mirror's black albedo,
  leaving a hole the denoiser spread onto the wall.
- **FPS dropping after 10–20 minutes on Communications Transfer**
  ([#4](https://github.com/Inkub0/dude/issues/4)). The point-light shadow cache never evicted
  anything, so once it was full every new light was re-rendered every frame until the next map
  load. It now evicts least-recently-used shadows, and the automatic budget leaves 20% of the
  card's VRAM free (at least 768 MB) and no longer thrashes when the lights in view don't fit.
  On Vulkan, destroying a render target no longer throws away every cached texture binding (the
  hitches and "descriptor pool exhausted" warnings in long sessions), the light/entity
  interaction table grows instead of being dropped when a map outgrows it, and particles no
  longer create and destroy a GPU buffer per stage per frame.
- **Controller not working on Linux** ([#3](https://github.com/Inkub0/dude/issues/3)). A fresh
  install — or a config carried over from dhewm3, whose bindings live in a file DUDE doesn't
  read — had no gamepad bindings at all. When no pad button is bound, DUDE now applies the
  default gamepad layout once (the same one *Reset Gamepad Bindings* uses). Existing bindings
  are never touched.

### Path Tracing tier: sharp shadows on moving characters

- **No more shadow smear behind NPCs and monsters.** ReSTIR's change detection only noticed a
  shadow *arriving* on a surface, not one *leaving* it, so the trailing edge of a moving
  character's shadow lingered. It now catches both, resets the denoiser's history where the
  light really changed, and gives low-confidence pixels a stronger fresh sample. Retuned
  defaults for all of it.
- **No more trails behind characters' details** (ears, noses, weapons) when a pose was reused
  between game tics: the reused frame reported zero animation velocity to the denoiser.
- **Doom 3's ambient lights under RT GI** (`r_rtgiAmbient`, new, default *Floor + Fold*). With
  RT GI on, the designers' ambient fill lights used to be switched off entirely and left to the
  GI to replace — dark where few real lights bounce (Mars outdoors, RoE's Erebus). They now add
  only what the ray-traced lighting falls short of, and also feed the GI bounce.
- **Faster:** the chosen light is shaded once instead of twice, last frame's side data is
  ping-ponged instead of copied, the light grid fills per 4×4×4 block (≈ 0.95 → 0.35 ms), and
  the per-frame light buffers live in VRAM.

### Doom 3, the way id made it

- **Ambient lights shade like vanilla again** on the GL3 and Vulkan renderers. They were read as
  colours instead of a lighting direction, so every ambient light came out magenta-tinted and
  flat — Communications Transfer's brown Mars ambience rendered dark red-purple. Now neutral and
  bump-mapped, as id's code draws them.
- **Window glass in a doorway plane draws from both sides.** A pane lying exactly in a portal
  plane was culled from the side it faces (22 such panes in Doom 3, 45 in RoE).
- **Window glass sorted as a decal keeps its reflection.** The matte-decal enhancement took some
  of id's window panes (`outdoor_glass1`, RoE's `gridglass*`) for dirt decals and stripped the
  stage that *is* the visible pane.
- **RoE / Lost Mission portal skies** are rendered as part of the player's view instead of as a
  second view in front of it. On the temporal and RT paths the sky camera's picture was blended
  into FSR2 / denoiser history (a double exposure) and could stay black under lit geometry; glass
  reflection probes in portal-sky maps (Phobos 1–3, Delta X, Hell) baked a frozen picture instead
  of the sky.
- **Low-res fullscreen effects keep HDR.** Frames the game renders into a smaller crop (RoE / LM
  with `g_lowresFullscreenFX 1`) lost tone mapping, exposure and bloom.
- **Soft particles only soften the line** where a particle meets a wall or the floor. They stay
  depth-tested, so large smoke and fire no longer show through things in front of them.
- **Savegames from anywhere.** DUDE now loads retail Doom 3, dhewm3 and every DUDE 0.9.x
  savegame. A save whose GUI state no longer matches the installed GUI files (a widescreen pack
  added or removed, retail data under a mod) used to crash or stop the load; it is now matched
  window by window, and retried against the other installed copies of the file.
- Scrolling light textures (`cloudscroll`) no longer go dark after half a minute at the RT tiers.
- Debug overlays (`r_showLights`, debug lines and text) line up under sub-native FSR2.
- The warning `Couldn't load image: }` after a map change is gone — an id slip in the PDA
  video/audio defaults, present in retail Doom 3 and dhewm3 alike.

### Widescreen without packs

- **The stock HUD, crosshair and multiplayer HUD of Doom 3, RoE and Lost Mission keep their shape
  on a wide screen** — no WideGuis pack needed. The engine anchors the stock files itself; checked
  window by window against the community pack's hand-placed anchors, it matches. A pack or mod
  that anchors its own HUD keeps its layout.
- **The death screen fills a wide screen**, its backdrop stretched and the menu kept at 4:3.

### Feel and options

- **Smoother animation at high frame rates.** With render interpolation (`com_interpolate`),
  monsters jittered — worst on stairs — because the joints the game sets once per tic (walking IK,
  look and head joints, ragdolls) stayed put while the body glided. Final poses are now
  interpolated, and a weapon no longer twitches for a frame entering its reload.
- **Classic 62.5 Hz game tics** (*Options → Game*, off by default): Doom 3 originally ran its game
  at 16 ms tics, not dhewm3's 60 Hz. Applies at the next map load; recorded demos keep it.
- **Extra lights** (`g_extraLights`, F10 *Graphics → New Light Effects*, off by default): a fire light on
  the Lost Soul, and the projectile lights id wrote but commented out (Doom 3, RoE, Lost Mission).
- **Adaptive Shadow Resolution** (*System → Advanced*, off by default): when the point lights in
  view don't fit the shadow budget, the farthest ones get smaller shadow maps instead of being
  re-rendered every frame. Softer distant shadows — opt-in.
- **Monitor picker works**: the window opens on the monitor chosen in the settings, not the one
  the mouse happens to be on (the mouse decides only when none is chosen or it isn't connected).
- **Settings menu (F10) reorganised**: the Graphics tab is regrouped (GPU and monitor pickers, a
  restart bar), Debugging is split into four sub-tabs with a search box, and every slider has a
  reset button plus *Restore defaults* per group.
- `ctrl+v` pastes in the console again.

### Sound and language

- **48 kHz (and 32/24/16/8 kHz) sounds play** instead of the default beep — common in custom maps.
  They are converted once on load; Doom 3's own sounds are untouched.
- A partial community translation shows missing strings in English instead of as `#str_` ids.

### Mods

- **Game libraries built for dhewm3 1.5.5 run unchanged again**: four places where DUDE's headers
  had drifted from dhewm3's binary layout are fixed, with a checker to keep it that way.
- `+set fs_game d3le` from the command line now puts RoE underneath, as the mods menu does.
- A texture shipped only as a same-path `.dds` loads, on every renderer.
- Sliders and lists in menus without widescreen anchors (Lost Mission, many mods) take the click
  where it lands — they used to jump to "high".

### Stability

Rounds of AddressSanitizer runs found and fixed, all in code Doom 3 runs: a use-after-free when
a glowing item's fill light was removed mid-frame, another when loading a game from the death
screen, an
over-read in id's string append, uninitialised contact materials in collision traces, a menu
choice reading past its values, and a 64-bit collision-model memory leak.

## Under the hood: a second game as a test bench

Doom 3's own data only exercises the paths id's maps happen to use. To find what the engine still
got wrong, DUDE started loading a second idTech 4 game — Raven's *Quake 4* — through the same
renderer, file system, GUI, sound and collision code. It uses the engine differently enough that
every mismatch pointed at something: the ambient-light, portal-glass, decal-glass, portal-sky,
crop-HDR, soft-particle, motion-vector, interaction-table, widescreen-HUD and most of the
stability fixes above came out of that work. Everything Quake 4 needs is gated to Quake 4, so
Doom 3 parses and plays exactly as before.

Quake 4 support itself is early work and **not part of this release's downloads**. If you own
Quake 4 and want to try it, you can build it yourself on Linux — single player only, expect rough
edges:

```
git clone --branch v0.9.7 https://github.com/Inkub0/dude.git
git clone --branch v0.9.7 https://github.com/Inkub0/dude-q4.git
cd dude
./build.sh --vulkan                                   # DUDE itself (dependencies: see README.md)
Q4GAME_DIR=../dude-q4 tools/quake4/build-q4game.sh    # the Quake 4 game library -> build/q4base.so
./run-q4.sh                                           # finds a Steam / GOG install, or Q4DIR=/path/to/Quake\ 4 ./run-q4.sh
```

Use the same tag for both repositories: each DUDE release has a matching
[dude-q4](https://github.com/Inkub0/dude-q4) tag.

You need your own retail Quake 4 (no game data is included). The game library is built from
Raven's Quake 4 SDK and lives in its own repository because it is under the SDK's licence
(non-commercial use only), not the GPL. Saves and settings go to `~/.local/share/dude/q4base`
and `~/.config/dude/q4base`, apart from Doom 3's.

## Upgrading from 0.9.6

- **Your graphics settings are reset once** on the first start, so the retuned defaults reach
  existing installs too. Key bindings, audio, resolution and display mode, monitor, renderer and
  GPU are kept; **pick your quality preset and ray-tracing level again** afterwards.
- **New savegames carry a DUDE revision**: an older DUDE build restarts the map with your
  persistent player data instead of loading them. 0.9.7 loads all your older saves.

## Known limitations

- **Emissive Mode is still experimental and ships off**, as in 0.9.6 (it got faster this release:
  fewer samples by default and a cheaper emitter target).
- Switching from OpenGL 3 to Vulkan inside the menu can still lose the 2D GUI — restart the game
  after changing the renderer.

## Setup (same as 0.9.6)

You need the original _DOOM 3_ / _Resurrection of Evil_ game data (`base/*.pk4`,
`d3xp/*.pk4`) from your own copy (Steam/GOG classic, **not** the BFG Edition).

- **Windows (64-bit)**: unzip and copy the **contents** of `dude-0.9.7-win64` into your
  Doom 3 directory, next to `base/` (the included pk4/config files merge into `base/` and
  `d3xp/`). Or run from anywhere with `+set fs_basepath "C:\path\to\Doom 3"`.
- **Linux (x86_64)**: untar and copy the contents into your Doom 3 directory the same way,
  run `./dude`.

Renderer selection: `+set r_graphicsAPI opengl|opengl3|vulkan` or the in-game menu. For the
ray-traced tiers pick the **Vulkan** renderer and turn ray tracing up.
Configs/saves live in `~/.local/share/dude` (Linux) / `Documents/My Games/dude` (Windows).

If the **Vulkan renderer won't start** (the OpenGL renderers are unaffected), update or
reinstall your GPU driver — the Vulkan driver (the ICD the loader dispatches to) ships with it,
and no bundled loader can substitute for a missing or Vulkan-incapable one. On Windows,
`vulkan-1.dll` comes from your GPU driver; DUDE deliberately does not ship it. On Linux a
current loader is bundled in `lib/` as a fallback for old distro loaders, but it still needs
your driver's ICD.

## Optional download: baked ambient-occlusion pack

`dude-0.9.7-baked-ao.zip`: precomputed per-material AO maps for subtle contact shading on world
surfaces, props and weapons. All packs were re-baked this release with the new GPU baker (its
occlusion rays are traced with Vulkan ray queries) and are now stored as 8-bit grayscale maps — a
quarter of the old file size, with no loss from the new format. Extract into your Doom 3 directory the same way. Entirely optional, and unused at the Path Tracing
tier (ray-traced GI/AO replace it there).

DUDE is GPLv3 — source at https://github.com/Inkub0/dude.
