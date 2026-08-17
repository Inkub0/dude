// New shader (was fixed function): textured draw modulated by color,
// with optional alpha test.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_map;

VARY(0) in vec2 var_TexCoord;
VARY(1) in vec4 var_Color;

layout(location = 0) out vec4 fragColor;

void main() {
	vec4 c = texture( u_map, var_TexCoord ) * var_Color;
	if ( u_alphaTest.y != 0.0 && c.a < u_alphaTest.x ) {
		discard;
	}
	// HDR overbright saturation compensation (r_hdrOverbrightSat via localParam0.x > 1): the tonemap
	// desaturates the very bright colours overbright pushes into, so pre-saturate here. Pushing the
	// colour away from its own luma boosts saturation; white (luma == colour) is untouched. 0 (the
	// default for every other generic stage) is a no-op.
	if ( u_localParam0.x > 1.0 ) {
		float luma = dot( c.rgb, vec3( 0.2126, 0.7152, 0.0722 ) );
		c.rgb = max( mix( vec3( luma ), c.rgb, u_localParam0.x ), 0.0 );
	}
	fragColor = c;
}
