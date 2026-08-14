// SSAO Phase 1 depth-mip max-downsample (docs/ssao-perf-optimization.md).
// Fullscreen NDC quad, identity mvp — same as ssao_depthmip.vert.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;

VARY(0) out vec2 var_TexCoord;

void main() {
	var_TexCoord = attr_TexCoord;
	gl_Position = u_mvpMatrix * attr_Position;
}
