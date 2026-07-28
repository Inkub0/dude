# Screen-space ambient occlusion (GTAO) — design

Design reference for a **Ground-Truth Ambient Occlusion (GTAO)** pass on the GL 3.3
`opengl3` (RHI) backend, added during **Phase 3.5** alongside the shadow-mapping and
emissive-lighting work in [shadow-system.md](shadow-system.md). This document is the
**design**; sections graduate to "as-built" as the phases land (see §11 Status). Like
the rest of Phase 3.5 it is built on the RHI so it ports to Vulkan in Phase 4 rather
than being re-derived.

Like everything in the enhancement suite it is **opt-in and enhancement-gated**
(`R_BackendSupportsEnhancements()` → core profile only; nothing on the legacy ARB2
backend), and **off by default** (`r_ssao 0`). Original Doom 3 has no AO, so this is an
addition, not a reproduction — see the [fidelity note policy](shadow-system.md) framing.

Primary code (planned): the GTAO pass in
[`neo/renderer/rhi/RhiWorld.cpp`](../neo/renderer/rhi/RhiWorld.cpp) (after the depth
prepass), shaders `neo/shaders/ssao.*` + `neo/shaders/ssao_blur.*`, application in
[`neo/shaders/ambientlight.frag`](../neo/shaders/ambientlight.frag). Cvars:
[`neo/renderer/RenderSystem_init.cpp`](../neo/renderer/RenderSystem_init.cpp). UI:
Enhancements/Developer tab in
[`Dhewm3SettingsMenu.cpp`](../neo/framework/Dhewm3SettingsMenu.cpp).

---

## 1. Why — the "plastic" problem

Doom 3 models read as plastic for two reasons: (1) the **ambient / fill term is flat**
— there is no self-occlusion in it, so recessed areas receive the same fill as exposed
ones; and (2) specular has **no occlusion**, so it glints uniformly across a surface
regardless of cavities. GTAO addresses (1) directly — contact darkening plus, via the
**bent normal**, a *direction* for the ambient so it looks lit rather than painted on.
Point (2) is the stronger anti-plastic lever and is handled by the optional
**specular-occlusion** path (§7), gated separately.

## 2. Two kinds of directionality (the design's core rule)

The original requirement was "light directionality so it won't look strange when light
conditions change." Two distinct mechanisms deliver that, and only the first is
essential:

1. **Application-level (essential): AO modulates only the ambient term, never direct
   light.** Doom 3 is per-light additive with stencil/shadow-map direct occlusion
   already handled. If AO multiplied the final image (or every light), a dynamic light
   sweeping into a crease would leave a baked-dark corner — the exact artifact. By
   confining AO to the `ambientlight` pass (§6), a direct light shining into a crease
   still brightens it correctly; AO only darkens fill that no light justifies. This
   alone satisfies "won't look strange when light changes."
2. **Sampling-level (refinement): shade the ambient along the bent normal.** The ambient
   cube map already carries directionality; sampling it along the average *unoccluded*
   direction (the bent normal) instead of the geometric normal makes the ambient
   respond to where it can actually reach from. A quality bonus on top of (1), not a
   correctness requirement.

