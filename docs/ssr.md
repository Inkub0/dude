# Screen-Space Reflections (PBR Phase C.2)

Opt-in (`r_ssr`, default off) screen-space reflections for the GL3/RHI backend.
Part of the PBR feature (docs/pbr-materials.md, Phase C): stock Doom 3 ships **no
environment image content** — the ambient cubemap is a constant direction vector —
so reflective materials have nothing to mirror. Phase C.1 (`r_pbrEnvScale`) gives
metals a light-proportional glow floor; C.2 gives glossy surfaces the real thing:
the scene itself, marched in screen space.

Fidelity: entirely additive and opt-in. `r_ssr 0` (default) leaves every pass and
shader untouched; the vanilla and plain-PBR paths render bit-identically to before.

## 1. What it does

- Polished floors (ceramic_sheen), bare metal and low-roughness table entries
  reflect on-screen geometry: fixtures, screens, characters, lights.
- Glass and other cube-reflection surfaces (`r_ssrGlass`, on by default with
  `r_ssr`) mirror the on-screen scene instead of Doom 3's static generic
  cubemap, falling back to the cubemap where the ray leaves the screen or
  misses (see §2.1).
- Reflection strength follows the PBR material system: Schlick Fresnel per pixel
  (dielectrics reflect mostly at grazing angles — the classic wet-floor look;
  metals reflect at all angles), faded out toward `r_ssrMaxRoughness`.
- Works with `r_hdr` on or off, and independently of `r_pbr` (the classifier
  table and category sliders drive reflectivity either way).

## 2. Architecture (v1 sharp reflections + C.2.1 res scale / temporal)

Deliberately scoped to *sharp* reflections on low-roughness surfaces:

1. **G-buffer MRT** — the existing SSAO normal prepass (`gbuffer.frag`,
   docs/ssao-gtao.md) gains a second RGBA8 color attachment:
   `out1 = (roughness, metalness, 0, 1)` resolved per surface by the same
   priority chain as the lit path (category cvars > baked table > globals,
   shared helper `RB_RHI_ResolvePbrMaterial`). Attachment 0 (normal + weapon
   mask) is unchanged, so SSAO is unaffected. The prepass now also runs when
   only SSR wants it.
2. **Scene snapshot at the split point** — the shader-pass chunk in
   `RB_RHI_DrawView` is split at the first surface with sort > SS_DECAL:
   opaque geometry, lit interactions, emissive panels/screens and decals are
   already down; translucents (glass, particles) are not. `_currentRender` is
   copied there, so reflections contain the lit opaque scene and later
   translucents still draw *over* the reflections correctly.
