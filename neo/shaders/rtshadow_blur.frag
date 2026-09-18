// DUDE RT shadow blur (r_rtShadowBlur, docs/rtx-shadow-blur.md): turns one light's hard
// ray-traced visibility into a contact-hardening penumbra with a separable, variable-width,
// depth-aware Gaussian - screen-space PCSS (MohammadBagher et al. 2010) fed by the ray's exact
// occluder distance. Nothing here is random and nothing is carried between frames: the same
// camera gives the same mask.
//
// Runs twice per light: horizontally (ray target -> ping), then vertically (ping -> ray target).
//
// Input texel (rtshadow_ray.frag):
//   R = visibility (pass 1: binary; pass 2: horizontally blurred)
//   G = penumbra half-width in pixels ALONG THIS PASS'S AXIS, 0 = nothing to spread
//   B = view distance d (0 = never traced: ignored)
//   A = pass 1 only: the half-width along the OTHER axis, forwarded to pass 2 in G
//
// Per pixel: the tile early-out first (rtshadow_tiles.frag - no shadow edge within reach = pass
// the texel through; it also bounds the sweep to the widest shadow that reaches). Otherwise (1) which shadowed neighbours REACH me - a neighbour x pixels away
// whose own half-width is >= x - and their mean half-width; none = unchanged. (2) Gaussian of that
// width (sigma = half-width / 2) over the visibility. A contact shadow (tiny half-width) reaches
// nobody, so it stays as sharp as the hard ray left it, right next to a wide soft one.
//
// Depth guide: on a plane 1/d is LINEAR in screen space, so the taps are compared against the
// centre's 1/d extrapolated along the axis with the local slope - floors at grazing angles keep
// their whole kernel, while anything off that plane (another object, a silhouette) drops out.
//
//   u_localParam0.xy = pass axis, (1,0) or (0,1)
#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_src;
SAMPLER_BINDING(1) uniform sampler2D u_tiles;		// per 8x8 tile: R = 1 a shadow edge is within reach, G = that reach / 16

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

const int   K         = 16;		// the widest penumbra half-width, in pixels (= HW_MAX in rtshadow_tiles.frag)
const float DEPTH_TOL = 0.02;	// relative 1/d mismatch where a tap's weight falls to 1/e (5% = gone)

float depthWeight( float iz, float expected, float izc ) {
	float e = abs( iz - expected ) / ( izc * DEPTH_TOL );
	return exp( -e * e );
}

vec4 fetchTap( ivec2 q, ivec2 hi ) {
	return ( q.x < 0 || q.y < 0 || q.x > hi.x || q.y > hi.y ) ? vec4( 1.0, 0.0, 0.0, 0.0 ) : texelFetch( u_src, q, 0 );
}

void main() {
	ivec2 ip = ivec2( gl_FragCoord.xy );
	vec4  c  = texelFetch( u_src, ip, 0 );
	// pass-through: visibility as is, the other axis' half-width moves into G for pass 2
	fragColor = vec4( c.r, c.a, c.b, 0.0 );
	if ( !( c.b > 0.0 ) ) {
		return;
	}
	vec4 tile = texelFetch( u_tiles, ip >> 3, 0 );
	if ( tile.r < 0.5 ) {
		return;									// no shadow edge can reach this tile
	}
	// sweep only as far as the widest shadow that reaches this tile (+1: the reach test's soft
	// edge). Most penumbrae are a few pixels wide, so this is usually a handful of taps, not 33.
	// Taps are fetched in each loop rather than kept in a local array: 33 vec4s would spill out of
	// registers into slow local memory, and the second fetch is a texture-cache hit.
	int   Ke   = min( int( ceil( tile.g * float( K ) ) ) + 1, K );
	ivec2 hi   = textureSize( u_src, 0 ) - 1;
	ivec2 axis = ivec2( u_localParam0.xy + 0.5 );
	float izc  = 1.0 / c.b;

	// local 1/d slope along the axis: the flatter of the two one-sided differences, so a
	// silhouette on one side doesn't tilt the plane
	float dA = fetchTap( ip + axis, hi ).b, dB = fetchTap( ip - axis, hi ).b;
	float sA = ( dA > 0.0 ) ? ( 1.0 / dA - izc ) : 1e9;
	float sB = ( dB > 0.0 ) ? ( izc - 1.0 / dB ) : 1e9;
	float slope = ( abs( sA ) < abs( sB ) ) ? sA : sB;
	if ( abs( slope ) > 0.5 * izc ) { slope = 0.0; }		// silhouettes on both sides

	// (1) the shadowed neighbours that reach this pixel, and their mean half-width
	float rSum = 0.0, rW = 0.0, oSum = 0.0;
	for ( int i = -Ke; i <= Ke; i++ ) {
		vec4 t = fetchTap( ip + axis * i, hi );
		if ( !( t.g > 0.0 ) || !( t.b > 0.0 ) ) {
			continue;
		}
		float x = float( i );
		float reach = clamp( t.g - abs( x ) + 1.0, 0.0, 1.0 );
		if ( reach <= 0.0 ) {
			continue;
		}
		float w = reach * ( 1.0 - t.r + 0.02 ) * depthWeight( 1.0 / t.b, izc + slope * x, izc );
		rSum += w * t.g;	rW += w;	oSum += w * t.a;
	}
	if ( !( rW > 1e-4 ) ) {
		return;									// no shadow reaches this pixel along this axis
	}
	float radius = min( rSum / rW, float( K ) );
	fragColor.g = oSum / rW;					// pass 2's half-width, spread along pass 1's axis
	if ( radius < 0.35 ) {
		return;									// narrower than a pixel: leave the hard edge alone
	}

	// (2) Gaussian over the visibility, sigma = half-width / 2
	float sigma = max( radius * 0.5, 0.5 );
	float inv2s = 1.0 / ( 2.0 * sigma * sigma );
	int   n = min( int( ceil( radius ) ), Ke );
	float vSum = 0.0, vW = 0.0;
	for ( int i = -n; i <= n; i++ ) {
		vec4 t = fetchTap( ip + axis * i, hi );
		if ( !( t.b > 0.0 ) ) {
			continue;
		}
		float x = float( i );
		float w = exp( -x * x * inv2s ) * depthWeight( 1.0 / t.b, izc + slope * x, izc );
		vSum += w * t.r;	vW += w;
	}
	if ( vW > 1e-4 ) {
		fragColor.r = vSum / vW;
	}
}
