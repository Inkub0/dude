// DUDE ReSTIR DI R3a disocclusion boost - the YOUNG test and its inputs (docs/rtx-restir-di.md Plan R3). Included by
// restir_di_young.frag (writes the per-pixel reason) and by the boost variant of the gather (restir_di_boost.frag, which
// reads that reason back clustered). Uniform packing: restir_di.frag header (u_tessParms, u_prevMvpMatrix).
SAMPLER_BINDING(4) uniform sampler2D u_prevDenoised;		// R3a: last frame's denoised DI, .w = NRD history length (frames)
SAMPLER_BINDING(5) uniform sampler2D u_prevViewZ;		// R3a: last frame's gather att1 copy ( viewZ, lum )
SAMPLER_BINDING(6) uniform sampler2D u_prevNormal;		// R3a: last frame's gather att2 copy (NRD-packed normal)
SAMPLER_BINDING(9) uniform sampler2D u_prevSide;		// R3a: last frame's gather att3 copy ( SH1, row + 1 [+ 0.5 boosted] )
SAMPLER_BINDING(10) uniform sampler2D u_velocity;		// R3a: velocity at the gather res


// returns the reason the pixel is young: 0 = not young, 1 = NRD's history short, 2 = no previous pixel is this surface
// (just revealed), 3 = held by the hysteresis / entered the screen. Stored in the row id's fraction (x 0.25) for debug view 7.
int diBoostWanted( vec2 sfrag, in DiSurface S ) {
	const float hOn    = u_tessParms.y;
	const float hOff   = u_tessParms.z;
	const int   prevCb = int( floor( u_tessParms.w + 0.5 ) );				// -1 = no side data
	vec2 uvc = sfrag * u_screenCorrection.xy;
	vec2 vel = texture( u_velocity, uvc ).rg;
	vel.y = ( u_windowCoord.z < 0.0 ) ? -vel.y : vel.y;
	vec2 prevUV = uvc - vel;
	if ( any( lessThan( prevUV, vec2( 0.0 ) ) ) || any( greaterThanEqual( prevUV, vec2( 1.0 ) ) ) ) { return 3; }	// just entered the screen: no history at all
	ivec2 res     = ivec2( floor( 1.0 / u_screenCorrection.xy + 0.5 ) );
	ivec2 prevPix = clamp( ivec2( floor( prevUV * vec2( res ) ) ), ivec2( 0 ), res - 1 );
	// NRD's history length where this pixel was - the nearest texel, never bilinear (a frame count must not blend across an edge)
	float h = texelFetch( u_prevDenoised, prevPix, 0 ).a;
	if ( !( h >= hOn ) ) { return 1; }										// young by NRD's own count (NaN / garbage counts as young)
	if ( prevCb < 0 ) { return 0; }											// no side data: NRD's count is all there is
	// a DIFFERENT surface was here last frame (the RTXDI same-surface test, as the gradient): disoccluded THIS frame.
	// NRD resets this pixel below anyway; boosting now rather than a frame late keeps the first wake sample clean too.
	// Tested against the 2x2 previous pixels the reprojection straddles, like NRD's bilinear reprojection: the jitter
	// lands the reprojection on a NEIGHBOUR as often as not, and against a single pixel every depth / normal edge of a
	// perfectly static view read as "disoccluded" every frame (boost 4 cost +1.5 ms standing still, 2026-10-12). It is
	// a disocclusion only when NO traced tap of the footprint is this surface. Under checkerboard a tap not traced last
	// frame holds nothing (its row sits in the other parity) and is skipped.
	float expectedPrevVz = ( u_prevMvpMatrix * vec4( S.Pw, 1.0 ) ).z;
	ivec2 base = clamp( ivec2( floor( prevUV * vec2( res ) - 0.5 ) ), ivec2( 0 ), res - 2 );
	bool  anyMatch = false, anyBoosted = false;
	for ( int t = 0; t < 4; t++ ) {
		ivec2 pp = base + ivec2( t & 1, t >> 1 );
		ivec2 rp = pp;
		if ( prevCb > 0 ) {
			if ( ( pp.x & 1 ) != ( ( pp.y + prevCb - 1 ) & 1 ) ) { continue; }	// not traced last frame
			rp.x = pp.x >> 1;
		}
		vec2 pvz = texelFetch( u_prevViewZ, rp, 0 ).xy;
		if ( abs( expectedPrevVz - pvz.x ) > 0.1 * max( abs( expectedPrevVz ), 1.0 ) ) { continue; }
		vec3 pN = unpackNrdNormal( texelFetch( u_prevNormal, rp, 0 ).xyz );
		if ( dot( S.Nw, pN ) < 0.5 ) { continue; }
		anyMatch = true;
		// hysteresis: this same-surface tap was boosted last frame (the +0.5 in its stored row id)
		if ( fract( texelFetch( u_prevSide, rp, 0 ).w ) >= 0.2 ) { anyBoosted = true; }
	}
	if ( !anyMatch ) { return 2; }											// nothing of this surface in the footprint: just revealed
	return ( anyBoosted && !( h >= hOff ) ) ? 3 : 0;						// stays boosted until the history reaches the OFF threshold
}

