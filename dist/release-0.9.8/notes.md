# DUDE 0.9.8 — Welcome to Stroggos

A Path Tracing and emissive-lighting release on top of [0.9.7](https://github.com/Inkub0/dude/releases/tag/v0.9.7). The Path Tracing tier runs about 14 % faster than 0.9.7 (45 % faster in Emissive Mode), and the experimental Emissive Mode shifted to a custom file with an in-game Light Editor to author it. The same executable now runs the retail **Quake 4** campaign, with its game library as a separate download (see *Quake 4* below).

## Changed since 0.9.7

### Path Tracing faster: +14 % fps, +45 % in Emissive Mode

Measured against 0.9.7 (both release builds, demo1 timedemo, 1440p, Path Tracing, RTX 3080 Ti limited to 166 W):

| | 0.9.7 | 0.9.8 | |
|---|---|---|---|
| fps | 47.8 | 54.3 | +14 % |
| 1 % low | 18.9 fps | 25.2 fps | +33 % |
| GPU time per frame | 19.7 ms | 17.4 ms | −12 % (−19 % clock for clock) |
| **Emissive Mode:** fps | 27.9 | 40.6 | +45 % |
| 1 % low | 8.7 fps | 16.3 fps | +87 % |
| GPU time per frame | 33.8 ms | 23.2 ms | −31 % (−36 % clock for clock) |

Most of it is the pass that gives newly revealed pixels extra light samples (about 85 % cheaper), then the direct-light pass (a third cheaper in Emissive Mode). The figures include the retuned denoiser and, in Emissive Mode, the emitters lighting the haze, which both cost time (below). A GPU that is not power-limited runs the new code at a higher clock than 0.9.7's and gains nearer the clock-for-clock figures.

- **The 0.9.7 boost settings are reverted, the largest single gain; existing configs are updated automatically (config schema 4).** 0.9.7's fix for shadow smear behind moving characters also gave newly revealed pixels much stronger fresh sampling. That part cost about 2 ms per frame at 1440p (13.97 → 12.02 ms GPU in a Path Tracing Nightmare timedemo on an RTX 3080 Ti) and turned out not to be needed: detecting a shadow *leaving* a surface is what removed the smear, and that stays. Only that one setting is migrated; nothing else in your config is reset.
- **Rooms near a small fog volume keep their bounce light.** The fade that stops ray-traced GI washing out fog took its fog distance from any fog volume in view — the mist inside a fridge was enough to black out a whole room's bounce light (and its ceiling) as soon as the fridge came into view. It now only uses fog the camera is standing in.
- **Ray-traced GI cheaper while you move.** Pixels that just came into view get extra direct-light samples; that pass also re-traced their GI bounce from scratch, rebuilding exactly what the main pass had just computed. It now reuses it (the same image): about 2.4 ms less per frame while moving in a Path Tracing Nightmare timedemo at 1440p.
- **Steadier lighting around lights that move.** A light the game moves or re-parms every frame (on a lift or door, a projectile, a scripted fade) counted as a brand-new light each frame, which reset the denoiser's history around it every frame. It now keeps its identity for as long as it exists.
- **Path Tracing direct light a little cheaper, same image.** The direct-light pass does the same math with fewer GPU instructions: emitter triangles are weighed without normalising vectors, lights behind a surface are rejected before any square root, and a light's projection texture carries its size instead of being queried for every candidate. About 0.4 ms less GPU time per frame (demo1 timedemo, Emissive Mode, 1440p, RTX 3080 Ti: 17.92 → 17.52 ms); the image is unchanged.
- **Newly revealed pixels cheaper, same image.** Pixels that just came into view get extra light samples; the pass that added them ran the whole light selection a second time. The main pass now draws those extra samples in its own light selection, so the extra pass only shades them, and the light lists are read ahead. About 0.6 ms less GPU time per frame, likely closer to 1 ms clock for clock (demo1 timedemo, Emissive Mode, 1440p, RTX 3080 Ti: 17.54 → 16.94 ms).
- **Smoother frame pacing at Path Tracing, same image.** The direct-light pass needed 128 registers per GPU thread, mostly for code that only runs with debug options; it now needs 96 (RTX 3080 Ti), so a third more of its threads run at once. The pass is about 13 % faster and the 1% lows rise about 5 % (demo1 timedemo, Emissive Mode, 1440p, RTX 3080 Ti: 7.18 → 6.28 ms for the pass, 1% low 28.3 → 29.7 fps). On a power-limited GPU part of the gain goes into a lower clock; a GPU with power headroom gains more.
- **Emissive Mode a little cheaper, same image.** Each glowing surface stored the direction it emits in a packed form that every pixel unpacked again for every emitter it considered, in the direct light and in the GI bounce; it is now stored ready to use. About 0.5 ms less GPU time per frame, the direct-light pass about 5 % faster (demo1 timedemo, Emissive Mode, 1440p, RTX 3080 Ti, compared clock for clock).
- **Direct-light denoiser retuned.** New defaults for the Path Tracing direct-light denoiser (*Debugging → DI Denoiser (ReLAX)*): a wider spatial filter (4 A-trous passes), a slightly longer history (450 ms), no anti-firefly clipping, 3 samples for newly revealed pixels and a finer light-change check. Configs that already hold these settings keep their own values. It costs about 0.4 ms of GPU time per frame (the denoiser about 20 % slower), included in the figures above.

### Emissive Mode: authored, not guessed

Emissive Mode (*Path Tracing tier, Vulkan, off by default*) lets the map's self-lit surfaces — light panels, strip lights, door lights — light the scene as real area lights, in place of the placed lights they stand for. Until now it guessed, from geometry alone, which placed light each glowing surface replaced and how bright it should be; too often it guessed wrong (dead ceiling lights, lights counted twice, a fixture inheriting the wrong light).

- **No more guessing.** Emissive Mode now reads a per-map setup, `emissive/<map>.cfg` in your save path: which placed lights are deleted, which surfaces emit, and how. **A map without a setup is unchanged**: nothing emits and no light is removed.
- **The Light Editor**, in game. Bind `editLights` to a key (or *F10 → Debugging → Ray Tracing → RT Emitters → Enter look mode*) and keep playing:
  - every placed light is drawn as a sphere (white active, red deleted, yellow overridden) and every glowing surface is tinted (green emits, grey can emit, orange switched off);
  - what you point at turns cyan, with its volume drawn — a light's radius box or projection frustum, a surface's reach — and a label under the crosshair names it;
  - **right-click** it for a menu with everything that applies to it;
  - **Del** deletes the light you point at (press again to bring it back) or stops a surface emitting, **Ins** makes a surface emit, **Home** solos it, and the **mouse wheel** changes a surface's brightness (Ctrl: every surface of its material) or a light's.
- **What you can set:**
  - **Surfaces:** emit or not, per surface or for every surface of a material in the map; brightness; reach; **physical radiance** (the glow's own brightness times a multiplier — new emitters start physical, at the brightness they already had) or matched to the reach; **tint**; **emission profile** (bare tube, diffuser panel, globe); one- or two-sided; **follow a light**, so a surface flickers and pulses with the light it replaces; **match a light's power**, so it lights the room the way that light did.
  - **Lights:** delete in Emissive Mode only (normal mode keeps every light), and **colour, brightness and radius** overrides that apply in Emissive Mode.
  - the lights the engine adds for glowing screens and items (*Screens emit light*, item glow) are drawn light blue, so you can tell them from the map's own;
  - **Solo** (Home, or the menu): only the light or surface you point at lights the scene — to see exactly what one light does.
  - Every change shows immediately. *Save* writes the map's file; unsaved edits are kept in a recovery copy if you change map or quit, and the editor offers them back the next time you open it on that map.
- **Floors under ceiling lights no longer shadow themselves.** A surface facing an emitter tested its shadow ray right up to the emitter's own surface and counted itself in shadow: tiled and grated floors under a ceiling panel stayed dark on their flat faces and lit only in the grooves, whatever the brightness.
- **No more fireflies under neon tubes and bulbs.** An emitter's shadow ray aimed at its centre, which for a tube lies inside the tube itself: almost every ray was blocked by the light's own surface, and the few that slipped through sparkled. Shadow rays now aim at points on the light's surface, which also gives emitters soft shadows.
- **Emitters light the haze.** With volumetric lighting on, the glowing surfaces now light the air around them as the lights they replace did (`r_volEmissiveScale`, 1 by default; the 32 strongest near you). It costs about 2.3 ms of GPU time per frame in the measurement above; `r_volEmissiveScale 0` turns it off.
- **Neon tubes and bulbs bounce light again**, and **glows that pulse or flicker by themselves now pulse their light too** (before, they lit the room at one constant level).
- **Emitters light as true area lights**: no round hot spot where the old light sat, and walls behind pillars or alcove frames no longer go dark because the old light's origin was hidden.
- **Glowing doors and props split per fixture**: a model with several lit parts (a door with a light on each side) is one surface per part, so each can be set on its own.
- **Cleaner emitter light, for less GPU time.** Each pixel now draws an emitter's light from the parts of the surface that actually reach it (the near end of a strip light, the side of a tube that faces it), and never picks a light that faces away from it. One sample per pixel now looks like 16 did and costs what 2 did, so *Emitter Samples / pixel* (`r_rtEmissiveSamples`) defaults to **1**; more samples only cost time now (about 0.1 ms each at 1440p).
- **Emitter GI about 2 ms cheaper, and it no longer leaks through walls.** A GI bounce used to sample every glowing surface near the point it hit and shadow-test only the brightest one, so the others lit through walls; it now picks one surface by how much it can contribute and shadow-tests that one. Same average brightness with the shadow test off, noise as before — GI from fixtures behind walls reads a little darker, which is correct (demo1 timedemo, Emissive Mode: 46.8 → 52.9 fps).
- **No stutter when a glowing prop appears.** A lit prop that shows up mid-level (a console, a spawned machine part, a pickup) rebuilt the whole emitter table: 14–47 ms each time, longer as the level filled. It is now added on its own, in about 1 ms.

### Fixes and smaller changes

- **A killed lost soul no longer leaves the room lit.** With *Extra Dynamic Lights* on, a lost soul's fire light kept burning for a few seconds after the skull burst; it now goes out the moment it dies.
- **The first shot after a level loads registers.** A fire tapped the instant a level finished loading was dropped while the weapon was still raising; it now fires once the weapon finishes coming up.
- **Eye adaptation is opt-in.** HDR auto-exposure is no longer switched on by any quality preset — it stays off unless you turn it on. Its toggle sits with the other HDR/tonemap controls under **Post-Process** (in the in-game *System* options the **HDR** toggle moved there from *Advanced*).

### Removed

- **`emitters.cfg` and the Emitter Editor.** Their settings (radiance, tint, emission profile, two-sided) are part of each map's setup now, edited live in the Light Editor; `editEmitter` opens the Light Editor's look mode. An existing `emitters.cfg` is no longer read — its radiance value is the Light Editor's *Radiance x*.
- **The guessing:** the light census, the pairing of surfaces with lights, power sharing and the "indicator" rule, with their settings (`r_rtEmissivePowerMatch`, `r_rtEmissiveFixtureMinArea`, `r_rtEmissiveFixtureFullArea`, `r_rtEmissiveShareExponent`), the `rtEmissiveLights` command and debug views 4 and 5 of `r_rtEmissiveTableDebug` (look mode shows the lights now).
- **Flare inference** (`r_rtEmissiveFlareInfer`): a fixture whose material has no glow texture is no longer turned into an emitter through the flare sprite on it.
- With Emissive Mode off, the GI-bounce-only emitters (`r_rtEmissiveBounce`, off by default) follow the map's setup too.

### For developers

- `tools/bench/timedemo-ab.sh`: an A/B timedemo runner — one binary per variant, two warm-up runs and a cool-down between runs, GPU time normalised to the median clock, and a check that every run had its variant's settings. Emissive-mode runs get your authored `emissive/<map>.cfg` setups copied into each run's sandbox (`EMISSIVE_DIR`), and the report flags a run whose map came up unauthored.
- **The timedemo loads its map like a level load** (`com_timeDemoLevelLoad`, on by default). A demo never does a level load, so it used to load every texture on first sight inside the timed run: demo1 showed 400–800 ms stalls (flashlight, imp fireballs) at every RTX tier that play does not have. The timedemo now precaches the map, its entities and the player (with his weapons) the way the game loads a level, then times one pass; what a level load would not load still streams in, as in play (demo1 at Path Tracing: hitch frames 56–75 → 23–24, worst ~800 → ~150 ms). `timeDemoQuit demo1 twice` now works like `timeDemo` for a fully warmed run, and the report's playback line says which load ran. The bench runner uses the level load and one pass by default (`TWICE=1` adds the un-timed pass).
- **The timedemo updates animated characters the way play does.** Demo playback re-creates a character's render entity every few frames, and its ray-tracing data was then rebuilt from scratch each time instead of updated in place (0.3–1.5 rebuilds per frame in demo1, about none in play). It is now updated: about 0.2 ms less GPU time per frame in the demo1 timedemo at Path Tracing, and the benchmark is closer to real play.
- `r_vkGpuTime 2` also times the work before the scene: `skin`, `tess.deform`, `rt.animblas` (with `.blas` and `.tlas`) and `rt.frameas`. `vkMemReport` lists the Vulkan memory heaps and any GPU-read buffer that landed in system RAM; a warning names such a buffer when it is created (likely with Resizable BAR off, once the 256 MB CPU-visible VRAM window is full).
- The bench report (`tools/bench/timedemo_ab_report.py`) counts only loaded clock samples, shows each run's thermal / power-cap throttling, adds a clock-independent per-pass comparison (paired demo windows against the passes that did not change), and flags a variant whose own runs disagree.
- `tools/shaderstats`: offline shader checks on an AMD GPU with Mesa's RADV driver, without starting the game: compile every shader, register and instruction counts per program and per function, and an equivalence test of the emitter weights between two shader versions (`tools/shaderstats/README.md`).
- The AAS "out of date" warning now prints both CRCs (the map geometry the nav file was built against, and the map as loaded), so a broken nav mesh — a stray region file or an edited map — is easy to pin down.
- `r_rtEmissiveTable` is quieter: the per-material census and the excluded-material list print only with `r_rtEmissiveTableDebug`, and nothing prints at all unless emissive is actually in use.
- `docs/rtx-light-editor.md` (the Light Editor and the per-map file format) and `docs/rtx-sigma-shadows.md` (research into NRD SIGMA shadows: parked).

## Upgrading from 0.9.7

- **One setting is updated on the first start** (the Path Tracing boost history); nothing else is reset — keep your preset and ray-tracing level.
- **Savegames are unaffected**: 0.9.8 loads every save 0.9.7 did.
- `emitters.cfg` is ignored (see *Removed*).
- A saved `r_rtEmissiveSamples` is kept: set it to 1 (the new default) unless you want to pay for more.

## Known limitations

- **Emissive Mode is still experimental and ships off.** It only changes a map you (or a mod) have authored a setup for.
- While moving in the Light Editor's look mode, its markers smear slightly under the temporal upscaler.
- A light whose material ignores the light colour cannot be recoloured in the Light Editor; its brightness still can.

## Setup for Doom 3 (same as 0.9.7)

You need the original _DOOM 3_ / _Resurrection of Evil_ game data (`base/*.pk4`, `d3xp/*.pk4`) from your own copy (Steam/GOG classic, **not** the BFG Edition).
- **Windows (64-bit)**: unzip and copy the **contents** of `dude-0.9.8-win64` into your Doom 3 directory, next to `base/` (the included pk4/config files merge into `base/` and `d3xp/`). Or run from anywhere with `+set fs_basepath "C:\path\to\Doom 3"`.
- **Linux (x86_64)**: untar and copy the contents into your Doom 3 directory the same way, run `./dude`.

## Setup for Quake 4 — new in this release

DUDE now runs the retail **Quake 4** single-player campaign out of the box, from your own install: the same `dude` executable, on the OpenGL 3.3 and Vulkan renderers and the ray-traced tiers, with DUDE's quality presets. Quake 4's effects (BSE) are drawn by DUDE's own reimplementation. Multiplayer is not supported yet.

- **Download** the Quake 4 game library as well: `dude-q4-0.9.8-linux-x86_64.tar.gz` or `dude-q4-0.9.8-win64.zip`. It is a separate archive from a separate project, [dude-q4](https://github.com/Inkub0/dude-q4) (tag `v0.9.8`), because it is built from Raven Software's Quake 4 SDK and is under the SDK's licence, not the GPL: non-commercial use only, free copies only, with the licence file (included in the archive) attached to every copy. No Quake 4 game data is included.
- **Setup:** extract DUDE 0.9.8 and the `dude-q4` archive into your Quake 4 folder (the one that contains `q4base/`) and start `dude` there; DUDE finds Quake 4's paks and runs Quake 4. From anywhere else, start it with `+set fs_basepath "/path/to/Quake 4"`. A folder that holds both Doom 3's and Quake 4's game data stops with a message: start with `+set com_title doom3` or `+set com_title quake4` to choose.
- **Full retail Quake 4 only** (Steam, GOG or disc). The Quake 4 demo, or an install missing its map paks, is refused: the SDK licence lets the game library run with the full version of Quake 4 only.
- **Forgot the game library?** DUDE stops with a message naming the archive for your version and platform, its licence terms and where to extract it, with a button that opens the download page.

Quake and Quake 4 are trademarks of their owners (id Software / ZeniMax Media). DUDE is an independent, non-commercial project, not affiliated with, endorsed or sponsored by id Software, ZeniMax Media, Microsoft, Raven Software or Activision.

Renderer selection: `+set r_graphicsAPI opengl|opengl3|vulkan` or the in-game menu. For the ray-traced tiers pick the **Vulkan** renderer and turn ray tracing up. Configs/saves live in `~/.local/share/dude` (Linux) / `Documents/My Games/dude` (Windows).

If the **Vulkan renderer won't start** (the OpenGL renderers are unaffected), update or reinstall your GPU driver — the Vulkan driver (the ICD the loader dispatches to) ships with it, and no bundled loader can substitute for a missing or Vulkan-incapable one. On Windows, `vulkan-1.dll` comes from your GPU driver; DUDE deliberately does not ship it. On Linux a current loader is bundled in `lib/` as a fallback for old distro loaders, but it still needs your driver's ICD.

## Optional downloads: baked ambient-occlusion packs

Baked ambient occlusion is now a **separate, self-contained download with its own version** — it only changes when the baker does, so one pack serves many DUDE releases and you don't re-download it every version. It is a single drop-in `.pk4` per title. Entirely optional, and unused at the Path Tracing tier (ray-traced GI/AO replace it there).

- **Doom 3 + Resurrection of Evil + The Lost Mission:** one pack, `z_baked_ao.pk4` → drop it into `base/` (it covers all three). From the **Baked AO** download on the [releases page](https://github.com/Inkub0/dude/releases).
- **Quake 4:** `z_baked_ao.pk4` → drop it into `q4base/`. From the [dude-q4 releases page](https://github.com/Inkub0/dude-q4/releases).

No re-extraction next to `base/` — just drop the pack into the matching game folder.

DUDE is GPLv3 — source at https://github.com/Inkub0/dude. The Quake 4 game library is under the Quake 4 SDK licence; the DUDE author's code linked into it carries a GPLv3 section 7 permission (`COPYING-QUAKE4-EXCEPTION.txt`).
