# RT irradiance cache - multi-bounce GI at the bounce hit (plan authored 2026-10-16; C0-C3 implemented the same day)

**Status:** C0-C3 IMPLEMENTED on branch `feat/rtx-irradiance-cache` (built, not merged, not pushed; see the
implementation log + the C4 checklist at the end). Picked 2026-10-16 as the fidelity item from the merian /
merian-quake review (see the memory note `merian-quake-ideas-review`); it is also the hard prerequisite of
docs/rtx-experimental-lighting.md. Next: C4 user validation, then C5 preset wiring.

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
| `r_rtgiCacheHistory` | 32 | Nmax (frames of memory per cell) when `r_rtgiCacheHistoryMs` is 0 |
| `r_rtgiCacheHistoryMs` | 500 | the history in milliseconds through the low-passed frame rate (added at C4: Doom's light tables are authored in time) |
| `r_rtgiCacheReset` | 1 | the DI gradient restarts a cell whose light switched (diagnostic A/B, added at C4) |
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

### C1 - helper, buffers, resolve, debug views (what exists)

- `neo/shaders/rt_hash_grid.glsl` - the shared grid (G0 reuses it): `hgLevel` (`floor( log2( 2 tan dist ) * 2 )`,
  two levels per octave, width recomputed from the level), `hgCell` (half-cell stagger on odd levels),
  `hgNormalBin` (cube face; 45-degree Z-then-X rotation on odd levels), `hgHash` / `hgMakeKey` (PCG; 32-bit
  collision key never 0; probe start aligned to the 4-slot span), `hgFind` / `hgClaim` over a flat `HgWords` view
  (`stride` uints per slot, stamp at `stampOfs`) - claim = existing | first empty via `atomicCompSwap` | oldest
  foreign slot older than `evictAge` (stamp 0 = claimed this frame, never evicted) | -1 drop.
- `neo/shaders/rtgi_cache.glsl` - slot `{ key, stamp, irr.rg f16x2, irr.b f16 | N u16 }`, accumulation
  `{ r, g, b x1024, count | reset bit 31 }`, `RtgiCacheParms` + `rtgiCacheParmsFromUniforms` (u_channelMask /
  u_skinParms / u_contactParms + the per-consumer slot address), `rtgiCacheLookup`, `rtgiCacheUpdate` (claim,
  stamp, evict-zeroing + reset request, peak clamp 64, 4 atomics), `rtgiCacheRequestReset`.
- `neo/shaders/rtgi_cache_resolve.comp` - 256 threads/group, bindings 0/1 = slots/accum, push constants
  `{ slotCount, mode, nMax, frame, alphaMin }`; mode 1 = clear. `alphaMin` 0.02 is a constant.
- `RhiWorld.cpp` - `rtgiCacheState_t rhiRtgiCache` + `RB_RHI_RtgiCacheBegin` (allocate `BU_STORAGE_DEVICE` x2,
  resize on `r_rtgiCacheSlotsLog2`, clear on first use / `renderWorld->mapName` change / re-enable, loads
  `rtgi_cache_resolve`), `RB_RHI_RtgiCachePack`, `RB_RHI_RtgiCacheResolve` (`midScene` + `afterFragmentWrites`,
  GPU scope `rtgi.cache`), `RB_RHI_RtgiCacheDebugOverlay` (called after the DI overlay in RhiBackend.cpp). Hooked
  into `RB_RHI_RtgiPass` (Begin before the uniforms, resolve right after the ray pass) and `RB_RHI_ReStirDIPass`
  (Begin with the combined-GI block, resolve after the gradient chain, before the denoise).
- Debug overlay `rtgi_cache_debug.frag`: full-res G-buffer reconstruction (the gather's math at scale 1), then
  `hgMakeKey` + `hgFind` per pixel. View 1 is meaningful from C1 on (keying needs no content; found cells are the
  bright ones once C2 feeds them).

**C1 visual check (user; the session cannot run the game).** `r_rtgi 1` (or dude_rtx 2), `r_rtgiCache 1`,
`r_rtgiCacheDebug 1`: every surface must be tiled with same-tinted cells that (a) keep a roughly constant SCREEN
size as you walk toward / away from a wall (they are re-cut at each distance level: a sudden re-tint across a whole
wall when crossing a level boundary is expected, not a bug), (b) split along the normal-bin faces (a floor and a
wall never share a tint across their edge), (c) are all DIM at C1 (nothing claims cells yet). `r_vkGpuTime 2` must
list `rtgi.cache` at ~0.05-0.1 ms (1M slots, almost all early-outs) and the first frame after enabling / a level
load adds the clear once. With `r_rtgiCache 0` nothing changes and no `rtgi.cache` scope appears.

### C2 - update at P, lookup at H (what exists)

- `rtgi_bounce.glsl`: `RtgiBounceParms.cacheLookup / cache`; after the hit's direct light + emissive + dominant-light
  shadow ray, `Li += min( albedo, albedoClamp ) * rtgiCacheLookup( H, hitN, eye ) * scale`. `rtgiTraceBounce` gained an
  `out vec3 LiCache` = Li minus the DIFORWARD lights' direct contribution (tracked per light, the dominant-shadow
  subtraction applied to it only when the dominant light is not fast) - so the flashlight / muzzle / projectile
  bounce is never learned even in the standalone RTGI pass, which bounces them for display.
- `restir_di.frag`: builds `bp.cache` from `u_particleRadius.zw` + the shared slots; after the trace, `#if !DI_BOOST`,
  `rtgiCacheUpdate( P, N, eye, hit == 1 ? LiCache : 0 )` - a miss teaches 0 (open directions belong to the cell's
  cosine-lobe average), the boost variant does not teach twice, checkerboard teaches only the traced pixels.
  `rtgi_ray.frag`: the same with the slot address in `u_alphaTest.xy`.
- RhiWorld.cpp `RB_RHI_RtgiCacheBegin`: flags = update | lookup whenever the cache is on. `r_rtgiCache 0` =
  bit-identical: the cache branch is uniform and Li's arithmetic is untouched (LiCache is a separate accumulator).
- SPIR-V after C2 (same instruments as the C0 baseline; `spirv-opt -O` counts added since they approximate what
  the driver sees better than the raw emission):

  | shader | instructions | function variables | atomics | bytes | `-O` instructions pre -> post |
  |---|---|---|---|---|---|
  | restir_di.frag | 4473 -> 5800 | 306 -> 401 | 7 | 75192 -> 96868 | 3078 -> 3764 (+22%) |
  | restir_di_boost.frag | 4997 -> 5866 | 360 -> 419 | 0 (lookup only) | 83012 -> 97364 | - |
  | rtgi_ray.frag | 2582 -> 3911 | 162 -> 256 | 7 | 44284 -> 66000 | 1379 -> 2082 |
  | restir_di_gradient.frag | unchanged until C3 | | | | |

  Two key derivations per traced pixel (the update at P, the lookup at H) are most of the growth; the live state
  added across the bounce call is the parms struct (uniform-derived, rematerialisable) + `LiCache` (3 scalars) +
  the claim loop's temporaries, which die before the DI shade resumes. The real register verdict is the
  `r_vkPipelineStats restir_di` readout, cache off vs on (C4).

**C2 A/B procedure (user).** Same spot, same settings, `r_rtgi 1` (dude_rtx 2 has it): toggle `r_rtgiCache 0 / 1`
and look at (1) a corridor side wall whose light fixture points at the floor, (2) the underside of a counter /
console lip, (3) a ceiling in a room lit from below, (4) the far end of a dark passage a bright room spills into.
With the cache on each must brighten over ~1-2 s (the first frames show the one bounce; `r_rtgiCacheDebug 4` goes
white as cells converge) and then hold steady - no pulsing, no creeping brighter indefinitely (the albedo clamp 0.9
+ scale 1 bound the series). `r_rtgiCacheDebug 2` shows what the cells hold: smooth, low-frequency, no single-cell
fireflies (the per-sample clamp 64 and the resolve mean take care of those); `1` shows every lit cell bright (claimed)
after a few frames of looking at it. Cost: `r_vkGpuTime 2` - `restir.di.gather` (or `rtgi.ray`) with the cache
0/1, averaged, plus `rtgi.cache`; expected <= 0.2 ms total at 1440p. If `restir.di.gather` grows by more than
~0.3 ms the key derivation is worth hoisting (one `hgMakeKey` for P reused for the update + the gradient).

### C3 - responsiveness (what exists)

- **Nmax** = `r_rtgiCacheHistory` (32): the resolve's `N = min( N + 1, Nmax )`, blend `alpha = max( 1 / N, 0.02 )`.
  A converged cell takes 2 % of every frame, so even without a reset a change is 63 % absorbed in ~50 frames and
  a cell under Nmax follows faster.
- **R1-gradient reset:** `restir_di_gradient.frag` (runs AFTER the gather, rev. 2 - which is why the gather cannot
  do this itself) computes `Lprev` / `Lnow` for its stratum's winning sample; when
  `|Lprev - Lnow| > 0.35 * max( Lprev, Lnow )` (and the brighter one is not black) it calls
  `rtgiCacheRequestReset( Sp.Pw, Sp.Nw, eye )`: `atomicOr` of bit 31 into the cell's accumulation count. The
  resolve then restarts the cell (`N = 0`): next frame's samples REPLACE the stale mean. 0.35 is a constant in
  `RB_RHI_RtgiCacheBegin` (`resetThresh`), chosen above the DI dead zone (`r_rtReStirDIGradientDeadzone` 0.25) so
  Doom's 5-15 % authored flicker tables never reset a cell every frame; a switch is a 100 % change of the light's
  own value. Flag 4 of `u_skinParms.z` gates it (always set with the cache on; the gradient runs only when the DI
  gradient chain runs, i.e. `r_rtReStirDIGradient` + a previous frame).
- **Stamp eviction:** `hgClaim` with `evictAge = clamp( 4 * Nmax, 64, 4096 )` frames: a span full of foreign cells
  gives up its oldest one (last update >= evictAge frames ago) to the newcomer; the evictor zeroes the payload and
  requests a reset so none of the old cell's value survives. A slot claimed this frame but not yet stamped (stamp
  0) is never evicted. Dead cells (a room left behind) thus stop pinning slots without live cells churning.
