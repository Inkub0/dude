// HDR bloom (docs/hdr-pipeline.md Phase C): first pass. Sample the HDR scene at half resolution
// and keep the part above the brightness threshold (soft knee: scale by how far each pixel's max
// channel exceeds the threshold, so the colour/hue is preserved). Feeds the downsample chain.
//
//   u_localParam0.x = brightness threshold (r_hdrBloomThreshold)
//   u_localParam1   = the part of the image the scene fills (offset.xy, scale.zw; 0,0,1,1 for a
//                     full view): a world view rendered into a crop (Quake 4's double vision,
//                     RoE/LM's low-res fullscreen FX) blooms from the crop, stretched to the
//                     screen as the game's 2D composite stretches it. Clamped half a texel inside.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_scene;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	vec2  ht = 0.5 / vec2( textureSize( u_scene, 0 ) );
	vec2  uv = clamp( u_localParam1.xy + var_TexCoord * u_localParam1.zw, u_localParam1.xy + ht, u_localParam1.xy + u_localParam1.zw - ht );
	vec3  c  = texture( u_scene, uv ).rgb;
	float br = max( max( c.r, c.g ), c.b );
	float contrib = max( br - u_localParam0.x, 0.0 ) / max( br, 1e-4 );
	fragColor = vec4( c * contrib, 1.0 );
}
