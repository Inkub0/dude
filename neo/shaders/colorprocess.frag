// Translated from glprogs/colorProcess.vfp (fragment program).

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentRender;

VARY(0) in vec4 var_InvFraction;
VARY(1) in vec4 var_TargetScaled;

layout(location = 0) out vec4 fragColor;

void main() {
	vec2 screenTc = gl_FragCoord.xy * u_windowCoord.xy * u_screenCorrection.xy;
	vec4 src = texture( u_currentRender, screenTc );

	// grey scale, scaled by target color, lerped against the source
	float grey = ( src.x + src.y + src.z ) * 0.33;
	vec4 target = grey * var_TargetScaled;

	fragColor = vec4( ( src * var_InvFraction + target ).xyz, 1.0 );
}
