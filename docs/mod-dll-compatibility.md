# Mod DLL Compatibility

Status: **planned** (design record). Owner: DUDE / dhewm-rt.

Goal: make dude genuinely mod-friendly — any Doom 3 mod that provides a **game
module compiled against a dude SDK** should drop in and load via `fs_game`,
without recompiling the engine. Establish ABI discipline so dude's own renderer
churn stops silently breaking third-party game libraries.

The headline target — **Doom 3: Phobos** — is **achievable**, but via *source*,
not its proprietary binary. Team Future GPL'd the game code and it is already
ported to the dhewm3 Mod SDK at the same game-API version as dude (§5). The path
is "recompile the GPL game source against dude's SDK", which is exactly Phase 2.
Loading the retail *binary* remains out of scope (and unnecessary) — §5 records
why, so it isn't relitigated.

---

## 1. Current state (verified)

dude is **already a DLL-loading engine** — nothing needs re-enabling:

- `HARDLINK_GAME` is **OFF** and `__DOOM_DLL__` is **ON**
  (`neo/CMakeLists.txt:69`, `:1459`). The game is **not** statically linked.
- Proof in `build/`: `base.so` (~27 MB) and `d3xp.so` (~28 MB) are separate
  shared libraries the `dude` executable loads at runtime through
  `GetGameAPI` (`neo/framework/Common.cpp:2824`, `LoadGameDLL`).
- The lib name is `<fs_game>` + platform suffix
  (`neo/sys/sys_local.cpp:93`, `DLL_GetFileName`). So
  `+set fs_game hardcorps` looks for `hardcorps/hardcorps.so` (or `.dll`),
  falling back to `fs_game_base`, then base.
- Boundary contract: `gameImport_t` / `gameExport_t` swapped by `GetGameAPI`,
  gated on `GAME_API_VERSION` (`neo/framework/Game.h:366`, currently **9**).

**Consequence:** a mod that ships a `<fs_game>.so` / `<fs_game>.dll` built
against dude's headers already loads today. The missing pieces are (a) a
published SDK, and (b) ABI discipline so we don't break those libs on the next
renderer commit.

## 2. What a `+set dllModCompat` cvar can and cannot do

- **Static-vs-DLL is compile-time** (`HARDLINK_GAME`), not a cvar — but this is
  moot, because dude is already on the DLL side.
- A runtime cvar **can** usefully: relax the strict version-equality at
  `neo/framework/Common.cpp:2897` from `FatalError` → `Warning` (attempt load of
  a lib built against a *nearby* dude API), and/or allow an alternate lib
  name/path.
- A cvar **cannot** load a foreign-ABI or foreign-arch binary. Skipping the
  version check just converts a clean fatal error into a crash.

## 3. The ABI surface (what a game DLL actually sees)

Critical correction: **`tr_local.h` is invisible to the game DLL.** Nothing in
`neo/game` or `neo/d3xp` includes it — it is engine-internal, behind the
interface. Reverting SSAO / `tr_local.h` work has **zero** effect on mod ABI
compatibility.

The real game-visible surface is the **public** headers and the idlib types they
pass by value/pointer:

- `neo/framework/Game.h` — `gameImport_t`, `gameExport_t`, `GAME_API_VERSION`
- `neo/renderer/RenderWorld.h` — `renderEntity_s`, `renderLight_s`,
  `renderView_s` (the game builds these every frame)
- `neo/renderer/RenderSystem.h`, `Material.h`, `DeclManager.h`
- vtables of every interface handed across the boundary: sys, common,
  cmdSystem, cvarSystem, fileSystem, networkSystem, renderSystem, soundSystem,
  renderModelManager, uiManager, declManager, AASFileManager,
  collisionModelManager
- idlib value types crossing the line: `idStr`, `idVec3`, `idBounds`, `idDict`,
  `sysEvent_t`, decl types

Note: this fork's POM commit (`b7d41e6a`) touched `RenderWorld.h` — the exact
kind of change (fields on `renderEntity_s`) that breaks game-lib ABI. That, not
`tr_local.h`, is the surface to watch.

## 4. Reference baseline for ABI diffs: **original Doom 3 source, not dhewm3**

dhewm3 has already diverged from retail idTech4 (it bumped `GAME_API_VERSION`,
changed idlib types and interface vtables, and recompiles game code from source
precisely because it is *not* retail-ABI-compatible). Diffing dude against
dhewm3 would only measure drift from an already-incompatible reference.

Therefore the ground-truth ABI baseline is the **original Doom 3 GPL source**:

- Repo: `id-Software/DOOM-3` (the released 1.3.1 codebase)
- `GAME_API_VERSION = 8`
- Retail `gameImport_t` / `gameExport_t`, retail `renderEntity_t` /
  `renderLight_t` / `renderView_t`, retail idlib layouts and interface vtable
  orders.

This is the layout a retail-SDK-built binary (Phobos, and most pre-source-port
mods) expects. Measuring dude against it tells us the true distance to
binary compatibility — dhewm3 is merely an intermediate, already off-baseline.

## 5. The Phobos reality — achievable via SOURCE, not the binary

**Corrected finding (2026-08).** Chasing Phobos's proprietary binary is the wrong
target. Team Future released the Phobos **game source under GPL**, and it has
already been ported to the dhewm3 Mod SDK:

- Repo: `github.com/TwelveEyes/dhewm3-sdk`, branch **`tfphobos`** (GPLv3).
- The dhewm3 Mod SDK is "mostly the same source files as the original Doom 3
  SDK", relicensed GPLv3; mod ports are made by diffing the original Doom 3 SDK
  against the mod's GPL source and applying that patch onto the dhewm3-sdk tree.
- The community compatibility patch already ships **both an x86_32 Windows DLL
  and an AMD64 (64-bit) Linux `.so`** — i.e. Phobos-on-64-bit-Linux is a solved
  problem on stock dhewm3.
- Crucially, `tfphobos` is **`GAME_API_VERSION = 9` — the same as dude** (both
  are dhewm3 lineage). The version gate matches out of the box.

### The binary wall only ever applied to loading the retail DLL

The three-layer wall below is real **only for `dlopen`-ing the proprietary
`gamex86.dll`**. The source route dissolves every layer:

| Blocker (retail *binary* route) | Source route (build `tfphobos` for dude) |
|---|---|
| 32-bit Windows PE only | recompile 64-bit |
| PE vs Linux ELF | recompile ELF |
| ABI layout drift (v8 retail vs v9 dhewm3) | recompilation adopts dude's *current* layout — drift is a non-issue |
| proprietary / no redistribution | GPLv3 source, clean |

So there is **no need** to build a retail-ABI engine or run Phobos under
Wine. dude reaches Phobos the same way it reaches any healthy mod: **compile the
GPL game source against dude's SDK.**

### Concrete path to Phobos-on-dude

1. Take the `tfphobos` game source.
2. Build it against **dude's** public headers instead of stock dhewm3-sdk's.
3. Fix the **source-level** API differences from dude's divergence (new
   `renderEntity_s` POM fields, PBR additions, any renamed/re-signatured
   methods). These are **compile errors fixed once**, not silent ABI crashes —
   this is the payoff of the source route over the binary route.
4. Produce `phobos.so` (64-bit ELF) → dude loads it via `fs_game phobos`.

This makes Phobos the **validating target for Phase 2** (the dude SDK), not an
out-of-scope stretch goal.

### Known gaps to expect

The dhewm3 port omits effects Team Future implemented **engine-side** (biohelmet
overlay, bloom, camera blur, 1:1 clickable item GUIs, message-GUI stack shift);
toxin-damage entities were removed where the biohelmet was required. Opportunity
note: several of these (bloom, HDR, post) **exist in dude** and could be
restored on dude's side rather than left dropped — a differentiator vs the stock
dhewm3 port.

---

## 6. Plan

### Phase 0 — Pin the game-visible ABI surface (foundation)
- Enumerate the exact boundary headers/types from §3.
- **Diff dude's versions against the original Doom 3 GPL source
  (`id-Software/DOOM-3`, §4)** — not against dhewm3 — to measure true distance
  from retail ABI and to see exactly what this fork has already changed under the
  line (start with `RenderWorld.h` / `RenderSystem.h`; POM is a known toucher).
- Adopt the rule: **append-only fields, never reorder virtuals; new renderer
  state lives behind interfaces or after existing members.**
- Add `static_assert` size/offset guards on `renderEntity_t`,
  `renderLight_t`, `renderView_t`, `gameImport_t`, `gameExport_t` so an
  accidental layout change fails the **build**, not a modder's game.

### Phase 1 — `dllModCompat` cvar + graceful negotiation
- Under `dllModCompat`, convert the version check at
  `neo/framework/Common.cpp:2897` from `FatalError` → `Warning` (attempt load,
  log an honest "unsupported, may crash" line).
- Optionally support an alternate lib name/path so a mod can ship
  `<mod>_dude.so` alongside its retail `gamex86.dll`.
- Keep bumping `GAME_API_VERSION` whenever layout *is* broken, so mismatches are
  loud rather than silent crashes.

### Phase 2 — Ship the dude SDK (the actual deliverable)
- Export the public headers + a `CMakeLists` template that builds
  `<fs_game>.so` / `<fs_game>.dll` against dude, mirroring how `base.so` /
  `d3xp.so` are produced.
