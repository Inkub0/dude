// DUDE GTAO debug overlay (r_ssaoDebug; docs/ssao-gtao.md). Blits the finished AO
// buffer over the scene so the pass can be inspected before it feeds ambientlight.
//   u_localParam0.x = 1 -> show AO scalar (grayscale), 2 -> show bent normal.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_ssao;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	vec4 s = texture( u_ssao, var_TexCoord );
	if ( u_localParam0.x > 1.5 ) {
		vec3 bn = s.gba * 2.0 - 1.0;
		fragColor = vec4( bn * 0.5 + 0.5, 1.0 );
	} else {
		fragColor = vec4( vec3( s.r ), 1.0 );
	}
}
