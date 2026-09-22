// DUDE RT GI apply (docs/rtx-rtgi.md H6c): add one bounce of indirect diffuse into the lit HDR scene,
// scene-wide. For each pixel: indirect = receiver albedo * gathered irradiance. The receiver albedo is
// the RTGI prepass's 4th attachment (gbuffer.frag out_Albedo); the irradiance is the denoised (H6b) /
// raw (H6a) gather (rtgi_ray.frag), which already returns the hemisphere-averaged incoming radiance
// (the 1/pi * pi of cosine sampling cancels). Additive (GLS_SRCBLEND_ONE|ONE) into the live rhiHdrRT,
// after world lighting and before FSR2/tonemap. VK + RT hardware only; drawn only when r_rtgi is on.
//
// Uniform packing (RB_RHI_RtgiApply):
//   u_localParam0.x       = r_rtgiIntensity
//   u_localParam0.y       = r_rtgiBilateralUpscale (0 = plain bilinear, 1 = depth+normal-aware upscale)
//   u_localParam0.zw      = 1 / GI buffer size (reduced-res texel, for the 2x2 bilateral footprint)
//   u_localParam1.x       = depth sensitivity (bigger = sharper depth cut); u_localParam1.y = normal power
//   u_screenCorrection.xy = 1 / viewSize   (gl_FragCoord -> [0,1] uv; the reduced-res GI buffer
//                           bilinear-upscales at this uv, the full-res albedo samples 1:1)
//   u_depthTexRecip.xy    = gl_FragCoord -> _currentDepth tc (sky skip)

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_gi;            // gathered indirect radiance (reduced res)
SAMPLER_BINDING(1) uniform sampler2D u_albedo;        // receiver albedo (RTGI prepass attachment 3), full res
SAMPLER_BINDING(2) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(3) uniform sampler2D u_normalBuffer;  // full-res view-space normal (xyz), for the bilateral weight

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

#define depth_consts DUDE_DEPTH_CONSTS()

// NRD REBLUR radiance decode (REBLUR_BackEnd_UnpackRadianceAndNormHitDist -> _NRD_YCoCgToLinear): the GI
// buffer (denoised output OR the raw gather, both written YCoCg by rtgi_ray.frag) is YCoCg; return linear.
vec3 nrdYCoCgToLinear( vec3 c ) {
	float t = c.x - c.z;
	return max( vec3( t + c.y, c.x + c.z, t - c.y ), vec3( 0.0 ) );
}

float linDepth( float raw ) { return -1.0 / ( min( raw, 0.9994 ) * depth_consts.x + depth_consts.y ); }	// positive linear view depth

void main() {
	vec2  frag = gl_FragCoord.xy;
	float raw  = texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;
	if ( raw >= 0.9994 ) { fragColor = vec4( 0.0 ); return; }	// sky / no geometry: no receiver, no indirect

	vec2 uv     = frag * u_screenCorrection.xy;
	vec3 gi;
	if ( u_localParam0.y > 0.5 ) {
		// Joint-bilateral upscale: the GI is low-res, so a plain bilinear stretch bleeds it across silhouettes
		// and creases. Reweight the 2x2 low-res footprint by how well each texel's surface (full-res depth +
		// normal sampled at the texel centre) matches THIS pixel's, so the bounce stays on the right surface.
		vec2  depthScale = u_depthTexRecip.xy / u_screenCorrection.xy;	// normalized uv -> POT depth tc
		float d0 = linDepth( raw );
		vec3  n0 = texture( u_normalBuffer, uv ).xyz * 2.0 - 1.0;
		vec2  giTexel = u_localParam0.zw;								// 1 / GI size
		vec2  f  = uv / giTexel - 0.5;
		vec2  b  = floor( f );
		vec2  fr = f - b;
		vec4  bw = vec4( ( 1.0 - fr.x ) * ( 1.0 - fr.y ), fr.x * ( 1.0 - fr.y ), ( 1.0 - fr.x ) * fr.y, fr.x * fr.y );
		vec2  offs[4] = vec2[4]( vec2( 0.5, 0.5 ), vec2( 1.5, 0.5 ), vec2( 0.5, 1.5 ), vec2( 1.5, 1.5 ) );
		vec3  sum = vec3( 0.0 );
		float wsum = 0.0;
		for ( int k = 0; k < 4; k++ ) {
			vec2  gUV = ( b + offs[k] ) * giTexel;
			float dk  = linDepth( texture( u_currentDepth, gUV * depthScale ).x );
			vec3  nk  = texture( u_normalBuffer, gUV ).xyz * 2.0 - 1.0;
			float dw  = exp( -abs( d0 - dk ) / max( d0, 1.0 ) * u_localParam1.x );		// relative depth (scale-invariant)
			float nw  = pow( max( dot( n0, nk ), 0.0 ), u_localParam1.y );
			float w   = bw[k] * dw * nw;
			sum  += nrdYCoCgToLinear( texture( u_gi, gUV ).rgb ) * w;
			wsum += w;
		}
		gi = ( wsum > 1.0e-4 ) ? sum / wsum : nrdYCoCgToLinear( texture( u_gi, uv ).rgb );	// degenerate: fall back to bilinear
	} else {
		gi = nrdYCoCgToLinear( texture( u_gi, uv ).rgb );			// plain bilinear (baseline)
	}
	vec3 albedo = texture( u_albedo, uv ).rgb;

	// indirect diffuse = receiver albedo * incoming irradiance, scaled by r_rtgiIntensity. Additive.
	fragColor = vec4( albedo * gi * u_localParam0.x, 1.0 );
}
