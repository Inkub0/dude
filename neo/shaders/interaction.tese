// DUDE tessellation (docs/tessellation.md): interaction evaluation shader.
// PN-smooths the model-space position; linearly interpolates the interaction
// varyings (identical to the flat path's rasterizer interpolation — PN only
// reshapes the silhouette, not the shading vectors); recomputes gl_Position.
// gl_Position is injected `invariant` by compile_spv.py so this matches the
// zfill tese bit-for-bit (depth-EQUAL interaction pass).

#include "renderparms.glsl"
#include "tess.glsl"

SAMPLER_BINDING(1) uniform sampler2D u_bumpMap;	// normal map, for Phase 2 displacement

layout(triangles, equal_spacing, cw) in;

VARY(0)  in vec3 i_TexLightVec[];
VARY(1)  in vec2 i_TexBump[];
VARY(2)  in vec2 i_TexFalloff[];
VARY(3)  in vec4 i_TexProjection[];
VARY(4)  in vec2 i_TexDiffuse[];
VARY(5)  in vec2 i_TexSpecular[];
VARY(6)  in vec3 i_TexHalfVec[];
VARY(7)  in vec4 i_Color[];
VARY(8)  in vec3 i_TexViewVec[];
VARY(9)  in vec3 i_ShadowCubeVec[];
VARY(10) in vec3 i_ModelPos[];
VARY(11) in vec3 i_ModelNormal[];

VARY(0) out vec3 var_TexLightVec;
VARY(1) out vec2 var_TexBump;
VARY(2) out vec2 var_TexFalloff;
VARY(3) out vec4 var_TexProjection;
VARY(4) out vec2 var_TexDiffuse;
VARY(5) out vec2 var_TexSpecular;
VARY(6) out vec3 var_TexHalfVec;
VARY(7) out vec4 var_Color;
VARY(8) out vec3 var_TexViewVec;
VARY(9) out vec3 var_ShadowCubeVec;

void main() {
	vec3 tc = gl_TessCoord;

	vec3 pos = dudeTessPN( i_ModelPos[0], i_ModelPos[1], i_ModelPos[2],
	                       normalize( i_ModelNormal[0] ),
	                       normalize( i_ModelNormal[1] ),
	                       normalize( i_ModelNormal[2] ), tc );

	var_TexLightVec   = i_TexLightVec[0]   * tc.x + i_TexLightVec[1]   * tc.y + i_TexLightVec[2]   * tc.z;
	var_TexBump       = i_TexBump[0]       * tc.x + i_TexBump[1]       * tc.y + i_TexBump[2]       * tc.z;
	var_TexFalloff    = i_TexFalloff[0]    * tc.x + i_TexFalloff[1]    * tc.y + i_TexFalloff[2]    * tc.z;
	var_TexProjection = i_TexProjection[0] * tc.x + i_TexProjection[1] * tc.y + i_TexProjection[2] * tc.z;
	var_TexDiffuse    = i_TexDiffuse[0]    * tc.x + i_TexDiffuse[1]    * tc.y + i_TexDiffuse[2]    * tc.z;
	var_TexSpecular   = i_TexSpecular[0]   * tc.x + i_TexSpecular[1]   * tc.y + i_TexSpecular[2]   * tc.z;
	var_TexHalfVec    = i_TexHalfVec[0]    * tc.x + i_TexHalfVec[1]    * tc.y + i_TexHalfVec[2]    * tc.z;
	var_Color         = i_Color[0]         * tc.x + i_Color[1]         * tc.y + i_Color[2]         * tc.z;
	var_TexViewVec    = i_TexViewVec[0]    * tc.x + i_TexViewVec[1]    * tc.y + i_TexViewVec[2]    * tc.z;
	var_ShadowCubeVec = i_ShadowCubeVec[0] * tc.x + i_ShadowCubeVec[1] * tc.y + i_ShadowCubeVec[2] * tc.z;

	// optional normal-map displacement along the interpolated geometric normal
	vec3 geoN = normalize( i_ModelNormal[0] * tc.x + i_ModelNormal[1] * tc.y + i_ModelNormal[2] * tc.z );
	pos = dudeTessDisplace( pos, geoN, u_bumpMap, var_TexBump );

	gl_Position = u_mvpMatrix * vec4( pos, 1.0 );
}
