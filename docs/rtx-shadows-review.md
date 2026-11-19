# RT shadows: system review and suggestions (2026-11-19)

Written after a day of fixing characters under ray-traced lights (docs/rtx-shadow-blur.md,
"Characters under ray-traced lights"). It reviews the ray-traced shadow system as a whole on the
RTX hybrid tier (`dude_rtx 1`), and suggests changes grounded in published work. It does not
re-open decisions already made with data (docs/rtx-sigma-shadows.md section 8, the abandoned
stochastic soft shadows of docs/rtx-shadow-roadmap.md R5); where a suggestion touches one, it says so.

Status of every claim: **measured** means a number from this project's own profiling notes;
**estimate** means mine and unverified; **unmeasured** means nobody has timed it yet.

## 1. The system today

A forward renderer: each light draws its interactions, and the shadow is one factor in that draw.

| Piece | What it does | Where |
|---|---|---|
| Hard ray (mode 4) | One ray per lit fragment toward the light's centre, from the interpolated surface point pushed `r_rtSunShadowOffset` (1 unit) along the vertex normal | `interaction.frag` |
| Softened mask (modes 5 / 6) | The same ray traced per screen pixel from the DEPTH BUFFER into a mask, then a tile-bounded, depth-aware, variable-width separable blur. Width = `blurScale * gap^curve`, gap = occluder distance / (light distance - occluder distance). Checkerboard ray, optional staggered refresh with reprojection | `rtshadow_ray / _tiles / _blur.frag` |
| Character cone (mode 7) | For RT lights with no mask: n rays over the light's physical size, 2x2 quad average | `rtSoftConeVis` |
| RT contact shadows | A second, short ray (14 units, 12 degree cone, 40% strength) from 0.05 units off the surface, outside hits only | `rtContactOcclusion` |
| Casters | World areas and static models as BLASes, animated characters as a per-frame refit of the COARSE skinned mesh, grates as alpha-tested candidates | `tr_main.cpp`, `VulkanBackend.cpp` |

What is good and should stay: it is deterministic and stateless (same camera, same image), it needs
no denoiser, the blur hardens at contact from an exact blocker distance (screen-space PCSS,
MohammadBagher et al. 2010, after Fernando 2005), and the tile classification keeps the blur off
pixels with no shadow edge. Those were deliberate choices after the stochastic version failed.

## 2. Findings

### F1. Where a shadow ray starts is the system's weak point

Every defect fixed on 2026-11-19 came from the ray's start point or from skipping the ray:

- The hard ray is pushed a fixed **1 world unit** off the surface. That is far more than numerical
  safety needs. It is there to clear the mismatch between the rendered surface (tessellated,
  displaced) and the ray-traced copy (coarse, flat triangles), and its price is that occluders
  closer than a unit are missed: the nose over the lip. RT contact shadows exist to patch that.
- The mask's ray has no surface point at all. It rebuilds one from the depth buffer and pushes it
  along a normal computed from depth derivatives, which is per flat triangle, undefined at
  silhouettes, and points sideways in a crease.

Published practice:

- Waechter and Binder, "A Fast and Robust Method for Avoiding Self-Intersection" (Ray Tracing Gems,
  2019, ch. 6): offset along the GEOMETRIC normal by an amount scaled to the floating-point error of
  the hit position, not by a scene-scale constant.
- Hanika, "Hacking the Shadow Terminator" (Ray Tracing Gems II, 2021, ch. 4): for a smooth-shaded
  mesh traced as flat triangles, move the shadow ray's start onto the smooth surface the vertex
  normals imply. That removes facet self-shadowing without a large offset.
- Epic's own documentation states the same proxy problem for Unreal: ray-traced shadows use the
  Nanite fallback mesh, and it warns of visual artifacts because that mesh has fewer triangles. Lumen's
  answer (Wright, Narkowicz, Kelly, SIGGRAPH 2022 Advances) is to trace in SCREEN SPACE first,
  where the rendered surface is exact near the ray start, then continue with the hardware ray.
- Bend Studio's screen-space shadows (Aldridge; Days Gone, presented at SIGGRAPH 2023) are the
  reference for a robust, cheap depth-buffer march used to add the small-scale contact a coarser
  shadow technique misses.

Suggestions, in order of preference:

