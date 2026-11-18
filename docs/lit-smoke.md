# Environment-aware smoke & steam (darkness fade, lit smoke, density)

**One switch (2026-11-18):** `r_smokeDarkBlend` is the master switch of all three parts, shown as **Environment-Aware
Smoke & Steam** in the Graphics tab (Particles & Smoke) - the only smoke setting there. 0 = the stock smoke, every part
off; 1 = the darkness fade plus lit smoke and density at their own levels. The tuning knobs (Darkness Floor, Full-opacity
Light Level, Lit Smoke, Reference Light, Smoke Density) are in **Debugging > Raster > Smoke & Steam**; each part can be
dialled out there on its own (Floor 100% = no fade, Lit Smoke 0, Density 0). The cvar keeps its old name so configs and
the presets (on at Ultra and up) carry over.

**Defaults (2026-11-18, the user's in-game tuning):** `r_smokeDarkBlend 1` (still set per preset: on at Ultra and
Nightmare / Path Tracing, off below), `r_smokeDarkBlendFloor 0.55`, `r_smokeDarkBlendKnee 0.07`, `r_smokeLit 0.4`,
`r_smokeLitRef 0.12`, `r_smokeDensity 0.6`, `r_useSoftParticles 1`. Lit smoke and density are not preset-owned: they act
wherever Soft Particles are on (Low and up). Fidelity: smoke no longer has its stock look by default; 0 restores each.

Doom 3 draws smoke, steam and dust as self-lit sprites: the same grey puff in a black corridor and under a red alarm
light. Two opt-in settings make smoke respond to the light around it. Both live in the soft-particle stage
(`RB_RHI_RenderSoftParticleStage`, `RhiBackend.cpp`, and `softparticle.frag`) and share its limits.

## Which particles count as smoke

- Alpha-blended particles (`blend blend`): always (dust, fog; fire and sparks are additive).
- Additive particles (`blend add`, e.g. `textures/particles/smokepuff` steam): only when the material name reads as smoke
  (`RB_RHI_ParticleLooksLikeSmoke`: smoke, steam, dust, fog, mist, vapor, haze, smog, cloud, fume, exhaust), so flames,
  sparks and glares keep their own look. A fire system's own smoke sub-stage counts, its flame does not.
- Only soft-particle sprites reach the stage: `orientation view` particles over 2 units across (Model_prt.cpp
  `SetSofteningRadii`). Both settings turn Soft Particles (and depth capture) on.

## Smoke blends into darkness (`r_smokeDarkBlend`, 2026-07)

The lit scene is captured once per view before the first smoke draws (particles sort back to front, so opaque geometry,
sky and emissive fills are already down). The puff's opacity follows the luminance of the scene behind it:
`r_smokeDarkBlendFloor` on black, full at `r_smokeDarkBlendKnee`. Alpha smoke fades its alpha, additive smoke its RGB.

## Lit smoke (`r_smokeLit`, 2026-11-18)

The smoke takes the **hue** and **level** of the light around it: `colour *= mix( 1, hue * clamp( level / ref, 0, 2 ),
r_smokeLit )`, with hue and level by the light's brightest channel (luminance would blow a saturated light up: pure
blue would be 14x in blue). `r_smokeLitRef` is the level at which the smoke keeps its authored brightness.

- **The light.** Path Tracing (ReSTIR DI applied this view): the DI buffer, direct light plus one bounce **without
  surface colour** (`RB_RHI_SmokeLightSource`, the same buffer the ambient floor reads), times a mid-grey 0.5 so its
  level compares with a lit background. Other tiers and GL3: the background capture the darkness blend already takes,
  half desaturated, because it carries the wall's own colour. The Hybrid tier's RTGI buffer is not used: it holds the
  bounce without the direct light, which is drawn forward there.
- **Softness.** Five taps spread over the puff's radius on screen (4..64 px, from the projection's focal length), so the
  smoke takes the room's light instead of stencilling the wall's detail behind it.
- **Not used:** the RT irradiance cache at the particle's world position - its cells sit on surfaces, so a lookup in
  mid-air mostly misses.
- Parameters (soft-particle pass only): `localParam0.w` strength, `localParam1.z` source (0 background, 1 DI YCoCg,
  2 DI linear), `screenCorrection` = ( 1 / view size, DI scale, reference ), `windowCoord.x` focal length in pixels,
  unit 3 the DI buffer.
- Works with or without the darkness blend; with both, the blend still sets the opacity and lit smoke the colour.
  Fidelity: on by default since 2026-11-18 (0.4); 0 = the stock fixed look.

## Smoke density (`r_smokeDensity`, 2026-11-18)

Additive smoke (`blend add`, most of Doom 3's steam) only ever adds light, so the scene behind a puff stays sharp and
just brightens - more so with lit smoke in bright light (the user's report that prompted this). With a density the
backend draws classified **additive** smoke premultiplied (`ONE, ONE_MINUS_SRC_ALPHA`):

- **density** = the puff's own brightest channel (texture x vertex colour x soft fade, before lit smoke or the darkness
  blend change its colour, then times the darkness blend's opacity) x gain (`2 * r_smokeDensity`), capped at 0.95. It is
  the alpha: that share of the scene behind is hidden.
- **Forward scatter:** `density * 0.7` of the scene behind comes back **blurred** (centre + 8-tap ring over half the
  puff's screen radius, from the pre-smoke capture); the remaining 0.3 is absorbed. Detail behind thick smoke softens
  and dims, the puff's own glow (its RGB) stays.
- **Alpha-blended smoke** (`barrelpoof`: the dark smoke of `barrelfire`, burning imps / mancubi, the archvile flame
  wall, cyberdemon / cacodemon mouth fire) already hides what is behind it, but what shows through stays sharp. It
  keeps its own `SRC_ALPHA` blend; a share `b = (1 - a) * saturate( a * gain )` of the see-through part is replaced by
  the blurred scene: `out = a * rgb + b * blur + (1 - a - b) * dst` - none at thin edges, most in the dense core,
  brightness unchanged.
- **Not covered:** `blacksmokepuff` (`blend gl_zero, gl_one_minus_src_color`, a darkening filter; the lost soul
  explosion, `flametrap`) - the soft-particle stage only takes `ONE` and `SRC_ALPHA` blends.
- `windowCoord.y` = the gain (0 = off), `windowCoord.x` = the focal length in pixels (now always set for the taps).
- Limit: the blur reads the pre-smoke capture, so each of several stacked puffs blurs the unsmoked scene and replaces
  part of the puffs drawn before it; thick stacks read a little thinner than they would.
- Fidelity: on by default since 2026-11-18 (0.6); 0 = the stock steam, a pure glow.
