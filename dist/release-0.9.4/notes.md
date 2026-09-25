# DUDE 0.9.4 — ray-traced global illumination, volumetric lighting, and subsurface skin

A feature release on top of [0.9.3](https://github.com/Inkub0/dude/releases/tag/v0.9.3).
Where 0.9.3 turned **Ultra Nightmare** into a real hybrid ray-traced tier (RT shadows on
every light, RT reflections, RTAO), 0.9.4 pushes it toward full path tracing: light now
**bounces** — one-bounce ray-traced global illumination — and the air itself scatters light
in **volumetric shafts**. Reflections cover more of the scene, skin stops reading as plastic,
and there's a proper in-game benchmark to measure it all on your own GPU.

The ray-traced additions need the **Vulkan** renderer and an RTX / ray-query-capable GPU.
On anything else Ultra Nightmare still falls back cleanly to the Nightmare raster path, and
the non-RT improvements (subsurface skin, anisotropic filtering, tessellation fixes) work on
both the OpenGL 3.3 and Vulkan backends.

## Changed since 0.9.3

### Ray-traced global illumination (new)

- **One-bounce RT global illumination** (`r_rtgi`, *Lighting → RT Global Illumination*):
  one cosine-hemisphere ray per pixel gathers the incoming light where it lands — that hit
  shaded by the scene's real lights and emissive surfaces — and adds it back as indirect
  diffuse. Rooms pick up colored bounce light they never had: a red wall bleeds onto the
  floor, a lit corridor spills around a corner. Denoised with NVIDIA NRD, gathered at reduced
  resolution with a depth+normal joint-bilateral upscale and a full-res checkerboard option.
- GI **supersedes both SSAO and RTAO** at the RT tier — ambient occlusion is simply this
  same gather with the radiance set to zero, so you run one technique, not three.
- Part of **Ultra Nightmare**. Off by default everywhere else — it is a deliberately
  non-vanilla look. Needs the HDR pipeline (`r_hdr`).

### Volumetric lighting (new)

- **Volumetric lighting** (`r_volumetrics`, *Ultra Nightmare*): the air scatters the scene's
  real lights toward the camera — visible cones under spotlights, shafts through grates,
  monsters cutting dark wedges out of a lit haze. A low-resolution ray march through the
  shared scene-light buffer, composited into the HDR scene after the stock fog.
- Off by default; enabled only by the Ultra Nightmare preset. Vulkan + ray-query + `r_hdr`.

### Ray-traced reflections (extended)

- **Wider coverage**: `func_static` props and dropped items now reflect the real scene
  instead of rendering black; glass reflects the **bumped**, live scene rather than a static
  cube map.
- **Per-pixel emissive** in RT reflections — glowing screens and panels light their own
  reflection correctly.
- Reflected hits are lit through the **projected-light frustum**, so a reflected spotlight
  stops at its cone edge like the real one.
- **Ultra Nightmare runs RT reflections at half resolution** with a steadier temporal
  accumulation (the old quarter-res shimmer is gone), and SSR is retired on the RT tier —
  one Reflections settings group, one technique at a time. Glass reflectivity on visors and
  the cockpit is energy-normalized so it no longer blows out.

### Ray-traced shadows & AO (refined)

- **Grates and fences cast shadows again under RT lights** (`r_rtPerforatedShadows`): the
  0.9.3 limitation where alpha-tested casters dropped out with *RT Shadows for All Lights*
  is addressed with per-pixel gating (about +0.24 ms at a grate).
- **Reduced-resolution RTAO** (`r_rtaoResScale`): AO rays and the NRD denoiser run at a
  fraction of the resolution and bilinear-upscale (Ultra Nightmare 0.75), and RTAO no longer
  leaks a bright line along creases.
- **No shadow ray is launched where the light is already black** or facing away — measured
  the RT-vs-shadow-map cost gap from ~0.9 ms down to ~0.5–0.6 ms in a light-dense scene.
- Mirror reflections are clipped to the mirror surface again (no reflection spill onto the
  frame), and soft-shadow blur got a tile fast-path.

### Materials & shading

- **Subsurface scattering on skin and Hell flesh** (`r_skinShading`, default **on** from
  Low up): a cheap wrapped diffuse warms and softens the lighting terminator so faces and
  flesh stop reading as plastic under the PBR GGX lighting. Direct light only, keyed off the
  PBR classifier. Master toggle in *Graphics → Lighting*, tuning in *Debugging*.
- **Anisotropic filtering actually works now** on the OpenGL 3.3 and Vulkan backends — it was
  silently dead (the backends never reported the hardware capability). Wired into the quality
  presets with a gentle 1/1/2/2/4/4/6 ramp.
- **Tessellation fixes** (Vulkan enemy tessellation): displacement is now **signed** (dents
  as well as bulges), the iris is lifted off the tessellated eye face so eyes stop poking
  through, the min-edge gate is lowered so more of a face tessellates, and the dead seam-weld
  path was removed — mirror seams are no longer pinned in the displacement mask.

### Rendering & tone

- **New HDR / tonemap defaults** tuned for the GI-lit scene: HDR display gamma
  (`r_hdrGamma`) defaults to 1.18 to lift shadows without touching the faithful highlights,
  with retuned overbright, bloom (0.1) and lens-flare (0.25) defaults.
- **Per-bone skinned motion vectors** in the velocity buffer, so FSR2 and the temporal
  passes track animated characters correctly.
- Fixed: the IPN intro cutscene could flash on Vulkan (the backend now presents only on a
  real swap), a GL3 shader-compile break in `interaction.frag`, and the ambient-occlusion
  baker baking texels that were buried under a co-located sub-mesh.

### Tools

- **Real-time timeDemo benchmark** (`com_timeDemoRealTime`): plays a recorded demo at true
  wall-clock speed and reports average / percentile frame times with a CPU/GPU split — the
  way to measure GI, volumetrics and the RT tier on your own hardware. Level-load precaching
  of decal and occlusion maps removes the load-time hitches from the run.
- The **Graphics settings tab was reorganized** into clearer groups.

## Performance notes

Global illumination and volumetric lighting are the heaviest features in DUDE and are
Vulkan + RTX only; they are the reason Ultra Nightmare exists as a separate tier. Rather than
quote numbers from one machine, use the new in-game benchmark (`com_timeDemoRealTime 1` then
`timeDemo demo1`) to see the cost on your GPU. If Ultra Nightmare is too much, the Nightmare
preset gives you the full raster enhancement suite with none of the ray tracing.

## Known limitations

- RT global illumination, volumetric lighting and the rest of the RT tier are **Vulkan-only**
  and need hardware ray-query support; the OpenGL renderers are unchanged by them.
- One-bounce GI is a single indirect bounce, not full path tracing — it adds bounce light and
  ambient occlusion, not caustics or multi-bounce color bleeding.
- Under a light that refreshes its soft shadows at a reduced rate, shadows of moving things
  can still be a frame or two late.

## Setup (same as 0.9.3)

You need the original _DOOM 3_ / _Resurrection of Evil_ game data (`base/*.pk4`,
`d3xp/*.pk4`) from your own copy (Steam/GOG classic, **not** the BFG Edition).

- **Windows (64-bit)**: unzip and copy the **contents** of the `dude-0.9.4-win64`
  folder into your Doom 3 directory, next to `base/` (the included pk4/config files
  merge into `base/`). Or run from anywhere with `+set fs_basepath "C:\path\to\Doom 3"`.
- **Linux (x86_64)**: untar and copy the contents into your Doom 3 directory the same
  way, run `./dude`.

Renderer selection: `+set r_graphicsAPI opengl|opengl3|vulkan` or the in-game menu.
For the full RT tier pick the **Vulkan** renderer and the **Ultra Nightmare** preset.
Configs/saves live in `~/.local/share/dude` (Linux) / `Documents/My Games/dude` (Windows).

## Optional download: baked ambient-occlusion pack

`dude-0.9.4-baked-ao.zip` (~161 MB): precomputed per-material AO maps for subtle contact
shading on world surfaces, props and weapons. **Re-baked for 0.9.4** with the buried-texel
contact fix, so this pack differs from the 0.9.2/0.9.3 one — re-download it if you use it.
Extract into your Doom 3 directory the same way. Entirely optional, and unused at the RT tier
(GI/RTAO replace it there).

DUDE is GPLv3 — source at https://github.com/Inkub0/dude.
