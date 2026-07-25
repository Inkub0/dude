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

VARY(0) in float var_Dist;
VARY(1) in vec2 var_TexCoord;

void main() {
	if ( u_alphaTest.y != 0.0 && texture( u_map, var_TexCoord ).a < u_alphaTest.x ) {
		discard;
	}
	gl_FragDepth = clamp( var_Dist, 0.0, 1.0 );
}
