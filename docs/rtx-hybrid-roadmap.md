# DUDE RTX — the Hybrid roadmap (and the road to Full RT)

The strategic umbrella over the three RT sub-roadmaps
([rtx-shadow-roadmap.md](rtx-shadow-roadmap.md), [rtx-reflections.md](rtx-reflections.md),
[rtx-animated-blas.md](rtx-animated-blas.md)). It answers one question: **what do we build, in what
order, so that a shippable "Solid Hybrid RT" version also pays down the eventual "Full RT with
Monte-Carlo lights" renderer instead of throwing work away?**

## Two destinations, one direction

- **Solid Hybrid RT (the next release).** Raster still computes the base image; RT *augments* it with
  physically-correct shadows, occlusion, and reflections. Every RT effect is a gated tier
  (Ultra Nightmare), the game stays playable with RT off, and the raster path is never removed.
- **Full RT / path tracing (months out, the north star).** Raster lighting is *replaced* by traced
  light: primary visibility + Monte-Carlo direct lighting (many lights) + multi-bounce global
  illumination + reflections/refractions, accumulated over frames and denoised. Q2RTX / Portal RTX
  class. Ships as an additional "Full RT" tier beside the hybrid one, not as a rewrite that deletes
  the raster renderer.

**Guiding rule:** prefer the hybrid step whose machinery *is* a piece of the path tracer. Grade every
feature by Full-RT reuse and build the high-reuse ones first.

## Why Doom 3 is a realistic target

- Light geometry (low-poly rooms), few lights per scene — the exact conditions path tracers thrive in
  (Q2RTX runs fully path-traced on far weaker GPUs than an RTX 3080 Ti).
- **FSR2 is already integrated** (motion vectors + jitter + temporal resolve, docs/fsr-temporal-pipeline.md):
  render RT at a lower internal resolution and upscale — the standard way these renderers hit framerate.
- The **G-buffer already exists** (`gbuffer.frag`: view normal + roughness/metalness MRT + per-pixel
  motion vectors) — exactly the guide buffers a denoiser needs.
- All scene geometry is already a single ray-traceable TLAS: static world (R2) + animated monsters
  (R3.5) + movers. A path tracer needs precisely this.

## The linchpin: a denoiser + temporal framework

Soft shadows, RTAO, glossy reflections, and GI all trace ~1–few samples per pixel → noise → unusable
raw. **Every realistic RT effect needs a denoiser.** It is the single most-reused thing we can build:
100% carried into Full RT.

- **Choice: NVIDIA NRD** (Real-Time Denoisers) — open source, Vulkan-integratable, runs on any RT GPU
  incl. the 3080 Ti. `SIGMA` denoises shadows; `ReBLUR`/`ReLAX` denoise AO / GI / reflections. The
  shadow roadmap already earmarked "STBN + SIGMA" for soft shadows — same decision.
- **Reuses what we have:** the G-buffer normal/depth + FSR2 motion vectors are NRD's guide inputs; the
  `ssr_temporal` / `ssao_temporal` history ping-pong is the pattern for the accumulation pass;
  `RB_RHI_TemporalReproj` already computes the camera reprojection.
- Alternative (no dependency): a hand-rolled SVGF-style temporal+à-trous denoiser. More work, lower
  quality. Keep NRD as the plan, SVGF as the fallback.

## Reuse ledger (the honest accounting)

| Hybrid feature | Ships in Solid Hybrid RT | What's REUSED in Full RT | What's RETIRED in Full RT |
|---|---|---|---|
| RT sun shadows (R3 ✅) | yes | the AS/ray-query foundation | the sun-only special case (folds into NEE) |
| Animated BLAS (R3.5 ✅) | yes | **all** — monsters in the TLAS is mandatory for PT | nothing |
| Geometry table + attribute fetch (RR0/RR1 ✅) | yes | **all** — PT fetches attributes at every hit | nothing |
| **RR4 bindless materials** | yes | **all** — PT evaluates a material at every hit | nothing |
| **Denoiser + temporal (NRD)** | yes | **all** — PT is unusable without it | nothing |
| **RT soft shadows (all lights)** | yes | ~**90–100%** — this *is* PT direct lighting (NEE) | nothing (becomes the direct-light stage) |
| **RTAO** | yes | ~**80%** — same hemisphere sampling + denoise as GI | the AO output (GI supersedes it) |
| RT reflections (RR2/3/5/6) | yes | infra (materials, world geo, denoiser) | the dedicated SSR+RT pass (becomes a specular bounce) |
| RT GI (bridge, H6) | optional | **all** — it already *is* the indirect path | nothing |

