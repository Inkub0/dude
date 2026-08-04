// DUDE tessellation (docs/tessellation.md): SSAO normal G-buffer evaluation shader.
// Same domain / spacing / winding and the same PN position + displacement as
// zfill.tese, so the normal buffer's geometry lines up with the depth prepass and the
// lit render — SSAO's per-pixel normals then follow the rounded silhouette instead of
// hugging the flat, un-tessellated edges. The view-space tangent frame (T/B/N) and the
// texcoords are just interpolated barycentrically, like any varying.

#include "renderparms.glsl"
#include "tess.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_bumpMap;	// unit 0 = the material bump; also drives displacement

layout(triangles, equal_spacing, cw) in;

VARY(0) in vec2 i_TexBump[];
VARY(1) in vec3 i_T[];
VARY(2) in vec3 i_B[];
VARY(3) in vec3 i_N[];
VARY(4) in vec2 i_TexCoverage[];
VARY(5) in vec3 i_ModelPos[];
VARY(6) in vec3 i_ModelNormal[];

VARY(0) out vec2 var_TexBump;
VARY(1) out vec3 var_T;
VARY(2) out vec3 var_B;
VARY(3) out vec3 var_N;
VARY(4) out vec2 var_TexCoverage;

void main() {
	vec3 tc = gl_TessCoord;

	vec3 pos = dudeTessPN( i_ModelPos[0], i_ModelPos[1], i_ModelPos[2],
	                       normalize( i_ModelNormal[0] ),
	                       normalize( i_ModelNormal[1] ),
	                       normalize( i_ModelNormal[2] ), tc );

	var_TexBump     = i_TexBump[0] * tc.x + i_TexBump[1] * tc.y + i_TexBump[2] * tc.z;
	var_TexCoverage = i_TexCoverage[0] * tc.x + i_TexCoverage[1] * tc.y + i_TexCoverage[2] * tc.z;
	var_T = i_T[0] * tc.x + i_T[1] * tc.y + i_T[2] * tc.z;
	var_B = i_B[0] * tc.x + i_B[1] * tc.y + i_B[2] * tc.z;
	var_N = i_N[0] * tc.x + i_N[1] * tc.y + i_N[2] * tc.z;

	// same displacement as zfill.tese (same bump texel, same interpolated geometric
	// normal) so the normal buffer's depth/shape matches the depth prepass.
	vec3 geoN = normalize( i_ModelNormal[0] * tc.x + i_ModelNormal[1] * tc.y + i_ModelNormal[2] * tc.z );
	pos = dudeTessDisplace( pos, geoN, u_bumpMap, var_TexBump );

	gl_Position = u_mvpMatrix * vec4( pos, 1.0 );
}
