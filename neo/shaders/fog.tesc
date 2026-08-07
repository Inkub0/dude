// DUDE tessellation (docs/tessellation.md): fog pass control shader. Identical
// factor logic to zfill.tesc / interaction.tesc so the fog interaction pass
// subdivides each patch the same way the depth prepass did (depth-EQUAL
// constraint) — otherwise a tessellated model's fog fails the depth test and it
// renders as a dark, un-fogged silhouette.

#include "renderparms.glsl"
#include "tess.glsl"

layout(vertices = 3) out;

VARY(2) in vec3 i_ModelPos[];
VARY(3) in vec3 i_ModelNormal[];
VARY(4) in vec2 i_TexBump[];

VARY(2) out vec3 o_ModelPos[];
VARY(3) out vec3 o_ModelNormal[];
VARY(4) out vec2 o_TexBump[];

void main() {
	// per-vertex output writes must be indexed by the literal gl_InvocationID
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
