// New shader (was fixed function): GUI/2D drawing and plain old-style material
// stages. Texture matrix via the diffuse matrix slots; vertex color modes via
// modulate/add (1.0 / color / 1-color), then scaled by the stage color.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;
layout(location = 5) in vec4 attr_Color;

VARY(0) out vec2 var_TexCoord;
VARY(1) out vec4 var_Color;

void main() {
	vec4 st = vec4( attr_TexCoord, 0.0, 1.0 );
	var_TexCoord = vec2( dot( st, u_diffuseMatrixS ), dot( st, u_diffuseMatrixT ) );

	var_Color = ( attr_Color * u_vertexColorModulate + u_vertexColorAdd ) * u_color;

	gl_Position = u_mvpMatrix * attr_Position;
}
