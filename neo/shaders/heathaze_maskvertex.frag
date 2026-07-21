// Translated from glprogs/heatHazeWithMaskAndVertex.vfp (fragment program).
// The mask is additionally scaled by the vertex color for particle fading.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentRender;
SAMPLER_BINDING(1) uniform sampler2D u_normalMap;
SAMPLER_BINDING(2) uniform sampler2D u_maskMap;

VARY(0) in vec2 var_TexMask;
VARY(1) in vec2 var_TexDistort;
VARY(2) in vec2 var_DeformMag;
VARY(3) in vec4 var_Color;

layout(location = 0) out vec4 fragColor;

void main() {
	vec2 mask = texture( u_maskMap, var_TexMask ).xy;
	if ( mask.x - 0.01 < 0.0 || mask.y - 0.01 < 0.0 ) {
		discard;
	}
	mask *= var_Color.xy;

	vec4 bump = texture( u_normalMap, var_TexDistort );
	bump.x = bump.a;
	vec2 localNormal = ( bump.xy * 2.0 - 1.0 ) * mask;

	vec2 screenTc = gl_FragCoord.xy * u_windowCoord.xy;
	screenTc = clamp( localNormal * var_DeformMag + screenTc, 0.0, 1.0 );
	screenTc *= u_screenCorrection.xy;

	fragColor = vec4( texture( u_currentRender, screenTc ).xyz, 1.0 );
}
