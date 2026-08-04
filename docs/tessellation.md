# GPU tessellation (character & prop mesh smoothing) — design

Design reference for **hardware-tessellated enemies and props** on the Vulkan (RHI)
backend. Same enhancement framing as the rest of the M7 suite (HDR in
[hdr-pipeline.md](hdr-pipeline.md), SSAO in [ssao-gtao.md](ssao-gtao.md), SSR in
[ssr.md](ssr.md)): built on the RHI, **opt-in and off by default**, driven by a config
slider.

**Vulkan-only — and why.** GPU hardware tessellation is *not* a Vulkan exclusive (it has
been in OpenGL 4.0 and D3D11 since ~2010). But the `opengl3` backend requests a **GL 3.3
core context** ([`glimp.cpp:452`](../neo/sys/glimp.cpp)), which is below the GL 4.0
tessellation feature level. Bumping GL3 to a GL4 path is out of scope, so in *this*
codebase tessellation is genuinely Vulkan-only. The Vulkan backend already bakes primitive
topology into its pipeline key ([`VulkanBackend.cpp:4262`](../neo/renderer/rhi/vk/VulkanBackend.cpp)),
so a patch-list + tessellation-state variant is a contained extension rather than a rewrite.

## What tessellation actually buys us here

Plain tessellation subdivides each triangle into more **coplanar** triangles — pixel-identical
output — unless the new vertices are moved. Doom 3 ships **no height/displacement maps**; all
its surface detail lives in tangent-space normal maps. So the useful forms are:

1. **PN-triangles / Phong tessellation** *(base feature).* Curves the surface using the
   existing per-vertex normals (cubic Bézier for position, quadratic for normal), rounding
   out low-poly silhouettes — imp shoulders, pinky snout, pipe props — with **no new art
   assets**. This is the real visual win.
2. **Normal-map displacement** *(optional layer).* Derive a pseudo-height from the bound bump
   map and push the generated vertices along the interpolated normal. Adds surface relief but
   is fiddly and can look wrong; conservative strength, separately gated.

Scope: **enemies (md5 skinned) + static props.**

## The constraint that dictates the whole design

Interactions run with **`GLS_DEPTHFUNC_EQUAL`** against a separate **depth prepass** (zfill,
`DEPTHFUNC_LESS`) — see [`RhiWorld.cpp:54`](../neo/renderer/rhi/RhiWorld.cpp) and
[`RhiWorld.cpp:1012`](../neo/renderer/rhi/RhiWorld.cpp). **Any** vertex movement (PN curving
*or* displacement) applied only in the lit interaction pass will fail the depth-equal test
against the flat prepass depth → the monster's lit surfaces vanish or z-fight.

Therefore tessellation **must be applied identically in the zfill depth prepass and the
interaction pass**, with a **deterministic** per-surface tess factor (distance-based LOD is
fine, but both passes must compute the same value from the same inputs). This is not optional;
it is the core correctness requirement of the feature. It means the tess `.tesc`/`.tese` pair
is shared by *both* the `zfill` and `interaction` pipelines.

## Fidelity note

This is an opt-in enhancement, **off by default = bit-for-bit vanilla** (no patch pipeline is
ever built, the device feature is left disabled). When on:

- **Stencil shadows are CPU-generated from the base mesh**
  ([`tr_stencilshadow.cpp`](../neo/renderer/tr_stencilshadow.cpp)) → a tessellated/displaced
  monster casts an **un-tessellated silhouette**. Usually hidden; visible on close silhouettes.
  Shadow volumes are **not** tessellated initially (accepted mismatch, documented here).
- **Silhouette reshaping.** PN smoothing rounds iconic monster shapes; displacement adds
  crawling relief if the pseudo-height is aggressive. Both ship with conservative defaults.
- **Tangent basis.** The evaluation shader must re-interpolate and renormalize normal, tangent
  and bitangent per generated vertex, or the normal-mapped lighting shifts/crawls.

See the [fidelity note policy](shadow-system.md).

---

