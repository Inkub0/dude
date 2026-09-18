# DUDE 0.9.3 — the RTX release: ray-traced shadows on every light, reflections and ambient occlusion

A feature release on top of [0.9.2](https://github.com/Inkub0/dude/releases/tag/v0.9.2), and
almost all of it is ray tracing. 0.9.2 introduced the first RT elements (sun, moving lights,
monster shadows); 0.9.3 turns **Ultra Nightmare** into a real hybrid ray-traced tier: **every
shadow-casting light is ray-traced and softened**, reflective floors and glass show **ray-traced
reflections** of the room and its monsters, and screen-space AO is replaced by **ray-traced
ambient occlusion** with NVIDIA's NRD denoiser.

Everything here needs the **Vulkan** renderer and an RTX / ray-query-capable GPU. On anything
else Ultra Nightmare still falls back cleanly to the Nightmare raster path.

## Changed since 0.9.2

### Ray-traced shadows

- **RT shadows for every light** (`r_rtAllLights`, *Shadows → Ray Tracing → RT Shadows for All
  Lights*): every shadow-casting light — point, projected and parallel — now takes the
  pixel-exact ray route that only the sun and moving lights had in 0.9.2. No shadow maps are
  rendered at all while it is on: no map resolution, no bias artefacts, no light budget.
- **Soft RT shadows** (`r_rtShadowBlur`, *Soften RT Shadows*): ray-traced shadows stay sharp
  where an object touches the surface it shadows and soften as the gap grows, like a real light.
  Deterministic — no noise and nothing accumulated over frames, so nothing swims or lags when you
  move. Tune it under *Debugging → RT Shadows*: **Blur Intensity** and a **Blur Falloff Curve**
  that shapes how quickly shadows soften with distance from their caster.
- **Distant soft shadows refresh less often** (`r_rtShadowBlurStagger`, *Refresh Distant Soft
  Shadows Less Often*): lights close to you refresh their soft shadows every frame, farther ones
  every 2nd, the farthest every 3rd, re-projected to the current camera in between. Roughly
  halves the cost of softening a light-dense room, with no visible change in testing. The two
  distances are tunable under *Debugging → RT Shadows*.
- All three are part of **Ultra Nightmare**.

### Ray-traced reflections

- **Ray-traced reflections** (`r_rtReflections`, *Reflections → Ray-Traced Reflections*):
  reflective floors and metal now reflect the real room **and its monsters** — including things
  that are off screen or hidden from the camera, which screen-space reflections can never show.
  Reflected surfaces are textured per texel, and the result is temporally upscaled so it stays
  affordable.
- **Glass reflects the live scene** instead of a static cube map, with grazing-angle Fresnel and
  a sky gradient where a ray escapes; on the SSR tier glass gets a screen-space version of the same.
- Fireflies on glossy floors are clamped (`r_ssrFireflyClamp`), and reflection fall-off with
  roughness is tunable (`r_ssrRoughnessFade`).

### Ray-traced ambient occlusion

- **RTAO** (`r_rtao`, *Ambient Occlusion → RT Ambient Occlusion*): one ray per pixel against the
  real scene, denoised by **NVIDIA NRD (ReBLUR)**, replaces the screen-space GTAO at Ultra
  Nightmare — occlusion from geometry the screen can't see, no screen-edge fade. Own intensity
  control (`r_rtaoIntensity`).
- **NVIDIA NRD 4.18** is now built into the engine as the denoising foundation for the RT tier.

### RT under the hood

- **Animated monsters as GPU-resident ray-tracing instances**: skinned characters feed the
  acceleration structure straight from the GPU skinning buffers and are refitted instead of
  rebuilt (`r_rtAnimBlas`) — the groundwork RT reflections of monsters are built on.
- **Stability**: fixed the GPU hangs / device-lost seen in heavy combat with RT on
  (acceleration-structure rebuilds are now ordered after in-flight ray traversal; rays with
  degenerate directions are never launched).

### Rendering

- **Cutscenes render correctly with every depth-based effect.** The game lowers the camera's near
  plane during cinematics, but SSAO, RTAO, screen-space and RT reflections, soft particles and the
  new soft shadows all assumed the gameplay value — so in cutscenes they worked from positions
  three times too far away. They now read it from the view itself.
- **Eye adaptation stands down during cutscenes** and resumes by itself afterwards, so the
  exposure no longer pumps across camera cuts.
- **New "DUDE" tonemap curve** (`r_hdrTonemap 5`), HDR gamma control and an experimental adaptive
  white point. Default HDR exposure raised to 1.5 (`r_hdrExposure`).
- **Smooth rendering above 60 fps by default**: render interpolation (`com_interpolate`) is now
  part of every preset above Potato.
- **Cheaper ambient occlusion**: the normal pre-pass is merged with the depth pre-pass on Vulkan
  (one geometry pass instead of two), and Ultra Nightmare runs AO at full resolution.
- **Cheaper GPU skinning**: the joint palette is uploaded once per character instead of once per
  surface, fixing the frame-rate drops around multi-surface monsters.
- Fixed: with FSR2 / motion vectors on, the first-person weapon and hands could render black and
  the GPU could be lost (invalid motion vectors on the view weapon).
- Smoke darkness blending defaults retuned (darkness floor 55%, full opacity at 20% light);
  refreshed PBR material list.

## Performance notes (RTX 3080 Ti, 1440p, busy Mars City scene)

- RT shadows for all lights: about **+0.25 ms** GPU — the rays replace every shadow-map render.
- Softening all of them: about **+5 ms**; with *Refresh Distant Soft Shadows Less Often* (on in
  Ultra Nightmare) about **+2.7 ms**.
- If that is too much for your GPU, turn off *Soften RT Shadows* first: the hard ray-traced
  shadows on every light are nearly free.

## Known limitations

- With *RT Shadows for All Lights* on, **grates and fences do not cast shadows** (they did
  through the shadow-map path). Turn the option off if a scene depends on them.
- Under a light that refreshes at a reduced rate, shadows of moving things can be a frame or two
  late, and re-projected pixels without valid data fall back to a hard-edged ray for that frame.
- Ray tracing is Vulkan-only and needs hardware ray-query support; the OpenGL renderers are
  unchanged by the RT features.

## Setup (same as 0.9.2)

You need the original _DOOM 3_ / _Resurrection of Evil_ game data (`base/*.pk4`,
`d3xp/*.pk4`) from your own copy (Steam/GOG classic, **not** the BFG Edition).

- **Windows (64-bit)**: unzip and copy the **contents** of the `dude-0.9.3-win64`
  folder into your Doom 3 directory, next to `base/` (the included pk4/config files
  merge into `base/`). Or run from anywhere with `+set fs_basepath "C:\path\to\Doom 3"`.
- **Linux (x86_64)**: untar and copy the contents into your Doom 3 directory the same
  way, run `./dude`.

Renderer selection: `+set r_graphicsAPI opengl|opengl3|vulkan` or the in-game menu.
For the full RT tier pick the **Vulkan** renderer and the **Ultra Nightmare** preset.
Configs/saves live in `~/.local/share/dude` (Linux) / `Documents/My Games/dude` (Windows).

## Optional download: baked ambient-occlusion pack

`dude-0.9.3-baked-ao.zip` (~100 MB): precomputed per-material AO maps for subtle
contact shading on world surfaces, props and weapons — unchanged since 0.9.2, so skip it if
you already have it. Extract into your Doom 3 directory the same way. Entirely optional.

DUDE is GPLv3 — source at https://github.com/Inkub0/dude.
