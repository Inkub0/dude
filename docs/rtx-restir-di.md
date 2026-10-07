# ReSTIR DI — direct lighting (the first H7 / `dude_rtx 2` piece)

> Status: **SHIPPED to local master** (not pushed), 2026-10-07 — the **RTGI/NRD architecture**
> (`feat/rtx-restir-di-nrd-denoise`, merge `fc5df0d1`). Bring-up / debug-only: nothing in the lit
> scene consumes the DI yet, so with `r_rtReStirDI 0` (default) play is byte-identical; `r_rtReStirDIDebug`
> blits the buffer over the scene. Part of the Full-RT end-state in
> [rtx-hybrid-roadmap.md](rtx-hybrid-roadmap.md); the direct-lighting half of H7.
>
> ⚠ **The original reservoir plan was RETIRED.** This doc used to describe a hand-rolled spatiotemporal
> **reservoir** (ping-pong SSBO, temporal reproject, Stages 0–6). That reuse left moving-object trails that
> no disocclusion/weapon special-casing could fix — see **The pivot** below. The shipped design hands ALL
> temporal accumulation to NRD instead, exactly like RTGI.

## Where it lives

RTX is an orthogonal axis, not a raster preset rung:

- `dude_preset` ([Common.cpp](../neo/framework/Common.cpp)) is raster only, **0..5** (Potato … Nightmare).
- `dude_rtx` is the RT axis, **0..2**: `0` off, `1` hybrid RT ("Solid Hybrid RT"), **`2` = full RT — H7**.

**ReSTIR DI targets `dude_rtx 2`**, the tier that reclaims the name **"Ultra Nightmare"** (unrelated to the
`g_skill 4` "Ultra Nightmare" *difficulty* — a different axis). It is not preset-wired yet (bring-up); you
enable it directly with `r_rtReStirDI 1`.

## Why ReSTIR DI

Doom 3 scenes routinely have dozens–hundreds of point/projected lights in view. The hybrid tier
(`r_rtAllLights`) traces **one visibility ray per light per pixel** — the exact cost + stagger that killed
H3 soft shadows ([rtx-soft-shadows-h3.md](rtx-soft-shadows-h3.md)). ReSTIR DI (Bitterli et al. 2020)
importance-samples the light set to **one chosen light + one visibility ray per pixel**, regardless of light
count. The single visibility ray *is* the abandoned H3 shadow ray — reused, not rebuilt. It is the
direct-lighting stage of the path tracer, so most of it carries into full ReSTIR GI later.

## The pivot — reservoir → NRD architecture

The first implementation was textbook ReSTIR: a per-pixel reservoir, temporal reuse (reproject last
frame's reservoir through the velocity buffer), optional spatial reuse. It converged beautifully when
**still**, but any motion — an NPC walking, the view weapon swaying, strafing past a lit surface — left
**trails**: the reprojected reservoir carried stale light/visibility onto moving silhouettes, and every
disocclusion/weapon special-case we tried only moved the artifact around.

RTGI (H6) had already hit and solved the *same* problem for indirect light: it does **zero hand-rolled
temporal**. A fresh 1-spp gather every frame, all accumulation handed to NRD ReBLUR, a confidence guide for
moving-light antilag, and the view weapon simply **masked out**. So DI adopted RTGI's architecture wholesale
— the reservoir ping-pong, temporal merge, stable-light-id remap, and reproject were **deleted**. What
survives from ReSTIR is the cheap, correct core the perf scan endorsed (see **Performance**): resampled
importance sampling to **one light + one shadow ray**, with a good distribution; the denoiser owns stability.

## Architecture as shipped

Fullscreen fragment passes (no compute), gated on `SupportsRayQuery()` + `r_rtReStirDI`, primary view only,
needing the sealed depth + bump-normal G-buffer and the velocity MRT (`r_fsr` / `r_motionVectors`). Wired in
`RB_RHI_DrawWorld` after `RB_RHI_RtgiPass`; opens its own `restir.di.gather` / `.conf` / `.denoise` GPU
scopes (`r_vkGpuTime 2`).

### 1. The gather — `restir_di.frag` (fresh, 1-spp, no history)
Per pixel: reconstruct world pos+normal from depth, **mask the view weapon** (`nt.a < 0.5` → zero, like
RTAO/SSR/RTGI), draw `M` candidate lights uniformly from the shared `RtLight[]` set (`dudeLightInRange`
cull), weighted-reservoir-pick **one**, trace **one** any-occluder visibility ray for it, and output the
unbiased direct irradiance `rad(y)·W·vis`. The reservoir is a per-pixel local that lives and dies in this
one `main()` — there is no reservoir buffer, no reprojection.

Output is NRD's 3-attachment guide MRT (`CreateRenderTargetRtgiMrt`, the same target RTGI's gather writes),
so `RtaoDenoise(giMode)` drops in unchanged:
- **att0** (RGBA16F): NRD-**YCoCg** radiance + normalized **hit distance** in alpha. The "hit distance" for
  direct lighting = distance to the chosen light, normalized `saturate(dl / (A + |viewZ|·B))`, `A =
  0.5·1024`, `B = 0.1` (must match the `ReblurHitDistanceParameters` the pass sets). It drives ReBLUR's
  world-space blur radius.
