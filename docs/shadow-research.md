# Shadow-mapping state-of-the-art — survey & roadmap for DUDE

A literature survey (2026-08) mapping bleeding-edge shadow techniques onto DUDE's actual renderer:
forward, per-light interaction passes, dual RHI (OpenGL 3.3 + Vulkan 1.4), GPU-heavy on an RTX 3080 Ti.

## Where DUDE stands today

Already shipped (and genuinely good): 2D projected/spot + 6-face point **cube shadow maps**, a
**budget-limited cache** (`r_shadowMapPointLimit`) with **adaptive per-light resolution by radius**
(`r_shadowMapSizeScale`), **static/dynamic split caching** (static faces cached, only dynamic
re-renders), **per-face token invalidation**, hysteresis + per-view update stagger, and **PCF**
filtering. Most modern "scalability" papers are generalizations of this architecture — the cache is an
asset, not a liability.

Three gaps define the opportunity:
1. **No directional / cascaded path.** Parallel/"sun" lights and giant omni "sun-replacement" lights
   (radius > `r_shadowMapStencilRadius`) fall back to Carmack **stencil** volumes. This is the blocker to
   retiring stencil.
2. **PCF-only filtering** — hard-ish penumbrae, no contact-hardening.
3. **Cube pass is the heaviest GPU pass (~1.2 ms).**

Ray-query (RTX) is the long-horizon endgame but is gated on a prerequisite we lack (see §Keystone).

---

## Ranked shortlist (value : effort : fit)

### 1. Cascaded Shadow Maps (CSM) — closes the stencil-for-sun gap · MED effort · GL3 + VK
> **Milestone 1 SHIPPED (`r_shadowMapSun`, 2026-08-10)** — a single per-view fitted virtual
> projection per "sun" light, not yet cascades: a distant oversize-omni/parallel sun renders a
> fitted 2D map (perspective-from-light subtending the view sphere / ortho along the light dir,
> quantized for cache stability, `r_shadowMapSunRange`); a big walk-around omni the fit declines
> falls through to the **cube** path (adaptive tiers make it crisp) — so stencil is now the last
> resort on every routing, its CPU volume build skipped for mapped oversize lights too.
> User-verified: "looks even better", +2–3% fps. Cascades (below) remain the milestone-2 option
> if single-map sun quality ever wants more.
The unanimous #1. N stacked ortho 2D maps over view-frustum depth slices, selected per-pixel by depth.
Reuses DUDE's existing 2D-map path, PCF, and static/dynamic cache — **no new RHI primitives**, works on
both backends. Modern best-practice checklist:
- **Sphere-based cascade bounds** (rotation-invariant) — not tight AABBs (which wobble on rotation).
- **Texel snapping**: quantize each cascade origin to shadow-texel increments → kills shimmer/crawl.
  Single most important flicker fix.
- **Disable Z-clip / "pancaking"** so off-screen casters aren't clipped.
- **Blend across cascade seams** in a transition band.
- **Cascade placement**: start with **PSSM** practical-split (free), later add **SDSM** (a sub-ms compute
  min/max depth reduction that places cascades to exactly cover visible depth — automatic, no hand-tuned
  splits; VK compute native, GL3 via a mip min/max reduction).
- **Retires stencil for sun/parallel lights: YES.** This is the direct replacement and the unlock for the
  "demote stencil to Potato/Low" goal.

