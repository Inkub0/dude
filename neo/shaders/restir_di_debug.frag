// DUDE ReSTIR DI debug overlay (docs/rtx-restir-di.md, r_rtReStirDIDebug 1): blit the DI irradiance buffer
// over the finished view, exactly like rtgi_debug.frag. The view weapon reads as no-DI (dark) - correct (it
// is lit by the normal path, not world DI), and re-masked here so a reduced-res upscale can't bleed world DI
// onto it.
//
// The source is NRD-YCoCg and may be REDUCED resolution (r_rtReStirDIResScale), so it is upscaled to full res:
// plain bilinear (the sampler), or a depth+normal-aware joint-bilateral when r_rtReStirDIBilateralUpscale is
// on (u_localParam0.x) - the exact 2x2 reweight from rtgi_apply.frag, so the DI stops bleeding across
// silhouettes/creases. YCoCg is decoded with _NRD_YCoCgToLinear (inverse of the gather's pack). Outputs
// LINEAR radiance: this writes into the pre-tonemap HDR scene, so the normal pipeline (FSR2 -> tonemap)
// exposes it; tonemapping here too would double-tonemap and read far too dark. Opaque.
//
//   u_src (unit 0)        = DI irradiance, NRD-YCoCg, reduced res (denoised when it ran, else the raw gather)
//   u_currentDepth (1)    = sealed scene depth (POT)
//   u_normalBuffer (2)    = full-res view-space normal (xyz, a = weapon/AO mask)
//   u_localParam0.x       = bilateral on/off ; .y = depth sensitivity ; .z = normal power
//   u_localParam0.w       = R0 view mode: 0 = DI radiance (YCoCg decode, upscale as above); 1 = PASSTHROUGH of an RGBA8
//                           overlay (NRD OUT_VALIDATION, r_rtReStirDIDebug 3); 2 = HISTORY LENGTH: the denoised output's
//                           .w (accumulated frames, ReBLUR returnHistoryLengthInsteadOfOcclusion) over u_localParam1.x
//                           (the max history) as a grey ramp (r_rtReStirDIDebug 4). Modes 1/2 skip the weapon re-mask
//                           (the overlay is about the denoiser's field, weapon pixels included).
//   u_screenCorrection.xy = 1 / (rw, rh)  - the reduced-res DI texel in the view's 0..1 uv (the 2x2 footprint)
//   u_depthTexRecip.xy    = (w/uploadW, h/uploadH)  - view uv -> depth tc (the POT depth image)

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_src;			// ReSTIR DI irradiance (RGBA16F, NRD-YCoCg, reduced res)
SAMPLER_BINDING(1) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(2) uniform sampler2D u_normalBuffer;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

#define depth_consts DUDE_DEPTH_CONSTS()

// NRD REBLUR radiance decode (_NRD_YCoCgToLinear), the exact inverse of restir_di.frag's nrdLinearToYCoCg.
vec3 nrdYCoCgToLinear( vec3 c ) {
	float t = c.x - c.z;
	return max( vec3( t + c.y, c.x + c.z, t - c.y ), vec3( 0.0 ) );
}

float linDepth( float raw ) { return -1.0 / ( min( raw, 0.9994 ) * depth_consts.x + depth_consts.y ); }	// positive linear view depth

void main() {
	vec2  uv  = var_TexCoord;
	int   mode = int( u_localParam0.w + 0.5 );
	if ( mode == 1 ) {												// NRD validation overlay: as rendered
		fragColor = vec4( texture( u_src, uv ).rgb, 1.0 );
		return;
	}
	if ( mode == 2 ) {												// accumulated history frames -> grey ramp
		float frames = texture( u_src, uv ).a;
		float ramp = clamp( frames / max( u_localParam1.x, 1.0 ), 0.0, 1.0 );
		fragColor = vec4( vec3( ramp ), 1.0 );
		return;
	}
	float raw = texture( u_currentDepth, uv * u_depthTexRecip.xy ).x;
	if ( raw >= 0.9994 ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }	// sky / no geometry: no DI
	vec4 nt0 = texture( u_normalBuffer, uv );
	if ( nt0.a < 0.5 ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }	// view weapon: no world DI (re-masked, like rtgi_apply)

	vec3 gi;
	if ( u_localParam0.x > 0.5 ) {
		// Joint-bilateral upscale (rtgi_apply.frag): reweight the 2x2 reduced-res footprint by how well each
		// texel's surface (full-res depth + normal at the texel centre) matches THIS pixel's, so the DI stays
		// on the right surface instead of bleeding across silhouettes/creases.
		float d0 = linDepth( raw );
		vec3  n0 = nt0.xyz * 2.0 - 1.0;
		vec2  giTexel = u_screenCorrection.xy;							// 1/(rw,rh)
		vec2  f  = uv / giTexel - 0.5;
		vec2  b  = floor( f );
		vec2  fr = f - b;
		vec4  bw = vec4( ( 1.0 - fr.x ) * ( 1.0 - fr.y ), fr.x * ( 1.0 - fr.y ), ( 1.0 - fr.x ) * fr.y, fr.x * fr.y );
		vec2  offs[4] = vec2[4]( vec2( 0.5, 0.5 ), vec2( 1.5, 0.5 ), vec2( 0.5, 1.5 ), vec2( 1.5, 1.5 ) );
		vec3  sum = vec3( 0.0 );
		float wsum = 0.0;
		for ( int k = 0; k < 4; k++ ) {
			vec2  gUV = ( b + offs[k] ) * giTexel;
			float dk  = linDepth( texture( u_currentDepth, gUV * u_depthTexRecip.xy ).x );
			vec3  nk  = texture( u_normalBuffer, gUV ).xyz * 2.0 - 1.0;
			float dw  = exp( -abs( d0 - dk ) / max( d0, 1.0 ) * u_localParam0.y );	// relative depth (scale-invariant)
			float nw  = pow( max( dot( n0, nk ), 0.0 ), u_localParam0.z );
			float wk  = bw[k] * dw * nw;
			sum  += nrdYCoCgToLinear( texture( u_src, gUV ).rgb ) * wk;
			wsum += wk;
		}
		gi = ( wsum > 1.0e-4 ) ? sum / wsum : nrdYCoCgToLinear( texture( u_src, uv ).rgb );	// degenerate: fall back to bilinear
	} else {
		gi = nrdYCoCgToLinear( texture( u_src, uv ).rgb );			// plain bilinear (the sampler upscales)
	}
	fragColor = vec4( gi, 1.0 );
}
