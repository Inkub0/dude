// HDR eye adaptation (Phase B1, docs/hdr-pipeline.md): compute the adapted exposure into a
// 1x1 ping-pong target. Reads the 1x1 geometric-mean scene luminance (exp of the averaged
// log-luma) and the previous frame's exposure, targets a mid-gray key, clamps to [min,max],
// and eases toward it — the temporal lag IS the eye-adaptation feel. The resolve then samples
// this 1x1 as its exposure instead of the static r_hdrExposure.
//
//   u_localParam0.x = r_hdrExposure (the exposure at a mid-gray ~0.18 scene)
//   u_localParam0.y = min exposure clamp
//   u_localParam0.z = max exposure clamp
//   u_localParam0.w = blend alpha = 1 - exp(-dt / tau)  (0 = frozen, 1 = snap)
//   u_localParam1.x = previous exposure usable (1) or snap to target (0, first frame / reset)
//   u_localParam1.y = luma source mip level (Vulkan single-level view -> 0; GL3 -> coarsest)

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_lumaAvg;       // 1x1 average log-luma (coarsest luma mip)
SAMPLER_BINDING(1) uniform sampler2D u_prevExposure;  // 1x1 previous adapted exposure

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

const float MIDGRAY = 0.18;

void main() {
	int   lod  = int( u_localParam1.y + 0.5 );
	float logL = texelFetch( u_lumaAvg, ivec2( 0 ), lod ).r;
	float L    = exp( logL );                                     // geometric-mean luminance
	// expose so a mid-gray scene lands at r_hdrExposure; brighter scenes expose down, darker up.
	float target = clamp( u_localParam0.x * MIDGRAY / max( L, 1e-4 ),
	                      u_localParam0.y, u_localParam0.z );
	float prev = texelFetch( u_prevExposure, ivec2( 0 ), 0 ).r;
	float adapted = ( u_localParam1.x > 0.5 ) ? mix( prev, target, u_localParam0.w ) : target;
	fragColor = vec4( adapted, 0.0, 0.0, 1.0 );
}
