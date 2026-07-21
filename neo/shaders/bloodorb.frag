// Translated from d3xp glprogs/bloodOrb1-3.vfp (fragment program).
// Blends _currentRender (weighted by a mask alpha) with an orb texture warped
// toward screen center, then tints the result. The three ARB variants differ
// ONLY in the redBlend tint constant:
//   bloodOrb1 = (1, 1, 1), bloodOrb2 = (1, 0.98, 0.98), bloodOrb3 = (0.98, 0.98, 1)
// supplied here via u_localParam1.rgb (the backend picks the tint per material).

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentRender;  // texture[0]
SAMPLER_BINDING(1) uniform sampler2D u_orbMap;         // texture[1]
SAMPLER_BINDING(2) uniform sampler2D u_maskMap;        // texture[2]

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	const float warpFactor = 0.995;

	// mask alpha comes from the green channel of texture[2]
	float alpha = texture( u_maskMap, var_TexCoord ).y;

	// perturbed _currentRender coords, warped toward screen center (projective)
	vec4 r0 = gl_FragCoord * u_windowCoord;   // R0 = fragment.position * env[1]
	float invW = 1.0 / r0.w;
	r0.w = 1.0;
	r0.xy *= invW;
	r0.xy = ( r0.xy - 0.5 ) * warpFactor + 0.5;
	vec4 warpedAdjustedTC = r0 * u_screenCorrection;   // * env[0] (NPOT adjust)

	// plain screen-adjusted coords (projective)
	vec4 adjustedTC = gl_FragCoord * u_windowCoord * u_screenCorrection;

	vec4 c0 = textureProj( u_currentRender, adjustedTC ) * alpha;
	vec4 c1 = textureProj( u_orbMap, warpedAdjustedTC ) * ( 1.0 - alpha );

	// redBlend.w is 1 in every variant, so alpha passes through
	fragColor = vec4( u_localParam1.rgb, 1.0 ) * ( c0 + c1 );
}
