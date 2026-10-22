# DUDE 0.9.6 — the Path Tracing tier

A ray-tracing release on top of [0.9.5](https://github.com/Inkub0/dude/releases/tag/v0.9.5).
0.9.5 was about speed; 0.9.6 is about the **light**. The top Vulkan tier is now a full
ray-traced **Path Tracing** path: direct lighting is sampled and denoised with ReSTIR instead
of rasterised, a ReGIR light grid and a multi-bounce irradiance cache make that affordable, and
the whole stack has been retuned into a new shipping baseline that matches what the author
actually plays on.

Everything below the Path Tracing tier is unchanged — the OpenGL renderers and the lower
Vulkan presets render exactly what they did in 0.9.5.

## Changed since 0.9.5

### Ray-traced direct lighting — the new Path Tracing tier

- **ReSTIR direct illumination.** At the Path Tracing tier (Vulkan, top `dude_rtx` setting) the
  scene's lights are sampled as area lights through reservoir resampling (ReSTIR DI) and denoised
  with NRD ReLAX, replacing the rasterised light loop. Soft, correct direct shadows and
  per-light shading, with the lights that are actually visible in the frame always entering the
  sample set.
- **ReGIR light grid** (`r_rtLightGrid*`): a world-space grid builds a small per-cell importance
  set of lights each frame, so the DI gather samples a handful of relevant lights per pixel
  instead of the whole scene — the single biggest cost cut on the tier (gather time **≈ −68%**).
  Looked up with a per-pixel half-cell jitter to hide the grid, and ranked by each light's own
  falloff.
- **Multi-bounce irradiance cache**: a hashed world-space cache stores bounce irradiance and
  feeds additional light bounces back into the gather (and serves the GI path tail), so light
  carries further through a scene for little extra cost. History and resets are tracked in time,
  not frames, and cells reset only when the DI gradient says the light really changed —
  flickering lights blend instead of restarting.
- **SH bump detail** (`r_rtReStirDISH`, on at the tier): the denoised direct light is
  re-modulated per texel against the surface bump/normal, bringing back the fine relief that a
  reduced-resolution denoised buffer would otherwise flatten — without the "watermelon" banding
  the naive version produced.
- **Material fidelity in the gather**: skin and flesh get subsurface warmth at the terminator, a
  full-resolution specular re-shade sharpens highlights, death-fade crossfades blend instead of
  popping to black, and surfaces with no diffuse stage read black (not white) albedo. Decals no
  longer pick up a direct-light glint, and per-stage vertex colour is honoured for terrain
  blends.

### Ray-traced GI, AO and shadows

- **GI bounce sampling moved to an R2 low-discrepancy sequence**, replacing the interleaved
  gradient noise whose 45° lattice streaked diagonally through the denoiser. RTAO got the same
  treatment. Cleaner GI and AO with no extra cost.
- **RT shadows now scale from Medium up to Nightmare** instead of only the top tier, and the soft
  grate shadows take the shape of the shadow-map caster (no more cage-sphere approximation) and
  soften under the same blur law as the rest of the RT shadows. RT shadow-blur intensity and
  falloff were retuned softer.
- **Irradiance cache and eye adaptation enable wherever RTGI runs**; the cache path tail stays
  Path-Tracing-only.

### A retuned, hand-balanced baseline

- The **ReSTIR DI, RTGI, light-grid, irradiance-cache, RTAO and RT-shadow defaults were all
  rebaked** from the author's own dialed-in configuration — the cache/quality/frame-time balance
  that the Path Tracing tier actually ships at. Fresh installs now start from that balance rather
  than the raw code defaults. The quality presets still own the per-tier enables and resolutions;
  only the preset-independent "free knobs" were moved.
- The DI debug / tuning surface in the in-game menu was reorganised into clear groups with every
  ReLAX and GI denoiser knob exposed, and Film Grain got its own labelled switch + knob block.

### Load time

- **Level sounds now decode in parallel** across the image/worker threads instead of
  synchronously inside material parsing — another cut to cold level-load time, on every backend.

### Fixes

- The monster **spawn-in flash** is toned down and reddened (it was a blinding white pop), in
  both Doom 3 and Resurrection of Evil.
- **AO bake**: eyewear is excluded from the bake and refused at runtime, and translucent surfaces
  no longer occlude the bake.
- **GPU skinning**: the seam weld is decoupled from lighting (crack-free and cut-free), with a
  toggle in the GPU Offload settings group.
- **Windows / Proton: animated characters no longer smear.** The MD5 GPU-skinning compute kernels
  were runtime-compiled on first use, which needs a shader compiler the Windows build doesn't ship —
  so GPU skinning silently stayed off there, and without it skinned characters wrote no motion
  vectors and smeared/ghosted under the temporal passes (RT denoiser, FSR2) while the static world
  stayed sharp. Those kernels are now compiled to SPIR-V at build time and embedded like every other
  shader, so GPU skinning and skinned motion vectors work on the Windows/Proton build too. (Linux was
  unaffected.)

### Emissive Mode — experimental, shipping OFF (incomplete)

0.9.6 includes a large amount of in-progress work on an **Emissive Mode** for the Path Tracing
tier — a system that turns a level's self-illuminated surfaces (light fixtures, screens, glowing
materials) into real emitters that light the scene directly, with a per-material emitter table
and an in-game emitter editor. **This feature is not finished and ships disabled by default.** It
is not wired into any quality preset and is gated behind its own cvars
(`r_rtEmissiveLighting`, `r_rtEmissiveTable`, `r_rtEmissivePhysical`); leave them off for the
intended 0.9.6 experience. It is included only so the work is in the open — treat it as a preview
that may change or break, not a supported feature.

