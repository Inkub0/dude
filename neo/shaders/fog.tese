// DUDE tessellation (docs/tessellation.md): fog pass evaluation shader. Same
// domain / spacing / winding and the SAME PN evaluation + displacement as
// zfill.tese, so the depth this pass rasterizes matches the prepass bit-for-bit
// (gl_Position injected `invariant` by compile_spv.py). Without this, a
// tessellated model's fog is depth-EQUAL-rejected and it reads as a dark
// silhouette in the fog. The fog texgen is recomputed from the displaced
// position (the flat fog.vert computes it from attr_Position the same way).

#include "renderparms.glsl"
#include "tess.glsl"

// bump/normal map for displacement — bound on unit 2 by the fog pass (units 0/1
// are the fog distance ramp + enter fade). Same image + texel zfill.tese samples,
// so the displacement is identical.
SAMPLER_BINDING(2) uniform sampler2D u_bumpMap;

layout(triangles, fractional_odd_spacing, cw) in;

VARY(2) in vec3 i_ModelPos[];
VARY(3) in vec4 i_ModelNormal[];
VARY(4) in vec2 i_TexBump[];

VARY(0) out vec2 var_TexFog;
VARY(1) out vec2 var_TexFogEnter;

void main() {
	vec3 tc = gl_TessCoord;

	// PN wants unit corner normals; .w is the UV-seam displacement mask (tess.glsl)
	vec3 n0 = normalize( i_ModelNormal[0].xyz );
	vec3 n1 = normalize( i_ModelNormal[1].xyz );
	vec3 n2 = normalize( i_ModelNormal[2].xyz );

	vec3 pos = dudeTessPN( i_ModelPos[0], i_ModelPos[1], i_ModelPos[2], n0, n1, n2, tc );

	// must match zfill.tese exactly (depth-EQUAL): same displacement along the same
	// interpolated geometric normal with the same bump texel.
	vec2 texBump = i_TexBump[0] * tc.x + i_TexBump[1] * tc.y + i_TexBump[2] * tc.z;
	vec3 geoN = normalize( n0 * tc.x + n1 * tc.y + n2 * tc.z );
	float seam = i_ModelNormal[0].w * tc.x + i_ModelNormal[1].w * tc.y + i_ModelNormal[2].w * tc.z;
	pos = dudeTessDisplace( pos, geoN, u_bumpMap, texBump, seam );

	// fog texgen from the displaced model-space position (the flat fog.vert
	// computes the same dot products from attr_Position)
	vec4 p = vec4( pos, 1.0 );
	var_TexFog      = vec2( dot( p, u_texGen0S ), dot( p, u_texGen0T ) );
	var_TexFogEnter = vec2( dot( p, u_texGen1S ), dot( p, u_texGen1T ) );

	gl_Position = u_mvpMatrix * p;
}
