# RT world-space light grid - a PERFORMANCE feature (scope + plan, authored 2026-10-16)

**Why this, why now (user, 2026-10-16): "we are suffering performance wise".** Of everything in the RT lighting
project the light grid is the one perf lever (the merian review ranked it "perf, not fidelity"). It is pulled OUT of
the emissive plan's E3 and scoped here as a standalone feature for the CURRENT Path Tracing tier (`dude_rtx 2`); the
emissive mode will later reuse it.

## The target: the DI exhaustive RIS

At the user's 40-light corridor, `dude_rtx 2`, 1440p-class, `r_vkGpuTime 3`: the frame's single biggest cost is
`restir.di.gather` at ~10 ms of a ~17 ms world. Per the C6 timing analysis that cost is dominated by the DI candidate
RIS, which by default is **exhaustive**: `restir_di.frag` enumerates EVERY light in the scene-light buffer once per
pixel (`r_rtReStirDICandidates 0`, `draws = nLights`), calling `diShadeLight` (cull + dir + `dudeLightAtten` + N.L +
colour) for each, weighted-reservoir-picks one, traces one visibility ray. The scene-light buffer holds up to 256
lights (`RT_REFL_MAX_LIGHTS`, `RB_RHI_GatherSceneLights`); the corridor has ~38. So the gather is **O(nLights) per
pixel**, ~38-256x a single light. The same O(nLights) loop runs at every GI bounce hit (`rtgi_bounce.glsl:177`).

The uniform-M alternative (`r_rtReStirDICandidates N>0`) already cuts that to N draws, but uniformly: a rarely-drawn
dominant light arrives as sparse spikes the denoiser clips (reads dimmer than forward) - documented in the cvar help.
The light grid is the principled fix: **per-cell IMPORTANCE-sampled candidates**, so N draws carry the few lights that
actually matter at that point, with far less variance than uniform N and a fraction of exhaustive's cost.

## The idea: ReGIR (world-space reservoir light grid)

A world-space grid of cells around the camera. A once-per-frame GPU pass fills each cell with a small set of light
candidates drawn in proportion to their IMPORTANCE at that cell (flux x geometry term), plus the cell's total
importance (for the consume's pdf). Each shading pixel (DI gather, bounce hit) then RIS-samples over its CELL's
candidate list instead of over all lights - O(gridSlots) not O(nLights), with the candidate set already biased toward
the lights that reach the point. Reference: Boksansky/Wyman ReGIR; merian `scene/nee.slang` `LightGrid` (BSD-3):
camera-centred cascades, slots x orientation bins, flux x geometry importance, per-slot feedback atomics, even-share
floor. We take the structure now and defer bins + feedback to a later stage.

**Unbiasedness (the "off = exhaustive brightness" guarantee).** ReGIR + RIS is unbiased for the full sum over all
lights when the cell is a correct weighted sample: the fill builds the cell by weighted reservoir sampling over the
in-range lights with weight = importance_j, and stores each kept slot's light index and the cell's `sumImp`. The
per-pixel consume draws a stored candidate with its selection pdf `q_j = imp_j / sumImp`, targets it with `pHat_j`,
and weights `w = pHat_j / q_j`; the reservoir pick and the final `W = (wSum/M)/pHat_chosen` then estimate the same sum
the exhaustive path does. The grid adds candidate VARIANCE (a cell is a subset), but importance-sampled, so it is
strictly better than uniform-M and NRD cleans the rest. Brightness must match exhaustive in the A/B (L5); only noise
and cost differ.

## Grid structure (camera-centred cascades)

- **Cascades.** `numCascades` levels (default 4), each an `R^3` cell grid (default R = 32) centred on the camera,
  cell size `cellBase * 2^c` (default `cellBase` 16 u -> cascades reach about +-256 / 512 / 1024 / 2048 u). Sized so
  the four cascades cover a whole Doom area (rarely over 2000 units across, user 2026-10-16) with fine near cells,
  rather than one coarse cascade reaching far beyond any room. A world point picks the smallest cascade whose box
  contains it (max-axis distance test), then quantizes to a cell. The camera centre is SNAPPED to the cascade's cell
  size each frame so cells do not swim as the camera moves (clipmap rule); the finer cells re-centre in smaller steps.
- **Cell index.** `cell = c*R^3 + (z*R + y)*R + x`, `x,y,z in [0,R)`. Total cells `numCascades * R^3` (4*32^3 =
  131072 default). Decodable back to a cell CENTRE in the fill.
- **No orientation bins in stage 1.** merian splits each cell into 7 face-normal bins so a light behind the shading
  surface is not a candidate. The consume's `diShadeLight` already rejects `N.L <= 0` (pHat 0 = a dead draw, not a
  wrong one), so bins are a slot-efficiency optimisation, deferred to L4. Without them a cell's list may hold a few
  lights that face away - tolerable with Doom's small light counts.