## Performance notes

The Path Tracing tier is demanding — it is a full ray-traced lighting path. Use it on a capable
Vulkan GPU, and lean on **FSR2 sub-native upscaling** (*Graphics → Upscaling*, from 0.9.5) and
the per-tier resolution/checkerboard controls to hit your frame-time budget. The ReGIR light grid
and the irradiance cache are what make the tier playable; both are on automatically at the tier.
Measure on your own GPU with the in-game benchmark (`com_timeDemoRealTime 1` then
`timeDemo demo1`).

## Known limitations

- The Path Tracing tier — ReSTIR DI, the ReGIR light grid, the irradiance cache and the SH bump
  detail — is **Vulkan-only**. The OpenGL renderers get the load-time win and the spawn-flash and
  AO-bake fixes, but none of the ray-traced lighting.
- **Emissive Mode is incomplete** and ships off (see above).
- The retuned defaults only reach **fresh installs or a graphics-config reset** — an existing
  config keeps your current values. Delete your graphics settings (or use the in-game reset) to
  pick up the new baseline.

## Setup (same as 0.9.5)

You need the original _DOOM 3_ / _Resurrection of Evil_ game data (`base/*.pk4`,
`d3xp/*.pk4`) from your own copy (Steam/GOG classic, **not** the BFG Edition).

- **Windows (64-bit)**: unzip and copy the **contents** of `dude-0.9.6-win64` into your
  Doom 3 directory, next to `base/` (the included pk4/config files merge into `base/` and
  `d3xp/`). Or run from anywhere with `+set fs_basepath "C:\path\to\Doom 3"`.
- **Linux (x86_64)**: untar and copy the contents into your Doom 3 directory the same way,
  run `./dude`.

Renderer selection: `+set r_graphicsAPI opengl|opengl3|vulkan` or the in-game menu. For the
full ray-traced tier pick the **Vulkan** renderer and turn ray tracing up to **Path Tracing**.
Configs/saves live in `~/.local/share/dude` (Linux) / `Documents/My Games/dude` (Windows).

If the **Vulkan renderer won't start** (the OpenGL renderers are unaffected), update or
reinstall your GPU driver — the Vulkan driver (the ICD the loader dispatches to) ships with it,
and no bundled loader can substitute for a missing or Vulkan-incapable one. On Windows,
`vulkan-1.dll` comes from your GPU driver; DUDE deliberately does not ship it. On Linux a
current loader is bundled in `lib/` as a fallback for old distro loaders, but it still needs
your driver's ICD.

## Optional download: baked ambient-occlusion pack

`dude-0.9.6-baked-ao.zip`: precomputed per-material AO maps for subtle contact shading on world
surfaces, props and weapons. The world-surface pack was re-baked this release with the bake fixes
above (translucent surfaces no longer occlude); the prop and weapon packs are unchanged from
0.9.5. Extract into your Doom 3 directory the same way. Entirely optional, and unused at the Path
Tracing tier (ray-traced GI/AO replace it there).

DUDE is GPLv3 — source at https://github.com/Inkub0/dude.
