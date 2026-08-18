// 460 (not 450): GL_EXT_ray_query declares its types only from GLSL 4.60 - under 450
// the extension enables but rayQueryEXT stays undeclared. 460 is a strict superset of
// 450 for everything this codebase writes; the SPIR-V target is vulkan1.4 either way.
#version 460

// Buffer references (GPU pointers) for manual vertex fetch — the Phase 3.2b BDA
// path (zfill_bda, docs/gpu-offload-plan.md §3.2b). Declared once here because
// compile_spv.py injects `invariant gl_Position;` ahead of each shader body, and
// an #extension must precede any statement. Inert for shaders that never name a
// buffer_reference type, so it changes no other shader's SPIR-V.
#extension GL_EXT_buffer_reference : require

// Vulkan prelude. Set 0 holds the per-draw uniform block, set 1 the
// combined image samplers, addressed by the same unit numbers the ARB
// programs used.
// (glslang's Vulkan mode predefines the VULKAN macro; don't redefine it here)
#define UBO_BINDING(b) layout(std140, set = 0, binding = b)
#define SAMPLER_BINDING(b) layout(set = 1, binding = b)
// SPIR-V requires explicit locations on varyings (matched by location)
#define VARY(loc) layout(location = loc)
// Transpiled-ARB fragment.position (ArbToGlsl emits RB_WPOS): Vulkan's gl_FragCoord is
// top-down, but the ARB programs (and the _currentRender capture they sample) assume GL's
// bottom-up window origin, so flip Y into GL convention. Viewport height = 1/u_fenv[1].y
// (the window-coord scale RB_RHI_RenderCustomStage always fills). Only transpiled shaders
// expand this — hand-written shaders use gl_FragCoord directly and flip via u_windowCoord.
#define RB_WPOS vec4( gl_FragCoord.x, ( 1.0 / u_fenv[1].y ) - gl_FragCoord.y, gl_FragCoord.z, gl_FragCoord.w )
