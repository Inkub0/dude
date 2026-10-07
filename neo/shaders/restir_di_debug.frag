// DUDE ReSTIR DI debug overlay (docs/rtx-restir-di.md, r_rtReStirDIDebug 1): blit the DI irradiance buffer
// over the finished view, exactly like rtgi_debug.frag. The view weapon is masked out of the gather, so it
// reads as no-DI (dark) here - that is correct (the weapon is lit by the normal path, not world DI).
//
// Outputs LINEAR radiance: this overlay writes into the pre-tonemap HDR scene, so the normal pipeline
// (FSR2 -> eye-adapt -> tonemap) exposes it. Tonemapping here too would double-tonemap and read far too
// dark. Opaque: fully replaces the scene while on.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_src;			// ReSTIR DI irradiance (RGBA16F, linear)

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	fragColor = vec4( max( texture( u_src, var_TexCoord ).rgb, vec3( 0.0 ) ), 1.0 );
}