Takeaway: **soft shadows, RTAO's machinery, materials, and the denoiser are pure down-payments.**
Reflections give the least Full-RT leverage (the *pass* is transitional), so they rank lower in the
build order even though they're the most eye-catching in isolation.

## Build order

Numbered `H*` phases. Each is a shippable, gated increment; each (except the reflection tail) is a
Full-RT down-payment.

- **H0 — foundations (DONE / in progress).** TLAS with all geometry (R2/R3.5 ✅), geometry table +
  attribute-fetch validator (RR0/RR1 ✅), per-material colour (RR3 ✅), reflections MVP (RR2 ✅).
- **H1 — RR4 bindless materials.** `VK_EXT_descriptor_indexing` + a runtime descriptor array of the
  resident textures + a per-geometry material/texture index in the geometry table. The RT shader (and
  every future RT pass) samples the real diffuse/normal/emissive at a hit's `st`. *Prerequisite for
  everything below.* See [rtx-reflections.md](rtx-reflections.md) RR4.
- **H2 — denoiser + temporal framework (NRD).** Integrate NRD into the VK backend; wire the G-buffer +
  motion vectors as guide inputs; stand up the history/accumulation pass and a validator on a synthetic
  noisy signal. Nothing user-visible yet — it's the substrate H3/H4/H5 render into.
- **H3 — RT soft shadows, all lights.** Extend `r_rtSunShadows` (one hard sun ray) to **area-sampled**
  shadow rays for every light (a few rays to random points on the light's area → penumbra), denoised
  with SIGMA. Replaces/augments the shadow-map + stencil path per light. *Highest Full-RT reuse — this
  becomes the path tracer's next-event estimation.* Extends [rtx-shadow-roadmap.md](rtx-shadow-roadmap.md) R5.
- **H4 — RTAO.** Cosine-weighted hemisphere occlusion rays against the TLAS, denoised (ReBLUR),
  darkening the ambient term. Replaces SSAO (which misses off-screen occluders, like SSR). *Machinery
  is ~80% of RT indirect diffuse — the GI seed.*
- **H5 — RT reflections finish.** RR5 (world reflections: rebuild the static world BLAS with full
  attributes + materials, shade world hits → **replaces the env-probe/snapshot glass system**) and RR6
  (glossy via roughness-driven blur + reflection denoise, temporal, SSR↔RT seam softening).
- **H6 — RT GI (bridge to Full RT).** Extend the RTAO ray (H4) from "did I hit something" to "gather the
  incoming radiance at the hit" (one indirect diffuse bounce), denoised (ReLAX/ReBLUR). This is the
  moment the hybrid scene starts looking *lit by physics* — and it's already the indirect stage of the
  path tracer.

**"Solid Hybrid RT" release = H1–H5 shipped** (RT sun+soft shadows on all lights, RTAO, textured
reflections incl. the world), on the RR4 materials + NRD denoiser, gated to Ultra Nightmare. H6 (GI) is
the optional headliner if it lands in time.

## Full RT end-state (what changes, what carries)

When Full RT is built (a separate future phase), it *reuses* H1/H2/H3/H4/H6 wholesale and adds:

- **Primary-ray path tracing** (or keep the raster G-buffer as primary hits — "hybrid deferred PT" — to
  save the primary trace; a common shortcut that keeps the raster rasteriser earning its keep).
- **ReSTIR DI** — reservoir importance-sampling so hundreds of Doom 3 point/projected lights can be
  direct-lit in one sample/pixel. The H3 shadow ray *is* the visibility term it needs.
- **ReSTIR GI / multi-bounce** — H6's single bounce extended to a path, spatiotemporally reused.
- Lighting and shadowing **unify**: there is no shadow pass; a light is lit iff the sampled ray reaches
  it. Doom 3's lights become emitters/analytic lights the sampler queries. (This is the
  "light casting / shadow casting" question — in Full RT it's one operation.)

What gets **retired**: the dedicated SSR+RT reflection pass, SSAO, the shadow-map/stencil tiers (for the
Full-RT tier only — the hybrid + raster tiers stay for non-RT hardware and lower presets).

## Non-negotiables carried from the hybrid work
- Everything gates on `SupportsRayQuery()` + its cvar; raster/GL3/non-RT paths untouched and default.
- Validator-first (the R2/R3/R3.5/RR pattern): each noisy RT effect ships a `*_test` cvar that diffs the
  GPU result against a CPU reference before it renders.
- FSR2 upscaling is the perf backstop; internal-res scaling is expected for the RT tiers.
