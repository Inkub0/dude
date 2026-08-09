// DUDE tessellation (docs/tessellation.md): 2D shadow-map caster evaluation shader.
// Runs the SAME dudeTessPN + dudeTessDisplace as zfill.tese, so the occluder surface
// this pass rasterizes into the shadow map coincides with the lit (displaced) surface
// the interaction pass draws — the cast shadow then hugs the smoothed body instead of
// the low-poly silhouette. The light-projective coords + falloff depth are recomputed
// from the DISPLACED position (the flat shadow_sm.vert computes them from attr_Position).

#include "renderparms.glsl"
#include "tess.glsl"

// displacement source, bound on unit 1 (unit 0 is the perforated-coverage map).
SAMPLER_BINDING(1) uniform sampler2D u_bumpMap;

layout(triangles, fractional_odd_spacing, cw) in;

VARY(1) in vec2 i_TexCoord[];
VARY(2) in vec3 i_ModelPos[];
VARY(3) in vec4 i_ModelNormal[];
VARY(4) in vec2 i_TexBump[];

VARY(0) out float var_Falloff;
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

	// light-projective coords + falloff depth from the displaced position
	vec4 p = vec4( pos, 1.0 );
	float s = dot( p, u_lightProjectionS );
	float t = dot( p, u_lightProjectionT );
	float q = dot( p, u_lightProjectionQ );
	var_Falloff = dot( p, u_lightFalloffS );

	var_TexCoord = i_TexCoord[0] * tc.x + i_TexCoord[1] * tc.y + i_TexCoord[2] * tc.z;

	gl_Position = vec4( 2.0 * s - q, 2.0 * t - q, 0.0, q );
}
