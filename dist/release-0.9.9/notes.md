# DUDE 0.9.9: Steadier on Stroggos

A stability and Quake 4 release on top of [0.9.8](https://github.com/Inkub0/dude/releases/tag/v0.9.8). It fixes the crashes reported since 0.9.8 (GitHub #8, #10, #11, #14 and the Quake 4 MCC landing), finishes the Quake 4 port's rough edges (terrain blends, smooth motion above 60 fps, running by default, its own window icon), adds a playable third-person camera, and makes texture loading much faster: a quality preset change now takes well under a second, and the Vulkan renderer uses compressed textures on the lower presets.

## Crash fixes

- **Random crashes since 0.9.7 (GitHub #10).** With frame interpolation (high fps), a smoke puff's vertices could be freed twice when its light surfaces outlived the puff, corrupting memory that a later level change tripped over (often at a level start, for example when hell breaks loose in Mars City Underground). Fixed at the source.
- **"Assertion failed" dialogs and the crashes behind them (GitHub #11, #14).** The release builds stopped on internal consistency checks that the original games compiled out, for example one of Raven's own self-correcting checks in the Quake 4 hangar. A failed check is now written to the log once and the game carries on, as the original games did.
- **Quake 4: crash loading `mcc_1`, and when the MCC ship lands.** The map's navigation file for one creature size is an empty stub that was read as a real one; it is now treated as absent. The landing crash was one of the checks above, now harmless.
- **Fullscreen on Wayland (GitHub #8, Hyprland).** Opening the Graphics settings in fullscreen aborted when the window manager had changed the fullscreen state on its own. DUDE now follows the window's real state. The problem occurred on Wayland, and it's fixed there. Hyprland should behave similarly.
- **OpenGL 3: crash when changing the quality preset** with a level loaded. The settings menu's own OpenGL loader went stale after the renderer restarted.
- **Linux: DUDE starts from Steam.** The Linux build no longer links libcurl, whose Steam runtime copy refused to load it (`./dude # %command%` in the Launch Options now works). libcurl only served multiplayer pak downloads.
- **Quake 4 paks copied into Doom 3's `base/`** now stop with a message saying so, instead of "bad type: 15". Following install instructions should avoid this problem in the first place.
- **Frozen picture with ray tracing (GitHub #12, Intel Arc).** Every ray is checked before it is traced, so an invalid ray can no longer lose the GPU. If the GPU is lost anyway, DUDE restarts the renderer by itself (up to 3 times a session, `r_vkDeviceLostRestart`) instead of freezing; from the second loss in a session it also steps ray tracing down one level for the rest of that session, without touching your saved settings. This is not a proper fix, but I don't have an Arc card to work with it.

## Quake 4

- **Terrain blends show as in Raven's game (GitHub #18).** Ground that crossfades between soil, sand and rock (the canyons from the second level on) rendered as a single coarse rock layer, and burn marks painted into the map were missing: Quake 4's map files leave out a vertex's colour when it is the default, and DUDE filled the gap with white instead of black.
- **Smooth above 60 fps (GitHub #13).** The Quake 4 game library now draws characters, the view, weapons, effects and particles between game tics, as Doom 3 already did; with frame interpolation on, 144 fps now looks like 144 fps. Doom 3's effects, particles and animated materials are drawn between tics too.
- **You run by default, Shift walks**, as in the original Quake 4 (it walked at half speed in 0.9.8).
- **The default brightness is Quake 4's own** (1.2). Existing Quake 4 configs are updated once on the first start.
- **The window and taskbar show Quake 4's icon (GitHub #19)**, read from the `Quake4.exe` in your install (DUDE ships none of Quake 4's art). Without it, DUDE's own icon is used.
- **Bullet-hole highlights follow the light.** Quake 4's bullet holes carry a painted-on shine at full strength, so they glowed on unlit walls; the shine is now scaled by the light on the wall (`r_decalLitHighlights`, on by default, off at Potato, the faithful floor).
- **Path Tracing lighting no longer trails tessellated characters.** Quake 4's marines and talking heads (and Doom 3's characters with tessellation on) had no animation motion, so their lighting lagged their movement by up to a second.

## Playable third person

`pm_thirdPerson 1`, or *F10 → Game Options → Camera*, in Doom 3 and Quake 4: the HUD stays on, the crosshair sits where your weapon actually points, the camera frames over your right shoulder (`pm_thirdPersonSide`, distance and height in the same menu), and your body casts a shadow.

## Faster texture loading, less video memory

- **Quality preset changes in about half a second.** A preset that changes texture compression reloads the textures instead of restarting the renderer, decoded on every CPU core while the finished ones upload: OpenGL 3 went from about 16 s to under 1.5 s, Vulkan from 2.0 s to 0.4 to 0.5 s.
- **Vulkan uses compressed textures at Potato, Low and Medium**: it reads the games' own `.dds` textures and compresses the rest on load (VRAM use at Medium about 14 % instead of 23 % on a 12 GB card). High and above stay uncompressed, as before. OpenGL 3 compresses at the same three presets, and every preset now loads full-size textures (an old Quake 4 low-detail setting could keep them shrunk).
- **Faster level loads.** Textures load on all cores without waiting between batches (3.5 → 1.6 s on a Doom 3 level), and the height fields that parallax and tessellation integrate out of the normal maps are cached on disk after the first load (`generated/heightfields/` in your DUDE folder, `r_heightFieldCache`), which cuts a second load of the same level by about 30 % (3.5 → 2.4 s).
- **Tessellated characters bake on the GPU.** Quake 4 levels with many tessellated characters spent up to half a minute preparing them at load; that is now about a second.
- **No per-frame index buffers for animated characters on Vulkan.** Up to about 3,300 buffers a second were created and freed in a busy Quake 4 fight (and flooded the log with system-RAM warnings when Resizable BAR is off); now about 12.

## Visuals

- **RT contact shadows**: small, sharp contact shadows traced through the ray-traced scene (feet, props on tables, weapon detail), on in the ray-traced presets from High up (*Graphics → Shadows*).
- **Screen-space contact shadows without the dots**: a smoother march, retuned defaults, and the quality presets now set them (off at Potato and Low, on from Medium). Configs still on the old defaults are moved once.
- **RT shadow blur retuned**: new defaults for the ray-traced shadow blur and RT contact shadows; configs on the old defaults are moved once, deliberate values stay.
- **OpenGL 3: no more black squares** for a frame or two when firing, reloading or hitting enemies (soft particles sampled a stale texture).
- **No black face on some models** (the medkit, among others): degenerate texture coordinates produced invalid tangents with the SSE4.1 math.
- **The HUD's "Downloading PDA" bar** no longer draws off-centre after loading a save made while it was showing.
- **Vulkan: materials with their own shader programs** (mods, some effects) compile again in the release builds; they failed for want of a shader header that only the build machine had.

## For developers

- `r_heightFieldCache` (above); the load and `reloadImages` prints split the worker time by phase and say why a height field missed the cache.
- `com_speeds` prints precise milliseconds and a GPU column.
- Vulkan validation messages name the render pass they came from; new colour targets and storage images start zeroed.
- `tools/bench`: a 0.9.8-versus-current timedemo set and a CPU frontend/backend split set.
- The ASAN build gives every block-pool allocation its own heap block, so pool double frees are caught where they happen.
- No build-machine paths in the release binaries.
- A structural pass split the largest renderer and Vulkan functions by stage, with identical output.

## Upgrading from 0.9.8

- **Savegames are unaffected**: 0.9.9 loads 0.9.8's saves.
- **Quake 4: update both archives together.** The 0.9.9 Quake 4 game library does not work with DUDE 0.9.8. The 0.9.8 library still runs on DUDE 0.9.9, but without the smooth motion above 60 fps and the third-person camera.
- A few settings move once on the first start (Quake 4 running and brightness, contact shadows, RT shadow blur, decal highlights at Potato), only where your config still holds an old default or a preset's value. Nothing else is reset.

## Known limitations

- **Emissive Mode is still experimental and ships off.**
- **Quake 4 multiplayer** is not supported yet.

## Setup for Doom 3 (same as 0.9.8)

You need the original _DOOM 3_ / _Resurrection of Evil_ game data (`base/*.pk4`, `d3xp/*.pk4`) from your own copy (Steam/GOG classic, **not** the BFG Edition). Step by step: the [install guide](https://inkub0.github.io/dude/install.html).

- **Windows (64-bit)**: unzip and copy the **contents** of `dude-0.9.9-win64` into your Doom 3 directory, next to `base/` (the included pk4/config files merge into `base/` and `d3xp/`). Or run from anywhere with `+set fs_basepath "C:\path\to\Doom 3"`.
- **Linux (x86_64)**: untar and copy the contents into your Doom 3 directory the same way, run `./dude`. From Steam, set the game's Launch Options to `./dude # %command%`.

## Setup for Quake 4 (same as 0.9.8)

DUDE runs the retail **Quake 4** single-player campaign from your own install: the same `dude` executable, on the OpenGL 3.3 and Vulkan renderers and the ray-traced tiers, with DUDE's quality presets. Multiplayer is not supported yet.

- **Download** the Quake 4 game library as well: `dude-q4-0.9.9-linux-x86_64.tar.gz` or `dude-q4-0.9.9-win64.zip`. It is a separate archive from a separate project, [dude-q4](https://github.com/Inkub0/dude-q4) (tag `v0.9.9`), because it is built from Raven Software's Quake 4 SDK and is under the SDK's licence, not the GPL: non-commercial use only, free copies only, with the licence file (included in the archive) attached to every copy. No Quake 4 game data is included.
- **Setup:** extract DUDE 0.9.9 and the `dude-q4` archive into your Quake 4 folder (the one that contains `q4base/`) and start `dude` there. From anywhere else, start it with `+set fs_basepath "/path/to/Quake 4"`. A folder that holds both Doom 3's and Quake 4's game data stops with a message: start with `+set com_title doom3` or `+set com_title quake4` to choose.
- **Full retail Quake 4 only** (Steam, GOG or disc). The Quake 4 demo, or an install missing its map paks, is refused: the SDK licence lets the game library run with the full version of Quake 4 only.

Quake and Quake 4 are trademarks of their owners (id Software / ZeniMax Media). DUDE is an independent, non-commercial project, not affiliated with, endorsed or sponsored by id Software, ZeniMax Media, Microsoft, Raven Software or Activision.

Renderer selection: `+set r_graphicsAPI opengl|opengl3|vulkan` or the in-game menu. For the ray-traced tiers pick the **Vulkan** renderer and turn ray tracing up. Configs/saves live in `~/.local/share/dude` (Linux) / `Documents/My Games/dude` (Windows).

If the **Vulkan renderer won't start** (the OpenGL renderers are unaffected), update or reinstall your GPU driver: the Vulkan driver ships with it, and no bundled loader can substitute for a missing or Vulkan-incapable one. On Windows, `vulkan-1.dll` comes from your GPU driver; DUDE deliberately does not ship it. On Linux a current loader is bundled in `lib/` as a fallback for old distro loaders, but it still needs your driver.

## Optional downloads: baked ambient-occlusion packs (unchanged)

Baked ambient occlusion is a separate download with its own version; it only changes when the baker does, so the current packs serve 0.9.9 too. Entirely optional, and unused at the Path Tracing tier.

- **Doom 3 + Resurrection of Evil + The Lost Mission:** one pack, `z_baked_ao.pk4`, dropped into `base/` (it covers all three), from [Baked AO v1](https://github.com/Inkub0/dude/releases/tag/baked-ao-v1).
- **Quake 4:** `z_baked_ao.pk4`, dropped into `q4base/`, from the [dude-q4 releases page](https://github.com/Inkub0/dude-q4/releases).

DUDE is GPLv3, source at https://github.com/Inkub0/dude. The Quake 4 game library is under the Quake 4 SDK licence; the DUDE author's code linked into it carries a GPLv3 section 7 permission (`COPYING-QUAKE4-EXCEPTION.txt`).
