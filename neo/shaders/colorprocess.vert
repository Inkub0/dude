// Translated from glprogs/colorProcess.vfp (vertex program).
// Grey-lerp post effect; parameters arrive via material stage parms
// (ARB local[0] = fraction, local[1] = target hue).

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;

VARY(0) out vec4 var_InvFraction;  // texcoord[0]: 1 - fraction
VARY(1) out vec4 var_TargetScaled; // texcoord[1]: fraction * target color

void main() {
	var_InvFraction = vec4( 1.0 ) - u_localParam0;
	var_TargetScaled = u_localParam1 * u_localParam0;

	gl_Position = u_mvpMatrix * attr_Position;
}