- **att1** (R16F): signed linear view-Z.
- **att2** (RGBA16F): world normal, NRD encoding 2 (signed octahedron).

Guides are written for **every** pixel (sky/weapon included, radiance zeroed there) so NRD sees a valid
field everywhere.

### 2. The denoise — NRD REBLUR_DIFFUSE
`RB_RHI_ReStirDIPass` feeds the per-object velocity MRT (`GetRenderTargetImage3`) as NRD `IN_MV`
(`mvScale = (−1, +1)`, 2D screen-space) and calls `RtaoDenoise(giMode=true)`, which denoises into the
shared GI output (`GetRtgiOutputImage`). NRD owns **all** temporal accumulation; the debug overlay shows
that denoised output (YCoCg-decoded) when the denoise ran, else the raw gather. Previous-frame camera is
tracked (`rhiReStirDIPrev*`) for the NRD reprojection; `reset` on the first DI frame / after an interruption.

> **Shared NRD instance.** DI borrows the one `REBLUR_GI` denoiser + its output image — the same slot RTGI
> uses. Only one `giMode` denoise can run per frame (one instance = one resolution + one history). See
> **auto-claim** below.

### 3. Confidence pass — present, DEFAULT OFF
A history-confidence pass (reuses `rtgi_confidence.frag` verbatim — it is signal-agnostic) can feed NRD's
`IN_DIFF_CONFIDENCE` to reject history where lighting moved over intact geometry (moving-light antilag). On
RTGI's dim, low-variance bounce it kills edge ghosting cleanly. On DI it **fries**: the 1-spp direct signal
is noisy enough that the luminance-confidence metric misreads *stable* lit surfaces as a lighting change,
rejects history, and starves NRD. So `r_rtReStirDIConfidence` **defaults to 0 (off)** — NRD's built-in
antilag + the weapon mask carry the temporal. The knob stays for A/B.

### 4. NRD spatial cleanup — the tuning knobs
NRD cleans flat, stable, temporally-rich surfaces well, but is **edge-stopped on high-frequency geometry**
(grates) and **history-starved on moving silhouettes** (a swinging limb), so the 1-spp variance survives
exactly there. Four `giMode`-only overrides on `RtaoDenoiseArgs` (applied in `VulkanBackend::RtaoDenoise`
only when `>0`, so RTGI/RTAO keep NRD defaults) push the denoiser's spatial behaviour:

- `prepassBlurRadius` — NRD `diffusePrepassBlurRadius`: a pre-accumulation spatial reuse blur, built for
  "badly defined / probabilistic" 1-spp input. The main lever; too large on sparse bright samples can blob.
- `historyFixFrameNum` — frames of strong spatial reconstruction where history is thin (moving parts);
  clamped below the fast-history window.
- `maxBlurRadius` — base spatial denoise radius.
- `edgeStopScale` — scales `lobeAngleFraction` + `planeDistanceSensitivity`; `>1` blurs **across** depth/
  normal edges, the only lever that reaches sub-pixel grate noise (costs that detail).

Spatial **reuse** (the ReSTIR in-frame neighbour combine) is the textbook alternative to this NRD tuning and
remains the fallback if the knobs can't clean the noise without over-softening — see **TODO**.

