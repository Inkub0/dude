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
   Half-width = `blurScale * dOcc / (dLight - dOcc)`: zero at contact, growing with the gap;
   projected to pixels at the pixel's depth and foreshortened per axis by the geometric normal.
   The origin is rebuilt from the depth buffer, so the unprojection includes the FSR2 jitter
   (projection shear terms) and a distance-growing bias - without them far terrain flickers.
2. **`rtshadow_tiles`** - 8x8 tiles: "holds a lit pixel" / "holds a shadowed pixel that can
   spread", then the reach search (the kernel reaches up to 24 px = 3 tiles). A penumbra only exists near a
   shadow edge; this is what lets the blur skip everything else on ONE fetch.
3. **`rtshadow_blur`** - horizontal then vertical, 33 taps, only in flagged tiles: which shadowed
   neighbours *reach* this pixel (neighbour at x px counts if its half-width >= x), their mean
   half-width, then a Gaussian (sigma = half-width / 2) over visibility. Depth-aware in 1/d, which
   is linear in screen space on a plane, extrapolated with the local slope (grazing floors keep
   their kernel; silhouettes drop out). Screen-space PCSS (MohammadBagher et al. 2010) with an
   exact blocker distance.

Deterministic, stateless: no noise, no history, no motion vectors, no NRD. View-sized targets
shared by all lights (2x RGBA16F + R16F mask + two tile maps: ~66 MB at 1440p), freed of any per-light
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

`r_rtShadowBlurDebug` (since removed) split what was left: **1** = rays only (tiles + blur skipped, hard mask) -
`r_vkGpuTime` vs. 0 is the blur's cost, vs. `r_rtShadowBlur 0` the ray pass's; **2** = per-second
readout of lights blurred per view and the screen area their rects add up to.

**Measured by the user after perf pass 1 (same scene, 3080 Ti):** blur off 8.60 ms; rays only
(`Debug 1`) 9.85 ms; full blur 11.0 ms. So the toll fell from +7.8 to **+2.4 ms**, split about
evenly: **+1.25 ms the ray pass** (over the inline mode-4 rays it replaces) and **+1.15 ms tiles +
blur**.

Perf pass 2: the finished mask is now its own single-channel R16F target (the vertical pass used
to write back into the RGBA16F ray target) - every lit fragment of the light samples it, at a
quarter of the bytes, and the vertical pass writes a quarter of the bytes. `Debug 3` (first-hit rays) was added here to price closest-hit; see below.

**Sgt Kelly's window (user, `Debug 2`):** 2 lights blurred per view, their scissors covering 2.00
screens - two full-screen lights, ~1.2 ms each. `Debug 3` (first-hit rays, since removed) ran at
the same frame rate as closest-hit (88-89 vs 91 views/s): **closest-hit costs nothing measurable**,
so the ray premium is not traversal - it is a screen pass doing work the inline ray never did.

Perf pass 3: `vLight->scissorRect` is the union of the light's surface SCISSORS, and a surface's
scissor is its whole area's portal rect - one lit wall of the viewer's room makes the light "full
screen". The inline ray only ever ran on the fragments of lit surfaces; every screen pass here paid
for the full rect. `RB_RHI_RtShadowBlurRect` now uses the union of the PROJECTED BOUNDS of the
light's opaque interaction surfaces (each clipped to its scissor; near-plane crossers keep their
scissor; padded for jitter + 4 world units of tessellation displacement). `Debug 2` prints both
numbers, so the gain is visible as "blurred rects cover X screens (light scissors: Y)". If X stays
near Y in a scene, that light really does shade the whole screen and ~1.2 ms per full-screen light
at 1440p is this design's floor (one ray pass, tile pass, two blur passes, three target clears).

**Final numbers, Kelly's window (user, `r_vkGpuTime`, 1440p, 3080 Ti):** blur off ~8.88 ms, on
~10.62 ms = **+1.74 ms for two full-screen ray-served lights (~0.9 ms each)**. Both lights
genuinely shade the whole view ("blurred rects cover 2.00 screens (light scissors: 2.00)" - the
viewer's own room crosses the near plane, so projected bounds cannot tighten it). The user's
budget for this scene: *"less than 2 ms is ok to sacrifice for blurred shadows"* - accepted, so
no half-res / fidelity trade was built.

A fourth pass (horizontal pass discarding in unflagged tiles + raw fallback) measured no gain
(10.6 ms before and after) and was reverted; the `r_rtShadowBlurDebug` cost-split cvar is removed.
What remains per full-screen light is fixed: one ray pass, the tile passes, two blur passes, three
target clears. Lights with small lit areas pay proportionally less.

## The "shadows cut along a horizontal line" bug (2026-09-18) - a backend scissor bug, fixed

User, Site 2 Transfer Area, blur on: the top half of the far columns' shadows missing, split along
a screen-horizontal line. The same signature had appeared with the abandoned soft shadows.

Root cause, `VulkanBackend::ApplyDynState`: the GL bottom-up -> Vulkan top-down conversion of the
SCISSOR was keyed on `effFlipY`, the per-draw flag that cancels the viewport's negative-height
mirror when a fullscreen pass samples a color target on unit 0. So any such pass got its scissor
rect taken as top-down: vertically mirrored. With a full-target rect nobody could tell (every
post pass until now); with a light's partial rect, the tile + blur passes (unit 0 = the ray / ping
target) wrote a mirrored band, and the part of the light's real rect outside it kept the clear
value - "lit" - so shadows vanished above/below a horizontal line. (The ray pass itself has
`_currentDepth` on unit 0 and converted correctly, which is why `Debug 1` never showed it.) The
soft-shadow upsample pass had the identical setup.

