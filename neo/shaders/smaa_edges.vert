// DUDE SMAA 1x pass 1: luma edge detection (docs/antialiasing.md).
// Thin wrapper over the vendored reference implementation (smaa.glsl).
// u_localParam0 = SMAA_RT_METRICS (1/w, 1/h, w, h) of the exact-size scene.

#include "renderparms.glsl"

#define SMAA_GLSL_3 1
#define SMAA_RT_METRICS u_localParam0
#define SMAA_PRESET_HIGH 1
#define SMAA_INCLUDE_PS 0
#include "smaa.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;

VARY(0) out vec2 var_TexCoord;
VARY(1) out vec4 var_SmaaOffset[3];

void main() {
	var_TexCoord = attr_TexCoord;
	SMAAEdgeDetectionVS( attr_TexCoord, var_SmaaOffset );
	gl_Position = u_mvpMatrix * attr_Position;
}
