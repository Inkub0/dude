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
- Reflection strength follows the PBR material system: Schlick Fresnel per pixel
  (dielectrics reflect mostly at grazing angles — the classic wet-floor look;
  metals reflect at all angles), faded out toward `r_ssrMaxRoughness`.
- Works with `r_hdr` on or off, and independently of `r_pbr` (the classifier
  table and category sliders drive reflectivity either way).

## 2. Architecture (v1: sharp reflections)

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
3. **March + composite in one fullscreen pass** (`ssr.frag`) — additive blend
   onto the framebuffer at the split point:
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
   - weight = Fresnel(NdotV, F0 = mix(0.04, 0.9, metal)) × gloss fade ×
     `r_ssrIntensity`; pixels below a threshold discard before marching.

Not in v1 (see §5): glossy (blurred) reflections, temporal filtering, half-res.

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

Developer-tab sliders mirror all of these; the Enhancements tab has the on/off
toggle next to SSAO.

## 5. Known limitations / future work (Phase C.2.1)

- **Sharp only**: roughness dims the reflection but does not blur it. Glossy
  SSR needs a roughness-driven blur (mip chain or jittered rays) plus the
  temporal filter to hide the noise — the SSAO temporal machinery
  (reprojection matrix, history ping-pong, neighbourhood clamp) is the
  template.
- **Screen-space by nature**: off-screen content cannot appear in reflections;
  rays fade at screen edges. C.1's light-glow floor remains the fallback.
- Reflections snapshot the scene *before* fog and translucents — a reflected
  corridor shows no fog/glass. Acceptable at Doom 3 fog densities.
- The C.1 env glow and an SSR hit can mildly double-count on metals; both have
  independent sliders.
- Half-res march (`r_ssrResScale`) is an obvious perf lever if full-res marching
  shows up in `r_gl3GpuTime`; needs a bilateral upsample to avoid edge halos.
- Marching uses fixed view-space steps — long rays under-sample distant
  geometry; a Hi-Z march would fix reach and cost together.

## 6. Status

- v1 (sharp SSR as above): built 2026-07-30, pending in-game verification.
