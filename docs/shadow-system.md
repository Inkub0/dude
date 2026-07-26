# Shadow & lighting enhancements (as-built)

As-built reference for the shadow-mapping and emissive-lighting features added on
the GL 3.3 `opengl3` (RHI) backend during **Phase 3.5**. The forward-looking design
lives in [port-phases.md](port-phases.md) Phase 8; **this document is what actually
shipped** and is the place to look as the lighting path grows. Everything here is
built on the RHI, so it ports to Vulkan in Phase 4 rather than being re-derived.

All of it is **opt-in and enhancement-gated**: nothing changes with default cvars,
and none of it runs on the legacy ARB2 backend (`R_BackendSupportsEnhancements()`).
The faithful stencil-shadow look is still the default (`r_shadowMapping 0`).

Primary code: [`neo/renderer/rhi/RhiWorld.cpp`](../neo/renderer/rhi/RhiWorld.cpp)
(`RB_RHI_DrawWorld` is the per-light loop). Frontend caster linking:
[`neo/renderer/Interaction.cpp`](../neo/renderer/Interaction.cpp). Cvars:
[`neo/renderer/RenderSystem_init.cpp`](../neo/renderer/RenderSystem_init.cpp).
UI: Developer tab in [`Dhewm3SettingsMenu.cpp`](../neo/framework/Dhewm3SettingsMenu.cpp).

---

## 1. Per-light technique selector

`r_shadowMapping 1` does **not** mean "everything is shadow-mapped." The technique is
chosen per light, per frame, and stencil/shadow-map lights mix freely in one frame
(the interaction loop was always per-light). Decision, in order, for a shadow-casting
light with interaction geometry:

| Light | Condition | Technique |
|---|---|---|
| Projected / spot | always | **2D depth map** (reuses the light's projection texgen) |
| Point / omni | largest `light_radius` axis > `r_shadowMapStencilRadius` | **stencil volumes** (§7 oversize fallback) |
| Point / omni | within `r_shadowMapPointLimit` budget | **6-face cube map** |
| Point / omni | out of budget | unshadowed |
| Parallel / sun | (none) | no map → unshadowed, unless oversize → stencil |
| any | `r_shadowMapping 0` | **vanilla stencil volumes** |

Notes:
- **Parallel/directional shadow maps are deliberately not built.** The target mod
  (*The Fragging Phobos*) has zero `parallel` lights across all maps and decls — its
  sky/sun is faked with **giant point lights (radius up to 5000)**, which is what the
  §7 oversize→stencil path exists for. An ortho/cascade path would have no payoff here.
- Stencil volumes are built by the frontend **unconditionally**
  (`R_CreateShadowVolume`, gated on shadow-casting, *not* on `r_shadowMapping`), so a
  light routed to stencil always has volumes to draw.

---

## 2. Projected / spot lights — 2D maps

Single 2D depth map per light. The caster shader (`shadow_sm.vert/.frag`) **reuses the
light's projection S/T/Q planes** so the map aligns exactly with the lit cone, and
writes **linear falloff as depth** (no perspective z-fighting, no separate shadow
matrix). The interaction shader samples it with a hardware `sampler2DShadow` 2×2 PCF
plus a 4-tap Poisson kernel. Resolution: `r_shadowMapSize` (× adaptive scale, §4).

## 3. Point / omni lights — cube maps

Six-face `GL_TEXTURE_CUBE_MAP` depth target, sampled with `samplerCubeShadow`. Depth
stored is **linear radial distance / range**, where:

```
range = (lightRadius.Length() + lightCenter.Length()) × r_shadowMapPointRangeScale
```

- **Budget:** `r_shadowMapPointLimit` (default 64) caps how many point lights get a
  cube per view, chosen by on-screen importance; over-budget point lights render
  unshadowed. `0` = all.
- **Face culling:** `r_shadowMapFaceCull` skips rasterizing occluders into faces whose
  90° cone can't overlap the view frustum (faces are still cleared for seamless
  sampling). Big win at high resolution; a cached cube renders all 6 faces since it may
  be sampled from any later angle.
- Per-face resolution: `r_shadowMapPointSize` (default 2048, × adaptive scale).

## 4. Adaptive resolution