- **Fast lights never enter the cache** (plan: the flashlight must not lag): the DI bounce skips DIFORWARD lights
  (`skipForwardLights`, LiCache == Li) and the standalone RTGI pass bounces them for display but learns `LiCache`
  without them (C2).
- SPIR-V: `restir_di_gradient.frag` 3650 -> 4408 instructions, 249 -> 298 function variables, 1 atomic, 61256 ->
  73748 bytes.

**C3 procedures (user).**
1. *Switched light:* a room with a wall switch (Mars City lobby / Alphalabs offices). Cache on, `r_rtgiCacheDebug 0`:
   switch the light off - the indirect fill on the OPPOSITE walls / ceiling must fade within the DI's own fade
   (the NRD history of `r_rtReStirDIHistory`); it must not trail for the 32-frame Nmax (that would mean the
   gradient reset is not reaching the cells - check with `r_rtgiCacheDebug 4`: cells in the room should drop to
   black (N = 0) the frame after the switch and climb back). Switch it on again: same test in reverse.
2. *Swinging light:* under a swinging lamp (Mars City underground / Alphalabs corridors). The indirect glow on
   the walls must follow the lamp with no longer trail than the direct light's; `r_rtgiCacheDebug 4` should show
   the cells along the swing path resetting (dark flicker) while the rest stay converged (white).
