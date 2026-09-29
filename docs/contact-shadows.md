# Screen-space contact shadows

Opt-in short-range shadowing that grounds the small geometry Doom 3's shadow maps are too coarse
to resolve — dropped pickups, thin props, weapon detail, a monster's feet. A per-light screen-space
depth march inside the interaction pass darkens a lit pixel when a nearer occluder sits between it
and the light. GL3 + Vulkan (the RHI enhanced path); off by default.

**Cvar:** `r_contactShadows` (0). **Tiers:** intended for Medium → Nightmare. **Not** Ultra
Nightmare — there `r_rtAllLights` already traces a real hard ray per light, which is strictly better,
so leave contact shadows off at UN (enabling both just does redundant work; the raster march never
runs on the RT-served lights anyway, which use the `interaction_rt` variant).

## Why it lives in the interaction pass

The effect is inherently **per light**: it shadows one light's contribution by marching toward *that*
light. The forward interaction pass processes one light at a time with that light's direction already
in hand (`u_localLightOrigin`), so the march folds into the existing shadow term with no extra pass
and no all-lights buffer (unlike the RT reflection/GI passes, which are fullscreen and had to gather
lights into a device buffer). The base `interaction.frag` is the right home: at Medium → Nightmare
both backends use it (the RT `interaction_rt` variant is only selected for the UN RT-light modes).

The march visibility multiplies into the same `shadowVis` the shadow map produces, *before*
`lightScale = N·L · shadowVis`, so it stacks with shadow mapping / stencil and rides through to the
skin-SSS term for free. It is skipped on the wet-blood film draw (a decal glint, not a receiver).

## How the march works (`interaction.frag` `contactShadowTerm`)

Conventions mirror `ssr.frag`, which is proven on both backends:

1. Take the fragment and the light to **view space** through `u_modelViewMatrix` (the surface's
   model→view; `var_ModelPos` and `u_localLightOrigin` are both in model-local space, so one matrix
   maps both consistently). View space is rigid, so distances stay in world units.
2. March from the fragment toward the light in `r_contactShadowSteps` steps over
   `r_contactShadowLength` world units (clamped to the distance to the light). The start point is
   offset half-to-one-and-a-half steps off the surface (a per-pixel hash dither, `shadowHash`, that
   both breaks the step banding into grain and biases the first sample off the surface).
3. Project each step with `u_projectionMatrix`, convert NDC→uv, re-orient the row for Vulkan's
   top-down depth buffer (`u_windowCoord.z` = view-Y sign, the `deviceRow` trick), and scale into the
   depth texture's texel space (`u_depthTexRecip.xy = viewSize / uploadSize`, so GL's power-of-two
   depth copy is addressed correctly; on VK the capture is native so the ratio is ~1).
4. Sample the captured scene depth (`_currentDepth`, unit 16), linearise it with the view's
   `u_depthParms` pair (`DUDE_DEPTH_CONSTS`, correct under cinematic near-plane cramming), and compare:
   a surface **nearer the camera than the ray** (both view-Z negative, so `sceneZ > rayZ`) that sits
   **within `r_contactShadowThickness`** world units is an occluder. Darken by
   `r_contactShadowStrength`, faded by march distance (a nearer occluder darkens more), and stop.

`_currentDepth` is a completed, shader-readable copy captured at the top of `RB_RHI_DrawWorld`
(before the light loop; SSAO already consumes it that frame), *not* the live depth-test attachment —
so there is no feedback with the depth buffer bound for the interactions' depth-EQUAL test.

## Cvars

| cvar | default | meaning |
|---|---|---|
| `r_contactShadows` | 0 | master toggle |
| `r_contactShadowStrength` | 0.8 | darkening on a found occluder (0..1) |
| `r_contactShadowLength` | 12 | march length toward the light, world units |
| `r_contactShadowThickness` | 8 | occluder thickness band, world units (a sample nearer than the ray by more than this is background, not an occluder) |
| `r_contactShadowSteps` | 8 | march steps per lit pixel (2..32; runs per light, so keep low) |

All are `CVAR_ARCHIVE`. None is preset-wired — this is a look change, so it stays a manual toggle
until a visual pass says otherwise (like `r_specOcclusion` / `r_ssrIntensity`).

## Backend wiring

- **Unit 16** carries `_currentDepth`. Units 0–14 are the interaction material/shadow/decal samplers
  and 15 is the tessellation height map (VK `.tese`), so 16 is the first free binding. GL3 raw-binds
  it loop-global in `RB_RHI_DrawWorld` (nothing in the loop touches unit 16); VK carries it per draw as
  `DrawArgs::contactDepth` and writes it into set-1 binding 16 (`VK_TEX_BINDINGS` 16→17). White when
  unbound → raw depth 1 → no occluder → fully lit (a safe inert fallback).
- **UBO:** a new `u_contactParms` vec4 = (steps, length, thickness, strength), appended last in
  `RenderParams`/`renderparms.glsl`. `RB_RHI_DrawInteraction` fills it — plus `modelViewMatrix`,
  `projectionMatrix`, `depthParms`, `depthTexRecip` and `windowCoord.z` — only on non-ambient draws
  when `r_contactShadows` is on and the depth capture is ready; everything stays zero (and the shader
  early-outs) otherwise, so every existing draw is byte-identical with the feature off.

## Costs & caveats

- Cost scales with **lights × lit pixels** (the march runs per interaction). Keep `r_contactShadowSteps`
  low (8 is the default); a light-dense scene multiplies it. Measure with `r_gl3GpuTime` / `r_vkGpuTime`.
- **Self-shadow acne** is the classic failure. The dithered start offset + a small per-step epsilon
  guard against it; if grazing surfaces still show acne, the first fixes are a shorter
  `r_contactShadowLength`, fewer steps, or (a small follow-up) a dedicated slope-scaled bias cvar.
- GL3 addresses fragment texture unit 16, i.e. it needs `GL_MAX_TEXTURE_IMAGE_UNITS ≥ 17`. Every
  target GL 3.3 desktop GPU (the GTX-1070-class reference exposes 32) satisfies this; only bare-minimum
  16-unit iGPUs would not, and this is a Medium+ opt-in they would not enable.
- Fidelity: purely additive darkening, but a genuine look change vs. stock — hence opt-in, gated by eye,
  not preset-wired.

## Verification protocol

Build side (done): clean build, `SPIR-V … 0 failures`, `spirv-dis interaction.frag.spv` confirms
`u_contactDepth` at DescriptorSet 1 / Binding 16. `r_contactShadows 0` leaves every draw byte-identical.

In-engine (user gate): with a Medium/High/Nightmare preset, `r_contactShadows 1` and look under dropped
items, small props, weapon detail and feet for the added contact grounding; A/B against `0`. Watch for
self-shadow acne on grazing floors/walls (tune thickness/length/steps), for over-darkening in creases
(lower strength), and check `r_vkValidation 1` is clean on VK and the GPU-time delta is acceptable. Only
after that gate should it be considered for the Medium→Nightmare preset columns.