`r_shadowMapSizeScale` scales each light's map resolution with its radius so the
texel-to-world size (edge sharpness) stays roughly constant. `tier =
round(log2(radius / r_shadowMapSizeScaleRadius))`, clamped, with render targets pooled
per tier. `r_shadowMapSizeScaleRadius` (default 380) is the radius that maps to the
base resolution. Clamped to `glConfig.maxTextureSize` / `maxCubeMapSize`.

## 5. Static cube cache

Regenerating every static light's cube every frame dominated frame time (~20× win to
fix). `r_shadowMapCache` keeps each point light's cube across frames and only
re-renders it when the light or one of its shadow casters moves:

- Per-frame **token** = hash of the light pose + every caster's
  entity/modelMatrix/geometry pointer/index count/vertex-cache handle. Token match →
  skip the render, sample the stored cube.
- Lights with an **animated** caster (non-`DM_STATIC`) skip the cache (they'd never
  hit) and use the scratch pool.
- **VRAM-bounded:** `r_shadowMapCacheMB` (`-1` = auto, half of detected video memory
  via `GL_NVX_gpu_memory_info` / `GL_ATI_meminfo`; `0` = unlimited). LRU eviction by
  last-seen frame.
- **Teardown:** `RB_RHI_FreeShadowCubeCache()` (called from
  `idRenderWorldLocal::FreeDefs`) drops the whole cache on world teardown / map reload,
  because cached cubes are keyed to light indices that `FreeDefs` frees — otherwise a
  new level could sample a previous level's cube (and leak its VRAM until eviction).

## 6. Depth bias (per-receiver)

Bias is applied in normalized [0,1] depth space, so the effective world-space bias for
a cube is `bias × range`. Models curve and self-shadow differently from flat world
brushes, so the receiver picks the bias:

- `r_shadowMapBias` (0.0025) — **world / perforated** receivers (`IsStaticWorldModel()`).
- `r_shadowMapModelBias` (0.005) — **model** receivers (characters, weapons, props;
  includes alpha-tested character skins, which take the model bias).

Caster face selection is `r_shadowMapCull` (0 front / 1 back = second-depth, less acne
/ 2 two-sided).

## 7. Oversize "sun" lights → stencil fallback

`r_shadowMapStencilRadius` (default **250**, `0` = off): a light whose largest
`light_radius` axis exceeds it **skips the shadow map and draws Carmack stencil volumes
instead**. A sun-sized cube map can't resolve a shadow thrown thousands of units (it
pixelates) and wastes VRAM; stencil is pixel-exact, distance-independent, and costs no
map memory. Cheap and safe — the frontend already built the volumes (§1). The Developer
tab slider is capped at 512; the cvar accepts up to 16384 from the console.

## 8. Perforated (alpha-tested) casters

Grates, fences, and foliage are almost all flagged `noShadows` in the base game
*because stencil can't perforate them*. `r_shadowMapPerforated` (default 1) overrides
both the material and entity `noShadows` for `MC_PERFORATED` surfaces when shadow
mapping is on, so they cast real punched-out shadow maps. The caster pass samples the
alpha/coverage texture and `discard`s below the threshold (mirroring the z-fill alpha
test), and forces `CT_TWO_SIDED` (thin single-sided planes). This is the constructive
answer to "switch off the fake grate shadow": enable the real one.

## 9. Emissive fill lights (related lighting feature)

Interactive GUI screens (monitors, keypads, panels) glow but cast no light in Doom 3,
so they read as decals on an unlit wall. `r_emissiveSurfaces` (default off) spawns a
small, shadowless fill light per visible GUI surface. Each light is a **forward-facing
projected cone** whose apex sits behind the screen, so it lights the wall and the space
in front but never leaks through to the far side of the mount (the recessed-screen fix).
The old omnidirectional point-light mode was removed — it leaked through walls and none of
the tuning knobs shaped it. Tuning (all live-updating via the Developer tab):

- brightness / reach / per-view budget: `r_emissiveLightScale`, `…Radius`, `…Limit`
- `r_emissiveLightSpread` sets the cone width (tight beam … wide near-hemisphere)
- `r_emissiveLightFalloff` (default 0.5) slides the cone's near-falloff plane: 0 =
  full-bright then a sharp edge at its reach, 1 = a long gentle fade almost from the screen
- colour = alpha-weighted mean RGB sampled per image (`idImage::averageColor`),
  desaturated toward white by `r_emissiveLightSaturation` so it reads as bleed
