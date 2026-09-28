# Load time: the material parse was generating height fields

How a "6.8 s model parse" turned out to be 8.5 s of Poisson solves inside the material parser, and
what was done about it. Companion to [binary-model-cache.md](binary-model-cache.md); the timers are in
`framework/LoadTiming.h` (printed after `load phases (msec)` on every map load).

## The measurement chain (hell1, RTX 3080 Ti, Vulkan, `developer 0`)

1. **First split** (`asset parse`): `modelParse=6808 ms` for 551 models, 58 % of an 11.8 s load. The
   binary model cache was built on that number.
2. **Cache verified** (175 hits) — but the static bucket fell only 3417 → 2908 ms. A hit costs ~1 ms,
   so the number had to contain something a model cache cannot remove.
3. **Decl first-parse split out** (`decl parse`, measured at `idDeclLocal::ParseLocal`, each scope
   exclusive of the parses it nests, and the model scopes exclusive of all of it):

| slice | ms | share of the 10.9 s load |
|---|---|---|
| **decl parse, all** (1512 decls) | **9438** | **86 %** |
| ↳ **materials** (649) | **8482** | 77 % — **13 ms per material** |
| ↳ other (863) | 956 | 9 % |
| images + sound (`media`) | 988 | 9 % |
| **all model geometry** (551 models) | **118** | 1 % |

Geometry was never the cost. The MD5 sub-split (`deformInfo=9 gpuSkinBake=2`) killed the planned MD5
cache outright.

## Why 13 ms per material

A material decl is a few dozen lines; the lexer is microseconds. The cost is two DUDE additions in
`idMaterial::ParseStage` ([Material.cpp](../neo/renderer/Material.cpp)), each gated at *parse* time:

- `r_parallax` on + a bump stage → `R_CreateParallaxHeightImage( bumpProgram )`
- `r_tessellation` on + a bump stage → `R_CreateTessHeightImage( ... )` (a *separate* image: the two
  want different normalizations)

Both registered their image through `ImageFromFunction`, whose contract is the tiny permanent built-ins
(`_white`, `_fog`, …): it **generates immediately** (`image_preload`), bypassing the level-load deferral
every file image gets. So for every bump material, inside the parse, on the main thread: decode the
full 2K normal-map program, then `R_NormalMapToHeightMap` — a 64-iteration Jacobi relaxation of the
Poisson equation over the whole image plus a two-pass wrap-aware box blur — and on this configuration
**twice** (parallax and tessellation). The same normal map was then decoded a *third* time later as the
real bump texture. And because generator images are "always kept around", every visited map's height
fields stayed resident for the session.

## The fix: level-content generators (`r_deferLevelGenerators`, default on)

`idImageManager::ImageFromLevelGenerator( name, generatorFunction, generatorCpu )`
([Image_init.cpp](../neo/renderer/Image_init.cpp)) registers a generated image that is *level content*,
not a built-in. `idImage::IsPermanentGenerator()` carries the distinction at the four places the image
manager used to skip all generators (`BeginLevelLoad`, the `EndLevelLoad` purge, its parallel collect,
its serial fallback):

- **Deferred.** Inside a level load it is only marked referenced; `EndLevelLoad` produces it with every
  other texture. Outside a level load (`reloadDecls`, `vid_restart`) it generates immediately as before.
- **Parallel.** `generatorCpu` is the produce-pixels half. Under `r_parallelImageLoad` (Vulkan),
  `LoadImageCpu` calls it on a worker in place of the file decode; the mip pyramid and the main-thread
  upload are the unchanged file-image path. The hooks are worker-safe by construction: a read-only hash
  probe (materials are all parsed before `EndLevelLoad`), the same `R_LoadImageProgram` the loader
  already runs on workers, and a solver on private buffers. On GL3 they take the existing serial route.
- **Purgeable.** Reloaded and purged like a file image, so height fields no longer accumulate across
  maps.
- **Identical output.** The classic `generatorFunction` remains the serial path and is now a thin
  wrapper over the same CPU half with the same `GenerateImage` parameters; the 1×1 flat mid-grey
  fallback for a missing source is preserved (a worker failure would otherwise have produced the
  checkerboard default).

**Measured (hell1, cold start, 2026-09-28):** total load **11310 → 2458 ms**; material parse 8482 →
**7 ms**; `proc` 1079 → 95 (the world's own materials were generating too); `media` ~1030 → 1126 — the
`EndLevelLoad` summary printed `305 of them height fields generated here, not in the material parse`
inside a 1.1 s image phase alongside 1157 regular textures. **Visually unchanged, user-confirmed.** (That
run parsed fewer decls than the earlier ones — 534 vs 649 materials — so the exact ratio is soft; the
per-material figure is not.)

**Rejected:** caching the derived height fields on disk. At source resolution that is 256 KB–4 MB per
material — roughly 0.6 GB for stock and up to ~10 GB with a 2K texture pack — and it would have left
the first load of every map at 8.5 s.

## The per-type split, and two more findings

The `decl by type` line then resolved the ~960 ms of non-material decls (full-content cold run,
11310 → 2919 ms): **sound 395** (401 shaders — `FindSound` loads and decodes the sample inside the
shader parse, stock behaviour), **particle 352** (115 decls), **model 203** (44 game-side modelDefs
parsing their `.md5anim` text), everything else ~10. The "2.4 s spawn/script floor" turned out to be
these same parses.

- **Particle bounds — fixed.** `idDeclParticle::GetStageBounds` stepped "1000 possible particles as a
  representative sampling", but never advanced its `steppingRandom` inside that loop, so all 1000
  iterations reseeded identically and recomputed the same origins. One sample is bit-identical at
  1/1000 the cost: **352 → 5 ms**, load **2919 → 2571 ms** (4.4× vs the original 11.3 s).
- **Filesystem probes — measured, not a lever.** `fs opens (msec)` on the same load: 52 ms for 10,320
  opens (6,503 misses), ~5 µs each. The search-path walks are cheap, so the sound bucket is genuine
  decoding, and its fix would be sound-cache surgery (a thread-safe sample allocator and decoder pool,
  and splitting the OpenAL upload out of `Load`) for ~300 ms at real audio-glitch risk. Held.

## What remains on the load-time ledger (2.57 s)

`media` 1.4 s (images incl. the height fields, sound purge), sound decode 0.41 s, modelDef animation
parsing 0.21 s (game-DLL code, three copies), collision conversion 0.16 s, all model geometry 0.12 s.
Nothing left is both large and low-risk; this is the stopping point for the 0.9.5 load-time work.
