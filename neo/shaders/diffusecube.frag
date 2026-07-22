// Diffuse cube: sample the cube map by the vertex normal, modulated by the
// stage color.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform samplerCube u_cubeMap;

VARY(0) in vec3 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	fragColor = texture( u_cubeMap, var_TexCoord ) * u_color;
}
