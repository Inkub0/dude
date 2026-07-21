// DUDE post-process pass (film grain, chromatic aberration).
// Fullscreen draw over _currentRender, run after the 3D view and BEFORE any
// 2D/GUI rendering so the HUD is never affected. Wired in Phase 3 (Chunk F).

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;

VARY(0) out vec2 var_TexCoord;

void main() {
	var_TexCoord = attr_TexCoord;
	gl_Position = u_mvpMatrix * attr_Position;
}
