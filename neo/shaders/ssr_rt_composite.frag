// DUDE RT reflections composite (docs/rtx-reflections.md RR6b/RR8): additively composite the RT
// reflection (rhiRtReflRT, or its RR6a/RR6c temporal-upscaled result) onto the lit scene, with an
// optional box blur AND a full-res material mask.
//
// WHY BLUR (RR6b). RT fills reflective pixels the screen-space march misses; a lone 1px fill is a small
// brightness/hue STEP its neighbours don't share, which HDR preserves into a dot temporal can't dissolve
// (it's spatially static). A box blur spreads each fill into a low-frequency haze and dims ISOLATED
// speckles by peak/N^2 while contiguous fills keep their brightness.
//
// WHY THE MATERIAL MASK (RR8). The reflection is traced + upsampled at r_ssrResScale (low res), so at a
// reflective surface's SILHOUETTE it bleeds outward — the blur and the bilinear upscale both spill the
// low-res reflection a texel or two past the true edge. On a slightly-reflective monster (flesh) that
// reads as a WHITE HALO around it, worst at 1/4 res where the low-res edge is coarse (pixelated). SSR
// never shows this because ssr_composite applies its Fresnel/gloss weight at FULL res, cutting the
// reflection at the material's real silhouette. We do the same here: mask the contribution by the
// FULL-RES G-buffer reflectivity, so a non-reflective background pixel (gloss ~ 0) gets nothing no matter
// how far the low-res reflection bled into it. This is a hard-edged MASK (0/1), not the soft weight
// ssr_rt already applied at the hit — so it kills the halo without dimming the reflection twice.
//
//   unit 0 (u_rtRefl)       = the RT reflection contribution (rgb = weighted radiance, a = hit mask)
//   unit 1 (u_materialBuffer)= full-res G-buffer x = roughness, y = metalness
//   u_localParam0.x         = blur strength 0..1 (r_rtReflBlur; 0 = sharp 1:1, mask still applies)
//   u_localParam0.y         = tap spacing in target texels (spread)
//   u_localParam0.z         = roughness fade start (fraction of the cutoff)
//   u_localParam1.z         = max roughness cutoff
//   u_screenCorrection.xy   = 1 / rtTargetSize (texel size of the RT reflection target)
// Additive blend (GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE) is set by the pipeline.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_rtRefl;
SAMPLER_BINDING(1) uniform sampler2D u_materialBuffer;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	vec4 c = texture( u_rtRefl, var_TexCoord );

	float amt = clamp( u_localParam0.x, 0.0, 1.0 );
	vec4 result;
	if ( amt <= 0.0 ) {
		result = c;						// r_rtReflBlur 0: sharp 1:1 (the mask below still applies)
	} else {
		// 5x5 box as 8 BILINEAR taps + the already-fetched centre, instead of 25 point taps (RR11).
		// A contiguous box (the shipped tap spacing is 1 texel, u_localParam0.y = 1) is separable, and a
		// bilinear fetch at the MIDPOINT of two adjacent texels returns their exact average. Grouping each
		// axis as {-2,-1},{0},{+1,+2} -> {pair, centre, pair} with per-axis weights {2,1,2}/5 reproduces the
		// 25-texel equal average bit-for-bit: every source texel still contributes 1/25. Corner taps average
		// a 2x2 block (weight 4/25), edge taps a 1x2 pair (2/25), the centre is the lone texel (1/25 -> reuse
		// c). Exact at spacing 1; a dilated spacing turns it into an equivalent smooth box, not the sparse one.
		vec2 o = 1.5 * u_screenCorrection.xy * max( u_localParam0.y, 0.0 );	// midpoint of texels +/-1 and +/-2
		vec4 sum = c * ( 1.0 / 25.0 )
			+ ( texture( u_rtRefl, var_TexCoord + vec2( -o.x,  0.0 ) )
			  + texture( u_rtRefl, var_TexCoord + vec2(  o.x,  0.0 ) )
			  + texture( u_rtRefl, var_TexCoord + vec2(  0.0, -o.y ) )
			  + texture( u_rtRefl, var_TexCoord + vec2(  0.0,  o.y ) ) ) * ( 2.0 / 25.0 )
			+ ( texture( u_rtRefl, var_TexCoord + vec2( -o.x, -o.y ) )
			  + texture( u_rtRefl, var_TexCoord + vec2(  o.x, -o.y ) )
			  + texture( u_rtRefl, var_TexCoord + vec2( -o.x,  o.y ) )
			  + texture( u_rtRefl, var_TexCoord + vec2(  o.x,  o.y ) ) ) * ( 4.0 / 25.0 );
		result = mix( c, sum, amt );	// blend sharp<->blurred by strength
	}

	// RR8 full-res material mask: cut the contribution wherever THIS pixel's own surface isn't reflective,
	// so the low-res reflection (blurred + upscaled) can't halo past a reflective monster onto the
	// non-reflective background. Same gloss window ssr_rt/ssr_composite use, but taken as a 0/1 mask (a
	// soft ramp only to anti-alias the silhouette) rather than re-applying the weight.
	float rough    = texture( u_materialBuffer, var_TexCoord ).x;
	float maxRough = u_localParam1.z;
	float gloss    = 1.0 - smoothstep( maxRough * u_localParam0.z, maxRough, rough );
	float mask     = smoothstep( 0.0, 0.04, gloss );

	fragColor = result * mask;
}
