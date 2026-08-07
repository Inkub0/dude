# Shared GLSL shader source (Phase 2 of the Vulkan port)

Translations of the ARB assembly programs (`glprogs/*.vfp` in `pak000.pk4` plus the
soft-particle shader embedded in `draw_arb2.cpp`), written once and compiled two ways:

- **OpenGL 3.3 core**: prepend [prelude.gl.glsl](prelude.gl.glsl), resolve `#include`
  textually (the engine loader does this — GL drivers have no include support).
- **Vulkan (SPIR-V)**: prepend [prelude.vk.glsl](prelude.vk.glsl), compile with glslang
  (`GL_GOOGLE_include_directive` handles the include), target Vulkan 1.0.

Shader files carry **no `#version` line** — the prelude provides it plus the macros
(`UBO_BINDING`, `SAMPLER_BINDING`, `VARY`) that abstract the binding-model
differences. `VARY(n)` marks inter-stage varyings: SPIR-V requires explicit
locations on them while GLSL 330 forbids the qualifier (varyings match by name
there), so the macro expands to `layout(location = n)` only for Vulkan. Vertex
`out` and fragment `in` lists must be kept in the same order with the same numbers.

**Validation**: `python3 neo/shaders/validate.py` compiles every shader for both
targets with glslangValidator (all 56 compiles must pass).

## Interface contract (backend must provide)

**Vertex attributes** (fixed locations, matching idDrawVert / shadowCache_t):

| loc | name | data |
|-----|------|------|
| 0 | `attr_Position` | vec4 (w=1 for idDrawVert; shadow verts pass real w) |
| 1 | `attr_TexCoord` | vec2 `idDrawVert::st` |
| 2 | `attr_Normal` | vec3 |
| 3 | `attr_Tangent` | vec3 `tangents[0]` |
| 4 | `attr_Bitangent` | vec3 `tangents[1]` |
| 5 | `attr_Color` | vec4 normalized ubyte |

**Uniforms**: one shared std140 block, [renderparms.glsl](renderparms.glsl), updated
per draw. The member comments give the exact ARB `program.env[N]` / `program.local[N]`
slot each value replaces (the ARB slots were overloaded per-program; here every purpose
has its own named member). Matrices replace `state.matrix.*` — ARB programs used
`OPTION ARB_position_invariant` (fixed-function transform), which becomes an explicit
`u_mvpMatrix` multiply.

**Samplers**: texture-unit numbers preserved from the ARB programs (interaction:
0=normalization cube, 1=bump, 2=falloff, 3=projection, 4=diffuse, 5=specular,
6=specular LUT).

## Hardware-baseline compliance (Vulkan 1.1 / GL 3.3)

These shaders are written to fit the guaranteed *minimum* limits of the baseline
profile, so they run on the oldest targeted hardware without per-device shader
variants. Any **new** shader added here must keep within these — they are the
constraint, not the current maximums:

- **≤ 2 descriptor sets** (set 0 = RenderParams UBO, set 1 = samplers); guaranteed
  `maxBoundDescriptorSets` ≥ 4. Current max used: 2.
- **≤ 16 samplers per stage** (`maxPerStageDescriptorSampledImages`). Current max:
  interaction.frag = 7.
- **≤ 16 vec4 (64 components) of varyings.** Current max: interaction = 22 components.
- **No geometry/tessellation shaders** — not guaranteed on baseline hardware *and*
  not part of the faithful renderer. Vertex + fragment only.
- **No `push_constant` blocks** — all per-draw data goes through the RenderParams UBO
  (it is ~700 B, far over the 128 B push-constant guarantee). If a fast-path push
  constant is ever added, it must be ≤ 128 B.
- Sampler filtering (anisotropy), wireframe, wide lines, and depth clamp are **pipeline
  state, not shader code** — gated in the backend by optional-feature queries (see
  docs/vulkan-port.md Hardware baseline); nothing here depends on them.

Forward note: **Phase 8 shadow-map shaders** (cubemap/cascade layers) will use
`multiview` (1.1 core) and thus need `#extension GL_EXT_multiview` + `gl_ViewIndex`;
none of the current shaders do.

## Translated

| GLSL | replaces | notes |
|------|----------|-------|
| interaction.vert/.frag | interaction.vfp | main per-light pass; keeps normalization-cubemap for the light vector and math-normalize for the half angle, RXGB `.x = .a` swizzle, LUT specular — pixel-faithful |
| ambientlight.vert/.frag | ambientLight.vfp | ambient cube version of interaction |
| environment.vert/.frag | environment.vfp | per-pixel cube reflection |
| bumpyenvironment.vert/.frag | bumpyEnvironment.vfp | bumped cube reflection |
| shadow.vert/.frag | shadow.vp | stencil shadow volume projection (fragment writes masked color) |
| softparticle.vert/.frag | embedded TDM shader | see depth-consts caveat below |
| heathaze.vert/.frag | heatHaze.vfp | screen-space refraction |
| heathaze_mask.vert/.frag | heatHazeWithMask.vfp | + mask texture, `KIL` → `discard` |
| heathaze_maskvertex.vert/.frag | heatHazeWithMaskAndVertex.vfp | + vertex-color fade |
| heathaze_maskvertex_mask.vert/.frag | heatHazeWithMaskAndVertex.vfp (VS) + heatHazeWithMask.vfp (FS) | split pair — vppinch_bfgbolt, vpsphere |
| colorprocess.vert/.frag | colorProcess.vfp | grey-lerp post effect |
| zfill.vert/.frag | *(new — was fixed function)* | depth prepass w/ optional alpha test |
| generic.vert/.frag | *(new — was fixed function)* | GUI/2D/old material stages: texmatrix + vertex-color modes |
| berserk.vert/.frag | *(new — reimplements textures/decals/berserk)* | berserk-vision display: blits the accumulated feedback buffer (or the plain scene as fallback); the effect lives in berserk_accum |
| berserk_accum.vert/.frag | *(new — berserk feedback)* | faithful port of the stock `textures/decals/berserk` ARB material: `mix(scene, prevFrame·centerscale 0.95, berserk2.alpha)` every frame in a ping-pong RT — the reliable cross-frame feedback the recursive `_scratch` path lacks on RHI; look constants baked in (r_berserkFade is the game-driven wind-down only) |
| fog.vert/.frag | *(new — was fixed function)* | fog pass texgen planes |
| gammabrightness.vert/.frag | *(new — was hardware gamma / ARB env[21])* | final r_gamma/r_brightness pass (r_gammaInShader) |
| blendlight.vert/.frag | *(new — was fixed function)* | blend-light projection |
| portalsky.vert/.frag | d3xp portalSky.vfp | samples _currentRender at screen pos |
| bloodorb.vert/.frag | d3xp bloodOrb1-3.vfp | screen-warp orb; 3 tints via u_localParam1 |

