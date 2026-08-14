// DUDE berserk vision (RHI backends): the display blit. The whole effect lives in the
// feedback buffer built by berserk_accum (a faithful port of the stock ARB material
// textures/decals/berserk — see that shader), exactly as the stock captures the composited
// result into _scratch and blits it to the screen. So here we simply show the accumulated
// trail. If no trail is available (the backend can't build it — e.g. enhancements off), fall
// back to the plain captured scene so berserk still renders something sane.
//
//   unit 0 (u_map)   = _scratch, the live captured scene (fallback only)
//   unit 1 (u_trail) = the accumulated feedback buffer (the effect)
//   u_localParam0.z  = hasTrail (1 = show the trail, 0 = plain scene)

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_map;
SAMPLER_BINDING(1) uniform sampler2D u_trail;

VARY(0) in vec2 var_TexCoord;
VARY(1) in vec4 var_Color;

layout(location = 0) out vec4 fragColor;

void main() {
	vec4 col = ( u_localParam0.z > 0.5 ) ? texture( u_trail, var_TexCoord )
	                                     : texture( u_map, var_TexCoord );
	fragColor = col * var_Color;
}
