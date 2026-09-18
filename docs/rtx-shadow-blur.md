# RT shadow blur — a screen-space blur on top of the hard ray-traced shadows

`r_rtShadowBlur` (Vulkan + RT hardware, default 0, Shadows → Ray Tracing → "Soften RT Shadows").

## Where this came from

The H3 attempt ([rtx-hybrid-roadmap.md](rtx-hybrid-roadmap.md)) gave lights a physical size:
random rays over an emitter, denoised by NRD SIGMA, for a budget of lights. It never got rid of a
shadow "stagger" while strafing (six diagnoses, none confirmed), cost ~0.5 ms and ~25 MB per
light, and its deterministic successor - every light traced + blurred per frame - cost 11.4 ms in a
light-dense room, because it replaced CACHED cube shadow maps (free at rest) with per-frame rays.
The user called it (2026-09-18): *revert all of it, keep the old ray-traced "stencil style"
shadows, and just put a blur on top.* Both soft-shadow branches (`feat/rtx-soft-shadows`,
`feat/rtx-soft-shadows-spatial-blur`) are abandoned, unmerged; this feature starts from master.

## What it does

Nothing about light routing changes. The engine already serves two kinds of light with one hard
ray per lit fragment, inline in the interaction shader (mode 4): suns (`r_rtSunShadows`) and
moving point lights (`r_rtMovingLights`). Every other light keeps its cached shadow map. With
`r_rtShadowBlur 1`, those ray-served lights - and only those - get their shadow blurred.

A forward per-light renderer multiplies the shadow into the light inside the interaction shader, so
the shadow has to exist as an image before it can be blurred. Per blurred light, inside the light
loop right before its interactions draw (`RB_RHI_RtShadowBlurLight`, RhiWorld.cpp; the scene pass is
suspended exactly as for a shadow-map render):

1. **`rtshadow_ray`** - the mode-4 ray for every screen pixel in the light's scissor rect, toward
   the light's centre (deterministic), closest hit so the occluder distance is known. RGBA16F:
   visibility, penumbra half-width in pixels per screen axis, view distance. The interactions then
   look the mask up (mode 5, unit 13) instead of tracing, so the ray count is unchanged - it moved.
   Half-width = `lightRadius * dOcc / (dLight - dOcc)`: zero at contact, growing with the gap;
   projected to pixels at the pixel's depth and foreshortened per axis by the geometric normal.
   The origin is rebuilt from the depth buffer, so the unprojection includes the FSR2 jitter
   (projection shear terms) and a distance-growing bias - without them far terrain flickers.
2. **`rtshadow_tiles`** - 8x8 tiles: "holds a lit pixel" / "holds a shadowed pixel that can
   spread", then dilated 5x5 (the kernel reaches 16 px = 2 tiles). A penumbra only exists near a
   shadow edge; this is what lets the blur skip everything else on ONE fetch.
3. **`rtshadow_blur`** - horizontal then vertical, 33 taps, only in flagged tiles: which shadowed
   neighbours *reach* this pixel (neighbour at x px counts if its half-width >= x), their mean
   half-width, then a Gaussian (sigma = half-width / 2) over visibility. Depth-aware in 1/d, which
   is linear in screen space on a plane, extrapolated with the local slope (grazing floors keep
   their kernel; silhouettes drop out). Screen-space PCSS (MohammadBagher et al. 2010) with an
   exact blocker distance.

Deterministic, stateless: no noise, no history, no motion vectors, no NRD. Four view-sized
targets shared by all lights (2x RGBA16F + two tile maps: ~59 MB at 1440p), freed of any per-light
state. Translucent interactions and depth-hacked view weapons keep the inline mode-4 ray (the
mask describes the opaque depth buffer only). Needs the normal G-buffer prepass (SSAO / RTAO /
motion vectors); without it, or on any failure, lights keep their hard ray.

## Perf pass 1 (2026-09-18: user measured 8.6 -> 16.4 ms GPU with the blur on)

Not profiled by me (the game is the user's to run); fixed by reading the first cut for work that
could not affect the image:

- **The sweep was always 33 taps**, in every flagged tile, although a tap further away than the
  shadow's own half-width has zero weight - and typical half-widths are a few pixels. Tiles now
  carry the widest half-width that can REACH them (`rtshadow_tiles` pass 2, G channel) and the blur
  sweeps only that far (+1). Same output, usually 7-13 taps instead of 33.
- **Tiles were flagged in a blanket 5x5 (40 px) around any shadow**, so in a room full of shadow
  edges most of the screen ran the sweep. A neighbour n tiles away now only counts if its
  half-width spans the (n-1)*8 px gap, and a tile is flagged only if a lit pixel lies within that
  reach (a fully shadowed neighbourhood cannot change).
- **`vec4 tap[33]`** (528 bytes per invocation) spilled to slow local memory. Removed: each loop
  fetches its own taps (the second fetch is a texture-cache hit).
- **Tile pass 1 read the whole screen per light** (64 texels x every tile): a fixed cost that made
  a small moving light as expensive as a full-screen one. Scissored to the light's rect.

Still per light and unavoidable in this design: the ray pass is closest-hit (mode 4 stops at the
first hit), three view-sized target clears, and five render-pass switches.

`r_rtShadowBlurDebug` splits what is left: **1** = rays only (tiles + blur skipped, hard mask) -
`r_vkGpuTime` vs. 0 is the blur's cost, vs. `r_rtShadowBlur 0` the ray pass's; **2** = per-second
readout of lights blurred per view and the screen area their rects add up to.

## Cvars

- `r_rtShadowBlur` (0): the toggle. Off = exactly the previous hard RT shadows.
- `r_rtShadowBlurLightSize` (3): light sphere radius in world units = the softness. Point lights
  scale it by `max(1, largest light_radius axis / 256)`.
- `r_rtShadowBlurSunAngle` (1.0): angular radius of parallel suns, degrees.
- `r_rtShadowBlurDebug` (0, not archived): 1 = rays only, 2 = lights/coverage readout (remove once the cost is settled).

## Limits

- Half-width capped at 16 pixels: very wide penumbrae come out harder than physical.
- A blur of a centre-ray shadow cannot show an emitter partly visible around a thin occluder.
- Only ray-served lights soften; shadow-mapped lights keep their PCF edge.

## Status

**USER-VERIFIED 2026-09-18** ("the blurring seems great"). Fidelity: changes the look of shadows,
so opt-in. Open: GPU-time delta on/off (`r_vkGpuTime`) not yet reported; Ultra Nightmare preset row
+ docs/todo.md table not wired yet.
