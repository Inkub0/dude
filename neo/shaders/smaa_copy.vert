// DUDE SMAA pass 0: de-POT copy of the _currentRender snapshot into an
// exact-size scene target, so the SMAA passes get clean [0,1] texel math
// (the POT padding would otherwise bleed into border searches). LDR path
// only; the HDR path samples its exact-size float scene buffer directly.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;

VARY(0) out vec2 var_TexCoord;

void main() {
	var_TexCoord = attr_TexCoord;
	gl_Position = u_mvpMatrix * attr_Position;
}
