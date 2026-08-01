# Improvements over the classic engine + post-process stack

Every fidelity-affecting feature and the ordered post-process chain (Phase 11).
All default to the classic look unless noted. Hub: [vulkan-port.md](vulkan-port.md).
For deliberate deviations already shipped on the GL3 path, see
[readme-changes.md](readme-changes.md).

## "Improvements over the classic engine" settings section
This section is implemented as the **Enhancements** tab in the F10 settings menu
(GL3/Vulkan-gated) and is the deliverable of **Phase 3.5** — every toggle below is
built on the GL 3.3 `opengl3` backend first, then ported to Vulkan together in
Phase 4 (see [port-phases.md](port-phases.md)).

A dedicated section in the F10 settings menu collecting every
fidelity-affecting toggle, each defaulting to the classic look unless noted:
- soft particles (`r_useSoftParticles` — already in dhewm3, default on)
- shadow mapping / soft shadows (Phase 8)
- parallax occlusion mapping (Phase 9, default off)
- specular model (`r_shading`, fhDOOM-style): **default = the original Doom 3
  specular-falloff LUT** (baked half-angle curve, faithful — see interaction.frag).
  Opt-in live alternative: Blinn-Phong (close approximation of the LUT). It
  alters highlight shape/size — a look change, not free. (A classic-Phong mode
  was also shipped, then removed 2026-08-01: reflection-vector, wrong for
  Doom 3's half-angle model, redundant with Blinn-Phong.)
- uncapped framerate via tick interpolation (Phase 7)
- native-resolution console font scaling (standalone QoL; the console renders in
  virtual 640×480 coords today — scaled blurry at high res. UI-only change.)
- **film grain** (`r_postFilmGrain`, 0=off) and **chromatic aberration**
  (`r_postChromaticAberration`, 0=off): one fullscreen post pass over
  `_currentRender` after the 3D view, **before 2D/GUI — HUD unaffected**.
  Shader authored (shaders/postprocess.*), wired in Phase 3 Chunk F.
  Fidelity: look change, defaults off.

Debug aids (not fidelity-relevant, console cvars): `r_whiteWorld` renders all
diffuse maps as white to judge lighting on its own (implemented on the legacy
path; carry into the Phase 3 IR).

The section also inventories the **pre-existing dhewm3-era departures from 2004**,
so it's the single honest list of everything non-classic:
- widescreen main-menu scaling (`r_scaleMenusTo43`, default on)
- the `zWideGuis` widescreen GUI data patches (installed in base/, d3xp/, d3le/ —
  data-side, shown as detected info rather than a toggle)
- window-alpha fill for Wayland (`r_fillWindowAlphaChan`, cosmetic/compat)

## Phase 11 — DUDE post-process & screen-space effect stack

An ordered, opt-in effect chain over the new backend. Every effect defaults OFF
(improvements menu); strength 0 is an exact passthrough. Built as stages in one
framegraph, NOT ad-hoc passes. Effects group into tiers by the extra buffer they
need — this is also the build order:

**Cross-renderer guarantee**: every effect here (and every improvements-menu feature)
is RHI-level + shared GLSL, so it runs on **all three** renderers — modern GL 3.3,
Vulkan 1.1, vulkan-rt — not just one. Portability rule that makes this hold: **author
effects as fullscreen fragment passes, not compute** (GL 3.3 core has no compute; it's
GL 4.3+). A compute variant for speed on Vulkan is allowed only with a fragment
fallback for the GL backend. HDR fp16 (`R16G16B16A16_SFLOAT`) is a mandatory Vulkan
format, so the Tier-3 target works on the 1.1 baseline too. The ONLY profile-exclusive
rendering is ray tracing (Phase 10, vulkan-rt only) — nothing in this post stack.

**Performance mitigation (fragment-vs-compute).** The portability rule costs speed only
for tiled/separable effects (bloom down/up-sample, gaussian blur, SSAO gather, motion
blur), where compute shares a tile in workgroup memory instead of re-fetching
overlapping texels. Mitigation = **shared core math + two thin harnesses**: the effect
algorithm lives in one GLSL include; a fragment `main()` (fullscreen triangle, the
mandatory portable path, ground truth) and an optional compute `main()` (tiled
dispatch) both call it, so results match within fp tolerance and parity checks still
apply. The RHI dispatches compute when the backend supports it (compute is core in
Vulkan 1.0+, so BOTH Vulkan profiles — not just vulkan-rt; GL 3.3 always takes the
fragment path). **vulkan-rt** gets extra headroom: subgroup reductions for the gathers
and optional async compute to overlap post work. Only write compute variants for the
tiled effects that measurably benefit; SMAA/CAS/dither/tonemap stay fragment-only.
Proportionality: at Doom 3 scale the fragment path is typically sub-ms on modern GPUs,
so compute is a targeted win for heavy combined stacks / weaker hardware, not a rewrite.

- **Tier 1 — color buffer only** (cheap, land first, extend the Chunk F post pass):
  - **SMAA** — subpixel morphological AA; best fit for Doom 3 (sharp, no temporal
    ghosting/TAA blur).
  - **CAS** — contrast-adaptive sharpen (pairs after SMAA). FSR (EASU+RCAS) only if a
    render-scale option is ever added; not needed for perf, so deferred.
  - **Blue-noise dithering** — kill 8-bit banding in dark areas / post-tonemap;
    artifact removal, run last before output.
- **Tier 2 — needs depth (+normals)** (`_currentDepth` already exists):
  - **Screen-space contact shadows** — short depth-march toward the light; adds small
    contact shadows stencil/shadow-maps miss. Complements Phase 8.
  - **SSAO** — NOTE: Doom 3 has no baked AO (fully dynamic lighting), so this *adds*
    ambient occlusion to the ambient term, it doesn't swap anything. Needs a
    depth+normal source (normal prepass or reconstruct from depth).
- **Tier 3 — needs an HDR (fp16) render target** (the one real architectural add;
  allocate only when a Tier-3 effect is on, keeping the faithful LDR path default):
  - **HDR tonemapping** — conservative filmic/Reinhard highlight rolloff. Caveat:
    Doom 3 is LDR-authored (lights clamp at 1.0), so payoff is modest without bloom.
  - **Better bloom** — progressive down/up-sample bloom; looks right in HDR, so rides
    with tonemapping.
- **Tier 4 — needs a velocity buffer + previous-frame transforms**:
  - **Per-object motion blur** — per-surface motion vectors. Build alongside Phase 7
    (fixed-tick interpolation already tracks prev-frame transforms).

Canonical in-frame order: [SSAO, contact shadows folded into lighting] → 3D view to
HDR → motion blur → bloom → tonemap → SMAA → CAS → dither → 8-bit output → **then**
2D/GUI (HUD always excluded, as with the Chunk F film grain / chromatic aberration).

Fidelity: every effect is a look change (dithering the mildest — it removes an
artifact); all default off, all in the "Improvements over the classic engine" menu.