### 5. Auto-claim — DI owns the shared NRD slot
Because the NRD instance is shared, `RB_RHI_RtgiPass` and `RB_RHI_RtaoPass` **stand down entirely while
`r_rtReStirDI` is on** (their early-out gates include `|| r_rtReStirDI.GetBool()`). So enabling DI reliably
gives it the slot — no manual `r_rtgi 0` / `r_rtao 0` juggling. **Interim:** GI/RTAO and DI are mutually
exclusive until DI gets its own NRD instance (fine while DI is debug-only).

## cvars + committed defaults

| cvar | default | what |
|------|--------:|------|
| `r_rtReStirDI` | 0 | master enable (bring-up; not preset-wired). VK + RT + velocity MRT. |
| `r_rtReStirDIDebug` | 0 | 1 = blit the DI buffer (denoised when it ran, else raw) over the scene, LINEAR. |
| `r_rtReStirDICandidates` | 16 | candidate lights `M` per pixel before the WRS pick. Clamp **1..128**. |
| `r_rtReStirDIResScale` | 1 | gather/denoise resolution fraction (0.25..1), upscaled for display. Composes with checkerboard. |
| `r_rtReStirDICheckerboard` | 0 | trace a half-pixel checkerboard (packed left), NRD reconstructs via a full-res guide pass + IN_MV. |
| `r_rtReStirDIBilateralUpscale` | 1 | depth+normal-aware upscale of a reduced-res gather (vs bilinear); inert at full res. |
| `r_rtReStirDIHistory` | 8 | NRD ReBLUR history length (its own; no longer borrows `r_rtgiHistory`). 2..63. |
| `r_rtReStirDIConfidence` | 2.4 | confidence-pass sensitivity; `0` = off. (Off on the raw signal — it fried — but ~2.4 once the NRD spatial cleanup + checkerboard tame the input.) |
| `r_rtReStirDIPrepassBlur` | 64 | `diffusePrepassBlurRadius` px; main noise lever. 0 = NRD default (30). |
| `r_rtReStirDIHistoryFix` | 0 | `historyFixFrameNum`; moving-part lever. 0 = NRD default (3). |
| `r_rtReStirDIBlurRadius` | 32 | `maxBlurRadius` px. 0 = NRD default (30). |
| `r_rtReStirDIEdgeStop` | 6 | edge-stopping scale; grate lever, costs detail. 0/1 = no scaling. |

Defaults are the "good compromise" from in-game tuning (2026-10-07). All `CVAR_ARCHIVE`, so a user's own
values persist in config regardless of these source defaults. `hitDist` scale (1024) is in-source only.

## ImGui

