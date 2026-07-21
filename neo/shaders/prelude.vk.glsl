#version 450

// Vulkan prelude. Set 0 holds the per-draw uniform block, set 1 the
// combined image samplers, addressed by the same unit numbers the ARB
// programs used.
// (glslang's Vulkan mode predefines the VULKAN macro; don't redefine it here)
#define UBO_BINDING(b) layout(std140, set = 0, binding = b)
#define SAMPLER_BINDING(b) layout(set = 1, binding = b)
// SPIR-V requires explicit locations on varyings (matched by location)
#define VARY(loc) layout(location = loc)
