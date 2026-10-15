# GPU AO Baker — plan (draft 2026-10-15)

Status: PLAN / for review. No code yet. Goal: move the offline self-occlusion AO bake
(`neo/tools/compilers/aobake/aobake.cpp`) from the CPU to a headless Vulkan compute +
ray-query path, and — while we're in there — fix a latent correctness bug: the CPU baker
bakes the FLAT base mesh, but the runtime renders the TESSELLATED + DISPLACED mesh.

---

## 1. Why

The CPU baker is embarrassingly-parallel hemisphere ray casting: for each texel of each
surface's AO map, shoot N rays (default 128) against an occluder grid and store the
visibility fraction. `AO_SIZE 0` (auto) bakes each surface at its bump-map resolution, so
HD-pack characters hit 2K–4K surfaces → **minutes per surface, hours for a full
`models/md5` sweep (254 models)**. This is exactly what GPUs — and RT cores — are for.

We are unusually well-positioned: the engine already does ray-traced AO *at runtime*
(RTAO, `rtx-rtao-h4`: hemisphere rays via `VK_KHR_ray_query` against the `r_rtWorld`
BLAS, denoised by NRD). The offline baker is the **texture-space twin** of that.

### Foundation already in place (reuse, don't reinvent)
- **Compute + ray-query + BLAS, already validated:** `RayQuerySelfTest()`
  (`neo/renderer/rhi/vk/VulkanBackend.cpp:295`, `r_rayQueryTest`) builds a synthetic
  BLAS/TLAS, traces a ray grid **from a compute shader** via `GL_EXT_ray_query`, and diffs
  every hit's `t` + primitive index against a CPU reference. This is the baker kernel in
  miniature.
- **BLAS from real geometry:** the persistent `r_rtWorld` path + animated-BLAS-from-
  `gpuSkinVB` (`rtx-animated-blas-phase`) already build acceleration structures from models.
- **Compute dispatch API:** `rhi::RHI::Dispatch( ComputeArgs )` (`neo/renderer/rhi/RHI.h:550`).
- **Tess displacement is shared GLSL:** `neo/shaders/tess.glsl` (used by `zfill.tese`,
  `gbuffer.tese`, `interaction.tese`, …) — the same PN + displacement the runtime renders.
- **Output + parity helpers (keep byte-compatible):** `AO_GeneratedPathForSurface()`
  (`aobake.cpp:806`) for the `generated/aomaps/<model>_s<N>.tga` path convention, and the
  occluder-skip rules `MC_TRANSLUCENT` + `AO_SurfaceIsEyewear()` (`aobake.cpp:184`, `:929`).

Expected speedup: well past 10× — RT cores + full-GPU parallelism on a pure-visibility
workload. Target: the hours-long full bake becomes **minutes**, making bake iteration free.

---

## 2. The tessellation / displacement correctness win (the ear problem)

**The concern (correct):** the CPU baker instantiates the model in its **reference bind
pose, un-tessellated** (`aobake.h`), so AO is computed against the FLAT silhouette. At
runtime the same mesh is PN-tessellated and displaced (`tess.glsl`). Two separate effects:

1. **UV sampling "stretch" — already fine.** The AO map lives in the base mesh's UV layout.
   The tessellated surface interpolates those UVs, so the texture maps across the rounded
   surface automatically. No action needed; this part is not the bug.
2. **Baked VALUES are wrong on displaced geometry — the real bug.** The occlusion stored
   per texel reflects the *flat* shape. Where tessellation+displacement adds curvature or
   protrusion — ears, brows, muscle bulges, armor rivets — the flat-mesh AO misses the new
   self-occlusion (e.g. the behind-the-ear crease reads too bright) and misses contact
   darkening where displaced features approach each other. Low displacement ⇒ subtle; a
   pronounced ear ⇒ visibly wrong.

**The fix (free on GPU, impractical on CPU): bake against the geometry the runtime actually
renders.** Per model/surface, bake the mesh in exactly the state the runtime draws it:
- Tessellated + displaced **iff** the runtime tessellates it (gate on the same predicate
  the renderer uses — `RB_RHI_TessOrDeform` / the tess material+entity gate), using the
  shared `tess.glsl` displacement at a chosen **reference tess factor** (the max/target
  level = displacement ground truth).
- Base mesh otherwise (static props, non-tessellated chars) — identical to today.

Mechanics: run the tess+displace to produce displaced verts → build the BLAS from them →
rasterize the **displaced** mesh into UV space so each texel carries its *displaced* world
position + normal → trace against the displaced BLAS. Result: AO correct for the rendered
silhouette, stored in base-UV space (runtime samples it correctly at any LOD). Because AO
is low-frequency, a runtime LOD below the bake's reference factor only blurs it slightly —
no popping. This turns a limitation into a quality improvement, and heads (ears!) are the
headline beneficiary — exactly the surfaces the eyewear-AO fix also targets.

Open question: one reference tess factor for all, or per-material? Start with a single high
reference factor (cvar `r_occlusionMapBakeTess`), measure, refine.

---

## 3. Architecture

### 3.1 Headless Vulkan context
The CPU baker runs inside the full `dude` client under `xvfb` (software GL fallback — see
`bake_ao.sh`). The GPU baker wants a **real Vulkan device with RT/ray-query**, which needs
**no display** (headless VK compute is standard on NVIDIA Linux; no surface/swapchain).
- Reuse the RHI VK device-init path but in a **surfaceless/offscreen** mode (no swapchain,
  no window). Confirm/extend `VulkanBackend` init to support headless (device + queues +
  descriptors + the ray-query/accel-struct extensions already gated by `SupportsRayQuery()`).
