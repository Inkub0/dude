# Baked ambient-occlusion (occlusion) maps

Per-material **baked ambient-occlusion textures** for the GL 3.3 `opengl3` (RHI) backend,
added in Phase 3.5 alongside SSAO. Where [SSAO/GTAO](ssao-gtao.md) computes occlusion in
screen space, an occlusion map is an **art-authored** AO texture that ships with a material
and darkens the same terms SSAO does. The two are complementary and stack.

Like the rest of the enhancement suite it is **opt-in and enhancement-gated**
(`R_BackendSupportsEnhancements()` → core profile only; nothing on the legacy ARB2 backend)
and **off by default** (`r_occlusionMaps 0`). It reuses the SSAO application path rather than
adding a new one.

## Fidelity note

**Zero impact on stock Doom 3.** No vanilla material declares an occlusion stage, so
`idMaterial::GetOcclusionStage()` returns `NULL` on every base-game surface and the feature
is completely inert there — even with `r_occlusionMaps 1`. It only does anything for
materials that explicitly opt in with the new `occlusionmap` keyword, i.e. mods / custom art.
Combined with the default-off cvar and the enhancement gate, there is no way for this to
alter a stock frame.

## How it reuses SSAO

The SSAO work already established the application contract in
[`ambientlight.frag`](../neo/shaders/ambientlight.frag) and
[`interaction.frag`](../neo/shaders/interaction.frag): *sample an occlusion scalar, then
multiply the ambient term (always) and the direct-light diffuse (scaled) by it, floored so it
never crushes to black.* An occlusion map is just a **different source** for that scalar — a
per-surface texture instead of the screen-space `u_ssao` buffer — so the plumbing is the same
multiply. When both SSAO and an occlusion map are active they multiply together
(`ambient *= ssaoAO; ambient *= mix(1, mapAO, strength)`).

The two-kinds-of-directionality rule from SSAO (§2 of [ssao-gtao.md](ssao-gtao.md)) applies
identically: darkening only ambient is *most faithful* but invisible in Doom 3's near-zero
ambient, so the map also pulls on **direct-light diffuse** (scaled by `r_occlusionMapDirect`,
default 0.9) — that is what makes it show. A baked-dark crease can't be re-lit by a moving
light, so the direct pull stays below full by default; `r_occlusionMapDirect 0` gives the
purist ambient-only mode.

## Authoring — the `occlusionmap` material stage

Two forms, mirroring `bumpmap` / `diffusemap` / `specularmap`:

```
// top-level shorthand
textures/mymod/crate
{
    bumpmap     textures/mymod/crate_local
    diffusemap  textures/mymod/crate_d
    specularmap textures/mymod/crate_s
    occlusionmap textures/mymod/crate_ao
}
```

```
// explicit stage form
textures/mymod/crate
{
    {
        blend occlusionmap
        map textures/mymod/crate_ao
    }
    diffusemap textures/mymod/crate_d
    bumpmap    textures/mymod/crate_local
}
```

The AO map is a grayscale texture; only the **red channel** is read (`1` = unoccluded, `0` =
fully occluded). It is sampled with the **diffuse UV set / texture matrix** (`var_TexDiffuse`),
the standard assumption that AO is baked in the same UV layout as the albedo — a per-stage
texture matrix on the occlusion stage is ignored.

Internally the stage is tagged `SL_OCCLUSION`, a new `stageLighting_t` that sorts *after*
`SL_SPECULAR` (so it never disturbs the low-end BUMP/DIFFUSE/SPECULAR ordering) and is
ignored by every legacy pass: the ambient/general passes only draw `SL_AMBIENT` stages, and
the interaction decomposition only handles BUMP/DIFFUSE/SPECULAR.

## Baking from existing assets

An offline baker generates AO maps from the geometry the game already ships — no new art
required. It ray-casts **self-occlusion** per model (folds, recesses, cavities), which is
exactly what screen-space SSAO can't see, so the two are complementary.

### Commands (offline)

