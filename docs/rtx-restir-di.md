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

**Shipped (Stage D, 2026-10-08, user test pending):** `r_rtReStirDIApply` — DI LIGHTS THE SCENE: full-BRDF
G-buffer gather (demodulated), additive apply inside the FSR2 opaque snapshot, and the forward per-light draws +
shadow work skipped for every light the scene-light buffer covers. Details under "Stage D" below.

**Shipped (Stage G, 2026-10-08):** the scene-light buffer carries each light's projection cookie + falloff
ramp as bindless slots (`RtLight.projTex/falloffTex`, row 128 B) and DI + the RTGI bounce attenuate with
them (`rt_light_tex.glsl`), behind `r_rtLightTextures` (default 1). Details under "Stage G" below.

**TODO:**
- **Spatial reuse** — if the NRD knobs can't clean the grate/limb noise without over-softening, add an
  in-frame ReSTIR spatial pass (combine geometry-matched neighbours). No history/reproject → no trails;
  distinct from the retired *temporal* reservoir.
- **DI's own NRD instance** — so GI + DI can run together and the auto-claim mutual-exclusion can retire.
- **Preset wiring** — once Stage D holds up in testing, wire `r_rtReStirDI` + `r_rtReStirDIApply` (+ Stage G) into
  the `dude_rtx 2` "Ultra Nightmare" tier and the menu RT selector; then the v1 approximations listed under
  Stage D (stage/vertex colours, occlusion maps, diffuse/spec denoise split).

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

**Known v1 approximations (deliberate, listed for the test):** diffuse/specular STAGE colours + vertex colour
assumed 1 (not in the G-buffer); baked occlusion maps not applied to DI; the legacy non-PBR specular LUT not
replicated (`r_pbr 0` = Lambert); no Toksvig widening (G-buffer normal is unit); the organic spec-fallback
(flesh without a spec stage) not replicated; AO applies to diffuse+spec together; `noSpecular` light parm
ignored; multi-stage light cookies (Stage G) take the first live stage. The RTGI/RTAO auto-claim still stands
them down while DI is on (DI's own NRD instance remains the fix).

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
