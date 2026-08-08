// SSR Hi-Z min-Z mip-chain MIN-downsample (docs/ssao-perf-optimization.md, r_ssrHiZ).
// Fullscreen NDC quad, identity mvp — same as ssr_depthmin.vert.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;

VARY(0) out vec2 var_TexCoord;

void main() {
	var_TexCoord = attr_TexCoord;
	gl_Position = u_mvpMatrix * attr_Position;
}
