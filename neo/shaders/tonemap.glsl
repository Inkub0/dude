// DUDE HDR tonemap curves (r_hdrTonemap). Shared by hdrresolve.frag and
// hdrresolve_smaa.frag: takes the linear HDR scene colour, applies a static
// exposure multiply, then maps it into the display [0,1] range with the
// selected curve. The result is linear-light [0,1]; the caller's gamma/brightness
// tail (or the standalone GL gamma pass) does the sRGB encode afterwards, so the
// order stays exposure -> tonemap -> encode.
//
// mode 0 is a PURE passthrough (no exposure, no curve, no clamp) so with r_hdrTonemap 0
// the resolve is bit-identical to the pre-tonemap behaviour REGARDLESS of r_hdrExposure:
// the existing gamma tail still does the final clamp exactly as before. Exposure only
// shapes the curves (modes 1-4).
//
//   0 = off / faithful   (pure passthrough)
//   1 = Reinhard         (baseline)
//   2 = ACES             (Narkowicz fitted; the game-standard filmic look)
//   3 = AgX              (Wrensch minimal AgX; neutral, gentle highlight rolloff)
//   4 = Khronos PBR Neutral (hue-preserving; least colour shift of D3's authored art)
//   5 = DUDE             (signature: faithful blacks/midtones, hue-preserving highlight shoulder)

#ifndef DUDE_TONEMAP_GLSL
#define DUDE_TONEMAP_GLSL

// --- ACES (Narkowicz 2015 fitted approximation) -------------------------------
vec3 DudeTonemapACES( vec3 x ) {
	const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
	return clamp( ( x * ( a * x + b ) ) / ( x * ( c * x + d ) + e ), 0.0, 1.0 );
}

// --- AgX (Benjamin Wrensch minimal AgX; the three.js / Godot port) ------------
vec3 DudeAgxContrast( vec3 x ) {
	vec3 x2 = x * x;
	vec3 x4 = x2 * x2;
	return   15.5    * x4 * x2
	       - 40.14   * x4 * x
	       + 31.96   * x4
	       -  6.868  * x2 * x
	       +  0.4298 * x2
	       +  0.1191 * x
	       -  0.00232;
}

vec3 DudeTonemapAgX( vec3 val ) {
	const mat3 agxIn = mat3(
		0.842479062253094,  0.0423282422610123, 0.0423756549057051,
		0.0784335999999992, 0.878468636469772,  0.0784336,
		0.0792237451477643, 0.0791661274605434, 0.879142973793104 );
	const mat3 agxOut = mat3(
		 1.19687900512017,   -0.0528968517574562, -0.0529716355144438,
		-0.0980208811401368,  1.15190312990417,   -0.0980434501171241,
		-0.0990297440797205, -0.0989611768448433,  1.15107367264116 );
	const float minEv = -12.47393;
	const float maxEv =   4.026069;

	val = agxIn * val;
	val = clamp( log2( max( val, 1e-10 ) ), minEv, maxEv );
	val = ( val - minEv ) / ( maxEv - minEv );
	val = DudeAgxContrast( val );
	val = agxOut * val;
	// AgX's contrast approx works in a ~2.2 space; lift back to linear so the
	// downstream gamma encode matches the other curves.
	val = pow( max( val, 0.0 ), vec3( 2.2 ) );
	return clamp( val, 0.0, 1.0 );
}

// --- Khronos PBR Neutral (hue-preserving compressor) --------------------------
vec3 DudeTonemapPBRNeutral( vec3 color ) {
	const float startCompression = 0.8 - 0.04;
	const float desaturation     = 0.15;

	float x = min( color.r, min( color.g, color.b ) );
	float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
	color -= offset;

	float peak = max( color.r, max( color.g, color.b ) );
	if ( peak < startCompression ) {
		return color;
	}

	float d = 1.0 - startCompression;
	float newPeak = 1.0 - d * d / ( peak + d - startCompression );
	color *= newPeak / peak;

	float g = 1.0 - 1.0 / ( desaturation * ( peak - newPeak ) + 1.0 );
	return mix( color, newPeak * vec3( 1.0 ), g );
}

