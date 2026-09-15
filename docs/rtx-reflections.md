# RT reflections on monsters — trace the TLAS, shade the dynamic hit

The first consumer of the R3.5 animated BLAS ([rtx-animated-blas.md](rtx-animated-blas.md)). Now that
monsters are GPU-resident, model-space, per-entity TLAS instances with full `gpuSkinVB` attributes
(pos/normal/color, reachable by device address), a reflective surface can trace a reflection ray into
the TLAS and show a monster — **even one that is off-screen or screen-space-occluded**, which is
exactly what today's reflection stack cannot do.

## Where reflections stand today (the gap)

Three layers, none of which reflect off-screen dynamic geometry:
1. **Env probes** (`environment` / `bumpyenvironment`, `bakeGlassProbe`): cubemaps baked per-area.
   Static — a walking monster is never in them.
2. **SSR** (`ssr.frag` march + `ssr_composite.frag`, docs/ssr.md): dynamic, but **screen-space** —
   a reflection ray that leaves the frame or hits a screen-occluded surface finds nothing. A monster
   behind the camera, or hidden behind a wall from the camera's view but visible to the reflection,
   does not reflect.
3. (raster shadow-map path — unrelated.)

SSR already builds exactly the inputs an RT reflection pass needs: a G-buffer with view-space normal
+ PBR material (roughness/metalness) (`gbuffer.frag`, MRT), `_currentDepth`, and a Fresnel/gloss
composite (`ssr_composite.frag`). We reuse all of it.

## The mechanism (confirmed feasible)

- **In-shader TLAS trace** is already done for RT sun shadows (`interaction.frag`): the TLAS device
  address rides in as a `uvec2` push/uniform, `accelerationStructureEXT(uvec2(...))` + `rayQueryEXT`.
  Reflections trace a **closest-hit** ray (no terminate-on-first) and inspect the committed hit.
- **Identify the hit:** `rayQueryGetIntersectionInstanceCustomIndexEXT` + `…GeometryIndexEXT` +
  `…PrimitiveIndexEXT` + `…BarycentricsEXT` + `…ObjectToWorldEXT`. We tag each TLAS instance with a
  custom index → a row in a shader-readable geometry table.
- **Fetch attributes at the hit:** `GL_EXT_buffer_reference` (already used + working for the batched
  BDA zfill, `r_vkBdaZfill`) dereferences the hit geometry's `gpuSkinVB` (idDrawVert: pos/st/normal/
  tangent/color) and its 32-bit index buffer — both device-addressable since R3.5 S0. Read the 3
  triangle verts, interpolate normal + vertex color by barycentrics, transform the normal to world by
  `ObjectToWorld`.
- **Shade the hit cheaply:** reuse the RT sun direction + colour already plumbed for sun shadows —
  `diffuse = saturate(N·sunDir)·sunColor·albedo + ambient`, `albedo = vtxColor · matTint`. Optionally
  trace a second (shadow) ray from the hit to the sun for self/scene shadowing in the reflection.

No G-buffer of world materials is needed because we only fully shade the **dynamic** (monster) hit —
the case SSR/probes cannot cover. Static/world hits fall through to the existing SSR + probe layers.

## Design — hybrid, monster-focused

**RT augments the reflection stack; it does not replace SSR.** SSR stays the primary dynamic layer
(cheap, accurate, on-screen); RT contributes the pixels SSR structurally misses — dominated by
off-screen monsters. This bounds cost (RT only where it adds something) and risk (SSR still ships as
the default), and matches the ask ("reflections on monsters") precisely.

- **Reflective pixels only.** Same gate SSR uses: roughness ≤ cutoff, gloss window, metalness — from
  the G-buffer material MRT. Non-reflective pixels never trace.
- **Monster hits shade; everything else defers.** On a committed hit whose instance is a monster
  (table row has a `gpuSkinVB` address), fetch + shade. On a static/miss hit, write nothing — SSR and
  the env probe already own those pixels. (RR3 may add world-hit shading to also fill SSR's off-screen
  *static* misses, but that is not the MVP.)
- **Composite** additively into the same reflection buffer SSR feeds, weighted by the same Fresnel/
  gloss so material response and edges stay identical to SSR.

### Per-instance geometry table (the one new piece of infrastructure)

A device-local SSBO the reflection shader reads: a flat array of geometry descriptors, one per
`(instance, geometry)` pair. Each row:

```
struct RtGeoDesc { uint64_t vtxAddr; uint64_t idxAddr; uint vtxStride; uint flags; };
```

Each TLAS instance's `instanceCustomIndex` = the base row for its BLAS's geometries; the shader reads
`table[customIndex + geometryIndex]`. Monster instances (built in `RefreshAnimBlas`) fill real
`gpuSkinVB`/index addresses + `flags |= MONSTER`; static/mover instances get `vtxAddr = 0`
(`flags = 0`) → "no attributes, defer". Rebuilt each frame alongside the TLAS (cheap: a few dozen
rows). The normal transform is NOT stored — the shader gets it from `ObjectToWorldEXT`.

