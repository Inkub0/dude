// DUDE gamma/brightness pass (r_gammaInShader on the GL 3.3 core backend).
// Applies r_gamma/r_brightness to the whole finished frame, matching the math
// of the legacy hardware gamma table in R_SetColorMappings():
//     j = clamp( color * brightness, 0, 1 );  out = pow( j, 1/gamma );
//
// u_localParam0.x     = r_brightness
// u_localParam0.y     = 1.0 / r_gamma
// u_screenCorrection.xy = NPOT adjust into the oversized _currentRender copy

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentRender;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	vec3 color = texture( u_currentRender, var_TexCoord * u_screenCorrection.xy ).rgb;

	// brightness first (clamped like the hardware table), then gamma
	color = clamp( color * u_localParam0.x, 0.0, 1.0 );
	color = pow( color, vec3( u_localParam0.y ) );

	fragColor = vec4( color, 1.0 );
}