3. **Three-stage pipeline at the split point** (C.2.1 split the original single
   pass so the march can run below full resolution):
   - **march** (`ssr.frag`) into an offscreen RGBA16F buffer at `r_ssrResScale`
     of the view (HDR reflected energy survives the intermediate). Miss = 0;
     hit = scene color with the per-ray fades (edge/range/facing) premultiplied.
   - **temporal accumulation** (`ssr_temporal.frag`, `r_ssrTemporal`, on by
     default): blends against the previous frame's result reprojected by camera
     motion (the `ssao_temporal` reprojection recipe, ping-ponged RGBA16F
     history, `r_ssrTemporalFeedback` fraction). The march jitter rotates per
     frame (golden-ratio walk) so successive frames sample different ray
     offsets and the grain genuinely resolves. Because reflections are a
     binary-ish signal (jittered rays swing thin-feature pixels between hit
     and miss), the AO-style hard min/max neighbourhood clamp let flicker
     through and forced feedback past 0.95 — replaced with **variance
     clipping** (history clamped to the 3×3 mean ± 1.25σ per channel) plus
     **hit-mask-aware feedback** (a jittered miss against an established
     reflection keeps ≥0.96 history; real disocclusions still die because the
     collapsed neighbourhood collapses the clip box first).
   - **composite** (`ssr_composite.frag`) — full-resolution additive blend onto
     the framebuffer, applying Schlick Fresnel × gloss × `r_ssrIntensity` from
     the full-res G-buffer. Weighting at full res means a half-res march only
     softens the reflected *image*, never the material/Fresnel edges.

   March details (`ssr.frag`):
   - reconstruct view-space position from `_currentDepth` (same
     `depth_consts` recipe as ssao.frag / softparticle.frag);
   - reflect the view ray about the g-buffer normal;
   - fixed-step view-space march projected through the real
     `u_projectionMatrix`, jittered per pixel (interleaved gradient noise);
     hit = **armed crossing**: the ray must first be seen in front of the depth
     surface, then pass behind it within `r_ssrThickness`. The arming step is
     load-bearing: bump-mapped normals (tile edges) tilt some rays into their
     own surface, and unarmed those rays "hit" the adjacent floor and smear a
     self-reflection haze across the whole grazing band (found in the first
     washroom test);
   - binary refinement (4 steps) to tighten the hit;
   - rejects: sky pixels, view-weapon texels (g-buffer alpha mask, both as
     source and as hit — the weapon's depth-hacked depth would smear), screen
     edges (uv fade), rays toward the camera (fade);
   - pixels whose Fresnel × gloss weight can't produce visible output discard
     before marching.

Not built yet (see §5): glossy (roughness-blurred) reflections.

### 2.1 Glass (translucent cube-reflection stages)

The three-stage pipeline above never touches glass: translucent materials are
excluded from the G-buffer prepass, and the composite runs at the split point
*before* translucents draw — so glass kept reflecting its static default
cubemap (first sighted on the Mars City hall windows). Rather than force glass
into the G-buffer, the `TG_REFLECT_CUBE` shader-pass stage marches for itself:

- Glass draws after the split, so `_currentRender` already holds the lit
  opaque scene snapshot and `_currentDepth` this view's opaque depth — exactly
  the data the march needs, at no extra capture cost.
- While the view's SSR pass has completed (`RB_RHI_SsrSceneValid`, compared
  per-viewDef so subviews/2D views never match), `RB_RHI_RenderTexgenStage`
  swaps `environment`/`bumpyenvironment` for `environment_ssr`/
  `bumpyenvironment_ssr` per draw — the material IR is untouched, so `r_ssr 0`
  or `r_ssrGlass 0` keeps the vanilla path bit-identical.
- The variants (shared march in `glass_ssr.glsl`) compute the cube reflection
  exactly as the base shaders, then march the ssr.frag recipe (armed crossing +
  binary refinement, weapon-mask and backface rejects) from the glass
  fragment's own view-space position/normal (interpolated varyings — glass is
  in neither the G-buffer nor the depth buffer, which also means the ray can
  never self-hit and arms naturally). Hit confidence = edge x range x facing
  fades; the result is `mix(cube, scene x r_ssrIntensity, confidence)`, so the
  cubemap takes back over smoothly where screen-space data runs out, and the
  stage colour (and `r_gl3ReflectionScale`) modulates the result either way.
- Full-resolution, no temporal accumulation: glass pixels are few, the surface
  is smooth (no roughness jitter to resolve), and the static interleaved-
  gradient dither hides the march banding.

## 3. Data flow

```
depth prepass ──> _currentDepth copy ──────────────┐
normal prepass ─> rhiNormalRT: N+mask │ rough+metal│  (MRT)
interactions + opaque shader passes                │
        │                                          v
        ├─ split (first sort > SS_DECAL) ─> copy _currentRender
        │                                   ssr.frag (additive)
        v
translucent shader passes, fog, post-process (unchanged)
```

Uniform packing (RB_RHI_ScreenSpaceReflections):
- `u_projectionMatrix` — view->clip for marching
- `u_localParam0` = (1/proj00, 1/proj11, maxDistance, thickness)
- `u_localParam1` = (steps, intensity, maxRoughness, 0)
- `u_screenCorrection.xy` = 1/viewSize; `.zw` = view/POT scale of _currentRender
- `u_depthTexRecip.xy` = fragcoord -> _currentDepth texcoord

