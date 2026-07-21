// Translated from the soft-particle ARB shader embedded in draw_arb2.cpp
// (originally from The Dark Mod 2.04, (C) Broken Glass Studios, BSD/GPLv3).

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;
layout(location = 5) in vec4 attr_Color;

VARY(0) out vec2 var_TexCoord;
VARY(1) out vec4 var_Color;

void main() {
	// texture matrix from the material stage (ARB env[12]/env[13])
	vec4 st = vec4( attr_TexCoord, 0.0, 1.0 );
	var_TexCoord = vec2( dot( st, u_diffuseMatrixS ), dot( st, u_diffuseMatrixT ) );

	var_Color = attr_Color;

	gl_Position = u_mvpMatrix * attr_Position;
}
