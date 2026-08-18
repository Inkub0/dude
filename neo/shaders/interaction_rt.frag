// DUDE RT sun shadows (docs/rtx-shadow-roadmap.md R3): the ray-query interaction variant.
// A separate program because the SPIR-V ray-query capability fails pipeline creation on
// non-RT Vulkan hardware; the backend loads this only when the device reports KHR_ray_query,
// and the frontend binds it only on mode-4 (RT sun) interaction draws.
#define DUDE_RT_SUN 1
#include "interaction.frag"
