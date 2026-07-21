// New shader (was fixed function): blend light projection times falloff,
// modulated by the light stage color.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_lightProjection;
SAMPLER_BINDING(1) uniform sampler2D u_lightFalloff;

VARY(0) in vec4 var_TexProjection;
VARY(1) in vec2 var_TexFalloff;

layout(location = 0) out vec4 fragColor;

void main() {
	fragColor = textureProj( u_lightProjection, var_TexProjection )
	          * texture( u_lightFalloff, var_TexFalloff )
	          * u_color;
}
