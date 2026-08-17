// HDR bloom (docs/hdr-pipeline.md Phase C): dual-filter downsample. Each dest texel gathers a
// centre-weighted 5-tap of the (larger) source — the classic Kawase dual-filter kernel, cheap and
// flicker-stable. Bilinear sampling means each tap already averages a 2x2 source block.
//
//   u_localParam0.xy = source texel size (1/srcW, 1/srcH)

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_src;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	vec2 hp = u_localParam0.xy * 0.5;					// half a source texel
	vec3 s  = texture( u_src, var_TexCoord ).rgb * 4.0;
	s += texture( u_src, var_TexCoord - hp ).rgb;
	s += texture( u_src, var_TexCoord + vec2(  hp.x, -hp.y ) ).rgb;
	s += texture( u_src, var_TexCoord + hp ).rgb;
	s += texture( u_src, var_TexCoord + vec2( -hp.x,  hp.y ) ).rgb;
	fragColor = vec4( s * ( 1.0 / 8.0 ), 1.0 );
}
