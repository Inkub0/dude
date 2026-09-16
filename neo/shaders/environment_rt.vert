// DUDE RT glass reflections (docs/rtx-reflections.md RR5c) — vertex stage, unbumped.
// The RT counterpart of environment.vert: instead of a cube-reflection texcoord it hands the
// fragment everything needed to trace a WORLD-space reflection ray off the glass surface — the
// world position (ray origin), the world normal, and the world view direction. The model->world
// rows arrive in u_modelMatrixRow0..2 (set for every TG_REFLECT_CUBE stage by the backend), so this
// works for glass on moving entities too, not just static world brushes. Stage colour (tint) folds
// in exactly as environment.vert does, so the RT reflection is dimmed/tinted like the cube was.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 2) in vec3 attr_Normal;
layout(location = 5) in vec4 attr_Color;

VARY(0) out vec3 var_WorldPos;      // ray origin
VARY(1) out vec3 var_WorldNormal;   // reflect normal
VARY(2) out vec3 var_WorldToEye;    // surface -> viewer (world)
VARY(3) out vec4 var_Color;         // stage colour (tint / strength), SVC applied

void main() {
	// world position: M * localPos. u_modelMatrixRow0..2 are M's rows (.w = translation), so a full
	// vec4 dot with attr_Position (w = 1) gives the translated world coordinate — the ray origin.
	var_WorldPos = vec3( dot( u_modelMatrixRow0, attr_Position ),
	                     dot( u_modelMatrixRow1, attr_Position ),
	                     dot( u_modelMatrixRow2, attr_Position ) );

	// rotate the local normal + local view vector into world space by M's rotation (rows.xyz),
	// matching bumpyenvironment.vert's toEye transform ( (M v)_i = dot(row_i, v) ).
	var_WorldNormal = vec3( dot( u_modelMatrixRow0.xyz, attr_Normal ),
	                        dot( u_modelMatrixRow1.xyz, attr_Normal ),
	                        dot( u_modelMatrixRow2.xyz, attr_Normal ) );
	vec3 localToEye = u_localViewOrigin.xyz - attr_Position.xyz;
	var_WorldToEye = vec3( dot( u_modelMatrixRow0.xyz, localToEye ),
	                       dot( u_modelMatrixRow1.xyz, localToEye ),
	                       dot( u_modelMatrixRow2.xyz, localToEye ) );

	// stage colour, vertex-colour mode applied first — same fold as environment.vert so the RT
	// reflection carries the material's dimming tint (SVC_IGNORE -> u_color only).
	var_Color = ( attr_Color * u_vertexColorModulate + u_vertexColorAdd ) * u_color;
	gl_Position = u_mvpMatrix * attr_Position;
}
