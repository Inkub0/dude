# ReSTIR DI — direct lighting (the first H7 / `dude_rtx 2` piece)

> Status: **SHIPPED to local master** (not pushed), 2026-10-07 — the **RTGI/NRD architecture**
> (`feat/rtx-restir-di-nrd-denoise`, merge `fc5df0d1`). Bring-up / debug-only: nothing in the lit
> scene consumes the DI yet, so with `r_rtReStirDI 0` (default) play is byte-identical; `r_rtReStirDIDebug`
> blits the buffer over the scene. Part of the Full-RT end-state in
> [rtx-hybrid-roadmap.md](rtx-hybrid-roadmap.md); the direct-lighting half of H7.
>
> **2026-10-11 REVIEW + PLAN:** the shipped design was compared against public ReSTIR DI (RTXDI, RTX Remix, Q2RTX, NRD,
> the papers) with the goal of minimal ghosting/lag + preserved detail. Verdict: it is exhaustive RIS + a radiance-
> accumulating denoiser, and the lag symptoms follow from that; the fixes are a reservoir-gradient confidence pass,
> ReLAX, and spatial reuse with decoupled visibility. See **Review against public ReSTIR** and **Plan R0-R6** below.
>
> **2026-10-13 R6 CONSOLIDATION (build-verified, user playtest pending):** DI is now **ReLAX-only** (the ReBLUR path +
> its dead knobs removed for DI; ReBLUR still serves RTAO/RTGI), the forward path is **weapon-only** (mode-1 fast-light
> rules + handover crossfade removed - the R1 gradient covers the rest), the dormant **luminance confidence** is gone,
> and two RTAO/RTGI scissor chain-ender bugs are fixed. Still open: **R3b** spatial reuse (the structural 1-spp / 5 ms
> gather answer) + the **[A]** NRD shared-path items - see **R6** in the plan.
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
temporal**. A fresh 1-spp gather every frame, all accumulation handed to NRD (ReLAX only, since R6), a confidence guide for
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
RTAO/SSR/RTGI), weigh the candidate lights from the shared `RtLight[]` set — **every** light in the buffer
once by default (exhaustive RIS, `r_rtReStirDICandidates 0`; `dudeLightInRange` sphere cull + the exact box
pre-reject keep it cheap), or `M` uniform random draws (`M > 0`) — weighted-reservoir-pick **one**, trace
**one** any-occluder visibility ray for it, and output the unbiased direct irradiance `rad(y)·W·vis`. The
reservoir is a per-pixel local that lives and dies in this one `main()` — there is no reservoir buffer, no
reprojection.

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

### 2. The denoise — NRD RELAX_DIFFUSE (R6: ReLAX-only)
`RB_RHI_ReStirDIPass` feeds the per-object velocity MRT (`GetRenderTargetImage3`) as NRD `IN_MV`
(`mvScale = (−1, +1)`, 2D screen-space) and calls `RtaoDenoise(giMode=true)` with `denoiser = 1`, which
denoises the DI radiance through `RELAX_DIFFUSE(_SH)` into the shared GI output (`GetRtgiOutputImage`).
ReLAX won the R2 A/B (sharper shadows, shorter flashlight trail, no floor flicker) and is now the only DI
denoiser — the ReBLUR path was removed for DI at R6 (ReBLUR still serves RTAO / RTGI). NRD owns **all**
temporal accumulation; the debug overlay shows that denoised output (YCoCg-decoded) when the denoise ran,
else the raw gather. Previous-frame camera is tracked (`rhiReStirDIPrev*`) for the NRD reprojection;
`reset` on the first DI frame / after an interruption / on a camera cut.

> **Shared NRD instance.** DI borrows the one `REBLUR_GI` denoiser + its output image — the same slot RTGI
> uses. Only one `giMode` denoise can run per frame (one instance = one resolution + one history). See
> **auto-claim** below.

### 3. Confidence pass — the R1 gradient (R6: luminance fallback removed)
The DI history confidence fed to NRD's `IN_DIFF_CONFIDENCE` is the **R1 gradient** (`r_rtReStirDIGradient`,
default on): re-shade last frame's chosen light at the reprojected surface under this frame's lights, A-trous
filter the change, and drop history only where the lighting really changed — the RTXDI / Q2RTX recipe (see
**R1** in the plan). The old luminance-confidence fallback (a `rtgi_confidence.frag` reuse pass behind
`r_rtReStirDIConfidence`) was removed at R6: on DI's noisy 1-spp signal the luminance metric misread *stable*
lit surfaces as a change and fried them — it measured noise, not lighting change, which is exactly what the
gradient fixed.

### 4. NRD spatial cleanup — the tuning knobs
NRD cleans flat, stable, temporally-rich surfaces well, but is **edge-stopped on high-frequency geometry**
(grates) and **history-starved on moving silhouettes** (a swinging limb), so the 1-spp variance survives
exactly there. The DI-specific overrides on `RtaoDenoiseArgs` that ReLAX reads (applied in
`VulkanBackend::RtaoDenoise` only when `>0`, so RTGI/RTAO keep NRD defaults) push the denoiser's spatial
behaviour. R6 trimmed the two ReBLUR-only ones (`prepassBlurRadius`, `maxBlurRadius`) now DI is ReLAX-only;
what remains:

- `historyFixFrameNum` — frames of strong spatial reconstruction where history is thin (moving parts);
  clamped below the fast-history window.
- `edgeStopScale` — scales `lobeAngleFraction`; `>1` blurs **across** depth/normal edges, the only lever
  that reaches sub-pixel grate noise (costs that detail).
