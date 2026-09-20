# Volumetric lighting (light shafts) — plan

**Status: Route A chosen 2026-09-20 (kept Route-B-ready). V0 + V1 + V2 BUILT on `feat/volumetric-lighting`,
user gate pending; V3 (temporal + depth-aware upsample) and V4 (look, menu) next.**
Backlog origin: [todo.md](todo.md) "Fidelity backlog — Volumetric light shafts".

## 1. What and why

Light from a lamp is visible *in the air* between the lamp and the wall: a cone under a ceiling spot, a
shaft through a grate, a monster's silhouette cutting a dark wedge out of a lit haze. Doom 3 has none of
this — its only participating medium is the **fog light** (a uniform-colour volume blended by view depth,
no light interaction) and a few hand-placed additive "beam" models. It is the largest remaining *mood*
win that needs no new assets.

**Fidelity note (policy).** This is the biggest look departure on the backlog: it adds light to pixels stock
never lit and lifts black levels inside lit volumes. Therefore: **strictly opt-in**, its own master toggle,
not wired into any preset until a visual pass says so, and the defaults tuned *thin* (Doom's air is clean —
a hint of haze in lit cones, not a smoke machine). Potato→Nightmare stay untouched regardless.

## 2. The model (identical for both routes)

Single scattering, homogeneous-ish medium, evaluated at sample points `x` along the view ray from the eye
to the opaque depth:

```
L_in(x)   = Σ_lights  color_l · atten_l(x) · window_l(x) · V_l(x) · phase(cosθ_l)
L_shaft   = Σ_steps   T(0→x) · σs(x) · L_in(x) · Δt          T = exp( −∫σt )   (Beer–Lambert)
final     = scene · T(0→depth) + L_shaft
```

- **`atten_l · window_l`** — exactly the RR16c light evaluation: radial falloff × soft box window for
  point/parallel lights, axial `1−f` × soft S/T window for spots, from the four world-space `lightProject`
  planes. **Already written** (`ssr_rt.frag`, RR16c) and already fed by a per-view device buffer
  (`RB_RHI_GatherReflLights`: origin, colour, cull sphere, planes, flags; cull-sphere-first rows so a
  non-reaching light costs 16 B). The volumetric pass consumes the *same buffer* — hoist the evaluation into
  a shared include (`rt_light_eval.glsl`) so reflections and volumetrics cannot drift apart.
- **`V_l(x)`** — visibility, the part that makes shafts *shafts*. At Ultra Nightmare: one `rayQueryEXT`
  any-hit ray `x → light` against the scene TLAS (the same TLAS `r_rtAllLights` / RT reflections use;
  terminate-on-first-hit, opaque-only — the cheapest ray type). Lights flagged `noShadows` skip the ray
  (they leak through walls in stock too, so this is faithful). Raster tiers: shadow-map lookup (§6, later).
- **`phase`** — Henyey–Greenstein, `g ≈ 0.3–0.5` (mild forward scatter: looking toward a lamp glows more
  than looking away). One cvar.
