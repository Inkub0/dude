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
	var_TexFog = vec2(0);
	var_TexFogEnter = vec2(0);

	vec4 s1 = u_texGen0S;
	vec4 t1 = u_texGen0T;
	vec4 s2 = u_texGen1S;
	vec4 t2 = u_texGen1T;
	var_TexFog = vec2(dot(attr_Position, s1), dot(attr_Position, t1));
	var_TexFogEnter = vec2(dot(attr_Position, s2), dot(attr_Position, t2));
	gl_Position = u_mvpMatrix * attr_Position;
}