Refs: MJP, *A Sampling of Shadow Techniques* (therealmjp.github.io, 2013, canonical) ·
Valient, *Stable Rendering of Cascaded Shadow Maps* (ShaderX6, 2008) ·
MS Learn, *Cascaded Shadow Maps* · Alex Tardif, *Shadow Mapping* (alextardif.com) ·
Lauritzen, Salvi, Lefohn, *Sample Distribution Shadow Maps*, I3D 2011
(https://dl.acm.org/doi/10.1145/1944745.1944761 · Intel writeup) ·
Zhang et al., *Parallel-Split Shadow Maps*, GPU Gems 3 ch.10 (NVIDIA).

### 2. Per-face caster culling for the cube pass — the GPU-time omni lever · MED effort · VK-leaning
NOT multiview (see §Out). The real GPU win on the cube pass: represent a point light as **six 90°
frustums**; skip any face whose frustum doesn't intersect the view frustum, then cull casters per
surviving face (light-sphere prefilter → per-face frustum), and skip faces that end up empty. The
rigorous form culls casters that don't shadow **visible receivers** (receiver-mask + light-space
hierarchical occlusion), reported at 3–10× (cities) / 1.5–2× (games). Pairs naturally with the planned
GPU-cull compute path. Bittner is general-shadow-map, not cube-specific — combining visible-receiver
masking with per-cube-face culling is under-documented (a small original contribution for us).

