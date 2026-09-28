# Binary model cache (`r_binaryModelCache`)

The single biggest map-load lever in DUDE, found by measurement rather than guesswork.

## The measurement that chose it

The `load phases (msec)` line was split further (commit `79961c6d`, `framework/LoadTiming.h`)
into the asset-parse work a cache could attack. On hell1, `developer 0`, RTX 3080 Ti box:

| slice | ms | share of the 11.8 s load |
|---|---|---|
| **render-model parse** (551 models) | **6808** | **58 %** — 72 % of the 9.4 s `game` phase, ~12.4 ms per model |
| spawn / script remainder | 2430 | 21 % |
| collision conversion (125 props) | 153 | 1.3 % |
| `.aas` parse (6 files) | 42 | 0.4 % |
| settle (10 warm-up frames) | 0 | — |

So the long-standing plan to cache **collision** models (`todo.md`, "~2.8 s cacheMedia") was aimed
at the wrong thing: converting a prop's ASE/LWO into a `cm_model_t` costs 153 ms a load. What
costs 6.8 s is text-lexing 551 ASE/LWO files and re-deriving their sil edges, mirrored/dup verts,
face planes and tangents (`R_CleanupTriangles`) on every single load. That is what this caches.

## What it does

After a static model (`.ase` / `.lwo` / `.flt` / `.ma`) is parsed and `FinishSurfaces()` has run,
the finished surfaces are serialized to `generated/<model path>.bmodel` in the writable dude
folder (`fs_savepath`). The next load of that model reads the file straight back into the same
tri-surf pools and **skips both the text parse and `FinishSurfaces`**. BFG-style.

- **Output-identical by construction** — the cache *is* the parse result. Nothing that reads a
  model changes: the renderer, and the collision converter that builds a prop's `cm_model_t`
  from the same surfaces, see the same bytes.
- **First load of a map still parses** (it populates the cache; the write cost is reported
  separately). Every later load of that map, and every purged-model reload
  (`LoadModel()` = `PurgeModel(); InitFromFile()`), hits.
- **Local-only, never shipped.** It is a reformat of the game's own geometry, so it is derived id
  content exactly like the baked-AO packs — see docs/releasing.md. It is generated on the user's
  machine from the user's pk4s.

## Cache key — when it silently re-parses

A `.bmodel` is used only if **all** of these match, else the model is parsed again and the file
rewritten:

- the **source file's timestamp** (what `idRenderModelStatic::timeStamp` already records — a
  loose file shadowing a pk4 entry has a different one, so the cache follows the override);
- `BMODEL_VERSION` — **bump it in `Model.cpp` whenever the serialized layout OR any load-time
  derive changes** (`R_CleanupTriangles` and what it calls), else stale caches keep old derived
  data;
- `ENGINE_VERSION` — every release re-derives everything, as a backstop for the above;
- the `sizeof` of `idDrawVert`, `glIndex_t`, `silEdge_t`, `dominantTri_t`, `idPlane`, `ID_TIME_T`
  (any layout change invalidates by itself);
- `r_mergeModelSurfaces` and `r_slopVertex/TexCoord/Normal` — the ASE/LWO conversion welds
  vertices by these, so a changed setting must miss.

## Format v1 (raw native byte image)

Every field is written as-is with `idFile::Write` — no endian swapping — because reader and writer
run on the same machine and the file never leaves it. The reader is a bounds-checked cursor over
the whole file in memory: a truncated or corrupt `.bmodel` unwinds whatever it added and falls
back to the text parse.

```
magic 'DBM1'  version  ENGINE_VERSION(len+chars)  sourceTimestamp  sizeof[6]
mergeModelSurfaces  slop[3]  fastLoad  modelBounds  numSurfaces
per surface:  id  shaderName(len+chars)
  tri: bounds  flags(generateNormals|tangentsCalculated|facePlanesCalculated|perfectHull)
       numVerts verts[]  numIndexes indexes[]
       hasSilIndexes silIndexes[]  numMirroredVerts mirroredVerts[]
       numDupVerts dupVerts[2*n]  numSilEdges silEdges[]
       hasFacePlanes facePlanes[numIndexes/3]  hasDominantTris dominantTris[numVerts]
       numShadowIndexesNoFrontCaps  numShadowIndexesNoCaps  shadowCapPlaneBits
```

Runtime-only fields (vertex caches, `ambientSurface`, the GPU skin / tess handles) are not stored;
they stay at `R_AllocStaticTriSurf`'s zero. `deformedSurface` is never true for a file-loaded
model's own geometry; the writer refuses a model where it is.

## What is deliberately excluded

- **The renderbump partial path** (`PartialInitFromFile` sets `fastLoad` *before* `InitFromFile`):
  bypassed entirely, so a tool that wants the bare parse gets exactly what it always did.
  `LoadFLT`'s mid-parse `fastLoad` is different — it is serialized and restored, so a hit
  reproduces that parse's output.
- **`_area*` world models**: built straight from the `.proc`, never through `InitFromFile`. They
  are the separate `proc=` load phase (~1.1 s here) — a possible later target, not this one.
- **MD5 characters, MD3, particles, liquids**: not cacheable types. The `asset parse` line now
  buckets the model time as `[static=.. md5=.. other=..]` precisely so the post-cache residue shows
  whether an MD5 cache is worth doing next.

## Files

- `renderer/Model.cpp` — the cvar, `BinaryModelPath`, `idBinaryModelReader`, `BModel_WriteTri` /
  `BModel_ReadTri`, `idRenderModelStatic::WriteBinaryModel` / `LoadBinaryModel`, and the hook in
  `InitFromFile`.
- `renderer/tr_trisurf.cpp` + `tr_local.h` — `R_AllocStaticTriSurfSilIndexes / SilEdges /
  DominantTris / MirroredVerts / DupVerts`: the derived arrays must come from the very pools
  `R_ReallyFreeStaticTriSurf` frees them into.
- `framework/LoadTiming.h` + `Session.cpp` — the per-type buckets and the
  `bmodel cache: N hits, N misses, N msec writing` console line.

## Turning it off / clearing it

`r_binaryModelCache 0` always parses and never writes (archived, so it sticks). To discard the
cache, delete `generated/` under the dude folder; there is no rebuild mode because the key
already covers every staleness case above.
