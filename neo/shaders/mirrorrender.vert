// DUDE: mirrorRenderMap / xrayRenderMap stages (GitHub #34). The vanilla path (draw_common.cpp
// RB_PrepareStageTexturing, TG_SCREEN) generates s,t,q from the clip-space position and runs them
// through the stage texture matrix; the materials' "translate 0.5, 0.5 / scale 0.5, 0.5" map NDC
// to 0..1 over the subview capture. Same here: the matrix rows come in u_diffuseMatrixS/T as
// (m00, m01, 0, m02) and the divide by q happens per fragment (projective, like fixed function).

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 5) in vec4 attr_Color;

VARY(0) out vec3 var_TexCoord;	// s, t, q
VARY(1) out vec4 var_Color;

void main() {
	gl_Position = u_mvpMatrix * attr_Position;

	vec4 stq = vec4( gl_Position.x, gl_Position.y, 0.0, gl_Position.w );
	var_TexCoord = vec3( dot( stq, u_diffuseMatrixS ), dot( stq, u_diffuseMatrixT ), gl_Position.w );

	var_Color = ( attr_Color * u_vertexColorModulate + u_vertexColorAdd ) * u_color;
}
