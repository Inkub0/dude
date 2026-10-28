# MCPG path guiding for the bounce ray - both tiers (plan, authored 2026-10-16, NOT started)

**Source:** Alber, Hanika, Dachsbacher 2025, "Real-Time Markov Chain Path Guiding for Global Illumination and
Single Scattering", PACMCGIT 8(1) article 15, https://doi.org/10.1145/3728296 (author's version read in full
2026-10-16); reference code = merian-quake `res/shader/render_mcpg/{mcpg.comp, mc.glsl, light_cache.glsl}` (GPLv3,
compatible with dude) and merian's Slang `mcpg-guiding.slang` (BSD-3). **User decision 2026-10-16:** every
improvement taken from the paper applies to BOTH the current tier (ReSTIR DI + one-bounce RTGI, `dude_rtx 2`) and
the experimental emissive lighting mode (docs/rtx-experimental-lighting.md).

## Where it lands, and why that covers both tiers

The one place both tiers trace a hemisphere ray is `rtgiTraceBounce` (rtgi_bounce.glsl), called from the DI gather
(`restir_di.frag`, with `r_rtgi` on: direct + indirect in one buffer) and from `rtgi_ray.frag`. Guiding
replaces the bounce's cosine direction there, so the current tier gets it with no change to its direct term. The
direct term stays explicit light sampling (RIS over the light buffer today, NEE over the emissive table in the
E-mode): the paper itself ranks ReSTIR above guiding wherever many explicit lights exist (Sec. 7, Fig. 8 right,
"ReSTIR remains orthogonal to our method"). In the E-mode guided rays ALSO discover emitters the way the paper's
Quake does (no light list), which is combined with NEE by MIS shares exactly as merian's current renderer does
(`shares: guiding 0.75, bsdf 0.25, nee 0.1`).

## What the paper gives us (and what it does not)

- **Unbiased guided bounce at 1 spp** via stochastic MIS over the N_mc accessed states + the BSDF share: the
  estimator is `L(hit) f_r cos / (p_bsdf p_cos + (1 - p_bsdf) sum_i w_i vMF_i(w) / sum_i w_i)` (eq. 7 + Alg. 2). Cosine
  sampling is the `p_bsdf` fallback, so a cold grid degrades to exactly today's bounce.
