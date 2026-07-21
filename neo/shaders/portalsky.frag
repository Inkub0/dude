// Translated from d3xp glprogs/portalSky.vfp (fragment program).
// The portal-sky effect samples _currentRender at the screen position (showing
// the scene rendered behind it); the ARB FP ignores the scrolled texcoord.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentRender;

VARY(0) in vec2 var_TexCoord;  // unused (kept to match the VP output)

layout(location = 0) out vec4 fragColor;

void main() {
	vec2 screenTc = gl_FragCoord.xy * u_windowCoord.xy * u_screenCorrection.xy;
	fragColor = vec4( texture( u_currentRender, screenTc ).xyz, 1.0 );
}
