// New shader (was fixed function): fog pass. Both units' texcoords come from
// eye-space texgen planes. Unit 1 is the "fog enter" fade: S is constant per
// viewer (distance of the eye to the fog terminator plane), T varies per
// vertex (distance of the surface to that plane) — matching the old fixed-
// function RB_T_BasicFog, where dropping the per-vertex T loses the soft
// enter/exit transition.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;
layout(location = 2) in vec3 attr_Normal;
// DUDE tessellation (docs/tessellation.md): the alpha channel carries the
// per-vertex UV-seam displacement mask idMD5Mesh stamps in (see tess.glsl).
// Unused otherwise by this stage.
layout(location = 5) in vec4 attr_Color;

VARY(0) out vec2 var_TexFog;       // texgen S/T planes
VARY(1) out vec2 var_TexFogEnter;  // enter fade: S constant, T per-vertex
// model-space control net for the tessellation stages (DUDE tessellation): the fog
// interaction pass must PN-tessellate + displace bit-identically to zfill.tese, or the
// tessellated model's depth won't match under DEPTHFUNC_EQUAL and its fog is rejected
// (a dark, un-fogged silhouette). Unconsumed by fog.frag in the flat pipeline.
VARY(2) out vec3 var_ModelPos;
VARY(3) out vec4 var_ModelNormal;	// .w = UV-seam displacement mask
VARY(4) out vec2 var_TexBump;

void main() {
	var_TexFog      = vec2( dot( attr_Position, u_texGen0S ), dot( attr_Position, u_texGen0T ) );
	var_TexFogEnter = vec2( dot( attr_Position, u_texGen1S ), dot( attr_Position, u_texGen1T ) );

	var_ModelPos = attr_Position.xyz;
	var_ModelNormal = vec4( attr_Normal, attr_Color.a );
	vec4 st = vec4( attr_TexCoord, 0.0, 1.0 );
	var_TexBump = vec2( dot( st, u_bumpMatrixS ), dot( st, u_bumpMatrixT ) );

	gl_Position = u_mvpMatrix * attr_Position;
}
