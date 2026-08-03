// Translated from glprogs/heatHaze.vfp (vertex program).
// Screen-space refraction: scroll the distortion normal map, and scale the
// deform magnitude by projection distance. The deform math must match the
// ARB original exactly (project (1, 0, eyeZ, 1) through projection rows 0
// and 3) — an earlier translation used the wrong rows and the magnitude
// exploded at some view angles, smearing glass into an opaque liquid warp.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;

VARY(0) out vec2 var_TexDistort;  // texcoord[1]: scrolled normal map coords
VARY(1) out vec2 var_DeformMag;   // texcoord[2]

void main() {
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

	gl_Position = u_mvpMatrix * attr_Position;
}
