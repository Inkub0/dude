// DUDE ReSTIR DI debug overlay (docs/rtx-restir-di.md, r_rtReStirDIDebug 1): blit the DI irradiance buffer
// over the finished view, exactly like rtgi_debug.frag. The view weapon is masked out of the gather, so it
// reads as no-DI (dark) here - that is correct (the weapon is lit by the normal path, not world DI).
//
// The source is NRD-YCoCg (both the raw gather att0 and the denoised REBLUR_DIFFUSE output are packed YCoCg
// by restir_di.frag / NRD), so decode with REBLUR_BackEnd_UnpackRadianceAndNormHitDist's _NRD_YCoCgToLinear -
// the exact inverse of the gather's pack, same as rtgi_apply.frag. (Blitting YCoCg straight shifts to red.)
//
// Outputs LINEAR radiance: this overlay writes into the pre-tonemap HDR scene, so the normal pipeline
// (FSR2 -> eye-adapt -> tonemap) exposes it. Tonemapping here too would double-tonemap and read far too
// dark. Opaque: fully replaces the scene while on.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_src;			// ReSTIR DI irradiance (RGBA16F, NRD-YCoCg)

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

// NRD REBLUR radiance decode (_NRD_YCoCgToLinear), the exact inverse of restir_di.frag's nrdLinearToYCoCg.
vec3 nrdYCoCgToLinear( vec3 c ) {
	float t = c.x - c.z;
	return max( vec3( t + c.y, c.x + c.z, t - c.y ), vec3( 0.0 ) );
}

void main() {
	fragColor = vec4( nrdYCoCgToLinear( texture( u_src, var_TexCoord ).rgb ), 1.0 );
}
