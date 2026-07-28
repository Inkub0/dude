// DUDE GTAO temporal accumulation pass (docs/ssao-gtao.md).
// Fullscreen NDC quad; identity mvp, st 0..1. Same as ssao_blur.vert -- the
// reprojection matrix rides in u_modelViewMatrix, not u_mvpMatrix, so the quad
// still maps straight through.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;

VARY(0) out vec2 var_TexCoord;

void main() {
	var_TexCoord = attr_TexCoord;
	gl_Position = u_mvpMatrix * attr_Position;
}
