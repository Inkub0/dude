// DUDE ReSTIR DI debug overlay (docs/rtx-restir-di.md, r_rtReStirDIDebug): blit the Stage-1 unshadowed
// direct-irradiance buffer over the finished view, Reinhard-tonemapped so the HDR 1-spp result is
// visible. It WILL look grainy - that is the raw single-sample estimate before temporal/spatial reuse
// and NRD (Stages 3-5). Opaque: fully replaces the scene while on. Eyeball check against r_rtAllLights
// (with shadows off this reads as the same diffuse light, just noisier).

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_src;		// ReSTIR DI irradiance (RGBA16F)

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	vec3 c = max( texture( u_src, var_TexCoord ).rgb, vec3( 0.0 ) );
	c = c / ( c + vec3( 1.0 ) );		// Reinhard tonemap for display
	fragColor = vec4( c, 1.0 );
}
