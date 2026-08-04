// DUDE tessellation (docs/tessellation.md): zfill (depth prepass) evaluation
// shader. Same domain / spacing / winding and same PN evaluation as
// interaction.tese, so the depth it writes matches the interaction pass bit-for-
// bit (gl_Position injected `invariant` by compile_spv.py).

#include "renderparms.glsl"
#include "tess.glsl"

layout(triangles, equal_spacing, cw) in;

VARY(0) in vec2 i_TexCoord[];
VARY(1) in vec3 i_ModelPos[];
VARY(2) in vec3 i_ModelNormal[];

VARY(0) out vec2 var_TexCoord;

void main() {
	vec3 tc = gl_TessCoord;

	vec3 pos = dudeTessPN( i_ModelPos[0], i_ModelPos[1], i_ModelPos[2],
	                       normalize( i_ModelNormal[0] ),
	                       normalize( i_ModelNormal[1] ),
	                       normalize( i_ModelNormal[2] ), tc );

	var_TexCoord = i_TexCoord[0] * tc.x + i_TexCoord[1] * tc.y + i_TexCoord[2] * tc.z;

	// subview near-clip in model-local space, recomputed from the PN position
	gl_ClipDistance[0] = dot( vec4( pos, 1.0 ), u_clipPlane );
	gl_Position = u_mvpMatrix * vec4( pos, 1.0 );
}