- `r_emissiveLightSpecular` toggles specular highlights vs diffuse-only fill

Changing any baked-in knob (spread, fade-off, scale, radius, saturation, specular) bumps a
signature that forces a one-frame rebuild of the fill-light set, so slider edits take effect
immediately. See also the private design note `emissive-gui-lights` in the agent memory.

---

## 10. Cvar reference

| cvar | default | range | purpose |
|---|---|---|---|
| `r_shadowMapping` | 0 | 0/1 | 0 = stencil (faithful), 1 = shadow maps where supported |
| `r_shadowMapStencilRadius` | 250 | 0–16384 | lights bigger than this (max radius axis) fall back to stencil; 0 = off |
| `r_shadowMapSize` | 1024 | 256–4096 | 2D map resolution (projected/spot) |
| `r_shadowMapPointSize` | 2048 | 128–4096 | cube face resolution (point/omni) |
| `r_shadowMapPointLimit` | 64 | 0–128 | max cube-mapped point lights per view; 0 = all |
| `r_shadowMapPointRangeScale` | 1 | 0.1–32 | scale cube far plane + depth normalizer |
| `r_shadowMapSizeScale` | 1 | 0/1 | scale resolution with light radius |
| `r_shadowMapSizeScaleRadius` | 380 | 16–8192 | radius that maps to base resolution |
| `r_shadowMapFaceCull` | 1 | 0/1 | cull cube faces outside the view frustum |
| `r_shadowMapCache` | 1 | 0/1 | cache static point-light cubes across frames |
| `r_shadowMapCacheMB` | -1 | -1–32768 | cache VRAM budget; -1 = auto (½ VRAM), 0 = unlimited |
| `r_shadowMapBias` | 0.0025 | 0–0.5 | depth bias, world/perforated receivers |
| `r_shadowMapModelBias` | 0.005 | 0–0.5 | depth bias, model receivers |
| `r_shadowMapCull` | 1 | 0–2 | caster faces: 0 front / 1 back / 2 two-sided |
| `r_shadowMapPerforated` | 1 | 0/1 | perforated grates/fences cast punched-out maps |
| `r_shadowMapDebug` | 0 | 0–2 | 1 = per-view summary, 2 = per-light readout |
| `r_emissiveSurfaces` | 0 | 0/1 | GUI screens cast a fill light |
| `r_emissiveLightScale` | 0.50 | 0–4 | fill-light brightness |
| `r_emissiveLightRadius` | 1.80 | 0.25–16 | fill reach × screen size |
| `r_emissiveLightFalloff` | 0.5 | 0–1 | cone fade: 0 = sharp edge, 1 = long soft fade |
| `r_emissiveLightSaturation` | 0.75 | 0–1 | keep 0 = white … 1 = full screen hue |
| `r_emissiveLightLimit` | 24 | 0–256 | max fill lights per view; 0 = unlimited |
| `r_emissiveLightSpread` | 1.60 | 0.5–3.5 | fill cone width (tight beam … wide hemisphere) |
| `r_emissiveLightSpecular` | 1 | 0/1 | fill lights add specular vs diffuse-only |

## 11. Debugging

`r_shadowMapDebug 1` prints a per-view summary line: lights lit, projected (2D-mapped /
no-shadow), point (cube-mapped / cube-casters / faces drawn+culled), cache
(hit/rendered/scratch/dynamic + MB), **stencil-big** (oversize→stencil count), parallel,
and perforated-casters. `r_shadowMapDebug 2` adds a per-light readout (technique
`map=cube/2D/stencil/none`, occluder counts, distance, radius, range). Whole-frame GPU
time: `r_gl3GpuTime 1`.

## 12. Open items

- **Per-light bias scaling** — one global bias can't serve every light (flashlight
  wants ~0.0001–0.005, large fans want much more). Bias should scale with the light's
  falloff/range. The world/model split (§6) is a partial fix. See
  [known-bugs.md](known-bugs.md).
- **Grille under a giant point light casts no shadow** (parked) — likely the same
  coarse-resolution/large-bias problem a sun-sized cube has; such lights now route to
  stencil (§7), which still can't perforate a grate, so it stays shadowless. Accepted.
- **Parallel/ortho shadow maps** — intentionally not built (no parallel lights in the
  target content; sun is giant point lights).
