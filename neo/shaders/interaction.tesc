// DUDE tessellation (docs/tessellation.md): interaction control shader.
// Passes the interaction.vert varyings through unchanged and sets crack-free
// per-edge tessellation factors from the shared model-space positions.

#include "renderparms.glsl"
#include "tess.glsl"

layout(vertices = 3) out;

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
VARY(11) in vec4 i_ModelNormal[];
VARY(12) in vec4 i_ShadowProjection[];

VARY(0)  out vec3 o_TexLightVec[];
VARY(1)  out vec2 o_TexBump[];
VARY(2)  out vec2 o_TexFalloff[];
VARY(3)  out vec4 o_TexProjection[];
VARY(4)  out vec2 o_TexDiffuse[];
VARY(5)  out vec2 o_TexSpecular[];
VARY(6)  out vec3 o_TexHalfVec[];
VARY(7)  out vec4 o_Color[];
VARY(8)  out vec3 o_TexViewVec[];
VARY(9)  out vec3 o_ShadowCubeVec[];
VARY(10) out vec3 o_ModelPos[];
VARY(11) out vec4 o_ModelNormal[];
VARY(12) out vec4 o_ShadowProjection[];

void main() {
	// glslang requires per-vertex output writes to be indexed by the literal
	// gl_InvocationID (not a copied local)
	o_TexLightVec[gl_InvocationID]   = i_TexLightVec[gl_InvocationID];
	o_TexBump[gl_InvocationID]       = i_TexBump[gl_InvocationID];
	o_TexFalloff[gl_InvocationID]    = i_TexFalloff[gl_InvocationID];
	o_TexProjection[gl_InvocationID] = i_TexProjection[gl_InvocationID];
	o_TexDiffuse[gl_InvocationID]    = i_TexDiffuse[gl_InvocationID];
	o_TexSpecular[gl_InvocationID]   = i_TexSpecular[gl_InvocationID];
	o_TexHalfVec[gl_InvocationID]    = i_TexHalfVec[gl_InvocationID];
	o_Color[gl_InvocationID]         = i_Color[gl_InvocationID];
	o_TexViewVec[gl_InvocationID]    = i_TexViewVec[gl_InvocationID];
	o_ShadowCubeVec[gl_InvocationID] = i_ShadowCubeVec[gl_InvocationID];
	o_ModelPos[gl_InvocationID]      = i_ModelPos[gl_InvocationID];
	o_ModelNormal[gl_InvocationID]   = i_ModelNormal[gl_InvocationID];
	o_ShadowProjection[gl_InvocationID] = i_ShadowProjection[gl_InvocationID];

	if ( gl_InvocationID == 0 ) {
		vec3 p0 = i_ModelPos[0], p1 = i_ModelPos[1], p2 = i_ModelPos[2];
		// outer[i] is the edge opposite corner i (tc component i == 0)
		gl_TessLevelOuter[0] = dudeTessEdgeFactor( p1, p2 );
		gl_TessLevelOuter[1] = dudeTessEdgeFactor( p0, p2 );
		gl_TessLevelOuter[2] = dudeTessEdgeFactor( p0, p1 );
		gl_TessLevelInner[0] = max( gl_TessLevelOuter[0],
		                       max( gl_TessLevelOuter[1], gl_TessLevelOuter[2] ) );
	}
}
