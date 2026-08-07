// DUDE berserk vision (RHI backends): reproduces the stock "streak zoom". The ARB
// effect recursively re-samples the previous frame scaled ~3% each frame, so you see
// a handful of DISCRETE ghost copies that fade out (the HUD "100" trails as ~4 numbers,
// not a smooth smear). This does the same in one pass: a few discrete zoom taps toward
// centre with a geometric fade. u_localParam0.x = edge reach, .y = sharp-centre radius,
// .z = ghost-tap count.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_map;

VARY(0) in vec2 var_TexCoord;
VARY(1) in vec4 var_Color;

layout(location = 0) out vec4 fragColor;

const int MAX_TAPS = 12;

void main() {
	const vec2 center = vec2( 0.5, 0.5 );
	vec2 dir = var_TexCoord - center;

	// Radial focus mask: keep a central area sharp, ramp the ghosts up toward the edges.
	float r = length( dir ) / 0.5;					// 0 centre -> 1 edge -> ~1.41 corner
	float mask = smoothstep( u_localParam0.y, 1.0, r );
	float zoom = u_localParam0.x * mask;			// per-pixel: 0 at centre, full at edges

	int taps = int( u_localParam0.z );
	taps = clamp( taps, 2, MAX_TAPS );

	vec4 sum = vec4( 0.0 );
	float total = 0.0;
	float w = 1.0;
	for ( int i = 0; i < MAX_TAPS; i++ ) {
		if ( i >= taps ) {
			break;
		}
		float t = float( i ) / float( taps - 1 );	// 0 .. 1 across the ghosts
		float scale = 1.0 - zoom * t;				// each ghost steps further toward centre
		sum += texture( u_map, center + dir * scale ) * w;
		total += w;
		w *= 0.65;									// per-frame decay of the ARB feedback
	}

	fragColor = ( sum / total ) * var_Color;
}
