// DUDE post-resolve antialiasing (FXAA) pass.
// Fullscreen draw over _currentRender, run after the finished 3D view and
// BEFORE any 2D/GUI so the HUD/menus are never smoothed. Wired as r_rhiAA.
// Same fullscreen setup as postprocess.vert (docs/antialiasing.md).

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;

VARY(0) out vec2 var_TexCoord;

void main() {
	var_TexCoord = attr_TexCoord;
	gl_Position = u_mvpMatrix * attr_Position;
}
