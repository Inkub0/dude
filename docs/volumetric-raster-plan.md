# Volumetric lighting on the raster tiers (shadow-map visibility) - project plan

**Status: PLANNED 2026-11-10, not scheduled.** Written at the user's request after asking for volumetric lighting
"on at all Ultra and Nightmare presets regardless of RTX and Path Tracing". The preset change waits for this
project: without it, raster Ultra / Nightmare would get unshadowed haze (light cones through walls).
Parent design: [volumetric-lighting.md](volumetric-lighting.md) (Route A, shipped; section 6 sketched this).
To-do entry: [todo.md](todo.md), "Volumetric lighting on the raster tiers".

## 1. Goal

Light shafts with real occlusion at Ultra and Nightmare **with RTX off**, using the shadow maps the raster light
loop already renders instead of ray queries. Then wire `r_volumetrics` into the raster preset table (Ultra and
Nightmare on, every RT mode), so the haze no longer depends on the RTX axis.

Non-goals: froxels (Route B), lit particles, gobo textures in the haze (separate backlog items in the parent doc).
RTX on keeps today's ray-query visibility unchanged.

## 2. Where things stand (as-built facts this plan relies on)

| Fact | Where |
|---|---|
| The volumetric pass runs only with `r_volumetrics`, Vulkan, `SupportsRayQuery()`, an HDR frame and a captured depth | `RhiWorld.cpp` `RB_RHI_VolumetricLighting` |
| Visibility = one any-hit `rayQueryEXT` per reaching light per march sample, against the scene TLAS; TLAS address 0 forces shadows off | `vol_scatter.glsl` `dudeVolInScatter`, `RhiWorld.cpp` (`mp.localParam1[3]`) |
| The TLAS exists only while an RT cvar wants it (`r_rtWorld`, `r_rtSunShadows`, `r_rtMovingLights`, `r_rtAllLights`, `r_rtao`, `r_rtgi`) | `tr_main.cpp` `R_RtSceneWanted` |
| So with RTX off and `r_volumetrics 1`, the haze is **unshadowed** on RT hardware and **absent** on non-RT GPUs and GL3 | the two gates above |
| Light rows: 128 B std430 per light, via buffer device address, with the world-space `lightProject` planes S/T/Q/F | `rt_light_eval.glsl` `RtLight`, `RB_RHI_GatherSceneLights` |
| Spot / projected maps store **linear falloff (F plane) as depth**, rendered through the light's own S/T/Q planes. The volumetric visibility test is therefore just `s = S.p/Q.p`, `t = T.p/Q.p`, compare against `F.p`, all from the row | `shadow-system.md` section 2 |
| Static spot maps persist in `rhiMapCache[32]` (token + LRU). Moving lights and the flashlight render into the shared per-tier scratch target, overwritten by the next light | `RhiWorld.cpp` `RB_RHI_Acquire2DTarget` |
| Point lights: 6-face cube, depth = radial distance / `range`, `range = (radius.Length() + center.Length()) * r_shadowMapPointRangeScale`. Static layer cached (`rhiCubeCache`, VRAM-budgeted LRU); movers in a per-frame scratch cube (`u_shadowCubeDyn`, unit 12) | `shadow-system.md` section 5 |
| Point lights over `r_shadowMapPointLimit`: unshadowed. Oversize point lights (> `r_shadowMapStencilRadius`) and parallel lights: stencil or nothing, no map | `shadow-system.md` sections 1 and 7 |
| Bindless set 2 (`sampler2D u_rtTextures[]`, descriptor indexing) is enabled whenever the device reports the Vulkan 1.2 bits - **independent of ray tracing** | `VulkanBackend.cpp` `haveDescriptorIndexing` |
| The light rows are already culled per view ray (`dudeVolCullRay`, at most `DUDE_VOL_MAX_LIGHTS` = 16 candidates) | `vol_scatter.glsl` |

## 3. Design

### 3.1 One march, two visibility back-ends

`vol_scatter.glsl` keeps a single in-scatter loop. The visibility term becomes a function chosen at compile time:

- `DUDE_VOL_VIS_RAY` (today): the any-hit ray query. Built into `vol_march.frag` as now.
- `DUDE_VOL_VIS_SM` (new): a shadow-map lookup. Built into a second program, `vol_march_sm.frag`, which does **not**
  enable `GL_EXT_ray_query`, so it loads on any Vulkan 1.3 device with descriptor indexing.

`RB_RHI_VolumetricLighting` picks the program per frame: the TLAS exists and `r_volShadows` is on, use the ray
variant; otherwise the shadow-map variant. The `SupportsRayQuery()` gate is dropped for the SM variant. The
composite, temporal upscale and blur passes are shared unchanged.

### 3.2 Per-light shadow side table

The 128 B light row is full, and the map a light uses is only known after the light loop has run (cache hit,
re-render or scratch). So a parallel **side buffer**, one 16 B entry per light row, filled after the light loop,
just before the volumetric pass:

```
struct VolShadow { uint kind; uint slot; float range; float bias; };
// kind: 0 = no map (skip the light in raster haze), 1 = spot 2D map, 2 = point cube (static layer),
//       3 = point cube + movers layer, 4 = NOSHADOWS light (lit, unshadowed, as in stock)
// slot: bindless index of the map(s); range: cube depth scale; bias: in the map's depth units
```

Address passed next to the light-buffer address (spare `u_rtParms` lanes, unused by the SM variant since it has no
TLAS). The light loop records, per light index, which render target it sampled this frame; the gather writes the
table from that record.

### 3.3 Getting the maps to the shader

- **2D (spot) maps:** register each map that a volumetric candidate uses in the bindless set 2 array as a plain
  `sampler2D` with a linear, non-compare sampler, and do the compare in the shader
  (`step( F.p - bias, texture( map, st ).r )`, or four taps of a 2x2 gather for a soft edge). Slots come from a
  small reserved range at the end of the bindless array (the set is variable-count and update-after-bind).
- **Cube maps:** set 2 needs a second binding, `samplerCube u_volCubes[]` (fixed count, partially bound; only the
  last binding may be variable-count). Same reserved-slot scheme.
- Depth render targets must be sampleable as colour reads (they already are: the interaction pass samples them).
  Check the image view aspect (depth only) and layout (`SHADER_READ_ONLY` after the light loop) on both the
  cached and scratch paths.

### 3.4 Lights whose map does not survive the light loop

The real problem. Only the cached maps persist; everything else is overwritten:

| Light | Today after the loop | Plan |
|---|---|---|
| Static spot (cache hit or re-render into its slot) | persists | use as is |
| Moving spot, **flashlight** | scratch, gone | **"keep for haze"**: when the light is a volumetric candidate (spot, in view, reaches the view frustum), acquire a persistent target from a small per-frame pool (`r_volShadowKeep`, default 4, one 4 MB map each) instead of the scratch target. The flashlight is the single most important case (the brightest cone in the game); it always gets a keep slot. |
| Static point light (cube cached) | persists | use as is (R3) |
| Point light's movers layer | scratch, gone | R3 option: keep the movers cube for the N nearest point lights, else the haze from point lights ignores monsters (they are faint ambience, `r_volPointLightScale` 0.12 - acceptable for a first cut) |
| Point light over budget / oversize / parallel | no map | **skip in raster haze** (no light rather than a leaking cone). NOSHADOWS lights stay lit, unshadowed, as stock. |

The keep pool costs VRAM only while volumetrics runs on the raster path (4 x 4 MB at 1024, x4 at 2048).

### 3.5 Shader cost

One texture fetch per candidate light per sample instead of a ray query: cheaper than the RT path, so the raster
tiers can afford the same `r_volSteps` / `r_volResScale`. Budget to verify in R1: under 0.5 ms at 1440p, spots
only, Quarter res (the RT path measured +0.5 ms spots only, ~+1.2 ms all lights).

### 3.6 Look parity with the RT path

- Map resolution shows as stair-stepped shaft edges; the per-frame IGN jitter + temporal accumulation already
  smooth the haze. If edges still crawl, use a 2x2 bilinear PCF of the compare result, then a per-sample
  rotated 4-tap like the interaction path.
- Bias: the haze samples are in air, so there is no slope term; a constant bias in falloff units (spot) or
  normalised radius (cube) of about one texel is enough. Leaking at the occluder's foot is invisible in haze.