- the ReLAX A-trous settings (`r_rtReStirDIRelax*`) — Phi luminance, iteration count, variance history,
  confidence-driven relaxation, anti-firefly — are the primary spatial levers under ReLAX.

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
| `r_rtReStirDIDebug` | 0 | 1 = raw gather, 2 = denoised, **R0:** 3 = NRD validation overlay, 4 = history length ramp, **R1:** 5 = gradient confidence, 6 = filtered gradient, **R3a:** 7 = boosted pixels (white). |
| `r_rtReStirDICandidates` | 0 | candidate lights per pixel before the WRS pick: **0 = every light** (exhaustive RIS, no candidate noise), `M > 0` = M uniform random draws (a rarely-drawn light reads dimmer: sparse spikes the denoiser clips). Clamp **0..128**. |
| `r_rtReStirDIResScale` | 1 | gather/denoise resolution fraction (0.25..1), upscaled for display. Composes with checkerboard. |
| `r_rtReStirDICheckerboard` | 0 | trace a half-pixel checkerboard (packed left), NRD reconstructs via a full-res guide pass + IN_MV. |
| `r_rtReStirDIBilateralUpscale` | 1 | depth+normal-aware upscale of a reduced-res gather (vs bilinear); inert at full res. |
| `r_rtReStirDIDynamicForward` | 1 | **R6 bool:** 1 = first-person weapon lights stay forward (masked out of DI anyway), 0 = none (every light through DI). The gradient handles all other lights; the old mode-1 fast-light rules + handover crossfade were removed. |
| `r_rtReStirDIHistory` | 8 | NRD history length (its own; no longer borrows `r_rtgiHistory`). 2..63. |
| `r_rtReStirDIHistoryFix` | 6 | `historyFixFrameNum`; moving-part lever. 0 = NRD default (3). |
| `r_rtReStirDIEdgeStop` | 6 | edge-stopping scale (ReLAX `lobeAngleFraction`); grate lever, costs detail. 0/1 = no scaling. |
| `r_rtReStirDIHistoryMs` | 0 | **R0** history window in ms (0 = `r_rtReStirDIHistory` frames); converted per frame from a 0.2 s low-passed FPS. |
| `r_rtReStirDIFastClamp` | 2.0 | **R0** NRD `fastHistoryClampingSigmaScale` [1;3] (also read by ReLAX). |
| `r_rtReStirDIAntilag` | 1 | **R0** NRD built-in antilag; 0 = inert (NRD's recommended initial-integration baseline). |
| `r_nrdSigma` | 0 | **R0** keep `SIGMA_SHADOW` in the NRD instance (never dispatched since H3); rebuilds NRD. |
| `r_rtReStirDIGradient` | 1 | **R1** temporal gradient confidence -> NRD IN_DIFF_CONFIDENCE (re-shade last frame's light; RTXDI / Q2RTX recipe). |
| `r_rtReStirDIGradientPower` | 4 | **R1** confidence sharpening power. |
| `r_rtReStirDIGradientHistory` | 0 | **R1** rise-only temporal smoothing (frames); 8 is the reservoir-reuse value, not ours. |
| `r_rtReStirDIGradientFloor` | 0.1 | **R1** minimum confidence (ReLAX applies it directly). |
| `r_rtReStirDIGradientBias` | 0.001 | **R1** darkness bias in the change's denominator. |
| `r_rtReStirDIGradientDeadzone` | 0.25 | **R1** relative per-light change below which nothing counts (authored flicker). |
| `r_rtReStirDIGradientStatic` | 2.0 | **R1** static threshold (full-res px) for the exact previous-position re-shade. |
| `r_rtReStirDIGradientStratum` | 3 | **R1** NxN stratum (3 = RTXDI 1/9; 5 = NRD's 1/5 res, ~1/3 the cost). |
| `r_rtReStirDIGradientFilterPasses` | 2 | **R1** A-trous passes over the gradient (RTXDI 4); 2 hugs a change tighter (user-tuned). |
| `r_rtReStirDIRelaxFast` / `RelaxPrepass` / `RelaxPhi` / `RelaxAtrous` / `RelaxVarianceHist` / `RelaxConfRelax` / `RelaxFirefly` | 1 / 8 / 1.0 / 3 / 1 / 0.7 / 1 | **R2** ReLAX settings (Remix direct-light preset + RTXDI sample). ReLAX is the only DI denoiser since R6. |
| `r_rtReStirDIBoost` | 2 | **R3a** K picks + K rays on clustered YOUNG pixels (revealed from behind an occluder / entered the screen / held a few frames); 0 or 1 = off, 2..4 = K. Default 2 since 2026-10-12 (user decision). |
| `r_rtReStirDIBoostHistory` | 0 | **R3a** NRD-history trigger: frames below which a pixel is boosted (stays until 2x, capped at `r_rtReStirDIHistory`); 0 = off, only true disocclusion + screen entry (held 4 frames). |

Defaults are the user-tuned compromise (2026-10-07/08). All `CVAR_ARCHIVE`, so a user's own values persist in
config regardless of these source defaults. `hitDist` scale (1024) is in-source only.

## ImGui

Debugging tab → **"ReSTIR Direct Lighting"** group (`DrawDbgGroup_ReStirDI` in `Dhewm3SettingsMenu.cpp`,
mirrors `DrawDbgGroup_RTGI`). Its **Noise Suppression** leads with **History Length** — identical name/range
to RTGI's — then the DI-specific ReLAX spatial levers (History Fix, Edge-Stop Relax) under a subheading. R6
removed the now-unnecessary **History Confidence** (the R1 gradient replaced it) and the ReBLUR-only
**Prepass Blur** / **Blur Radius** sliders.

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

**Status 2026-10-08 (end of day, all on local master, not pushed):** `dude_rtx 2` "Ultra Nightmare" is real.
- Stage G: scene lights carry their cookie + falloff textures (`r_rtLightTextures`), 2D-only cookies.
- Stage D: DI lights the scene (`r_rtReStirDIApply`): full-BRDF deferred gather (demodulated), additive apply at the
  opaque split inside the FSR2 snapshot (full-view scissor), forward per-light suppression from the gather's own
  membership, grate-aware visibility ray with a per-view box list, material parity (stage colour, vertex colour,
  spec map, baked occlusion map).
- Fast-changing lights stay forward (`r_rtReStirDIDynamicForward`): weapon lights, moved, recoloured (20 %) or
  re-appeared, 16-frame hysteresis; DUDE's fill lights exempt; the combined bounce skips them too.
- Combined DI + GI in one buffer, one NRD denoise (`r_rtReStirDICombineGI`, the "path 2" decision) via the shared
  `rtgi_bounce.glsl`; RTGI's own pass stands down under DI.
- Debug: `r_rtReStirDIDebug` 0/1/2 (off / raw / denoised) + ImGui dropdown; `r_shadowMapDebug 1` prints
  "N of M lit lights DI-covered".
- Preset: `dude_rtx 2` applies/detects it; both menus offer "Ultra Nightmare".
- Tuned defaults (user, 2026-10-08): ResScale 0.75, Checkerboard on, Bilateral on, History 16, Confidence 0.1,
  PrepassBlur 36, BlurRadius 24, HistoryFix 5, EdgeStop 0.1.
- Device-loss tooling born of the (hardware) crash hunt: DI cvars in the idle-rebuild signature, NV diagnostic
  checkpoints on every GPU scope + sync submission, named fence waits, per-light `light.*` scopes.

**Shipped (Step B `fc5df0d1`):** fresh 1-spp gather, one visibility ray, weapon mask, NRD REBLUR_DIFFUSE
denoise, confidence pass, the four NRD spatial-cleanup knobs, auto-claim, the ImGui group, candidates to 128.

**Shipped (Step C):** the full resolution set, mirroring RTGI. `r_rtReStirDICheckerboard` traces a half-pixel
checkerboard (packed left, draw restricted to the left half) and reconstructs via a ray-free `rtgi_guides`
full-res guide pass + NRD constant-BLACK; `r_rtReStirDIResScale` runs the whole chain (gather/guides/
confidence/NRD) at a fraction, with the velocity downsampled to that res; the debug overlay upscales the
reduced-res output (`r_rtReStirDIBilateralUpscale` = the `rtgi_apply` joint-bilateral, else bilinear) and
re-masks sky/weapon.

**TODO (in rough priority):** - SUPERSEDED 2026-10-11 by **Plan R0-R6** below (the lag / ghosting / detail work);
the items here stay as the material-parity backlog.
- **Play-test the whole tier** at `dude_rtx 2` with `r_vkGpuTime 2`: brightness/spec parity vs `r_rtReStirDIApply 0`,
  grates, flashlight/flash lag, combined-GI look + cost (~2.2 ms expected), the handover dip when a light settles.
- **Remaining v1 approximations:** specular stage colour; legacy non-PBR spec LUT; Toksvig; organic spec fallback;
  AO + occlusion map on the combined diffuse+spec term (needs the diffuse/spec denoise split); `noSpecular`;
  multi-stage cookies take the first live stage.
- **Diffuse/specular denoise split** (REBLUR_DIFFUSE + REBLUR_SPECULAR), only if specular over-blur reads badly.
- **Per-view grate box list** overflows at 64 (last box swallows the rest → most pixels take the slow ray in
  grate-heavy areas): measure, then cull or raise.
- **RTAO coexistence:** RTAO still stands down under DI (the GI replaces it at this tier anyway); revisit only if
  AO is missed.
- **Spatial reuse** — still available if NRD's knobs ever prove insufficient.

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

### Stage G — gobo/projection + falloff sampling in the RT light buffer — SHIPPED 2026-10-08
Makes DI **and** the RTGI bounce (they share `RtLight`) reproduce shaped/colored lights. Branch
`feat/rtx-restir-di-light-textures`. Behind **`r_rtLightTextures`** (default **1**; 0 = the old analytic
frustum attenuation for A/B). Not behind `r_rtReStirDIApply` because nothing in the lit scene consumes DI
yet; the only default-visible change is the RTGI bounce (opt-in `r_rtgi`) now picking up real cookie
shapes/colours instead of uniform box beams.

**G0 findings (feasibility, verified in source):**
- **No residency API needed.** `VulkanBackend::CreateTexture2D` calls `SyncBindlessSlot(handle)` on every
  image it creates, slot = `rhiHandle - 1`, and the set-2 array is bound once per frame cb for every draw.
  So a light image's bindless index is simply `idImage::rhiHandle` (0 = not bridged). Under VK images
  demand-load on `Bind()` (`RB_RHI_BindUnit` idiom), so the gather calls `img->Bind()` before reading the
  handle — otherwise a light whose forward draws are later suppressed (Stage D) would never load.
- **Projection recipe** (`interaction.vert:47-53` + `interaction.frag:531`): cookie =
  `textureProj(lightProjection, (S·p, T·p, 0, Q·p))` = sample at `(S·p/Q·p, T·p/Q·p)`; falloff =
  `texture(lightFalloff, (F·p, 0.5))`; `lightAtten = cookie * falloff` (RGBA; rgb multiplies the colour).
  Light images are `TR_CLAMP_TO_ZERO` → VK `CLAMP_TO_BORDER` black, so outside the frustum the textures
  read black by themselves — no analytic gate when textured, exactly like forward. Behind the apex
  (`Q·p <= 0`) is rejected explicitly.
- **Texture matrix**: a light stage with `texture.hasMatrix` (scrolling/rotating cookie) has
  `RB_GetShaderTextureMatrix(lightRegs)` baked into the S/T planes per draw (`tr_render.cpp:834-836`,
  `RB_BakeTextureMatrixIntoTexgen`). The gather already evaluates the registers (`cregs`), so it bakes the
  same matrix into the world-space planes (`RB_RHI_BakeLightTextureMatrix`, a global-free copy).
- **Flags bits 16-31**: no shader read them (the `g.flags >> 16` sites are GEOMETRY rows); the C++ still
  packed the retired reservoir's lightDef index there — removed. Two handles need ~13 bits each, so the
  struct **grew 112 → 128 B** (`projTex, falloffTex, pad0, pad1`) instead of packing into 16 bits.
- **`rt_light_eval.glsl` stays sampler-free** (its stage-agnostic contract for the future compute froxel
  route). The textured path lives in a NEW include **`rt_light_tex.glsl`** that declares `u_rtTextures[]`
  (set 2) under a guard and provides `vec3 dudeLightAttenTex(...)` (falls back to `dudeLightAtten` when
  `projTex == 0`). `rtgi_ray.frag`'s own `u_rtTextures` declaration now comes from that include.

**Shipped pieces (G1–G4):** `RB_RHI_GatherSceneLights` fills `projTex` from the first live stage's
`texture.image` and `falloffTex` from `ld->falloffImage` (the fade-override-aware one); multi-stage lights
sum all stage colours as before but carry the first live stage's cookie (approximation, rare). DI gather
(`restir_di.frag`) and RTGI bounce (`rtgi_ray.frag`) take `u_windowCoord.w = r_rtLightTextures` and call
`dudeLightAttenTex` → `rad = color * atten(rgb) * N·L`. Attenuation is now a **vec3** (coloured gobos).
`ssr_rt.frag` / `vol_march.frag` keep the analytic path (unchanged, out of scope).

**G5 — validate (USER TEST pending):** `r_rtReStirDI 1 r_rtReStirDIDebug 1` on VK: a spot/cookie light
shows its real cone/blob + colour (not a square pyramid); toggle `r_rtLightTextures 0/1` to A/B;
`r_rtgi 1 r_rtgiDebug 1`: the bounce picks up coloured/shaped light. Watch for: a light reading darker
than before (expected — cookies are black outside the blob, that's the point), any light going fully
black (a cookie that failed to bridge → `VK: no RHI image` warning), scrolling cookies drifting vs forward.

### Stage D — deferred DI apply + forward per-light suppression — SHIPPED 2026-10-08 (user test pending)
Branch `feat/rtx-restir-di-apply`. Behind **`r_rtReStirDIApply`** (default 0; needs `r_rtReStirDI 1`; the debug
overlay `r_rtReStirDIDebug` overrides it). ImGui: Debugging → ReSTIR Direct Lighting → "Light the Scene with DI".

**Decision point (per view, before the light loop):** `RB_RHI_ReStirDIPass` computes `diApply` = cvar on + no
debug overlay + albedo/material G-buffer present + apply shader loadable, and sets `rhiReStirDIApplyThisView`
after the gather ran. The forward loop's suppression and the apply BOTH key off that one flag, so they cannot
disagree within a view (and a sub-view, where the DI pass early-outs, is never suppressed).

- **D1 — G-buffer:** `wantAlbedo` (`RB_RHI_NormalPrepass`) now also fires for `r_rtReStirDI && r_rtReStirDIApply`
  (forces the standalone 4-MRT prepass: normal / material / velocity / albedo). NEW: the prepass binds the
  surface's **specular map on unit 3** and `gbuffer.frag` writes its **luminance into material att1 `.b`**
  (`out_Material = (rough, metal, specLum, 1)`; nothing else read `.b`; black = no spec stage, as forward).
- **D2 — full-BRDF gather (`restir_di.frag` apply mode, `u_localParam1.w`):** binds albedo (unit 2) + material
  (unit 3) and shades each candidate with interaction.frag's PBR combine: GGX NDF (pi folded out), Schlick-GGX
  vis, `lobe = min(D·vis, r_pbrFireflyClamp)`, `spec = lobe·F·specMask·mix(r_pbrSpecScale,1,metal)·2`, the
  metal env floor `F0·metal·r_pbrEnvScale·(1-0.5·rough)·specMask`, diffuse `albedo·(1-F)·(1-metal·kd)` with
  `kd = 1 - r_pbrMetalDiffuse`; `r_pbr 0` → Lambert only. pHat = luminance of the SHADED contribution (so a
  light that only grazes a metal specularly can still be picked). **Demodulation:** the output is divided by
  `diDemodFactor(albedo, metal, specMask)` (`restir_di_common.glsl`, = the material's reflectance share, floored
  0.02) so NRD denoises a texture-free signal; the apply multiplies the identical factor back (exact).
- **D3 — apply pass `RB_RHI_ReStirDIApply` + `restir_di_apply.frag`:** additive ONE|ONE into `rhiHdrRT` at the
  opaque/translucent split, right before `RtgiApply` at all three of its call sites (inside the FSR2 opaque
  snapshot), idempotent per view. Upscales like the debug overlay (bilateral when reduced-res), remultiplies
  by `diDemodFactor` from the full-res G-buffer, by **`backEnd.lightScale`** (forward `lightColor = lightScale ×
  stage colour`; the gather carries raw stage colours) and by the AO-on-direct-light term (`mix(floor,1,ao)`
  blended by `r_ssaoDirectLight`, when an AO buffer exists this view). Sky + weapon get nothing.
- **D4 — forward suppression (`RB_RHI_DrawWorld`):** `RB_RHI_GatherSceneLights` now records per-view
  MEMBERSHIP (generation-stamped per lightDef index, `RB_RHI_SceneLightCovered`). A lit light is DI-covered iff
  the apply flag is set, it has a lightDef, is not ambient, and the gather included it this view+frame. For a
  covered light: no `SelectLightShadowTechnique` (no shadow map / cube / sun map / blurred ray mask), no stencil
  volumes; `lightRtPoint` is set when the TLAS is live so the surfaces that still draw forward take the inline
  ray (mode 4), else unshadowed. `RB_RHI_DrawInteraction` early-returns for the light's **opaque world** surfaces
  (depth-EQUAL, not weapon/model depth-hacked, not ambient, not a film draw).
- **D5 — stays forward:** view weapon + depth-hacked models, translucent interactions (depth-LESS), blood films,
  ambient lights (never in the buffer), fog/blend lights, sub-views, and any light beyond the buffer's capacity
  (248) or without a lightDef — all by construction of the membership rule.
- **D6 — validate (USER TEST pending):** `r_rtReStirDI 1` then toggle `r_rtReStirDIApply 0/1` → ≈ the same look;
  `r_vkGpuTime 2`: the per-light interaction + shadow/blur-mask cost drops, `restir.di.*` + `restir.di.apply`
  appear. `r_shadowMapDebug 1` prints "N of M lit lights DI-covered". Watch for double-bright (a light drawn by
  both — membership mismatch), dark patches (over-suppression), brightness scale vs forward (lightScale), spec
  highlights (spec map luminance ≈ forward's rgb mask), and NRD softness on the now-textured signal.

**Fixed after the first user test (2026-10-08):** "parts of the map scissored away, only self-lit stages visible"
= the apply quad was drawn under the LAST surface draw's per-surface scissor rect (the SSR pass that used to
reset it early-outs with `r_ssr`/`r_rtReflections` off), so DI only landed inside that patch while the covered
lights' forward draws were already skipped. Both `RB_RHI_ReStirDIApply` and `RB_RHI_RtgiApply` (same latent
issue) now set the full-view viewport + scissor and restore `backEnd.currentScissor` before drawing.

**Perforated casters (2026-10-08, after the user noticed grates stopped casting under the apply):** the DI
visibility ray now takes the forward inline ray's grate-aware route (`rt_perforated.glsl`, `r_rtPerforatedShadows`
+ `r_shadowMapPerforated`). The forward path decides per LIGHT whether a grate sits in its volume; DI's pixel
picks its own light, so `RB_RHI_GatherSceneLights` also tracks the union bounds of the included lights' cull
spheres and the DI pass gathers every grate box inside it (up to 64, per-frame slot, `u_particleRadius.zw` +
count in `u_windowCoord.x`, strength `.y`, geo table `u_particleRadius.xy`). A segment through none of the boxes
keeps the force-opaque 0xFD ray; one that can meet a grate traces 0xFF with candidates alpha-tested against the
coverage texture and dims to `1 - strength` behind a solid texel. `rt_perforated.glsl`'s `u_rtTextures[]` is now
guarded by the same macro as `rt_light_tex.glsl` so both include together.

**Debug views (2026-10-08):** `r_rtReStirDIDebug` is now the RTGI-style 0/1/2 (off / RAW 1-spp gather /
NRD-denoised) with the matching ImGui dropdown. The raw view is also the checkerboard proof: with
`r_rtReStirDICheckerboard` on, the traced half sits packed in the buffer's LEFT half. Note the checkerboard only
halves the GATHER's fragments + rays (`restir.di.gather` in `r_vkGpuTime 2`); the NRD denoise, the guides pass,
the confidence pass and the apply run at full cost regardless, so the frame-time saving is the gather's half.

**Fast-changing lights stay forward (2026-10-08, after the user hit flashlight / muzzle-flash lag):** DI is
NRD-accumulated over `r_rtReStirDIHistory` frames, so a light that moves or switches every frame trails. Behind
`r_rtReStirDIDynamicForward` (default 1) the gather classifies each lightDef per frame (`sceneLightTrack_t`): a
first-person weapon light (`allowLightInViewID`), a pose that moved recently (`RB_RHI_LightIsMoving`, the
r_rtMovingLights tracker, 30-view hysteresis), a luminance change > 10 % vs last frame, or a light absent from
last frame's buffer (a flash, a light switched on) — the last two with a 16-frame hysteresis — is flagged
`DUDE_LIGHT_DIFORWARD` (bit 5). It stays in the buffer for volumetrics / GI / reflections, but the DI candidate
loop skips it and the membership stamp is withheld, so the forward loop keeps lighting it with the inline ray
at zero lag. Known seam: a light that stops changing hands over to DI after the hysteresis and its DI history
starts empty, so it ramps in over the NRD history length (the confidence pass shortens that); a light that
never settles (a strobe) simply stays forward.

**Combined DI + GI in one buffer (2026-10-08, user chose "path 2"):** with `r_rtgi` on, `r_rtReStirDICombineGI`
(default 1) has the DI gather also trace the one-bounce GI ray at the same pixel through the new shared include
`rtgi_bounce.glsl` (the bounce lifted verbatim out of `rtgi_ray.frag`, which now calls the same function - its
output is unchanged). The bounce is finished as `rtgi_apply.frag` would (receiver albedo × `r_rtgiIntensity` ×
the fog fade from the shared `RB_RHI_RtgiFogCoefs`), demodulated by the same material factor as the direct term
and summed into the DI radiance, so ONE NRD denoise and ONE apply serve direct + indirect at about the cost of
DI alone. The forward light scale now rides the gather (`u_shadowFalloffS.x`), not the apply. Compromises: DI's
resolution / checkerboard / history / NRD knobs shape the bounce too; the direct term's light distance drives
the blur radius for both; AO-on-direct applies to the bounce as well; `r_rtgiDebug` shows nothing while DI is
on (use the DI debug views, which now include the bounce). The GI's own pass still stands down under DI.

**Fast-light rule refinements (2026-10-08, user saw FPS fall just from looking around a monitor-filled room):**
DUDE's own fill lights (`r_emissiveSurfaces` screens, `r_itemGlow`) are exempt from the fast-light rules via the new
`idRenderLightLocal::dudeFillLight` mark (stamped each frame in tr_light.cpp): their tint follows an animated screen
and the per-view budget churns them, so the luminance / appeared rules pushed dozens onto the forward path. The
luminance step is now 20 % (was 10 %) so subtle authored flicker tables stay in DI. `r_shadowMapDebug 1` prints
"N of M lit lights DI-covered" per frame - the number to watch.

**Material parity (2026-10-08, closes two v1 approximations):** the albedo prepass now writes
`diffuseMap × diffuse-stage colour × vertex-colour term` (the stage's colour registers in `u_diffuseModifier`, the
`SVC_*` mode via `u_vertexColorModulate/Add` and a new `var_Color` varying threaded through gbuffer.vert/tesc/tese),
so the DI shade and the GI receiver albedo match what interaction.frag multiplies the light by. The material
attachment's spare `.a` carries the baked occlusion map (`RB_RHI_SurfaceOcclusionImage`, factored out of the
forward fill, bound on unit 10 of the albedo prepass), and the DI apply scales by
`mix(1, map, r_occlusionMapScale × r_occlusionMapDirect)` like the forward direct diffuse.

**Cookie mip level (2026-10-08, user A/B: DI brighter than forward on a white ceramic floor, closed by
`r_rtLightTextures 0`):** the forward draw's `textureProj` picks mips from screen derivatives and Doom's cookies
fade to a black border, so blurred mips read dimmer at distance / grazing angles; DI sampled mip 0. `dudeLightAttenTex`
now takes the pixel's world footprint and derives the LOD analytically from the projection planes
(`grad(s) = (S.xyz - s·Q.xyz)/q`, footprint in texels, log2), fed by `u_shadowFalloffS.y = 2/(proj00·viewW)` at
full res; the GI bounce keeps mip 0 (a ray hit has no screen footprint). Isotropic like anisotropic filtering's LOD.

**SH denoise mode (2026-10-08, user: bump detail vanished in dim DI light, blur knobs irrelevant → the reduced-res /
checkerboard gather + the denoiser's spatial blur erase N·L detail):** `r_rtReStirDISH` (default 1) switches the DI
denoise to NRD `REBLUR_DIFFUSE_SH`. The gather's att0 keeps the SH0 layout (YCoCg + hitdist); a 4th attachment carries
SH1 = direction × luminance (the chosen light's direction, plus the bounce's sample direction in combined mode), and the
gather leaves OUT its own normal's cosine (floored at 0.05 so the GGX 1/N·L term stays bounded; the cosine-sampled
bounce is divided by its implicit cosine likewise). The backend gets a 4th denoiser id (`NRD_ID_REBLUR_GI_SH`), a second
output image (`GetRtgiOutputSh1Image`) and `CreateRenderTargetRtgiShMrt`. The apply resolves per pixel against the
FULL-res bump normal: `L = SH1/|SH1|`, concentration `k = |SH1|/Y`, factor `mix(0.5 + 0.5·N·L, max(N·L,0), k)` -
exact cosine for a concentrated direction, wrapped half-cosine as the light spreads. The debug views show SH0 only
(no cosine → flatter / brighter than the lit scene; use the lit scene for judgement).

**Handover crossfade (2026-10-09, user: lights "blink in and out", revealing fried pixels until history catches up):**
a light flipping between the forward path and DI used to cut over instantly, but DI's denoiser has no history for a
light that was forward (it ramps in over the accumulation) and still holds one that just went forward (it decays
out) - so the light dipped / doubled and the ramp-in showed under-accumulated noise. The gather now records each
flip (`sceneLightTrack_t::transitionFrame/toDI`, classified once per frame) and `RB_RHI_SceneLightForwardWeight`
keeps a weighted FORWARD draw of the light's opaque world surfaces during the window: `exp(-t/history)` after a
forward→DI flip, `1 - exp(-t/history)` after DI→forward, ~3 time constants long, applied to the diffuse/specular
modifiers in `RB_RHI_DrawInteraction` (weapon, depth-hacked models and translucents unaffected). The sum of the fading
forward draw and the ramping DI stays at full brightness, and the noisy early DI frames sit under a clean draw.

**Exhaustive RIS (2026-10-09, user: the skyway "sun" through the windows reads noticeably dimmer than forward):**
the gather drew `M = 32` candidates uniformly (with replacement) from the per-view light buffer, which in a lit
corridor holds ~200 lights (comm1 Skyway: 120 lights within 1400 units, the orange `lights/xray02` outdoor light
of radius 1600×1750×1250 among them). The dominant light was then a candidate at only `1-(1-1/200)^32 ≈ 15 %` of the
pixels, where RIS's `W = wSum/(M·pHat)` made it a ~6× spike; the expectation is right, but NRD's anti-firefly and
history clamping clip sparse spikes, so the sun lost energy and read dim (a few-light room, where 32 draws cover
every light several times, was "extremely close"). `r_rtReStirDICandidates` now defaults to **0 = exhaustive**:
the loop enumerates every light in the buffer once (RIS weight `pHat`, `W = wSum/pHat(y)`), so the only remaining
randomness is the pick among the lights that really reach the pixel; the 16 B sphere cull and the exact box
pre-reject in `dudeLightAttenTex` keep it near the cost of the bounce's light loop (`rtgi_bounce.glsl`, which has
always enumerated). `M > 0` keeps the stochastic draw for scenes beyond the 256-light buffer. Note: the cvar is
archived, so a config saved with the old `32` must be reset once (`r_rtReStirDICandidates 0`). Also note the
xray02 light is table-driven (0.7..1.0 over a 10 s cycle), so two screenshots can differ by phase alone.

