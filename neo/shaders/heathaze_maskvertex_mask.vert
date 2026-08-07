// Combined program for the two stock materials that pair the
// heatHazeWithMaskAndVertex.vfp *vertex* program with the heatHazeWithMask.vfp
// *fragment* program (textures/sfx/vppinch_bfgbolt, textures/sfx/vpsphere).
// This vertex half is identical to heathaze_maskvertex.vert; the paired
// fragment half (heathaze_maskvertex_mask.frag) is heathaze_mask.frag, which
// does NOT modulate the mask by vertex color — so var_Color is emitted here but
// left unread, which the SPIR-V/GLSL interface permits.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;
layout(location = 5) in vec4 attr_Color;

VARY(0) out vec2 var_TexMask;
VARY(1) out vec2 var_TexDistort;
VARY(2) out vec2 var_DeformMag;
VARY(3) out vec4 var_Color;

void main() {
	var_TexMask = attr_TexCoord;
	var_TexDistort = attr_TexCoord + u_localParam0.xy;

	float viewZ = dot( vec4( u_modelViewMatrix[0][2], u_modelViewMatrix[1][2],
	                         u_modelViewMatrix[2][2], u_modelViewMatrix[3][2] ),
	                   attr_Position );
	vec4 r0 = vec4( 1.0, 0.0, viewZ, 1.0 );

	float projX = dot( vec4( u_projectionMatrix[0][0], u_projectionMatrix[1][0],
	                         u_projectionMatrix[2][0], u_projectionMatrix[3][0] ), r0 );
	float projW = dot( vec4( u_projectionMatrix[0][3], u_projectionMatrix[1][3],
	                         u_projectionMatrix[2][3], u_projectionMatrix[3][3] ), r0 );

	projW = max( projW, 1.0 );
	float deform = min( projX / projW, 0.02 );

	var_DeformMag = vec2( deform * u_localParam1.x, deform * u_localParam1.y );

	var_Color = attr_Color;

	gl_Position = u_mvpMatrix * attr_Position;
}
