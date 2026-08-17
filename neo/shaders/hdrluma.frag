// HDR eye adaptation (Phase B1, docs/hdr-pipeline.md): level 0 of the luminance reduction.
// Sample the HDR scene and write its LOG luminance; hdrlumadown then box-averages this down
// the mip chain to a 1x1 texel = the average log-luma, whose exp() is the geometric-mean
// scene luminance (robust to a few very bright pixels). Rendered at the luma target's base
// resolution, so var_TexCoord (0..1) maps the whole scene into this coarse sampling grid.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_hdrScene;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	vec3  c    = texture( u_hdrScene, var_TexCoord ).rgb;
	float luma = dot( c, vec3( 0.2126, 0.7152, 0.0722 ) );
	// log-luminance; floor keeps log() finite and bounds pure-black areas.
	fragColor  = vec4( log( max( luma, 1e-4 ) ), 0.0, 0.0, 1.0 );
}
