// New shader (was fixed function): fog pass. Both units' texcoords come from
// eye-space texgen planes. Unit 1 is the "fog enter" fade: S is constant per
// viewer (distance of the eye to the fog terminator plane), T varies per
// vertex (distance of the surface to that plane) — matching the old fixed-
// function RB_T_BasicFog, where dropping the per-vertex T loses the soft
// enter/exit transition.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;

VARY(0) out vec2 var_TexFog;       // texgen S/T planes
VARY(1) out vec2 var_TexFogEnter;  // enter fade: S constant, T per-vertex

void main() {
	var_TexFog      = vec2( dot( attr_Position, u_texGen0S ), dot( attr_Position, u_texGen0T ) );
	var_TexFogEnter = vec2( dot( attr_Position, u_texGen1S ), dot( attr_Position, u_texGen1T ) );
	gl_Position = u_mvpMatrix * attr_Position;
}
