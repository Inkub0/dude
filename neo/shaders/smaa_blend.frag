// DUDE SMAA 1x pass 3: neighborhood blending (smaa.glsl). Resolves the
// final anti-aliased image from the scene and the blending weights; pixels
// with zero weights pass through untouched, so the image is bit-identical
// to the input wherever no edge was detected.

#include "renderparms.glsl"

#define SMAA_GLSL_3 1
#define SMAA_RT_METRICS u_localParam0
#define SMAA_PRESET_HIGH 1
#define SMAA_INCLUDE_VS 0
#include "smaa.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_sceneTex;
SAMPLER_BINDING(1) uniform sampler2D u_blendTex;

VARY(0) in vec2 var_TexCoord;
VARY(1) in vec4 var_SmaaOffset;

layout(location = 0) out vec4 fragColor;

void main() {
	fragColor = SMAANeighborhoodBlendingPS( var_TexCoord, var_SmaaOffset, u_sceneTex, u_blendTex );
}
