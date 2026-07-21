// New shader (was fixed function): depth prepass fill with optional alpha test.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_map;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	vec4 c = texture( u_map, var_TexCoord );
	if ( u_alphaTest.y != 0.0 && c.a < u_alphaTest.x ) {
		discard;
	}
	fragColor = u_color;
}
