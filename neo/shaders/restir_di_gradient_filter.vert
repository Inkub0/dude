// DUDE ReSTIR DI R1 gradient-confidence passes (docs/rtx-restir-di.md Plan R1). Fullscreen NDC quad; identity mvp,
// st 0..1 (restir_di.vert pattern).

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;

VARY(0) out vec2 var_TexCoord;

void main() {
	var_TexCoord = attr_TexCoord;
	gl_Position = u_mvpMatrix * attr_Position;
}
