// HDR bloom (docs/hdr-pipeline.md Phase C): 3x3 tent upsample + combine. Tent-upsamples the smaller
// (already up-accumulated) level from unit 0 and ADDS this level's own downsample content (unit 1),
// writing the sum — so the up-chain accumulates a wide soft glow WITHOUT relying on load-preserve
// (VK BeginTargetPass always clears, so we combine in-shader and overwrite a separate up target).
//
//   u_localParam0.xy = source texel size of unit 0 (1/srcW, 1/srcH) for the tent offsets

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_src;   // smaller level to tent-upsample
SAMPLER_BINDING(1) uniform sampler2D u_add;   // this level's downsample content, added at 1:1

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	vec2 t = u_localParam0.xy;							// one source texel
	vec3 s  = texture( u_src, var_TexCoord + vec2( -t.x, -t.y ) ).rgb;
	s += texture( u_src, var_TexCoord + vec2(  0.0, -t.y ) ).rgb * 2.0;
	s += texture( u_src, var_TexCoord + vec2(  t.x, -t.y ) ).rgb;
	s += texture( u_src, var_TexCoord + vec2( -t.x,  0.0 ) ).rgb * 2.0;
	s += texture( u_src, var_TexCoord ).rgb * 4.0;
	s += texture( u_src, var_TexCoord + vec2(  t.x,  0.0 ) ).rgb * 2.0;
	s += texture( u_src, var_TexCoord + vec2( -t.x,  t.y ) ).rgb;
	s += texture( u_src, var_TexCoord + vec2(  0.0,  t.y ) ).rgb * 2.0;
	s += texture( u_src, var_TexCoord + vec2(  t.x,  t.y ) ).rgb;
	vec3 up  = s * ( 1.0 / 16.0 );
	vec3 add = texture( u_add, var_TexCoord ).rgb;
	fragColor = vec4( up + add, 1.0 );
}
