// DUDE HDR resolve (r_hdr). Fullscreen draw that copies the RGBA16F scene buffer
// back onto the SDR backbuffer at buffer-swap time, right before the gamma pass.
// Phase A is a straight passthrough; Phase B folds exposure + tonemap in here.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;

VARY(0) out vec2 var_TexCoord;

void main() {
	var_TexCoord = attr_TexCoord;
	gl_Position = u_mvpMatrix * attr_Position;
}