Units: 0 = _currentRender, 1 = _currentDepth, 2 = normal g-buffer, 3 = material
g-buffer.

## 4. Cvars

| cvar | default | meaning |
|---|---|---|
| `r_ssr` | 0 | enable screen-space reflections |
| `r_ssrIntensity` | 1.0 | reflection strength multiplier |
| `r_ssrMaxRoughness` | 0.55 | roughness cutoff (fade starts at 70% of it); 0.55 keeps the 0.45-rough ceramic floors partially reflective — 0.45 or lower excludes them entirely |
| `r_ssrSteps` | 24 | linear march samples per ray |
| `r_ssrMaxDistance` | 1000 | march reach in view units |
| `r_ssrThickness` | 16 | depth tolerance for a hit (view units) |
| `r_ssrResScale` | 1.0 | march buffer resolution fraction (menu stops: 1/2, 2/3, 3/4, Full) |
| `r_ssrTemporal` | 1 | accumulate across frames; resolves the march grain |
| `r_ssrTemporalFeedback` | 0.9 | history fraction kept per frame (variance clipping handles flicker; no need to push this) |
| `r_ssrGlass` | 1 | glass/cube-reflection stages march the scene too (§2.1); inert while `r_ssr` is 0 |

Developer-tab sliders mirror the tuning knobs; the Enhancements tab has the
on/off toggle, the Resolution stops and the Temporal checkbox next to SSAO.

## 5. Known limitations / future work

- **Sharp only** (glossy blur *deferred — gated on a real sighting*): roughness
  dims the reflection but does not blur it. Glossy SSR needs a roughness-driven
  blur of the (now offscreen) march buffer before the composite. **Decision
  2026-07-31: not worth chasing proactively for Doom 3.** The surfaces where SSR
  is most visible (wet floors, glass, polished panels) *should* stay fairly sharp
  and already look right; the grimy medium-rough surfaces that would want blur
  already have their reflection dimmed hard by roughness, so blurring a
  near-invisible contribution is a subtle win in a dark, low-contrast game. The
  artifact it fixes — a too-clean mirror ghost on a dirty floor — only bites in a
  narrow roughness band when the scene is bright enough to show it. Revisit only
  if playtesting turns up a specific surface reading as too clean; the cheap first
  move then is a single roughness-weighted blur pass at the already-low SSR
  resolution (the Half-res upsample softens a little already). No fidelity cost to
  skipping it — `r_ssr` is fully opt-in and vanilla has no SSR at all.
- **Screen-space by nature**: off-screen content cannot appear in reflections;
  rays fade at screen edges. C.1's light-glow floor remains the fallback.
- Reflections snapshot the scene *before* fog and translucents — a reflected
  corridor shows no fog/glass. Acceptable at Doom 3 fog densities.
- The C.1 env glow and an SSR hit can mildly double-count on metals; both have
  independent sliders.
- The low-res upsample is plain bilinear weighted by full-res material response;
  if depth-edge halos show at Half resolution, a bilateral (depth-aware)
  upsample in the composite is the fix.
- Marching uses fixed view-space steps — long rays under-sample distant
  geometry; a Hi-Z march would fix reach and cost together.

## 6. Status

- v1 sharp SSR: built 2026-07-30, verified in-game (Marine Command corridor);
  armed-crossing hit test added after the first washroom test caught the
  bump-normal self-reflection haze.
- C.2.1 resolution scale + temporal accumulation: built 2026-07-30, pending
  in-game verification. Glossy blur deferred (see §5) — gated on a real sighting,
  not on the active roadmap.
- Glass extension (§2.1, `r_ssrGlass`): built 2026-08-01 after the Mars City
  hall windows were still showing the default cubemap with SSR on; pending
  in-game verification.
