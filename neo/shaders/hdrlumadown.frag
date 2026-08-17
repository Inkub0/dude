// HDR eye adaptation (Phase B1): box-average an 8x8 block of the source into each dest texel — a
// fixed 8x reduction (64->8, then 8->1). Averaging log-luma values yields a geometric mean once
// exp()'d in hdrexpose. The source is an ordinary single-level render target, so this samples lod 0
// (no mip-level param, unlike the old mip-chain version): the whole reduction runs through the
// properly-synchronized BeginTargetPass path.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_src;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	ivec2 sz   = textureSize( u_src, 0 );
	ivec2 mx   = sz - 1;
	ivec2 base = ivec2( gl_FragCoord.xy ) * 8;   // top-left of this dest texel's 8x8 source block
	float sum  = 0.0;
	for ( int y = 0; y < 8; y++ ) {
		for ( int x = 0; x < 8; x++ ) {
			sum += texelFetch( u_src, min( base + ivec2( x, y ), mx ), 0 ).r;
		}
	}
	fragColor = vec4( sum * ( 1.0 / 64.0 ), 0.0, 0.0, 1.0 );
}
