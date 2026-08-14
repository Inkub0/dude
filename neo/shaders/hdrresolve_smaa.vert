// DUDE fused SMAA resolve (docs/antialiasing.md, docs/gpu-offload-plan.md fps levers). The SMAA 1x
// neighborhood-blending vertex stage — identical to smaa_blend.vert — paired with the resolve's
// grain/gamma tail in hdrresolve_smaa.frag. Folding the blend into the HDR resolve lets the
// anti-aliased float scene go straight to the 8-bit backbuffer in one pass, dropping the separate
// rhiHdrAaRT round-trip. u_localParam0 = SMAA_RT_METRICS (1/w, 1/h, w, h).

#include "renderparms.glsl"

#define SMAA_GLSL_3 1
#define SMAA_RT_METRICS u_localParam0
#define SMAA_PRESET_HIGH 1
#define SMAA_INCLUDE_PS 0
#include "smaa.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;

VARY(0) out vec2 var_TexCoord;
VARY(1) out vec4 var_SmaaOffset;

void main() {
	var_TexCoord = attr_TexCoord;
	SMAANeighborhoodBlendingVS( attr_TexCoord, var_SmaaOffset );
	gl_Position = u_mvpMatrix * attr_Position;
}
