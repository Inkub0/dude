# Intel Masked Software Occlusion Culling (MOC) — Fit Assessment

**Verdict.** MOC *can* be bolted onto dhewm-rt cleanly, but for **stock Doom 3 it is not worth it**:
the visportal system already does its job (better) for indoor content, it adds CPU cost to the
single-threaded front-end we are trying to *lighten*, and it points sideways from the ray-query
endgame. **It is filed here as a deferred, opt-in option to revisit for the Quake 4 port** and other
large/open-map content, where portals under-perform and MOC's Morrowind-style win actually applies.

Researched 2026-09-19. No code touched. See also `gpu-offload-plan.md`, `scissor-idtech4.md`.

---

## 1. What MOC is

Intel's [Masked Software Occlusion Culling](https://github.com/GameTechDev/MaskedOcclusionCulling)
is a CPU library that SIMD-rasterizes large opaque **occluders** into a *masked hierarchical depth
buffer*, then tests object bounding boxes (**occludees**) against it and drops the occluded ones
before they reach the GPU.

| Property | Detail |
|---|---|
| License | **Apache 2.0** — GPLv3-compatible (one-way combine into GPLv3 OK). Vendorable like NRD. |
| Build | C++, compiled (not header-only): core + AVX2/AVX-512 TUs, CMake. Runtime CPU dispatch. |
| SIMD | AVX-512 (experimental) / AVX2 / **SSE4.1** / SSE2. Fast paths are AVX2+. |
| API | `Create` → `SetResolution` (w×8, h×4) → `ClearDepthBuffer` → `RenderTriangles(occluders)` → `TestRect`/`TestTriangles` → `VISIBLE / OCCLUDED / VIEW_CULLED`. `SetNearClipPlane`, backface modes, optional `CullingThreadpool`. |
| Accuracy | **Approximate** — Intel's own README: *"leakage is part of the algorithm."* Not "perfect". |
| Status | ⚠️ Repo **archived 2026-09-07** — no upstream support/patches. Mature but you'd own it. |

The library's headline wins come from **OpenMW / Morrowind** (MSOC for MWSE, OpenMW graphics builds):
open world, thousands of small interactable meshes, **no portal system** — ~40 % of the scene removed,
+15–30 FPS in dense cities. That context is the whole story (see §2).

---

## 2. Why it is a poor fit for **stock Doom 3**

idTech 4 is the near-opposite of the engine MOC was built to rescue. Confirmed against the tree:

- **Visibility today = view-frustum + BSP visportals + scissor. No occlusion culling at all**, by
  design — `tr_light.cpp:1948`: *"no occlusion query — a light hidden behind a wall stays cheap and
  is culled at render time."* Portals *seal whole areas exactly* at compile time
  (`RenderWorld_portals.cpp:155` `FloodViewThroughArea_r`, `:566` `CullEntityByPortals`).
- **Low per-view entity counts.** Doom 3 shows *dozens* of entities per view, not thousands. MOC's
  payoff scales with objects-needlessly-submitted, which portals already keep low indoors. The only
  residual MOC could catch is **intra-area** occlusion (a prop behind a pillar in the *same* room),
  which is small in stock content.

### Frictions (all confirmed in-tree)

1. **CPU cost on the wrong stage.** MOC's occluder raster runs in the single-threaded front-end
   (`R_AddAmbientDrawsurfs`, `tr_light.cpp:2175`) — the exact stage `gpu-offload-plan.md` exists to
   *un*load. We are CPU-front-end bound; spending more CPU to save GPU is backwards unless the removed
   draws are heavy.
2. **Shadow-caster correctness trap.** The frustum-cull site `tr_light.cpp:2241` feeds the single
   `drawSurfs` list that *every* pass consumes (ambient, interactions, **stencil shadows**) — see
   `gpu-offload-plan.md` §"Increment 3 — STRUCK". An object occluded from the eye can still cast a
   shadow visible in-frame. Doom 3 is shadow-defined, so naive MOC culling would **drop shadows**.
   Safe use = cull only the primary-view/ambient contribution, *not* the shadow role — which is where
   much of D3's per-object cost lives. That makes it a careful integration, not a drop-in.
3. **SSE4.1 ceiling.** Our SIMD tops out at SSE4.1 (`CMakeLists.txt:516`, `Simd.cpp:79`) — zero AVX.
   MOC would run its *slowest* SIMD path. Raising the baseline to AVX2 breaks the "Potato runs
   anywhere" floor.
4. **Trajectory mismatch.** The stated GPU-driven-visibility endgame is **ray-query** against the
   TLAS we already build (`r_rtWorld`); `gpu-offload-plan.md` explicitly *struck* raster occlusion
   culling. A CPU raster occluder is a detour from that.
5. **Archived dependency** (see §1) for a marginal-value feature.

---

## 3. Where it **does** pay off — the Quake 4 case

MOC earns its keep exactly where portals fail: **large / open maps with long sightlines and geometry
that self-occludes within one area.** That is the Morrowind case, and it is why this doc exists:

- **Quake 4** — bigger, more open spaces than Doom 3; the future port ([quake4-port-phase]) is the
  primary reason to keep MOC on file. Scout early whether Q4 maps lean on portals as hard as D3's do;
  if they have big open areas, MOC (or ray-query occlusion) becomes genuinely useful.
- Outdoor Mars sections, sprawling **mod maps**, RoE open areas — same shape, lesser degree.

Even there, weigh MOC against **ray-query occlusion** (on-strategy, GPU-resident, more accurate). MOC
is the CPU stopgap; ray-query is the destination. If the RT path is mature by the time Q4 lands,
prefer ray-query.

---

## 4. Integration sketch (for when Q4 lands — not now)

The plumbing is genuinely low-risk *if scoped to the ambient pass only*:

- **Insertion point:** `R_AddAmbientDrawsurfs` at `tr_light.cpp:2241`, right where `R_CullLocalBox`
  already runs. The inputs MOC needs — `tri->bounds` + `vEntity->modelMatrix` — are already in hand,
  and already recorded for the GPU-cull prototype at `tr_light.cpp:2246`. Coarser per-entity variant:
  `CullEntityByPortals` (`RenderWorld_portals.cpp:566`).
- **Occluders:** large opaque BSP world surfaces (+ big static models). Portals already give a natural
  front-to-back area order for occluder submission.
- **Occludees:** per-surface / per-entity AABBs via `TestRect`.
- **Depth prepass already exists** (`RB_RHI_FillDepthBuffer`) if we ever want to seed occluders from it.
- **Guard rails:** `r_moc` cvar, **default off**; **ambient pass only** (never cull shadow casters —
  see §2.2); output-identical A/B against `r_moc 0`; benchmark on actual open maps before committing.
  Keep the SSE4.1 fallback; do **not** raise the engine SIMD floor for it.

---

> **Decision (2026-09-19):** Do **not** add MOC to stock Doom 3. Keep it as a deferred, opt-in,
> ambient-only experiment to evaluate during the **Quake 4 port**, and only if ray-query occlusion
> isn't already carrying that load by then.
