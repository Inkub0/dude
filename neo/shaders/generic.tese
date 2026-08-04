// DUDE tessellation (docs/tessellation.md): generic (blended material / decal)
// evaluation shader. Same PN evaluation, domain and winding as interaction.tese so
// a blood-overlay decal — which shares the monster's vertex positions and normals —
// PN-follows the deformed base and stops clipping through it. No displacement: blood
// decals carry no bump map, so they ride the PN silhouette but not the (small) normal-
// map push; a decal drawn with polygon offset tolerates that residual.

#include "renderparms.glsl"
#include "tess.glsl"

layout(triangles, fractional_odd_spacing, cw) in;	// fractional = smooth LOD:
										// new verts slide in as the distance factor changes instead of popping.
										// Must match in every pass (crack-free + depth-EQUAL).

VARY(0) in vec2 i_TexCoord[];
VARY(1) in vec4 i_Color[];
VARY(2) in vec3 i_ModelPos[];
VARY(3) in vec3 i_ModelNormal[];

VARY(0) out vec2 var_TexCoord;
VARY(1) out vec4 var_Color;

void main() {
	vec3 tc = gl_TessCoord;

	vec3 pos = dudeTessPN( i_ModelPos[0], i_ModelPos[1], i_ModelPos[2],
	                       normalize( i_ModelNormal[0] ),
	                       normalize( i_ModelNormal[1] ),
	                       normalize( i_ModelNormal[2] ), tc );

	var_TexCoord = i_TexCoord[0] * tc.x + i_TexCoord[1] * tc.y + i_TexCoord[2] * tc.z;
	var_Color    = i_Color[0]    * tc.x + i_Color[1]    * tc.y + i_Color[2]    * tc.z;

	gl_Position = u_mvpMatrix * vec4( pos, 1.0 );
}
