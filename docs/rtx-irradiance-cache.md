# RT irradiance cache - multi-bounce GI at the bounce hit (plan, authored 2026-10-16, NOT started)

**Status:** plan only. Picked 2026-10-16 as the fidelity item from the merian / merian-quake review (see the
memory note `merian-quake-ideas-review`); it is also the hard prerequisite of docs/rtx-experimental-lighting.md.

## Why (fidelity)

The RT GI (docs/rtx-rtgi.md) is ONE bounce: the hemisphere ray's hit is shaded with its direct light (scene lights
+ emissive) and that is all the receiver gets. Anything lit only by second-order light stays black - the side
walls of a corridor whose light faces the floor, the underside of a counter, a ceiling in a room lit from below,
the far end of a dark passage that a bright room spills into. A world-space, camera-adaptive hash cache of
INDIRECT irradiance closes the recursion: the hit's outgoing radiance becomes
`albedo(H) * (E_direct(H) + E_cache(H)) + emissive(H)`, and E_cache(H) was learned from the frames in which H's
cell was itself a primary pixel receiving a bounce. Over a few frames the cache converges to all orders of
diffuse bounce at the cost of one extra lookup. A second fidelity effect: the cached term is an average over
frames and over the cell's pixels, so the bounce signal NRD sees is smoother than the raw 1-spp direct-at-hit
term, and the denoiser can keep more detail at the same history.

References: NVIDIA SHARC (hash-grid radiance cache, fixed-point accumulate + resolve); merian's
`hashed-irradiance-cache.slang` + `hash-grid.slang` (BSD-3: level from camera distance, staggered levels, normal
bins, coherent probing, LRU eviction); the legacy merian-quake `light_cache.glsl` (GPLv3, 81 lines: the same thing
in GLSL with a frame-stamp lock). Nothing is vendored; the recipe is small enough to write here.

## What the paper says about it (read 2026-10-16)

Alber et al. 2025 use exactly this cache: an adaptive hash grid aligned with the guiding grid, "coarser levels are
accessed only upon hash collisions", "a small accumulation factor" for responsiveness, and - in their fast mode -
the cache IS the path tail: "the path length can even be reduced to trace direct light only, and the path tail is
read from the irradiance cache ... biased ... more than 120 FPS ... while achieving a similar error at low sample
counts". It is also the training signal of the Markov chains (docs/rtx-mcpg-guiding.md), so C precedes G. Their
sizing for Quake: target resolution 0.6 degrees from the camera, 1e6 entries, 44 MB for the (larger) guiding state;
our 2^20 x 16 B cache plus the accumulation buffer is in the same range.

## Design

- **Value.** A cell stores `E_ind`: the indirect irradiance arriving at the cell's surface, as the gather measures
  it at a primary point P: `pi * L_out(H)` for a cosine-sampled bounce hit H, where `L_out(H)` is exactly what
  `rtgiTraceBounce` returns today plus the new cached term. The cache is indirect-only - DI stays DI, the combined
  DI+GI buffer keeps its meaning, the apply is untouched.
- **Key.** `hash(int3 cell, level, normalBin)`. Level from the point's distance to the eye: cell width
  `w = 2 * tanAlphaHalf * dist`, quantized in log2 with N steps per octave (merian: `GRID_POWER 2`,
  `GRID_STEPS_PER_UNIT_SIZE`), a half-cell stagger per level (Instant-NGP, so levels do not share vertices), normal
  bin = cube face, the cube rotated 45 degrees on odd levels. Position jitter (stochastic interpolation) is a knob,
  off by default.
- **Storage.** Two buffers, SHARC-style. Slot (16 B): `key`, `stamp` (frame last touched), `irr` (3 x f16),
  `N` (u16). Accumulation entry (16 B): `sumR, sumG, sumB` (fixed point x1024, `atomicAdd` on uints - no float
  atomics extension needed), `count`. Default 2^20 slots = 32 MB; cvar 2^19..2^22. Linear probing over 4 slots
  with the start index aligned to 4 (coalesced per warp); a miss claims an empty slot or evicts the oldest stamp
  via `atomicCompSwap` on the key.
- **Update** (in the gather, no new pass): `restir_di.frag` (combined-GI path, after the bounce) and
  `rtgi_ray.frag` add the pixel's bounce estimate into P's accumulation entry. Checkerboard / res-scale: only traced
  pixels update (fine).
