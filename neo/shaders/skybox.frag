// Skybox / wobblesky cube: sample the cube map by the generated direction,
// modulated by the stage color (the fixed-function path used GL_MODULATE with
// the primary color).

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform samplerCube u_skyCubeMap;

VARY(0) in vec3 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	fragColor = texture( u_skyCubeMap, var_TexCoord ) * u_color;
}
