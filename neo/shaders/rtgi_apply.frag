// DUDE RT GI apply (docs/rtx-rtgi.md H6c): add one bounce of indirect diffuse into the lit HDR scene,
// scene-wide. For each pixel: indirect = receiver albedo * gathered irradiance. The receiver albedo is
// the RTGI prepass's 4th attachment (gbuffer.frag out_Albedo); the irradiance is the denoised (H6b) /
// raw (H6a) gather (rtgi_ray.frag), which already returns the hemisphere-averaged incoming radiance
// (the 1/pi * pi of cosine sampling cancels). Additive (GLS_SRCBLEND_ONE|ONE) into the live rhiHdrRT,
// after world lighting and before FSR2/tonemap. VK + RT hardware only; drawn only when r_rtgi is on.
//
// Uniform packing (RB_RHI_RtgiApply):
//   u_localParam0.x       = r_rtgiIntensity
//   u_screenCorrection.xy = 1 / viewSize   (gl_FragCoord -> [0,1] uv; the reduced-res GI buffer
//                           bilinear-upscales at this uv, the full-res albedo samples 1:1)
//   u_depthTexRecip.xy    = gl_FragCoord -> _currentDepth tc (sky skip)

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_gi;            // gathered indirect radiance (reduced res, bilinear upscaled)
SAMPLER_BINDING(1) uniform sampler2D u_albedo;        // receiver albedo (RTGI prepass attachment 3), full res
SAMPLER_BINDING(2) uniform sampler2D u_currentDepth;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

#define depth_consts DUDE_DEPTH_CONSTS()

// NRD REBLUR radiance decode (REBLUR_BackEnd_UnpackRadianceAndNormHitDist -> _NRD_YCoCgToLinear): the GI
// buffer (denoised output OR the raw gather, both written YCoCg by rtgi_ray.frag) is YCoCg; return linear.
vec3 nrdYCoCgToLinear( vec3 c ) {
	float t = c.x - c.z;
	return max( vec3( t + c.y, c.x + c.z, t - c.y ), vec3( 0.0 ) );
}

void main() {
	vec2  frag = gl_FragCoord.xy;
	float raw  = texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;
	if ( raw >= 0.9994 ) { fragColor = vec4( 0.0 ); return; }	// sky / no geometry: no receiver, no indirect

	vec2 uv     = frag * u_screenCorrection.xy;
	vec3 gi     = nrdYCoCgToLinear( texture( u_gi, uv ).rgb );	// H6b: NRD output is YCoCg-packed
	vec3 albedo = texture( u_albedo, uv ).rgb;

	// indirect diffuse = receiver albedo * incoming irradiance, scaled by r_rtgiIntensity. Additive.
	fragColor = vec4( albedo * gi * u_localParam0.x, 1.0 );
}