3. *Flashlight:* in a dark room sweep the flashlight across a wall near a lit surface, cache on: the far-side
   indirect light must NOT change with the beam (`r_rtgiCacheDebug 2` stays unchanged under the beam). Then the
   same with `r_rtgi` on but DI off (standalone RTGI path, dude_rtx 1): the beam's own one-bounce shows (it
   always did) but `r_rtgiCacheDebug 2` must still not change - the cache never learned it.
4. *Eviction:* walk through three rooms and back; `r_rtgiCacheDebug 3` in the first room should show mostly
   dark (fresh) cells again within a few frames of looking at them, no magenta / black holes where a cell could
   not be claimed; `r_rtgiCacheDebug 1` must show every lit cell bright (claimed) - dim cells on a visible, lit
   surface after a second mean a full probe span (raise `r_rtgiCacheSlotsLog2`).

### Post-C3 fixes (self-review, same branch)

- The frame whose clear is pending packs flags 0 (no lookup / update / reset): the gathers run before the resolve
  point that zeroes the buffers, so that frame would otherwise read undefined memory (one frame of random bounce
  light into the NRD history). Learning starts the frame after a first use / level change / resize / re-enable.
- A hit on an unshadeable geometry row (`rtgiTraceBounce` 2: mover / prop without attributes) teaches nothing
  (unknown, not dark); a miss still teaches 0.
