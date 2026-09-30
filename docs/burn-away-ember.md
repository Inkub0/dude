# Monster burn-away: the ember band

How the stock Doom 3 corpse dissolve confines its ember/fire texture to the receding edge
of the body, why the RHI (GL3 + Vulkan) path lit the *whole* corpse instead, and the fix.
Hub: [vulkan-port.md](vulkan-port.md). Related: the tessellation depth-EQUAL notes in
[tessellation.md](tessellation.md) (ember vanishing under displacement).

## Stock mechanism (materials/monsters.mtr, `models/monsters/imp/imp`)

The burn-away is pure material data driven by `parm7` (`SHADERPARM_TIME_OF_DEATH`), no
engine code. Two conditional stages precede the bump/diffuse/specular:

```
{   // 1: skin coverage
    if parm7
    blend gl_zero, gl_one              // no colour write
    map models/monsters/imp/imp_dis.tga
    alphaTest 0.05 + 0.3 * (time - parm7)
}
{   // 2: fire
    if parm7
    privatePolygonOffset -1            // stage-only offset: "behind the other stages"
    blend gl_one, gl_zero              // opaque write of the ember texture
    map models/monsters/imp/imp_dis.tga
    alphaTest 0.3 * (time - parm7)
}
```

Both stages alpha-test the **same** dissolve map at two thresholds that sweep up over
time, so at any instant the fire threshold (lower) passes on a superset of the skin
threshold's texels. What separates them is depth:

1. **Depth fill** (`RB_T_FillDepthBuffer`): a perforated material draws *every* live
   alpha-tested stage, and `RB_PrepareStageTexturing` applies each stage's
   `privatePolygonOffset`. Stage 1 seals the skin region at depth `D`. Stage 2 is biased
   by `r_offsetUnits * -1` (= +600 units with the default `-600`, i.e. pushed **away**), so
   under `DEPTHFUNC_LESS` it only lands where stage 1 did not: the thin band where
   `0.3t < dis <= 0.05 + 0.3t`, sealed at `D + bias`.
2. **Interactions** (bump/diffuse/specular) draw at `DEPTHFUNC_EQUAL` with no stage offset
   and match only `D` -- the skin.
3. **Fire stage** draws at `DEPTHFUNC_EQUAL` *with* its offset, so it matches only the
   band. The lit skin recedes, and an ember rim a few texels wide trails the edge. That is
   the vanilla look (user screenshot, 2026-09-30: imp corpse in Mars City, ember only on
   the silhouette of the vanishing mesh, the rest of the body still normally lit).

The `alphaTest` gap of `0.05` is the band width, in dissolve-map units.

## What the RHI path did

- `ef49a685` ported per-stage `privatePolygonOffset` into `RB_RHI_RenderShaderPasses`
  (`RB_RHI_StagePolyOffset{Begin,End}`), but the RHI depth prepass never applied it to its
  alpha-tested stage draws. Both thresholds therefore sealed at `D`; the fire stage was
  then biased **off** that depth, failed EQUAL, and vanished.
- `043a4786` "fixed" the vanishing ember by excluding `DEPTHFUNC_EQUAL` stages from the
  per-stage offset. Now the fire matched `D` everywhere the *lower* threshold passed, i.e.
  over the entire still-visible body: the corpse glowed all over, on top of the lit skin,
  and the skin never receded ahead of the fire. That was verified against a
  misremembered reference ("the whole body glows in vanilla") and shipped.

## Fix (2026-09-30, `fix/burn-away-ember-band`)

Restore stock parity on both sides of the EQUAL test:

- **Depth prepasses** bracket each live alpha-tested stage draw with
  `RB_RHI_StagePolyOffsetBegin/End`: the zfill (`RB_RHI_FillDepthBuffer`,
  [RhiWorld.cpp](../neo/renderer/rhi/RhiWorld.cpp) perforated loop) and the merged gbuffer
  prepass (`r_ssaoMergeNormal`, the sealed scene depth on Vulkan High+), whose perforated
  loop mirrors the zfill. `End` restores the material-level `MF_POLYGONOFFSET` bias or
  clears it, so nothing latches into the next draw on Vulkan's persistent dynamic bias.
- **Stage pass**: `RB_RHI_StageWantsPolyOffset` no longer excludes EQUAL stages.
- The helpers moved from file-static to shared declarations in
  [RhiTess.h](../neo/renderer/rhi/RhiTess.h) (the existing RhiWorld/RhiBackend two-TU header).

Tessellation composes with this unchanged: polygon offset is rasterizer state applied on
top of whatever the tess/deform-once chain produced, and the prepass and stage pass already
displace bit-identically ([tessellation.md](tessellation.md)). Material-level
`MF_POLYGONOFFSET` composes the same way it always did (prepass applies it too).

Vulkan note: `SetPolygonOffset` is a dynamic depth bias, always enabled in every pipeline
with `(0,0)` as "off" ([vulkan-backend.md](vulkan-backend.md)); `units` maps to the
constant factor in minimum-resolvable-depth steps, so the same bias on the same triangle
yields the same depth in both passes -- the same property the decal offset relies on.

Other monsters use the identical two-stage pattern (wraith, zombies, ...), so they all
change together. Weapon stages with `privatePolygonOffset` are drawn at LESS and are
unaffected by the EQUAL change.

## Verification (user, in-game)

Kill an imp on Vulkan **and** GL3, watch the body dissolve:

- Ember/fire texture only in a thin rim along the receding edge; the still-present skin is
  lit normally, not tinted orange.
- The skin visibly recedes *ahead* of the fire (fire trails the skin edge, never covers it).
- Repeat with `r_tessellation 1` and `r_ssaoMergeNormal 0/1` on Vulkan: same result
  (prepass variants). `r_offsetunits` at its default `-600`.
- Blood decals on walls and the view weapon unchanged (material-level offset path).
