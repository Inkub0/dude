// DUDE: mirrorRenderMap / xrayRenderMap stages (GitHub #34), see mirrorrender.vert.
// Fixed-function GL_MODULATE of an RGB capture: the texel's alpha is 1, so the stage colour's alpha
// (e.g. "alpha 0.20" with "blend blend") alone sets the blend weight. The capture may carry scene
// alpha (RGBA16F in HDR frames), which is ignored like the vanilla GL_RGB copy ignored it.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_map;

VARY(0) in vec3 var_TexCoord;
VARY(1) in vec4 var_Color;

layout(location = 0) out vec4 fragColor;

void main() {
	vec2 tc = var_TexCoord.xy / max( var_TexCoord.z, 1e-6 );
	fragColor = vec4( texture( u_map, tc ).rgb, 1.0 ) * var_Color;
}
