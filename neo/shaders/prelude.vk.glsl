#version 450

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