## Phase 1 — pipeline plumbing  *(planned)*

Make the Vulkan backend *able* to build a tessellation pipeline variant; no visual change yet.

1. **Device feature.** Query `tessellationShader` (and `maxTessellationGenerationLevel`,
   `maxTessellationPatchSize`) at device-select; enable it only when present. Absent → the
   whole feature disables gracefully (cvar clamps to 0).
2. **Shader module table.** Extend the per-shader record (currently `vert` + `frag`,
   [`VulkanBackend.cpp:2393`](../neo/renderer/rhi/vk/VulkanBackend.cpp)) to optionally carry
   `tesc` + `tese` modules, loaded from `shaders/spv/<name>.tesc.spv` / `.tese.spv` when they
   exist.
3. **Offline compile.** Extend [`compile_spv.py`](../neo/shaders/compile_spv.py) so its suffix
   filter accepts `.tesc`/`.tese` (glslang detects the stage from the suffix; the
   `invariant gl_Position` injection stays vertex-only). Targets `vulkan1.4` like the rest.
4. **Pipeline creation.** When a shader has tess modules: add the two stages, attach a
   `VkPipelineTessellationStateCreateInfo` (`patchControlPoints = 3`), and force input-assembly
   topology to `VK_PRIMITIVE_TOPOLOGY_PATCH_LIST`. Add **one pipeline-key bit** for the tess
   variant so cached flat/patch pipelines don't collide.
5. **Descriptors.** Bind the bump map into the **tessellation-eval** stage's descriptor flags
   (displacement samples it in the TES).

**Key files:** [`VulkanBackend.cpp`](../neo/renderer/rhi/vk/VulkanBackend.cpp) (module table,
`CreateShaderModule` loader, pipeline builder, key), [`compile_spv.py`](../neo/shaders/compile_spv.py),
[`RHI.h`](../neo/renderer/rhi/RHI.h) (any new pipeline-desc field).

## Phase 2 — shaders  *(planned)*

1. **`interaction.tesc`** — compute the per-edge/inner tess factor. Screen-space edge-length
   LOD, clamped to `r_tessLevel` and `maxTessellationGenerationLevel`, faded out past
   `r_tessMaxDist`. **Deterministic** so zfill matches.
2. **`interaction.tese`** — PN-triangle position (cubic Bézier from the 3 corner positions +
   normals) and quadratic normal; re-interpolate + renormalize tangent/bitangent; optional
   `#define`-gated displacement sampling the bump-derived height along the interpolated normal,
   scaled by `r_tessDisplace`.
3. **`zfill` tess variant** — the *same* control/eval logic (shared include) so the depth
   prepass produces identical positions. Depth-only, no lighting.

Reuse the existing Vulkan shader prelude ([`prelude.vk.glsl`](../neo/shaders/prelude.vk.glsl)).

## Phase 3 — routing + controls  *(planned)*

1. **Classification.** Tag enemy/prop interaction surfaces (via
   [`MaterialIR`](../neo/renderer/rhi/MaterialIR.h) / entity parms) and route **only** those
   through the tess pipeline; world/BSP geometry stays flat.
2. **Cvars** (in [`RenderSystem_init.cpp`](../neo/renderer/RenderSystem_init.cpp)):
   - `r_tessellation` — master on/off (default `0`).
   - `r_tessLevel` — subdivision cap / slider (e.g. `1`–`8`).
   - `r_tessDisplace` — displacement strength `0..1` (`0` = PN smoothing only).
   - `r_tessMaxDist` — distance beyond which tess factor falls to 1 (LOD/perf guard).
3. **UI.** Slider in the Enhancements tab
   ([`Dhewm3SettingsMenu.cpp`](../neo/framework/Dhewm3SettingsMenu.cpp), `enhancementOptions[]`).
   Wire into a Vulkan-only **Ultra Nightmare** preset tier (name reserved for Vulkan-only tiers).

---

## Risks / open questions

- **Depth-equal prepass (critical).** Covered above — the single thing that silently breaks the
  feature if the zfill pass isn't tessellated identically. Prototype this first.
