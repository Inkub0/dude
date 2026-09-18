// SSR Hi-Z (docs/ssao-perf-optimization.md, r_ssrHiZ): linearize the captured projection
// depth (_currentDepth) into positive view-space eye depth, written to R of the SSR-res
// min-Z mip-chain level 0. ssr_depthdown then MIN-downsamples this down the chain (the
// opposite filter to SSAO's max: the nearest surface must never be leapt over). A linear
// depth is required for a meaningful reduction (raw projection depth is affine in 1/z, so
// a 2x2 reduction of raw depth is not a depth) — that is why this pass exists. The march
// in ssr.frag samples a coarse level to leap provably-empty span (see hiZMin there).
// Byte-identical to ssao_depthmip.frag — kept separate so the two chains stay decoupled.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentDepth;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

// same near / near-infinite far GL clip-depth -> linear eye-z constants ssao.frag uses
// depth -> view z pair of THIS view (renderparms.glsl u_depthParms): the game lowers the near plane
// for cinematic cameras, so it is not the constant ( 0.33333333, -0.33316667 ) it used to be hard-coded as
#define depth_consts DUDE_DEPTH_CONSTS()

void main() {
	// gl_FragCoord is at the AO-buffer resolution; u_depthTexRecip.xy maps it into the
	// POT _currentDepth texcoords exactly as ssao.frag's rawDepth() does.
	vec2  frag = gl_FragCoord.xy;
	float raw  = texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;
	// vz is negative (eye looks down -z). Store positive linear depth: it keeps half-float
	// precision densest near the camera (where AO matters most) and averages cleanly.
	float vz   = 1.0 / ( min( raw, 0.9994 ) * depth_consts.x + depth_consts.y );
	fragColor  = vec4( -vz, 0.0, 0.0, 1.0 );
}