- **Memory.** `slots` candidates/cell (default 16), each `{ uint lightIndex; float pdf; }` = 8 B, + one `float sumImp`
  per cell. 131072 cells x (16*8 + 4) = ~18 MB, device-local. R, cascades, slots are the size knobs (ComputeGfxCfg
  signature). Orientation bins (L4) would x7 it, hence deferred.

## Importance metric (fill)

Per cell, per in-range light: `imp = flux * geom`, where
- `flux = luminance(color) * falloffFactor` - the light's emitted power. For a radial light `falloffFactor ~
  radius^2` (bigger reach = more power); parallel (sun) lights use a large constant; projected lights use their
  frustum extent. Cheap, from the row fields already in `RtLight`.
- `geom = 1 / max(d^2, cellSize^2)` from the cell centre to the light, clamped so a light AT the cell does not blow
  up (the cell has extent). In-range cull first (`dudeLightInRange` on the cell centre, widened by the cell's half
  diagonal so a light reaching any corner is considered). Parallel lights skip the distance term.

This is a cheap proxy for each light's contribution to the cell; the feedback stage (L4) replaces it with the MEASURED
contribution the consumers report, which learns occlusion and cookies the proxy cannot see. Stage 1-3 use the proxy -
already enough to kill the exhaustive loop.

## Fill pass (compute, once per frame, after GatherSceneLights, before the DI gather)

One thread per cell (`numCascades*R^3` threads). Decode the cell centre; stream the `nLights` lights; for each
in-range light compute `imp` and keep the `slots` HIGHEST-importance ones in a DETERMINISTIC streaming top-N (no rng).
Write the kept light indices + their `imp` and the sum of the kept importances (`sumStored`). Deterministic matters:
the grid is then STABLE frame-to-frame for a static camera (no flicker), and when a cell has `<= slots` in-range lights
it keeps them ALL = the exact in-range set (lossless, exhaustive-equivalent at a fixed `slots` cost); only a cell with
MORE than `slots` drops its dimmest. The per-pixel CONSUME (L2) does the importance-proportional draw among the kept
lights (`q = imp/sumStored`), which gives the per-pixel variation the denoiser averages. A cell with no in-range light
writes count 0 and the consumer falls back (L2). Cost scales with `cells * nLights` on the GPU, independent of pixel
count and bounded by the in-range cull; the per-pixel saving is `nLights -> slots`. (merian re-samples per frame with
temporal reuse; deterministic top-N suits Doom's small light counts and is flicker-free. L4's feedback adds learning.)

Reuses the cache's proven plumbing: `rhi::BU_STORAGE_DEVICE` device-local buffers, device address via push constant +
`RtLights`/grid buffer_reference (like `rtgi_cache_resolve.comp`), `rhi::RHI::Dispatch` with `midScene`. A NEW `.comp`
needs a CMake re-configure (memory `cmake-glob-new-shaders`).

## cvars (RT tier, VK + ray query; default OFF = today's exhaustive RIS, bit-identical)

| cvar | default | meaning |
|---|---|---|
| `r_rtLightGrid` | 0 | consume per-cell candidates instead of the exhaustive/uniform RIS (DI + bounce) |
| `r_rtLightGridSlots` | 16 | candidates stored per cell (reconfiguring: ComputeGfxCfgSignature) |
| `r_rtLightGridRes` | 32 | cells per cascade axis R (reconfiguring) |
| `r_rtLightGridCascades` | 3 | number of camera-centred cascades (reconfiguring) |
| `r_rtLightGridCellBase` | 48 | cascade-0 cell size in units |
| `r_rtLightGridDebug` | 0 | 1 cascade tint, 2 candidate count per cell, 3 dominant stored light, 4 cell importance |

ImGui: a "Light Grid" sub-block of the RT GI / ReSTIR group, Debugging tab (user-named ImGui in scope for this work).

## Stages (each ships behind `r_rtLightGrid`, off = bit-identical)

- **L0 feasibility.** The grid buffer alloc (device-local, sizes in ComputeGfxCfgSignature, dropped in
  `RB_RHI_ResetWorldTargets`); the light-buffer address reaches a new `.comp` via push constant + `RtLights`; CMake
  re-configure picks up the new shader (the `.spv` timestamp proves the GLSL compile). The cvars. Nothing consumes.
- **L1 fill + debug.** Cell<->world mapping (shared helper, fragment + compute), the fill compute (reservoir over
  importance), the grid buffer layout, clear on size change / level load. `r_rtLightGridDebug` 1-4. Still nothing
  consumes -> bit-identical. Acceptance (user): debug views show sensible cells (lights' neighbourhoods populated,
  counts fall off with distance, cascades nest), fill cost via `r_vkGpuTime 2` (expect < 0.5 ms).
- **L2 DI consume - THE PERF WIN.** `restir_di.frag`: when `r_rtLightGrid`, replace the exhaustive/uniform draws with
  the pixel's cell candidate list; unbiased RIS (`pHat/q`); the SAME one visibility ray. Fallback when a cell is empty
  or the point is beyond the last cascade: fall back to the existing path (exhaustive or uniform-M) so nothing goes
  dark. Acceptance (user): brightness matches `r_rtLightGrid 0` (exhaustive) at the same spot; `restir.di.gather` drops
  (the headline number); noise acceptable after NRD; A/B at 38 lights and in a denser room.
- **L3 bounce consume.** `rtgi_bounce.glsl` light loop -> the hit cell's candidate list (smaller win, but the bounce is
  O(nLights) too). Same fallback.
- **L4 orientation bins + feedback learning.** 7 face-normal bins per cell; per-slot contribution atomics the
  consumers report -> next frame's importance (learns occlusion + cookies), even-share floor for exploration. Quality
  + a further slot-efficiency win. Only if L2/L3 show the static proxy wants it.
- **L5 validate + tune (user).** Brightness match vs exhaustive; `r_rtLightGrid 0/1` gather + fill ms; cell-size /
  slots / cascades sweep; moving lights (no lag beyond the fill's one frame); many-light room (where exhaustive hurts
  most); fireflies.
- **L6 wiring.** Unlike the fidelity features this is a PERF win, so once proven it likely becomes a tier (or RT-wide)
  default rather than opt-in - decided after L5 with the user. Until then `r_rtLightGrid` default 0.

Order: L0 -> L1 -> L2 (stop here for the first real perf A/B) -> L3 -> L4 -> L5 -> L6.

## Hard rules (from memory; repeated for the doc)

Default off, off = bit-identical; VK + ray query only; primary view only; GL3 untouched. One branch per change
(`feat/rt-light-grid`), commit as Inkub0, never push. Reconfiguring (buffer-size) cvars join `ComputeGfxCfgSignature`
or the device is lost mid-frame. Every file-static RhiWorld GPU handle dropped (zeroed) in `RB_RHI_ResetWorldTargets`.
Never launch the game or view frames - write the exact cvars / spot / `r_vkGpuTime` lines and ask the user. Trim
diagnostic cvars once tuned.

## References

- merian `include/merian-shaders/scene/nee.slang` `LightGrid` (BSD-3): the cascade + slots + orientation + feedback
  reference; `light-cdf.slang` for the flux CDF.
- Boksansky, Wyman et al., "Rendering Many Lights with Grid-Based Reservoirs" (ReGIR), Ray Tracing Gems II ch. 23.
- Our own `rt_hash_grid.glsl` / `rtgi_cache_resolve.comp` for the device-buffer + compute-fill plumbing pattern;
  `RB_RHI_GatherSceneLights` for the light buffer; `rt_light_eval.glsl` `RtLight` / `dudeLightInRange` /
  `dudeLightAtten` for the importance metric; `restir_di.frag` draws loop for the consume site.