- **Cost scales per visible monster × tess factor**, re-run every frame on CPU-skinned md5
  geometry. `r_tessLevel` + `r_tessMaxDist` are the throttle; profile with `r_gl3GpuTime`'s
  Vulkan equivalent.
- **Shadow silhouette mismatch** (stencil shadows from base mesh) — accepted for v1; tessellating
  shadow volumes is a possible follow-up.
- **Perforated/alpha-tested surfaces** in the prepass — verify the tess variant still honours the
  coverage the depth prepass seals ([`RhiWorld.cpp:2410`](../neo/renderer/rhi/RhiWorld.cpp)).

## Suggested first step

Prototype **props-only PN, no displacement**, with `zfill` + `interaction` sharing the tess
pair, as a fast visual + depth-equal sanity check before extending to md5 enemies and the
displacement layer.

---

## Status

- **Phase 1 (pipeline plumbing)** — **built.** `tessellationShader` feature gated
  (`haveTessellation`); `ShaderRec` carries optional `tesc`/`tese`, loaded when
  `shaders/spv/<name>.tesc.spv` + `.tese.spv` exist; descriptor set layouts include
  the tess stages; `PipelineDesc::tessellate` drives a patch-list +
  `VkPipelineTessellationStateCreateInfo` variant on its own pipeline-key bit.
- **Phase 2 (shaders) — PN only, built; displacement pending.** `tess.glsl`
  (PN-triangle eval + crack-free per-edge factor). `interaction` / `ambientlight` /
  `zfill` each got a `.tesc` + `.tese`; their `.vert` emit model-space pos+normal for
  the PN net. The tese linearly interpolates the varyings (matches the flat
  rasterizer — PN only reshapes the silhouette) and recomputes `gl_Position`
  (`invariant`, so the three passes and the prepass agree under depth-EQUAL). All
  passes that draw these surfaces under depth-EQUAL are covered: **zfill (prepass),
  interaction, ambientlight.** Displacement (`u_tessParms.z`) is wired through the
  UBO but unused (strength forced 0).
- **Phase 3 (controls) — built.** `r_tessellation` (0) / `r_tessLevel` (4) /
  `r_tessMinEdge` (0.72) / `r_tessMaxDist` (512) / `r_tessDebug` (0). `RB_RHI_TessellateSurf`
  routes by **asset path** — only `models/characters/` and `models/monsters/` (real skinned
  actors); worldspawn BSP, all `models/mapobjects/` props, view-model, GUI/emissive/
  translucent, and the named facial sub-meshes are excluded. **ImGui**: Enhancements tab →
  "Tessellation (only Vulkan)" = on/off + Level (+ Distance); Debugging tab → Min Triangle
  Size + "Log Tessellated Materials" (`r_tessDebug`, prints `name (dm=… idx=…)` once each).

Branch: `feat/vulkan-tessellation`.

**Verified so far (2026-08-04):** clean build; Vulkan init validation-clean; a full
intro-map load with `r_vkValidation 1` + `r_tessellation 1` `r_tessLevel 8` runs to a
clean shutdown with only the pre-existing benign "attribute not consumed" warnings —
no VUID errors, no pipeline-creation failure, no crash. **Visual verification (does it
smooth silhouettes; winding/cull correct; PN hard-edge cracks; any depth-EQUAL
dropouts on lit surfaces) is still pending — needs an in-engine look.**

### Verified visually (2026-08-04, mars_city1)
- PN smoothing works — low-poly blockiness gone on NPCs/props at the default level.
- **TES winding `cw` is correct** — no inside-out/culling issues.
- **Emissive/GUI dropout found + fixed:** monitors, the info kiosk GUI and emissive
  panels rendered dark and shimmered when tessellated. Cause: those surfaces are
  sealed at the PN-displaced depth by the tessellated prepass but **redrawn flat** by
  their own depth-EQUAL pass (`RB_RHI_RenderShaderStages` ambient/emissive stages, and
  the GUI pass), which isn't tessellated → the flat redraw failed depth-EQUAL. Fix:
  `RB_RHI_TessellateSurf` now excludes any material with a GUI or an `SL_AMBIENT`
  stage; only pure lit surfaces (interaction stages) tessellate.

