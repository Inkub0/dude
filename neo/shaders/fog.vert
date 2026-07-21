// New shader (was fixed function): fog pass. Unit 0 texcoords come from
// eye-space texgen planes, unit 1 uses the constant "fog enter" coordinate
// the old path set with glTexCoord2f (passed in u_localParam0.xy).

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;

VARY(0) out vec2 var_TexFog;      // texgen S/T planes
VARY(1) out vec2 var_TexFogEnter; // constant enter coord

void main() {
	var_TexFog = vec2( dot( attr_Position, u_texGen0S ),
	                   dot( attr_Position, u_texGen0T ) );
	var_TexFogEnter = u_localParam0.xy;

	gl_Position = u_mvpMatrix * attr_Position;
}
