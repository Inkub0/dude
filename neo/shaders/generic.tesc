// DUDE tessellation (docs/tessellation.md): generic (blended material / decal)
// control shader. Same crack-free factor logic as interaction.tesc; passes the
// generic varyings through. Lets blood-overlay decals PN-follow the tessellated
// monster they sit on.

#include "renderparms.glsl"
#include "tess.glsl"

layout(vertices = 3) out;

VARY(0) in vec2 i_TexCoord[];
VARY(1) in vec4 i_Color[];
VARY(2) in vec3 i_ModelPos[];
VARY(3) in vec3 i_ModelNormal[];

VARY(0) out vec2 o_TexCoord[];
VARY(1) out vec4 o_Color[];
VARY(2) out vec3 o_ModelPos[];
VARY(3) out vec3 o_ModelNormal[];

void main() {
	o_TexCoord[gl_InvocationID]    = i_TexCoord[gl_InvocationID];
	o_Color[gl_InvocationID]       = i_Color[gl_InvocationID];
	o_ModelPos[gl_InvocationID]    = i_ModelPos[gl_InvocationID];
	o_ModelNormal[gl_InvocationID] = i_ModelNormal[gl_InvocationID];

	if ( gl_InvocationID == 0 ) {
		vec3 p0 = i_ModelPos[0], p1 = i_ModelPos[1], p2 = i_ModelPos[2];
		gl_TessLevelOuter[0] = dudeTessEdgeFactor( p1, p2 );
		gl_TessLevelOuter[1] = dudeTessEdgeFactor( p0, p2 );
		gl_TessLevelOuter[2] = dudeTessEdgeFactor( p0, p1 );
		gl_TessLevelInner[0] = max( gl_TessLevelOuter[0],
		                       max( gl_TessLevelOuter[1], gl_TessLevelOuter[2] ) );
	}
}