### Hard-surface inflation + eye-bulge fix (2026-08-04)
- **Static props ballooned.** PN assumes vertex normals encode curvature; hard-surface
  props (file cabinets, crates, chairs) carry lighting-smoothed normals on flat panels, so
  PN inflated them like balloons. This is inherent PN behaviour, not a shader bug. First fix
  gated on `IsDynamicModel() != DM_STATIC`, but a knocked-over moveable becomes a dynamic AF
  ragdoll and leaked it (`chairs/chair3`). Final fix: **gate by asset path** — only
  `models/characters/` and `models/monsters/` tessellate; every `models/mapobjects/` prop is
  excluded regardless of model state.
- **Face-skin distortion.** On heads that bake the eyes into the head-*skin* mesh (e.g.
  `scientist/head02/oldscihead02`), PN distorts the eye-socket/nose region — and the ears you
  want smoothed share that one material, so it can't be name-gated. Handled with the size
  gate instead: `r_tessMinEdge` default **0.72** leaves the dense head detail flat while
  coarse body/limb triangles smooth. Slider lives in the Debugging tab for tuning.
- **NPC eyes bulged** through the lids — the eyeball is a tiny high-curvature cluster,
  the same PN over-inflation in miniature. An offset-clamp relative to edge length did
  *not* isolate it (the eye offset isn't large relative to its own small edges). Fix
  instead: an **absolute minimum edge length** — `dudeTessEdgeFactor` returns factor 1
  for any edge shorter than `r_tessMinEdge` model-space units (default 4), so fine dense
  clusters (eyeballs, detailed features) render as the original flat mesh while big
  low-poly silhouette triangles still subdivide. Crack-free (per shared edge) and
  identical `u_tessParms.w` across all three passes, so depth-EQUAL holds. Tune
  `r_tessMinEdge` **up** (6/8/12) if eyes still bulge, **down** if faces under-smooth.
- **Facial small-bits excluded by name.** The size gate alone couldn't isolate the eyes
  without also flattening other fine detail (ears, fingers), so the small convex facial
  sub-meshes are filtered directly in `RB_RHI_TessellateSurf` by material-name substring
  (case-insensitive): **`eye`, `teeth`, `tongue`, `mouth`, `jaw`, `lashes`, and the whole
  `characters/common/` folder**.
  - Monsters + player name them directly (`cacoeye`, `cacodemon_mouth`, `pinky/teeth`,
    `mtongue`, `zjaw01`, `.../eyes`…). The only non-facial `"eye"` hits (`skcubeyellow`,
    hell `eyeskin` walls) are static/world, already excluded upstream.
  - Human NPC eyes/teeth/tongue are the shared `models/characters/common/` materials
    (`left*`/`right*` eyes, `teeth*`, `tongue`) — all facial bits, no body geometry.
  - `"lashes"` (not `"lash"`) is used so it doesn't match the commando's muzzle flash
    (`mflash`).
  The head *skin* (ears) is a separate material and still tessellates. With this,
  `r_tessMinEdge` can stay low (default **0.66**) to catch ears/fingers while the facial
  bits stay put.

### Known prototype limitations (Phase 2+ follow-ups)
- **Displacement not yet applied** (PN silhouette smoothing only).
- **Static props excluded** (PN is wrong for hard-surface geometry). A curvature-aware
  scheme could re-admit rounded props but won't save boxes — deferred; opt-in later.
- **GUI / emissive / blend / fog surfaces excluded** (they draw flat in a
  non-tessellated depth-EQUAL pass).
- **PN hard-edge cracks** at UV/smoothing seams (tiny gap at the NPC arm/hand seam).
- **Stencil-shadow silhouettes** come from the un-tessellated base mesh (accepted).
