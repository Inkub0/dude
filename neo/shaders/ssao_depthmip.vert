// SSAO Phase 1 linear-depth mip level 0 (docs/ssao-perf-optimization.md).
// Fullscreen NDC quad, identity mvp, st 0..1 — same as ssao.vert.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;

VARY(0) out vec2 var_TexCoord;

void main() {
	var_TexCoord = attr_TexCoord;
	gl_Position = u_mvpMatrix * attr_Position;
}
