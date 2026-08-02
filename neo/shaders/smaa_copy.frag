// DUDE SMAA pass 0: de-POT copy (see smaa_copy.vert).
// u_screenCorrection.xy = content extent in the oversized POT snapshot.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentRender;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	fragColor = texture( u_currentRender, var_TexCoord * u_screenCorrection.xy );
}
