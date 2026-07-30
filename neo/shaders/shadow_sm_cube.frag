// DUDE Phase 3.5 point-light shadow-map caster pass (fragment). Stores the light's
// linear radial distance as depth so interaction.frag can compare against it when
// sampling the cube. Depth target only (drawbuffer NONE), so no color is written.
//
// Alpha-tested (perforated) casters — grates/fences/foliage — sample the coverage
// texture and discard below the alpha threshold, so their shadow is punched out
// exactly like the visible surface (same as the 2D caster shadow_sm.frag). Opaque
// casters bind whiteImage with the test disabled (u_alphaTest.y == 0).

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_map;

VARY(0) in vec3 var_LightVec;
VARY(1) in vec2 var_TexCoord;

void main() {
	if ( u_alphaTest.y != 0.0 && texture( u_map, var_TexCoord ).a < u_alphaTest.x ) {
		discard;
	}
	// Perforated shadow strength (u_alphaTest.z, r_shadowMapPerforatedStrength):
	// discard an ordered-Bayer fraction of this caster's map texels; the receiver's
	// PCF averages hit and hole texels, lightening the shadow toward `strength`
	// instead of full dark. Opaque casters (u_alphaTest.y == 0) are never dithered.
	if ( u_alphaTest.y != 0.0 && u_alphaTest.z < 1.0 ) {
		const float bayer[16] = float[16]( 0.0,  8.0,  2.0, 10.0,
		                                  12.0,  4.0, 14.0,  6.0,
		                                   3.0, 11.0,  1.0,  9.0,
		                                  15.0,  7.0, 13.0,  5.0 );
		ivec2 t = ivec2( gl_FragCoord.xy ) & 3;
		if ( ( bayer[t.y * 4 + t.x] + 0.5 ) / 16.0 > u_alphaTest.z ) {
			discard;
		}
	}
	// per-fragment radial distance: length() must happen HERE, not in the vertex
	// shader (see shadow_sm_cube.vert — interpolated lengths overestimate inside
	// big triangles near the light). Normalizer matches interaction.frag's ref.
	gl_FragDepth = clamp( length( var_LightVec ) / max( u_shadowParms.w, 1.0 ), 0.0, 1.0 );
}
