# Benchmark (`timeDemo`)

DUDE keeps Doom 3's `timeDemo` command as its benchmark, but changes what it measures and
what it reports.

## Why the classic timedemo no longer fits

Doom 3's render demos are recorded at 30 Hz (one `DC_RENDERVIEW` every two 16 ms game tics).
The original `timeDemo` consumed **one demo frame per rendered frame**, so on any machine faster
than 30 fps the demo fast-forwards: at 300 fps `demo1` plays at 10x speed and is over in seconds.
That was the point on 2004 hardware (the number of demo frames divided by wall time is a pure
render-throughput figure), but DUDE decouples rendering from the 60 Hz game tic, so real play
renders many frames per world update. A fast-forwarded demo measures a different workload (every
frame carries a full world-state update and there is no frame-to-frame coherence for the shadow
caches, temporal passes, RT accumulation and so on).

There was also a DUDE-specific cap: the main loop's end-of-frame sleep (`Com_WaitForNextTicStart`)
waited for the next tic whenever a frame finished inside one, so a classic timedemo could not
exceed the tic rate unless `com_interpolate` happened to be on. The loop now free-runs whenever a
timeDemo is active, in both modes.

## Real-time mode (`com_timeDemoRealTime 1`, default)

The demo advances on the tic clock exactly like `playDemo` (`AdvanceRenderDemo` computes how many
demo frames are due from `latchedTicNumber`), and the engine renders as many frames as it can in
between; when no demo frame is due the current `renderView` is rendered again. The result is the
demo at its recorded speed with the renderer running the way it does in play.

- Faster than 30 fps: several rendered frames per demo frame (like `com_interpolate` gameplay).
- Slower than 30 fps: demo frames are dropped to stay on the clock. `playDemo` caps the drop at
  4 frames per render (slow motion); the benchmark does not, so the wall time stays honest. The
  report flags a run that still could not keep up.

`com_timeDemoRealTime 0` restores the classic one-demo-frame-per-frame behaviour for comparison
with old numbers.

### Aspect ratio

A render demo carries the field of view of the display it was recorded on (`demo1` is 4:3:
fov_x 90, fov_y 73.7) and its view rect is mapped onto the whole screen, so vanilla playback on
a widescreen display is stretched horizontally. `Demo_FitViewToScreen` (all demo playback, not
just the benchmark) keeps the recorded vertical fov and derives fov_x from the real viewport
aspect, the same Hor+ rule `idGameLocal::CalcFov` applies in play, so the 3D view has the same
proportions and field of view the game would have at that resolution. The recorded 2D HUD
overlay is still a 640x480 GUI stretched like any other, as in play.

`timeDemo demo1 twice` (any second argument) still plays the demo once un-timed to precache.
Sound stays muted in both modes, as before.

## What is measured

`idSessionLocal::TimeDemoFrame` runs once per rendered frame while the demo plays and samples:

| sample | source |
|---|---|
| wall frame time | `Sys_MillisecondsPrecise` between consecutive frames |
| CPU frontend / backend ms | `renderFrameStats_t` (new sub-millisecond `pc.frontEndMs`, `backEnd.pc.ms`) |
| GPU frame time | the backend frame timer (`GL_TIME_ELAPSED` on opengl3, timestamp queries on Vulkan), kept running by `SetGpuTimerRequested` without the `r_*GpuTime` prints |
| draw calls, triangles, lights, entities, views | the `tr.pc` / `backEnd.pc` counters, sampled once per **demo** frame (see below) |
| demo length | demo frames x 2 tics on the 60 Hz tic clock (game time runs 16 ms/tic, 4% shorter, so `renderView.time` would understate it) |

The RHI executor (`RB_RHI_ExecuteBackEndCommands`) never timed itself before, so `com_speeds`'s
`bk:` column was always 0 on opengl3/Vulkan. It is filled now.

`renderFrameStats_t` / `idRenderSystem::GetFrameStats` are appended at the end of the
`idRenderSystem` interface so game DLLs built against the older vtable keep working.

## The report

Printed to the console and written to `benchmarks/<demo>-<YYYYMMDD-HHMMSS>.txt` under
`fs_savepath` (followed by a dump of every renderer and system cvar, so two runs can be diffed).
One CSV row per run is appended to `benchmarks/history.csv` for tracking a machine over time.

