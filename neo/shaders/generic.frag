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
	fragColor = c;
}