- **`vid_restart` with the cache on lost the device (user, C4 item 14, 2026-10-16).** The backend restart reissues
  every handle from 1 and frees every buffer, but `rhiRtgiCache` kept its buffer handles, device addresses and
  resolve-program handle: the next gather wrote its atomics through the stale address (GPU page fault =
  `VK_ERROR_DEVICE_LOST`), and the stale shader handle silently recorded no clear. Fixed by dropping the cache state
  in `RB_RHI_ResetWorldTargets` (the `RB_RHI_Shutdown` hook every other RhiWorld buffer static uses - scene lights,
  DI light table, perforated boxes), without `DestroyBuffer`, with the clear re-armed. RULE for the G buffers too:
  every file-static GPU handle in RhiWorld must be zeroed there. (Vulkan has no `reloadShaders` pipeline rebuild;
  the statistics readout needs `vid_restart`, which is now safe with the cache on.)

### C4 first readings (user, 2026-10-16) and what changed because of them

- **Cost** (`r_vkGpuTime 3`, same spot, 1440p-class, dude_rtx 2): frame 14.85 -> 15.42 ms, `restir.di.gather`
  4.43 -> 4.92 ms (+0.49, above the +0.1-0.3 estimate), `rtgi.cache` 0.046 ms, gradient and denoise unchanged.
  Verdict pending the register readout (`r_vkPipelineStats restir_di` + `vid_restart`, which lost the device until
  the restart fix above): a register tier jump would explain a uniform ~10 % gather slowdown; otherwise it is the
  dependent probe loads in an occupancy-starved shader.
- **History view 4 while moving forward: "swimming black radial areas".** Expected by construction and now softened:
  the cell width follows the eye distance (two levels per doubling), so moving forward sweeps the level thresholds
  across the world in rings and every cell on a ring is re-cut at the finer level. Before, a re-cut cell restarted
  from one frame's mean (N = 1 = black in view 4); now a freshly claimed cell is SEEDED from its one-coarser parent
  with 4 frames of trust (`RTGI_CACHE_SEED_N`), so the learned field is continuous while the fine cell's own samples
  take over, and a lookup at a bounce hit falls back to the coarser level when the hit's own cell is absent. The
  rings are still visible in view 4 (young cells) and in view 5 as GREEN; the lit image must not show them.
- **Static view never fully white.** Two defects found and fixed while reasoning about it: (1) the 45-degree bin
  rotation on odd levels put every axis-aligned normal exactly on a rotated face edge, so bump-mapped texels of one
  cell fell 50/50 on either side at random (two half-cells per cell, a salted cell view) - now 22.5 degrees about Z
  then X, ~20 degrees of margin for every axis normal; (2) the gradient reset had no darkness floor - now
  `|dL| > 0.35 * ( max( Lprev, Lnow ) + 0.05 )`. The remaining suspect is the gradient reset firing on 1-spp /
  flicker noise, which the next screenshot decides: **`r_rtgiCacheDebug 5`** (activity: red = reset this frame,
  green = created / seeded, grey = steady, black = absent) in a static scene must settle to all grey; if red keeps
  flashing, `r_rtgiCacheReset 0` is the A/B (and the 0.35 threshold / 0.05 bias move up). View 4 now floors present
  cells at 15 % grey so a young cell and a hole are told apart.