The four *(new)* shaders replace fixed-function paths that never had ARB programs;
their exact parameter plumbing gets verified when the Phase 3 GL 3.3 backend wires
them up.

**d3xp (RoE) customs:** portalSky and bloodOrb1-3 are hand-translated above. The
remaining d3xp customs (`enviroSuit`, `flare`, `motionBlur`, `glasswarp`) lean on
projective screen-warps (`TXP` / `fragment.position.w`) — deliberately left for the
ARB→GLSL transpiler (below), which mechanizes `TXP`→`textureProj` and the `.w`
bookkeeping uniformly rather than risking hand-translation errors on each.

## Not translated yet (tracked follow-ups)

- `test.vfp` (r_testARBProgram debug aid) — low value, translate on demand.
- `megaTexture.vfp` — not present in retail pak000; only needed for megatexture materials.
- **d3xp customs, remaining** (`enviroSuit.vfp`, `flare.vfp`, `motionBlur.vfp`,
  `*glasswarp.txt`) — material-referenced expansion shaders with projective
  screen-warps; slated for the transpiler, must exist before flipping d3xp to the new
  backends. (`portalSky`, `bloodOrb1-3` already hand-translated.)
- **Cross-verification**: `scripts/crossdiff_shaders.py` executes hand-translated and
  transpiled shader pairs (two independent derivations of the same ARB source) as
  Python vec-ops with identical seeded inputs and compares outputs numerically —
  currently 11 pairs × vp+fp × 6 seeds, 120/120 matching. It caught one real hand
  translation bug (heathaze_maskvertex: mask×vertexColor must precede the kill test,
  fixed). Note: hand shaders number varyings sequentially while transpiled ones keep
  ARB texcoord indices (color at loc 8) — semantically fine per pair, mapped in the
  harness; the Phase 3 loader must never mix stages across the two conventions.
- **Transpiler status**: the ARB→GLSL transpiler exists (renderer/ArbProgram.{h,cpp}
  parser + renderer/ArbToGlsl.{h,cpp} codegen, `arbtool` CLI). Transpiled shaders use
  the raw ARB parameter model via [arbparams.glsl](arbparams.glsl) (u_env[]/u_local[]
  + state matrices) instead of RenderParams. Corpus result: 64/64 programs (base,
  d3xp, Phobos incl. bloom suite) transpile and all 128 outputs compile as GLSL 330
  and SPIR-V (`scripts/validate_transpiled.py`). Remaining: cross-diff vs the hand
  translations, degrade-don't-crash wiring in the Phase 3 loader.
- **Mod compatibility (goal: translate ARB from as many public mods as possible)**:
  materials can reference arbitrary custom ARB programs from mod pk4s. The target is a
  general **ARB assembly → GLSL transpiler** that handles the real-world corpus of
  publicly released Doom 3 / RoE mods, not just the stock set — so custom-shader mods
  keep working on the GL 3.3 and Vulkan backends without per-mod hand-porting.
  - **Test corpus**: collect `.vfp`/`.vp`/`.vfp`-style programs from as many public
    mods as we can (Phobos's 9 `.vfp` files, Sikkmod, Wulfen/Monoxead texture packs,
    Hexen: Edge of Chaos, The Dark Mod lineage, Rivensin/Ruiner, etc.) as a regression
    set the transpiler must compile.
  - **Policy for the long tail**: ship exact translations for the stock + common shaders;
    for an unknown ARB program, run it through the transpiler; if that fails, fall back
    to rendering the stage as a plain textured/blended stage (degrade, don't crash).
  - The legacy ARB loader stays functional on the classic GL path as the ultimate
    fallback and reference for validating transpiler output.
  - Scope boundary: this covers **shader** compatibility only. Mods that ship a compiled
    Windows `gamex86.dll` (game logic) are out of scope — see docs/vulkan-port.md
    "Mod compatibility scope"; those need source recompilation or Wine, not the engine.

## Caveats

- `softparticle.frag` inherits TDM's hard-coded `depth_consts`, derived from Doom 3's
  near-infinite-zFar projection matrix. **Vulkan's 0..1 clip depth changes these
  constants** — revisit when the VK backend lands (marked in the source).
- ARB programs left some result components undefined (`result.color.xyz` writes);
  the GLSL versions write alpha = 1.0 explicitly in those spots.
