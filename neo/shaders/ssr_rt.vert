// DUDE RT reflections pass (docs/rtx-reflections.md RR2). Fullscreen NDC quad; identity mvp, st 0..1.
// Same fullscreen vertex as ssr_composite.vert — the frag does all the work.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;

VARY(0) out vec2 var_TexCoord;

void main() {
	var_TexCoord = attr_TexCoord;
	gl_Position = u_mvpMatrix * attr_Position;
}