**Look-away fade — ACCEPTED LIMITATION (2026-10-09):** after looking away from the sun's window-frame shadows and
back, they wash out and fade back in over the history length: the returning pixels have no NRD history and
ReBLUR treats a fresh pixel as dirty (5-frame history fix at a 14 px stride, the v4.17 post-reset convergence boost,
the lit side blurring at full radius since its hit distance is the light distance). Four DI-only ReBLUR overrides
(`minHitDistanceWeight`, history-fix stride, convergence b/s) were built, tested and REVERTED by the user's decision
("it won't get better"): the one-buffer direct+bounce denoise cannot favour the clean direct term without
grainying the bounce. Not to be re-chased with knobs; the structural options remain a raw-direct blend in the
apply keyed on NRD's history length, or a separate direct denoise.

**Known v1 approximations (deliberate, listed for the test):** the SPECULAR stage colour assumed 1 (the diffuse
stage colour is in the albedo now); the legacy non-PBR specular LUT not replicated (`r_pbr 0` = Lambert); no
Toksvig widening (G-buffer normal is unit); the organic spec-fallback (flesh without a spec stage) not replicated;
AO and the baked occlusion map apply to diffuse+spec together; `noSpecular` light parm ignored; multi-stage light
cookies (Stage G) take the first live stage. The RTGI/RTAO auto-claim still stands
them down while DI is on (DI's own NRD instance remains the fix).

**Defaults re-baselined 2026-10-12 from the user's saved config** (`~/.config/dude/base/dude.cfg`): BlurRadius 18, EdgeStop 6,
History 8, HistoryFix 6, GradientPower 4, GradientHistory 0, GradientDeadzone 0.25, Confidence 0.5 (the dormant luminance
fallback), RelaxAtrous 3, RelaxPrepass 8. `r_rtReStirDI` / `r_rtReStirDIApply` stay preset-driven (`dude_rtx 2`).

### Moving volumetric lights "20 % FPS" (2026-10-12) - resolved to coverage, not fog
`r_vkGpuTime 3` with moving volumetric lights in view: `volumetrics` 2.35 ms, in line with still scenes elsewhere - the fog was
never the cost. `r_shadowMapDebug 1` showed **7 of 21 lit lights DI-covered: fast 4, not-in-buffer 10**; the 14 forward lights
carried ray shadows + the RT blur (`light.shadow` 0.7-0.9 ms, `light.interact` 0.6 ms). Two causes: (1) the scene-light gather's
2048-unit REACH cull (written for reflections / haze) dropped far lights that the frontend still lists as lit - fixed: a light in
the view's lit list always enters the buffer, the reach cull applies to off-screen lights only; (2) "fast 4" with weapon 0 is
only possible in `r_rtReStirDIDynamicForward 1` (moving lights forward) - the user's console had it at 1 from the weapon hunt.
(3) The real exclusion, from the new per-reason breakdown on the coverage line: **capacity** - the 256-entry buffer was filled in
lightDef order from every light within reach and was full before some on-screen lights got their turn ("capacity 7" of 18 lit).
The gather now runs two passes: the lights lit in this view first, then the off-screen ones (reflections, haze) until full.

### Bugs found and fixed 2026-10-12 (branch `fix/rtx-restir-di-gradient-bounds-weapon-mask`)
- **VK device loss with DI on (new):** the R1 gradient dereferenced last frame's light rows and the lightDef -> row table
  through indices stored a frame earlier; after a map change the ring slot still holds the OLD map's rows, so a stale index
  read past the written table and the light buffer (a GPU page fault = device lost). Every stored index is now bounded by
  the slot's light count + the table capacity; `r_rtReStirDIGradient` / `GradientStratum` / `r_rtReStirDISH` joined the
  gfx-config signature (they (re)create targets).
- **Weapon dark under `r_rtReStirDIApply` (only self-lit stages visible, lit only inside some lights' rects):** the
  interaction draws take their shadow map / shadow cube (depth-compare samplers) from units 7 / 8 of the shared unit table
  (`RB_RHI_VkTextures`), and an UNSHADOWED light never rebinds them - safe for years because only shadow passes ever wrote
  those units. The R1 gradient bound a colour velocity image on 7 and a colour side copy on 8; every DI-covered light is
  unshadowed, so its weapon draws carried colour images on depth-compare samplers = an invalid descriptor that NVIDIA
  renders as nothing (the lit stages go through the interaction shader, the self-lit stages do not). The "lit rectangles"
  were lights that followed a shadowed light and inherited valid images. Gradient inputs now ride units 10 / 11, which every
  interaction draw rebinds. Hunt lessons kept: the counters showed the draws WERE issued and the deferred depth WAS written,
  which ruled out the prepass split; `r_rtReStirDIWorldOnlyPrepass 0` "fixing" it was coincidence of unit state, not a cure.
  **RULE:** no fullscreen pass may bind a colour image on units 7 / 8 / 12 (the shadow samplers); use 0-6, 9-11.
- **Prepass jitter correction** (found on the way, real): with the two-phase prepass the motion-vector jitter correction
  was recomputed per CALL, so the deferred weapon phase got zero and FSR2 saw a jitter-sized error on the weapon's
  velocity; now computed once per frame. Any per-frame state in `RB_RHI_NormalPrepass` must be frame-guarded.
- Diagnostics added during the hunt (`r_rtReStirDIForwardRay`, `ForwardShadowWork`, `WorldOnlyPrepass`, draw counters, a
  state-hygiene barrier) were removed once the fix was confirmed. The per-reason coverage breakdown on the
  `r_shadowMapDebug 1` line stays (R1b's "N of M covered" diagnostic).

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

## Review against public ReSTIR (2026-10-11)

Scope: the shipped gather / apply / confidence / NRD wiring above, read against the public sources that describe
ReSTIR DI as actually shipped in first-person games - the RTXDI SDK + runtime library, the RTX Remix runtime
(Portal RTX / Half-Life 2 RTX, `dxvk-remix`), Quake II RTX (A-SVGF), NVIDIA NRD v4.18 (README, headers, UPDATE.md),
Bitterli 2020, Wyman & Panteleev HPG 2021, the SIGGRAPH 2023 ReSTIR course (incl. the Cyberpunk 2077 chapter),
ReSTIR GI 2021, GRIS 2022, Area ReSTIR 2024, Kettunen 2023. Goal of the review: **minimise ghosting and lag while
keeping fine detail** (hard contact shadows, grates, bump N·L). Sources are listed at the end of the section.

### Verdict

The shipped design is **not ReSTIR in the published sense**: it is exhaustive RIS (one light + one shadow ray) feeding a
radiance-accumulating denoiser. Every lag symptom fought so far - the flashlight trail, the handover blink, the
look-away fade - is the predictable result of that split, not of the sampling. Published ReSTIR carries only the
light **selection** across frames: the reservoir stores a light index + weight, never radiance; each frame the stored
light is re-scored with the CURRENT light parameters at the CURRENT pixel and the winner gets a fresh shadow ray
(Bitterli §3.2; RTXDI `TemporalResampling.hlsli`: `weightAtCurrent = RAB_GetLightSampleTargetPdfForSurface(...)`; Remix
fork identical). A light that moves, dims or switches off loses its weight the same frame, so selection reuse is
lag-free by construction, and the denoiser only smooths a signal whose mean is already right.

The pivot removed selection reuse and kept radiance accumulation (ReBLUR). The fast-light forward path, the 16-frame
hysteresis, the handover crossfade and the ramp-in seam are all compensations for denoiser lag. **No public engine
special-cases player lights this way**: RTXDI and Remix have no view-model / muzzle-flash handling at all (grep of both
repos); Quake II RTX only damps the weapon's *specular* gradient (`flt_grad_weapon 0.25`).

The trails the retired reservoir showed match the textbook failure modes, each with a public fix:

| Failure mode we hit | Public handling |
|---|---|
| stale visibility reused with the reservoir (shadow trails) | Wyman 2021 §7: only the CANDIDATE's visibility feeds resampling, the winner is traced fresh; RTXDI bounds reused visibility to `finalVisibilityMaxAge 4` frames / `finalVisibilityMaxDistance 16` px |
| a light that vanished kept its reservoir | RTXDI/Remix: unmapped light -> `weightSum = 0`, M kept (`RAB_TranslateLightIndex < 0`), per-frame light-index mapping buffer |
| unbounded / absolute history | cap previous M at `maxHistoryLength * curSample.M`: RTXDI 20, **Remix ships 4 for DI**, course notes 5-30 |
| reuse across silhouettes | normal dot >= 0.5 + 10 % relative depth (RTXDI); Remix 0.8 + view-angle-scaled depth; 9-try jittered search, else spatial boost |
| everything sourced from the previous frame | fused reuse "delays convergence by one frame" (HPG 2021); RTXDI: also more biased |

What IS right and stays: exhaustive RIS at <= 256 lights (strictly less variance than RTXDI's tiled initial sampling;
the weight derivation `W = wSum / pHat(y)` is correct), material demodulation (matches NRD's `NRD_MaterialFactors`
guidance), hit distance = occluder distance when shadowed / light distance when lit (NRD README: "For direct lighting a
distance to an occluder or a light source is needed", "hitT must approach 0 at contact points"), SH mode for the
reduced-res output (NRD: "a must-have when NRD produces a lower-resolution output").

### Findings, ranked by effect on lag + detail

**F1 - the confidence pass measures noise, not lighting change.** `rtgi_confidence.frag` compares a 3x3 mean of the
noisy 1-spp gather with last frame's DENOISED output, so a stable surface reads as "changed" and history starves -
that is why it fries on DI. Every public system computes a temporal **gradient** instead: re-shade the SAME sample in
the other frame so the only difference is the lighting change. Q2RTX replays the previous RNG seed
(`asvgf_gradient_reproject.comp`, picks the BRIGHTEST pixel per 3x3 stratum: "helps prevent bright trails when the light
has moved"). RTXDI (`ComputeGradients.hlsl`, `Doc/Confidence.md`) and Remix (`rtxdi_compute_gradients.comp.slang`) store
the previous pixel's CHOSEN LIGHT and re-shade it at the reprojected surface under current lights, one pixel per 3x3
stratum ("we can reuse the reservoirs instead of random numbers"), filter with a 4-pass 3x3 A-trous, convert to
confidence as `saturate(1 - g / (|L| + darknessBias))^sensitivity` (sensitivity 8), smooth temporally in a non-linear
space (RTXDI `pow 0.25`) or ONLY while confidence rises (Remix: "eliminate the ghost issue cause by temporal
confidence"), and floor it (`minimumConfidence 0.1`, hitT-scaled). An unmapped light counts as a 100 % change - this
replaces our "appeared" rule. NRD README: confidence inputs are "the best way to mitigate temporal lags", "less than 5%
of the frame time", "An application should not rely solely on the anti-lag provided by REBLUR/RELAX"; v4.17+: the
confidence texture may be provided at lower resolution. Our gather makes this cheap: the seed is pixel + frame, the
light ring (`RT_REFL_LIGHT_SLOTS`) keeps previous-frame rows, the only missing piece is a stable light identity across
the per-frame distance sort (`RtLight.pad0` is free; RTXDI's `lightIndexMappingBuffer` is the recipe).

**F2 - ReBLUR is the denoiser NRD does not recommend for this signal.** NRD README: "RELAX - A-trous based denoiser, has
been designed for RTXDI", "The Sweet Home of RELAX is RTXDI sample". Remix ships `RELAX_DIFFUSE_SPECULAR` for direct light
with `diffuseMaxAccumulatedFrameNum 26`, **`diffuseMaxFastAccumulatedFrameNum 1`** ("to reduce specular trail"),
`diffusePrepassBlurRadius 0`, `confidenceDrivenRelaxationMultiplier 0.7 / LuminanceEdgeStoppingRelaxation 1.4 /
NormalEdgeStoppingRelaxation 0.6`; the RTXDI sample sets `diffusePhiLuminance 1.0`,
`spatialVarianceEstimationHistoryThreshold 1`, `enableAntiFirefly true`. RELAX "uses hit distances only in the Pre-Pass
and for specular tracking", so the hit-distance-driven blur radius that widened our grate shadows does not apply; its
A-trous luminance/normal/depth edge-stopping is what keeps hard shadow edges. The ReLAX SPIR-V is already vendored
(`neo/libs/nrd/Shaders/RELAX_*.cs.spirv.h`); adding `RELAX_DIFFUSE` / `RELAX_DIFFUSE_SH` to the instance is wiring. Input
differences (verified in `NRD.hlsli`): RELAX takes LINEAR radiance + RAW hit distance (`RELAX_FrontEnd_PackRadianceAndHitDist
= float4(radiance, hitDist)`), REBLUR takes YCoCg + normalized; RELAX output `.w` is the diffuse history length (v4.16).
Cost per NRD's table (1440p, RTX 4080): RELAX_DIFFUSE_SPECULAR 3.25 ms vs REBLUR 2.55 ms; anti-firefly +7-10 % on RELAX.

**F3 - no spatial reuse, so a disoccluded pixel is ONE sample for many frames.** This is the accepted "look-away fade".
Wyman 2021 (60 Hz strafe): temporal-only disocclusion noise "persist[s] for over 20 frames"; +1 spatial tap -> "3 or 4
frames"; +5 spatial -> "2 frames"; one tap also "stochastically dithers out temporal correlations". RTXDI:
`numDisocclusionBoostSamples 8` (normal `numSamples 1`, radius 32); Remix: 4 taps for the first `disocclusionFrames 8`.
Kettunen 2023 names it "variance lag around disocclusions". ReBLUR's history fix cannot substitute: it has no new
samples to reconstruct from (the knobs tried and reverted on 2026-10-09 confirm this). The literature fix is a
1-tap spatial RIS, boosted only where the denoiser reports short history, so the extra cost lands on disoccluded pixels.

**F4 - ReBLUR settings left at defaults that add lag or blur** (see the table in Plan R0/R4): fast history 6 (Remix
direct: 1), `fastHistoryClampingSigmaScale` 2.0 (NRD sample: 1.5, "works well even for dirty signals"),
`maxStabilizedFrameNum` default 63 (clamped to history; the Cyberpunk chapter: with ReSTIR as the pre-blur make the
denoiser "more conservative: Shorter history length; Smaller radius of the final blur", "skipping Pre-Blur"), prepass 36 px
(Remix direct 0), history in frames (NRD: accumulate in seconds via `GetMaxAccumulatedFrameNum` with a 0.2 s low-pass
FPS - "minimizes lag at low FPS and maximizes IQ at high FPS"), 2D motion vectors (NRD README: 2.5D recommended, "2D MVs
can lead to massive history reset on moving objects" - directly relevant to NPCs and the weapon), ReBLUR antilag at
defaults (NRD: "Initial integration should be done with disabled antilag"; validate with the `OUT_VALIDATION` overlay
so "diff frames" stays "in the violet zone").

**F5 - the residual noise that forces the heavy blur.** pHat is the UNSHADOWED luminance. Where the brightest reaching
light is occluded, most frames pick it and output 0; the occasional pick of a dimmer visible light arrives as a spike
with `W = wSum / pHat` that anti-firefly then clips - the same clipping that dimmed the skyway sun, now acting inside
every multi-light shadow (a darkening bias plus noise). Course notes §9: "Consider always using visibility in your
target functions and MIS weights until you have validated your code". Spatial reuse with Wyman's decoupled visibility
(F3) is the standard answer; it is the same pass.

**F6 - smaller gaps.**
- NRD's normal encoding we use (`R10_G10_B10_A2_UNORM`, materialID in `.w`) is written as 0 everywhere; NRD documents
  `historyFixAlternatePixelStrideMaterialID` for "moving objects (like protagonist's weapon) constantly getting a history
  reset", and `minMaterialForDiffuse` keeps the denoiser from mixing tagged pixels with the world. We shade the weapon
  pixels into the field (unmasked) but never tag them.
- Neither DI nor RTGI resets NRD on the camera-cut flag the temporal camera already tracks
  (`rhiTemporalCam.historyReset`); `da.reset` fires only on the first DI frame. Indiana Jones (REAC 2025): "a full reset
  of temporal buffers and then present the last frame a few extra times".
- The SH resolve in `restir_di_apply.frag` is a custom cosine mix; NRD ships `NRD_SG_ResolveDiffuse` + `NRD_SG_ReJitter`
  (the Jacobian from the 4-neighbour normals, clamped `[1/2; 2]`) for exactly the micro-detail recovery wanted, and notes
  the re-jitter "can also be used with RESTIR produced sampling without involving SH denoisers" given the light direction.
- Combined DI+GI hitT: NRD README recipe lerps direct/indirect hit distance by the direct contribution (`maxContribution
  0.5`) "to get sharper shadows"; we always pass the direct term's, which is the sharper choice already. NRD also says
  "always ignore 0 hitT produced by RESTIR in disocclusions" - a pixel with no reaching light but a bounce writes 0.
- Checkerboard: NRD marks `CheckerboardMode::OFF // RECOMMENDED` in favour of probabilistic sampling; Area ReSTIR 2024
  shows integer backprojection itself invalidates reuse on high-frequency normals. Not urgent; noted for R5.

### Sources
RTXDI SDK + library (`NVIDIA-RTX/RTXDI`, `NVIDIA-RTX/RTXDI-Library`: `Include/Rtxdi/DI/TemporalResampling.hlsli`,
`Source/ReSTIRDI.cpp`, `Doc/Confidence.md`, `Doc/NoiseAndBias.md`, sample `ComputeGradients.hlsl` /
`FilterGradientsPass.hlsl` / `ConfidencePass.hlsl`); RTX Remix (`NVIDIAGameWorks/dxvk-remix`: `RtxOptions.md`,
`rtx_rtxdi_rayquery.h`, `rtx_nrd_settings.h`, `rtxdi_compute_gradients.comp.slang`, `ConfidenceFunctions.slangh`);
Quake II RTX (`asvgf_gradient_*.comp`, `asvgf_temporal.comp`, `global_ubo.h`); NRD v4.18 (`README.md`, `Include/NRDSettings.h`,
`Shaders/NRD.hlsli`, `UPDATE.md`); NRD-Sample; Bitterli et al. 2020; Wyman & Panteleev HPG 2021; SIGGRAPH 2023 course
notes + Cyberpunk 2077 chapter (https://intro-to-restir.cwyman.org/); Ouyang et al. 2021 (ReSTIR GI); Lin et al. 2022
(GRIS); Zhang et al. 2024 (Area ReSTIR); Kettunen et al. 2023; MachineGames REAC 2025 slides. Production talks for
Cyberpunk / Alan Wake 2 / Doom: The Dark Ages / Portal RTX are account-walled; only their public abstracts were used.

## Plan: lag + ghosting reduction (R0-R6) - authored 2026-10-11, NOT started

Each stage = its own branch (`feat/rtx-restir-di-r<N>-<slug>`), its own default-off cvar(s), the forward path and
`r_rtReStirDI 0` bit-identical, VK + `SupportsRayQuery()` only, new shaders need the CMake re-configure. Measure every
stage with `r_vkGpuTime 2` (its own `restir.di.*` scope) and A/B with the DI debug views. A stage's knobs that lose
the A/B are removed at R6 (trim-debug-cvars rule). Order: R0 -> R1 -> R2 -> R3 -> R4 -> R5 -> R6; R2 is independent of
R1 and may run in parallel; R3 needs R1 (its disocclusion boost reads the history length R1's plumbing exposes).
**Amended 2026-10-12** after the cross-consumer NRD investigation ([rtx-nrd.md](rtx-nrd.md), *Plan N0-N3*): every item
that lives in the shared `RtaoDenoise` path is built ONCE for RTAO / RTGI / DI (N0 with R0, N1 with R4); the DI-named
cvars below get RTAO / RTGI siblings at today's values. Amendments are marked **[A]**.

### R0 - diagnostics + zero-risk fixes (F4, F6) - small - **IMPLEMENTED 2026-10-12** (branch `feat/rtx-restir-di-r0-diagnostics`, USER TEST PENDING)
Shipped as planned with these notes: the cut reset is a RhiWorld helper (`RB_RHI_NrdHistoryReset`, the backend cannot see the
temporal camera) that runs the once-per-frame discontinuity classify ITSELF (factored into `RB_RHI_TemporalClassify`) so the
NRD passes - which run before the SSR / FSR2 consumers that used to classify first - see THIS frame's cut, not last frame's;
`RB_RHI_CinematicView()` is "a cinematic camera is the main view", not a cut, so rotation-only shot cuts are a follow-up
(an angle test next to `r_temporalResetDist`). The seconds-based history scales the FAST history with the main window
(keeps NRD's 5-7x ratio) and clamps stabilized to main. The validation / history-length views are DI-only for now
(`r_rtReStirDIDebug 3 / 4`); the args are generic, so RTGI / RTAO views are one line each when wanted. `returnHistoryLength`
is set for DI only (RTGI byte-identical). ImGui untouched (house rule). Cvars added - all at today's effective values:
`r_rtReStirDIHistoryMs` 0, `r_rtReStirDIFastHistory` 6, `r_rtReStirDIFastClamp` 2.0, `r_rtReStirDIStabilize` 63,
`r_rtReStirDIAntilag` 1, `r_rtaoHistoryMs` 0, `r_rtaoFastClamp` 2.0, `r_rtaoHistoryFix` 3, `r_rtgiHistoryMs` 0,
`r_rtgiFastClamp` 2.0, `r_nrdSigma` 0 (in the gfx-config signature: it rebuilds the NRD instance).
**First user test (2026-10-12):** cut reset verified (one black frame in view 4). View 3: every guide viewport healthy, only
"diff frames" shows trails - i.e. the inputs are right and the trails are history RESETS: the wake behind movers / the weapon
(true disocclusion -> F3 / R3) and, if the mover's own body stays dark, false resets (-> R4's 2.5D MVs + weapon material ID).
Added `r_rtReStirDIDisocclusion` (0.01 = NRD default, range 0.005..0.05) to separate the two: trails that shrink at 0.02 are
false resets. **Result:** the wake behind a mover is unchanged at 0.02 / 0.04 -> TRUE disocclusion, exactly F3; the
refill time is the problem, not the reset. R3 (spatial reuse + disocclusion boost) is confirmed as the structural fix; the
knob stays at 0.01 (a diagnostic, candidate for the R6 trim).
**R0 acceptance (user, 2026-10-12):** lit scene at defaults = same brightness, frame-to-frame variation only (a 1-spp
temporally accumulated signal never repeats run-to-run); `r_vkGpuTime 2` unchanged; cut reset + views 3/4 work. **RTAO A/B
`r_rtaoFastClamp 1.1` + `r_rtaoHistoryFix 2` LOST:** a little ghosting at a walking NPC's feet plus frying pixels around it
(the tighter clamp + shorter fix starve the 45-frame history on the disoccluded ring) -> defaults stay 2.0 / 3; the knobs
remain for the seconds-based-history retest (N0) and are R6 trim candidates. R0 MERGED to master.
**Test:** (a) `r_rtReStirDI 1` defaults -> lit scene unchanged vs master; (b) teleport (`noclip` + a long jump, or a map's
teleporter) with `r_rtReStirDIDebug 4` -> the ramp goes black for one frame everywhere, then refills; (c) `r_rtReStirDIDebug 3`
-> NRD's overlay renders (coloured accumulated-frames field; NRD: keep it "in the violet zone" under stable lighting);
(d) RTAO A/B `r_rtaoFastClamp 1.1` + `r_rtaoHistoryFix 2` on an NPC walking past a wall: AO trail shorter, no new sparkle;
(e) `r_rtReStirDIHistoryMs 266` at a 60 Hz cap then uncapped: the same look at both rates; (f) `r_vkGpuTime 2` unchanged.
Files: `RhiWorld.cpp` (`RB_RHI_ReStirDIPass`, `RB_RHI_RtgiPass`), `VulkanBackend.cpp` (`RtaoDenoise`, `RtaoDenoiseArgs`),
`Dhewm3SettingsMenu.cpp` (ReSTIR group), `restir_di_debug.frag`.
1. **Camera-cut reset** **[A: shared]**: `a.reset || rhiTemporalCam.historyReset` INSIDE `RtaoDenoise`, so RTAO, RTGI and
   DI all get it (none has it today; the FSR2 / eye-adaptation passes already key off the flag). Optional follow-up: hold the apply's previous output for 2 frames after a cut (Indiana recipe).
2. **NRD validation view**: `r_rtReStirDIDebug 3` = NRD `OUT_VALIDATION` (set `cs.enableValidation`, bind the output,
   blit). Lets us SEE accumulated frames / history resets / antilag per pixel instead of guessing.
3. **History-length view**: ReBLUR `returnHistoryLengthInsteadOfOcclusion = true` (we never consume the `.w` AO) -
   `r_rtReStirDIDebug 4` shows it; R3 consumes it.
4. **Expose the lag knobs as cvars** (defaults = today's behaviour, so no visual change until tuned):
   `r_rtReStirDIFastHistory` (6), `r_rtReStirDIFastClamp` (2.0, `fastHistoryClampingSigmaScale`),
   `r_rtReStirDIStabilize` (63, `maxStabilizedFrameNum`; 0 = off), `r_rtReStirDIAntilag` (1; 0 = push both
   `ReblurAntilagSettings` to inert for the A/B NRD asks for). **[A]** The `giMode`-only overrides in `RtaoDenoiseArgs`
   become generic fields (`fastFrames`, `fastClampSigma`, `stabilizedFrames`, `historyFixFrameNum`) so RTAO / RTGI set them
   too: `r_rtaoFastClamp` (2.0; NRD header: "1.1 is a safe value for occlusion denoising") + `r_rtaoHistoryFix` (3; NRD-Sample
   occlusion mode: 2) are the RTAO A/B for AO trails under moving occluders; `r_rtgiFastClamp` (2.0; NRD-Sample: 1.5).
6. **[A] SIGMA_SHADOW out of the instance**: never dispatched since H3, yet `NrdCreate` builds its pipelines and the pools
   carry its share on every (re)create (incl. the idle-rebuild on a settings change). Drop it from the table behind
   `r_nrdSigma` (0); the per-light penumbra experiment in rtx-nrd.md N3 re-adds it if ever tried.
5. **Seconds-based history**: `r_rtReStirDIHistory` stays the user knob but gains a companion
   `r_rtReStirDIHistoryMs` (0 = frames as today; >0 = `nrd::GetMaxAccumulatedFrameNum(ms, lpfFps)` with a 0.2 s
   low-pass on the frame time, applied to main / fast / stabilized together). **[A]** Same helper and `*HistoryMs`
   companions for `r_rtaoHistory` (45 frames = 0.31 s @144 Hz but 0.75 s @60 Hz) and `r_rtgiHistory`.
Acceptance: byte-identical output at defaults except after a cut (no ghost of the previous shot); the validation view
renders; `r_vkGpuTime 2` unchanged.

### R1 - gradient confidence (F1) - the linchpin, medium - **USER-VALIDATED + MERGED 2026-10-12** (`r_rtReStirDIGradient` default 1)
Shipped as planned, with these notes. **Light identity:** `RtLight.pad0 = lightDef + 1` and a per-frame `lightDef -> row + 1`
table (`s_diLightMapBuf`, 4096 defs, ring slot per frame) built after the sort; the ring also records each slot's light-buffer
address + frame so the DI pass can name LAST frame's buffer. **Side data without a ping-pong:** the gather MRT is now the
4-attachment `CreateRenderTargetReStirDIMrt` in both SH modes, att1 widened to RG16F ( viewZ, fp16 gradient luminance ) and
att3.w = the chosen light's row + 1; the gradient pass runs BEFORE this frame's gather, so last frame's side data is still
in the MRT (the RT target limit is 4 attachments, hence the packing). The stored luminance is the chosen light's shaded,
demodulated, VISIBLE contribution at the light scale (`diGradientLuminance`), without the RIS weight W: W multiplies both
frames alike and cancels in the normalized gradient, so it need not be stored. **One shading implementation:** the surface
reconstruction, material response, per-light shade and visibility ray moved verbatim into `restir_di_shade.glsl`, used by
both the gather and the gradient pass (any drift would read as a lighting change). **Validity test:** normal dot >= 0.5
and the current point's expected previous view z (through last frame's world->view, `u_prevMvpMatrix`) within 10 % of the
stored one. **Checkerboard:** the stratum only considers cells last frame traced (parity + packed column). **A light that
left DI** (switched off, culled, moved to the forward path) maps to no row -> `L_now = 0` -> a 100 % change. **Confidence**
runs at the gather res (bilinear from the stratum res), rise-only smoothing, floor `max(cvar, historyFix / history)`,
dither 0.5 / history. **Not done:** the optional combined-mode bounce gradient (`r_rtReStirDIGradientBounce`), R1b.
**First user test (2026-10-12):** view 6 (the gradient) was right - black when static, the beam's footprint under the
flashlight, nothing on a walking NPC - but view 5 (the confidence) fried as grey blobs even when static, held a half-second
drop after a blink and trailed behind movers. Three causes, fixed the same day: (1) the re-shade landed on the CURRENT pixel
centre, which under the FSR2 jitter + reprojection is a sub-pixel-different world point than the stored one (bump texel,
cookie texel, shadow edge) - a few-percent mismatch that the power-8 confidence turns grey; now a point that moved less
than `r_rtReStirDIGradientStatic` (2.0 full-res px) is re-shaded at its EXACT stored position / normal / previous camera
(last frame's inverse view rides `u_projectionMatrix`), so static world + static light compares bit-identical (RTXDI
shades the same world point too); movers keep the current surface (their old position would be self-shadowed in the
current TLAS). (2) Doom's authored flicker tables pulse many lights 5-15 % per frame - a genuine per-light change that
reset history every frame; `r_rtReStirDIGradientDeadzone` (0.1) ignores relative changes below 10 % (noise-free signal,
so a real threshold; a light switching off is still 100 % of its own value). (3) the rise-only hold (RTXDI/Remix 8
frames) exists so REUSED reservoirs can stabilize; our gather is fresh every frame, so `r_rtReStirDIGradientHistory`
defaults to 2 - the half-second drop and the mover trail in view 5 were this hold. Cost measured 0.351 ms grad + 0.043
filter (+ conf) at 1440p x 0.75, above the estimate because the 1/9-res visibility ray dominates; `r_rtReStirDIGradientStratum`
(3) lets a 4 or 5 (NRD: 1/5 res suffices) cut it to ~0.2 / ~0.13 ms.
**Second user test (2026-10-12):** view 5 white when static except under blinking lights (correct: those are real changes);
dead zone 0.2 is the sweet spot (default). But the flashlight sweep showed its circle in view 6 and NO blob in view 5 - and the
circle was only the TRAILING edge: a gradient that re-shades last frame's choice detects a light LEAVING a surface, never one
ARRIVING (last frame's choice there was another, unchanged light). The leading edge is the bright-lag half of the flashlight
problem. Fixed the RTXDI way ("brightest of the current or previous sample"): the pass also finds the brightest light reaching
the stratum pixel NOW and shades it with its PREVIOUS-frame row (through last frame's table, `u_parallaxParms2.xy`) at the
stored previous surface; the brighter of the two samples decides. Costs one exhaustive unshadowed light loop per stratum
(~1/9 of the gather's), no extra ray.
**Third user test (2026-10-12):** flashlight now right in both views and in the lit scene, but view 5 missed the circle about
1 pull-out in 7. Cause: light identity was the lightDef INDEX, and `idRenderWorldLocal::AddLightDef` reuses a freed slot at
once - a weapon switch frees the old weapon light and allocates the flashlight into the same index, so last frame's table
mapped the new light onto the old light's row, the two shaded alike on the same wall, and the arrival read as "unchanged".
Fix: `idRenderLightLocal::dudeSerial` (a process-unique creation serial, stamped in AddLightDef) rides `RtLight.pad1`, and both
gradient directions require slot AND serial to match - otherwise the light is "absent" (100 % change). RTXDI keys its
mapping on stable light identity for the same reason.
**Accepted (user, 2026-10-12):** the circle shows every time; the lit scene follows the flashlight; static scenes stay white in
view 5 except under blinking lights (correct). Default flipped to 1. The old luminance confidence (`r_rtReStirDIConfidence`)
stays as the dormant fallback until the R6 trim. Next: R1b.
Cvars: `r_rtReStirDIGradient` 0 (the A/B switch), `r_rtReStirDIGradientPower` 8, `r_rtReStirDIGradientHistory` 8,
`r_rtReStirDIGradientFloor` 0.1, `r_rtReStirDIGradientBias` 0.001. Debug: `r_rtReStirDIDebug 5` = the confidence (grey),
`6` = the filtered gradient (red |change|, green magnitude). New GPU scopes `restir.di.grad / gradfilter / gradconf`.
**Test:** `r_rtReStirDI 1 r_rtReStirDIGradient 1`: (a) static scene, view 5 stays white (no false resets - the failure of
the old pass), view 6 black; (b) `r_rtReStirDIDynamicForward 0`, sweep the flashlight / fire: view 6 lights up exactly in
the beam's old + new footprint, view 5 darkens there for a few frames, and the lit scene trails by <= 2 frames (vs the
16-frame smear with the gradient off); (c) a light switching off (a door light, a destroyed lamp): its footprint goes dark
within the fast history, no black dip halo; (d) an NPC walking under a static light: view 6 stays black on its body
(geometry is NRD's job); (e) `r_vkGpuTime 2`: the three new scopes together well under 0.2 ms.
Files: NEW `restir_di_gradient.frag` + `restir_di_gradient_filter.frag` + `restir_di_confidence.frag`, `restir_di.frag`
(two extra outputs), `rt_light_eval.glsl` (`pad0` -> `lightDef`), `RhiWorld.cpp` (`RB_RHI_GatherSceneLights` mapping table,
`RB_RHI_ReStirDIPass` new passes), `VulkanBackend.cpp` (confidence at reduced res, v4.17+ allows it).
1. **Stable light identity**: `RtLight.pad0 = lightDef index + 1`; `RB_RHI_GatherSceneLights` also fills a per-frame
   `lightDef -> row` table (uint per lightDef, 0 = absent this frame; `numDefs` entries, same ring slot idiom). This is
   RTXDI's `lightIndexMappingBuffer`.
2. **Gather side-outputs** (new attachment, RG32UI or RGBA16F, ping-pong prev/curr at gather res): `(chosen lightDef+1,
   luminance of the SHADED visible contribution rad·W·vis, packed viewZ, packed normal)`. RTXDI (`u_RestirLuminance`)
   stores exactly this; the viewZ/normal are for the gradient's own geometric validity test since the gather MRT is
   overwritten each frame.
3. **Gradient pass** at `ceil(rw/3) x ceil(rh/3)`: for the 3x3 stratum, backproject each current pixel through the
   velocity, read prev `(light, lum, viewZ, N)`, accept on the RTXDI test (normal dot >= 0.5, relative depth <= 10 %),
   pick the BRIGHTEST accepted candidate (Q2RTX/RTXDI). Map its lightDef through the table: absent -> gradient = 100 %;
   else re-shade THAT light at the current surface with the gather's own per-light code path (textured attenuation,
   BRDF, demod, cosine) and ONE visibility ray against the current TLAS (NRD recipe: previous TLAS not needed),
   `L_now`; round both through f16 (RTXDI) and store `(|L_prev - L_now|, max(L_prev, L_now))`. **[A]** Checkerboard: the
   stored `(light, lum)` exists only for last frame's TRACED cells, so the stratum picks among those (the re-shade itself
   is independent of the gather). **[A] Combined mode gap**: the gradient re-shades the DIRECT term only, so a change
   visible only through the bounce (a light round a corner lighting this wall indirectly) never fires it, and in
   combined mode nothing else detects it (DI's old confidence is off, RTGI's pass stands down). Optional item
   `r_rtReStirDIGradientBounce`: at the stratum pixel also re-trace the bounce with LAST frame's IGN rotation / offset
   (CPU-hoisted per frame - store the previous pair; the Q2RTX replay) and fold `|B_prev - B_now|` into the gradient; costs
   1/9 of a bounce ray. Remix's cheaper alternative (invalidate GI from the DI gradient at the bounce HIT point) needs the
   hit position per pixel, which the combined gather does not keep.
4. **Filter**: 4 A-trous passes, 3x3 triangular kernel, strides 1/2/4/8, differences and maxima filtered separately
   (RTXDI: "a small local change in a bright region is unlikely to result in history invalidation"). Optional Remix
   hit-distance bilateral weight.
5. **Confidence** (stratum res, fed to `IN_DIFF_CONFIDENCE` directly): `c = saturate(1 - g.x / (g.y + bias))^s`, `s =
   r_rtReStirDIGradientPower` (8), `bias = 2^-12 * scene scale`; temporal blend `1/(1+h)` with `h =
   r_rtReStirDIGradientHistory` (8) applied ONLY while `c` rises (Remix anti-ghost rule); floor
   `r_rtReStirDIGradientFloor` (0.1). Master `r_rtReStirDIGradient` (1 once validated). **[A] Floor rule (NRD README,
   History Confidence):** under ReBLUR `historyLength *= lerp(confidence, 1, 1/(1+historyLength))` and the shortened
   length "goes through all passes and the feedback loop", so repeated low confidence compounds, and a confidence below
   `historyFixFrameNum / maxAccumulatedFrameNum` TRIGGERS the history-fix reconstruction (the 14 px-stride wash that
   caused the look-away fade). NRD's tips: clamp confidence to at least that ratio (16-frame history, 5-frame fix ->
   floor 0.31, not Remix's 0.1, which is a ReLAX number) and dither it by `0.5 / maxAccumulatedFrameNum`. So the
   effective floor is `max(r_rtReStirDIGradientFloor, historyFix / history)` for ReBLUR; ReLAX applies confidence "here
   and now" (`historyLength = min(historyLength, max * confidence)`) and "expects smaller values" - NRD-Sample squares the
   confidence for RELAX (`gradient *= gradient`). Both rules live in the shared path keyed on the active denoiser.
6. **Retire** the luminance confidence (`r_rtReStirDIConfidence`, the `rtgi_confidence` reuse) for DI once R1 wins the
   A/B; RTGI keeps its own.
7. **R1b - narrow the fast-light rule** - **USER-VALIDATED + MERGED 2026-10-12** (projectile lights clean through DI; mode 2 marginally better than 1 in chaotic lighting, equal when static; coverage line reads "36 of 37 ... weapon 1", every other light in DI; the r_shadowMapDebug line now breaks the uncovered lights down by reason): `r_rtReStirDIDynamicForward` is now an integer, **2** (default) = weapon lights only, 1 = the old full rule set for the A/B, 0 = none. The luminance / appeared tracking and the crossfade bookkeeping still run (a flip can only come from `allowLightInViewID` changing now, so the crossfade is effectively idle - R6 decides its removal). **Test:** fire the plasma gun / rocket launcher down a corridor: the projectile's light sweeps the walls through DI with no trail and no blink; a swinging / carried light and a strobe (> 20 % flicker) stay in DI without the 16-frame forward handover; `r_shadowMapDebug 1` "N of M lit lights DI-covered" should now be M minus the weapon light; A/B with `1`. Original plan: with gradients live, set `r_rtReStirDIDynamicForward` to cover ONLY
   `allowLightInViewID` weapon lights (new value 2), keep 1 as the fallback. Re-test the handover crossfade
   (`RB_RHI_SceneLightForwardWeight`) and the 16-frame hysteresis; the expectation from RTXDI/Remix is that both become
   unnecessary and are removed at R6 (a gradient reset is per-pixel and per-frame, a path flip is per-light and 3 tau).
Cost estimate: 1/9 of the gather's pixels x (one light shade + one ray) + 4 tiny A-trous passes + 1 confidence pass:
< 0.2 ms at 1440p x 0.75 (RTXDI: "< 5 % of the frame"). Acceptance: (a) flashlight / muzzle flash with
`r_rtReStirDIDynamicForward 0`: no trail beyond ~2 frames, (b) a light switching off: footprint goes dark within the
fast history, no "black dip" halo (the rise-only smoothing), (c) STATIC scene: validation view shows accumulated frames
unaffected (no frying / dimming - the failure of the old pass), (d) NPC walking under a static light: no gradient
fires (geometry change is NRD's job; the normal/depth test rejects it).

### R2 - ReLAX A/B (F2) - medium, independent of R1 - **MERGED 2026-10-12; ReLAX is the DEFAULT since 2026-10-12** (`r_rtReStirDIDenoiser` 1)
**Default decision (2026-10-12):** with `r_rtReStirDIRelaxAtrous` 3 (user: "roughly 3 ms") ReLAX lands within ~0.1 ms of ReBLUR's
2.87 ms at the same settings, and it won every quality count of the A/B below, so `r_rtReStirDIDenoiser` defaults to 1 with the
user's saved ReLAX values (`RelaxPrepass` 8, `RelaxAtrous` 3, the rest at the Remix / RTXDI starting values). ReBLUR stays as
`r_rtReStirDIDenoiser 0` for the A/B until R6 trims the loser; the ReBLUR-only knobs (`FastHistory`, `FastClamp`, `Stabilize`,
`Antilag`, `PrepassBlur`, `HistoryFix`, `BlurRadius`) are inert under ReLAX. NOTE for anyone with a saved config: every DI cvar is
`CVAR_ARCHIVE`, so a `seta r_rtReStirDIDenoiser "0"` in `dude.cfg` keeps ReBLUR until it is set to 1 once.
**Cost (user, 1440p x 0.75, 3080 Ti, SH mode, checkerboard):** `restir.di.denoise` ReBLUR 2.87 ms vs ReLAX 5.81 ms, +2.9 ms -
four times NRD's table delta (RELAX_DIFFUSE_SPECULAR 3.25 vs REBLUR 2.55 at 1440p/4080; SH +50 % on RELAX). Quality won on
every count, but +2.9 ms on a ~12.6 ms tier frame is not a default. Cost levers to measure before revisiting: `r_rtReStirDIRelaxAtrous`
5 -> 3 (each A-trous pass is a full internal-res pass), `r_rtReStirDIRelaxFirefly 0` (NRD: +7-10 %), and whether the SH variant is
needed under ReLAX at all (its luminance edge stopping may keep the bump N.L detail without the SG resolve - A/B `r_rtReStirDISH 0`
with ReLAX). If a ~4 ms ReLAX keeps the quality, flip the default then.
Shipped as planned: `r_rtReStirDIDenoiser` 0 = ReBLUR (today) / 1 = ReLAX; the `RELAX_DIFFUSE` + `RELAX_DIFFUSE_SH` pair joins the
NRD instance only on request (gfx-signature rebuild, like `r_nrdSigma`); `RtaoDenoiseArgs::denoiser` is generic (RTGI can A/B it
later, N2). Encodings verified in `NRD.hlsli`: ReLAX takes LINEAR radiance + RAW hit distance (`RELAX_FrontEnd_PackRadianceAndHitDist`)
and outputs linear, except SH mode whose SH0 comes back YCoCg (`RELAX_BackEnd_UnpackSh`: c0 / chroma); SH1 = direction * luminance in
both families. The gather switches its loc-0 encoding on `u_shadowFalloffS.w`; the apply / debug decode take a "source is linear" flag
computed per view. Under ReLAX the R1 confidence is squared (NRD sample) and the ReBLUR floor rule is skipped; `r_rtReStirDIEdgeStop`
scales `lobeAngleFraction` only. Starting values (Remix direct-light preset + RTXDI sample): `r_rtReStirDIRelaxFast` 1, `RelaxPrepass` 0,
`RelaxPhi` 1.0, `RelaxAtrous` 5, `RelaxVarianceHist` 1, `RelaxConfRelax` 0.7 (with 1.4 / 0.6), `RelaxFirefly` 1; the history is the shared
`r_rtReStirDIHistory`. Debug views 2-6 work under both. **Test (A/B at the same history):** `r_rtReStirDIDenoiser 1`: (a) grate /
contact shadows and the window-frame shadows sharper or equal vs 0; (b) flashlight trail shorter (fast history 1); (c) flat lit
floors: no new flicker (raise `RelaxPrepass` to 5-10 if the 1-spp noise shows - ReLAX has no spatial reuse feeding it yet, R3);
(d) `r_vkGpuTime 2 restir.di.denoise` delta (expect +0.3-0.7 ms); (e) look-away fade: ReLAX's history fix is 3 frames max - note
whether the wash reads better or worse. Decide the default; the loser's cvars are trimmed at R6.
**First user A/B (2026-10-12):** ReLAX WINS on all three: sharper shadows, shorter flashlight trail, no floor flicker. The new
sharpness exposed a moving-shadow softness: an NPC's shadow is soft and a little light while it walks and converges to sharp +
darker when it stops. Two causes: (1) the region a moving shadow enters is correctly history-reset by the gradient and then
rebuilt from ONE visibility ray per pixel - the denoiser's spatial filter widens where history is short and mixes lit neighbours
in. That is F3 in its lighting form and R3's disocclusion boost is the fix (8 samples where history is short). (2) a gradient
gap on the TRAILING edge: the second direction re-traced last frame's visibility in the CURRENT TLAS, where the mover had already
left, so "was lit, is lit" never reset the floor a shadow vacates. Fixed: when the current light IS the stored light (slot +
serial), the stored value - which holds the true previous visibility - is used as L_prev (also saves the ray). Plus
`r_rtReStirDIGradientFilterPasses` (4) to let the reset region hug a moving shadow more tightly.
**Rev. 2 (2026-10-12, after the user still saw a dark trail):** the stored-visibility fix could not reach the trailing edge
because the stratum picked its pixel by LAST frame's brightness, and a pixel a shadow just vacated was dark last frame - a lit
neighbour always won. RTXDI picks the brightest of EITHER frame, and this frame the vacated pixel is the bright one. So the
gradient now runs AFTER the gather: the DI pass first copies last frame's att1 / att2 / att3 aside (`restir.di.sidecopy`,
three plain copies at the gather res), the gather runs, then the gradient reads THIS frame's picks from the gather MRT and
last frame's from the copies, scores every stratum pixel by max(prevLum, curLum) and takes the winner. When the pixel shaded
the SAME light (slot + serial) in both frames, both luminances are the gather's own stored, truly-visible values: no re-shade,
no ray, exact in both directions. Only a changed identity re-shades the winning frame's light in the other frame (one ray).
The per-stratum exhaustive light loop is gone. `r_rtReStirDIGradientFilterPasses` default 4 -> 2 (user: the NPC shadow edge
improved). Remaining softness INSIDE a moving shadow = one sample per pixel after a correct reset -> R3.
Files: `VulkanBackend.cpp` (`NrdCreate` denoiser table: `NRD_ID_RELAX_DI = RELAX_DIFFUSE`, `NRD_ID_RELAX_DI_SH =
RELAX_DIFFUSE_SH`; `RtaoDenoise`: a `denoiser` arg, `nrd::RelaxSettings` path), `restir_di.frag` + `restir_di_apply.frag` +
`restir_di_debug.frag` (encoding switch), `RhiWorld.cpp` (the arg + cvar), ImGui dropdown.
1. `r_rtReStirDIDenoiser` 0 = ReBLUR (today), 1 = ReLAX. Both instances live in the one NRD instance (one runs per frame,
   as GI/GI_SH today). **[A]** The `denoiser` arg is generic in `RtaoDenoiseArgs`, so standalone RTGI can A/B ReLAX too
   (rtx-nrd.md N2; RTAO cannot - no RELAX occlusion variant). With ReLAX active, R1's confidence is squared and the floor
   rule drops to the raw cvar (see R1 item 5).
2. **Encoding switch in the gather / apply / debug**: ReLAX mode writes LINEAR radiance + RAW hit distance (world units)
   and SH1 via the `RELAX_FrontEnd_PackSh` layout (check `NRD.hlsli:936` at implementation); the apply decodes
   linearly and reads history length from `.w` (R3 uses it instead of R0's ReBLUR flag).
3. **Starting settings = Remix direct-light preset**: `diffuseMaxAccumulatedFrameNum` from `r_rtReStirDIHistory` (26-ish
   by seconds), `diffuseMaxFastAccumulatedFrameNum 1`, `diffusePrepassBlurRadius 0` (our 36 is a ReBLUR compensation),
   `diffusePhiLuminance 1.0`, `spatialVarianceEstimationHistoryThreshold 1`, `atrousIterationNum 5`, `enableAntiFirefly
   true`, `confidenceDrivenRelaxationMultiplier 0.7 / LuminanceEdgeStoppingRelaxation 1.4 / NormalEdgeStoppingRelaxation
   0.6` (active once R1 feeds confidence), `historyFixFrameNum 3`. Expose `phiLuminance`, `atrous`, `prepass`, `fast` as
   `r_rtReStirDIRelax*` cvars for the tune.
4. Keep radiance in `[0; 250]` (NRD: RELAX tracks x^2 in FP16) - clamp in the gather (the firefly clamp already bounds the
   lobe; check the light scale).
Acceptance (A/B vs ReBLUR at the same history): grate / contact shadows sharper or equal in the lit scene; flashlight
trail shorter (fast history 1); no new flicker on flat lit floors (ReLAX fast-history clamp); GPU cost delta recorded
(expect +0.3-0.7 ms for diffuse-only). Decide the default; the loser's cvars are trimmed at R6.

### R3 - spatial reuse with decoupled visibility + disocclusion boost (F3, F5) - large
**REVERTED 2026-10-12 (user: "everything else got worse, shadows flickering, NPCs flickering too").** Three changes made
while chasing the NPC trail were backed out together: gradient rev.2 (run after the gather, brightest of either frame), rev.3
(known-change-first selection + two-frame compare under checkerboard) and R3a (history-driven multi-pick boost, default 4).
Likely culprits: rev.3's known-change path cannot tell a lighting change from a visibility flip caused by the sub-pixel jitter
at a hard shadow edge (the stored values of two frames sit on slightly different points), so it reset history along every
shadow edge every frame; and the boost toggles a pixel between 1 and K rays as its history crosses the threshold, changing
the noise the denoiser sees frame to frame. Two of the three shipped WITHOUT an A/B switch and the third defaulted ON - a
breach of the stage rule (every change behind a default-off cvar) that made the regression impossible to isolate by cvar.
The branch is back to R2 (ReLAX) + the R1 gradient with the stored-visibility same-light fix, the state the user approved.
**Rev. 2 re-applied (user: "I remember gradient 2 being also good"):** the regressions came with rev.3 / the boost; rev.2 (the
gradient after the gather with three side copies, brightest sample of either frame, filter passes default 2) is behaviour-neutral
under checkerboard, cheaper (no per-stratum light loop) and correct with checkerboard off, so it stays. Rev.3 and the boost stay out.
**Lessons kept:** the NPC trail IS the geometric disocclusion wake refilling (view 4), so R3's boost is still the right idea,
but it must (a) be default-off, (b) not toggle: once a pixel is boosted keep K for the whole refill (hysteresis on history
length, or boost by a smooth weight), and (c) be tested alone. A known-change selection needs the exact-point comparison
(same world point, same visibility ray) - stored values from two jittered frames are not that.

**Gradient rev. 2 bug found 2026-10-12 while testing R3a (user: NRD validation view 3 "crazy flickering", with boost 0 and under
ReBLUR alike):** rev. 2's same-light shortcut compared the two frames' STORED luminances, which were shaded at two sub-pixel-
different points (FSR2 jitter). At every hard shadow edge their one-ray visibilities flip, a 100 % change every frame, the
confidence floor, a history reset along every shadow edge - view 3 painted it as flicker along edges, and under the boost those
edge pixels would have been permanently young (K rays). The 2a direction had the same flaw on static surfaces (stored current
sample vs a re-shade at the previous point). FIXED on the R3a branch: exactly one stored value is the truth and the other frame is
re-shaded AT THAT POINT with one ray - the winning light is last frame's pick (or the same light on a static surface): Lprev stored,
Lnow = its current row at the stored previous point; the winning light is this frame's pick (or the same light on a mover): Lnow
stored, Lprev = its previous row at the current point. Costs one ray per stratum again (rev. 1 measured 0.35 ms).
**Second finding (user: FSR2 off calms view 5, any FSR2 mode flickers it):** the re-shade lands on the stored previous point
reconstructed from a HALF-FLOAT view z (att1 RG16F): up to d/2048 along the view ray, which slides the point along a grazing
floor by a sub-pixel amount that CHANGES with the jitter every frame - enough to flip a hard-shadow ray. Without jitter the
error is constant (calm, if not exact). Fixed: att1 + its side copy are RG32F (`IF_RG32F`), so the gradient's point is the very
point the gather shaded. **Third finding (user: still flickers; "could it be a threshold?"):** yes - `r_rtReStirDIGradientStatic` (2 px) decided
static-vs-mover by the WORLD distance between the current point and the stored previous point against two pixels' lateral size
at that depth. With FSR2 the jitter-corrected reprojection often lands on the neighbouring pixel, and on a floor / wall at a
grazing angle one pixel apart is many "pixel sizes" of surface, so most static floors were classed as movers and took the
mover path (two different points). Fixed: static = the velocity's on-screen length <= N full-res pixels (a static point's
velocity is only the jitter delta). **Test:** view 5 white in a static scene WITH FSR2 (the R1 acceptance), view 3 calm,
flashlight circle still shows, NPC trailing edge still resets.

**Validation after the weapon fix (2026-10-12):** one remaining first-frame error was ours - the confidence pass bound its
never-written ping-pong partner as "last frame's confidence" on the first frame after (re)creation (fixed: white until valid).
The other, `gOut_DiffSh ... OpImageWrite Texel operand only contains 3 components` on every NRD SH dispatch, is NRD's own
REBLUR / RELAX SH shader writing a float3 into the RGBA16F SH1 output - vendored SPIR-V, harmless on NVIDIA, not ours to fix
(a newer VUID than NRD 4.18 targets); silence it with the duplicate limit or ignore.

**Weapon dark under the DI apply - THE cause (2026-10-12, after the noShadows gate and the player-body TLAS exclusion
both failed to cure it; the user's old "`r_rtReStirDIWorldOnlyPrepass 0` fixed it" was the real clue):** the DI pass ended
with a bare `backEnd.currentScissor = viewDef->scissor` - a cache update only. The backend saves / restores the live scissor
around every target pass, so after the DI chain the live scissor is still what the last scene draw before it set: the last
world zfill surface's rect (phase 0). The deferred weapon depth fill (phase 1) then meets a surface whose rect EQUALS the
cached full view, skips `SetScissor`, and writes the weapon's depth clipped to that prop's rect; every depth-EQUAL lit draw on
the gun fails outside it - black, position-dependent (which prop was drawn last), straight "scissor" cuts. Fixed:
`RB_RHI_ApplyViewScissor` (cache + hardware) at the DI pass end and before the weapon phase. The 17 other bare
`currentScissor = viewDef->scissor` sites are the same latent pattern (they get away with it because the light loop
re-applies per-light scissors) - sweep candidates for R6.

**Weapon dark under the DI apply - the REAL cause (2026-10-12, found with `r_vkValidation 1` + the user's "it depends on where the
weapon is, sometimes cut as if by a scissor"):** the covered-light branch gave the weapon's forward draw the inline shadow ray
for EVERY covered light, including `noShadows` lights (parm or shader) that the normal path never shadows and the gather skips
the ray for (`DUDE_LIGHT_NOSHADOWS`). A noShadows room light then blacked the gun wherever a wall or the player's own body sat
between it and the light - position dependent, straight "scissor" cuts along wall planes. Fixed: `lightRtPoint` only when
`cls.lightMayShadow`. The earlier unit-7/8 story was a coincidence (the DI pass zeroes every unit when it ends). Two validation
errors fixed on the way: the RG32F side copy reused the RG16F `smaa_copy` pipeline (colour-only RG16F / RG32F and the RGBA16F-led
MRTs now have their own pass classes), and NRD left `IN_DIFF_CONFIDENCE` in GENERAL (no post-barrier) so next frame's confidence
pass sampled it in the wrong layout.

**R3a cost finding (user, cave area, still view):** gather 7.1 ms with boost 4 vs 5.6 ms with boost 0 - 1.5 ms of rays on
pixels that were not young. Cause: the same-surface test ran against ONE reprojected previous pixel; under the FSR2 jitter that is
a neighbour as often as not, so every depth / normal edge of a static view read as disoccluded every frame. Now the test runs over
the 2x2 previous pixels the reprojection straddles (NRD's bilinear footprint, untraced checkerboard taps skipped) and a pixel is
young only when NO tap is this surface; the hysteresis bit comes from the matching taps. Debug view 7 shows the boosted pixels:
in a still view it must be black except the weapon and the screen edges. **Retest:** gather still, boost 4 vs 0 (expect equal),
then the wake test.

**R3a, the edge bands (user: view 7 white around ALL geometry, still):** the 2x2 footprint was not enough because the gather's
surface reconstruction sampled the depth / normal buffers LINEARLY - at a silhouette the point is a blend of the near and far
surface whose weight shifts with the jitter every frame, so no stored tap ever matched. `diSurfaceAt` now takes the nearest
texel for depth and normal (the gather, the gradient and the boost all go through it): every point sits on a real surface, the
shadow-ray origins too. **Retest:** view 7 black when still (weapon / screen edges aside), gather boost 4 == 0 still.

**R3a cost, second finding (user, still: 6.8 ms boost 4 vs 6.1 ms boost 0 with view 7 black bar specks):** not rays -
occupancy. The K-pick arrays stay live for every pixel, boosted or not, and the compiler keeps the registers. The boost is now
a COMPILED VARIANT (`restir_di_boost.frag` = `restir_di.frag` with `DI_BOOST 1`, picked on the CPU while `r_rtReStirDIBoost`
> 1); the plain gather carries none of it. Also: the boost never touches the view weapon (its DI is never displayed); the
reason code in the row fraction (0.25 history / 0.5 surface / 0.75 hysteresis) is what view 7 colours, and the gradient floors
it off with a 0.1 guard (a 0.25 guard misread 0.75 as the next row: a false light change, pinned history, red trails forever).

**R3a cost, third finding (user, 4 sets):** still, boost 0 = 5.3-5.9 ms gather, boost 4 (view 7 black bar specks) = 7.1-8.0 ms;
looking around, boost 4 = 7.6-9.3 ms. The variant program itself was the tax: a full per-pick record (pHat, radiance, direction,
distance, flags = 9 floats x 8) stayed live through the light loop on EVERY pixel. The extra picks now keep only the light
INDEX (4 uints) and re-shade their lights after the loop (deterministic: pHat matches the loop's); K max 4. Re-measure still /
turning before deciding the default. The entry band while turning is the stage's real cost and is by design.

**R3a cost, fourth finding (user: still 5.0-5.3 ms plain vs 7.5-8.0 ms with the boost program, slim picks made no difference):**
the variant PROGRAM is the tax - the shade / ray functions inlined twice make a slower gather for every pixel. The boost is now a
SECOND PASS (`restir.di.boost`, `r_vkGpuTime 2`) over the gather's targets with the boost program: it re-runs the young test and
DISCARDS every pixel that is not young, so only the young ones re-run the light loop with K picks and overwrite their attachments
(identical guides). The plain gather is the untouched plain program. Cost model: the young test on every pixel (a dozen fetches)
+ K rays on the young set. **Retest:** still, `restir.di.gather` boost 4 == boost 0, `restir.di.boost` small; looking around,
`restir.di.boost` = the entry band's price.

**R3a cost, fifth finding (user: still, gather 4.4 + `restir.di.boost` 3.6 ms with view 7 black):** the boost pass's `discard`
compiled to `OpDemoteToHelperInvocation` (glslang, vulkan1.4 target) - a demoted invocation keeps executing the light loop and
the ray queries as a helper, only its stores are dropped, so every pixel paid the full boost shader. `terminateInvocation`
(GL_EXT_terminate_invocation, SPIR-V 1.6 core) ends it for real. GOTCHA for every heavy shader with an early `discard`
(rtshadow_ray / rtao / rtgi early-outs): check the SPIR-V for OpDemote. **Retest:** still, `restir.di.boost` should be a few
tenths at most; looking around = the entry band.

**R3a cost, sixth finding (user, young + clustered boost):** still, young 0.06 ms, boost 1.4 ms; looking around, boost 2.6-2.9 ms.
What survives the cluster rule standing still are AREAS with a short NRD history: the footprints of flickering / animated lights,
which the gradient resets every frame on purpose. `r_rtReStirDIBoostHistory` 0 (new default) turns that trigger off - only a true
disocclusion (reason 2) and screen entry (3) boost, held until the history reaches 4 frames; > 0 keeps the old behaviour for the
flashlight-sweep case. **Retest:** still boost line at 0 vs 2; looking around at 0.

**R3a cost, seventh finding (user: still, boost 0.94 ms at threshold 0, 0.76 at 2):** the history trigger was not what survived
standing still. Two things were: (1) a HOLD that could never end - a pixel stayed boosted while NRD's history was below the OFF
threshold, and under a flickering light that history never gets there; (2) thin geometry (a 1-px railing jittering over its
background) reading as "revealed" every frame. Now: the young pass is a ping-pong target carrying each pixel's AGE since it became
young, and a hold lasts `hold` frames (4 with the trigger off, 2x the threshold with it on) regardless of NRD; and a pixel is
"revealed" only when no footprint tap is its surface AND at least one tap was NEARER (an occluder stood there) - a thin edge's taps
are all farther. **Retest:** still boost line (expect ~fixed cost only), looking around, NPC wake.

**R3a DEFAULT DECISION (user, 2026-10-12): `r_rtReStirDIBoost 2`, `r_rtReStirDIBoostHistory 0`.** Measured (cave, 3080 Ti, 0.75 x
1440p, checkerboard): standing still the boost pass costs 0.08 ms (K 2, 3x3 footprint; it was 1.5 ms before the footprint fix), the
young pass 0.08 ms; looking around 3.0 ms and an NPC walking past 1.0 ms were measured at K 4 before the footprint fix (K 2
halves the ray part, the footprint removed the false edge bands - expect well under those). The user judged the tighter NPC wake and the cleaner look-away worth it
at K 2 (half the ray part). Final architecture: plain gather (no boost code) -> young pass (per-pixel reason + age, ping-pong
R8G8) -> boost pass (young + >= 5 of 9 neighbours young; `terminateInvocation`, never `discard`) over the gather's targets.

**R3a REDONE 2026-10-12 as its own default-off stage** (branch `feat/rtx-restir-di-r3a-disocclusion-boost`, USER TEST PENDING),
on top of the ReLAX default. `r_rtReStirDIBoost` K (0 = off) / `r_rtReStirDIBoostHistory` H (4). What changed against the reverted form:
1. **Three ways to be young, tested in the gather (`diBoostWanted`)**: the pixel reprojects off screen; NRD's history length
   where it was (last frame's denoised `.w`, nearest texel - never bilinear, a frame count must not blend across an edge) is
   below H; or the RTXDI same-surface test against last frame's side copies fails (normal dot < 0.5 or > 10 % view-z mismatch
   through last frame's world -> view) - a TRUE disocclusion is boosted THIS frame, not a frame late when NRD first reports
   history 1 (the reverted form read the OCCLUDER's long history at the reprojected position and missed the wake's first frame).
2. **Hysteresis, no toggling**: the gather stores a "boosted" bit in its row id (`att3.w = row + 1 + 0.5`, exact in fp16 below
   1024 rows; the gradient floors it off) and a pixel that was boosted last frame STAYS boosted until its history reaches
   `min( 2H, r_rtReStirDIHistory )` (never below H). Under ReLAX the history is clamped to `max * confidence^2`, so near a
   moving light it hovers; without the band a pixel flipped between 1 and K rays every frame (the reverted form's flicker).
   A region whose confidence keeps the history under the OFF threshold stays boosted - bounded cost, and those are the pixels
   that need the rays.
3. **Camera cut / first DI frame / slot not ours**: every pixel is young (thresholds 1e6, side data declared absent).
4. **Side copies** (`restir.di.sidecopy`) are now made for the boost as well as the gradient (same gate), so the boost works
   with `r_rtReStirDIGradient 0` too. Under checkerboard a pixel not traced last frame reads its traced neighbour's cell.
5. Inputs on units 4 / 5 / 6 / 9 / 10 (white placeholders when off: the descriptor set stays complete; never 7 / 8 / 12).
The K picks themselves are the reverted recipe: K independent reservoir draws from the SAME candidate weights (one light loop,
K random streams), K visibility rays, the K unbiased estimates averaged (variance / K); the SH1 direction term averaged the
same way; the hit distance and the gradient luminance stay the first pick's. **Test (one change at a time, ReLAX default,
history 8):** (a) `r_rtReStirDIBoost 4`: a walking NPC's wake - the dark trail should shorten or vanish while view 4 still
shows the geometric wake (that is correct); screen edges while turning likewise; (b) static scene + flickering lights: NO new
flicker vs `r_rtReStirDIBoost 0` (the hysteresis claim); (c) `restir.di.gather` cost still vs walking NPC vs turning (expect
up only while something is disoccluding; a full-screen turn at K 4 is the worst case); (d) `r_rtReStirDIBoostHistory` 2 / 8
if a faint trail remains or the cost bites. Keep or revert on the user's word; the `[A]` neighbour-reservoir spatial reuse
(R3b) is unchanged below.

Files: NEW `restir_di_spatial.frag`, `restir_di.frag` (reservoir attachment), `RhiWorld.cpp` (the pass between gather and
denoise), `rt_light_eval.glsl` (none), optional NEW compute `restir_di_boiling.comp`.
1. **Gather writes its reservoir** (new attachment at gather res): `(row index of y, wSum, pHat(y), vis)` - the SH1
   attachment already carries `ldir·lum`, so no second direction needed.
2. **Spatial pass** replaces the radiance NRD sees: per pixel `N` neighbours in a 32 px radius (RTXDI), `N = 1`
   normally, `N = r_rtReStirDIDisoccTaps` (8) where the denoiser's history length (R0 item 3 / ReLAX `.w`, reprojected
   from last frame) is below `r_rtReStirDIDisoccFrames` (8); neighbours rejected on normal dot < 0.5 or relative depth >
   10 %; each neighbour's light is RE-SCORED at the centre pixel (shade it unshadowed with the gather's per-light code:
   that is `pHat_c(y_n)`), combined by weighted reservoir sampling with weights `pHat_c(y_n) · W_n · M_n`, biased 1/M
   normalisation first (`r_rtReStirDISpatialMis 0`), pairwise MIS as the follow-up (`1`); `M` capped at 20x (course).
3. **Decoupled visibility (Wyman §7)**: a neighbour's `vis` NEVER enters the combine weight; if the winner is the centre's
   own candidate reuse its traced `vis`, otherwise trace ONE fresh ray for the winner (RTXDI's reuse-within-16-px is the
   optional saving, off first). This is what removes the F5 darkening: the occluded dominant light now competes with
   the neighbours' visible picks at the right weight.
4. **Boiling / outlier filter**: per-pixel 3x3 clamp against the neighbourhood mean `wSum` at `r_rtReStirDIBoiling` (5x,
   the Remix direct-light threshold) in the fragment pass; the RTXDI 16x16 group-average variant (41x at strength 0.2)
   only if a compute pass is warranted.
5. Once R3 holds, re-tune `r_rtReStirDIPrepassBlur` toward 0 and `r_rtReStirDIBlurRadius` down (Cyberpunk: ReSTIR IS the
   pre-blur) and re-measure F5's skyway-sun parity.
Cost estimate: 1 (8 on disocclusion) light shades + <= 1 extra ray per pixel; expect +0.3-0.6 ms at 1440p x 0.75 in a
lit corridor, measured per-pass. Acceptance: look-away fade returns within <= 4 frames (Wyman's 1-tap number);
multi-light shadow regions brighter and quieter than the 1-spp gather in the RAW debug view; sun parity vs forward
held; no new blotching in motion (the course's correlation warning - the 1 tap dithers it).

### R4 - denoiser inputs (F4, F6) - small/medium
Files: `gbuffer.frag` (`out_Velocity`), `RhiWorld.cpp` (velocity target format + NRD `mvScale.z`), `restir_di.frag`
(material ID, hitT), `VulkanBackend.cpp` (`minMaterialForDiffuse`, `historyFixAlternatePixelStrideMaterialID`).
1. **2.5D motion vectors**: add `viewZprev - viewZ` to the velocity (RGBA16F attachment or a third channel) and feed NRD
   `motionVectorScale.z = 1` - for DI, RTGI and RTAO alike (NRD: fixes "massive history reset on moving objects").
   `r_rtMotionVectors25D` (1 once validated); FSR2 keeps reading `.xy`.
2. **Weapon material ID**: the gather writes materialID 1 in the NRD normal `.w` for `nt.a < 0.5` pixels, sets
   `minMaterialForDiffuse = 0` and `historyFixAlternatePixelStrideMaterialID = 1` with a shorter alternate stride (7):
   the weapon's constantly-reset pixels stop mixing into the world's blur and reconstruct from a tighter kernel.
   Cvar `r_rtReStirDIWeaponId`. **[A]** In checkerboard mode NRD reads its guides from the `rtgi_guides.frag` pass, NOT the
   gather's att2, so that shader must pack the same `.w` or the tag is lost on the default DI configuration. **[A]** The same
   tag goes into `rtao_ray.frag` / `rtgi_ray.frag` (N1): both carve a ZERO hole under the weapon with valid guides - the
   fried-halo mechanism DI hit - and the tag stops the world's blur pulling the hole in.
3. **hitT hygiene**: ignore-0 rule (a pixel with no reaching light but a bounce passes the bounce's `thit`, not 0);
   combined DI+GI blend per the NRD README (`directHitDistContribution`, `maxContribution 0.5`) as `r_rtReStirDIHitMix`
   (0 = today's direct-only).
Acceptance: NPC walking under a static light - validation view shows no history reset on its body; weapon sway leaves
no halo in the lit scene; grate shadows unchanged.

### R5 - detail recovery (F6) - medium
Files: `restir_di_apply.frag` (SG resolve), NEW GLSL port of `NRD_SG_ResolveDiffuse` / `NRD_SG_ReJitter` from `NRD.hlsli`.
1. Replace the custom cosine mix with NRD's SG resolve (`light.sharpness 2`, `ndf.sharpness 2`, the `max(Y, c0/pi)`
   floor) and the re-jitter Jacobian from the 4-neighbour full-res normals (clamped `[1/2; 2]`), `r_rtReStirDISHResolve`
   0 = today / 1 = NRD. NRD: with SG resolve `lobeAngleFraction` may be RELAXED (sample: x1.333) - retest
   `r_rtReStirDIEdgeStop`.
2. Re-evaluate `r_rtReStirDIResScale 1` + checkerboard off once R2/R3 have lowered the blur: the detail lost at 0.75 +
   checkerboard (user, 2026-10-08) may now be affordable at the same frame time. **[A]** If half the rays must stay, NRD's
   recommended form is NOT checkerboard (`CheckerboardMode::OFF // RECOMMENDED`) but probabilistic per-pixel skipping with
   `hitDistanceReconstructionMode = AREA_3X3` ("Probability at primary hit must be clamped to [1/4; 3/4]", Bayer
   dithering, not white noise) - full-res guides for free, no packed layout, no `rtgi_guides` pass. Worth one A/B.
Acceptance: bump N·L detail in dim DI light at least equal to today's SH mode; no new ringing at normal edges.

### R6 - consolidate - **DONE 2026-10-13** (branch `feat/rtx-restir-di-r6-consolidate`, build-verified, user playtest pending)
Done this pass:
- **Scissor fix (two genuine chain-ender bugs):** `RB_RHI_RtaoPass` / `RB_RHI_RtgiPass` ended their target-pass chain
  with a bare `backEnd.currentScissor = viewDef->scissor` (cache only) - the weapon-dark bug class - now call
  `RB_RHI_ApplyViewScissor`. The other ~20 bare writes already `SetScissor` explicitly or are GL/2D entry paths; left
  as-is (behaviour-equivalent, converting adds a viewport-offset risk for no gain).
- **Discard sweep:** the RT / RTAO / RTGI / DI shaders were already demote-safe (every early-out is `return` or
  `terminateInvocation`; `restir_di.frag` is the model). The only `discard`-before-heavy offender is `ssr.frag` (raster
  SSR, out of DI scope) - flagged, not changed.
- **DI is ReLAX-only:** `r_rtReStirDIDenoiser` removed (relaxMode always true for DI; ReBLUR still serves RTAO/RTGI).
  Trimmed the dead ReBLUR-only DI knobs `FastHistory` / `Stabilize` / `PrepassBlur` / `BlurRadius`; the dual-use knobs
  ReLAX also reads stay (`History`/`HistoryMs`, `FastClamp`, `HistoryFix`, `EdgeStop`, `Antilag`, `Disocclusion`).
- **Luminance confidence removed:** `r_rtReStirDIConfidence` + its `rtgi_confidence.frag` reuse pass + `rhiReStirDIConfRT`.
  The R1 gradient is the sole DI confidence now.
- **Forward path is weapon-only:** `r_rtReStirDIDynamicForward` is now a bool (1 = weapon lights forward, 0 = none).
  Removed mode 1's fast-light rule set, the handover crossfade (`RB_RHI_SceneLightForwardWeight` + `lightForwardWeight`),
  and the per-light history (`sceneLightTrack_t`); `diFast` is now just `allowLightInViewID != 0`.
- **ImGui dev menu:** removed the three now-unnecessary DI knobs (History Confidence, Prepass Blur, Blur Radius).
- **Tier:** no change needed - `dude_rtx 2` turns DI on and inherits the winning knob defaults from the cvar declarations.
- Doc "Architecture as shipped" + cvar table updated (this pass).

Still open: **[A]** the shared-path items in [rtx-nrd.md](rtx-nrd.md) (N0/N1 ride R0/R4; N2/N3 are independent
follow-ups) and the SIGMA decision there; **R3b** (spatial reuse with decoupled visibility) is still the structural
answer to the 1-spp noise / the 5 ms gather - decide after a normal play session across maps.

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

**Key sources:** (see also the per-section source list under *Review against public ReSTIR*) SIGGRAPH 2023 course *A Gentle Introduction to ReSTIR* (https://intro-to-restir.cwyman.org/);
Wyman & Panteleev, *Rearchitecting Spatiotemporal Resampling for Production*, HPG 2021; NVIDIA RTXDI SDK
(https://github.com/NVIDIA-RTX/RTXDI); Bitterli et al. 2020 (original ReSTIR DI); NVIDIA NRD
(https://github.com/NVIDIA-RTX/NRD).

## Non-negotiables (carried from every prior RT phase)
- `SupportsRayQuery()` + cvar gate; default off; raster/GL3/non-RT paths bit-for-bit unchanged.
- Runs at internal (FSR2) resolution like the other RT effects; FSR2 is the perf backstop.
- New shaders: re-run CMake configure so the glob picks them up ([cmake-glob gotcha]).