```
bakeAO <model> [-size 0] [-rays 128] [-dist 0] [-contrast 1.0] [-skin skins/name]
bakeAOFolder <path>            // every .lwo / .ase / .md5mesh under a folder
```

**Skins.** Because output is keyed by model + surface index (not material), **one bake serves
every skin** of a model — the red exploding barrel (`skins/exp_barrel_red`) and a grey one load
the same `exp_barrel_s0.tga`. `-skin skins/name` is therefore rarely needed; it only changes
which material's bump map feeds the detail cavity (useful if a skin swaps the normal map). The
lazy auto-bake passes the drawing entity's skin for that reason.

- Writes one **8-bit grayscale** TGA (type 3, `R_WriteGrayTGA`) per drawn surface to
  **`generated/aomaps/<model>_s<N>.tga`** — a quarter of an RGBA map, and identical once loaded
  (the shaders read only `.r`; the loader expands gray to R = G = B, A = 255), so no post-bake
  conversion is needed before packing. (N =
  surface index; collision/nodraw surfaces are skipped).
- `-size 0` = auto (default): each surface is baked at its own texture resolution (diffuse, else
  bump, else any stage image), so every map is **1:1 with the shipped art** and never larger than
  can show — e.g. a Doom 3 head bakes the 256² face, 128² eyes, 64² teeth each at native size
  instead of a flat oversized sheet. Pass a positive `-size` to force one resolution on all.
- `-dist 0` = auto (25% of the model's longest bounds axis = "nearby geometry").
- Static models (LWO/ASE) bake their resident geometry. **MD5 characters** are instantiated in
  their reference/bind pose first (that snapshot carries the deformed geometry — the base MD5
  model exposes none) and baked the same way; concave organic geometry (folds, sockets, muscle
  overlaps) is exactly where baked self-AO pays off most. Keyed by the surface's persistent id
  (== mesh index), so a skin that culls a mesh still lines the rest up.
- No GL. The rays are cast **on the GPU** when one with Vulkan ray query is present
  (`r_occlusionMapBakeGPU 1`, the default; Vulkan builds only) and on a CPU thread pool over a
  uniform grid otherwise — see *GPU tracer* below. Seconds per model either way.

**Surface detail from the bump/normal map.** Geometric ray-AO only sees the low-poly mesh, so
the detail authored into the normal map (seams, rivets, vents, handles) would be invisible. The
baker also reads the material's bump stage (`GetBumpStage()` — in Doom 3 the bump *is* the
normal map, evaluated through `R_LoadImageProgram` so `addnormals`/`heightmap` programs work)
and adds a **cavity** term: per texel it samples the surrounding tangent-space normals, and
where they lean back toward the centre (a concave groove) the texel darkens — convex bumps
don't. This works even on flat panels, where perturbing the ray hemisphere alone shows nothing.
Multiplied into the geometric AO; strength via `r_occlusionMapBakeDetail` (0 = geometry only).
Surfaces with no bump stage fall back to geometry-only.

