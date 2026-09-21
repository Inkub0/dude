// DUDE RT glass reflections (docs/rtx-reflections.md RR5c stage 2) — vertex stage, BUMPED.
// The RT counterpart of bumpyenvironment.vert, and the bumped sibling of environment_rt.vert: it hands
// the fragment everything needed to trace a WORLD-space reflection ray off a bump-perturbed glass
// surface. From bumpyenvironment.vert it keeps the texcoord + the tangent->global rows (so the frag can
// rotate the bump map's tangent-space normal into world space) + the world view vector; from
// environment_rt.vert it adds the world POSITION (ray origin) and folds the stage colour into var_Color
// (so the reflection carries the material's dimming tint / r_gl3ReflectionScale, exactly like the
// unbumped RT glass). Model->world rows arrive in u_modelMatrixRow0..2 (set for every TG_REFLECT_CUBE
// stage by the backend), so this works for bumped glass on moving entities too.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;
layout(location = 2) in vec3 attr_Normal;
layout(location = 3) in vec3 attr_Tangent;
layout(location = 4) in vec3 attr_Bitangent;
layout(location = 5) in vec4 attr_Color;

VARY(0) out vec2 var_TexCoord;      // bump map lookup
VARY(1) out vec3 var_WorldPos;      // ray origin (M * localPos)
VARY(2) out vec3 var_ToEyeGlobal;   // surface -> viewer (world)
VARY(3) out vec3 var_ToGlobalRow0;  // tangent-space -> global rows (as bumpyenvironment.vert)
VARY(4) out vec3 var_ToGlobalRow1;
VARY(5) out vec3 var_ToGlobalRow2;
VARY(6) out vec4 var_Color;         // stage colour (tint / strength), SVC applied

void main() {
	var_TexCoord = attr_TexCoord;

	// world position: M * localPos (u_modelMatrixRow0..2 are M's rows, .w = translation) — the ray origin.
	var_WorldPos = vec3( dot( u_modelMatrixRow0, attr_Position ),
	                     dot( u_modelMatrixRow1, attr_Position ),
	                     dot( u_modelMatrixRow2, attr_Position ) );

	// vector to eye in global coordinates (matches bumpyenvironment.vert)
	vec3 toEye = u_localViewOrigin.xyz - attr_Position.xyz;
	var_ToEyeGlobal = vec3( dot( toEye, u_modelMatrixRow0.xyz ),
	                        dot( toEye, u_modelMatrixRow1.xyz ),
	                        dot( toEye, u_modelMatrixRow2.xyz ) );

	// tangent space -> global space rows (bit-identical to bumpyenvironment.vert so the perturbed normal
	// the frag builds matches the cube path's; the flat column (tangent-space (0,0,1)) is var_ToGlobalRow2)
	var_ToGlobalRow0 = vec3( dot( attr_Tangent,   u_modelMatrixRow0.xyz ),
	                         dot( attr_Bitangent, u_modelMatrixRow0.xyz ),
	                         dot( attr_Normal,    u_modelMatrixRow0.xyz ) );
	var_ToGlobalRow1 = vec3( dot( attr_Tangent,   u_modelMatrixRow1.xyz ),
	                         dot( attr_Bitangent, u_modelMatrixRow1.xyz ),
	                         dot( attr_Normal,    u_modelMatrixRow1.xyz ) );
	var_ToGlobalRow2 = vec3( dot( attr_Tangent,   u_modelMatrixRow2.xyz ),
	                         dot( attr_Bitangent, u_modelMatrixRow2.xyz ),
	                         dot( attr_Normal,    u_modelMatrixRow2.xyz ) );

	// stage colour, vertex-colour mode applied first — same fold as environment_rt.vert so the RT
	// reflection carries the material's dimming tint (SVC_IGNORE -> u_color only, which the backend has
	// already scaled by r_gl3ReflectionScale for TG_REFLECT_CUBE).
	var_Color = ( attr_Color * u_vertexColorModulate + u_vertexColorAdd ) * u_color;
	gl_Position = u_mvpMatrix * attr_Position;
}
