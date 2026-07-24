// DUDE Phase 3.5 shadow-map caster pass (fragment). Stores the light's linear
// falloff distance as depth so interaction.frag can compare against it directly.
// The target is depth-only (drawbuffer NONE), so no color output is written.
//
// Alpha-tested (perforated) casters — grates/fences/foliage — sample the coverage
// texture and discard below the alpha threshold, so their shadow is punched out
// exactly like the visible surface. Opaque casters bind whiteImage with the alpha
// test disabled (u_alphaTest.y == 0), so the discard never fires for them.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_map;

VARY(0) in float var_Falloff;
VARY(1) in vec2 var_TexCoord;

void main() {
	if ( u_alphaTest.y != 0.0 && texture( u_map, var_TexCoord ).a < u_alphaTest.x ) {
		discard;
	}
	gl_FragDepth = clamp( var_Falloff, 0.0, 1.0 );
}
