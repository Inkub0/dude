// DUDE hell-time / Artifact vision (D3XP) — TEMPORAL accumulation pass (vertex).
// Fullscreen NDC quad, identity mvp, st 0..1 passthrough — the same shape as
// berserk_accum.vert. The radial zoom/rotate feedback fold happens in the frag stage.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;

VARY(0) out vec2 var_TexCoord;

void main() {
	var_TexCoord = attr_TexCoord;
	gl_Position = u_mvpMatrix * attr_Position;
}
