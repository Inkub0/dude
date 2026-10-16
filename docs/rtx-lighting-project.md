# RT lighting project - kickoff (authored 2026-10-16; START HERE in a fresh session)

One umbrella for three staged plans that build on each other. Read this file, then the sub-plan of the stage you
are on.

**Status 2026-10-16 (evening):** C0-C4 of the irradiance cache are done (merged to local master as
`feat/rtx-irradiance-cache`), and **C6 (the cache as the PATH TAIL) is IMPLEMENTED** on branch
`feat/rtx-irradiance-cache-tail` off master (3 commits, built, not merged, not pushed - the user tests first):
shared hash-grid helper (`rt_hash_grid.glsl`, G0's grid), cache buffers + resolve compute + debug views, the update
at P / lookup at H in both gathers, the history cap / DI-gradient reset / stamp eviction, and now the tail mode -
a converged + fresh cell stores the surface's outgoing radiance and SERVES the bounce hit (cheaper gather, smoother
NRD input), behind `r_rtgiCacheTail` (default 1, DI-apply only); `r_rtgiCacheTail 0` is the C2 indirect-only A/B.
`r_rtgiCache` default off = bit-identical. Next = **C6 user validation** (the test procedure at the end of the C6
section of docs/rtx-irradiance-cache.md), then C5 preset wiring, then G1. The G0 register-headroom instrument
(`r_vkPipelineStats <shader>`) shipped with C0. G and E are still PLANNED only.

| # | Plan | What it delivers | Doc |
|---|---|---|---|
| C | Irradiance cache | Multi-bounce GI: the RTGI bounce hit reads a hashed world-space cache of indirect irradiance; smoother bounce for NRD; prerequisite of G and E | docs/rtx-irradiance-cache.md |
| G | MCPG path guiding | The bounce ray is guided by Markov-chain vMF mixtures in hash grids (Alber et al. 2025), unbiased via stochastic MIS; applies to BOTH tiers | docs/rtx-mcpg-guiding.md |
| E | Experimental lighting | Emissive surfaces light the scene (emissive triangle table + light grid + NEE), map-placed lights dropped, sky kept; "Experimental Lighting" ImGui group | docs/rtx-experimental-lighting.md |

**Order:** C -> G1-G4 -> E1 -> E2 -> E3 -> E4 -> E5 -> E6, with G5 after E2. Every stage ships something on its own
behind a default-off cvar; the current `dude_rtx 2` tier gains from C and G before any E work starts.

## Why this project (user, 2026-10-16)

1. Fidelity first: of the ideas found in the merian / merian-quake review (memory `merian-quake-ideas-review`), the
   cache is the one that changes what you see - second-order light that is black today. The light grid is perf,
   not fidelity, so it is NOT picked on its own; the E-mode makes it mandatory (E3).
2. The user's "radical" idea: path tracing ignores the game's lights, only emissive surfaces emit, real-world
   falloff, skies keep the sun, gated under "Experimental Lighting" in the ImGui. Assessed as feasible with a
   content caveat (Doom 3 is placed-light authored; fixture-less areas go dark until emitters are authored) - built
   as a MODE over foundations that help the normal tier.
3. User directive: every improvement taken from the MCPG paper is done for BOTH the current ReSTIR tier and the
   new mode. Both tiers share `rtgiTraceBounce` (rtgi_bounce.glsl), which is where guiding and the cache land.

## Decisions already taken (do not re-ask)

- **D1 lights that survive the E switch:** first-person weapon lights (flashlight, muzzle), parallel lights (suns),
  lights that APPEARED after level load (projectiles, explosions, fx; classified by the lightDef creation serial
  the DI gradient tracks - no game-DLL / ABI change). Map-placed idLights and DUDE's fill lights are dropped.
  Sub-toggle `r_xlKeepGameplayLights` for the A/B.
- **D2 emissive intensity:** a table + the in-game editor EXACTLY like PBR: `emissive/emissive_materials.cfg`
  (shipped, classifier-bootstrapped) + `emissive/emissive_overrides.cfg` (dude folder, later wins), `@cat` rows,
  `*` = inherit, a third tab in the existing "PBR Material Editor" window (Dhewm3SettingsMenu.cpp ~6470; model =
  Material.cpp:140-360 `pbrTable`).
- **D3 kept lights stay as they are:** authored radius / falloff / cookies / cull sphere untouched. No falloff switch.
- **D4 sky: OPEN, "it has to look good".** Prototype (a) sky surfaces in the TLAS under their own instance mask
  returning skybox radiance, vs (b) sun + flat sky-coloured ambient; both behind a debug toggle; the user judges
  on the Mars-surface stretch.
- **ImGui:** the user NAMED the ImGui for this work, so Debugging-tab groups / the material editor in
  Dhewm3SettingsMenu.cpp are in scope (the hard rule against touching it still holds for the GAME menu).
- Direct light stays explicit sampling in both tiers (RIS over the light buffer today; NEE over the emissive table
  in E), because the paper itself ranks ReSTIR above guiding wherever many explicit lights exist.

## Repo state at kickoff

- Local master 38afaa27 (2026-10-16), ~194 commits ahead of origin, NOT pushed (never push; the user publishes).
  Last change: ReSTIR DI grate visibility without the per-view box list (every ray takes the grate-aware walk;
  measured free). `build/` is the single build dir; `run.sh` launches `build/dude`.