- **View 5, second reading (user):** a static scene IS all grey - the reset does not fire on noise standing still;
  the sparse black specks are silhouette slivers (the overlay's own jittered reconstruction straddling an edge) and
  a few cells whose every bounce ray hits an unattributed prop (teach nothing) - neither touches the lit image.
  But LOOKING AROUND flashed most of the view red (resets) with green only at the leading screen edge (fresh cells,
  as expected), and `r_rtgiCacheReset 0` removed almost all of it (A/B confirmed). Cause: the gradient's two
  comparison directions. When LAST frame's light wins on a pixel that moved > 2 px on screen (any turn or walk),
  the gradient's mover path compares the stored luminance at the previous point with a re-shade at the current
  point, a pixel apart - on bump-mapped, cookie-lit surfaces that differs by 35 % routinely. The DI confidence
  absorbs it (dead zone 0.25, power 8, A-trous); the cache cannot. Fixed by taking only the EXACT comparisons: a
  static pixel (re-shaded at the stored point) or the direction where THIS frame's pick wins (both frames at the
  same point). Consequence: a light switching ON resets its cells even while moving; a switch-OFF while moving is
  left to the history blend (alpha >= 1/32: ~0.5 s) and resets the frame the player stops. If C4 shows that lag,
  the next step is driving the reset from the FILTERED confidence in `restir_di_gradient_conf.frag` (per gather
  pixel, after the A-trous + rise-only smoothing) instead of the raw stratum - fuller coverage, and the filter
  dilutes sparse false positives.
- **Third reading (user): looking around is now almost all grey; floor grates always show activity; a STATIC camera
  watching a walking NPC shows resets + young cells over all the walls; while walking, the leading edge of the
  swimming rings is red.** Three causes, three changes:
  1. **Movers polluted the cache.** The gather taught a cell for every traced pixel - the walking marine and the
     view weapon included. A moving mesh sweeps new world positions every frame and mints on the order of a thousand
     garbage cells a frame: a 1M-slot cache fills in tens of seconds, after which every newly cut cell must EVICT an
     old one (the red ring edges - an eviction painted the same red as a gradient reset). Now the view weapon
     (`nt.a < 0.5`) and movers never teach (they still look up): `rtgiCacheIsMover` compares where the per-object
     velocity says the pixel was last frame with where the camera's own motion alone would have put it (last frame's
     view matrix + this view's projection) - a static surface agrees within the jitter, a mover disagrees by many
     pixels (threshold 3). The velocity rides unit 10 in both gathers (flag 8 when bound).
  2. **The reset fired on moving SHADOWS.** The NPC's shadow sweeping a wall flips the one-ray visibility of a region
     every frame; the indirect term a moving shadow displaces is second-order, so resetting for it only injected
     noise. The gradient tells the cases apart: a light that switched off LEFT the buffer (its row is absent in the
     other frame), an occluded light is still present with zero visibility. The reset now fires when the light is
     absent on either side (switch / entered / left DI), or when it is LIT on both sides and changed by the threshold
     (dimming, brightening, a swinging cookie); a zero side with the light present is a shadow - no reset.
  3. **Eviction is its own request** (`RTGI_CACHE_EVICT_BIT`): the resolve drops that frame's mixed sums and keeps
     the payload the claimer left (now seeded from the coarser parent, like a fresh cell), flagged FRESH (green).
     Red in view 5 is a gradient reset and nothing else.
  Floor grates: perforated surfaces alternate between the grate plane and the floor beneath under the jitter, so
  their strata compare two different surfaces that both pass the DI's same-surface test; expected residual,
  localised to grates, and the cache value on a grate matters little (a thin surface mostly holes).