1. **Screen-space first, then the hardware ray** for the shadow ray on animated characters: march
   the depth buffer for the first 1 to 2 units (the engine already has this march,
   `contactOcclusion`), and start the hardware ray where the march ends. The near field is then
   exact against the surface actually drawn, and the hardware ray starts beyond the mismatch.
2. **Size the push from the real mismatch**, not a constant: the tessellation bake can store, per
   surface, how far the smooth displaced surface departs from its flat triangles. Offset = that
   bound + a Waechter-Binder epsilon. Static world surfaces would get almost none.
3. **Give the mask's ray a real normal.** A spare G-buffer channel carrying the interpolated vertex
   normal (octahedral, 16 bits) removes the depth-derivative normal and with it the crease problem
   the outside-hits rule works around.

If the start point is fixed this way, **RT contact shadows become redundant on ray-traced lights**
(see F3), which removes a ray per pixel per light.

Effort: medium for 1 (the march exists), medium for 2, small for 3 if a channel is free.
Risk: 1 inherits screen-space limits (off-screen and hidden occluders) for the first units only.

### F2. Three softness models that do not agree

| Path | Penumbra width near contact |
|---|---|
| Mask blur | `blurScale * gap^curve`; with the tuned curve 0.47, about 8x the physical width for a jaw over a neck (estimate from the formulas: 0.48 vs 0.06 units) |
| Character cone (mode 7) | physical: `blurScale * gap` |
| Contact shadows | a fixed 12 degree cone, unrelated to the light |

That mismatch is why cone-traced character shadows read as "defined lines" next to the softened
world. The curve below 1 is a legitimate art choice (PCSS with a non-linear width), but then every
path should take its width from the same function.

Suggestions:

- One shared GLSL function for the penumbra half-width, used by the mask and by anything cone-traced.
- Decide mode 7's fate with a measurement. It only serves lights the blur pass skips because their
  penumbra is under `r_rtShadowBlurMinPenumbra` pixels, and it traces a physical cone that is by
  definition under that size: 4 rays per pixel for close to a 2x2 box filter. Either make it
  PCSS-style (one blocker ray, then a cone sized by the shared function) or remove it.
  Measured 2026-11-19 (F4, routing readout): in demo1 at 1920x1080 internal it never ran, because
  no light's penumbra was under the 2 px minimum.
  **Removed the same evening.** No cutscene check was needed: the skip rule itself says a light
  loses its mask only beyond about 2900 units at 1920x1080 and 2 px (1440 units at Ultra's 4 px),
  where a character is a few pixels tall. With `r_rtShadowNearStart 2` also taking the contact ray
  off softened lights, a softened light is left with one penumbra model, the mask, and no shared
  function is needed.

Effort: small. Risk: low; it is a consolidation.

### F3. Contact shadows have become a second shadow system

On a shadow-MAPPED light, a short contact ray adds what the map's resolution and bias lose. That is
the role the technique has in shipped games (Control's ray-traced contact shadows complement its
shadow maps; Bend's screen-space shadows complement cascades). On a RAY-TRACED light it mostly
repeats the main ray, and exists only because the main ray starts a unit out. With a 14-unit reach
it is no longer "contact" either: it is a second shadow at 40% strength, with its own softness.

Cost today (measured 2026-11-19, see F4): 0.65 ms of the 2.57 ms interaction pass, 4 to 5% of the
frame. One extra ray per pixel per light on every lit surface, and since 2026-11-19 also on
hard-ray lights. It is the largest single cost this review found that a fix can remove outright.

Suggestions:

- After F1: no contact ray on ray-traced lights; keep it for shadow-mapped lights.
- Until then: a much shorter reach on ray-traced lights (2 units covers what the 1-unit push hides).
- Sampling: the new per-quad turn uses interleaved gradient noise (Jimenez 2014). This project
  already found that IGN's diagonal structure survives a denoiser and moved RTAO to an R2 sequence.
  If grain shows without FSR2, use the same R2 sequence or a spatiotemporal blue-noise mask
  (Wolfe et al., EGSR 2022) rather than more rays.

### F4. Today's fixes cost performance, and nobody has measured it

- `r_rtShadowOutsideHits` makes the mask's ray a non-opaque traversal (every candidate triangle is
  confirmed in the shader) for EVERY pixel of every softened light, although only characters need
  it. The pass used to be the hardware's fastest all-opaque ray.
- The mask's ray pass now also traces the skin scatter band on every surface, because no per-pixel
  skin flag reaches that pass.
- Contact rays now run on hard-ray lights too.

Suggestions:

- **Measure first**: `r_vkGpuTime 2`, scopes `rtblur.ray` and `light.interact`, each of
  `r_rtShadowOutsideHits`, `r_rtContactShadows`, `r_rtShadowCharRays` on and off, in a busy room.
- **A per-pixel "animated character" flag** in the normal G-buffer's alpha (today a 0 / 1 weapon
  mask read through `< 0.5` tests; a third value needs an audit of its readers). With it the mask's
  ray pass uses the slow path and the skin band only on character pixels.
