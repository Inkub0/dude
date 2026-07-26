// DUDE GTAO bilateral denoise (docs/ssao-gtao.md). SEPARABLE depth-aware blur: one
// horizontal then one vertical pass (2 * (2R+1) taps instead of (2R+1)^2), removing
// the per-pixel horizon-search noise without bleeding across depth discontinuities.
// Blurs the AO scalar (R) and the bent normal (GBA) together, then renormalizes the
// bent normal. No temporal term (by design — the backend has no TAA to hang one on).
//
//   u_localParam0.x       = axis (0 = horizontal, 1 = vertical)
//   u_screenCorrection.xy = 1 / aoTargetSize      (texel size / gl_FragCoord -> uv)
//   u_depthTexRecip.xy    = ratio / depthUploadSize (gl_FragCoord -> _currentDepth tc)

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_ssao;          // AO + bent normal (this axis' input)
SAMPLER_BINDING(1) uniform sampler2D u_currentDepth;  // for edge-stopping weights

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

#define BLUR_RADIUS 2

const vec2 depth_consts = vec2( 0.33333333, -0.33316667 );

float linDepth( vec2 frag ) {
	float raw = min( texture( u_currentDepth, frag * u_depthTexRecip.xy ).x, 0.9994 );
	return 1.0 / ( raw * depth_consts.x + depth_consts.y );       // negative
}

void main() {
	vec2  frag    = gl_FragCoord.xy;
	vec2  texel   = u_screenCorrection.xy;
	float centerZ = linDepth( frag );

	// blur along one axis only (separable); the CPU issues an H then a V pass
	vec2 uvStep  = ( u_localParam0.x < 0.5 ) ? vec2( texel.x, 0.0 ) : vec2( 0.0, texel.y );
	vec2 pixStep = ( u_localParam0.x < 0.5 ) ? vec2( 1.0, 0.0 )     : vec2( 0.0, 1.0 );

	vec4  sum  = vec4( 0.0 );
	float wsum = 0.0;
	for ( int i = -BLUR_RADIUS; i <= BLUR_RADIUS; ++i ) {
		float fi = float( i );
		vec4  s  = texture( u_ssao, var_TexCoord + uvStep * fi );
		float z  = linDepth( frag + pixStep * fi );
		// edge-stopping: relative depth difference (scale-free), plus a mild spatial
		// falloff along the axis. exp(a)*exp(b) folded into one exp(a+b).
		float dzr = ( z - centerZ ) / max( abs( centerZ ), 1.0 );
		float w   = exp( -( dzr * dzr * 800.0 + fi * fi * 0.25 ) );
		sum  += s * w;
		wsum += w;
	}

	vec4 avg = ( wsum > 0.0 ) ? ( sum / wsum ) : texture( u_ssao, var_TexCoord );

	vec3  bn = avg.gba * 2.0 - 1.0;
	float bl = length( bn );
	bn = ( bl > 1e-4 ) ? ( bn / bl ) : vec3( 0.0 );

	fragColor = vec4( avg.r, bn * 0.5 + 0.5 );
}
