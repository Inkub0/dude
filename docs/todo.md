# TODO / roadmap

Forward-looking work for the dhewm-rt enhanced renderer. These are planned
features / improvements — not defects (see [known-bugs.md](known-bugs.md)).

## Quality presets

Add one-click quality presets to the Enhancements UI that set the enhancement cvars as a
group, instead of making the user dial every slider by hand. Proposed scale, lowest to
highest:

**Potato · Low · Medium · High · Ultra · Ultra Nightmare**

Each preset writes the non-vanilla enhancement cvars together. At minimum it should cover:

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