Refs: Bittner, Mattausch, Silvennoinen, Wimmer, *Shadow Caster Culling for Efficient Shadow Mapping*,
I3D 2011 (https://www.cg.tuwien.ac.at/research/publications/2011/bittner-2011-scc/ ·
PDF https://arisilvennoinen.github.io/Publications/Shadow_Caster_Culling_for_Efficient_Shadow_Mapping.pdf) ·
DigitalRune shadow-caster-culling pipeline docs · Wicked Engine `DrawShadowmaps()`.

### 2.5 Optimized PCF (Castaño/gather kernels) — near-free filtering upgrade · LOW effort · GL3 + VK
Before any exotic filter: the shipped-AAA baseline upgrade to plain PCF is an **optimized
bilinear/gather kernel**. Castaño's trick (The Witness) converts an (N-1)×(N-1) kernel into
(N/2)×(N/2) hardware bilinear-PCF taps with solved weights — a 5×5 Gaussian-ish kernel in **9 taps
instead of 25**; MJP measured going 2×2 → 7×7 PCF at only **~+0.4 ms @1080p** with GatherCmp. Wider,
softer, rounder penumbrae essentially for free, identical on 2D and cube faces, no new targets.
`textureGather` is core VK / GL4.0; on our GL3.3 backend it needs `ARB_texture_gather` (universal on
DX10.1+ HW) or the pure bilinear-weight fallback. Add per-pixel **Vogel-disk + interleaved gradient
noise** rotation (Jimenez) for wider penumbrae without banding. This is the "do first, always" item.

Refs: Castaño, *Shadow Mapping Summary*, The Witness blog 2013
(https://www.ludicon.com/castano/blog/articles/shadow-mapping-summary-part-1/) ·
MJP, *A Sampling of Shadow Techniques* (OptimizedPCF/FixedSizePCF measurements) ·
Jimenez, *Next Generation Post Processing in COD:AW*, SIGGRAPH 2014 (IGN) ·
Sterna, *Contact-hardening Soft Shadows Made Fast*, 2018 (Vogel+IGN ~16 taps).

### 3. Screen-space contact shadows — cheap quality upgrade · LOW–MED effort · GL3 + VK, opt-in
A short depth-buffer ray-march toward the light per pixel, **min-combined** into the shadow factor —
adds high-frequency contact detail PCF/shadow-maps miss. Forward-friendly: reads the zfill depth we
already have; no G-buffer needed. Cost ~0.5–1.3 ms at 1080p (sample-count slider). Ship as opt-in
`r_contactShadows`, directional/flashlight-first (matches *Days Gone*'s shipped scope). Caveats
(→ opt-in, per fidelity policy): unknown thickness (`depth_thickness` param), off-screen occluders
unshadowable, self-shadow acne (min-delta bias + IGN jitter), streaking (bilinear + march-behind).

Refs: Bend Studio (Sony), *Inside Bend: Screen Space Shadows*, shipped *Days Gone* 2019, SIGGRAPH 2023
(https://www.bendstudio.com/blog/inside-bend-screen-space-shadows/) ·
h3r2tic, depth-raymarch kernel gist (https://gist.github.com/h3r2tic/9c8356bdaefbe80b1a22ae0aaee192db) ·
Panos Karabelas, *Screen space shadows* (2020) · UE5 Contact Shadows docs · Unity HDRP Contact Shadows.

### 4. Cache scheduling upgrades — refine the existing split cache · LOW–MED effort
Extend the static/dynamic split cache we already have with amortization the shipping engines use:
- **Per-face staggering** — update individual cube faces on demand (round-robin), not all six
  (HDRP `RequestSubShadowMapRendering(shadowIndex)`).
- **Importance by screen-size** — scale a light's update resolution by its screen-space area (HDRP).
  (We scale by radius today; screen-size was previously rejected for *resolution* due to cache thrash —
  but it's safe for *update-frequency* scheduling.)
- **Every-Nth-frame update rate** per light — explicit amortization for low-importance lights.
- **Budget-scheduled atlas** — CoD Cold War's fixed mem/perf budget with priority allocation.
- **Movement-threshold dirty flags** — re-render only when a light moves past a translation/angle
  threshold (we invalidate by token today; thresholds add hysteresis).

Refs: id Tech 6 / *DOOM 2016* atlas static/dynamic split (Courrèges graphics study) ·
Treyarch, *Shadows of Cold War: A Scalable Approach to Shadowing*, GDC 2021
(https://research.activision.com/publications/2021/10/shadows-of-cold-war) ·
Unity HDRP shadow-update-mode docs · UE5 Virtual Shadow Maps page-cache docs.

### 5. Ray-query hybrid shadows — the RTX endgame · HIGH effort · VK-only · soft tier gated on §Keystone
The long-horizon direction (already on the roadmap). Now grounded in hard numbers and shipped practice:

**Increment 1 — hard RT shadows via `VK_KHR_ray_query` (no denoiser, no motion vectors).**
Minimum stack is `VK_KHR_acceleration_structure` + `VK_KHR_ray_query` only — skip the RT-pipeline/SBT
extension entirely; inline queries drop into the existing forward interaction shader (our per-light
forward is a natural fit: the light position is already in hand). The 1-ray visibility test is
deterministic and noise-free. Flags: `TerminateOnFirstHit | Opaque | SkipClosestHit`; alpha-tested
surfaces marked non-opaque and resolved in the candidate loop via `rayQueryConfirmIntersectionEXT`.
- **It's genuinely fast**: Ray Tracing Gems ch.13 (RTX 2080 Ti, 1080p) — RT *hard* shadows **beat**
  shadow mapping by ~40–60% at 4 lights in heavy scenes (3.4–4.8 ms vs 5.9–9.1 ms); shadow maps win ~2×
  only in the single-light case. Ampere is faster still.
- **AS engineering** (the real work): static world = one grouped BLAS (`PREFER_FAST_TRACE` +
  compaction, ~52% memory saved, ~25–30 B/tri on NVIDIA); skinned monsters = per-frame BLAS **refit**
  (`ALLOW_UPDATE`, topology fixed) fed by **our existing deform-once compute buffer** — DUDE is unusually
  well-positioned, the deformed VB the refit wants already exists. Refit ≈5–7× cheaper than rebuild
  (Tellusim: sub-ms for D3-scale monsters); periodic/key-pose rebuild counters tree degradation; TLAS
  rebuilt per frame (cheap); AS builds on async compute. NVIDIA budget target ≤ ~2 ms AS work/frame.
- **This retires stencil outright** for every light it covers (no volume extrusion, no fill-rate cost,
  correct alpha-tested + self-shadowing) — including the oversize "sun" omnis CSM can't fully serve.
- **Shipped-practice note**: no id-Tech title has shipped RT *shadows* (Eternal/Youngblood shipped RT
  reflections; shadows stayed mapped) — this would be new ground for an id-Tech-lineage engine.

**Increment 2 — hybrid: trace only where the map is ambiguous (the AMD FidelityFX model).**
Keep shadow maps as the noise-free base; classify tiles from the map (reject fully-lit, reject
fully-shadowed, **ray-interval reduction** — shorten rays to the uncertain band) and trace 1 spp only
in penumbra tiles. This is the best perf/quality architecture on 3080-class HW with many lights, and it
preserves the entire cube-cache investment. Verdict from all sources: **ray query retires stencil;
shadow maps stay as the static/far tier of a hybrid — don't fully retire them.**

**Increment 3 — soft shadows (area lights).** Analytic **1-ray penumbra from occluder distance**
(record hit-T, derive penumbra width à la PCSS — no cone sampling) + **spatiotemporal blue-noise
(STBN)** sampling (free: a texture lookup) + a **spatial** penumbra-guided blur (NRD **SIGMA**'s
spatial core, **~0.4 ms @1440p on a 4080** — the cheapest production shadow denoiser, and the most
tolerant of missing motion vectors). Full temporal stability (SIGMA temporal / A-SVGF) **hard-requires
per-object motion vectors** (§Keystone). ReSTIR DI: **overkill** — its crossover vs per-light shadowing
is ~50+ overlapping shadowed lights/pixel; Doom 3 never gets there.

Refs: Boksansky, Wimmer, Bittner, *Ray Traced Shadows: Maintaining Real-Time Frame Rates*, Ray Tracing
Gems ch.13, 2019 (https://boksajak.github.io/files/RTG1_RayTracedShadows.pdf) ·
AMD FidelityFX Hybrid Shadows (https://gpuopen.com/fidelityfx-hybrid-shadows/) ·
Khronos, *Vulkan RT Best Practices for Hybrid Rendering* (Wolfenstein: Youngblood; 60% BLAS grouping win) ·
NVIDIA RTX best practices (refit heuristics, build flags) · Tellusim, *RT vs Animation* (refit costs) ·
zeux.io 2025 (measured AS memory) · NVIDIA NRD / SIGMA (https://github.com/NVIDIA-RTX/NRD) ·
Wolfe et al., *Spatiotemporal Blue Noise Masks*, EGSR 2022 (https://cseweb.ucsd.edu/~ravir/stbn.pdf) ·
Schied et al., *SVGF* HPG 2017 / *A-SVGF* HPG 2018 (Q2RTX) ·
Bitterli et al., *ReSTIR*, SIGGRAPH 2020 (+ crossover analysis).

---

## The keystone: per-object motion vectors

The single highest-leverage *enabling* piece. Building a motion-vector pass (prev-frame MVP vs current
per surface, or reconstruct camera motion from depth for static geo) unlocks **two** frontiers at once:
- **TAA** (already ~80% wired via the temporal-SSAO history/reprojection machinery — see
  `docs/antialiasing.md`), which is the only cheap fix for specular/normal-map shimmer.
- **Temporal RT-shadow denoising** (SIGMA temporal, A-SVGF), i.e. the soft ray-query endgame.

If we invest anywhere structural, motion vectors pay for two features.

---

## Out / deprioritized

- **Vulkan multiview single-pass cube rendering** — a **CPU/draw-call** optimization (helps our
  CPU-front-end-bound frames), **not** a GPU-time win: the rasterizer still processes 6 faces and the VS
  runs per-view (~6×). Worse, a single broadcast draw **forfeits per-face frustum culling** (§2), so on a
  GPU-bound frame it can be net-negative. Only revisit if we become CPU-bound *and* keep per-face culling.
  Avoid amplifying geometry shaders (net GPU loss); if ever single-pass, use VS-written `gl_Layer`.
- **Dual-paraboloid shadow maps** — obsolete: curved projection fights linear rasterization (needs heavy
  tessellation), split-plane seams. Tetrahedron maps (4 faces) are lighter but niche (many-light atlas),
  need manual `gl_ClipDistance` triangular clipping.
- **UE5-style Virtual Shadow Maps** — assume Nanite + deferred + GPU-driven submission; Epic explicitly
  says poor fit without Nanite. A VK-only **Sparse VSM** (ktstephano.github.io/rendering/stratusgfx/svsm)
  is the feasible variant but a large project — only a *much later* high-end VK tier, after CSM.
- **EVSM / MSM moment shadow maps** — prefilterable soft edges, but the AAA verdict is damning: the
  variance family **never displaced PCF in shipping games** (light leaks that can't be fully eliminated,
  EVSM4 = 128 bits/texel bandwidth, a depth→moments resolve pass; ~11.5 ms measured for the full EVSM4
  stack vs ~0.4 ms for 7×7 gather-PCF). MSM (Peters & Klein, I3D 2015 / JCGT 2017) is the best-in-class
  member — EVSM-grade leak resistance at 64 bits/texel — and the only one worth revisiting if a
  prefiltered soft tier is ever wanted; cube seams still forbid cross-face blurs (blur per-face). The
  Order: 1886 (SDSM+EVSM) is the one notable shipper. Defer indefinitely; optimized PCF + contact
  shadows deliver more per millisecond.
- **PCSS / contact-hardening (CHS)** — the shipped vendor add-on of 2014–2016 (GTA V, Far Cry 4, AC
  Unity...): blocker search (~16 gather taps) + variable-radius PCF (~16–64). Real distance-varying
  softness, but the heaviest per-light filter here, cube-face-seam-fragile for wide kernels, and its
  niche is largely superseded by SMRT/RT approaches. Only if contact-hardening becomes an explicit goal;
  the Vogel+IGN ~16-tap variant (Sterna 2018) is the affordable form.
- **ReSTIR DI** — over-engineered for dozens of lights. Crossover where it beats brute-force per-light
  shadowing is ~**50+ simultaneously-overlapping** shadowed lights/pixel; Doom 3 rarely reaches that.
  Adds motion-vector + G-buffer + denoiser + reservoir buffers to solve a problem we don't have. A
  genre change (hundreds–millions of lights), not a fidelity tweak.

---

## Recommended sequence

0. **Optimized gather-PCF (+ Vogel/IGN taps)** — the ~free filtering upgrade; do it whenever the
   shadow shaders are next open. (§2.5)
1. **CSM (+ PSSM split, sphere-stabilized, texel-snapped)** — the flagship: retires stencil for
   sun/parallel lights on both backends, reusing infrastructure we already ship. Later bolt on SDSM.
2. **Screen-space contact shadows** (`r_contactShadows`, opt-in) — cheap, stackable now, quality bump.
3. **Per-face caster culling** — GPU-time omni win; feeds the planned GPU-cull compute path.
4. **Per-object motion vectors** — the keystone; unlocks TAA *and* the RT-shadow denoising endgame.
5. **Ray-query hybrid shadows** — hard RT first (beats shadow maps outright at 4+ lights per the RTG
   numbers, and our deform-once buffer already feeds the BLAS refit), then the FidelityFX-style
   ambiguous-band hybrid, then soft (SIGMA + STBN) once motion vectors land.

---

## 2026-08 recency check — cube-pass cost mitigation (settled; don't re-litigate)

A fresh literature sweep (SIGGRAPH 2025 Advances, Eurographics/CGF 2025, i3D/EG 2026 catalogs,
current shipping practice) targeted at the measured **~1.2 ms point-light cube pass** — the single
biggest GPU pass at Nightmare. Conclusions recorded here so the survey stays the single source of
truth.

### Where the 1.2 ms actually lives (measured, r_vkGpuTime A/B)

Three terms, and every mitigation must name which it attacks. Two A/Bs pin them down:
halving `r_shadowMapPointSize` saved **~0.87 ms**, while cutting PCF taps 12→8 saved only
**~0.1 ms** (the taps lever is measured DEAD — see `shadow-map-perf-and-stencil` memory). Taps
don't move the needle but resolution does → the dominant cost is **resolution-quadratic face
work**, not interaction-pass tap count:
- **(a) Face-render fill + big-map bandwidth** — re-rendered 2048² faces (clear + depth fill,
  quadratic in res) plus the cache-miss footprint of sampling huge maps (same tap count, wider
  texel spread). The dominant term.
- **(b) Re-renders for movers** — the split cache measures 85–100% hit through combat; the
  irreducible churn is **moving lights** (muzzle flash, projectiles, monster-carried), which no
  cache can ever serve (a moving light *must* re-render all its faces). (a) and (b) compound:
  every un-cacheable re-render pays the resolution-quadratic fill.
- **(c) Interaction-pass sampling + fixed per-face draw overhead** — measured small (the dead
  taps lever); the draw side is a CPU story anyway.

### Verdict 1 — cheap levers: what's left is smaller than it looks; nothing 2024–2026 supersedes it

1. **Resolution-by-intrinsic-radius ALREADY EXISTS — don't rebuild it.** `r_shadowMapSizeScale`
   (default on, `RB_RHI_ShadowTier`, ½×…4× tiers, cube + 2D pools) shipped long ago, and the
   pivot tune (`r_shadowMapSizeScaleRadius` 340→480 on Ultra/Nightmare) already landed,
   user-verified. Remaining headroom on this lever = raising the pivot further (per-user console
   knob, area-dependent benefit) — not a new feature. (This sweep initially re-proposed it;
   caught against the 2026-08-10 recon. The trap to avoid next time.)
2. **Half-res dynamic scratch layer** (the one genuinely new cheap lever from this sweep): the
   split cache's *dynamic* layer re-renders every frame regardless — it has **no cache key to
   thrash** — so rendering movers at half resolution is safe by construction and quarters the
   per-frame fill they pay. Matches HDRP/CoD budget-by-importance practice. Attacks (a)×(b)
   where they compound.
   > **SHIPPED + user-verified 2026-08-20** (`r_shadowMapSplitDynDrop`, default **1** = half;
   > 2 = quarter; ImGui knob in Debugging → Shadows → static-cache group). The ratio rides
   > `u_pbrParms2.z` (the old 0/1 dyn gate), widening the dynamic cube's PCF disc + depth bias
   > to its coarser texels — verified "reasonably well, cube-edge softness"; static shadows
   > untouched via the min() combine. Measured on a single-zombie scene: ~0.04 ms (noise-floor,
   > as predicted — the win scales with split-light count and debris load). A/B lesson re-learned:
   > the GPU timer reads vsync-padded frames — **disable vsync before any r_vkGpuTime A/B**.
   > This lever is now CLOSED; the remaining structural work is the Option A/B decision below.
3. **Update scheduling** (Nth-frame refresh for low-importance lights, screen-size update
   frequency — the T2 item): confirmed still-current practice (UE5.7 VSM page caching, HDRP
   OnDemand/OnEnable update modes are the same idea industrialized). Attacks (b) by amortizing.
4. **Castaño gather-PCF** (§2.5) — **demoted from perf lever to quality-per-ms upgrade** by the
   dead-taps A/B: with taps 12→8 worth only ~0.1 ms, fewer fetches can't buy much. Its real value
   is a *wider, softer kernel* at unchanged cost. Do it for looks whenever the shadow shaders are
   next open; don't book perf for it.

### Verdict 2 — two live options for the big structural win (BOTH kept open, decision pending)

User leaning 2026-08: **RT-for-moving-lights is the way to go** — but the call is deferred until a
visual check; keep both alive until then.

- **Option A — RT for the un-cacheable lights** (refines R3's "then cube lights" into "***moving***
  cube lights first"). The heavy-pass profile's honest gap was "a moving-light lever is a separate,
  harder story" — this is that lever, and the cost decomposition above is squarely on its side: it
  deletes the resolution-quadratic re-render fill (the measured dominant term) exactly where the
  cache provably can't help. A moving light forces 6-face re-renders every frame on the map path,
  while a ray costs the same whether the light moves or not — and a local cube light covers far
  fewer pixels than the full-screen sun that measured +0.6 ms. Shipping practice corroborates the
  trajectory (id Tech 8 fully ray-based for *The Dark Ages*; UE5 MegaLights using rays precisely
  for the dynamic-light case). VK+RT-gated, feeds the R4 hybrid directly. Attacks (a)×(b) at the
  root. **Visual check required before committing**: RT hard shadows are exact/unfiltered — the
  moving-light look (muzzle flash, thrown barrels) must be eyeballed against the PCF-softened cube
  look for consistency.
- **Option B — VRS on the interaction pass** (`VK_KHR_fragment_shading_rate`, Turing+/RDNA2+;
  VK-only, GL3.3 has nothing). *Not previously surveyed.* Doom Eternal (forward, like DUDE)
  shipped hardware VRS on its forward passes with substantial pixel-shader savings; Microsoft's
  2026 Dark Ages write-up shows the win eroded only after id moved to compute-deferred — forward
  renderers are where VRS shines. 2×2 rate on interaction passes cuts per-pixel shading across
  *all* lights. **Honest cap:** the dead-taps A/B says the cube pass's *sampling* term is small,
  so VRS's win against the cube pass specifically is bounded — its real case is the whole
  lighting/interaction cost (specular, PBR, POM), of which shadow sampling is one slice. Evaluate
  it as a general lighting-pass lever, not a shadow fix. **Fidelity flag:** coarsens shading →
  strictly an opt-in cvar per policy; FSR2's temporal resolve masks most softening on Nightmare.

### Verdict 3 — checked and still overkill / still dead

- **Stochastic many-light shadowing** — the 2025 crop (*Many-Light Rendering Using ReSTIR-Sampled
  Shadow Maps*, EG 2025 · UE5 *MegaLights* · HypeHype stochastic tile-based lighting, both
  SIGGRAPH 2025 Advances) all converge on importance-sampling which few lights get real shadow
  work per frame, degrading the rest, leaning on temporal accumulation. At Doom 3 light counts
  this is the same "genre change" verdict as ReSTIR DI (§Out). The transferable kernel —
  importance-driven per-frame shadow budget — is exactly Tier-1 items 2–4, buildable with none of
  the machinery.
- **Reconfirmed dead ends, 2026-08 stamp:** multiview single-pass cube (still forfeits per-face
  culling), EVSM/MSM (no revival anywhere), dual-paraboloid/tetrahedral (silence), UE5-style VSM
  (5.7 makes non-Nanite viable but still assumes a GPU-driven submission pipeline — much-later
  project at best). *Real-Time Importance Deep Shadow Maps* (CGF 2025) is transparent/volumetric
  casters — niche for D3.

### Recommended order (this sweep)

Half-res dynamic scratch → T2 update scheduling (both cheap, cache-safe, attack the measured
dominant term) → then the Option A vs B decision (user visual check pending; **A favored** — and
the cost decomposition backs that instinct). Gather-PCF opportunistically, for quality not perf.

Refs (2026-08 sweep): SIGGRAPH 2025 Advances course (https://advances.realtimerendering.com/s2025/) ·
Zhang, Lin, Wyman, Yuksel, *Many-Light Rendering Using ReSTIR-Sampled Shadow Maps*, CGF/EG 2025
(https://www.cemyuksel.com/research/papers/restir-shadow-maps-eg2025.pdf) ·
Lempiäinen, *Stochastic Tile-Based Lighting in HypeHype*, SIGGRAPH 2025 Advances
(https://advances.realtimerendering.com/s2025/content/s2025_stb_lighting_v1.1_notes.pdf) ·
*Fast as Hell: idTech8 Global Illumination*, SIGGRAPH 2025 Advances ·
Microsoft, *How Variable Rate Compute Shaders Improved GPU Performance in DOOM: The Dark Ages*, 2026
(https://developer.microsoft.com/en-us/games/articles/2026/04/variable-rate-compute-shaders-doom-the-dark-ages/) ·
Coenen, *DOOM Eternal Graphics Study* (https://simoncoenen.com/blog/programming/graphics/DoomEternalStudy) ·
Kern, Brüll, Grosch, *Real-Time Importance Deep Shadow Maps with Hardware Ray Tracing*, CGF 2025 ·
Stephano, *Sparse Virtual Shadow Maps* (https://ktstephano.github.io/rendering/stratusgfx/svsm) ·
StraySpark, *VSM Optimization for Open Worlds in UE5.7*
(https://www.strayspark.studio/blog/virtual-shadow-map-optimization-open-worlds-ue5-7).