// --- DUDE (signature curve) ---------------------------------------------------
// Faithful blacks + midtones, hue-preserving filmic highlight shoulder. Doom 3's
// art is authored to the LDR clamp: inky shadows and lamp / fire cores that saturate
// at 1.0 are deliberate. So below the knee this is the identity (the vanilla look is
// untouched); only the > knee highlights - the real > 1.0 energy that r_hdrOverbright
// now injects - roll off along a shoulder that is C1-continuous with the identity
// segment (value knee and slope 1 at the knee) and asymptotes to 1.0. Rescaling every
// channel by a single peak-derived factor keeps hue, so coloured emissives (fire, lava,
// hell reds, monitor greens) compress WITHOUT the per-channel wash toward white that
// Reinhard / ACES apply. Exposure (applied before) slides the scene across the knee.
// knee  (r_hdrDudeKnee):  identity below it; exponential shoulder above. Lower = roll off sooner.
// desat (r_hdrDudeDesat): 0 = pure hue-preserving; >0 blends compressed highlights toward the
//                         white-hot target so bright cores read as detail, not a saturated blob.
// tint  (r_hdrDudeTint):  where "white-hot" points. 0 = neutral white (fully desaturated - can go
//                         pink on saturated lava, since red->white passes through pink); 1 = keep
//                         the highlight's own hue (no whitening). ~0.35 keeps molten cores warm.
// whitePoint (r_hdrAdaptWhitePoint via eye-adaptation): 0 = static shoulder (decay = 1-knee). When
// >knee it's the post-exposure luminance that should map to ~white, so the shoulder decay stretches
// to reach ~0.99 there — a bright core keeps internal gradient instead of a flat disc. Clamped to
// never roll off FASTER than static (extend-only), so enabling it can't crush highlights vs default.
vec3 DudeTonemapDude( vec3 c, float knee, float desat, float tint, float whitePoint ) {
	knee = clamp( knee, 0.05, 0.95 );
	float peak = max( c.r, max( c.g, c.b ) );
	if ( peak <= knee ) return c;                       // faithful: shadows + midtones untouched
	float tau = 1.0 - knee;                             // static shoulder decay
	if ( whitePoint > knee + 0.05 ) {                   // adaptive: reach ~0.99 at peak == whitePoint
		tau = max( ( whitePoint - knee ) / log( 100.0 * ( 1.0 - knee ) ), 1.0 - knee );
	}
	float mapped = 1.0 - ( 1.0 - knee ) * exp( -( peak - knee ) / tau );
	vec3  col = c * ( mapped / peak );                  // hue-preserving highlight rolloff
	// white-hot: the harder a highlight is compressed (peak - mapped), the more it fades toward the
	// target, so fireball / lava cores gain internal gradient instead of clipping to a saturated blob.
	// The target keeps `tint` of the highlight's own colour (tint 0 = neutral white), which stops a
	// saturated red core going pink on the way to white. desat scales the ramp; 0 = pure hue-preserving.
	vec3  target = mix( vec3( mapped ), col, clamp( tint, 0.0, 1.0 ) );
	float g = 1.0 - 1.0 / ( desat * ( peak - mapped ) + 1.0 );
	return mix( col, target, g );
}

// --- dispatch -----------------------------------------------------------------
vec3 DudeTonemap( vec3 c, float exposure, int mode, float dudeKnee, float dudeDesat, float dudeTint, float dudeWhitePoint ) {
	if ( mode == 0 ) return c;                          // pure passthrough (faithful); exposure only shapes the curves
	c *= exposure;
	if ( mode == 1 ) return c / ( c + vec3( 1.0 ) );   // Reinhard
	if ( mode == 2 ) return DudeTonemapACES( c );
	if ( mode == 3 ) return DudeTonemapAgX( c );
	if ( mode == 4 ) return DudeTonemapPBRNeutral( c );
	if ( mode == 5 ) return DudeTonemapDude( c, dudeKnee, dudeDesat, dudeTint, dudeWhitePoint );
	return c;
}

#endif // DUDE_TONEMAP_GLSL
