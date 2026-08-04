// DUDE tessellation (docs/tessellation.md): ambient-light evaluation shader.
// Same domain / spacing / winding and PN evaluation as interaction.tese so the
// ambient pass depth matches the prepass and the per-light interactions
// (depth-EQUAL). Linearly interpolates the ambient varyings.

#include "renderparms.glsl"
#include "tess.glsl"

SAMPLER_BINDING(1) uniform sampler2D u_bumpMap;	// normal map, for Phase 2 displacement

layout(triangles, equal_spacing, cw) in;

VARY(0) in vec2 i_TexBump[];
VARY(1) in vec2 i_TexDiffuse[];
VARY(2) in vec2 i_TexFalloff[];
VARY(3) in vec4 i_TexProjection[];
VARY(4) in vec3 i_ToGlobalRow0[];
VARY(5) in vec3 i_ToGlobalRow1[];
VARY(6) in vec3 i_ToGlobalRow2[];
VARY(7) in vec4 i_Color[];
VARY(8) in vec3 i_ModelPos[];
VARY(9) in vec3 i_ModelNormal[];

VARY(0) out vec2 var_TexBump;
VARY(1) out vec2 var_TexDiffuse;
VARY(2) out vec2 var_TexFalloff;
VARY(3) out vec4 var_TexProjection;
VARY(4) out vec3 var_ToGlobalRow0;
VARY(5) out vec3 var_ToGlobalRow1;
VARY(6) out vec3 var_ToGlobalRow2;
VARY(7) out vec4 var_Color;

void main() {
	vec3 tc = gl_TessCoord;

	vec3 pos = dudeTessPN( i_ModelPos[0], i_ModelPos[1], i_ModelPos[2],
	                       normalize( i_ModelNormal[0] ),
	                       normalize( i_ModelNormal[1] ),
	                       normalize( i_ModelNormal[2] ), tc );

	var_TexBump       = i_TexBump[0]       * tc.x + i_TexBump[1]       * tc.y + i_TexBump[2]       * tc.z;
	var_TexDiffuse    = i_TexDiffuse[0]    * tc.x + i_TexDiffuse[1]    * tc.y + i_TexDiffuse[2]    * tc.z;
	var_TexFalloff    = i_TexFalloff[0]    * tc.x + i_TexFalloff[1]    * tc.y + i_TexFalloff[2]    * tc.z;
	var_TexProjection = i_TexProjection[0] * tc.x + i_TexProjection[1] * tc.y + i_TexProjection[2] * tc.z;
	var_ToGlobalRow0  = i_ToGlobalRow0[0]  * tc.x + i_ToGlobalRow0[1]  * tc.y + i_ToGlobalRow0[2]  * tc.z;
	var_ToGlobalRow1  = i_ToGlobalRow1[0]  * tc.x + i_ToGlobalRow1[1]  * tc.y + i_ToGlobalRow1[2]  * tc.z;
	var_ToGlobalRow2  = i_ToGlobalRow2[0]  * tc.x + i_ToGlobalRow2[1]  * tc.y + i_ToGlobalRow2[2]  * tc.z;
	var_Color         = i_Color[0]         * tc.x + i_Color[1]         * tc.y + i_Color[2]         * tc.z;

	// optional normal-map displacement along the interpolated geometric normal
	vec3 geoN = normalize( i_ModelNormal[0] * tc.x + i_ModelNormal[1] * tc.y + i_ModelNormal[2] * tc.z );
	pos = dudeTessDisplace( pos, geoN, u_bumpMap, var_TexBump );

	gl_Position = u_mvpMatrix * vec4( pos, 1.0 );
}
