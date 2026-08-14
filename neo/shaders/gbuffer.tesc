// DUDE tessellation (docs/tessellation.md): SSAO normal G-buffer control shader.
// Identical factor logic to zfill.tesc / interaction.tesc so the normal prepass
// subdivides each patch the same way the depth/lit passes do — SSAO then evaluates
// occlusion against the SAME rounded surface that ends up on screen.

#include "renderparms.glsl"
#include "tess.glsl"

layout(vertices = 3) out;

VARY(0) in vec2 i_TexBump[];
VARY(1) in vec3 i_T[];
VARY(2) in vec3 i_B[];
VARY(3) in vec3 i_N[];
VARY(4) in vec2 i_TexCoverage[];
VARY(5) in vec3 i_ModelPos[];
VARY(6) in vec4 i_ModelNormal[];

VARY(0) out vec2 o_TexBump[];
VARY(1) out vec3 o_T[];
VARY(2) out vec3 o_B[];
VARY(3) out vec3 o_N[];
VARY(4) out vec2 o_TexCoverage[];
VARY(5) out vec3 o_ModelPos[];
VARY(6) out vec4 o_ModelNormal[];

void main() {
	// per-vertex output writes must be indexed by the literal gl_InvocationID
	o_TexBump[gl_InvocationID]     = i_TexBump[gl_InvocationID];
	o_T[gl_InvocationID]           = i_T[gl_InvocationID];
	o_B[gl_InvocationID]           = i_B[gl_InvocationID];
	o_N[gl_InvocationID]           = i_N[gl_InvocationID];
	o_TexCoverage[gl_InvocationID] = i_TexCoverage[gl_InvocationID];
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
