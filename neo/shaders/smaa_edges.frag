// DUDE SMAA 1x pass 1: luma edge detection over the scene (smaa.glsl).
// Writes the RG edge mask; discards where there is no edge, so the target
// must be cleared to 0 by the caller. On the HDR rail the scene is a float
// buffer whose >1 highlights merely over-detect edges (harmless: SMAA then
// blends pixels that barely needed it).

#include "renderparms.glsl"

#define SMAA_GLSL_3 1
#define SMAA_RT_METRICS u_localParam0
#define SMAA_PRESET_HIGH 1
#define SMAA_INCLUDE_VS 0
#include "smaa.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_sceneTex;

VARY(0) in vec2 var_TexCoord;
VARY(1) in vec4 var_SmaaOffset[3];

layout(location = 0) out vec4 fragColor;

void main() {
	fragColor = vec4( SMAALumaEdgeDetectionPS( var_TexCoord, var_SmaaOffset, u_sceneTex ), 0.0, 1.0 );
}
