// Translated from glprogs/heatHaze.vfp (fragment program).
// Screen-space refraction: sample _currentRender at the fragment's screen
// position, offset by the scaled normal-map distortion. Stock glass panes use
// this program as their see-through refraction, so the base MUST be the
// screen position (fragment.position * env[1] in the ARB original) — an
// earlier translation used the surface texcoord as the base, which repainted
// glass with a UV-mapped (upside-down) copy of the scene.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentRender; // _currentRender
SAMPLER_BINDING(1) uniform sampler2D u_normalMap;     // distortion normal map

VARY(0) in vec2 var_TexDistort;
VARY(1) in vec2 var_DeformMag;

layout(location = 0) out vec4 fragColor;

void main() {
	// normal map fetch + RXGB unpack (MOV localNormal.x, localNormal.a)
	vec4 bump = texture( u_normalMap, var_TexDistort );
	bump.x = bump.a;
	vec2 localNormal = bump.xy * 2.0 - 1.0;

	// screen texcoord in 0..1; u_windowCoord.w flips the row on Vulkan
	// (top-down gl_FragCoord vs the GL-layout capture), 0 on GL
	vec2 screenTc = gl_FragCoord.xy * u_windowCoord.xy + vec2( 0.0, u_windowCoord.w );
	screenTc = clamp( localNormal * var_DeformMag + screenTc, 0.0, 1.0 );
	screenTc *= u_screenCorrection.xy;

	fragColor = vec4( texture( u_currentRender, screenTc ).xyz, 1.0 );
}