Because Phase 3.5 shadow mapping now handles direct occlusion well, ambient-only is the
*most faithful* mode. **But in practice ambient-only is invisible in Doom 3**: the game
lights almost everything with dynamic lights over a near-zero ambient, so there is no
ambient term to darken. Confirmed on real maps — AO on the ambient pass alone shows
nothing in normal play. So the shipped behaviour also applies AO to **direct-light
diffuse** in `interaction.frag`, scaled by `r_ssaoDirectLight` (default 0.9; set 0 for the
purist ambient-only mode). This trades a little correctness under *moving* lights (a light
sweeping into a crease can't fully re-light the AO baked into the surface) for AO that is
actually visible — an acceptable trade since most Doom 3 lights are static, and the floor
(§below) plus a sub-1.0 strength keep it from looking like dirt. The true fix for the
moving-light case is bent-normal SSDO (C.2), still pending.

## 3. Pipeline placement

The frontend depth prepass already captures scene depth to a texture:
`RB_RHI_FillDepthBuffer` ends by copying the depth buffer into
`globalImages->currentDepthImage` (`_currentDepth`) at
[`RhiWorld.cpp:739`](../neo/renderer/rhi/RhiWorld.cpp:739). GTAO's core input is
therefore **already free** — no new geometry pass.

```
depth prepass ──► currentDepthImage
                        │
   (r_ssao)             ▼
             ┌─ GTAO pass ─────► AO buffer (AO + bent normal)
             └─ bilateral blur ─► AO buffer (denoised)
                        │
per-light loop:  ambientlight pass samples AO buffer by gl_FragCoord ─► darken ambient
```

The GTAO and blur passes are **fullscreen**, modelled on the existing `postprocess`
program (a full-screen `.vert`/`.frag` pair; see the boot list at
[`GL3Shaders.cpp:44`](../neo/renderer/rhi/GL3Shaders.cpp:44)). They run once per view,
between the depth prepass and the interaction/ambient loop.

## 4. Inputs — depth and normals

- **Depth:** `currentDepthImage`, reconstructed to view-space position with the
  projection constants already in the RenderParams block.
- **Normals: a bump-mapped normal G-buffer (default), or reconstructed from depth**, both
  behind the single shader function `sampleViewNormal` in `ssao.frag`. This was the
  designed **seam**, and the G-buffer now fills it (Option B / §13): when `r_ssaoNormalBuffer`
  is on, a `RB_RHI_NormalPrepass` renders opaque geometry through the `gbuffer` shader,
  writing the bump-mapped view-space normal into an RGBA8 colour+depth target; `sampleViewNormal`
  becomes a texture fetch of that. Everything downstream — the AO hemisphere and the bent
  normal — inherits the normal-map detail. With `r_ssaoNormalBuffer 0` it falls back to the
  depth-derivative reconstruction (screen-space partials, best-of-3 tap). The G-buffer is
  also the foundation for parallax-occlusion / displacement later.

Reconstructed normals are faceted and slightly noisier than mesh normals, but adequate
for a first cut; GTAO's horizon search is fundamentally depth-driven and uses the normal
only to (a) restrict the integration hemisphere and (b) form the bent normal.

## 5. The GTAO pass

Horizon-based visibility integration (McGuire/Jiménez "Ground-Truth AO"):

- For each pixel, march a small number of **slices** (directions in screen space);
  along each, step outward sampling depth to find the **horizon angles** on both sides
  within a world-space `r_ssaoRadius`. The occluded fraction of the hemisphere is the AO
  scalar.
- Accumulate the **bent normal** from the same horizon samples (average unoccluded
  direction) — nearly free from data already gathered.
- **Range check / thickness** to avoid haloing across depth discontinuities; a rotated
  per-pixel noise/jitter decorrelates the slice directions (cleaned up by the blur).
- Sample budget is `r_ssaoSlices` (directions) × `r_ssaoSteps` (per direction); the main
  GPU-cost knob on 3.3 hardware (§8), both capped at the shader's MAX_SLICES / MAX_STEPS.
- **Output packing:** bent normal (view space, ×3) + AO (×1) into one RGBA8 target.
  (A scalar-only R8 variant is possible if the bent-normal path is ever dropped, but
  packing both keeps §6.2 available.)

## 6. Application in the ambient pass

The dedicated ambient shader is
[`ambientlight.frag`](../neo/shaders/ambientlight.frag): it computes a per-pixel
`globalNormal` from the bump map, looks up `u_ambientCubeMap`, and multiplies the
falloff/projection/diffuse/colour terms. AO plugs in here, sampled by `gl_FragCoord`
against the screen-space AO buffer:

1. **Scalar multiply (Phase C.1, landed):** `outRgb *= mix(r_ssaoFloor, 1.0, ao)`. This is
   the whole of the essential directionality guarantee (§2.1) — the ambient term is the
   only thing touched. The **floor** (`r_ssaoFloor`, default 0.03) is the anti-crush
   countermeasure: a fully-occluded texel darkens only to the floor, never to pure black,
   so AO can't blacken already-dark scenes. Wired in `RB_RHI_DrawInteraction`: for ambient
   lights only, `localParam0` carries enable/floor/uv-scale and the blurred AO binds on
   unit 7 (free on the ambient path), gated on `rhiSsaoAppliedThisView` (fullscreen primary
   view). Because Doom 3's ambient is near-zero in dynamically-lit areas, AO is a no-op
   there and only grounds the flat ambient-fill regions.
2. **Bent-normal biasing (Phase C.2):** rotate the cube-map sampling normal from
   `globalNormal` toward the bent normal by a strength factor before the
   `u_ambientCubeMap` lookup. Refinement per §2.2; keeps the detail normal-map influence
   while borrowing the coarse bent direction.

Sampling by `gl_FragCoord` works because the ambient pass draws forward geometry into
the same screen-space framebuffer the AO buffer was computed for.

## 7. Specular occlusion (gated fallback)

`r_ssaoSpecular` (default 0). The strongest anti-plastic lever and the answer for scenes
with little/no ambient fill (where §6 is invisible by design). Attenuates specular in
occluded areas using the AO term (optionally horizon/bent-normal aware against the
reflection direction and roughness). On the ambient path it is safe; extending it lightly
to direct-light specular is a mild fidelity departure and is the reason it is a separate,
off-by-default flag. Left stubbed through Phases A–C, enabled only if ambient-only
validation (§11 Phase D) shows it is needed.

### Sibling: baked occlusion maps

Art-authored per-material AO textures are a complementary feature that **reuses this same
application path** (multiply the ambient + scaled direct-diffuse by an occlusion scalar), just
sourced from a texture stage instead of the screen-space buffer. They stack with SSAO. See
[occlusion-maps.md](occlusion-maps.md) (`r_occlusionMaps`, the `occlusionmap` material keyword).

## 8. Performance levers (3.3 hardware)

- `r_ssaoSlices` × `r_ssaoSteps` — directions × steps-per-direction; the primary
  cost/quality dial (slices is the bigger lever).
- `r_ssaoResScale` — AO buffer resolution as a fraction of the screen (0.5 half … 1.0
  full), bilaterally upsampled into the ambient pass; 0.5 is ~4× cheaper and the default.
  Exposed as a Half / 3-4 / 4-5 / Full slider in Enhancements.
- `r_ssaoRadius` — larger radius = wider depth reads = worse cache behaviour; tune for
  look, not just cost.
- **Separable bilateral blur** (horizontal + vertical), depth-aware — 2·(2R+1) taps
  instead of (2R+1)², for the same reach at ~2.5× fewer taps. **No temporal accumulation**
  yet — a deliberate choice to avoid a dependency on TAA (which the backend does not have)
  and the ghosting it brings; see §12 for the temporal plan. Cost via `r_gl3GpuTime 1`.

**Measured cost** (2026-07-26, GTX-class GPU, half-res; whole-frame GPU time via the
A/B-difference method against a 2.10 ms no-SSAO baseline). Cost is linear in
slices × steps (~0.043 ms per sample-group over a ~0.27 ms fixed floor) and ~3.2× for
full-res; the per-fragment direct-light sample in `interaction.frag` is ~0.03 ms (free):

| config (half-res) | SSAO cost |
|---|---|
| 6 slices / 1 step (**default**) | ~0.53 ms |
| 4 / 6 (old default) | 1.29 ms |
| 8 / 6 | 2.25 ms |
| 4 / 6 at full-res | 4.12 ms |

Defaults were dropped from 4/6 to 3/4 on this basis (the look the user validated as
"medium"), ~40% cheaper. Later **retuned to 6 slices / 2 steps** — the same 12 sample-groups
(so the same ~0.75 ms) but reallocated toward angular coverage: ~16 well-distributed
directions instead of 6 clustered ones, which reads smoother on thin geometry (handrails,
pipes). Alongside it: radius 32→48, intensity 1.3→2.4, floor 0.15→0.03, direct-light
1.0→0.9 — a stronger, broader, more grounded look, further from source-faithful and firmly
opt-in.

A later GPU-time pass (SSAO measured ~35% of frame on a GTX-1070-class GPU, dominated by
the wide-radius horizon reads) trimmed it to the **current defaults**: steps 2→1 (the
1-step look was already validated as near-identical) and radius 48→36 for cache locality —
~6 sample-groups, roughly halving the horizon cost while keeping the tuned look.

The **normal G-buffer pass** (Option B) measured **~0.08 ms** (within noise) — the extra
opaque geometry pass is effectively free (Doom 3 geometry is low-poly, the gbuffer shader
is trivial). So the mooted optimizations (half-res normal buffer, MRT-merge into the depth
prepass) are **not worth doing** — the pass isn't a measurable cost.

## 9. Cvar reference (proposed)

| cvar | default | range | purpose |
|---|---|---|---|
| `r_ssao` | 0 | 0/1 | master toggle (GL3/Vulkan only; non-vanilla) |
| `r_ssaoIntensity` | 2.4 | 0–4 | AO strength (power/scale on the occlusion term) |
| `r_ssaoFloor` | 0.03 | 0–1 | min visibility when fully occluded (anti-crush floor) |
| `r_ssaoDirectLight` | 0.9 | 0–1 | AO strength on direct-light diffuse (0 = ambient-only) |
| `r_ssaoRadius` | 36 | 1–256 | world-space sampling radius |
| `r_ssaoSlices` | 6 | 1–8 | horizon-search directions per pixel |
| `r_ssaoSteps` | 1 | 1–12 | samples marched per direction |
| `r_ssaoResScale` | 0.5 | 0.25–1.0 | AO buffer resolution fraction (0.5 half … 1.0 full) |
| `r_ssaoBentNormal` | 1 | 0/1 | shade ambient along the bent normal (§6.2) vs scalar only |
| `r_ssaoBentStrength` | 0.5 | 0–1 | blend toward the bent normal for the ambient cube lookup (C.2) |
| `r_ssaoNormalBuffer` | 1 | 0/1 | bump-mapped normal G-buffer (extra geom pass) vs depth reconstruct |
| `r_ssaoSpecular` | 0 | 0/1 | also attenuate specular in occluded areas (§7) |
| `r_ssaoDebug` | 0 | 0–3 | visualise AO (1) / bent normal (2) / normal G-buffer (3) |

`r_ssaoSlices` / `r_ssaoSteps` were set to 3/4 after the Phase-D profiling pass (§8
Measured cost); `r_ssaoRadius` is still a first cut, retune to taste.

## 10. UI (landed)

`Dhewm3SettingsMenu.cpp`, following the shadow-map / emissive-light pattern:
- **Enhancements tab** — an "Ambient Occlusion" section with the `r_ssao` master toggle
  and a **Resolution** slider (`r_ssaoResScale`: Half / 3-4 / 4-5 / Full)
  (`DrawEnhancementsMenu`).
- **Developer tab** — an "Ambient Occlusion (SSAO)" section (`DrawShadowDebugMenu`) with
  the master toggle, a **Debug View** combo (`r_ssaoDebug`: off / AO buffer / bent
  normals), and, gated on `r_ssao`, AO Intensity + Direct Light AO + Floor + Radius
  sliders (with reset buttons), Directions (slices) + Steps sliders, and Bent-Normals /
  Specular-Occlusion checkboxes. All live-updating; the tab is disabled on the legacy
  backend.

## 11. Status / phasing

- [x] **Phase A — plumbing (landed, inert).** Cvars `r_ssao*` (§9) in
  `RenderSystem_init.cpp` / `tr_local.h`; `CreateRenderTarget` gained an `IF_RGBA8`
  colour path (`GL3Backend.cpp`, was depth-only) with `GL_COLOR_ATTACHMENT0` / `GL_RGBA8`
  defines in `GL3Local.h`; and `RB_RHI_SSAOPass()` hooked after `RB_RHI_FillDepthBuffer`
  in `RhiWorld.cpp`, gated on `r_ssao` + `R_BackendSupportsEnhancements()` (no-op body).
  Builds clean; nothing changes with default cvars.
- [x] **Phase B — GTAO pass (landed).** `ssao.*` (horizon search → AO in R + view-space
  bent normal in GBA), `ssao_blur.*` (depth-aware bilateral), `ssao_debug.*` (overlay);
  all three registered in `GL3Shaders.cpp` and validated. `RB_RHI_SSAOPass` (RhiWorld.cpp)
  runs the two passes into pooled RGBA8 targets after the depth prepass, fullscreen
  primary view only, half-res by default; `RB_RHI_SSAODebugOverlay` blits at end-of-view
  under `r_ssaoDebug` (1 = AO, 2 = bent normal). The depth-capture gate now also fires for
  `r_ssao`. `RB_RHI_ForgetTexBinds` after the passes keeps the per-light binds correct.
  Not yet consumed by lighting — visible only via `r_ssaoDebug`.
- [x] **Phase C.1 — apply (landed).** Both `ambientlight.frag` (full strength) and
  `interaction.frag` (direct-light diffuse + optional specular, scaled by
  `r_ssaoDirectLight`) sample the AO buffer and multiply by `mix(r_ssaoFloor, 1.0, ao)` —
  floored so it never crushes to black. The AO buffer is bound once on **unit 9** for the
  whole light loop (`RB_RHI_DrawWorld`); `RB_RHI_DrawInteraction` sets localParam0
  (enable/floor/uv) and, for direct lights, localParam1 (strength / specular-occlusion),
  gated on `rhiSsaoAppliedThisView`. Direct-light application is what makes AO visible at
  all in Doom 3 (§2). Added `r_ssaoFloor` + `r_ssaoDirectLight` (cvars + Developer sliders);
  `r_ssaoSpecular` now actually drives direct-light specular occlusion.
- [x] **Normal G-buffer (Option B, landed).** `r_ssaoNormalBuffer` (default on):
  `RB_RHI_NormalPrepass` renders opaque geometry through the `gbuffer` shader into an RGBA8
  colour+depth target (new backend `CreateRenderTargetColorDepth`), writing bump-mapped
  view-space normals; `ssao.frag` samples them on unit 1 (via the `u_windowCoord.x` flag) in
  place of depth reconstruction. Removes faceting and picks up normal-map detail (Debug View
  3 / `r_ssaoDebug 3`), at the cost of one extra opaque geometry pass. Bump image per surface
  via `idMaterial::GetBumpStage()`; TBN from the per-surface `modelViewMatrix`. The seam
  (§4) is now filled; foundation for parallax-occlusion / displacement.
- [x] **Phase C.2 — bent-normal directional shading (landed).** `ambientlight.frag` blends
  the ambient cube lookup direction from the surface normal toward the bent normal by
  `r_ssaoBentStrength` (default 0.5, gated on `r_ssaoBentNormal`). The bent normal (AO
  buffer GBA, view space) is rotated to world with the inverse view rotation
  (`bentView * mat3(u_modelViewMatrix)`, `u_modelViewMatrix` = `worldSpace.modelViewMatrix`
  set on the ambient path). **Caveat (as predicted):** only bites where there is ambient
  light, which is sparse in Doom 3, so the effect is subtle — a polish, not a headline.
  Developer-tab strength slider added.
- [~] **Phase D — profile & tune (in progress).** Profiled with `r_gl3GpuTime` (§8
  Measured cost): SSAO ≈ 0.53 ms at the 6/1 default (half-res, ~6 sample-groups; a later
  GPU-time pass dropped steps 2→1), cost linear in slices × steps, ~3.2× for full-res,
  direct-light sampling ~free. Landed: defaults retuned to 6/1 (via 3/4 → 6/2), and the denoise made
  **separable** (H+V) for ~2.5× fewer taps.
  Remaining: temporal accumulation (§12) if AO is ever wanted on weak hardware.

## 12. Open items / risks

- **No-ambient scenes:** ambient-only GTAO is invisible where a scene has no ambient
  light at all (correct/faithful, but it is why §7 exists). Which regime the problem
  scenes fall into is only knowable once Phase C is on screen.
- **Reconstructed-normal quality:** faceting on curved/thin geometry may pepper the AO;
  the blur and the future normal G-buffer (§4 seam) are the mitigations.
- **Half-res edges:** bilateral upsample can leak across silhouettes; depth-aware
  weights and a full-res fallback (`r_ssaoResScale 1.0`) cover it.
- **Temporal accumulation (planned optimization).** The real way to cut SSAO cost is to
  amortize samples across frames: reproject the previous frame's AO by depth (no motion
  vectors needed for a static world; camera reprojection from the view matrices) and blend
  with the current few-sample AO, so effective quality rises without more samples/frame.
  This is the modern GTAO approach and would let slices/steps drop further (or weak GPUs
  run AO cheaply). It is **not** a cache — screen-space AO is view-dependent and recomputes
  every camera move, so frame-to-frame "skip if unchanged" only helps a static camera.
  Deferred because it needs a history buffer + reprojection and risks ghosting on fast
  motion / disocclusion (needs a depth-based rejection clamp); the backend also has no TAA
  to share history with. Revisit if AO needs to run on low-end hardware.