- A "hello mod" sample (one changed weapon or entity) proving the drop-in loop:
  build → `fs_game mymod` → loads.
- **Validation target: Phobos.** Build `TwelveEyes/dhewm3-sdk@tfphobos` against
  the dude SDK → `phobos.so`, reconciling source-level API drift (§5). Success =
  a real, complete TC running on dude and a proof the SDK works.
- `docs/modding-sdk.md`.

### Phase 3 — Windows + 64-bit parity
- Ensure a Windows dude build loads a `<mod>.dll` so Windows modders (the bulk
  of the scene) can target dude with a 64-bit MSVC build. Feeds off the existing
  `build-win.sh` cross-build work.

### Phase 4 — (optional, expensive) retail-ABI compat target
- Only for a **binary-only** mod with *no* GPL source (Phobos is NOT such a case
  — it has GPL source, see §5): a **separate** build target with headers frozen
  to the original Doom 3 layout (API v8), 32-bit, decoupled from mainline dude.
  It cannot carry the Vulkan renderer and is low-ROI. **Parked** — pursue only if
  a specific high-value binary-only mod ever justifies it.

### Recommended sequencing
Do **Phase 0 → 2** (makes dude genuinely mod-friendly, protects it from our own
renderer churn, and lands Phobos as the Phase-2 validation target), treat
**Phase 3** as follow-on, and **shelve Phase 4**.

## 7. Scoping audit + task breakdown

### 7.1 ABI-drift audit (measured 2026-08, dude HEAD vs `upstream/master` = dhewm/dhewm3)

Diffed every game-visible header. **Result: all drift is additive or a
defaulted-param extension — nothing removed, renamed, or reordered.** A mod
recompiled from source against dude needs **~zero header reconciliation.**

| Header | Change | Recompile impact |
|---|---|---|
| `framework/Game.h` | **clean** — `gameImport_t`/`gameExport_t`/`GAME_API_VERSION=9`/`idGame`/`idGameEdit` untouched | none — this is the only surface the mod *implements* |
| `renderer/RenderWorld.h` | `idRenderWorld::Trace()` gained `bool skipDecals=false` | default arg, call sites unchanged |
| `renderer/RenderSystem.h` | 4 fields appended to `glConfig` (`maxCubeMapSize`, `vidMemMB`, `coreProfile`, `rhiBackend`) | additive struct fields |
| `renderer/Material.h` | `SL_OCCLUSION` enum value + PBR/parallax getters, fields, `pbrCategory_t` | additive |
| `framework/Common.h` | new **pure-virtual** `idCommon::GetTicInterpolation()`; `dude_preset` extern; dhewm3→DUDE comment renames | engine implements `idCommon`, game only calls it → extra vtable slot, harmless on recompile |

The mod implements only `idGame`/`idGameEdit` (from the **clean** `Game.h`) and
*calls* the rest, so none of dude's additions force any mod code change. The one
ABI hazard to institutionalise: `GetTicInterpolation` was added as a pure-virtual
— safe here only because the game doesn't implement `idCommon`; the Phase 0 rule
(append pure-virtuals at interface end, bump version on real breaks) exists to
keep this true.

### 7.2 Work breakdown (ordered, with size S/M/L)

**Epic A — SDK foundation** (Phase 0 + 2)
- **A1 (S)** ABI guard rails: `static_assert` size/offset checks on
  `renderEntity_t`, `renderLight_t`, `renderView_t`, `gameImport_t`,
  `gameExport_t`; document the append-only + version-bump rule in `Game.h`.
- **A2 (M)** `dude-sdk`: export the game-visible headers + a CMake template that
  builds `<fs_game>.so` / `<fs_game>.dll` against dude, modelled on
  `TwelveEyes/dhewm3-sdk`'s CMake layout; wire the output name to dude's loader
  (`<fs_game>`+suffix, `sys_local.cpp:93`).
- **A3 (S)** "hello mod" sample (one tweaked weapon/entity) + `docs/modding-sdk.md`
  documenting the drop-in loop.

**Epic B — `dllModCompat` cvar** (Phase 1, independent of A)
- **B1 (S)** register `dllModCompat`; under it, relax the version check at
  `Common.cpp:2897` `FatalError`→`Warning`; optional alternate lib name/path so a
  mod can ship `<mod>_dude.so` beside its retail `gamex86.dll`.

**Epic C — Phobos validation** (Phase 2 target; depends on A2)
- **C1 (S)** acquire `TwelveEyes/dhewm3-sdk@tfphobos` source; confirm the Phobos
  game *assets* are installed (patch requires Phobos game files present).
