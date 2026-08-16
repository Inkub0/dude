// DUDE weapon-reload depth-of-field: fullscreen quad, identity mvp.
// Companion to depthoffield.frag; same fullscreen-quad path as postprocess.vert.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;

VARY(0) out vec2 var_TexCoord;

void main() {
	var_TexCoord = attr_TexCoord;
	gl_Position = u_mvpMatrix * attr_Position;
}
