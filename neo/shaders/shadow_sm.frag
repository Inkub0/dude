// DUDE Phase 3.5 shadow-map caster pass (fragment). Stores the light's linear
// falloff distance as depth so interaction.frag can compare against it directly.
// The target is depth-only (drawbuffer NONE), so no color output is written.
//
// Alpha-tested (perforated) casters — grates/fences/foliage — will sample the
// coverage texture here and discard below the alpha threshold (guardrail #2);
// milestone 1 is opaque-only, so that hook is not wired yet.

#include "renderparms.glsl"

VARY(0) in float var_Falloff;

void main() {
	gl_FragDepth = clamp( var_Falloff, 0.0, 1.0 );
}
