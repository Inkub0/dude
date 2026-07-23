# dhewm-rt: Vulkan Raster Port Plan

**Goal:** a faithful modern Vulkan port of the dhewm3 renderer — same look (depth prepass,
stencil shadow volumes, per-pixel ARB2-style lighting), classic Doom 3 game data, done
incrementally in this codebase with the game staying playable (on GL) at every step.

Surveyed 2026-07-20 at master `455b88e` (1.5.5).

This is the hub. The plan is split across focused documents so each can be loaded on its own:

| Document | Contents |
|---|---|
| [port-architecture.md](port-architecture.md) | Code survey, hardware baseline & backend switch, `vulkan-rt` profile, known risks, prior art |
| [port-phases.md](port-phases.md) | Detailed Phase 0–10 plan, Material IR, mod compatibility scope |
| [port-improvements.md](port-improvements.md) | "Improvements over the classic engine" toggles + Phase 11 post-process stack |
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
| 3 — RHI abstraction + GL 3.3 core backend | **in progress** — Chunks A–G done (A–F + texgen + debug tools); remaining: cinematics/screenshots/ImGui-on-core, subview near-clip polish, then parity/flip |
| 4 — Vulkan backend | not started |
| 5 — Validation & polish (the Vulkan payoff) | not started |
| 6 — Modern Vulkan device profile (`vulkan-rt`) | not started |
| 7 — Uncapped framerate (fixed-tick + interpolation) | not started |
| 8 — Shadow mapping (optional, per-light) | not started |
| 9 — Parallax occlusion mapping (experimental) | not started |
| 10 — Ray tracing path | not started |
| 11 — DUDE post-process & screen-space effect stack | Chunk F seeded (film grain, chromatic aberration) |

## Effort honesty
This is a multi-week project of focused sessions. Phases 1–3 land as ordinary GL
refactors with the game fully playable at every commit; **Phase 5 is where Vulkan
actually lights up**, milestone by milestone. (Phases 2.5 and 4 — Material IR and
POM — are not plain refactors; POM in particular is experimental and could be
deferred behind the core Vulkan port.)
