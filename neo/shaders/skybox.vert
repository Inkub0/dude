// Skybox / wobblesky cube (TG_SKYBOX_CUBE, TG_WOBBLESKY_CUBE). The texcoord is
// the direction from the (local-space) view origin to the vertex, optionally
// run through the wobblesky rotation — matching R_SkyboxTexGen /
// R_WobbleskyTexGen (which the fixed-function path baked into a dynamic vertex
// stream; computed here instead). The rotation arrives in u_modelMatrixRow0..2
// (identity for a plain skybox).

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;

VARY(0) out vec3 var_TexCoord;

void main() {
	vec3 dir = attr_Position.xyz - u_localViewOrigin.xyz;
	var_TexCoord = vec3( dot( dir, u_modelMatrixRow0.xyz ),
	                     dot( dir, u_modelMatrixRow1.xyz ),
	                     dot( dir, u_modelMatrixRow2.xyz ) );

	gl_Position = u_mvpMatrix * attr_Position;

	// far plane (z/w = 1): the sky sits behind all geometry and never culls it
	// (paired with depth-LEQUAL + no depth write + prepass skip in the backend)
	gl_Position.z = gl_Position.w;
}
