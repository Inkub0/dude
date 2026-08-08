// SSAO Phase 1 depth-mip max-downsample (docs/ssao-perf-optimization.md).
// Reduces one level of the LINEAR-depth chain into the next, taking the MAX (farthest)
// of each 2x2 footprint instead of a box average. A box average blends foreground and
// background depth across a silhouette into a false mid-depth surface; a far horizon
// step then reads it as a phantom occluder, casting a radial dark halo around objects.
// GTAO occlusion rises with CLOSER occluders, so keeping the farthest surface is the
// conservative choice: it removes the phantom without touching real near contact (level
// 0 stays exact). u_localParam0.x = the source mip level (Vulkan binds a single-level
// view -> 0; GL3 binds the whole texture -> the real source level for texelFetch).

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_srcDepth;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	int   srcLod = int( u_localParam0.x + 0.5 );
	ivec2 sz  = textureSize( u_srcDepth, srcLod );
	ivec2 mx  = sz - 1;
	ivec2 s   = ivec2( gl_FragCoord.xy ) * 2;   // 2x2 source footprint (clamped at odd sizes)
	float d0  = texelFetch( u_srcDepth, min( s,                 mx ), srcLod ).r;
	float d1  = texelFetch( u_srcDepth, min( s + ivec2( 1, 0 ), mx ), srcLod ).r;
	float d2  = texelFetch( u_srcDepth, min( s + ivec2( 0, 1 ), mx ), srcLod ).r;
	float d3  = texelFetch( u_srcDepth, min( s + ivec2( 1, 1 ), mx ), srcLod ).r;
	fragColor = vec4( max( max( d0, d1 ), max( d2, d3 ) ), 0.0, 0.0, 1.0 );
}