- **Let the hardware do the outside test**: Doom 3's outward side is Vulkan's back face, so the rule
  is a cull flag. Instances disable facing culling today; enabling it for character instances would
  keep the fast opaque traversal.

**Measured 2026-11-19** (`tools/bench/timedemo-ab.sh rtshadow`, short run: variants A B C, 2 rounds
after 2 warm-ups; demo1, Nightmare preset with RTX hybrid, 2560x1440 output at FSR2 75% = 1920x1080
internal; RTX 3080 Ti, power-capped at about 156 W and 740 MHz in every run, so read the shares
rather than the absolute times):

| Scope, ms per frame | A as merged | B outside hits off | C RT contact shadows off |
|---|---|---|---|
| frame | 17.28 | 17.39 | 16.58 |
| mask pipeline (`light.shadow`) | 4.07 | 4.05 | 4.08 |
| of which `rtblur.ray` | 1.68 | 1.61 | 1.64 |
| of which `rtblur.tiles` | 0.60 | 0.60 | 0.59 |
| of which `rtblur.blurH` + `blurV` | 1.80 | 1.84 | 1.84 |
| `light.interact` | 2.57 | 2.65 | 1.92 |
| fps | 54.9 | 54.8 | 57.2 |

Clock-independent change against A (median, p10 to p90 over the paired demo windows):

- **Outside-hits rule**: `rtblur.ray` 6.1% slower with it (1.0 to 12.2%), about 0.10 ms. No change in
  `light.interact` (the per-draw hard ray), and the whole frame is not separable from noise
  (-1.1%, range -3.8 to +0.4). The non-opaque traversal is nearly free here.
- **RT contact shadows**: `light.interact` 25.8% cheaper without them (18.1 to 35.6%), 0.65 ms; the
  frame 4.9% (2.9 to 6.9%), about 0.85 ms, +2.3 fps. With every light ray-traced the screen-space
  march is off, so this is the contact term against no contact term. It is the whole feature, not
  the part added on 2026-11-19 (contact rays on hard-ray draws); that share is not isolated.
**Second run, same day** (variants A D F G, 2 rounds; report in
`~/.local/share/dude/bench/20261119-195538-rtshadow`). G is A with `r_rtShadowDebug 1`: the same GPU
work, so it is an A against A control, and its log holds the routing readout.

- **The control sets the noise floor.** G against A shows `light.interact` -2.3% (0.06 ms) and
  `rtgi.ray` +15.5% (0.22 ms), both with their whole p10 to p90 range on one side of zero, although
  nothing differs on the GPU. `rtblur.ray` drifts -1.6% (-5.6 to +1.5). At 2 rounds the report's
  star is not a significance test: differences of this size are noise. A's own two runs differ by
  0.26 ms at the frame level.
- **Skin band** (F, skin shading off): `rtblur.ray` 7.0% cheaper (3.4 to 11.4%), about 0.12 ms,
  clear of the control. `light.interact` -2.6%, the same size as the control's drift, so nothing
  established there. Frame: no change.
- **Outside-hits rule**, re-read against the control: 6.1% of `rtblur.ray` (1.0 to 12.2%) is only
  marginally clear of it. "About 0.1 ms, perhaps half that" is the honest reading.
- **Character cone** (D, character rays 0): no change in any scope, and the readout says why: mode 7
  drew 0.0 times per frame in the whole demo. The minimum-penumbra skip (2 px) never fired, so
  every ray-traced light had a mask and characters used it. The cone's cost is unmeasured where it
  runs; in demo1 it does not run. See F2.
- **RT contact shadows** (first run): 25.8% of `light.interact` is ten times the floor. Solid.

**Routing in demo1** (per frame, mean over 140 one-second samples in two runs, which agree to 0.5%):

