// DUDE RT GI debug overlay (docs/rtx-rtgi.md, r_rtgiDebug): blit the GI radiance buffer over the finished
// view. Both the raw gather and the NRD-denoised output are NRD-YCoCg-packed (rtgi_ray.frag), so decode to
// linear here — otherwise the buffer reads as a red/pink mess. Opaque, so it fully replaces the scene.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_gi;	// GI radiance buffer, YCoCg-packed (raw gather or denoised output)

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

// NRD REBLUR radiance decode (_NRD_YCoCgToLinear), matching rtgi_apply.frag.
vec3 nrdYCoCgToLinear( vec3 c ) {
	float t = c.x - c.z;
	return max( vec3( t + c.y, c.x + c.z, t - c.y ), vec3( 0.0 ) );
}

void main() {
	fragColor = vec4( nrdYCoCgToLinear( texture( u_gi, var_TexCoord ).rgb ), 1.0 );
}