- **`σs`, `σt`** — a small global density (`r_volDensity`), optionally boosted inside stock **fog-light
  volumes** (their box is known; boost ∝ the fog's own density parm) so foggy rooms get proportionally
  stronger shafts and clean corridors stay clean. Extinction of the *scene* (`T(0→depth)`) defaults **off**
  (`r_volExtinction 0`): darkening distant surfaces is a second look change stacked on the first, and
  stock fog lights already own that job.
- **Which lights** (`r_volLights`): `1` = projected/spot lights only (default — they are the authored
  "feature" lights and the ones whose cones read as shafts); `2` = also point lights (every fill light
  becomes a faint glow ball: more haze, less contrast, more cost). Parallel/sun lights are included in both
  (outdoor shafts through windows are the money shot) — they are box-bounded in Doom 3 so the cost is bounded.
- **Not in v1:** the projection/falloff *textures* (gobo patterns, fan-blade lights). Volumes are uniform
  inside the soft window, like RR16c. A rotating-fan light loses its blade shadows in the air (its surface
  lighting is untouched). Fix later via the RR4 bindless table (add light textures, sample at `s,t`).

## 3. Route A — quarter-res ray march (fragment pass)

Everything below reuses existing machinery; **no new RHI primitives**.

1. **March** (`vol_march.frag`, fullscreen at `r_volResScale`, default Quarter; RGBA16F target: rgb =
   in-scattered radiance, a = transmittance). Per pixel: reconstruct the view ray from depth
   (`u_depthParms`, cinematic-safe), march `r_volSteps` (default 12) samples from the eye to
   `min(depth, r_volMaxDist)`. Step positions are **jittered per pixel + per frame** (interleaved-gradient
   noise × golden-ratio phase, the SSAO/RTAO idiom) so 12 steps resolve like ~100 after accumulation.
   Step spacing exponential-ish (denser near the eye). At each step: loop the light buffer → cull sphere →
   `rt_light_eval` → if contribution > ε, one any-hit ray → accumulate.
2. **Temporal accumulate + upscale** — the *fixed* `ssr_temporal` (2026-09-20: trace-texel clip spacing +
   un-jittered read, verified at Quarter). Own history pair at full res. One caveat to design for: the
   reprojection depth is the *opaque surface's*, while the fog lives in front of it — the same class of
   error as reflections-on-a-reflector, and harmless for a low-frequency signal; if lag shows on fast
   strafes, lower feedback for this pass only (own cvar), do not build virtual-depth reprojection.
3. **Composite** (`vol_composite.frag`) into the live HDR scene target: `scene·a + rgb`, with a
   **depth-aware upsample** (4 nearest low-res taps weighted by depth similarity) so shafts do not bleed
   across silhouettes — the one place a plain bilinear upscale visibly fails for volumetrics.
   **Where:** in `RB_RHI_DrawView` right **after `RB_RHI_FogAllLights`, before the post-process-sort
   surfaces** — so heat-haze/refraction distorts the shafts with the scene, the HUD is never touched
   (memory: scene-only effects must run before the HUD), and FSR2 / eye-adaptation / bloom downstream all
   see the shafts as part of the scene (bloom on a bright cone is free and desirable).

**Cost estimate (1440p, Quarter = 640×360 = 230 k px, 12 steps):** 2.8 M samples; after the cull sphere
~1–3 lights are evaluated per sample and perhaps a third of those pass the window → **~1–2 M any-hit rays
per frame**, well under RTAO's 3.7 M closest-hit rays. Expect **~0.6–1.2 ms** total (march + temporal +
composite), scaling with on-screen lit volume. *Estimate — measure with `r_vkGpuTime` at the first gate.*

**Limits (accepted for v1):** UN-only (needs the TLAS); translucent surfaces (particles, glass) are
composited over/under by opaque depth, so smoke in a shaft is lit as before, not by the shaft; cost scales
with resolution × steps × lights.