| | mean | range |
|---|---|---|
| Masks made | 10.6 | 2 to 19 |
| Masks reused, or skipped for a small penumbra | 0.0 | |
| World draws on a mask (mode 5) | 284 | 39 to 565 |
| World draws on the hard ray (mode 4: translucent surfaces, the view weapon) | 46 | 4 to 158 |
| World draws with no shadow term (mode 0) | 176 | 4 to 552 |
| Character draws on a mask | 5.5 | 0 to 26 |
| Character draws with no shadow term | 2.0 | 0 to 19 |
| Character draws on the cone (mode 7) | 0.0 | |
| Draws that trace the contact ray (all of the above) | 514 | |

Read from it:

- The mask pipeline's 4.07 ms buys 10.6 masks: about 0.38 ms per light per frame (F6, F5).
- **Every lit draw traces a contact ray, the 178 a frame that have no shadow term included.** Those
  are lights that the map, or the light's own material, marks as casting no shadow (`noShadows`,
  `LightCastsShadows()` false). A third of the contact-ray draws (by count, not by pixels) put a
  contact shadow under a light that was authored to have none. Whether that is wanted is a question
  for F3 and a fidelity decision, not a bug found.
- The hard-ray draws that gained the contact ray on 2026-11-19 are 46 of 514.
- Characters are 1.5% of the lit draws. demo1 cannot size anything character-specific.

What it changes in this review: the character flag (item 2) is worth about 0.2 ms at most, the
outside-hits rule and the skin band together, 1% of the frame, and does not pay for an audit of the
G-buffer alpha's readers. The contact ray (F3) is the cost worth removing, and F1 is what makes it
removable. The largest block is the mask pipeline itself: 4.07 ms, 23% of the frame, of which the
two blur passes are 1.80 ms and the tile pass 0.60 (F6) and the rays 1.68 (F5).

### F5. Trace only where the answer is uncertain (roadmap R4, not built)

With `r_rtAllLights`, the engine stops rendering shadow maps for ray-traced lights. But a STATIC
light's cached cube map costs nothing at rest and already knows where the shadow edges are.

- Story, "Hybrid Ray-Traced Shadows" (GDC 2015): use the shadow map to classify, ray-trace only
  where the map is unreliable, blend by blocker distance.
- AMD FidelityFX Hybrid Shadows: the same idea with tile classification, plus ray-interval
  reduction (the map bounds how far the ray must go).

Suggestion: for static lights keep the cached cube as a classifier. Pixels the map calls fully lit
or fully shadowed with margin skip the ray; only a band around shadow edges, and pixels near moving
casters, trace. Most pixels are far from any shadow edge, so this is the largest ray-count lever
left (estimate; needs a prototype to size). The existing tile maps are the natural place for it.

