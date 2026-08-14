// SSAO Phase 1 (docs/ssao-perf-optimization.md): linearize the captured projection
// depth (_currentDepth) into positive view-space eye depth, written to R of the AO-res
// mip-chain level 0. ssao_depthdown then max-downsamples this down the chain. A linear
// depth is required for a meaningful reduction (raw projection depth is affine in 1/z,
// so a 2x2 reduction of raw depth is not a depth) — that is why this pass exists.
// ssao.frag reads a coarser mip for farther horizon steps: far taps then touch a small,
// cache-local footprint instead of scattering across full-res _currentDepth.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentDepth;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

// same near / near-infinite far GL clip-depth -> linear eye-z constants ssao.frag uses
const vec2 depth_consts = vec2( 0.33333333, -0.33316667 );

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
