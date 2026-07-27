# TODO / roadmap

Forward-looking work for the dhewm-rt enhanced renderer. These are planned
features / improvements — not defects (see [known-bugs.md](known-bugs.md)).

## Quality presets  **[DONE — shipped 2026-07-27]**

One-click quality presets in the Enhancements tab set the enhancement cvars as a group
instead of making the user dial every slider by hand. Scale, lowest to highest:

**Potato · Low · Medium · High · Ultra · Ultra Nightmare**

**As-built** (`enhancementPresets[]` in
[Dhewm3SettingsMenu.cpp](../neo/framework/Dhewm3SettingsMenu.cpp), drawn at the top of
`DrawEnhancementsMenu()`): combo + "Apply Preset" button; a live status line reads back
"Current: <tier>" or "Custom (hand-tuned)" via `DetectEnhancementPreset()` once any slider
is touched. Separate from Video Options' vanilla `com_machineSpec` preset.

Design decisions taken:
- **Anchor: High = shipped defaults** (the GTX-1070-calibrated look), with one deliberate
  deviation: **Blinn-Phong specular** (`r_shading 1`) is on for every tier above Potato, where
  the shipped cvar default is the vanilla LUT (`r_shading 0`). It's effectively free (analytic
  math replacing a texture LUT fetch), so it rides on all enhanced tiers. Two tiers scale down
  for weaker GL 3.3 hardware, two push modern GPUs.
- **Potato = the faithful floor**: every enhancement off *and* vanilla specular → both the
  cheapest tier *and* an exact vanilla frame, so "source-accurate" is always one click away
  (satisfies the "keep faithful reachable" note below). Playable on any GL 3.3 card.
- Presets move the **performance levers** (on/off, resolution, sample counts, budgets) plus
  the **specular look** (shading model + scale + exponent). Other artistic-calibration cvars
  (SSAO radius/intensity/floor, emissive reach/tint, shadow biases) stay at the user's tuned
  globals so all tiers share one look at different cost.

| lever | Potato | Low | Medium | **High** | Ultra | Ultra Nightmare |
|---|---|---|---|---|---|---|
| soft particles | off | on | on | on | on | on |
| smoke dark-blend | off | off | off | off | on | on |
| emissive surfaces | off | off | on | on | on | on |
| SSAO | off | off | on | on | on | on |
| shadow mapping | off (stencil) | off (stencil) | on | on | on | on |
| specular shading | vanilla LUT | Blinn-Phong | Blinn-Phong | Blinn-Phong | Blinn-Phong | Blinn-Phong |
| specular scale | 1.0 | 1.8 | 1.8 | 1.8 | 1.8 | 1.8 |
| specular exponent | (n/a, LUT) | 62 | 62 | 62 | 62 | 62 |
| SSAO res scale | — | — | 0.5 | 0.5 | 0.8 | 1.0 |
| SSAO slices / steps | — | — | 4 / 1 | 6 / 1 | 8 / 2 | 8 / 4 |
| SSAO normal G-buffer | — | — | off | on | on | on |
| shadow 2D / cube res | — | — | 512 / 512 | 1024 / 1200 | 2048 / 2048 | 2048 / 2048 |
| cube PCF taps | — | — | 2 | 6 | 8 | 12 |
| point-light budget | — | — | 16 | 64 | 96 | 128 (all) |
| shadow size-scale | on* | on* | on | on | on | on |
| size-scale pivot radius | 380* | 380* | 380 | 380 | 340 | 300 |
| emissive light cap | — | — | 16 | 24 | 32 | 48 |
| film grain / chroma | off | off | 0.04 / 0.2 | 0.04 / 0.2 | 0.04 / 0.2 | 0.04 / 0.2 |
| reflection scale | 1.0 | 1.0 | 0.7 | 0.7 | 0.7 | 0.7 |

(Potato/Low carry the cheap Medium SSAO/shadow sub-params under the off toggles so a
manual feature flip from those tiers stays affordable. `*` = inert under stencil shadows
but carried for determinism.)

Shadow size-scaling (`r_shadowMapSizeScale`) stays **on at every tier**: it distributes the
per-light resolution budget by light importance (a light at the pivot radius gets the tier's
base cube res, bigger lights get more, smaller ones less). The tiered **base resolution** is
the primary lever; the **pivot radius** (`r_shadowMapSizeScaleRadius`) is a secondary
sharpness lever — *lowering* it makes more of a map's lights count as "large" and get boosted
above base res, so only the top two tiers tighten it (Ultra 340, Ultra Nightmare 300) while
Potato→High keep the Doom-3-typical 380 default.

Original spec (kept for reference) — each preset writes the non-vanilla enhancement cvars
together, covering at minimum:

- **SSAO** — `r_ssao`, `r_ssaoResScale`, `r_ssaoSlices`, `r_ssaoSteps`, `r_ssaoRadius`,
  `r_ssaoNormalBuffer`, `r_ssaoBentNormal`.
  (e.g. Potato = off; Medium ≈ the current half-res 6-slice/1-step defaults; Ultra
  Nightmare = full-res, more slices/steps, normal G-buffer on.)
- **Shadows** — `r_shadowMapping`, `r_shadowMapPointSize`, `r_shadowMapSize`,
  `r_shadowMapPointLimit`, `r_shadowMapFaceCull`, `r_shadowMapCubePcf`.
  (e.g. Potato = low point budget + 512 cubes + 1 PCF tap; Ultra = 2048 cubes, all point
  lights, 8–16 PCF taps. Cube PCF taps trade frame time for edge softness at no VRAM cost,
  so they can soften a low-resolution tier without paying the memory of a higher one.)
- **Extras** — `r_emissiveSurfaces`, `r_useSoftParticles`, `r_gl3ReflectionScale`.

### Design notes / open questions

- A preset is a **starting point, not a lock** — it writes the cvars and leaves them
  user-overridable. Consider showing a "Custom" state once any slider is touched after
  applying a preset.
- Anchor each preset to a rough **GPU-time budget** so the scale means something; calibrate
  with `r_gl3GpuTime` on a reference scene.
- Keep **"faithful" reachable**: one preset (or an explicit toggle) should map to
  source-accurate = enhancements off / most-vanilla.

### Calibration data (2026-07-27, GTX-1070-class GPU, busy scene, 12.24 ms baseline)

Subsystem cost, from an A/B `r_gl3GpuTime` sweep — useful for budgeting the presets:

| subsystem | cost | share |
|---|---|---|
| interactions (per-light lighting) | ~5.12 ms | 42% |
| SSAO (GTAO + normal pass) | ~4.34 ms | 35% |
| base (depth prepass, ambient, shadow-gen, post) | ~2.78 ms | 23% |
| soft particles | ~0.04 ms | ~0% |

Notes: shadow **mapping** is a net win here (~1.3 ms cheaper than the stencil fallback), so
it isn't a cost to cut. SSAO is the most preset-sensitive knob — half-res 6/1 vs full-res
with more samples spans a wide range, so it should drive most of the difference between
tiers.
