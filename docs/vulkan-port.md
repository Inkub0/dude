# dhewm-rt: Vulkan Raster Port Plan

**Goal:** a faithful modern Vulkan port of the dhewm3 renderer — same look (depth prepass,
stencil shadow volumes, per-pixel ARB2-style lighting), classic Doom 3 game data, done
incrementally in this codebase with the game staying playable (on GL) at every step.

Surveyed 2026-07-20 at master `455b88e` (1.5.5). Status refreshed 2026-08-02 at
`f7f6170c` — Phase 3/3.5 complete, Phase 4 planned in detail
([vulkan-backend.md](vulkan-backend.md)).

This is the hub. The plan is split across focused documents so each can be loaded on its own:

| Document | Contents |
|---|---|
| [port-architecture.md](port-architecture.md) | Code survey, hardware baseline & backend switch, `vulkan-rt` profile, known risks, prior art |
| [port-phases.md](port-phases.md) | Detailed Phase 0–10 plan, Material IR, mod compatibility scope |
| [vulkan-backend.md](vulkan-backend.md) | **Phase 4 milestone plan** (M0–M7): what the Vulkan backend inherits, structural gaps, decisions, risks |
| [port-improvements.md](port-improvements.md) | "Improvements over the classic engine" toggles + Phase 11 post-process stack |
| [shadow-system.md](shadow-system.md) | **As-built** shadow-mapping + emissive-lighting reference (Phase 3.5, GL3) |
| [known-bugs.md](known-bugs.md) | Open GL3-backend bugs + resolved log |
| [readme-changes.md](readme-changes.md) | Deliberate divergences from a faithful port (e.g. the far-plane sky) |

## Non-goals

- replacing the Doom 3 material system
- converting to deferred rendering
- removing stencil shadows
- requiring Vulkan 1.2+ hardware for the compatibility renderer (baseline is 1.1)
- changing game assets

## Phase status

Detail for each phase is in [port-phases.md](port-phases.md) (feature phases 8–11 also
in [port-improvements.md](port-improvements.md)).

| Phase | Status |
|---|---|
| 0 — Groundwork (CMake option, `r_graphicsAPI` cvar) | **done** |
| 1 — Narrow the funnel (immediate-mode → `idImmediateMode`) | **done** |
| 2 — Shader modernization (shared GLSL, ARB→GLSL transpiler) | **done** |
| 2.5 — Material IR | **done** (Chunk D) |
| 3 — RHI abstraction + GL 3.3 core backend | **done** — Chunks A–G plus cinematics, screenshots, and ImGui all run on the core backend; subview depth bug resolved ([known-bugs.md](known-bugs.md)). The **default flip** (opengl3 default, legacy → `opengl-legacy`) is **deliberately deferred** until after Phase 4 bring-up — legacy stays the easy-reach reference during Vulkan parity work |
| 3.5 — GL3 enhancement suite (pre-Vulkan) | **done** — shipped far beyond the original three-item list: specular tuning, shadow mapping (projected + point cube, cache), SSAO/GTAO, baked AO maps, HDR pipeline, PBR/GGX, SSR (+glass), FXAA/SMAA, film grain/chroma, emissive surfaces, item glow, smoke dark-blend, soft particles, quality presets Potato→Nightmare (+ classic-menu integration). Only POM stayed unbuilt — deliberately, it remains Phase 9 (experimental, marginal on stock art) |
| 4 — Vulkan backend | **planned — next up.** Milestone plan M0–M7 in [vulkan-backend.md](vulkan-backend.md) (2026-08-02) |
| 5 — Validation & polish (the Vulkan payoff) | not started |
| 6 — Modern Vulkan device profile (`vulkan-rt`) | not started |
| 7 — Uncapped framerate (fixed-tick + interpolation) | **largely done ahead of schedule** — `com_interpolate` (fixed 60Hz sim + render interpolation of view/weapon/world entities/lights) shipped + verified; `com_maxFPS` limiter |
| 8 — Shadow mapping (optional, per-light) | **substantially built on GL3 in Phase 3.5** (projected 2D + point cube maps, adaptive res, static cache, perforated casters, oversize→stencil) — see [shadow-system.md](shadow-system.md); Vulkan port + Poisson/cascades pending |
| 9 — Parallax occlusion mapping (experimental) | not started |
| 10 — Ray tracing path | not started |
| 11 — DUDE post-process & screen-space effect stack | **substantially shipped on GL3** via Phase 3.5 (grain/chroma, gamma-in-shader, HDR resolve, SSAO, SSR, SMAA/FXAA); carried to Vulkan in Phase 4 M7 |

## Effort honesty
This is a multi-week project of focused sessions. Phases 1–3.5 landed as ordinary GL
refactors and features with the game fully playable at every commit; **Phase 4 is
where Vulkan actually lights up**, milestone by milestone (M0–M7 in
[vulkan-backend.md](vulkan-backend.md)). Its two honest cost centers are moving
image ownership into the RHI (the GL3 backend still bridges through `idImage`'s GL
uploads) and the descriptor/pipeline plumbing — see the milestone plan's risk list.