```
==== DUDE benchmark: demo1 ====
DUDE 0.9.3  |  2026-09-24 15:30:12
backend: vulkan  |  GPU: NVIDIA GeForce RTX 3080 Ti (driver ...)
display: 2560x1440 fullscreen, vsync off, com_maxFPS 0
preset: Ultra Nightmare (6)  |  active: r_rhiAA=2 r_fsr=1 r_hdr=1 r_pbr=1 ...
playback: real-time (30 Hz demo clock, rendering decoupled)  |  demo: 2148 frames = 71.6 s, played in 71.6 s
frames rendered: 9345 in 74.6 s = 125.3 fps
frame time (ms): avg 7.98  median 7.71  min 4.12  max 41.20
1% low: 62.1 fps (99th percentile 16.10 ms)   0.1% low: 30.4 fps (99.9th percentile 32.90 ms)
hitches (frame > 19.3 ms = max(2.5x median, median + 5 ms)): 12 frames, worst 41.2 ms
CPU renderer (ms/frame): frontend 1.92  backend 2.41  total 4.33 (56% of the median frame)
GPU (ms/frame): avg 6.81  max 20.10  (88% of the median frame -> GPU-bound)
scene (per demo frame avg, 2148 sampled): 412 draw calls, 180k triangles, 12.3 lights, 61.0 entities, 1.0 views
report: benchmarks/demo1-20260924-153012.txt (fs_savepath)
```

Definitions:

- **1% low / 0.1% low**: 1000 divided by the 99th / 99.9th percentile frame time (the RTSS /
  reviewer convention), not the mean of the slowest 1%.
- **hitches**: frames longer than 2.5x the median, but never less than median + 5 ms (at
  1000 fps a 2 ms frame is not a hitch).
- **GPU-bound / CPU-bound**: GPU time above 85% / below 60% of the median frame. A hint, not a
  verdict; vsync or `com_maxFPS` cap the result and the header says so.
- **CPU renderer total** is frontend + backend only; game logic does not run during a render demo.
- **scene averages** are per demo frame, not per rendered frame. In real-time mode a demo frame
  is re-rendered as often as the fps allows, and averaging over rendered frames would weight cheap
  scenes (more re-renders) over heavy ones, differently on each backend. Sampled once per demo
  frame they describe the demo's content; lights and entities are frontend counts and should match
  across backends, draw calls and triangles are incremented at backend-specific sites (post passes,
  the Vulkan Z-fill) and are comparable within a backend rather than across.
- Triangle counts on the RHI backends only cover the draw paths that report indexes; treat them as
  a lower bound. The legacy `opengl` backend has no GPU timer.

The in-game results box shows the average, the 1% low, the frame count, the GPU average and the
report file name; the rest is in the console and the file.

## Occlusion-map precache

r_hitchTrace attributed demo1's remaining 20-36 ms frames to baked occlusion maps
(`generated/aomaps/<model>_sN`) loaded on the render thread the first time each never-before-lit
model was drawn (7-27 ms each; the per-skin name is resolved at draw time so the level-load image
preload missed them). `RB_RHI_PrecacheOcclusionMapsForModel`, called from the model manager's
`EndLevelLoad` while the image manager is still `insideLevelLoad`, touches every existing AO file
for the level's loaded models so the following `globalImages->EndLevelLoad` loads them on the
parallel loader, resident before play. It only loads existing files (never bakes), reuses the same
idImage the draw path resolves, and honors `r_occlusionMaps` and the new `r_occlusionMapPrecache`
(default 1) live, so the in-game quality toggles keep working and a mid-game enable falls back to
the old lazy load. Same fix removes the equivalent hitch when walking into a new area in play.

## Where to find the data after a run

- **Console**: the whole report is printed just before the results box. Open the console and
  scroll up (PgUp), or run `condump <name>` to write the console to `<name>.txt`.
- **stdout**: everything the console prints also goes to the terminal that launched DUDE.
- **Files**: `benchmarks/` inside the game folder under `fs_savepath`. With `run.sh` that is the
  repository (`base/benchmarks/`, or `d3xp/benchmarks/` when playing the expansion); with a stock
  launch it is `~/.local/share/dude/base/benchmarks/`. One `<demo>-<timestamp>.txt` per run,
  followed by the full renderer/system cvar dump, plus the running `history.csv`.
