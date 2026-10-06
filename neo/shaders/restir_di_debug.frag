// DUDE ReSTIR DI debug overlay (docs/rtx-restir-di.md, r_rtReStirDIDebug): blit the Stage-1 unshadowed
// direct-irradiance buffer over the finished view. r_rtReStirDIDebug 1 = the raw RIS estimate (grainy +
// FSR2-ghosted by design: a 1-spp signal before temporal/spatial reuse + NRD, Stages 3-5); 2 = the brute
// unshadowed ground truth the RIS converges to (clean - use this to confirm the lighting/colour).
//
// Outputs LINEAR radiance, exactly like rtgi_debug.frag: this overlay writes into the pre-tonemap HDR
// scene, so the normal pipeline (FSR2 -> eye-adapt -> tonemap) exposes it. Tonemapping here too would
// double-tonemap and read far too dark. Opaque: fully replaces the scene while on.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_src;		// ReSTIR DI irradiance (RGBA16F, linear)

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	fragColor = vec4( max( texture( u_src, var_TexCoord ).rgb, vec3( 0.0 ) ), 1.0 );
}
