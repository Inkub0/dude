// New shader (was fixed function): fog pass, two-texture modulate times
// the fog color.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_fogImage;      // alpha ramp
SAMPLER_BINDING(1) uniform sampler2D u_fogEnterImage; // enter ramp

VARY(0) in vec2 var_TexFog;
VARY(1) in vec2 var_TexFogEnter;

layout(location = 0) out vec4 fragColor;

void main() {
    float a = texture(u_fogImage, var_TexFog).a * texture(u_fogEnterImage, var_TexFogEnter).a;
    fragColor = vec4(u_color.xyz, a * u_color.a);
}