// DUDE ReSTIR DI R1 - gradient A-TROUS filter (docs/rtx-restir-di.md Plan R1; RTXDI FilterGradientsPass): one of
// 4 passes over the ( |dL|, max L ) gradient at the stratum resolution, 3x3 triangular kernel (1 centre, 0.5 edge,
// 0.25 corner), stride 1 / 2 / 4 / 8 (31 px reach in gradient space). The difference and the magnitude are filtered
// with the same weights but SEPARATELY, so a small local change in a bright region cannot invalidate history
// ("a small local change in a bright region is unlikely to result in history invalidation").
//   unit 0          = the gradient (RGBA16F, gw x gh): .xy as above, .z = the EXACT verdict's relative change
//   u_localParam0   = ( stride, gw, gh, dilate ) - dilate (first pass only): .z becomes the MAX of the 3x3 taps, a one-
//                     stratum dilation (A-SVGF), and later passes carry it unchanged; averaging it would dilute a thin
//                     changed band or a small shadow ~9x per pass (the trailing edge of an NPC's / a sentry's shadow)

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_src;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	ivec2 p    = ivec2( gl_FragCoord.xy );
	int   step = int( u_localParam0.x + 0.5 );
	ivec2 hi   = ivec2( int( u_localParam0.y + 0.5 ), int( u_localParam0.z + 0.5 ) ) - 1;
	vec2  sum  = vec2( 0.0 );
	float wsum = 0.0;
	const bool dilate = u_localParam0.w > 0.5;
	float zc   = texelFetch( u_src, clamp( p, ivec2( 0 ), hi ), 0 ).z;
	float zmax = zc;
	for ( int y = -1; y <= 1; y++ ) {
		for ( int x = -1; x <= 1; x++ ) {
			ivec2 q = clamp( p + ivec2( x, y ) * step, ivec2( 0 ), hi );
			float w = ( x == 0 ? 1.0 : 0.5 ) * ( y == 0 ? 1.0 : 0.5 );
			vec4 t = texelFetch( u_src, q, 0 );
			sum  += t.xy * w;
			wsum += w;
			zmax  = max( zmax, t.z );
		}
	}
	fragColor = vec4( sum / wsum, dilate ? zmax : zc, 1.0 );
}
