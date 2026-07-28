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

- Writes one grayscale TGA per drawn surface to **`generated/aomaps/<model>_s<N>.tga`** (N =
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
- Pure CPU + filesystem (no GL), accelerated by a uniform grid; seconds per model.

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