- **Resolve** (one small compute dispatch per frame after the gathers, `rhi::RHI::Dispatch`): for every slot with
  `count > 0`: `irr = mix(irr, sum / count, max(1 / N, alphaMin))`, `N = min(N + 1, Nmax)`, clear the sums. Nmax
  default 32 (merian 128-256 is too laggy for Doom's switched / swinging lights), alphaMin 0.02.
- **Lookup** (in `rtgiTraceBounce`, after the hit is shaded): probe H's cell (level from H's eye distance);
  `Li += albedo * E_cache(H) * r_rtgiCacheScale`. The miss (no cell) adds nothing, so a cold cache is exactly
  today's one bounce.
- **Responsiveness.** (1) the Nmax cap; (2) the R1 gradient: a pixel whose DI gradient fired (restir_di_gradient.frag
  "changed") also resets its cell (`N = 1` through a reset flag in the accumulation entry, honoured by the resolve);
  (3) stamp-based eviction keeps dead cells from lingering. Forward (DIFORWARD) lights are already excluded from
  the bounce (`skipForwardLights`), so the flashlight never enters the cache and never lags in it.
- **Leaks.** Same-facing surfaces on both sides of a thin wall share a cell when the cell is wider than the wall
  (SHARC has the same); the normal bin separates opposite faces only. Mitigation = the cell-angle knob (default
  ~4-8 screen pixels at the hit's distance) and the staggered levels; residual accepted, measured at C4.
- **Cost.** Lookup 1-4 cached 16 B reads per bounce; update 4 atomics per traced pixel; resolve ~1M slots.
  Expected <= 0.2 ms at 1440p.

## cvars (all RT-tier, VK + ray query; default OFF, bit-identical off)

| cvar | default | meaning |
|---|---|---|
| `r_rtgiCache` | 0 | the cache feeds the bounce hit (multi-bounce) |
| `r_rtgiCacheScale` | 1.0 | weight of the cached term at the hit |
| `r_rtgiCacheSlotsLog2` | 20 | slot count (reconfiguring: joins ComputeGfxCfgSignature, see memory `fsr-scale-device-lost`) |
| `r_rtgiCacheHistory` | 32 | Nmax (frames of memory per cell) |
| `r_rtgiCacheCellAngle` | 0.004 | tan(alpha/2): cell width per unit eye distance |
| `r_rtgiCacheDebug` | 0 | 1 cell colour, 2 cached value, 3 age, 4 N |

ImGui: a sub-block of the RT GI group (Debugging tab). The user named the ImGui for this work.

## Stages

- **C0 feasibility (small):** storage buffers with device addresses + uint atomics + `Dispatch` exist (helltime
  accum / deform-once tess use compute); a NEW compute shader needs a CMake re-configure (memory
  `cmake-glob-new-shaders`); confirm `restir_di.frag` has a free uniform slot for the cache addresses
  (`u_particleRadius.zw` is free since the grate box list went).
- **C1 buffers + resolve + debug view:** allocate, clear on level load / size change, the resolve pass, debug view 1
  (cell colouring proves keying + levels); nothing feeds the bounce yet.
- **C2 update + lookup:** the gather writes P's estimate; `rtgiTraceBounce` reads H's cell. A/B
  `r_rtgiCache 0/1`: a corridor side wall / under-counter / ceiling case brightens; `r_vkGpuTime 2` cost.
- **C3 responsiveness:** Nmax, gradient reset, eviction; test a switched light and a swinging light (no trail
  longer than the DI's own), the flashlight (must not enter the cache).
- **C4 validate (user):** the three fidelity cases, thin-wall leak hunt (Mars City corridors by the glass walls),
  cost, NRD detail at the same history; tune angle / history / scale.
- **C5 ship:** preset wiring (on at Ultra Nightmare once validated), docs/rtx-rtgi.md cross-link, memory.

## Non-negotiables

Default off; off = bit-identical; VK + `SupportsRayQuery()` only; primary fullscreen view only; raster / GL3 untouched;
diagnostic cvars trimmed once tuned (memory `trim-debug-cvars`).

## Implementation log (branch `feat/rtx-irradiance-cache`, 2026-10-16)

### C0 feasibility - what was found and what had to be added

- **Storage + device address + atomics in the gather: yes.** `BU_STORAGE` buffers carry `SHADER_DEVICE_ADDRESS`
  (RhiWorld.cpp `RB_RHI_GatherSceneLights` / `s_diLightMapBuf` are the pattern) and glslang (vulkan1.4 target)
  accepts `atomicAdd` / `atomicOr` / `atomicCompSwap` on members of a `buffer_reference` block, so the fragment
  gather reaches the cache through two device addresses in the RenderParams UBO - no storage descriptor is added to
  the graphics pipeline layout (which has none).
- **`BU_STORAGE` is HOST-VISIBLE memory** (VMA `HOST_ACCESS_RANDOM | MAPPED`, `HOST_VISIBLE | HOST_COHERENT` required):
  right for the per-frame CPU-written light buffer, wrong for 32 MB of fragment-shader atomics. Added
  `rhi::BU_STORAGE_DEVICE`: device-local (`VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE`), never mapped, addressable,
  compute-bindable; `UpdateBuffer` / `ReadBuffer` are no-ops on it, contents undefined until a clear dispatch.
- **`rhi::RHI::Dispatch` refuses to record inside a render pass** (the compute lane was pre-scene only: MD5 skin /
  deform / cull), and the DI chain runs INSIDE the open scene pass between its target passes. The NRD denoise and
  FSR2 end the scene pass themselves and the next `Draw` resumes it (`EnsureScenePass`, load variant). Added
  `ComputeArgs::midScene` (same contract) and `ComputeArgs::afterFragmentWrites` (an explicit
  FRAGMENT_SHADER -> COMPUTE_SHADER `VkMemoryBarrier`: a render pass's implicit dependencies order attachments, not
  the gather's storage atomics). Still refused inside an open TARGET pass.
- **Compute shaders were runtime-compiled C++ string literals** (`CreateComputeShader` through shaderc: MD5 skin,
  GPU cull, the RT world test). The resolve needs build-time validation (the game cannot be run from the session),
  so `.comp` joined the build: `compile_spv.py` and `embed_shaders.py` accept the suffix, both CMake globs list
  `shaders/*.comp` (a NEW file still needs a re-configure - memory `cmake-glob-new-shaders`), and
  `rhi::RHI::LoadComputeShader( name )` loads `spv/<name>.comp.spv` through the same VFS -> build tree -> embedded
  lookup as the graphics stages. The prelude (`#version 460` + `GL_EXT_buffer_reference`) is prepended as for every
  stage, so a `.comp` can `#include` the shared helpers.
- **Free RenderParams slots** in the gather family (restir_di.frag + its includes, restir_di_gradient.frag,
  rtgi_ray.frag): `u_particleRadius.zw` (restir_di only - rtgi_ray carries the IGN rotation there), `u_channelMask`,
  `u_skinParms`, `u_contactParms`, `u_alphaTest`, `u_tessParms2`, `u_texGen*`, `u_clipPlane`, `u_shadowProjection*`.
  The cache packing (C1) uses `u_channelMask` / `u_skinParms` / `u_contactParms` identically in every consumer and
  puts the slot-buffer address in `u_particleRadius.zw` (DI gather + gradient, which shares the gather's parms) or
  `u_alphaTest.xy` (rtgi_ray.frag, the debug overlay).
- **The R1 gradient runs AFTER the gather** (rev. 2), so "a pixel whose gradient fired resets its cell" cannot be
  done in the gather. The gradient pass itself writes the reset (bit 31 of the accumulation entry's count, an
  `atomicOr` on the cell of the stratum's winning sample) and the resolve honours it - C3.
- **G0 register-headroom check of restir_di.frag.** No offline register tool is available here (`/usr/bin/rga` is
  ripgrep-all, not Radeon GPU Analyzer; no amdllpc / spirv-stats), so two instruments were set up:
  1. SPIR-V static metrics of the build (`spirv-dis build/shaders/spv/<s>.spv`: instructions, function-scope
     `OpVariable`s), baseline at master 11db1c84, before any cache code:

     | shader | instructions | function variables | bytes |
     |---|---|---|---|
     | restir_di.frag | 4473 | 306 | 75192 |
     | restir_di_boost.frag | 4997 | 360 | 83012 |
     | rtgi_ray.frag | 2582 | 162 | 44284 |
     | restir_di_gradient.frag | 3650 | 249 | 61256 |

     (A proxy only: the driver's register allocation is what matters, and SPIR-V "variables" are pre-SSA.)
  2. **`r_vkPipelineStats <name>`** (VulkanBackend.cpp, VK_KHR_pipeline_executable_properties): pipelines created
     while the filter matches their shader name are built with `CAPTURE_STATISTICS` and every statistic the driver
     reports is printed (NVIDIA names registers / spills / binary size its own way; RADV reports VGPRs / SGPRs /
     spilled VGPRs / code size). **Procedure:** in the console `r_vkPipelineStats restir_di` then `vid_restart`,
     stand in a lit DI room so the gather pipeline is created, read the `r_vkPipelineStats restir_di [...]` block.
     Repeat with the cache on (C2) and compare; G's 5 vMF states + scores (~30 live scalars) need the fragment stage
     to stay clear of the spill line with the cache's lookup/update live. Baseline numbers: to be filled by the user
     at C4 (not measurable from the session).