- **View weapon excluded from SSAO.** The view weapon renders with a depth hack (compressed
  depth range so it never clips walls), so its `_currentDepth` depth doesn't reflect its true
  position and the horizon search reads far background geometry (desk edges) as false
  occluders on the hand/gun. Fix: the normal prepass writes an **AO mask** in the G-buffer
  alpha (0 for `weaponDepthHack` surfaces, 1 otherwise) and applies the weapon/model depth
  hack so the weapon rasterizes with the same coverage as the main view; `ssao.frag` skips
  the horizon search where the mask is 0 (leaves it fully unoccluded). Requires the normal
  G-buffer (`r_ssaoNormalBuffer`, default on); with it off, the weapon artifact returns.

## 13. Future / nice-to-have

- **Correct self-occlusion on the view weapon (parked).** The weapon is currently *excluded*
  from SSAO (§12) rather than shaded — so the hand/gun get no contact shadowing of their own.
  Doing it *correctly* is possible and not a dead end; the key opening is that **the depth
  hack only modifies the projection, not the modelview**, so `modelViewMatrix · position`
  still yields the weapon's true eye-space position — we just don't store it today. The clean
  approach is a small dedicated weapon-only pass:
  1. Store the weapon's **true view-space depth** (from the un-hacked modelview) alongside its
     normal — needs one more channel/attachment (the RGBA8 normal buffer is full: RGB normal
     + A mask), e.g. an MRT second target or an R32F/RG16F weapon-depth buffer.
  2. Run GTAO on the weapon **in isolation** — reject non-weapon horizon samples via the mask
     so it can only self-occlude (finger creases, gun detailing) and can't read the background;
     use the true depth so the radius is at the right world scale.
  3. **Composite** that onto the weapon pixels in place of the current "fully unoccluded" output.
  Cheap (weapon is tiny geometry over a small screen area); moderate complexity (weapon-depth
  channel + a masked SSAO variant + composite). Subtle payoff — do it during a nice-to-have pass.
- **MRT-merge the normal pass into the depth prepass** — see §8; not worth it (the pass is
  ~free) but noted for completeness, and it would also give a natural home for the weapon-depth
  channel above.
- **Temporal accumulation** — see §12; the real lever only if AO is ever wanted on low-end GPUs.
- **Normal G-buffer simplifications (refinements).** The normal prepass treats perforated
  (alpha-tested grates/fences) surfaces as solid — they write full-quad normals rather than
  punched-out, so their AO is slightly off. Polygon offset, the per-surface bump texture
  matrix, and the weapon/model depth hack ARE handled (matching the depth prepass). It is a
  full second opaque geometry pass — mergeable into the depth prepass via MRT later (blocked
  on the backbuffer-depth / MSAA-blit question, hence the standalone pass for now).
- **Transparencies / decals** are not in the depth prepass the same way; AO is a
  world-surface effect and does not apply to them — matches how the depth capture is
  already used.
