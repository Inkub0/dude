// Translated from d3xp glprogs/portalSky.vfp (vertex program).

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;

VARY(0) out vec2 var_TexCoord;  // scrolled uv per the VP; the ARB FP ignores it

void main() {
	var_TexCoord = attr_TexCoord + u_localParam0.xy;
	gl_Position = u_mvpMatrix * attr_Position;
}