- **C2 (M)** build `tfphobos` against the dude SDK → `phobos.so`. Expected
  compile friction is **not** engine-ABI (§7.1) but SDK-helper differences
  between stock dhewm3-sdk and dude; resolve as they surface.
- **C3 (M)** runtime bring-up: `+set fs_game phobos`, load, playtest the opening
  maps; log any interface-call mismatches.
- **C4 (stretch)** restore effects the stock dhewm3 port dropped that dude already
  has (bloom/HDR/post) — dude differentiator, not required for "runs".

**Epic D — Windows parity** (Phase 3, follow-on; depends on A2)
- **D1 (M)** ensure a Windows dude build loads `<mod>.dll`; MSVC build template;
  leans on existing `build-win.sh` cross-build work.

### 7.3 Critical path
A1 → A2 → C1 → C2 → C3 is the spine that lands Phobos on dude. B and A3 are
parallel/independent. D and C4 are follow-ons. **This is general infrastructure**
— Phobos is the *first* consumer, not the only one; any GPL-source mod (LibreCoop,
Classic Doom 3, …) reuses A2/A3 unchanged.

## 8. Ecosystem & distribution (forward-looking)

Turning dude into a modding *platform* (not just a source port) is the strategic
endgame and aligns with the codebase-independence direction. The key design call
is to **split one "mods repo" idea into three concerns**, because lumping them
creates ABI, legal, and security problems that separating avoids.

### 8.1 Three concerns, three homes

1. **The SDK (headers + CMake template + docs) → coupled to the engine.**
   A mod `.so` is ABI-locked to a dude version; if the SDK drifts from the
   engine's game-visible headers, mods crash. dhewm3 can keep `dhewm3-sdk` fully
   separate because its ABI is glacial — dude's renderer is actively churning, so
   couple it: SDK as an in-repo subdirectory **or** a git submodule tagged in
   lockstep, with an explicit "SDK vX ↔ dude vY" compat line. This is an
   extension of the Phase 0 ABI discipline (§6, §7.1), not a separate policy.

2. **The mod *ports* → a separate `dude-mods` repo, as patch-recipes, not
   re-hosted source.** Third-party GPL mods do not belong in the engine repo
   (clean separation; each mod is its own project with its own authors). Prefer a
   repo of **build recipes**: per mod, a script/patch that fetches the mod's own
   upstream GPL source and applies a small dude-port delta. dude maintains the
   *port delta*; original authors keep authorship — sidestepping most
   licensing/attribution friction and keeping the repo small.

3. **Assets → never redistributed.** Hard legal line and existing project policy.
   Mod *game code* is GPL and shareable; mod *assets* (maps, textures, sounds,
   models — e.g. Phobos's) are the team's IP, usually built on id's copyrighted
   base — not ours to ship. Any distribution mechanism ships the `.so`/patch only
   and pulls assets from the user's own install into the "dude folder"
   (`fs_game` dir). This mirrors how the community Phobos patch already works
   ("requires Doom 3: Phobos game files installed first").

### 8.2 In-game mod downloader (Phase 5+, security-sensitive)

A good vision that fits the "dude folder" model (downloaded mods drop in as
`fs_game` dirs), but heavier than it looks — two constraints must be honoured:

- **It is a remote-code-execution surface.** Downloading and `dlopen`-ing native
  game modules mandates a **curated, checksummed, signed** channel. Never open
  user uploads. This is the main reason to build it late and carefully.
- **Version-matching is mandatory.** Every mod `.so` is ABI-pinned to a dude
  version. The manifest records "built for dude vY"; the client refuses
  mismatches — otherwise the exact silent crashes Phase 0 prevents reappear. The
  downloader is downstream of the ABI/versioning work, not independent of it.

### 8.3 Recommended sequencing (don't build the store before the shelf is stocked)

1. **Now:** SDK coupled to engine (Epic A) + Phobos as first working port (Epic C)
   — prove one mod runs end-to-end.
2. **Then:** stand up `dude-mods` as a patch-recipe repo; add a 2nd/3rd GPL mod so
   the SDK is proven *general*, not Phobos-shaped.
3. **Later (Phase 5+):** the in-game manager — start as a simple **curated
   manifest** (JSON list of known dude ports + fetch/build/install steps) with
   checksums, assets-from-user, and version gating. A full CDN/signing story only
   once real demand exists.

## 9. Fidelity / legal notes
- This effort ships **no third-party binaries**; it enables *source-recompiled*
  mods only.
- Loading proprietary retail DLLs stays out of scope for the reasons in §5.
- Per project policy, mod-compat features are opt-in and must not degrade the
  faithful default experience.
