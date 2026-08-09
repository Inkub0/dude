// DUDE tessellation (docs/tessellation.md): cube (point-light) shadow-map caster
// evaluation shader. Same dudeTessPN + dudeTessDisplace as zfill.tese, then the
// light-relative vector is rebuilt from the DISPLACED position (the flat
// shadow_sm_cube.vert builds it from attr_Position). var_LightVec stays the VECTOR
// (fragment takes length()) so per-fragment radial depth is exact — tessellation only
// makes the linear interpolation finer.

#include "renderparms.glsl"
#include "tess.glsl"

// displacement source, bound on unit 1 (unit 0 is the perforated-coverage map).
SAMPLER_BINDING(1) uniform sampler2D u_bumpMap;

layout(triangles, fractional_odd_spacing, cw) in;

VARY(1) in vec2 i_TexCoord[];
VARY(2) in vec3 i_ModelPos[];
VARY(3) in vec4 i_ModelNormal[];
VARY(4) in vec2 i_TexBump[];

VARY(0) out vec3 var_LightVec;
VARY(1) out vec2 var_TexCoord;

void main() {
	vec3 tc = gl_TessCoord;

	// PN wants unit corner normals; .w is the UV-seam displacement mask (tess.glsl)
	vec3 n0 = normalize( i_ModelNormal[0].xyz );
	vec3 n1 = normalize( i_ModelNormal[1].xyz );
	vec3 n2 = normalize( i_ModelNormal[2].xyz );

	vec3 pos = dudeTessPN( i_ModelPos[0], i_ModelPos[1], i_ModelPos[2], n0, n1, n2, tc );
	vec2 texBump = i_TexBump[0] * tc.x + i_TexBump[1] * tc.y + i_TexBump[2] * tc.z;
	vec3 geoN = normalize( n0 * tc.x + n1 * tc.y + n2 * tc.z );
	float seam = i_ModelNormal[0].w * tc.x + i_ModelNormal[1].w * tc.y + i_ModelNormal[2].w * tc.z;
	pos = dudeTessDisplace( pos, geoN, u_bumpMap, texBump, seam );

	// light-relative vector from the displaced position (model->world rotation)
	vec3 d = pos - u_localLightOrigin.xyz;
	vec3 lr = vec3( dot( u_modelMatrixRow0.xyz, d ),
	                dot( u_modelMatrixRow1.xyz, d ),
	                dot( u_modelMatrixRow2.xyz, d ) );
	var_LightVec = lr;

	var_TexCoord = i_TexCoord[0] * tc.x + i_TexCoord[1] * tc.y + i_TexCoord[2] * tc.z;

	gl_Position = u_mvpMatrix * vec4( lr, 1.0 );
}
