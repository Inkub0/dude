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
- Glass and other cube-reflection surfaces (`r_ssrGlassProbes`, on by default
  with `r_ssr`) reflect a baked cubemap of the actual room instead of Doom 3's
  static generic `env/gen*` cubemap, at any viewing angle (see §2.1).
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

### 2.1 Glass (baked room probes)

The three-stage pipeline above never touches glass: translucent materials are
excluded from the G-buffer prepass, and the composite runs at the split point
*before* translucents draw — so glass kept reflecting its static default
cubemap (first sighted on the Mars City hall windows). Glass gets its own
mechanism instead (`r_ssrGlassProbes`, on by default with `r_ssr`): the
`TG_REFLECT_CUBE` stage binds a **baked cubemap of the actual room** in place
of the generic `env/gen*` image. No shader change — the stage colour,
`r_gl3ReflectionScale` and the vanilla reflection math all apply unchanged,
plus a glass-only brightness knob (`r_ssrGlassProbeScale`, folded into the
stage colour; the bumpy variant takes no stage colour — vanilla behaviour —
so the knob only reaches unbumped glass).

**Why not screen-space on glass?** A marching variant was built first
(environment_ssr/bumpyenvironment_ssr, the ssr.frag recipe from the glass
fragment's own varyings, twosided normals flipped toward the viewer). It
worked, but screen-space can only mirror what is on screen, and a pane viewed
head-on reflects the room *behind the camera* — so the march only visibly
contributed at grazing angles, while the probe covers every angle. Dropped
2026-08-01 (user decision): probe-only is simpler, cheaper (no per-pixel march
on glass at all), and loses almost nothing visually. The march variants live
in git history at 7cfec811 if a hybrid is ever wanted again.

- **Capture**: `bakeGlassProbe` renders six 90° views from the current eye
  position — the envshot recipe, same native cube layout (`_px.tga` …) the
  `cubeMap` keyword loads — into `envprobes/<map>/area<N>_*.tga` under
  `fs_savepath` (`r_ssrGlassProbeSize`, default 256). The view weapon is kept
  out via `tr.takingEnvProbe` (R_AddModelSurfaces skips weapon-depth-hack
  entities); the player body is already suppressed by the unchanged viewID.
- **Auto-bake** (`r_ssrGlassProbeBake`, default on): when a glass surface's
  area has no probe on disk, the backend buffers a `bakeGlassProbe` — a
  one-time hitch per area, then cached on disk forever. Captures only run for
  the area the viewer stands in (guaranteed-valid vantage); a pane looking
  into a neighbouring area gets its probe when the player goes there.
- **Lookup**: per portal area. The pane center is nudged toward the viewer
  before `PointInArea` (panes sit on window portals; the viewer's side is the
  room the reflection should show). Probes load lazily in the backend, keyed
  per map, and simply replace the `env/gen*` image bound on unit 0 of the
  cube-reflection stage.
- The probe is a static LDR snapshot from one point: no characters/dynamic
  objects in it, and parallax is approximate (standard env-map assumption).
- Re-capture a bad vantage with `bakeGlassProbe force`; probes are plain TGAs,
  deletable per map under the save path.
- The capture pipeline fixed a **vanilla envshot widescreen bug** on the way:
  `renderView_t` width/height are virtual 640x480 units, but envshot passed
  real pixels — harmless on 4:3 (ratios cancel), but on widescreen each face
  saved only a crop of its 90° view so the cube seams never matched. Both the
  probe bake and `envshot` itself now pass `SCREEN_WIDTH/HEIGHT`. (The odd
  rolled envshot axes are *correct*: they're mirrored cameras that emit
  native-layout faces directly from top-down screenshots.)

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
| `r_ssrIntensity` | 0.5 | reflection strength multiplier |
| `r_ssrMaxRoughness` | 0.56 | roughness cutoff (fade starts at 70% of it); 0.56 keeps the 0.45-rough ceramic floors partially reflective — 0.45 or lower excludes them entirely |
| `r_ssrSteps` | 26 | linear march samples per ray |
| `r_ssrMaxDistance` | 1024 | march reach in view units |
| `r_ssrThickness` | 26 | depth tolerance for a hit (view units) |
| `r_ssrResScale` | 1.0 | march buffer resolution fraction (menu stops: 1/2, 2/3, 3/4, Full) |
| `r_ssrTemporal` | 1 | accumulate across frames; resolves the march grain |
| `r_ssrTemporalFeedback` | 0.96 | history fraction kept per frame (variance clipping + hit-aware blending keep ghosting bounded even this high) |
| `r_ssrGlassProbes` | 1 | glass reflects baked per-area room cubemaps instead of env/gen* (§2.1); inert while `r_ssr` is 0 |
| `r_ssrGlassProbeBake` | 1 | auto-capture missing probes for the viewer's area (one-time hitch, cached to disk) |
| `r_ssrGlassProbeSize` | 256 | probe face resolution; `bakeGlassProbe force` re-captures |
| `r_ssrGlassProbeScale` | 1.0 | glass-only probe brightness (Developer-tab slider), on top of stage colour + `r_gl3ReflectionScale` |

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
- Glass march (`r_ssrGlass`, environment_ssr shader variants): built and then
  **dropped** 2026-08-01 — in-game it only visibly contributed at grazing
  angles (screen-space limit; a head-on pane reflects the room behind the
  camera), while the probes below cover every angle. Removed for simplicity
  and cost (user decision); last full version at commit 7cfec811.
- Baked room probes (§2.1, `r_ssrGlassProbes`): built 2026-08-01, user-picked
  over planar mirror subviews (probes: near-zero runtime cost, static content;
  mirrors: true dynamic reflections but an extra scene render per pane).
  Verified in-game after fixing the envshot widescreen crop (seams now match).
  `r_ssrGlassProbeScale` added as the glass-only brightness knob.
- Defaults retuned from in-game calibration 2026-08-01: intensity 0.5,
  cutoff 0.56, steps 26, distance 1024, thickness 26, feedback 0.96.