Risk: disagreement between map and ray at the band's border, handled with a conservative band; and
VRAM for the cubes (the budget work of issues #4 / #30 applies).

**Assessment after the 2026-11-19 measurements: not recommended now.**

- Its ceiling is the ray pass, 1.68 ms of the mask pipeline's 4.07 ms in demo1 at Nightmare. The
  tile and blur passes (2.4 ms) are untouched by it, and the classifier itself costs a filtered map
  lookup per pixel per light.
- It works against item 3. The near ray start exists so the main ray finds occluders under a unit
  in size. A cube map cannot resolve those; it would call such pixels "lit, with margin" and skip
  exactly the rays that find them. Keeping them means keeping the contact ray, 0.65 ms.
- A static light's map knows nothing about characters. Every pixel near a moving caster must trace
  anyway, which needs a per-light screen bound of the dynamic casters.
- It brings the cube maps back for every static light: VRAM, and a render on every light or door
  change (issues #4 / #30).

If the ray count has to come down, the engine already has the lever: `r_rtShadowBlurStagger`
refreshes distant lights every 2nd or 3rd frame and cuts rays, tiles and blur together. It is on at
Ultra and off at Nightmare by choice.

### F6. The blur's cost is bandwidth per light

Measured (2026-09-21): about 0.45 ms per refreshed light, each pass at the floor of one
full-screen-equivalent read, independent of tap count. The cost is the number of passes, not the
filter.

Boksansky, Wimmer and Bittner, "Ray Traced Shadows: Maintaining Real-Time Frame Rates" (Ray Tracing
Gems, 2019, ch. 13) keep per-light visibility in layers and filter them together. Suggestion:
pack up to four lights' visibility (and their half-widths) into one pair of targets and blur them
in the same two passes. Estimate: the per-light pass cost divides by the pack size where lights'
screen rects overlap, which is the busy-room case.

Effort: medium (the tile maps become per-channel). Risk: low; the maths per light is unchanged.

**Assessment after the 2026-11-19 measurements.** The target is real: tiles and blur are 2.4 ms for
10.6 lights a frame. But the effort above is too low. A light's mask is made right before its
interactions draw, so batching needs every softened light's rays traced up front (a restructure of
the light loop), per-channel write masks on shared targets, per-channel tile maps and blur widths,
a channel select in the interaction lookup, and a new answer for the staggered lights' own
persistent masks. That is days of work in a path that is on by default from High up.

So it is sized before it is built. `r_rtShadowDebug 1` now prints, per frame, the masks' screen
rects summed, and what they would sum to if four lights at a time (in the order they come) shared
their passes: the smaller of the four's bounding rect and their sum. The ratio is the share of the
2.4 ms a batched blur could keep at best. If the two numbers are close, the lights do not overlap
enough and F6 is dropped.

**First reading, 2026-11-19** (Nightmare with RTX hybrid, a close-up of Swann, a still view): 11.0
masks a frame, **3.69 screens light by light, 2.05 if four shared their passes**. The lights do
overlap: grouped in fours as they come, the passes would cover 56% of the area they cover today,
so up to 44% of the tile and blur cost, on the order of 1 ms of a 17 ms frame if the cost follows
the area and this scene is like demo1 (2.4 ms there). One still view is not a survey, and a close-up
is the kind case: every light's rect is a third of the screen. F6 stays a real candidate, for
after the release; before building it, take the same reading while playing through a few rooms.

### F7. Skin: a binary shadow under a scattering model

The skin shading (Penner, "Pre-Integrated Skin Shading", 2011) widens the diffuse falloff per
colour channel, but the shadow multiplying it is a single grey value. Penner's model includes the
matching piece: a penumbra scattering term, so a shadow edge on skin bleeds red the same way the
terminator does. Now that the mask provides a continuous penumbra value on characters, that term is
a few shader lines and no new pass: widen the visibility per channel by the existing wrap widths.
A visual upgrade, opt-in, in keeping with the fidelity policy.

**Built 2026-11-19**: `r_skinShadowScatter` (0 = off, the default), docs/skin-shading.md "Penumbra
scattering". Seen in game the same evening at full strength on Swann and kept, as an opt-in.

### F8. Screenshots do not show what the player sees

Screenshots are rendered with the temporal jitter off, and anything that relies on FSR2 to resolve
(the contact shadows' per-frame pattern) looks different in a capture than live. They are also
saved as JPEG quality 75. Both made the 2026-11-19 debugging much slower.

Suggestions: render screenshots through the same temporal path (or accumulate a few frames), and
default to PNG.

**Built 2026-11-19**: `r_screenshotAsSeen` (1, the default). A window-sized screenshot is now the
next frame exactly as displayed: it is no longer flagged as a screenshot frame, so FSR2 and its
jitter stay on, and its history is no longer reset on the way in and out. Larger-than-window and
blended shots keep the old path. Correction to the finding above: JPEG at quality 75 was this
machine's setting, not the default. The default format is TGA, which is lossless, so there is
nothing to change there.

Checked the same evening on a 2560x1440 capture of Swann against one taken through the old path
four hours earlier, same place: the old one shows hard stair-steps on the glasses' wire frame and
the ear's outline, the new one resolved edges. Share of full-contrast one-pixel steps on strong
edges: 12.1% before, 2.8% now.

### F9. No way to look at one shadow term

There is no debug view for the shadow visibility, the contact term, or which mode a light used on a
surface. Finding that "the lines" were the contact shadows took hours for want of one.

Suggestion: one debug cvar that replaces the lit result with a single term (visibility, contact
term, skin band, shadow mode as a colour), plus a once-a-second console line: masks made, lights
skipped by minimum penumbra, and draws per shadow mode. Small, and it pays for itself the first
time.

## 3. Still parked, and why this review does not change it

- **NRD SIGMA in place of the blur** (docs/rtx-sigma-shadows.md, option A): per-light, full-screen
  in this build, several ms dearer in a 12-light room. Nothing here changes that arithmetic.
- **Stochastic area-light sampling** (the abandoned R5): it would give true penumbrae, but needs
  a temporal denoiser per light. The deterministic blur was chosen over it on purpose.
- **A tessellated ray-tracing copy of characters**: tried on 2026-11-19 at level 8; no visible gain
  on the defects, 64x the triangles to refit. F1 addresses the same mismatch from the ray's side.

## 4. Suggested order

| # | Item | Why first | Effort |
|---|---|---|---|
| 1 | F4 measurements, F9 debug view | DONE 2026-11-19: `r_rtShadowDebug`, bench set `rtshadow`; numbers under F4 | small |
| 2 | F4 character flag or hardware culling | DEFERRED: measured at about 0.2 ms, 1% of the frame (outside hits 0.1, skin band 0.12) | small to medium |
| 3 | F1 ray start, then F3 | BUILT 2026-11-19 as an opt-in, `r_rtShadowNearStart` (docs/rtx-shadow-blur.md "Near ray start"): not screen-space first, but the softening ray started 0.05 units out where the surface faces the light, which the outside-hits rule already makes safe; setting 2 drops the contact ray on softened lights. Softened lights only. Looks right in game (2026-11-19); saving not measured (bench variants H, I ready); off by default | medium |
| 4 | F2 one penumbra model, decide mode 7 | DONE 2026-11-19: mode 7 and `r_rtShadowCharRays` removed; the mask is the one model | small |
| 5 | F5 hybrid classification | NOT RECOMMENDED: capped at the ray pass (1.68 ms), works against the near ray start, blind to characters, brings the cube maps back. See F5 | large |
| 6 | F6 batched blur | AFTER THE RELEASE: first reading 3.69 screens against 2.05 batched, so up to 44% of tiles and blur (about 1 ms); a light loop restructure in a default-on path. See F6 | large |
| 7 | F7 skin penumbra scattering, F8 screenshots | DONE 2026-11-19: `r_skinShadowScatter` (opt-in, seen in game and kept), `r_screenshotAsSeen` (default on, checked on a capture) | small |

## Sources

- Fernando, "Percentage-Closer Soft Shadows", SIGGRAPH 2005 Sketches.
- MohammadBagher, Kautz, Holzschuch, Soler, "Screen-Space Percentage-Closer Soft Shadows",
  SIGGRAPH 2010 Posters.
- Waechter, Binder, "A Fast and Robust Method for Avoiding Self-Intersection", Ray Tracing Gems
  (2019), ch. 6.
- Boksansky, Wimmer, Bittner, "Ray Traced Shadows: Maintaining Real-Time Frame Rates", Ray Tracing
  Gems (2019), ch. 13.
- Hanika, "Hacking the Shadow Terminator", Ray Tracing Gems II (2021), ch. 4.
- Chiang, Li, Burley, "Taming the Shadow Terminator", SIGGRAPH 2019 Talks:
  https://media.disneyanimation.com/technology/publications/2019/TamingtheShadowTerminator.pdf
- Deshmukh, Green, "Predictable and Targeted Softening of the Shadow Terminator", SIGGRAPH 2020
  Talks: https://research.dreamworks.com/wp-content/uploads/2020/08/talk_shadow_terminator.pdf
- Story, "Hybrid Ray-Traced Shadows", GDC 2015:
  https://developer.download.nvidia.com/assets/events/GDC15/hybrid_ray_traced_GDC_2015.pdf
- Wright, Narkowicz, Kelly, "Lumen: Real-time Global Illumination in Unreal Engine 5", SIGGRAPH
  2022 Advances in Real-Time Rendering:
  https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-Lumen-Wright%20et%20al.pdf
- Epic Games, "Hardware Ray Tracing in Unreal Engine" (the Nanite fallback mesh note):
  https://dev.epicgames.com/documentation/unreal-engine/hardware-ray-tracing-in-unreal-engine
- Bend Studio, "Inside Bend: Screen Space Shadows":
  https://bendstudio.com/blog/inside-bend-screen-space-shadows
- Penner, "Pre-Integrated Skin Shading", SIGGRAPH 2011 Advances in Real-Time Rendering.
- Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare", SIGGRAPH 2014
  (interleaved gradient noise).
- Wolfe, Morrical, Akenine-Moeller, Ramamoorthi, "Spatiotemporal Blue Noise Masks", EGSR 2022.
- AMD, FidelityFX Hybrid Shadows sample and FidelityFX Denoiser.