## Staged plan (validator-first, the way R2/R3/R3.5 landed)

- **RR0 — per-instance geometry table (no behavior change).** Add the SSBO + populate it where TLAS
  instances are built (monster rows real, static rows zero); assign `instanceCustomIndex`. Nothing
  reads it yet. Verify: row count == instance count, monster rows have non-zero addresses, no
  validation errors, non-RT/GL3 inert.
- **RR1 — reflection-ray + attribute-fetch validator (`r_rtReflTest`, one-shot).** Trace reflection
  rays at a set of surfaces; on a monster hit, fetch the interpolated world normal via the table+BDA
  and diff against a CPU reference (same triangle, same barycentrics). Proves identify + fetch +
  transform before anything renders. Mirrors `r_rtWorldTest` / `r_rtAnimBlasTest`.
- **RR2 — RT monster reflections (compute, additive) — MVP.** For reflective pixels: reconstruct
  world pos + normal from G-buffer/depth, reflect the view ray, trace closest-hit; on a monster hit
  shade (sun N·L + ambient from `gpuSkinVB` normal/colour) and write; else leave for SSR. Composite
  with the SSR Fresnel/gloss weighting. Gate `r_rtReflections`. **Visible:** monsters appear in
  reflective floors/metal, including off-screen.
- **RR3 — per-material colour, no bindless.** Replace the flat 0.55 albedo with each hit surface's
  **material average colour** (`idImage::averageColor`, already computed per texture) stored in the
  geometry table (one packed RGBA8 per row). Gives coloured monster reflections (brown zombie, grey
  sec-bot) — approximate, not per-texel, but a big step from uniform grey with zero new infra. Keeps
  the fixed key light + ambient (indoor Doom 3 has no sun; real per-light shading at a hit is RT-GI
  territory). **Verified visually 2026-09-15: RR2 geometry is pixel-perfect (user), so RR3 is a shading
  swap.**
- **RR4 — bindless material substrate → per-texel textured reflections.** The real prize, and the one
  genuine infra lift: add `VK_EXT_descriptor_indexing` (runtime descriptor array of the resident
  textures) + a per-geometry texture index in the table, so the RT shader samples the actual diffuse
  texture at the interpolated `st` (fetched from `gpuSkinVB`). Turns the coloured blob into a properly
  textured monster reflection. This substrate is the prerequisite for RR5.
- **RR5 — RT WORLD reflections (replaces the snapshot glass system).** The user's goal: today glass /
  env reflections use baked env-probe cubemaps + `_currentRender` snapshots, which can't show anything
  dynamic or off-probe. Rebuild the **static world BLAS from full `idDrawVert`** (not positions-only, so
  `st`/normal are available) + give each world geometry a material/texture index in the table (RR4
  bindless), then shade world hits with their real texture. Reflective surfaces trace the true room —
  no probes, no snapshots. Also lets RT fill SSR's off-screen *static* misses (a true hybrid).
- **RR6 — quality + verify + preset.** Roughness-driven glossy (SSR colour-mip blur), temporal reuse
  (`ssr_temporal` history + motion vectors), seam softening across the SSR↔RT boundary; A/B + perf;
  wire into Ultra Nightmare, `r_rtReflections` default per the measured cost.

## Risks & mitigations

- **Shading an off-screen hit with no material texture.** MVP shades from `gpuSkinVB` vertex colour ×
  a tint + sun/ambient — crude but recognisably the monster, lit consistently. Sampling the actual
  diffuse texture at the hit (bindless + st fetch) is an RR3+ refinement, not required for the win.
- **Cost.** Reflective pixels are a minority (gated by roughness/metalness), and RT runs only where
  SSR can't help. Trace at half-res + temporal (RR3) if needed. Measured in RR4; `r_rtReflections 0`
  reverts.
- **BDA / lifetime.** The table holds device addresses valid for the frame; monster `gpuSkinVB`/index
  addresses are stable per surface (rebuild the row on topology change, same trigger as the animated
  BLAS rebuild). Static rows are zero (never dereferenced). Guard every fetch on `vtxAddr != 0`.
- **Sync.** The reflection trace reads the TLAS (built in `RefreshAnimBlas`, already barriered to the
  fragment/compute ray-read stage) and the `gpuSkinVB` (written by the skin compute, barriered by the
  R3.5 S0 compute→AS-build/…→shader-read scope). The reflection compute pass slots after those, before
  the composite.
- **Non-RT / GL3 / SSR-off:** every path gates on `SupportsRayQuery()` + `r_rtReflections`; SSR and
  probes are untouched and remain the default.

## Not in scope (follow-ups this unlocks)
Full RT GI (indirect bounce off the same table), RT reflections of *world* dynamic movers with proper
material shading, denoised many-bounce reflections. The geometry table + hit-shading built here is the
shared substrate for those.
