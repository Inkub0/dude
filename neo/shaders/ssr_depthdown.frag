// SSR Hi-Z depth-mip MIN-downsample (docs/ssao-perf-optimization.md, r_ssrHiZ).
// Reduces one level of the LINEAR min-Z chain into the next, taking the MIN (NEAREST
// surface) of each 2x2 footprint. This is the OPPOSITE of SSAO's max: the march may only
// leap a screen span if it is provably in front of EVERY surface under that span, so the
// coarse cell must report the closest (min) surface — never over-report distance, or a
// leap could skip a real reflection hit. Min is therefore the safe (conservative) filter
// for "can the ray cross anything in this block". Level 0 stays exact.
// u_localParam0.x = the source mip level (Vulkan binds a single-level view -> 0; GL3
// binds the whole texture -> the real source level for texelFetch).

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
	fragColor = vec4( min( min( d0, d1 ), min( d2, d3 ) ), 0.0, 0.0, 1.0 );
}