- The four plan docs are untracked files in docs/ until committed.
- Today's `dude_rtx 2` budget at the user's settings (1440p-class, FSR2, 40-46 lights, r_vkGpuTime 3): frame
  13-16 ms; restir.di.gather 3.6-4.6, restir.di.denoise 1.7-2.3, volumetrics ~1.9, fsr2 ~1.8-2.3, reflections
  ~1.4. Any addition is judged against this with the same A/B (cvar 0/1, averaged samples, ignore <0.05 ms drift).

## First task: C0 + C1 (branch `feat/rtx-irradiance-cache`)

1. **C0 feasibility.** Confirm: storage buffer + device address + uint atomics in the gather (the scene-light
   buffer and the geo table are the pattern, RhiWorld.cpp `RB_RHI_GatherSceneLights` / `GetRtGeoTableAddress`);
   `rhi::RHI::Dispatch` (RHI.h:550) for the resolve compute; `u_particleRadius.zw` is FREE in `restir_di.frag`
   since the grate box list went (carry the cache + accumulation addresses there); a new compute shader needs a
   CMake re-configure (memory `cmake-glob-new-shaders`; shaders compile via neo/shaders/compile_spv.py into
   build/shaders/spv at build time - the .spv timestamps prove a successful GLSL compile).
2. **C1.** The hash-grid helper (`rt_hash_grid.glsl`: level from eye distance with `tanAlphaHalf`, half-cell stagger,
   normal bins with the 45-degree rotation on odd levels, pcg hash of (cell, level, bin), 4-slot coalesced probing,
   collision key, stamp eviction) - written once, reused by G. Cache slot + accumulation buffers (2^20 default),
   clear on level load / size change, the resolve pass, `r_rtgiCacheDebug 1` cell colouring. Nothing feeds the
   bounce yet; `r_rtgiCache 0` = bit-identical.
3. Then C2 (update from the gather at P, lookup in `rtgiTraceBounce` at H), C3 (Nmax, R1-gradient reset,
   eviction), C4 user validation, C5 preset wiring. Details and acceptance tests: docs/rtx-irradiance-cache.md.

Anchors: `rtgiTraceBounce` rtgi_bounce.glsl:53 (hit shading, light loop, emissive, dominant shadow ray; the lookup
goes after the emissive add); callers `restir_di.frag` (combined-GI path) and `rtgi_ray.frag`; `RtLight` +
`dudeLightAtten` rt_light_eval.glsl:18; DI pass `RB_RHI_ReStirDIPass` and the RT GI pass in RhiWorld.cpp; RT GI
cvars RenderSystem_init.cpp:513-524; ImGui RT GI / ReSTIR groups Dhewm3SettingsMenu.cpp ~4782.

## References (clone again with --depth 1; the session scratchpad copies are gone)

- Paper: Alber, Hanika, Dachsbacher 2025, PACMCGIT 8(1):15, https://doi.org/10.1145/3728296 ; author's PDF
  https://www.lalber.org/wp-content/uploads/2025/04/alber2025MCPG.pdf (159 MB - extract with `pdftotext`, the
  Read tool caps at 100 MB). Key numbers: 0.6 deg target resolution, 1e6 states / 44 MB, N_mc 5 (3 with firefly
  filter + denoiser), p_bsdf 0.15, p_adaptive 0.7, ~100 frames warm-up; Sec. 5 dynamic heuristics; Sec. 7 the
  many-light limitation.
- https://github.com/LDAP/merian-quake (legacy, GLSL): `res/shader/render_mcpg/{mcpg.comp, mc.glsl,
  light_cache.glsl, grid.h}` (GPLv3) = the guiding + cache reference; `render_restir/*` (BSD-3) = textbook
  ReSTIR; `raytrace.glsl` = alpha-tested traversal, `pull the ray origin back along the incoming direction`.
- https://github.com/LDAP/merian (current, Slang, BSD-3): `include/merian-shaders/utils/hash-grid.slang`
  (the hash grid recipe), `light-cache/hashed-irradiance-cache.slang` (SHARC-style cache with m2),
  `scene/nee.slang` LightGrid (E3: cascades, 32 slots x 7 orientations, feedback atomics),
  `merian-graph/nodes/guiding/mcpg/mcpg-guiding.slang`, `render_restir_di/*.slang` (pairwise-MIS reuse, reference
  only), `nodes/accumulate/calculate_percentiles.slang` (percentile firefly clamp).
- https://github.com/LDAP/merian-plugin-quake (GPLv2): `res/shader/quake-material.slang` `ldr_to_hdr` + fullbright
  hue gains (D2's intensity curve), `denoisers/svgf.json` (their graph).

## Working rules for this project (from memory; the session loads them, repeated here for the doc)

Never launch the game or view frames - ask the user for screenshots and `r_vkGpuTime 2/3` readouts. One branch
per change, named after it; commit as Inkub0; never push. Everything opt-in behind a default-off cvar, off =
bit-identical; flag fidelity impact; trim diagnostic cvars once tuned. Reconfiguring cvars (buffer sizes) join
`ComputeGfxCfgSignature` or the VK device is lost mid-frame. On any device loss check the LACT profile / clocks
first (Xid 79 is hardware), then `r_vkCrashLogging 1`. GL3 strict Doom 3 math first; the RT tier is VK + ray
query only, primary view only.
