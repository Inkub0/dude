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

## Attempts 5 and 6 (2026-09-25, same day) — vertex-space blending. Also failed.

The user's design, and a good one: leave one half exactly as authored, let the other bleed toward
its partner over a narrow band. Built as `r_mirrorSeamFade` on `feat/mirror-seam-bleed`, **branch
deleted, never merged.** Three forms, all rejected on sight:

| form | what it did | why it failed |
|---|---|---|
| scale tangents to `−1` at the seam | meant to reach the partner's reading | negating gives `M·W`, the *mirror* of the partner's normal — a different vector at a point on the plane. Inverted a whole half; visually the rejected attempt-four flip |
| scale tangents to `0` | removes the inversion | just deletes relief; the untouched half still has full relief at the seam, so no continuity either |
| **rotate** the frame onto the partner's | mathematically correct: if B shades with A's frame it produces A's normal exactly | correct at the seam, but the blend washed the relief out across the band |

**The number that explains all three:** a tangent and its mirrored target differ by a
**median of 82.2°** on betruger's head (p10 28.4, p90 156.5, max 178.9). The two halves' frames are
most of a right angle apart. There was never a small correction available, and any blend between
them over a band destroys the relief in the middle of that band:

| `t` | median \|lerp\| | faces below 0.3 |
|---|---|---|
| 0.5 | 0.753 | 17.9% |
| 1.0 (seam) | 1.000 | 0% |

The endpoint is clean — at the seam it is the partner's frame exactly, at full strength — so the
damage is purely mid-ramp. Renormalising the lerp avoids the dip but forces unit length on an
intermediate frame corresponding to no real parameterisation, which reads in game as the relief
swivelling. Lerp washes out; slerp swivels. There is no third option in vertex space.

**The actual lesson, and it is about resolution, not maths.** The user asked for the blend in the
NORMAL MAP over a 2-pixel band, and picked a second-texture layout to make it possible. I
substituted a per-vertex frame blend because it was smaller, and argued it was equivalent. It is
equivalent in *output* and not in *resolution*: on a 207-vertex head the narrowest expressible band
is one triangle ring, which is most of a cheek. Even `r_mirrorSeamFade 0.01` (seam ring only, the
minimum) still washed a full triangle band. **A 2-texel band is not expressible in vertex space.**

**Still untried, and the only remaining option with an argument behind it:** the texture route the
user asked for. The risky part is already validated offline — solving map B per texel so half B
reproduces half A's normal lands at median 0.00° over 53639 texels, and confining it to a 2-texel
rim touches 3.3% of texels instead of 99.6%. TBN handedness identifies the half per-vertex 100%
reliably with 0% near-degenerate, so no new vertex attribute is needed. What remains is plumbing:
generate map B at load, a texture unit through the Vulkan descriptor path, and a per-fragment pick.

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