Only **drawn** surfaces are baked and used as occluders — collision/nodraw surfaces
(`textures/common/collision`, present on many props as a 2nd surface) are skipped. A
collision hull *encloses* the visual mesh, so including it as an occluder blackens whole
faces (worst on the tightly-wrapped bottom); excluding it is essential. Rays are also
backface-culled (only an occluder's front face counts) to avoid self-occlusion on hollow /
single-sided geometry.

**Translucent surfaces are excluded as occluders too** (`Coverage() == MC_TRANSLUCENT`, in
`AO_BuildGrid`). The eye iris decals (`deform eyeBall`, e.g. `leftpoppy`) and other see-through
blend stages sit a hair in front of the opaque face; as solid occluders they bake a near-black
patch onto it, which the deferred ReSTIR DI apply then multiplies the DI radiance by (G-buffer
`out_Material.a`) — so the eyes read black at `dude_rtx 2` even though DI lit them
(`r_rtReStirDIDebug 2` shows the radiance; the apply's occlusion term kills it). Non-eyewear
translucents are still baked as *targets*, because one that carries a lighting interaction (hair,
lit glass) legitimately applies its occlusion map in the forward pass.

**Eyewear is disregarded entirely** (`AO_SurfaceIsEyewear`: material name contains `glasses` /
`lense` / `visor` / `goggle`, or `/frame` under `models/characters/` = Swann's sunglasses rim).
The translucent-only rule above is not enough, because the lens/frame of a pair of glasses is
*opaque* (alphaTest — scientist `glasses`/`glasses2`, Swann `frame`): it both occludes the face
behind it *and*, because it reaches the DI G-buffer, carries its own baked map that darkens the
glasses themselves. So eyewear is skipped on all three fronts:
- **occluder** (`AO_BuildGrid`) — it never darkens the face behind it;
- **bake target** (`AO_BakeModelToCache`) — no map of its own is written;
- **runtime apply** (`RB_RHI_SurfaceOcclusionImage` returns `NULL` for eyewear) — the renderer
  refuses to bind *any* occlusion map to it, so a stale map still sitting in `z_baked_ao.pk4` /
  the savepath from an earlier bake cannot darken it. This makes the glasses fix take effect on
  rebuild alone, with no re-bake.

**Re-bake still required for the faces:** the *face* maps baked before this change hold the dark
patch the glasses cast as occluders; regenerate every model that wears eyewear / eye decals (all
heads, glasses wearers, Swann — `./bake_ao.sh +bakeAOFolder models/md5`) to clear it. `'/frame'`
is scoped to `models/characters/` so door and weapon frames keep their AO.

Algorithm: rasterize each triangle into the diffuse-UV texture space (renderbump's approach),
and for every covered texel integrate visibility over a **cosine-weighted Hammersley
hemisphere** (deterministic → reproducible, low-noise) around the interpolated normal, with a
bounded ray distance and linear falloff. Result is dilated across UV seams so bilinear/mips
don't bleed. Output orientation matches `renderbump` (same `st` rasterization, top-origin TGA).

### Auto-load (runtime) — resolution order

When `r_occlusionMaps` is on, a **model-entity** surface (prop/NPC — never the static world
BSP) resolves its occlusion map in this order (first hit wins), cached per surface:

1. **Explicit `occlusionmap` material stage** — artist/mod authored; always wins.
2. **Mod `_ao` convention** — if the surface's diffuse is `x/foo` (or `x/foo_d`) and a sibling
   `x/foo_ao` (`.tga`/`.dds`) exists in the VFS (base or a pk4), it's used. Lets a modpack drop
   in AO textures with no material editing.
3. **Generated per-model-surface bake** — `generated/aomaps/<model>_s<N>.tga`, our `bakeAO`
   output. Optionally lazily baked (`r_occlusionMapsAutoBake`).

**Keying is by model + surface index**, *not* material name — immune to **skins** (one bake
serves red/grey/any skin of a model) and to **materials shared across different models** (each
model-surface gets its own map, no collisions). The world-BSP gate keeps generated AO off map
geometry.

Identifying the surface at draw time is subtle: `surf->geo` there is a **per-light culled copy**
(`lightTris`), not the model's surface geometry, so it can't be matched by pointer. Instead the
resolver matches the (already skin/customShader-remapped) **draw material** back to a model
surface by remapping each surface's material the same way — reliable because the renderer sets
the draw material via the identical `R_RemapShaderBySkin`. Result cached by `(model, surfIndex)`.

For a **static** model the surfaces are walked on the base model and keyed by position. For an
**MD5** (dynamic) model the base model exposes no surfaces, so the resolver walks the entity's
**instantiated snapshot** (`idRenderEntityLocal::dynamicModel`, this frame's deformed copy) and
keys by each surface's persistent **id (== mesh index)** — the same id `bakeAO` wrote for the
bind pose, so the two agree even when a skin culls some meshes. The cache still keys by the
stable base-model pointer, not the per-frame snapshot.

### Lazy bake (dev convenience)

`r_occlusionMapsAutoBake 1` (default 0): the first time a model-entity surface with no map is
drawn, its model is baked on the spot and cached to disk (a one-time hitch per model), then
picked up normally. Meant for iterating in-editor; ship the pre-baked `generated/aomaps/` tree
for release.

## Cvars

| cvar | default | range | purpose |
|---|---|---|---|
| `r_occlusionMaps` | 0 | 0/1 | master toggle (GL3/Vulkan only; non-vanilla) |
| `r_occlusionMapScale` | 1.0 | 0–1 | AO-map strength on the ambient term (0 = off, 1 = full) |
| `r_occlusionMapDirect` | 0.9 | 0–1 | strength on direct-light diffuse, as a fraction of the scale (0 = ambient-only) |
| `r_occlusionMapsAutoBake` | 0 | 0/1 | DEV: lazily bake a missing model's AO on first sight (one-time hitch) |
| `r_occlusionMapBakeSize` | 0 | 0–4096 | baker: output texture resolution; **0 = auto** (per surface, match its diffuse/bump map size so the AO is 1:1 with the shipped art) |
| `r_occlusionMapBakeRays` | 128 | 1–4096 | baker: hemisphere rays per texel |
| `r_occlusionMapBakeDist` | 0 | 0–… | baker: max ray distance in world units (0 = auto) |
| `r_occlusionMapBakeContrast` | 1.0 | 0.1–8 | baker: contrast/gamma on the visibility term |
| `r_occlusionMapBakeDilate` | 4 | 0–32 | baker: seam padding in pixels |
| `r_occlusionMapBakeDetail` | 1.5 | 0–4 | baker: bump/normal-map cavity strength (0 = geometry only) |
| `r_occlusionMapBakeContact` | 1.0 | 0–16 | baker: buried-texel contact distance, world units (capped at 2% of the model's longest axis); a texel mostly blocked within it sits under a co-located sub-mesh and is skipped (0 = off) |
| `r_occlusionMapBakeGPU` | 1 | 0/1 | baker: cast the rays on the GPU (Vulkan ray query) when available, else the CPU; not archived; Vulkan builds only |
| `r_occlusionMapBakeThreads` | 0 | 0–256 | baker: CPU tracer worker threads (0 = all hardware threads); not archived |

Ambient strength = `r_occlusionMapScale`; direct-diffuse strength =
`r_occlusionMapScale × r_occlusionMapDirect`.

## UI

- **Enhancements tab** → *Ambient Occlusion* section: a "Baked Occlusion Maps" master
  checkbox, next to the SSAO toggle.
- **Developer tab** → *Occlusion Maps* section (under the SSAO tuning): the master toggle
  plus **AO Map Strength** (`r_occlusionMapScale`) and **Direct Light AO (map)**
  (`r_occlusionMapDirect`) sliders, each with a reset button. Independent of `r_ssao`.

Wired into the [quality presets](todo.md): **on for every tier except Potato**. (It was
originally left out, on the reasoning that it was inert on stock assets — but once AO is baked
for the shipped characters/props it is no longer inert, and it is effectively free at runtime, so
it belongs on by default anywhere the rest of the enhancement suite is on. Potato keeps it off to
stay closest to vanilla.) Still gated by `R_BackendSupportsEnhancements()`, so it does nothing on
the legacy backend regardless of preset, and does nothing where no baked/authored map exists.

## Implementation

- **Parse** — `occlusionmap` keyword (both forms) and `SL_OCCLUSION` /
  `GetOcclusionStage()` in [`Material.cpp`](../neo/renderer/Material.cpp) /
  [`Material.h`](../neo/renderer/Material.h).
- **Uniform** — `u_occlusionParms` (enable, ambient strength, direct strength) appended to
  the shared block, [`renderparms.glsl`](../neo/shaders/renderparms.glsl) +
  [`RenderParams.h`](../neo/renderer/rhi/RenderParams.h).
- **Apply** — sampler on **unit 10** in `ambientlight.frag` (ambient) and `interaction.frag`
  (direct diffuse).
- **Bind / drive** — `RB_RHI_DrawInteraction` in
  [`RhiWorld.cpp`](../neo/renderer/rhi/RhiWorld.cpp) fetches the material's occlusion image,
  sets `u_occlusionParms`, and binds unit 10. Entirely in the RHI path — the shared
  `RB_CreateSingleDrawInteractions` / ARB2 backend / `drawInteraction_t` are untouched.
- **tmu cache** — `MAX_MULTITEXTURE_UNITS` 8 → 16 so the bind cache covers unit 10 (unit 9 =
  SSAO); inert for the legacy path, whose reported fixed-function unit count is ≤ 8.
- **Cvars** — `r_occlusionMaps` / `r_occlusionMapScale` / `r_occlusionMapDirect` in
  [`RenderSystem_init.cpp`](../neo/renderer/RenderSystem_init.cpp) /
  [`tr_local.h`](../neo/renderer/tr_local.h).

## Status

- [x] Material parsing (`occlusionmap`, `SL_OCCLUSION`, `GetOcclusionStage`).
- [x] Uniform + both shaders (ambient + direct-diffuse application).
- [x] RHI bind/drive, cvars, Enhancements + Developer UI.
- [x] Offline baker (`bakeAO`, `bakeAOFolder`) → `generated/aomaps/`, static models + MD5.
- [x] Bump/normal-map detail cavity (`r_occlusionMapBakeDetail`).
- [x] **Per-model-surface keying** (`<model>_s<N>`) — immune to skins + shared materials; resolution
      order explicit stage → mod `_ao` → generated. Runtime resolves by surface geometry pointer.
- [x] In-engine validation: file cabinet + all barrels bake and apply (A/B `r_occlusionMaps` toggle
      changes the render); collision surfaces excluded.
- [x] **MD5 characters (bind-pose bake).** The base MD5 model is instantiated in its reference
      pose (global joint matrices rebuilt from the relative default pose) to get a static snapshot,
      which is baked and keyed by mesh id. Runtime resolves via the entity's `dynamicModel`
      snapshot. Validated on the imp (`imp_s0.tga`): full AO gradient, deep sockets/rib creases.
- [x] **Buried texels under co-located sub-meshes (2026-09-23).** Dark blotches on characters on
      every preset with `r_occlusionMaps` on (Low and up; Potato, which has it off, is clean).
      Cause: several character models carry a second mesh a fraction of a unit under the skin —
      the morgue zombie's `skeleton01` (skull + ribs, shown through its wounds), goggle shells over
      a face, armour over a body. Every hemisphere ray from the skin texel under it hits that mesh
      at 0.1–0.8 units, so the texel bakes hard black (visibility ~0.02 at any cull rule); the texel
      itself is hidden in-game, but bilinear/mip filtering bleeds the black onto the visible skin
      next to it. Fix: `r_occlusionMapBakeContact` (default 1.0 world units, capped at 2% of the
      model's longest axis, 0 = off) — a texel with more than half its rays hitting within that
      distance is *buried* and left unwritten, so seam dilation pads it from its visible neighbours
      and wide buried regions stay white. Offline check on the morgue body: 29 of the 30 blackest
      triangles flagged, 7.5% of random texels, real creases untouched. **Every map baked before
      this date (the three shipped `z_baked_ao*.pk4` packs) carries the artefact and must be
      re-baked** (`./bake_ao.sh +bakeAOFolder models/md5 +bakeAOFolder models/mapobjects
      +bakeAOFolder models/weapons`, then repack).
- [x] **Occluder winding (same day).** The baker built each occluder's geometric normal as
      `(v1 - v0) x (v2 - v0)`, the negative of the engine's `R_DeriveFacePlanes` convention
      `(v2 - v0) x (v1 - v0)`, so its front-face cull ran inverted (cavities were seen through
      their far wall instead of their near one). Corrected — small, mostly lightening change on
      the bakes; it was not the cause of the blotches above. The normal is also oriented against
      the summed vertex normals as a guard.
- [ ] Mass bake of every static prop (full `generated/aomaps` coverage) — see [todo.md](todo.md)
      "Bake AO for every possible asset".
- [ ] Optional: specular occlusion from the map (mirroring `r_ssaoSpecular`). Diffuse-only for now.
- [ ] Minor: `bakeAOFolder` extension match is case-sensitive (misses `.ASE`); bake such models by name.

## Baker implementation

[`neo/tools/compilers/aobake/aobake.cpp`](../neo/tools/compilers/aobake/aobake.cpp) (+ `.h`):
self-contained uniform grid + Möller–Trumbore ray/tri + Hammersley cosine hemisphere +
UV rasterization + seam dilation. `AO_BakeModelToCache()` is shared by the `bakeAO*` commands
(registered in [`Common.cpp`](../neo/framework/Common.cpp)) and the renderer's lazy path; it
dispatches static models straight to `AO_BakeGeoModel()`, and MD5 models through
`AO_InstantiateBindPose()` (rebuilds the global bind pose, forces non-deferred tangents +
`r_showSkel 0`, calls `InstantiateDynamicModel`, then bakes the snapshot). Runtime resolve +
per-surface cache: `RB_RHI_SurfaceOcclusion` / `R_ResetOcclusionMapCache` in
[`RhiWorld.cpp`](../neo/renderer/rhi/RhiWorld.cpp).

### GPU tracer

[`aobake_gpu.cpp`](../neo/tools/compilers/aobake/aobake_gpu.cpp) +
[`shaders/aobake.comp`](../neo/shaders/aobake.comp) (2026-10-28). The ray casting is the whole
cost of a bake; it now runs as a ray-query compute shader, one thread per texel, against a
hardware BLAS of the model's occluders (the same drawn, opaque, non-eyewear triangles the CPU grid
holds). Everything around it stays on the CPU and is shared with the CPU path: UV rasterization
enumerates the covered texel centres, the GPU returns visibility + the buried flag per texel, and
`AO_StoreTexel` applies the bump cavity, contrast and the darker-wins overlap rule.

- **Own headless device.** The tracer creates a Vulkan 1.3 instance/device of its own (no surface,
  nothing shared with the renderer), so it works under GL3, Vulkan, xvfb (`bake_ao.sh`) and the
  lazy auto-bake alike. It picks a discrete GPU over an integrated one and skips CPU
  implementations (lavapipe); with no ray-query GPU it prints why once and the CPU tracer runs.
  `bakeAOFolder` holds the device for the whole folder; `bakeAO` / the lazy path open it per model.
- **Same integral.** Same Hammersley directions (uploaded, not recomputed), same origin bias,
  distance falloff, contact test. The CPU's *front-face hits only* rule maps onto Vulkan's
  back-face cull: each occluder is uploaded wound so `(v1 - v0) x (v2 - v0)` is its oriented
  normal (Vulkan's front face is counter-clockwise seen from the ray origin).
- **Fallback.** A device error mid-surface redoes that surface on the CPU and the rest of the
  acquisition bakes on the CPU. Dispatches are capped at ~32M rays so a slow GPU stays under the
  Windows 2 s watchdog.
- **Output.** Not byte-identical to the CPU bake (the hardware intersector is watertight and
  exhaustive where the CPU grid walk steps in half cells), but the same to within 8-bit
  quantization: `tools/aobake/test_aobake_gpu.sh` traces synthetic models both ways with the
  CPU tracer copied verbatim out of `aobake.cpp` — mean |Δvis| ≤ 0.0003, 99th percentile
  ≤ 0.007, buried flags identical, plus a facing test proving the cull convention. Throughput
  there (RTX 3080 Ti vs Ryzen 9900X, 24 threads, 1M texels × 128 rays): **262 ms vs 3.36 s**
  (~13×, including the BLAS build). Folder bakes gain less: model load, bump load and TGA writes
  stay on the CPU.
