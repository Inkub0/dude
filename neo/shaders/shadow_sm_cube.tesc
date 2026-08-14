// DUDE tessellation (docs/tessellation.md): cube (point-light) shadow-map caster
// control shader. Identical control net + crack-free factors as shadow_sm.tesc /
// zfill.tesc so the tessellated caster matches the lit body.

#include "renderparms.glsl"
#include "tess.glsl"

layout(vertices = 3) out;

VARY(1) in vec2 i_TexCoord[];
VARY(2) in vec3 i_ModelPos[];
VARY(3) in vec4 i_ModelNormal[];
VARY(4) in vec2 i_TexBump[];

VARY(1) out vec2 o_TexCoord[];
VARY(2) out vec3 o_ModelPos[];
VARY(3) out vec4 o_ModelNormal[];
VARY(4) out vec2 o_TexBump[];

void main() {
	o_TexCoord[gl_InvocationID]    = i_TexCoord[gl_InvocationID];
	o_ModelPos[gl_InvocationID]    = i_ModelPos[gl_InvocationID];
	o_ModelNormal[gl_InvocationID] = i_ModelNormal[gl_InvocationID];
	o_TexBump[gl_InvocationID]     = i_TexBump[gl_InvocationID];

	if ( gl_InvocationID == 0 ) {
		vec3 p0 = i_ModelPos[0], p1 = i_ModelPos[1], p2 = i_ModelPos[2];
		gl_TessLevelOuter[0] = dudeTessEdgeFactor( p1, p2 );
		gl_TessLevelOuter[1] = dudeTessEdgeFactor( p0, p2 );
		gl_TessLevelOuter[2] = dudeTessEdgeFactor( p0, p1 );
		gl_TessLevelInner[0] = max( gl_TessLevelOuter[0],
		                       max( gl_TessLevelOuter[1], gl_TessLevelOuter[2] ) );
	}
}
