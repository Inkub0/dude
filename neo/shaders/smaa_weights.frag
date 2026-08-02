// DUDE SMAA 1x pass 2: blending-weight calculation (smaa.glsl).
// Reads the edge mask plus the two precomputed SMAA lookup textures
// (AreaTex on unit 1, SearchTex on unit 2, uploaded from the vendored
// byte arrays in neo/renderer/rhi/smaa/). Target cleared to 0 by the
// caller. subsampleIndices is zero for single-frame SMAA 1x.

#include "renderparms.glsl"

#define SMAA_GLSL_3 1
#define SMAA_RT_METRICS u_localParam0
#define SMAA_PRESET_HIGH 1
#define SMAA_INCLUDE_VS 0
#include "smaa.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_edgesTex;
SAMPLER_BINDING(1) uniform sampler2D u_areaTex;
SAMPLER_BINDING(2) uniform sampler2D u_searchTex;

VARY(0) in vec2 var_TexCoord;
VARY(1) in vec2 var_SmaaPixcoord;
VARY(2) in vec4 var_SmaaOffset[3];

layout(location = 0) out vec4 fragColor;

void main() {
	fragColor = SMAABlendingWeightCalculationPS( var_TexCoord, var_SmaaPixcoord, var_SmaaOffset,
	                                             u_edgesTex, u_areaTex, u_searchTex, vec4( 0.0 ) );
}