Fix: where a rect lands follows the TARGET's convention (`curFlipY`); only the mirror follows the
draw (`effFlipY`). Scissor converts on `curFlipY`; the un-mirrored viewport keeps its region
(`y = H - (y + h)`, positive height). Bit-identical for full-target rects, i.e. for every
pre-existing caller. Kelly's window never showed it because both lights' rects were full screen.

## RT shadows for ALL lights - `r_rtAllLights` (2026-09-18, opt-in, not preset-wired yet)

User request: every remaining shadow-casting light ray-traced like the sun / moving lights, behind
its own toggle (Graphics -> Shadows -> Ray Tracing -> "RT Shadows for All Lights"), in two phases.

**Phase 1 - hard RT shadows.** In the light loop, ahead of every map route and independent of
`r_shadowMapping`: a light that may cast shadows and has interactions sets `ictx.lightRtPoint` -
the moving-light route, which is species-agnostic (mode 4 traces from the lit fragment toward
`u_localLightOrigin`: a point light's position, a projected light's apex, a parallel light's
far-away parallel point). No shadow map of any kind is rendered and no stencil volumes are drawn.
The cvar joins the RT-scene implication gates in tr_main.cpp (auto-builds the TLAS, gathers
monster casters). `r_shadowMapDebug 2` prints `map=rtAll`.
Known differences from the map path: perforated grates/fences stop casting (they are `noShadows`
surfaces the shadow-map path special-cases; they are not in the TLAS), and the cost model
inverts - a still light's cached cube map is ~free, a ray is paid per lit pixel per light per frame.

**Phase 2 - blur.** Nothing to route: `r_rtShadowBlur` keys on "ray-served light", so these lights
are softened by the same passes. What changes is the COUNT - a view can now hold a dozen blurred
lights instead of one or two - so the per-light fixed cost matters. Each light's passes on the
view-sized targets used to begin with a full-target clear (2x RGBA16F + R16F = ~66 MB of writes
per light at 1440p, whatever the light's size). New RHI call `SetNextTargetPassArea` (one-shot;
Vulkan: the render pass's `renderArea`, GL3: ignored) restricts the clear + the pass to the
light's rect; outside it the target is undefined, so the rects are nested so that no stage reads
beyond the previous one's area, and the ray rect is grown to whole 8x8 tiles (top-down aligned)
so the tile classification reads exactly what the rays wrote.

Status: builds. **NOT runtime-tested**; cost unmeasured - needs the user's `r_vkGpuTime` with
All Lights on, blur off vs on, in a light-dense room.

## Cvars

- `r_rtAllLights` (0): ray-trace every shadow-casting light (see above).
- `r_rtShadowBlur` (0): the toggle. Off = exactly the previous hard RT shadows.
- `r_rtShadowBlurIntensity` (1.5, 0..4; Debugging -> RT Shadows -> "Blur Intensity"): scales the blur width. Replaced
  the "Light Size" / sun-angle pair (user, 2026-09-18: with the emitter model gone a "light size"
  has no reason to exist - it is a blur amount). Internally the width is still
  `3 * intensity * dOcc / (dLight - dOcc)` world units (point lights x `max(1, largest radius axis
  / 256)`; parallel suns 1 degree x intensity), so contact hardening is unchanged.
- `r_rtShadowBlurCurve` (1.45, 0.25..4; Debugging -> RT Shadows -> "Blur Falloff Curve"): the exponent on the gap ratio -
  width = `blurScale * g^curve`, `g = dOcc / (dLight - dOcc)` (caster-to-surface over
  caster-to-light distance). 1 = geometric growth; < 1 softens sooner then levels off; > 1 stays
  crisp longer near the caster. Defaults 1.5 / 1.45 are the user's tuned look (2026-09-18); the
  Graphics tab keeps only the on/off toggle. g is usually < 1 indoors (casters sit nearer the surface than the
  light), which is the range those descriptions assume; for g > 1 the effect inverts.

## Limits

- Half-width capped at 24 pixels (16 at first; 32 measured ~+2.2 ms GPU in the user's scene, so
  24 is being tried, 2026-09-18): the cap is the
  blur's tap count per side and the tile search radius. Because the sweep is bounded by the widest
  shadow that actually reaches a tile, the higher cap only costs where a penumbra really is that
  wide (up to 49 taps per pass there); narrow shadows cost what they did before.
- A blur of a centre-ray shadow cannot show an emitter partly visible around a thin occluder.
- Only ray-served lights soften; shadow-mapped lights keep their PCF edge.

## Status

**USER-VERIFIED 2026-09-18** ("the blurring seems great"); cost accepted at +1.74 ms worst case
measured. Fidelity: changes the look of shadows, so it is on only in the **Ultra Nightmare** preset
(with the RT shadow routes it builds on) and a menu toggle everywhere else; docs/todo.md row added.
