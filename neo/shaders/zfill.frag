// New shader (was fixed function): depth prepass fill with optional alpha test.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_map;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	// the fetch only feeds the alpha test; && short-circuits on the uniform
	// enable, so plain opaque fills (the common case) skip the texture read
	if ( u_alphaTest.y != 0.0 && texture( u_map, var_TexCoord ).a < u_alphaTest.x ) {
		discard;
	}
	fragColor = u_color;
}