- **Fourth reading (user): movers black (a green flash when an NPC starts / stops - the velocity lags a frame; those
  few cells are evicted later, left as is); whole surfaces solid red or green, and "most cells flicker".** The
  coloured patches are the surfaces under Doom's authored FLICKER / STROBE lights (the fluorescent fixture housings,
  the lit door sign, the wall screen's strip): a light that changes by > 35 % every frame requests a reset every
  frame, and a cell restarted every frame never holds more than one frame's mean - the flicker. Green where such a
  cell was sampled only every other frame (checkerboard): a request with no samples leaves N = 0, and the next
  sampled frame reads as "created". Fix: a per-cell RESET COOLDOWN (3 bits in the slot, 7 resolves). A request on a
  cooled-down cell restarts it as before; a request inside the cooldown only re-arms it, so a flickering light
  restarts its cells ONCE and they then blend toward the light's average (alpha >= 1 / Nmax: the responsiveness
  knob is `r_rtgiCacheHistory`, 32 = ~0.5 s), which is what a cache of a periodic light should hold; a switch is a
  one-shot request and still restarts at once. View 5 shows held / cooling cells YELLOW.
- **Time, not frames (user question, agreed):** Doom's light tables and swings are authored in game time, so the
  cooldown's boundary between "follow" (a switch, a slow blink) and "average" (a flicker) is a frequency and must be
  a duration - 7 frames were 117 ms at 60 Hz but 49 ms at 144 Hz. Now `RTGI_CACHE_COOL_MS` = 250 ms converted to
  frames each view through the 0.2 s low-passed frame rate every seconds-based NRD window already uses
  (`RB_RHI_SmoothedFrameMs`, factored out of `RB_RHI_NrdHistoryFrames`), a 5-bit cooldown in the slot (<= 31 frames:
  ~215 ms at 144 Hz, the cap), the history field down to 8 bits (<= 255 frames). The history itself likewise:
  **`r_rtgiCacheHistoryMs`** (default 500 ms, 0 = `r_rtgiCacheHistory` in frames) sets Nmax per view, and the
  eviction age (4 x Nmax) follows. The gradient's own comparison stays per frame by construction; its 35 % threshold
  is self-correcting across frame rates (an interpolated table changes less per frame at 144 Hz, fewer requests, the
  cell blends either way).

## C4 - user validation checklist (the session cannot run the game; every item is yours)

Setup for all items: Vulkan, RT hardware, `dude_rtx 2` (ReSTIR DI lighting the scene + combined GI) at your usual
1440p-class settings; `r_rtgiCache 1`; defaults otherwise (`r_rtgiCacheScale 1`, `r_rtgiCacheHistory 32`,
`r_rtgiCacheCellAngle 0.004`, `r_rtgiCacheSlotsLog2 20`). Toggle the cache with `r_rtgiCache 0 / 1` for every
A/B - nothing else changes (0 is bit-identical to master). Console `r_vkGpuTime 2` for the timings (average over
a few prints, ignore < 0.05 ms drift; standing still at the same spot for both states). Screenshots: one per
item, cache 0 and 1, same spot (`screenshot` after `r_rtgiCacheDebug 0`).

- [ ] **0. Bit-identical off.** `r_rtgiCache 0`: no `rtgi.cache` scope in `r_vkGpuTime 2`, frame time and look
      unchanged vs master (spot check against the 0.9.5 build if in doubt).
- [ ] **1. Keying (C1 check, debug 1).** `r_rtgiCacheDebug 1`: cells tile every surface, constant screen size
      while walking toward / away from a wall (a whole-wall re-tint at a distance-level boundary is expected),
      floor / wall / ceiling never share a tint across an edge, every LIT visible cell turns bright (claimed) within
      a second. Dim cells on lit visible surfaces that stay dim = full probe spans -> try `r_rtgiCacheSlotsLog2 21`
      and report.
