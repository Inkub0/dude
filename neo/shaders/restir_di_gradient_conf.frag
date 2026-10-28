// DUDE ReSTIR DI R1 - gradient -> CONFIDENCE (docs/rtx-restir-di.md Plan R1; RTXDI ConfidencePass + the Remix /
// NRD rules). Runs at the gather's internal resolution (NRD IN_DIFF_CONFIDENCE, R16F): the filtered gradient is
// sampled bilinearly from the stratum res, normalized to a relative change, sharpened by a power, smoothed in time
// ONLY while it RISES (Remix: drops apply at once so a change is never ghosted, a recovery is held a few frames so
// ReSTIR has time to stabilize), floored (NRD: under ReBLUR a confidence below historyFix / history TRIGGERS the
// history-fix reconstruction - the 14 px-stride wash - so the floor is at least that ratio; the caller computes it)
// and dithered by 0.5 / maxAccumulatedFrameNum (NRD) so the quantized history length does not band.
//   unit 0 = filtered gradient ( |dL|, max L, exact relative change - max-dilated ), gw x gh, bilinear
//   unit 1 = last frame's confidence (RG16F, rw x rh): .x = what NRD got, .y = the held UN-squared value (the hold's state)
//   unit 2 = velocity at the internal res
//   u_localParam0 = ( power, rise blend 1/(1+h), floor, darkness bias )
//   u_localParam1 = ( dither amplitude, prev confidence valid, dead zone, square for ReLAX )
//                   R2: ReLAX applies confidence "here and now" (historyLength = min( historyLength, max * confidence )) and
//                   "expects smaller values" - the NRD sample squares it for RELAX; the ReBLUR floor rule is skipped by the caller
//                   dead zone: relative changes below this fraction count as no change - Doom's authored flicker tables
//                   pulse many lights by 5-15 % every frame, and a per-light comparison is noise-free, so this is a real
//                   threshold, not a noise guard. A light switching OFF is a 100 % change of ITS OWN value regardless.
//   u_windowCoord.z = view-Y sign (-1 Vulkan top-down: flip the velocity row)

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_gradient;
SAMPLER_BINDING(1) uniform sampler2D u_prevConf;
SAMPLER_BINDING(2) uniform sampler2D u_velocity;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

float ign( vec2 p ) { return fract( 52.9829189 * fract( dot( p, vec2( 0.06711056, 0.00583715 ) ) ) ); }

void main() {
	vec2  uv = var_TexCoord;
	vec3  g  = texture( u_gradient, uv ).xyz;
	// .z = the EXACT verdicts' relative change, max-dilated one stratum (restir_di_gradient_filter.frag): a sharp change
	// the averaging filter would have diluted (a thin trailing shadow band, a small occluder's shadow) still counts
	float rel = max( g.x / ( g.y + u_localParam0.w ), g.z );
	rel = max( rel - u_localParam1.z, 0.0 ) / max( 1.0 - u_localParam1.z, 1e-3 );
	float c  = clamp( 1.0 - rel, 0.0, 1.0 );
	c = pow( c, u_localParam0.x );
	if ( u_localParam1.y > 0.5 ) {
		vec2 vel = texture( u_velocity, uv ).rg;
		vel.y = ( u_windowCoord.z < 0.0 ) ? -vel.y : vel.y;
		vec2 prevUV = uv - vel;
		if ( all( greaterThanEqual( prevUV, vec2( 0.0 ) ) ) && all( lessThanEqual( prevUV, vec2( 1.0 ) ) ) ) {
			// the hold's state is last frame's UN-squared, unfloored, undithered value (.y). Blending with the stored NRD value
			// (.x, already squared) squared it again every frame: with a hold of 2+ frames 1.0 became an unstable fixed point and
			// the whole screen sank to ~0.1-0.25 confidence in the dither's diagonal pattern (user, r_rtReStirDIGradientHistory 3)
			float p = texture( u_prevConf, prevUV ).y;
			if ( c > p ) { c = mix( p, c, u_localParam0.y ); }		// rise-only smoothing (Remix)
		}
	}
	const float held = c;										// next frame's hold state
	if ( u_localParam1.w > 0.5 ) { c *= c; }
	c = max( c, u_localParam0.z );
	c += ( ign( gl_FragCoord.xy ) - 0.5 ) * u_localParam1.x;
	fragColor = vec4( clamp( c, 0.0, 1.0 ), held, 0.0, 1.0 );
}
