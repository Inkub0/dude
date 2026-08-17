// HDR eye adaptation (Phase B1): box-average one level of the log-luma chain into the next
// (2x2 footprint), reducing toward a 1x1 average log-luminance. Averaging log values yields
// a geometric mean once exp()'d in hdrexpose. u_localParam0.x = the source mip level (Vulkan
// binds a single-level view -> 0; GL3 binds the whole texture -> the real level for texelFetch).
// Mirrors ssao_depthdown.frag, but averages instead of taking the max.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_src;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	int   srcLod = int( u_localParam0.x + 0.5 );
	ivec2 sz  = textureSize( u_src, srcLod );
	ivec2 mx  = sz - 1;
	ivec2 s   = ivec2( gl_FragCoord.xy ) * 2;   // 2x2 source footprint (clamped at odd sizes)
	float a0  = texelFetch( u_src, min( s,                 mx ), srcLod ).r;
	float a1  = texelFetch( u_src, min( s + ivec2( 1, 0 ), mx ), srcLod ).r;
	float a2  = texelFetch( u_src, min( s + ivec2( 0, 1 ), mx ), srcLod ).r;
	float a3  = texelFetch( u_src, min( s + ivec2( 1, 1 ), mx ), srcLod ).r;
	fragColor = vec4( ( a0 + a1 + a2 + a3 ) * 0.25, 0.0, 0.0, 1.0 );
}
