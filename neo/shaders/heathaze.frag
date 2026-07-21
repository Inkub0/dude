// Translated from glprogs/heatHaze.vfp (fragment program).

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentRender; // _currentRender
SAMPLER_BINDING(1) uniform sampler2D u_normalMap;     // distortion normal map

VARY(0) in vec2 var_TexDistort;
VARY(1) in vec2 var_DeformMag;

layout(location = 0) out vec4 fragColor;

void main() {
	// distortion normal, RXGB swizzle
	vec4 bump = texture( u_normalMap, var_TexDistort );
	bump.x = bump.a;
	vec2 localNormal = bump.xy * 2.0 - 1.0;

	// screen texcoord 0..1, offset by scaled normal, clamped
	vec2 screenTc = gl_FragCoord.xy * u_windowCoord.xy;
	screenTc = clamp( localNormal * var_DeformMag + screenTc, 0.0, 1.0 );

	// NPOT adjust into _currentRender
	screenTc *= u_screenCorrection.xy;

	fragColor = vec4( texture( u_currentRender, screenTc ).xyz, 1.0 );
}
