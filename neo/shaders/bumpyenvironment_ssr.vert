// bumpyEnvironment.vert + view-space varyings for the glass SSR march
// (docs/ssr.md, glass extension). The global-space cube math is untouched; the
// march additionally needs the fragment's view-space position and a tangent ->
// view rotation so the per-pixel bump normal can reflect the screen-space ray.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;
layout(location = 2) in vec3 attr_Normal;
layout(location = 3) in vec3 attr_Tangent;
layout(location = 4) in vec3 attr_Bitangent;
layout(location = 5) in vec4 attr_Color;

VARY(0) out vec2 var_TexCoord;      // texcoord[0]
VARY(1) out vec3 var_ToEyeGlobal;   // texcoord[1]
VARY(2) out vec3 var_ToGlobalRow0;  // texcoord[2]
VARY(3) out vec3 var_ToGlobalRow1;  // texcoord[3]
VARY(4) out vec3 var_ToGlobalRow2;  // texcoord[4]
VARY(5) out vec4 var_Color;
VARY(6) out vec3 var_ViewPos;       // view-space position for the march
VARY(7) out vec3 var_ViewTangent;   // tangent basis rotated to view space
VARY(8) out vec3 var_ViewBitangent;
VARY(9) out vec3 var_ViewNormal;

void main() {
	var_TexCoord = attr_TexCoord;

	// vector to eye in global coordinates
	vec3 toEye = u_localViewOrigin.xyz - attr_Position.xyz;
	var_ToEyeGlobal = vec3( dot( toEye, u_modelMatrixRow0.xyz ),
	                        dot( toEye, u_modelMatrixRow1.xyz ),
	                        dot( toEye, u_modelMatrixRow2.xyz ) );

	// tangent space -> global space rows
	var_ToGlobalRow0 = vec3( dot( attr_Tangent,   u_modelMatrixRow0.xyz ),
	                         dot( attr_Bitangent, u_modelMatrixRow0.xyz ),
	                         dot( attr_Normal,    u_modelMatrixRow0.xyz ) );
	var_ToGlobalRow1 = vec3( dot( attr_Tangent,   u_modelMatrixRow1.xyz ),
	                         dot( attr_Bitangent, u_modelMatrixRow1.xyz ),
	                         dot( attr_Normal,    u_modelMatrixRow1.xyz ) );
	var_ToGlobalRow2 = vec3( dot( attr_Tangent,   u_modelMatrixRow2.xyz ),
	                         dot( attr_Bitangent, u_modelMatrixRow2.xyz ),
	                         dot( attr_Normal,    u_modelMatrixRow2.xyz ) );

	var_Color = attr_Color;

	// model -> view for the screen-space march (rigid model matrices, so the
	// upper 3x3 rotates the tangent basis correctly)
	mat3 mv = mat3( u_modelViewMatrix );
	var_ViewPos       = ( u_modelViewMatrix * attr_Position ).xyz;
	var_ViewTangent   = mv * attr_Tangent;
	var_ViewBitangent = mv * attr_Bitangent;
	var_ViewNormal    = mv * attr_Normal;

	gl_Position = u_mvpMatrix * attr_Position;
}
