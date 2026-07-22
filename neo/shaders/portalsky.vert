// Translated from d3xp glprogs/portalSky.vfp (vertex program).

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;

VARY(0) out vec2 var_TexCoord;  // scrolled uv per the VP; the ARB FP ignores it

void main() {
	var_TexCoord = attr_TexCoord + u_localParam0.xy;
	gl_Position = u_mvpMatrix * attr_Position;

	// Pin to the far plane (z/w = 1) so the sky is behind ALL geometry and can
	// never cull it. The backend pairs this with depth-LEQUAL + no depth write
	// and skips the surface in the depth prepass — classic skybox behavior,
	// chosen over depth-exact fidelity because forceOpaque portal-sky surfaces
	// otherwise seal the level and occlude what's in front of them.
	gl_Position.z = gl_Position.w;
}
