# The mirror seam on Doom 3 models — findings record

**Status: no engine fix. Four attempts, all failed.** This file exists so the fifth attempt starts
from what is already known instead of re-deriving it. Nothing here is shipped; it is a record.

## The artifact

A hard shading split down the middle of mirrored models. Most visible as the long-running "line
down the middle of characters' faces" (Betruger, the scientist heads), and — the observation that
finally broke the problem open — on `models/mapobjects/washroom/toilet.ase` in the mars city
restroom, where one half reads flatter and lighter than the other with a knife-edge boundary.

**It appears in every renderer, legacy included.** It is not a DUDE regression.

## What it is NOT (each row evidence-backed)

The toilet is what eliminates the whole MD5 family of hypotheses, because it is none of those
things. That single observation was worth more than two campaigns of source reading.

| ruled out | evidence |
|---|---|
| tessellation / PN | `RB_RHI_TessellateSurf` (RhiWorld.cpp) requires the entity **model path** to contain `monsters/`, `characters/`, `chars/` or `heads/`, and rejects `entityDef->index == 0`. A mapobject can never tessellate. User-confirmed in game |
| every MD5 weld | the toilet is a static ASE: `BuildGpuSkinData`, `UpdateSurface` and `StampTessSeamMask` never run on it |
| the tess displacement seam mask | `r_tessDisplace 0` leaves the face line unchanged, and `dudeTessDisplace` is the mask's only consumer |
| faceted / unsmoothed normals | the toilet's material has no `unsmoothedTangents`, and the ASE ships **no `MESH_NORMALS` block** → `generateNormals = true` → `R_CleanupTriangles` takes the smoothed `R_DeriveTangents`, which *does* weld dupVerts |
| smoothing groups | the ASE loader never parses them (Model_ase.cpp:341, *"we could parse ... smoothing groups here"*) |
| a genuine hard crease | dihedral across mirror-plane edges is **smoother** than the mesh average: median 12.37° vs 24.89°, only 2/11 edges over 30° vs 260/600 elsewhere |
| POM / parallax | requires `hModel->IsStaticWorldModel()`; a mapobject entity is not one |
| tangent handedness in the shaders | `interaction.vert:37`, `ambientlight.vert:45`, `gbuffer.vert:58` all use `attr_Bitangent` verbatim — no `cross(N,T)` reconstruction anywhere in the shading path |
| a tangent weld across the mirror | stock's dupVert weld (tr_trisurf.cpp) sums **normals only**; id's mirror-tangent merge directly below it is `#if 0` |
| bump / tangents / textures / AO / specular / shadows | the 2026-09-12 elimination ledger — see [[face-centerline-line-unsolved]] |

## What was measured

### The assets really are mirrored, and the halves really do share texels

`toilet.ase`: 207 verts, 412 faces, 156 tverts. **100% mirror-symmetric about y = 0** (207/207 verts
have a partner). Mirrored UV layout: the +y half is 172/176 negative polarity, the −y half 200/236
positive — the UV-polarity boundary *is* the visible line. **350 of 386** mirror-partner face pairs
carry opposite polarity, and **193 of 207** partner verts share at least one tvert.

### The frame relationship (the real finding)

Replicating id's own per-face frame (`R_DeriveTangentsWithoutNormals`: `n = d1×d0`,
`S = d0.xyz·d1.t − d0.t·d1.xyz`, `T = d0.s·d1.xyz − d0.xyz·d1.s`) and comparing each mirror-partner
face against the exact mirror image, `M` = reflection in the symmetry plane:

| asset | pairs | `n' vs M·n` | `S' vs M·S` | `T' vs M·T` |
|---|---|---|---|---|
| toilet.ase | 386 | **0.00°** | **180.00°** | **180.00°** |
| betruger | 326 | 0.0° | 180.0° | 180.0° |
| scihead02 | 158 | 0.0° | 180.0° | 180.0° |
| playerhead | 436 | 0.0° | 180.0° | 180.0° |
| imp | 479 | 0.0° | 180.0° | 180.0° |

`N' = +M·N` exactly, but `S' = −M·S` and `T' = −M·T` exactly, universally.

## The attempt that failed, and why

Reading the table above, the obvious inference is that for a tangent-space sample `n = (nx,ny,nz)`,
`W' = M·(−nx·S − ny·T + nz·N)` while mirror symmetry would need `W' = M·W` — so the mirrored half
"applies the normal map's in-plane tilt backwards", and negating both tangents on one half (a 180°
rotation about `N`, preserving that half's correct left-handedness) would make the frames exact
mirrors and close the seam.

That was built (`r_mirrorTangentFix`, 3-state, default off) and **user-tested: both settings made
the seam visibly WORSE.** Branch deleted.

**Why the inference was wrong:** `renderbump` baked the hi-poly normals into *these very same
engine-computed frames*. The map and the shading are self-consistent **by construction**. The
anti-mirrored tangents are the correct frame for that UV parameterisation; both halves were already
shading correctly. Flipping one half destroys a consistency that was already there.

**The tell that was missed:** the relationship was *exactly* 180.00° on every single pair of every
asset, with zero scatter. **A bug produces scatter. An exact, universal algebraic relationship is a
convention, not a defect.** "Exact and universal" was read as strong evidence *for* a bug when it
was strong evidence *against* one.

## If anyone picks this up again

1. **Verify the premise before building.** The premise to test is *"does the baked normal map
   disagree with the frame the engine computes?"* — and the only way to answer it is to inspect the
   baked map against its hi-poly source. Not by reasoning about frame algebra; that is precisely
   the step that produced a worse result here.
2. **Ask whether stock does it too, first.** One console line (`r_graphicsAPI opengl3`, or just
   looking at the legacy renderer) invalidated everything three campaigns had built. It costs
   seconds.
3. **When an artifact appears on two assets that share no code path, stop debugging the code path.**
   A static ASE prop and a skinned MD5 head have nothing in common but their authoring.
4. Remaining options are asset-side and are the user's call, not a bug fix: re-bake the affected
   normal maps against unmirrored UVs (needs the hi-poly sources, mostly absent), or author a
   seam-aware bump blend — a deliberate divergence from stock, not a fix.

## Method

Python probes over the shipped `.md5mesh` / `.ase` files, replicating the engine's CPU pipeline
exactly: parse → bind pose from joints+weights (quaternion `w = −sqrt(1−x²−y²−z²)`) →
`R_FaceNegativePolarity` → `R_DuplicateMirroredVertexes` → `R_CreateDupVerts` →
`R_BuildDominantTris` → the per-face frame above. Pair faces (never vertices — at a split position
there are two copies and picking the wrong one manufactures a phantom 180° flip, which happened
once here and had to be redone per-face).

Related: `docs/tessellation.md` (the seam mask, which is a genuinely different and now-fixed bug).
