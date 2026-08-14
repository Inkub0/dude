// SSR glossy reflection-mip build (docs/ssr.md, PBR Phase C.2.1, r_ssrGlossy).
// Builds a small pyramid of the (temporally-accumulated) reflection buffer so the
// composite can sample it at a roughness-proportional LOD — sharp at roughness 0,
// progressively blurred toward r_ssrMaxRoughness. Level 0 is a 1:1 copy of the
// result buffer; levels 1..N are a 2x2 box average of the previous level (the same
// average-downsample every prefiltered-radiance SSR uses).
//
// u_localParam0.x = the source mip level (Vulkan binds a single-level view -> 0; GL3
//                   binds the whole texture -> the real source level for texelFetch).
// u_localParam0.y = mode: 0 = copy the result buffer into level 0 (1 tap, 1:1);
//                   1 = 2x2 box downsample of the source level into the next level.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_srcColor;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	int srcLod = int( u_localParam0.x + 0.5 );

	if ( u_localParam0.y < 0.5 ) {
		// level 0: copy the reflection result into the pyramid base. The source is an
		// external colour target, so sample through texture()/var_TexCoord — that carries
		// the same colour-target flip the composite uses, keeping the pyramid oriented like
		// the sharp result buffer (texelFetch would bypass the flip and mirror it vertically).
		fragColor = texture( u_srcColor, var_TexCoord );
		return;
	}

	// levels 1..N: average the 2x2 source footprint (clamped at odd sizes)
	ivec2 mx = textureSize( u_srcColor, srcLod ) - 1;
	ivec2 s  = ivec2( gl_FragCoord.xy ) * 2;
	vec4  c0 = texelFetch( u_srcColor, min( s,                 mx ), srcLod );
	vec4  c1 = texelFetch( u_srcColor, min( s + ivec2( 1, 0 ), mx ), srcLod );
	vec4  c2 = texelFetch( u_srcColor, min( s + ivec2( 0, 1 ), mx ), srcLod );
	vec4  c3 = texelFetch( u_srcColor, min( s + ivec2( 1, 1 ), mx ), srcLod );
	fragColor = ( c0 + c1 + c2 + c3 ) * 0.25;
}
