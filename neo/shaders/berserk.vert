// DUDE berserk vision (RHI backends): vertex half of the radial zoom-blur that
// reproduces the stock "streak zoom". Like generic.vert — texture matrix into
// var_TexCoord (carries the _scratch flip + centering), vertex color into
// var_Color — but without the tessellation passthrough (berserk never tessellates).

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