- Keep `bake_ao.sh`'s throwaway `fs_configpath` trick; drop `xvfb` for the GPU path.

### 3.2 Per-model pipeline (one model at a time, mirrors `AO_BakeModelToCache`)
1. Load render model; resolve surfaces and skins exactly as the CPU baker does.
2. **Geometry prep:** for each surface decide base vs tessellated (runtime gate); produce
   final (optionally displaced) vertex + index buffers on the GPU.
3. **BLAS build** from the (displaced) occluder set — **excluding** `MC_TRANSLUCENT` and
   `AO_SurfaceIsEyewear()` surfaces, identical to the current occluder rules.
4. **Texel G-buffer (UV-space raster):** render each bake-target surface into an off-screen
   target the size of its AO map (auto = bump-map res, or `r_occlusionMapBakeSize`), with
   position = UV → clip space, writing `worldPos`, `worldNormal`, `coverageMask`. Use
   conservative rasterization (or a small dilate later) to fill seam texels.
5. **Trace kernel (compute, `GL_EXT_ray_query`):** per covered texel, generate N
   cosine-weighted hemisphere rays around the texel normal (deterministic blue-noise /
   fixed seed for reproducibility), `rayQueryEXT` against the BLAS with the bake max-dist,
   accumulate visibility → AO. Mirror the CPU baker's contrast/contact semantics
   (`r_occlusionMapBakeContrast`, `…BakeContact`, `…BakeDist`).
6. **Post:** GPU dilate (seam bleed, `…BakeDilate`), optional contact term, contrast; then
   read back and write the `.tga` via `AO_GeneratedPathForSurface()` — **same path, same
   format, drop-in** for the existing runtime loader and the `z_baked_ao*.pk4` packing.
7. **Skip no-op (white) maps — don't write them.** A fully-unoccluded surface bakes to a
   solid-white map (AO = 1 everywhere), which the runtime multiply treats as "no darkening"
   — identical to having *no* map at all. Writing it only inflates the pk4 and the runtime
   texture pool for zero visual effect. So after the post pass, run a cheap min-reduction
   over the **covered** texels (a parallel reduction on the GPU, or on the read-back buffer)
   and **skip the write** when the darkest covered texel is at/above a near-white threshold
   (cvar `r_occlusionMapBakeSkipWhite`, default ~0.996 to absorb quantisation; 1.0 = only
   exactly-white). This is behaviour-neutral: the runtime already treats an absent occlusion
   image as unoccluded (`RB_RHI_SurfaceOcclusionImage` → NULL → `occlusionParms[0] = 0`).
   Flat panels, simple un-creased surfaces, and many open convex surfaces fall out for free.
   (If a model bakes to *zero* saved maps, write nothing for it at all.)

### 3.3 Parity & determinism
- Byte-path compatible output: same filenames, same surface-index keying, same TGA format.
- Deterministic sampling so re-bakes are reproducible (fixed seed + blue-noise table).
- Honor every existing `r_occlusionMapBake*` cvar so results are comparable to the CPU bake.
- **Share the white-map skip (§3.2 step 7) with the CPU baker** — same behaviour-neutral
  win regardless of backend. A one-time re-pack could also prune solid-white maps already
  sitting in the shipped `z_baked_ao*.pk4` set (free size/VRAM back).

---

## 4. Phasing

- **M0 — headless VK compute proof:** stand up a surfaceless VK device in tool mode; port
  the `RayQuerySelfTest` kernel to run outside the client. Proves the context + ray-query.
- **M1 — flat-mesh GPU bake:** BLAS from base mesh + UV-space G-buffer + trace kernel +
  readback for ONE surface, then whole model. A/B vs CPU baker on flat surfaces → should
  match within tolerance. This alone delivers the 10×+ speedup.
- **M2 — tessellated/displaced bake:** apply `tess.glsl` displacement at the reference
  factor to the baked + occluder geometry (§2). Validate on an eared head (ear crease AO
  should now appear). This is the correctness win.
- **M3 — post-passes + full parity:** dilate/contact/contrast to match CPU semantics; sweep
  a folder; diff maps.
- **M4 — integration:** `bakeAO`/`bakeAOFolder` gain a GPU path (new `r_occlusionMapBakeGPU`
  default, or `gpuBakeAO*` commands); `bake_ao.sh` headless-VK mode; **CPU fallback** when
  no RT device. Update `docs/occlusion-maps.md`.

---

## 5. Risks / open questions
- **Headless VK device in tool mode** — the main unknown; needs surfaceless init. Low risk
  on NVIDIA Linux, must verify on the Windows cross-build too (or keep CPU fallback there).
- **UV-space seams** — conservative raster vs dilate; pick what matches CPU seam behavior.
- **Reference tess factor** — single global vs per-material; start global, measure.
- **Determinism across drivers** — fix the sample set; don't rely on wavefront ordering.
- **Quality parity sign-off** — needs user screenshots (never auto-launched/viewed here).

## 6. Non-goals
- No change to runtime RTAO or to the AO map format/paths (pure drop-in for the offline
  tool). No change to which surfaces get AO (same eyewear/translucent skip). Not removing
  the CPU baker — it stays as the portable fallback.