- Shaft occluders: the SM path sees only what the light loop rendered as casters. Perforated casters (grates)
  already render alpha-tested into the maps (`shadow-system.md` section 8), so grate shafts work.

## 4. Phases

Each phase ends with a clean build, `r_vkValidation 1` clean, `r_vkGpuTime` numbers, and **the user's visual check**.

| Phase | Work | Done when | Size |
|---|---|---|---|
| **R0 Recon** | Confirm: depth-target views sampleable from set 2, image layouts after the loop, bindless reserved-range allocation, where the light loop knows a light's render target. Write the side-table struct. No behaviour change. | A short findings note in this doc | S |
| **R1 Static spots** | `vol_march_sm.frag` + `DUDE_VOL_VIS_SM`, side table, bindless registration of cached 2D maps, program selection, raster gate (no `SupportsRayQuery`). Lights without a persistent map skipped. | Shafts with occlusion from static spot lights with RTX off; the RTX-on path unchanged | M |
| **R2 Moving spots + flashlight** | "Keep for haze" pool (`r_volShadowKeep`), candidate test before each spot's shadow render, flashlight always kept | The flashlight cone is occluded correctly; moving lights' shafts follow movers | M |
| **R3 Point lights** | Second set-2 binding (`samplerCube[]`), cube static layer in the side table, optional movers-layer keep for the N nearest | `r_volLights 2` on raster shows point-light haze with walls occluding it | M |
| **R4 Presets + menu** | `r_volumetrics` becomes a **raster column** (Ultra and Nightmare on, every RT mode; `r_volLights` 1 on Ultra, 2 on Nightmare if R3 lands), removed from `RtAugment` and from the RT-axis detection (`rtLive` in `DetectEnhancementPreset` counts `r_volumetrics` today, so raster volumetrics would read as "RTX on"). Config-schema targeted step. Move the menu checkbox from the Ray Tracing group to Lights & Atmosphere. Docs. | Applying Ultra or Nightmare with RTX off turns the haze on; preset detection still reads the right tier and RT mode | S |
| **R5 (optional) GL3** | The fragment march ports to GL 3.3: light rows in a texture buffer (no buffer references), maps copied into a 2D atlas / cube array (no bindless). Only if the user wants volumetrics on GL3. | Same look on `opengl3` | L |

## 5. Risks and open questions

- **Light-loop coupling:** the "keep for haze" decision must be made before a spot light's shadow render, i.e.
  the volumetric candidate test runs early in the light loop. Keep it cheap: spot + frustum-visible + reaches the
  view, nothing per pixel.
- **Map not refreshed this frame:** a cached map whose light was culled from this view's light list may be stale
  (still the right map, the light did not move - the token says so), but a light in the haze that was not in
  the light loop has no map: skip it.
- **Point cube face culling:** uncached cubes skip faces outside the view frustum (`r_shadowMapFaceCull`). A haze
  sample inside the view frustum always lands on a face that overlaps it, so this is safe; note it in R3 tests.
- **Shadow mapping off:** raster Potato / Low (stencil shadows) never get volumetrics anyway. A user forcing
  `r_volumetrics 1` with `r_shadowMapping 0` gets haze from NOSHADOWS lights only (everything else is kind 0):
  correct, and documented in the cvar help.
- **Flicker from budgets:** a point light dropping in and out of `r_shadowMapPointLimit` toggles its haze. The
  side table can hold a light's last kind for a few frames (hysteresis), as the shadow cache already does.
- **Fidelity:** volumetrics is the largest look departure (parent doc, section 1). Turning it on at raster Ultra and
  Nightmare is the user's decision of 2026-11-10; Potato to High stay untouched.

## 6. Verification (per phase)

Build side: SPIR-V `0 failures`, `spirv-val` on `vol_march_sm.frag.spv` (no `RayQueryKHR` capability in it),
dhewm3 ABI check, `r_vkValidation 1` clean on load + a minute of play. In game (the user): RTX off at Ultra, a
spot-lit corridor (mars_city1), the flashlight against a doorway, a grate shaft, a dark Delta map for black
levels; A/B the haze against RTX on (ray-traced shafts) for shape parity; `r_vkGpuTime` for the vol scopes.
