// DUDE tessellation (docs/tessellation.md): ambient-light control shader.
// Same crack-free factor logic as interaction.tesc; passes the ambientlight
// varyings through unchanged.

#include "renderparms.glsl"
#include "tess.glsl"

layout(vertices = 3) out;

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

VARY(0) out vec2 o_TexBump[];
VARY(1) out vec2 o_TexDiffuse[];
VARY(2) out vec2 o_TexFalloff[];
VARY(3) out vec4 o_TexProjection[];
VARY(4) out vec3 o_ToGlobalRow0[];
VARY(5) out vec3 o_ToGlobalRow1[];
VARY(6) out vec3 o_ToGlobalRow2[];
VARY(7) out vec4 o_Color[];
VARY(8) out vec3 o_ModelPos[];
VARY(9) out vec3 o_ModelNormal[];

void main() {
	o_TexBump[gl_InvocationID]       = i_TexBump[gl_InvocationID];
	o_TexDiffuse[gl_InvocationID]    = i_TexDiffuse[gl_InvocationID];
	o_TexFalloff[gl_InvocationID]    = i_TexFalloff[gl_InvocationID];
	o_TexProjection[gl_InvocationID] = i_TexProjection[gl_InvocationID];
	o_ToGlobalRow0[gl_InvocationID]  = i_ToGlobalRow0[gl_InvocationID];
	o_ToGlobalRow1[gl_InvocationID]  = i_ToGlobalRow1[gl_InvocationID];
	o_ToGlobalRow2[gl_InvocationID]  = i_ToGlobalRow2[gl_InvocationID];
	o_Color[gl_InvocationID]         = i_Color[gl_InvocationID];
	o_ModelPos[gl_InvocationID]      = i_ModelPos[gl_InvocationID];
	o_ModelNormal[gl_InvocationID]   = i_ModelNormal[gl_InvocationID];

	if ( gl_InvocationID == 0 ) {
		vec3 p0 = i_ModelPos[0], p1 = i_ModelPos[1], p2 = i_ModelPos[2];
		gl_TessLevelOuter[0] = dudeTessEdgeFactor( p1, p2 );
		gl_TessLevelOuter[1] = dudeTessEdgeFactor( p0, p2 );
		gl_TessLevelOuter[2] = dudeTessEdgeFactor( p0, p1 );
		gl_TessLevelInner[0] = max( gl_TessLevelOuter[0],
		                       max( gl_TessLevelOuter[1], gl_TessLevelOuter[2] ) );
	}
}
