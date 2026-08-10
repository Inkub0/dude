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

- **Shadow *maps* now tessellate** (2026-08-07) — the 2D and cube shadow-map caster passes
  run the same PN + displacement as the lit passes (`shadow_sm.tesc/.tese`,
  `shadow_sm_cube.tesc/.tese`), so a shadow-mapped character casts from its **deformed** surface
  and the shadow hugs the smoothed body. See "Tessellated shadow maps" below.
- **Stencil shadows are CPU-generated from the base mesh**
  ([`tr_stencilshadow.cpp`](../neo/renderer/tr_stencilshadow.cpp)) → under a **stencil-only** light
  (parallel lights, out-of-budget point lights, or `r_shadowMapping 0`) a tessellated/displaced
  monster still casts an **un-tessellated silhouette**. This is architecturally unfixable in the
  CPU-stencil model (the shadow volume is a light-relative silhouette of the flat mesh, position-only,
  no surface to tessellate) — the industry answer is that displaced content uses shadow maps, which
  is exactly what the fix above does. Residual, documented; the elegant cure is the deform-once
  architecture below.
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
- **Phase 2 (shaders) — built (PN + normal-map displacement).** `tess.glsl`
  (PN-triangle eval + crack-free per-edge factor + `dudeTessDisplace`).
  `interaction` / `ambientlight` / `zfill` each got a `.tesc` + `.tese`; their `.vert`
  emit model-space pos+normal (and a bump texcoord) for the PN net. The tese linearly
  interpolates the varyings (matches the flat rasterizer) and recomputes `gl_Position`
  (`invariant`). **Displacement:** each tese pushes the PN position along the
  interpolated geometric normal by `r_tessDisplace` × a pseudo-height (Doom 3 ships no
  runtime height maps, so height ≈ `1 - bumpN.z` from the bump map's blue channel,
  sampled with `textureLod(...,0)` so it's derivative-free and identical across passes).
  All three depth-EQUAL passes (zfill/interaction/ambient) bind the same bump map on
  unit 1 with the same bump matrix and apply the same displacement, so the prepass and
  lit passes still agree bit-for-bit. `r_tessDisplace 0` = pure PN smoothing.
- **Phase 3 (controls) — built.** `r_tessellation` (0) / `r_tessLevel` (5) /
  `r_tessMinEdge` (0.72) / `r_tessMaxDist` (160) / `r_tessDisplace` (-0.25) / `r_tessDebug` (0). `RB_RHI_TessellateSurf`
  routes by **asset path** — only `models/characters/` and `models/monsters/` (real skinned
  actors); worldspawn BSP, all `models/mapobjects/` props, view-model, translucent, GUI,
  and the named facial sub-meshes are excluded. Emissive: only **purely** emissive surfaces
  (no lit stage) are excluded — a lit monster that also has a glow/FX blend stage (e.g. the
  imp's conditional "burning corpse" stages over its bump/diffuse/specular) still tessellates. **ImGui**: Enhancements tab →
  "Tessellation (only Vulkan)" = on/off + Level + Displacement (+ Distance); Debugging tab →
  Min Triangle Size + "Log Tessellated Materials" (`r_tessDebug`, prints `name (dm=… idx=…)`
  once each). **Quality presets** (`enhancementPresets[]`): tessellation on from **High** up;
  displacement (`-0.25`) from **Ultra** up. `r_tessDisplace` default is **-0.25** (so a
  hand-enabled toggle displaces; the lower preset tiers pin it to 0).

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

### Rigid headgear excluded + main-character heads included (2026-08-04)
Ground-truthed with `r_tessDebug 1` + `condump` (the log now prints
`material  <-  model` so a surface can be traced to its `.md5mesh`):
- **Rigid headgear** — helmets, goggles/visors, eyeglasses — is hard, thin, near-flat
  shell geometry. PN balloons it and inward displacement cracks the visor/lens open (the
  security guard's goggles broke visibly). It's not organic and gains nothing from
  tessellation, so these material substrings join the skip list: **`gog`** (security
  guard headgear under the regsec skin, `.../security/gog`), **`zsechead`** (the security
  head *shell* itself — `zsecurity/zsechead2`/`zsechead3`, shared by the living guard and
  the zombie-sec; the zsec zombie *body* is `dsecurity`/`zsheild`, not `zsechead`, so it
  still smooths), **`marsec`** (the mars-sec mask), **`helmet`** (`sarge2/helmet`…),
  **`glasses`** (`scientist/head02/glasses2` + its `glasses2_fx` lens pass). `zsgogs`/
  `zsgogs2` also match `gog`. None collide with a body/face material.
- **Main-character heads now tessellate.** NPC heads live under
  `models/md5/characters/npcs/heads/` (matched by `characters/`), but the hero def_heads
  (Betruger, Campbell, Swann, Sarge, player) live under `models/md5/heads/` — which the
  model-path gate missed. Added **`heads/`** to the gate; every `.md5mesh` whose path
  contains `heads/` is a character/zombie head (no props/world geometry), so it's safe.

### SSAO normal prepass tessellates too (2026-08-04)
SSAO (default `r_ssaoNormalBuffer 1`) reads per-pixel normals from a dedicated
**normal-prepass G-buffer** (`RB_RHI_NormalPrepass`, shader `gbuffer`). That pass drew
flat geometry, so on a tessellated character SSAO evaluated occlusion with the *depth* of
the rounded silhouette (`currentDepthImage` already tessellates) but the *normals* of the
faceted mesh — leaving faint polygonal AO hugging the old edges. Fixed by tessellating the
prepass identically: `gbuffer.vert` emits model-space pos/normal; new `gbuffer.tesc`/
`.tese` PN-subdivide + displace (same `tess.glsl` helpers, same bump on unit 0 with the
matching bump matrix, so displacement is bit-identical to `zfill.tese`) and barycentrically
interpolate the view-space tangent frame + texcoords for the fragment normal. Only active
when `r_ssao` + `r_ssaoNormalBuffer` + tessellation are all on. Inspect with `r_ssaoDebug 3`
(the normal buffer should read rounded, not faceted).

### Blood-overlay decals follow the mesh (2026-08-04)
`idRenderModelOverlay` blood decals are added to the monster's model as their own
(translucent) surfaces. They're routed through a **PN tess variant of the `generic`
blend shader** (`generic.tesc`/`.tese`; `RB_RHI_TessellateSurf(surf, /*forBlendPass*/true)`
in the blended stage draw, gated on the entity's **model path** so decals-on-monsters
count as monster geometry). Overlay verts are memset to zero and never got a normal, so
PN produced `normalize(0) = NaN` and the decal vanished — fixed in
[`ModelOverlay.cpp`](../neo/renderer/ModelOverlay.cpp) by copying the base vertex's
normal/tangents onto the overlay vertex. Decals now PN-follow the base (they carry no
bump, so they ride the silhouette but not the displacement — polygon offset hides the
small residual). Classifier gates on `models/md5/monsters/` `models/md5/characters/`
(the md5 file paths) via the `monsters/` / `characters/` substrings.

### Seam welding (2026-08-04) — `r_tessWeldSeams` (off by default) — **SUPERSEDED, now dead weight**
> Kept for the record only. The bake-time weld in `idMD5Mesh::BuildGpuSkinData` (see
> [gpu-offload-plan.md](gpu-offload-plan.md), "TBN source") replaced this entirely, and does it
> better: welding the *bind* normals means coincident verts share a weight run and therefore stay
> bit-identical at every pose, where this per-frame weld only fixed the pose it ran on — and was
> silently overwritten whenever GPU skinning was on. Its dot-product gate also rejects exactly the
> pairs that crack (the measured offender was 49.54°, just past the 0.7 ≈ 45° default).
> **Worth removing:** `tr_light.cpp:78` still lists "the tess weld" as a blocker for retiring the
> CPU tangent derive, so deleting these two cvars removes one of only three obstacles to a real
> Milestone-C win.

Doom 3 md5 meshes are built from mirrored / UV-split halves — coincident vertices that
get independent normals, so PN + displacement pull the seam open (displacement worse,
since the two sides also sample different UVs). `R_WeldSeamNormals`
([tr_trisurf.cpp](../neo/renderer/tr_trisurf.cpp)) averages coincident-vertex normals
whose normals are **near-parallel** (`dot >= r_tessWeldThreshold`, default 0.7) so the
mesh deforms as one piece; the threshold preserves genuine hard creases. Runs in
`idMD5Mesh::UpdateSurface` (md5 only — world lighting untouched), only when the cvar is
on (vanilla is byte-identical off). Closes PN seams fully; displacement across a UV seam
keeps a small residual (different heights). Threshold is a Debugging-tab slider.

### UV-seam displacement pinning (2026-08-09) — unconditional
The residual noted above turned out not to be small: it is what opens the visible gaps around
hands and shoulders. `dudeTessDisplace` reads its pseudo-height **at the vertex UV**, and a UV
seam is by definition a pair of position-coincident verts carrying *deliberately different*
texcoords. Measured on `imp.md5mesh`: **396 of 891 source verts (44%) sit on a coincident group,
and the UV distance inside a group is 0.36 median / 1.62 max** — the two halves of a seam sample
opposite ends of the atlas. They were never going to agree on a height, so they displace by
different amounts and separate. Welding the bind normals ([gpu-offload-plan.md](gpu-offload-plan.md))
fixed the *direction* the halves move in; nothing fixed the *distance*.

Fix: a per-vertex seam mask. `idMD5Mesh::StampTessSeamMask` writes 0 into `color[3]` for every
vertex of a `dupVerts` pair and 255 elsewhere; the vertex stages forward it as `var_ModelNormal.w`
(the varying was already tess-only, so no new attribute, no new stream, no RHI change), the tesc
passes it through, and each tese interpolates it barycentrically and scales `relief` by it. Both
halves of a seam therefore displace by exactly 0 and stay welded, while displacement ramps back to
full one triangle in. Mirror-seam verts need nothing — `UpdateSurface` replicates them by copying
the whole source vertex, texcoord included, so they already sample the same texel.

The mask must be identical in every pass or depth-EQUAL breaks, so all seven displacing chains
(`zfill` / `interaction` / `ambientlight` / `gbuffer` / `fog` / `shadow_sm` / `shadow_sm_cube`)
carry it; `generic.{vert,tesc,tese}` is left alone because the blend pass never displaced.

**Fidelity:** costs relief in a band around every UV seam, and at 44% seam verts that band is not
narrow. Shipped with no dial regardless — a partial pin leaves a proportionally smaller gap, which
is still a gap, so the only useful setting is the one that closes it; `r_tessDisplace 0` remains
the way to opt out of displacement entirely. The alpha byte is free on md5 base surfaces: verts are `Clear()`ed to colour 0, so vertex colour
would render them black, and no stock character material uses it (every `vertexColor` /
`inverseVertexColor` stage is world/terrain blending, a projected `DECAL_MACRO`, or an additive
spawn effect). RGB is untouched.

### Known prototype limitations (Phase 2+ follow-ups)
- **Static props excluded** (PN is wrong for hard-surface geometry). A curvature-aware
  scheme could re-admit rounded props but won't save boxes — deferred; opt-in later.
- **GUI / emissive / blend surfaces excluded** (they draw flat in a non-tessellated pass).
- **Displacement across a boundary between two separate md5 meshes** (e.g. a `def_head` neck)
  is still unpinned — `dupVerts` only relates verts inside one `deformInfo`. Would need a
  model-level position hash across meshes; unmeasured, and the single-mesh imp doesn't hit it.
- **Stencil-shadow silhouettes** come from the un-tessellated base mesh, for stencil-only lights
  only (shadow-mapped lights now tessellate — see below). Architecturally unfixable in stencil.

### Fog surfaces now tessellate (2026-08-07)
`RB_RHI_FogChain`'s interaction chains run at `DEPTHFUNC_EQUAL`, so a tessellated model's fog was
depth-rejected against the tessellated zfill depth → the model rendered un-fogged (dark silhouette
in fog). Fixed by giving the fog pass its own `fog.tesc/.tese` mirroring zfill (bit-identical
`invariant gl_Position`); the frustum-volume fill pass never tessellates. See
[arb-parity-checklist.md](arb-parity-checklist.md).

### Tessellated shadow maps (2026-08-07)
The 2D (`RB_RHI_ShadowCasterChain`) and cube (`RB_RHI_ShadowCasterChainCube`) shadow-map caster
passes now tessellate. They already streamed the full `idDrawVert` (`VL_DRAWVERT`) — same data as
zfill — so the fix was the established three-line pattern (`RB_RHI_TessellateSurf` +
`RB_RHI_SetTessParms` + `RB_RHI_TessBumpForZfill`, bump on unit 1) plus `shadow_sm{,_cube}.tesc/.tese`
reusing `tess.glsl`. The caster's displaced surface coincides with the receiver's lit surface by
construction (same global tess params + bump). The cube tese keeps `var_LightVec` a *vector* so
per-fragment radial `length()` stays exact. Fixes low-poly shadows for shadow-mapped lights on both
2D and cube. Inert with `r_tessellation` off and on GL3.

The **receiver** (`interaction.tese`) was also updated: it now recomputes the light-space projective
quantities (falloff depth, cookie/shadow UV, cube light-vector) from the *displaced* position instead
of barycentric-interpolating the flat values, so the receiver's shadow reference matches the displaced
caster — otherwise the caster-displaced/receiver-flat asymmetry reintroduces self-shadow acne.

**Flashlight routing fix.** The player flashlight is a narrow projected spot with `flashRadius 400`
(`weapon_flashlight.def`), which tripped the `r_shadowMapStencilRadius` default (255) "oversize → stencil"
cutoff — a rule meant for giant omni sun-lights. So the flashlight cast a *stencil* (un-tessellatable,
low-poly) shadow while every other projected light was shadow-mapped. Exempted the flashlight from the
oversize cutoff (`RhiWorld.cpp`, `!ictx.lightIsFlashlight`) so it takes the 2D-map path and its shadows
tessellate. Side effect: the flashlight shadow is now a soft shadow-map (with `r_shadowMapFlashlightBias`)
instead of a hard stencil edge — consistent with all other shadow-mapped lights.

## Roadmap — "deform once, draw everywhere" (the elegant architecture)

The current design **re-runs** PN + displacement in every pass that needs the deformed geometry
(zfill, interaction, ambient, gbuffer, fog, blend-decal, and now the two shadow-map passes). Each is
a `DEPTHFUNC_EQUAL`/shared-parms site that must stay bit-for-bit in lockstep — fragile (the fog and
shadow passes were both silently low-poly until patched), and it does the deform N times per model.

The stronger architecture is **deform once per frame per model, then every pass draws that one
buffer** with a plain vertex shader — one source of truth, shadows and all passes consistent for
free, and the per-pass replication (and its depth-EQUAL fragility) retired entirely. It also does
the deform *once* on the GPU instead of N times, which is the CPU/GPU-offload direction the engine
wants.

**Chosen mechanism: compute-shader tessellation into a per-frame device-local buffer.** A compute
shader reads the post-skinning `idDrawVert` ambientCache as an SSBO, runs the same PN + displacement
math, and writes an expanded (indexed) displaced vertex+index buffer; every pass then draws it via
`RB_RHI_StreamAmbient` handing back the deform buffer instead of the raw ambientCache. Compute is
Khronos's endorsed replacement for transform feedback and is the natural foundation for the broader
GPU-offload roadmap (GPU skinning, GPU culling, GPU shadow-volume extrusion all reuse the same
compute + SSBO + barrier plumbing).

**Prerequisite the RHI doesn't have yet:** the RHI is deliberately draw-only — no compute pipeline,
no `vkCmdDispatch`, no `STORAGE_BUFFER` usage, no compute queue, no compute↔graphics barriers, and
`CreateBuffer` makes only host-visible VERTEX/INDEX/UNIFORM buffers. Building the first **compute
lane** (dispatch + SSBO usage + a device-local shader-writable buffer path + barrier plumbing in
`RHI.h`/`VulkanBackend.cpp`) is the gating work. The one real algorithmic cost is re-deriving the
crack-free PN subdivision topology in a compute kernel (fixed-function tess auto-generates it today).

Transform feedback (`VK_EXT_transform_feedback`) would reuse the existing `.tese` verbatim as the
deform kernel and is the shortest path to a *prototype* of deform-once, but it rides a deprecated
extension and emits an unindexed triangle soup — a stepping stone toward the compute design, not the
destination. Build this when the engine grows its first compute lane; until then, the per-pass
tessellation (now covering shadow maps + fog) is the shipping approach.
