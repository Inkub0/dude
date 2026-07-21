// New shader (was fixed function): blend light. Projective light texture on
// unit 0 (S/T/Q texgen planes), falloff on unit 1.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;

VARY(0) out vec4 var_TexProjection; // (s, t, -, q)
VARY(1) out vec2 var_TexFalloff;

void main() {
	var_TexProjection = vec4( dot( attr_Position, u_texGen0S ),
	                          dot( attr_Position, u_texGen0T ),
	                          0.0,
	                          dot( attr_Position, u_texGen0Q ) );
	var_TexFalloff = vec2( dot( attr_Position, u_texGen1S ), 0.5 );

	gl_Position = u_mvpMatrix * attr_Position;
}
