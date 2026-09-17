// DUDE RTAO resolve pass (docs/rtx-rtao.md H4d): composite NRD's denoised occlusion into
// the STANDARD AO buffer the lighting already consumes — RGBA8 with visibility in R
// (intensity-curved exactly like ssao.frag's output) and a view-space normal in GBA.
// RTAO has no bent normal (one cosine ray, not a horizon integral), so GBA carries the
// G-buffer's surface normal: the ambient's bent-normal lerp degrades to a no-op and every
// consumer (ambientlight, interactions, r_ssaoDebug) works unchanged. Sky and the view
// weapon read fully unoccluded, mirroring the GTAO early-outs.
//
//   u_localParam0.x       = r_ssaoIntensity (pow curve on the visibility)
//   u_screenCorrection.xy = 1 / viewSize   (gl_FragCoord -> [0,1] uv)
//   u_depthTexRecip.xy    = gl_FragCoord -> _currentDepth tc (sky check)

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_rtao;			// NRD OUT_DIFF_HITDIST (denoised occlusion)
SAMPLER_BINDING(1) uniform sampler2D u_normalBuffer;	// xyz = view-space normal, a = weapon mask
SAMPLER_BINDING(2) uniform sampler2D u_currentDepth;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	vec2  frag = gl_FragCoord.xy;
	vec4  nt   = texture( u_normalBuffer, frag * u_screenCorrection.xy );
	float raw  = texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;

	float vis = 1.0;
	if ( raw < 0.9994 && nt.a >= 0.5 ) {				// not sky, not the weapon
		vis = clamp( texture( u_rtao, frag * u_screenCorrection.xy ).r, 0.0, 1.0 );
		vis = pow( vis, max( u_localParam0.x, 0.0 ) );	// intensity, same curve as ssao.frag
	}
	fragColor = vec4( vis, nt.xyz );
}
