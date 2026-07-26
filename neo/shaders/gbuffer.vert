// DUDE SSAO normal G-buffer (docs/ssao-gtao.md). Renders opaque geometry writing the
// bump-mapped view-space normal, so SSAO uses real per-pixel normals (including normal-map
// detail) instead of reconstructing flat geometry from depth. Foundation for POM/displacement.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;
layout(location = 2) in vec3 attr_Normal;
layout(location = 3) in vec3 attr_Tangent;
layout(location = 4) in vec3 attr_Bitangent;

VARY(0) out vec2 var_TexBump;
VARY(1) out vec3 var_T;   // view-space tangent
VARY(2) out vec3 var_B;   // view-space bitangent
VARY(3) out vec3 var_N;   // view-space normal

void main() {
	vec4 st = vec4( attr_TexCoord, 0.0, 1.0 );
	var_TexBump = vec2( dot( st, u_bumpMatrixS ), dot( st, u_bumpMatrixT ) );

	// model -> view rotation (Doom 3 models carry no non-uniform scale, so the 3x3 is
	// an orthonormal rotation and normals/tangents transform with it directly)
	mat3 mv = mat3( u_modelViewMatrix );
	var_T = mv * attr_Tangent;
	var_B = mv * attr_Bitangent;
	var_N = mv * attr_Normal;

	gl_Position = u_mvpMatrix * attr_Position;
}