Debugging tab → **"ReSTIR Direct Lighting"** group (`DrawDbgGroup_ReStirDI` in `Dhewm3SettingsMenu.cpp`,
mirrors `DrawDbgGroup_RTGI`). Its **Noise Suppression** leads with **History Length** + **History
Confidence** — identical name/range to RTGI's — then the DI-specific NRD spatial levers under a subheading.
(RTGI's third, *Emissive Clamp*, has no DI equivalent: DI gathers direct lights, not an emissive bounce.)

## Substrate it rides (reuse, do not reinvent)

| Need | Where |
|------|-------|
| Candidate pool | `RtLight[]` buffer_reference, **max 256** (`RT_REFL_MAX_LIGHTS`); `dudeLightInRange`/`dudeLightDir`/`dudeLightAtten` in [rt_light_eval.glsl](../neo/shaders/rt_light_eval.glsl); filled by `RB_RHI_GatherSceneLights`, device address passed in `RenderParams.color`. The one shared light model (DI/RTGI/volumetrics). |
| Visibility ray | Inline any-occluder test (interaction.frag fast-opaque), TLAS via `GetTlasAddress()` bit-cast into `u_rtParms.xy`, mask `0xFD` (skip perforated casters), origin `Pw + Nw·r_rtSunShadowOffset`. |
| G-buffer | `RB_RHI_NormalPrepass` → `gbuffer.frag` MRT (view normal + weapon/AO mask in alpha, velocity in att3). No world-pos buffer — reconstruct from `currentDepthImage` + `u_depthParms`. |
| Denoiser | NVIDIA NRD v4.18, `RtaoDenoise(giMode)` → REBLUR_DIFFUSE ([rtx-nrd-h2.md](rtx-nrd-h2.md)). DI shares the single `REBLUR_GI` instance with RTGI. |
| Motion vectors | `gbuffer.frag` velocity att (jitter-cancelled), incl. skinned (`r_skinnedMotionVectors`) + weapon (`r_weaponMotionVectors`) motion. |
| Pass template | `RB_RHI_RtgiPass` — the recipe DI mirrors (gather MRT → velocity → confidence → `RtaoDenoise(giMode)`; prev-frame camera statics; checkerboard path for Step C). |

### House rules the pass obeys
Fullscreen fragment passes (shared `.vert`); ≤2 descriptor sets, no push-constants (all scalars via the
`rhi::RenderParams` UBO; TLAS + SSBOs via device-address bit-cast); shaders compile offline — **new**
`restir_*` shaders need a CMake re-configure ([cmake-glob gotcha]) but editing existing ones does not; new
RT targets add `CreateRenderTarget*` + handle/size/ready statics + a context-loss reset.

## What's shipped vs TODO

**Shipped (Step B `fc5df0d1`):** fresh 1-spp gather, one visibility ray, weapon mask, NRD REBLUR_DIFFUSE
denoise, confidence pass, the four NRD spatial-cleanup knobs, auto-claim, the ImGui group, candidates to 128.

**Shipped (Step C):** the full resolution set, mirroring RTGI. `r_rtReStirDICheckerboard` traces a half-pixel
checkerboard (packed left, draw restricted to the left half) and reconstructs via a ray-free `rtgi_guides`
full-res guide pass + NRD constant-BLACK; `r_rtReStirDIResScale` runs the whole chain (gather/guides/
confidence/NRD) at a fraction, with the velocity downsampled to that res; the debug overlay upscales the
reduced-res output (`r_rtReStirDIBilateralUpscale` = the `rtgi_apply` joint-bilateral, else bilinear) and
re-masks sky/weapon. Still debug-only — nothing lights the scene with DI yet.

**TODO:**
- **Spatial reuse** — if the NRD knobs can't clean the grate/limb noise without over-softening, add an
  in-frame ReSTIR spatial pass (combine geometry-matched neighbours). No history/reproject → no trails;
  distinct from the retired *temporal* reservoir.
- **DI's own NRD instance** — so GI + DI can run together and the auto-claim mutual-exclusion can retire.
- **Consume in the lit scene** — the big one: replace the `r_rtAllLights` per-light draw model as the
  `dude_rtx 2` direct-lighting path. Detailed plan in **"Next: DI lights the scene"** below.

## Next: DI lights the scene (detailed plan, authored 2026-10-07 — execute in a later session)

Goal: DI stops being debug-only and becomes the actual **direct-lighting path** at `dude_rtx 2`, replacing
the `r_rtAllLights` per-light shadow-ray loop. **Decision taken (user, 2026-10-07): "add gobo sampling
first"** — the full replacement including projected/cookie/spot lights, not a point-lights-only subset. So
there are two stages: **G** (make DI able to shade shaped/colored lights) then **D** (apply it + stop the
forward path double-counting). Everything rides a NEW cvar **`r_rtReStirDIApply`** (default 0), so with it off
the forward path is bit-identical and default play never regresses; on, it is a clean A/B against forward.

Why gobos block the clean replace: replacing a light's forward draw means DI must reproduce everything
`interaction.frag` does for it. The `RtLight` buffer today carries only a **solid color + the frustum planes**
(`rt_light_eval.glsl:15`, filled `RhiWorld.cpp:5447`); the forward path samples each light's **projection
cookie** (`din->lightImage`) and **falloff ramp** (`din->lightFalloffImage`) — `RhiWorld.cpp:1980-1981`,
`interaction.frag:531-532`. Doom leans heavily on these (spot cookies, colored lamps, flashlight), so DI must
sample them too or shaped lights render as flat box beams.

### Stage G — gobo/projection + falloff sampling in the RT light buffer
Makes DI **and** the RTGI bounce (they share `RtLight`/`dudeLightAtten`) reproduce shaped/colored lights.

- **G0 — CONFIRM FIRST (feasibility, read-only):** the bindless-residency API for light images, and the exact
  `textureProj` recipe. Grep targets: the set-2 bindless array population (`u_rtTextures[]`) in
  `VulkanBackend.cpp` + `RhiWorld.cpp` — look for `SyncBindlessSlot` / bindless / `texIndex`, i.e. how a
  material's diffuse texture gets a resident slot (the RR4 geo-table path). Determine whether an **arbitrary
  idImage** (a light's projection/falloff image, not a material texture) can be registered the same way and
  when (residency timing vs when `GatherSceneLights` fills the buffer and the DI/RTGI passes run). Read
  `interaction.frag:531-532` + `interaction.vert` for the exact projection/falloff coord math to replicate
  from the `projS/T/Q/F` planes + the hit world position. Confirm nothing still reads `RtLight.flags` bits
  16-31 (the retired reservoir stable-id) so they can be reclaimed for texture indices (grep shaders for
  `>> 16` / `& 0xFFFF` on light flags; packing at `RhiWorld.cpp:~5571-5588`). *(An agent was mid-investigation
  of exactly this when the session paused — re-run G0 before coding.)*
- **G1 — bindless-register light images:** in `RB_RHI_GatherSceneLights` (`RhiWorld.cpp:5447`), make each
  light's projection + falloff `idImage` resident in the set-2 bindless array and capture the slot indices.
  Handle **animated/scrolling cookies** (light shader stage registers can move per frame → residency per
  frame or a stable slot with updated contents; measure churn).
- **G2 — extend `RtLight`:** add `projTexIndex` + `falloffTexIndex`. Prefer reclaiming `flags` bits 16-31
  (free post-reservoir, confirm in G0); else grow the struct (GPU `rt_light_eval.glsl:15` **and** the C++
  mirror `RtReflLightGPU` `RhiWorld.cpp:5443`, keep std430 size + the 112 B comment in sync).
- **G3 — textured attenuation** in `rt_light_eval.glsl`: a `dudeLightAttenTextured()` (or extend
  `dudeLightAtten`) that projects the hit world pos through `projS/T/Q` (the same planes the forward path
  uses), `textureProj`-samples the cookie (set-2 bindless) × the falloff ramp, gated on the `PROJECTED` flag
  + a valid index; plain point lights keep the analytic radial/frustum path. Replicate `interaction.frag:531-532`.
- **G4 — use it:** `restir_di.frag` (DI gather) and `rtgi_ray.frag` (RTGI bounce) call the textured atten.
- **G5 — validate:** DI debug overlay — a spot/cookie light shows its real shape + color (not a box beam);
  RTGI bounce picks up colored light. Still no scene consumption yet.

### Stage D — deferred DI apply + forward per-light suppression
Mirrors `RB_RHI_RtgiApply` (`RhiWorld.cpp:8242`, additive `ONE|ONE` into `rhiHdrRT` at `:8319`).

- **D1 — widen the albedo G-buffer gate:** `wantAlbedo` (`RhiWorld.cpp:4432`, currently `r_rtgi && VK`) must
  also fire under `r_rtReStirDIApply`, so the standalone 4-MRT prepass produces **albedo (loc3)** + **rough/
  metal (loc1)** + bump normal (loc0) for a deferred shade. (Albedo = `GetRenderTargetImage4(rhiNormalResultRT)`.)
- **D2 — full-BRDF gather:** `restir_di.frag` shades with the G-buffer material instead of `albedo=1` diffuse:
  diffuse = `albedo·irradiance`, specular = GGX(`roughness`, F0 from `albedo`+`metalness`). v1 may denoise
  diffuse+spec **combined** as diffuse (specular slightly over-blurred); the split (REBLUR_DIFFUSE +
  REBLUR_SPECULAR, a 2nd NRD dispatch) is a later refinement.
- **D3 — DI apply pass:** additive `ONE|ONE` into `rhiHdrRT` at the opaque/translucent split
  (`RhiBackend.cpp:3573`, the `RtgiApply` site) **inside** the FSR2 opaque snapshot (`RB_RHI_FsrCaptureOpaque`
  `:3579`) or FSR2 de-ghosts it into blocky squares; order it alongside/just before `RtgiApply`. Reuse the
  bilateral upscale when reduced-res. Behind `r_rtReStirDIApply`.
- **D4 — forward suppression:** in the per-light loop (`RhiWorld.cpp:9859`), skip the **opaque** interaction
  draws (`:9942-9951`) for the lights DI covers. The DI-covered set MUST **exactly** match
  `GatherSceneLights` membership (view-ID gating `:5522-5524`, lights dropped for black/off-phase stages
  `:5538-5559`) — drive both from one per-view membership, or a light is double-bright (both) or dark
  (neither). Keep translucent (`:9963`) + film (`:9965`) forward.
- **D5 — always stays forward:** view weapon (`weaponDepthHack`/`modelDepthHack` spaces — excluded from the
  G-buffer, depth-hack makes screen-space unreliable), translucent interactions (no G-buffer coverage),
  film/decal interactions, fog/blend lights (separate multiplicative path `RB_RHI_FogAllLights`), and
  sub-views/mirrors (no G-buffer — primary fullscreen view only).
- **D6 — validate:** `r_rtReStirDIApply 0` vs `1` ≈ identical look but cheaper in light-dense scenes
  (`r_vkGpuTime 2`: forward interaction + shadow-ray cost drops, DI stays ~flat). Watch for double-bright
  (membership mismatch) or dark patches (over-suppression); verify cookie/colored lights match forward.

### Open risks / deferred decisions
- **Legacy (non-PBR) specular:** the spec *map* isn't in the G-buffer (only rough/metal) — approximate from
  roughness, or add a spec channel to the prepass. PBR-classified surfaces (`r_pbr`) are fine.
- **Shadowing model:** DI casts **one** shadow ray for the *resampled* light per pixel; a pixel lit by several
  comparable lights gets the chosen one shadowed and the rest folded in via the unbiased ReSTIR weight — not
  the per-light hard shadows `r_rtAllLights` gives. Confirm this reads acceptably (it is the whole point of the
  cost win, but shadow character changes).
- **Residency churn** from many animated cookies — measure; cap or stable-slot if needed.
- **Diffuse/spec denoise split** (2nd NRD instance) + **DI's own NRD instance** (so GI+DI coexist, retiring
  the auto-claim) pair naturally with this work.
- **Preset wiring:** once it holds up, wire `r_rtReStirDI` + `r_rtReStirDIApply` into the `dude_rtx 2` "Ultra
  Nightmare" tier (currently bring-up only, not preset-driven) + the menu RT selector label.

### Non-negotiables (same as every RT phase)
`r_rtReStirDIApply` default off, forward path bit-identical when off; `SupportsRayQuery()` + VK; primary
fullscreen view only; raster/GL3/non-RT untouched; new shaders → CMake re-configure.

## Performance (literature-calibrated, 2026-10-06)

> **At ≤256 analytic lights on a 3080 Ti, build ReSTIR's cheap core — not full production ReSTIR.**
> The 100× "millions of lights" wins come from machinery (ReGIR grids, 128×1024 light tiles, light BVHs)
> that tames light *counts we don't have*; tiling doesn't pay below ~128k lights, and our entire light set
> is smaller than RTXDI's single 16×16 tile budget. The only thing worth keeping is **resampling to one
> shadow ray/pixel with a good, temporally-stabilised distribution** — which is exactly what shipped.

**Consciously skipped at 256 lights:** ReGIR / spatial-hash grids, light tiles + presampling, light BVH /
Stochastic Lightcuts, MCMC decorrelation, Area ReSTIR. There is **no published per-pass Ampere ms** for
ReSTIR DI, so the budget is set by our own `r_vkGpuTime 2` measurement; the expectation is that the core is
cheaper than today's `r_rtAllLights` per-light-ray loop in light-dense scenes.

**Key sources:** SIGGRAPH 2023 course *A Gentle Introduction to ReSTIR* (https://intro-to-restir.cwyman.org/);
Wyman & Panteleev, *Rearchitecting Spatiotemporal Resampling for Production*, HPG 2021; NVIDIA RTXDI SDK
(https://github.com/NVIDIA-RTX/RTXDI); Bitterli et al. 2020 (original ReSTIR DI); NVIDIA NRD
(https://github.com/NVIDIA-RTX/NRD).

## Non-negotiables (carried from every prior RT phase)
- `SupportsRayQuery()` + cvar gate; default off; raster/GL3/non-RT paths bit-for-bit unchanged.
- Runs at internal (FSR2) resolution like the other RT effects; FSR2 is the perf backstop.
- New shaders: re-run CMake configure so the glob picks them up ([cmake-glob gotcha]).
