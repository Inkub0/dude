// HDR bloom (docs/hdr-pipeline.md Phase C): first pass. Sample the HDR scene at half resolution
// and keep the part above the brightness threshold (soft knee: scale by how far each pixel's max
// channel exceeds the threshold, so the colour/hue is preserved). Feeds the downsample chain.
//
//   u_localParam0.x = brightness threshold (r_hdrBloomThreshold)

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_scene;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	vec3  c  = texture( u_scene, var_TexCoord ).rgb;
	float br = max( max( c.r, c.g ), c.b );
	float contrib = max( br - u_localParam0.x, 0.0 ) / max( br, 1e-4 );
	fragColor = vec4( c * contrib, 1.0 );
}
