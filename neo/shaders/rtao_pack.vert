// DUDE RTAO normal-roughness guide pass (docs/rtx-rtao.md H4b).
// Fullscreen NDC quad over _currentDepth; identity mvp, st 0..1 (ssao.vert pattern).

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;

VARY(0) out vec2 var_TexCoord;

void main() {
	var_TexCoord = attr_TexCoord;
	gl_Position = u_mvpMatrix * attr_Position;
}
