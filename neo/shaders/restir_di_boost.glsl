// DUDE ReSTIR DI R3a disocclusion boost - the YOUNG test and its inputs (docs/rtx-restir-di.md Plan R3). Included by
// restir_di_young.frag (writes the per-pixel reason) and by the boost variant of the gather (restir_di_boost.frag, which
// reads that reason back clustered). Uniform packing: restir_di.frag header (u_tessParms, u_prevMvpMatrix).
SAMPLER_BINDING(4) uniform sampler2D u_prevDenoised;		// R3a: last frame's denoised DI, .w = NRD history length (frames)
SAMPLER_BINDING(5) uniform sampler2D u_prevViewZ;		// R3a: last frame's gather att1 copy ( viewZ, lum )
SAMPLER_BINDING(6) uniform sampler2D u_prevNormal;		// R3a: last frame's gather att2 copy (NRD-packed normal)
SAMPLER_BINDING(9) uniform sampler2D u_prevSide;		// R3a: last frame's gather att3 copy ( SH1, row + 1 [+ reason / 4] ) - view 7 only now
SAMPLER_BINDING(10) uniform sampler2D u_velocity;		// R3a: velocity at the gather res


SAMPLER_BINDING(11) uniform sampler2D u_youngIn;		// young pass: LAST frame's young ( reason / 4, age / 64 ); boost pass: THIS frame's
//   u_tessParms           = ( K picks, boost-ON NRD history (<= 0: trigger off), HOLD frames, last frame's checkerboard parity / -1 )
// Returns the reason the pixel is young: 0 = not, 1 = NRD's history short at the previous position (only with a positive
// threshold), 2 = REVEALED - no previous pixel of the footprint is this surface AND one of them was NEARER (an occluder stood
// here: a mover's wake, floor behind a column while turning; a thin railing jittering over its background is NOT revealed -
// its taps are all farther), 3 = held - it became young less than HOLD frames ago (its own age, from last frame's young
// target: NRD's history cannot end a hold, a flickering light keeps that short forever) / entered the screen.
int diBoostWanted( vec2 sfrag, in DiSurface S, out float ageOut ) {
	const float hOn    = u_tessParms.y;
	const float hold   = u_tessParms.z;
	const int   prevCb = int( floor( u_tessParms.w + 0.5 ) );				// -1 = no side data
	ageOut = 0.0;
	vec2 uvc = sfrag * u_screenCorrection.xy;
	vec2 vel = texture( u_velocity, uvc ).rg;
	vel.y = ( u_windowCoord.z < 0.0 ) ? -vel.y : vel.y;
	vec2 prevUV = uvc - vel;
	if ( any( lessThan( prevUV, vec2( 0.0 ) ) ) || any( greaterThanEqual( prevUV, vec2( 1.0 ) ) ) ) { return 3; }	// just entered the screen: no history at all
	ivec2 res     = ivec2( floor( 1.0 / u_screenCorrection.xy + 0.5 ) );
	ivec2 prevPix = clamp( ivec2( floor( prevUV * vec2( res ) ) ), ivec2( 0 ), res - 1 );
	if ( hOn > 0.0 ) {
		// NRD's history length where this pixel was - the nearest texel, never bilinear (a frame count must not blend across an edge)
		float h = texelFetch( u_prevDenoised, prevPix, 0 ).a;
		if ( !( h >= hOn ) ) { return 1; }									// young by NRD's own count (NaN / garbage counts as young)
	}
	if ( prevCb < 0 ) { return 0; }											// no side data: nothing to compare against
	// the same-surface test over the previous pixels around the reprojection (a tap not traced last frame under
	// checkerboard holds nothing and is skipped)
	float expectedPrevVz = ( u_prevMvpMatrix * vec4( S.Pw, 1.0 ) ).z;
	float tol  = 0.1 * max( abs( expectedPrevVz ), 1.0 );
	// 3x3 around the reprojected pixel, not the bilinear 2x2: under checkerboard only half the taps hold anything, and a
	// background pixel right behind a nearer object had both its traced taps ON that object - "revealed" every frame as the
	// jitter shifted, a 2-px band along every silhouette that passed the cluster rule and was then held (1.5 ms standing
	// still, 2026-10-12). With 9 taps the far side of an edge always has a traced tap of its own surface.
	ivec2 base = clamp( prevPix - 1, ivec2( 0 ), res - 3 );
	bool  anyMatch = false, anyNearer = false;
	for ( int t = 0; t < 9; t++ ) {
		ivec2 pp = base + ivec2( t % 3, t / 3 );
		ivec2 rp = pp;
		if ( prevCb > 0 ) {
			if ( ( pp.x & 1 ) != ( ( pp.y + prevCb - 1 ) & 1 ) ) { continue; }	// not traced last frame
			rp.x = pp.x >> 1;
		}
		float pvz = texelFetch( u_prevViewZ, rp, 0 ).x;
		if ( abs( expectedPrevVz - pvz ) > tol ) {
			if ( pvz > expectedPrevVz + tol ) { anyNearer = true; }		// view z is negative: larger = nearer the camera = an occluder was here
			continue;
		}
		vec3 pN = unpackNrdNormal( texelFetch( u_prevNormal, rp, 0 ).xyz );
		if ( dot( S.Nw, pN ) < 0.5 ) { continue; }
		anyMatch = true;
	}
	if ( !anyMatch ) { return anyNearer ? 2 : 0; }						// revealed from behind an occluder - or a thin edge jittering over its background (not young)
	// hold: this pixel's own age since it became young, from last frame's young target at its previous cell
	ivec2 yp = prevPix;
	if ( prevCb > 0 ) {
		if ( ( yp.x & 1 ) != ( ( yp.y + prevCb - 1 ) & 1 ) ) { yp.x ^= 1; }	// its traced neighbour's cell
		yp.x = min( yp.x, res.x - 1 ) >> 1;
	}
	vec2  py   = texelFetch( u_youngIn, yp, 0 ).xy;
	int   pr   = int( floor( py.x * 4.0 + 0.5 ) );
	float page = floor( py.y * 64.0 + 0.5 );
	if ( pr != 0 && page + 1.0 < hold ) { ageOut = page + 1.0; return 3; }
	return 0;
}
