// DUDE gamma/brightness pass (r_gammaInShader on the GL 3.3 core backend).
// Fullscreen draw over the finished framebuffer, run right before buffer swap.
// The core context has no fixed-function/ARB gamma and SDL3 has no hardware
// gamma ramp, so this is where r_gamma/r_brightness are applied there.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;

VARY(0) out vec2 var_TexCoord;

void main() {
	var_TexCoord = attr_TexCoord;
	gl_Position = u_mvpMatrix * attr_Position;
}