- **The irradiance cache is the training signal** (Sec. 4 "Irradiance Caching"): `L(hit_next)` for the Markov
  update comes from the cache, so docs/rtx-irradiance-cache.md (C) is the prerequisite, as the paper states. The
  paper also runs the cache as the PATH TAIL in its fast mode ("direct light only, tail from the irradiance cache ...
  biased ... >120 FPS ... similar error at low sample counts") - that is our C plan verbatim.
- **Dynamic-content heuristics** (Sec. 5) worth copying as-is: (1) discard a state whose stored luminance `w` is
  far above the fresh estimate while the sampled direction aligns with its lobe (`mu . w > r`) and its timestamp is
  not this frame - the light is gone; (2) look up the FIRST sample of a frame at the point's previous world position
  (moving geometry keeps its chains); (3) store the emitter velocity and advance `y` by `w (T - T_s) v` on load
  (eq. 10) - matters in the E-mode (moving emitters), near-inert in the current tier (bright lit surfaces do not
  move).
- **Sizing** (Sec. 6, Fig. 9): target resolution 0.6 degrees from the camera, adaptive buffer 1e6 states (44 B each,
  44 MB), static grid >= 10 MB with a coarse constant cell (25 units in Quake); N_mc = 5 (3 is acceptable with a
  firefly filter + denoiser); convergence ~100 frames; `p_adaptive` 0.7, `p_bsdf` 0.15, prior mean cosine 0 with
  `N_p = 1 / dist^2` (fast lock-on to small distant emitters), `N_max` 1024, `alpha_min` 0.01.
- **Limitations we inherit:** proximity bias at shadow boundaries (variance, not bias; NRD absorbs it as SVGF did
  for them); many small lights overwhelm a 5-state mixture (darkening + outliers) - which is why the E-mode keeps
  NEE over the emissive table + the light grid; a warm-up after load / teleport (the static grid and the previous-
  position lookup shorten it).
- **Not taken:** single scattering guiding (our volumetrics are froxels), the unsynchronised Markov writes
  (we keep one writer per cell per frame, as the cache does), MCPG guiding of DI itself.

## Design (dhewm-rt)

- **State** (40 B, matches the paper's Table 1): `w_tgt` (vec3, luminance-weighted emitter position), `sum_w`,
  `w_cos`, `mv` (f16x3), `T`, `N` (u16), `hash` (u16). Two buffers: adaptive grid (2^20 states default, cvar) and
  static grid (2^18, constant width `r_rtgiGuideStaticCell` 25 u). The hash grid helper (levels from camera
  distance, half-cell stagger, normal bins, collision hash) is the one C1 builds - one implementation.
- **Resample** (Alg. 1): N_mc loads (`p_adaptive` from the adaptive grid at an exponentially coarser level, else the
  static grid), collision -> `w = 0`, weighted reservoir pick; all N_mc vMF parameters kept for the SMIS pdf.
- **Sample** (Alg. 2): `p_bsdf` or empty -> cosine; else vMF from `(mu, kappa)` reconstructed from the sufficient
  statistics with the `1 / dist^2` prior. The bounce then traces exactly as today. The returned value is scaled by
  `(cos / pi) / p(w)` so the consume sites stay untouched (cosine case = 1.0).
- **Estimate + update** (Alg. 3): `f_mc = lum(L(hit) f_r cos / sum p)` with `L(hit)` = direct at the hit + emissive +
  `albedo(hit) * E_cache(hit)` (C). MCMC accept with `f_mc / (sum_mc / N_mc)`; MLE mix with `alpha = max(1 / N,
  alpha_min)`; write to a freshly hashed position in BOTH grids (spreads information); fast recovery; velocity.
  One update per traced pixel at its primary point - the only path vertex we have.
- **Flashlight / forward lights:** the bounce skips `DUDE_LIGHT_DIFORWARD` lights, so the chains learn a world
  without them; consistent with the cache.
- **Fireflies:** `(cos / pi) / p(w)` can spike where the mixture is wrong; clamp the ratio (`r_rtgiGuideMaxWeight`)
  and rely on the percentile firefly clamp (merian review item 3) + NRD anti-firefly. The paper pairs N_mc = 3
  with exactly that.
- **Gather radius:** guided rays aim at far bright surfaces; `r_rtgiRadius` (720) caps them, so a state pointing
  beyond the radius decays through fast recovery. Keep the radius in the current tier; the E-mode raises it.
- **Cost estimate:** paper, 7900 XTX at 1080p: PT 13.1 ms -> MCPG 15.3 ms for 2 spp x 2 bounces (+~0.5 ms per
  guided ray per full-res frame). Ours: one ray per traced pixel at 0.75 res + checkerboard -> +0.3-0.6 ms.
  Register pressure in `restir_di.frag` (already the heaviest shader) is the real risk; N_mc = 3 is the fallback.

## cvars (RT tier, VK + ray query; default OFF = today's cosine bounce)

| cvar | default | meaning |
|---|---|---|
| `r_rtgiGuide` | 0 | guide the bounce direction with the Markov-chain mixture |
| `r_rtgiGuideSamples` | 5 | N_mc states resampled per bounce (3..8) |
| `r_rtgiGuideBsdfProb` | 0.15 | share of cosine (discovery) rays |
| `r_rtgiGuideAdaptiveProb` | 0.7 | share of loads from the adaptive grid |
| `r_rtgiGuideSlotsLog2` | 20 | adaptive states (reconfiguring: ComputeGfxCfgSignature) |
| `r_rtgiGuideStaticCell` | 25 | static grid cell width |
| `r_rtgiGuideMaxWeight` | 8 | clamp on `(cos/pi)/p` |
| `r_rtgiGuideDebug` | 0 | 1 learned direction, 2 mean cosine, 3 w, 4 N, 5 grid cells (the paper's Fig. 3 views) |

ImGui: sub-block of the RT GI group, Debugging tab (user-named ImGui).

## Stages

- **G0** (with C1): the shared hash-grid helper; confirm `restir_di.frag` register headroom for 5 vMFs + scores.
- **G1 resample + sample + SMIS pdf** in `rtgiTraceBounce` behind `r_rtgiGuide`; no learning yet (cold grid =
  cosine, bit-identical off).
- **G2 update** at the primary point (MCMC + MLE + both grids) with the cache as `L(hit)`; debug views.
- **G3 dynamics:** fast recovery, previous-position first lookup, velocity.
- **G4 validate (user):** A/B at equal NRD history in a sun-through-window room, a lava room, a corridor lit by one
  far bright wall; warm-up time after load; cost via `r_vkGpuTime 2`; firefly check.
- **G5 E-mode:** guided rays as a second direct-light technique, MIS'd with NEE over the emissive table (shares
  as in merian); radius raise; moving-emitter velocity test (plasma).

Order overall: C -> G1-G4 (current tier benefits immediately) -> E1-E6 with G5.

## Re-audit of the live merian code (2026-10-16, after the cache shipped)

Corrections to the paper-derived numbers above, from the CURRENT merian Slang (not the legacy GLSL / the paper):
- **N_max = 255**, not 1024 - the current `mc.slang` caps N in 8 bits (`ML_MAX_N`); keep our 8-bit N (matches the cache).
- **Directional prior = a constant `dir_guide_prior = 0.2`** shrinking the resultant length `u + (1-u)·p/(n²+p)`
  (uncertainty ∝ 1/n²), rather than the paper's `N_p = 1/dist²`. Start from 0.2; the 1/dist² lock-on is an option.
- **`weight_exponent = 1.0`**: the per-state score is `sum_w^weight_exponent` (1.0 = linear; a knob).
- **κ from the inverse-Langevin** `κ = r(3 − 2.6r + 0.7r²) / (u(1.1 − 0.1u))`, `r = 1 − u`, `u = 1 − |mean|` - the
  exact closed form to use in Sample.
- `p_guiding` is ONE share (BSDF vs guided); `p_adaptive` is separately the adaptive-vs-static GRID-load split. Our
  `r_rtgiGuideBsdfProb` = 1 − p_guiding, `r_rtgiGuideAdaptiveProb` = p_adaptive.
- merian's shared hash grid matches C1 (GRID_POWER 2, PROBE 4, coarser-level fallback, timestamp eviction,
  previous-camera re-seed on a level crossing); its only extra is Morton `LOCALITY_BITS` for cache locality (a tiny
  perf option we can add to rt_hash_grid.glsl later).

**SSMM fork (evaluate before G1).** merian also ships **screen-space Markov mixture guiding** (`guiding/ssmm`, BSD-3):
the same mixture-of-vMF guiding but keyed in SCREEN space instead of the world hash grid - no ~32M-state adaptive
buffer, much less VRAM, at the cost of no off-screen memory. Worth a cost/quality A/B against the full hash-grid MCPG
(G) before committing to the VRAM-heavy adaptive grid; if SSMM is close, it is the cheaper default and G stays opt-in.

## Non-negotiables

Default off, off = bit-identical; VK + ray query only; primary view only; raster / GL3 untouched; diagnostics
trimmed once tuned.
