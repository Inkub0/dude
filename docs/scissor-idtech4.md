# idTech4 scissor semantics (and the RHI/Vulkan port)

Collected because scissor behaviour in idTech4 is subtle and has bitten us more than once
(most recently the Phase 3.2b batched depth prepass — see [gpu-offload-plan.md](gpu-offload-plan.md) §3.2b).
The renderer scissors *per surface* for overdraw control and *per portal* for correctness, and the
Vulkan backend has to replicate GL scissor state as dynamic state. This is the map.

## The mental model

A scissor rect clips rasterization to a screen-space rectangle. idTech4 uses it two ways:

- **Optimization (the common case):** limit a surface/light to its projected screen bounds so the GPU
  doesn't shade pixels the geometry can't cover. Widening such a scissor to the full view is *visually
  identical* — the geometry only rasterizes where it projects.
- **Correctness (portals):** geometry in another area, visible through a portal, is scissored to the
  portal opening's screen rect. Here the scissor genuinely clips geometry that projects **outside** the
  opening. Ignoring it can change the image (though in the depth prepass, nearer geometry usually
  overwrites the stray depth — do **not** rely on that; respect the scissor).

Because you can't cheaply tell "optimization" from "correctness" per surface, treat every scissor as
load-bearing.

## The data — where scissor rects come from

All rects are `idScreenRect` (`x1,y1,x2,y2`, inclusive), **viewport-relative** ("local inside renderView
viewport", `tr_local.h`). Width = `x2 + 1 - x1`.

| Rect | Source | Scope |
|---|---|---|
| `viewDef->scissor` (`tr_local.h`) | the whole view rect | the view's full scissor; `backEnd.currentScissor` is initialised to it at view start (`RhiBackend.cpp`) |
| `drawSurf->scissorRect` (ambient/depth surfaces) | `R_CalcEntityScissorRectangle(vEntity)` (`tr_light.cpp` ~`2408`, assigned ~`2011`) | **per render-entity** screen bounds ∩ its portal-chain visibility. **NOT** the full view — even a screen-filling entity's rect rarely equals `viewDef->scissor` exactly. **All surfaces of one entity share one rect** (the worldSpawn/BSP is one entity → all its surfaces share one rect). |
| `drawSurf->scissorRect` (interaction/lit surfaces) | the light's scissor (`tr_light.cpp:819`) | per light |
| light scissor | `R_CalcLightScissorRectangle(vLight)` (`tr_light.cpp` ~`1113`) | per light's projected bounds |

`r_useScissor` (default on) gates whether per-surface/-light scissors are *applied* at all; the rects are
still computed. When off, everything uses the full view.

⚠️ **The trap we hit:** assuming `drawSurf->scissorRect == viewDef->scissor` for "world" surfaces. It isn't —
it's the *entity* rect. An exact-match-to-view predicate collects **nothing**. Group by the rect instead.

## The RHI / Vulkan handling

- The frontend sets scissor with `RHI::SetScissor(x, y, w, h)` in **GL convention** (origin bottom-left),
  passing `viewDef->viewport.x1 + rect.x1`, `viewDef->viewport.y1 + rect.y1`, `w`, `h`. The loop only calls
  it when the surface's rect differs from `backEnd.currentScissor` (`RhiWorld.cpp` ~`1534`).
- The VK backend stores it in `scRect[]` and converts to Vulkan (top-left origin, plus the scene's
  **negative-height y-flip**) lazily in `ApplyDynState` (`VulkanBackend.cpp`), which also carries the
  viewport and polygon-offset depth-bias — all as **dynamic state** (`VK_DYNAMIC_STATE_SCISSOR/VIEWPORT/
  DEPTH_BIAS`), flushed on the next draw when `dynStateDirty`. Offscreen target passes (shadow maps) use a
  plain top-left viewport (no flip).
- GL scissor commands (`qglScissor`) are **NULL no-ops on VK** — scissor must go through the RHI or it
  silently vanishes (the same class of gap as [[vulkan-qgl-noop-state-gap]]).

## Consequences for GPU-driven / batched draws

One `vkCmdDrawIndirect`/`vkCmdDraw*` uses **one bound scissor** for all its primitives. So to batch surfaces
that have different scissors into fewer draws you must **group by scissor** and issue one indirect draw per
distinct rect, setting that rect first. In practice this batches well: surfaces of the same entity (and the
whole BSP) share one rect, so a scene of ~130–150 surfaces collapses to ~3–5 scissor groups.

Do **not** "simplify" by drawing the whole batch with the full-view scissor — that's the portal-correctness
hazard above, and it is not guaranteed pixel-identical.

The Phase 3.2b batched depth prepass (`r_vkBdaZfill 2`) implements exactly this: collect items + their
`surf->scissorRect`, reorder so each rect's items are contiguous, and pass `groups[]` (range + rect) to
`RHI::DrawZfillBatch`, which uploads the geometry once and draws per group. See
[gpu-offload-plan.md](gpu-offload-plan.md) §3.2b.

## Checklist when touching scissor

- Reading a surface's scissor? It's the **entity** rect, not the view rect. Compare/group by it; don't
  assume full-view.
- Adding a batched/indirect draw? **Group by scissor**; one bound scissor per draw.
- On the VK backend, is the scissor set through the RHI (`SetScissor`/`ApplyDynState`)? `qglScissor` is a
  no-op there.
- Restoring state after a custom draw that mutated `scRect`? Set `dynStateDirty = true` so the next real
  draw re-applies the caller's scissor.
- `r_useScissor 0` should behave like full-view everywhere — make new paths honour it.

## File references

- `neo/renderer/tr_local.h` — `idScreenRect`, `viewDef->scissor`, `drawSurf->scissorRect`
- `neo/renderer/tr_light.cpp` — `R_CalcEntityScissorRectangle`, `R_CalcLightScissorRectangle`, scissor
  assignment for ambient (~`2011`) and interaction (~`819`) surfaces
- `neo/renderer/rhi/RhiWorld.cpp` — `RB_RHI_FillDepthBuffer` per-surface scissor (~`1534`) + the batch
  collection/grouping/emit
- `neo/renderer/rhi/vk/VulkanBackend.cpp` — `SetScissor`, `ApplyDynState` (GL→VK conversion), `DrawZfillBatch`