- [ ] **2. Convergence (debug 4 / 2).** Look at a lit room: `r_rtgiCacheDebug 4` ramps to white in ~32 frames and
      stays; `2` shows a smooth low-frequency field, no single-cell fireflies, no cell drifting brighter forever.
- [ ] **3. Fidelity case A - corridor side wall** whose fixture points at the floor (Mars City lower corridors /
      Alphalabs service corridors): the wall brightens with the cache, stops at a plausible level, no pulsing.
- [ ] **4. Fidelity case B - underside** of a counter / console lip / stair (Mars City reception desk, Admin).
- [ ] **5. Fidelity case C - ceiling lit from below** (any room with floor-pointing fixtures or lit floor panels).
- [ ] **6. Spill - the far end of a dark passage a bright room opens into**: the passage's walls near the opening
      pick up the room's colour; the far end stays dark (no energy appears from nowhere).
- [ ] **7. Thin-wall leak hunt.** Mars City corridors by the GLASS walls / thin partition walls: compare the dark
      side of a thin wall whose other side is lit (same facing direction on both sides). With the default cell
      angle a faint leak within ~1 cell of the wall is the accepted residual; a leak that reads as the lit room
      "shining through" = report with a screenshot and try `r_rtgiCacheCellAngle 0.002` (half the cells).
- [ ] **8. Switched light** (procedure C3.1): off/on, the indirect fill follows within the DI's own fade; debug 4
      shows the room's cells reset to black the frame after the switch.
- [ ] **9. Swinging light** (C3.2): no longer indirect trail than the direct light's own.
- [ ] **10. Flashlight** (C3.3): the cached field (debug 2) never changes under the beam, in DI and in the
      standalone RTGI path (dude_rtx 1 + `r_rtgi 1`).
- [ ] **11. Eviction** (C3.4): three rooms and back, no holes / unclaimed lit cells.
- [ ] **12. Cost.** `r_vkGpuTime 2`: `rtgi.cache` (expected 0.05-0.15 ms), `restir.di.gather` cache 0 vs 1
      (expected +0.1-0.3 ms; > +0.4 ms = hoist the P key: one `hgMakeKey` shared by update + gradient),
      `restir.di.grad` 0 vs 1 (expected < +0.05 ms), total frame delta. Also at dude_rtx 1 (`rtgi.ray` 0 vs 1).
- [ ] **13. NRD detail at the same history.** Same spot, `r_rtReStirDIDebug 1` (raw gather) cache 0 vs 1: the
      bounce part should look smoother with the cache (averaged cells) - if so, try `r_rtReStirDIHistory` one
      step lower with the cache on and judge detail vs noise; report both.
- [ ] **14. Register headroom (G0).** `r_vkPipelineStats restir_di`, `vid_restart`, read the `restir_di [...]`
      block (fragment stage) twice: cache 0 and cache 1 (the pipeline is the same program; the stats are of the
      compiled code, so ONE readout suffices unless the driver specializes). Paste the register / spill lines
      into docs/rtx-irradiance-cache.md under C0's G0 item. Spills > 0 or registers at the device max = G's
      N_mc must drop to 3 (plan fallback).
- [ ] **15. Level change + settings.** Load another map with the cache on (debug 1: fresh dim cells, no stale
      bright ones), change `r_rtgiCacheSlotsLog2` in-game (no device loss, cache re-learns), toggle
      `r_rtgiCache` off/on (re-learns), `vid_restart` with it on.
- [ ] **16. Tune and report:** final values for `r_rtgiCacheScale`, `r_rtgiCacheHistory`, `r_rtgiCacheCellAngle`
      (and whether the 0.35 reset threshold / 0.9 albedo clamp / 64 sample clamp constants need to become cvars),
      the GPU numbers, and the screenshots -> C5 wires the cache into Ultra Nightmare.

Report format that lets the next session act without re-deriving anything: per item PASS / FAIL + one line, the
`r_vkGpuTime 2` lines for item 12, the `r_vkPipelineStats` block for item 14, and the screenshots' file names.