**Stages (validator-first, each gated by the user's eye + `r_vkGpuTime`):**
- **V0 — shared light eval.** Hoist RR16c's evaluation into `rt_light_eval.glsl`; `ssr_rt.frag` includes
  it; gather function renamed/generalised (`RB_RHI_GatherSceneLights`), called once per view and shared.
  *Gate: reflections pixel-identical, no perf change.*
- **V1 — unshadowed march.** March + composite, no rays, no temporal. Glowing cones that ignore
  occluders. *Gate: cones sit where the lights are, right colour, no HUD/2D bleed, cinematics OK.*
- **V2 — visibility rays.** *Gate: a monster / railing cuts a wedge out of the cone; light behind a wall
  does not glow through it; cost measured.*
- **V3 — jitter + temporal + depth-aware upsample.** *Gate: no banding, no shimmer at Quarter, no halos
  on silhouettes, acceptable lag in motion.*
- **V4 — look + controls.** Density/phase/intensity defaults by eye; fog-volume boost; `r_volLights 2`
  trial; menu group (Graphics → Lighting → Ray Tracing (RTX) section, off by default); docs + changelog.
  Preset wiring is a *separate, later* decision.

**Built so far (2026-09-20):**
- **V0** — `rt_light_eval.glsl` (row layout + `dudeLightInRange` / `dudeLightDir` / `dudeLightAtten`);
  `ssr_rt.frag` includes it (same math, same early-out order). The gather is now
  `RB_RHI_GatherSceneLights`, cached per view (reflections fill it, volumetrics reuse it), adds flag bit2 =
  `noShadows`, and — one deliberate behaviour change — takes a light's colour from its **evaluated shader
  registers** when the light is also in the view, so flickering / strobing / fading lights do the same in
  reflections and in their shafts (off-view lights keep the static colour).
- **V1 + V2** — `vol_scatter.glsl` (`dudeVolInScatter`: light loop + any-hit visibility ray + HG phase),
  `vol_march.frag` (jittered quadratic-spaced march, rgb in-scatter + a transmittance),
  `vol_composite.frag` (ONE / SRC_ALPHA), `RB_RHI_VolumetricLighting` called right after
  `RB_RHI_FogAllLights`. `r_volShadows 0` is the V1 look (unshadowed, leaks expected), `1` is V2.
  Requires VK + ray query + `r_hdr`; without a TLAS it runs unshadowed.

**Device-lost at the first gate (2026-09-20) → per-ray light culling.** The first cut looped the *whole*
scene-light buffer at every march sample. The buffer holds every light within ~2000 units (up to 256) and a
rejected light still costs a 16 B read, so 256 × 12 samples × 230 k px ≈ 10 GB of buffer reads per frame in a
light-dense map (`mc_underground`) — a multi-second frame, killed by the driver. Same lesson as RR16c's
+1.5 ms, twelve times over: **these loops are memory-bound.** Fix: `dudeVolCullRay` builds a ≤12-light
candidate list once per view ray (bounding sphere vs the ray segment + the light-mode filter) and
`dudeVolInScatter` iterates only that list; rays with no candidate return immediately. For Route B the
same function takes a per-froxel-cluster list — which is what a froxel pipeline wants anyway.

**Route-B readiness contract.** `rt_light_eval.glsl` and `vol_scatter.glsl` are **stage-agnostic**: no
`gl_FragCoord`, samplers, uniforms or derivatives — every input is a function argument. A froxel inject
kernel includes both unchanged and calls `dudeVolInScatter( lights, clusterList, count, tlas, shadows,
froxelCentre, viewDir, g )`. Everything Route-A-specific (the march loop, step distribution, screen jitter, low-res
target, composite) lives only in `vol_march.frag` / `vol_composite.frag` / `RB_RHI_VolumetricLighting`.
The light buffer, the cvars (`r_volDensity`, `r_volAnisotropy`, `r_volLights`, `r_volShadows`, …) and their
units (scattering coefficient per world unit) carry over as-is. Keep it that way: anything added to the
shading goes in the includes, anything added to the march stays out of them.

## 4. Route B — true froxel grid (compute, 3D textures)

The industry-standard structure (Wronski 2014; Hillaire 2015; idTech 6/7, UE, Frostbite).

1. **Grid:** camera-frustum-aligned 3D texture, e.g. 160×90×64, exponential depth slices, RGBA16F
   (rgb in-scatter, a extinction).
2. **Inject** (compute): per froxel, jittered sample position → the same `rt_light_eval` loop + ray →
   write `σs·L_in`, `σt`.
3. **Temporal** (compute): reproject the previous grid by camera motion (exact here — froxels have a real
   3D position, no "reflector depth" approximation), blend ~0.9–0.95.
4. **Integrate** (compute): front-to-back prefix sum along Z → each froxel holds accumulated in-scatter +
   transmittance from the eye.
5. **Apply:** *any* shader samples the grid at its own depth — a fullscreen pass for opaque, and
   crucially the particle/glass shaders too, so **smoke inside a shaft is lit by it**.

**Cost:** 921 k froxels × lights, independent of screen resolution; typically 1–2 ms on this class of GPU
with half the froxels culled by a Hi-Z max-depth test. Constant rather than cheap.

**What it needs that does not exist** (checked 2026-09-20, `neo/renderer/rhi/RHI.h`): the RHI has
`CreateComputeShader` / `Dispatch` but compute is **buffer-only**; there is no 3D image, no storage-image
create/bind, no image-layout tracking for compute-written images. New surface: `CreateImage3D`,
storage-image descriptors in the compute path, sampled-3D binding in the graphics path, barriers
(compute-write → fragment-read), retire/resize handling, device-lost recreation. That is a backend
feature in its own right — comparable to the Phase-1 compute bring-up — before the first visible pixel,
and new VK surface is where the device-lost history of this project lives.

**Stages:** B0 RHI 3D/storage images + validator (write a pattern in compute, sample it in a debug
overlay) → B1 inject unshadowed + apply (opaque) → B2 rays → B3 temporal + integrate → B4 apply in
translucent shaders → B5 look/controls.

## 5. Recommendation

**A first, B only if A's limits bite.** The expensive-to-get-right part — the per-sample shading
(`rt_light_eval` + visibility + phase + density) — is *byte-identical* between the routes and is written
once in V0–V2; B is then "move the loop from a fragment march into an inject kernel", with the look already
tuned. A reaches a judged image in ~2 sessions on proven infrastructure; B spends its first sessions on
backend plumbing with nothing to look at. The two things B buys — **lit particles** and
**resolution-independent cost** — are real, but Doom 3's shafts are mostly empty-air cones in corridors,
and Quarter-res + temporal was just verified stable. If, after V4, smoke-in-shafts looks wrong or the cost
at 4K is a problem, B0 starts with the shading and the look already settled.

## 6. Raster tiers (no RT) — deliberately later

Visibility from shadow maps instead of rays: the 2D spot maps are straightforward (one extra sampler
array / atlas bind + the light's shadow matrix in the buffer row), the point-light **cube cache** is not
(per-light cube slots, budgeted, staggered, some lights unmapped in a given frame → flickering shafts
unless handled). Plan: spots-only on raster tiers (which matches `r_volLights 1` anyway), after the UN
version is judged worth having at all. GL3 can run the same fragment march (no compute needed — another
point for Route A); it just needs the light buffer as a UBO/SSBO instead of a device address.

## 7. Risks / open questions

- **Light leaks:** the gather is distance-based (not PVS-based), so a light in the next room is in the
  buffer; the visibility ray is what stops it glowing through the wall. V1 (no rays) *will* show leaks —
  expected, not a bug. `noShadows` lights leak by design (as in stock).
- **Too many lights:** Doom 3 rooms can hold 20+ fill lights. `r_volLights 1` (spots only) is the main
  guard; the cull sphere the second; a per-sample light cap (nearest N) the fallback.
- **Black levels:** even thin haze lifts the blacks inside lit cones. Defaults must be judged in
  `mars_city1` (bright) *and* a dark Delta/Hell map. Eye adaptation will partially re-normalise — check
  it does not pump when walking through a cone.
- **Flashlight:** the player's flashlight is a projected light → it becomes a visible cone in front of the
  player. Probably the single most striking result, possibly also the most annoying (self-fog). Own
  scale cvar (`r_volFlashlightScale`), decided by eye.
- **Cinematics:** `r_znear` cram is handled by `u_depthParms`; verify in the intro.
- **Subviews / mirrors / GUIs:** primary fullscreen 3D view only, like the other scene-only effects.
- **FSR2 reactive mask:** the shafts land after `RB_RHI_FsrCaptureOpaque`, so FSR2's auto-reactive pass
  sees them as "translucent delta" and trusts history less there — likely fine (low-frequency), but it is
  the first thing to check if shafts shimmer only with `r_fsr 1`.

## 8. Cvars (Route A, all VK + RT; archive; off by default)

`r_volumetrics` (master) · `r_volIntensity` · `r_volDensity` · `r_volAnisotropy` (HG g) · `r_volLights`
(1 spots, 2 all) · `r_volSteps` · `r_volResScale` (Quarter/Half) · `r_volMaxDist` · `r_volExtinction` ·
`r_volFogBoost` · `r_volFlashlightScale` · `r_volTemporalFeedback` · debug: `r_volDebug` (raw march /
transmittance / per-pixel ray count).
