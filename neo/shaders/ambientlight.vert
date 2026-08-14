// Translated from glprogs/ambientLight.vfp (vertex program).
// Ambient cube-map lighting; texcoord[4..6] carry the tangent-space ->
// ambient-map-space rows built from the model matrix rows (ARB env[20..22]).

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;
layout(location = 2) in vec3 attr_Normal;
layout(location = 3) in vec3 attr_Tangent;
layout(location = 4) in vec3 attr_Bitangent;
layout(location = 5) in vec4 attr_Color;

VARY(0) out vec2 var_TexBump;       // texcoord[0]
VARY(1) out vec2 var_TexDiffuse;    // texcoord[1]
VARY(2) out vec2 var_TexFalloff;    // texcoord[2]
VARY(3) out vec4 var_TexProjection; // texcoord[3]
VARY(4) out vec3 var_ToGlobalRow0;  // texcoord[4]
VARY(5) out vec3 var_ToGlobalRow1;  // texcoord[5]
VARY(6) out vec3 var_ToGlobalRow2;  // texcoord[6]
VARY(7) out vec4 var_Color;
// model-space position + normal for the tessellation stages (DUDE tessellation,
// docs/tessellation.md); unconsumed by ambientlight.frag in the flat pipeline.
VARY(8) out vec3 var_ModelPos;
VARY(9) out vec4 var_ModelNormal;	// .w = UV-seam displacement mask

void main() {
	var_ModelPos = attr_Position.xyz;
	var_ModelNormal = vec4( attr_Normal, attr_Color.a );

	vec4 st = vec4( attr_TexCoord, 0.0, 1.0 );

	var_TexBump    = vec2( dot( st, u_bumpMatrixS ),    dot( st, u_bumpMatrixT ) );
	var_TexDiffuse = vec2( dot( st, u_diffuseMatrixS ), dot( st, u_diffuseMatrixT ) );

	var_TexFalloff = vec2( dot( attr_Position, u_lightFalloffS ), 0.5 );

	var_TexProjection = vec4( dot( attr_Position, u_lightProjectionS ),
	                          dot( attr_Position, u_lightProjectionT ),
	                          0.0,
	                          dot( attr_Position, u_lightProjectionQ ) );

	// tangent-space normal -> global space transform rows
	// (v * M == transpose(M) * v, so no explicit transpose is needed)
	mat3 tbn = mat3( attr_Tangent, attr_Bitangent, attr_Normal );
	var_ToGlobalRow0 = u_modelMatrixRow0.xyz * tbn;
	var_ToGlobalRow1 = u_modelMatrixRow1.xyz * tbn;
	var_ToGlobalRow2 = u_modelMatrixRow2.xyz * tbn;

	var_Color = attr_Color * u_vertexColorModulate + u_vertexColorAdd;

	gl_Position = u_mvpMatrix * attr_Position;
}
