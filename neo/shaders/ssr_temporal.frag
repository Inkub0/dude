// DUDE screen-space reflections — TEMPORAL accumulation (docs/ssr.md, Phase C.2.1).
// Blends this frame's marched reflections with the previous frame's result,
// reprojected by camera motion (static world -> a single view->prevClip matrix, the
// same recipe as ssao_temporal.frag). The per-frame march jitter (ssr.frag,
// u_windowCoord.y) makes successive frames sample different ray offsets, so the
// accumulation genuinely resolves the march grain instead of just smearing it.
// Ghosting is bounded by TAA-style VARIANCE CLIPPING (history clamped to the local
// 3x3 mean +- gamma*stddev) rather than a hard min/max clamp: reflections are a
// binary-ish signal (a jittered ray sometimes steps over a thin feature, swinging a
// pixel between bright hit and black miss), and a min/max clamp lets that flicker
// straight through — where the signal flickers the deviation is large, so the clip
// box widens and history survives; where it's stable the box is tight and ghosts
// die. A jittered MISS against an established reflection additionally keeps more
// history (hit-mask-aware feedback) instead of punching a one-frame hole.
//
//   unit 0 (u_curSSR)      = this frame's marched reflection (rgb + hit mask in a)
//   unit 1 (u_history)     = previous frame's accumulated result
//   unit 2 (u_currentDepth)= scene depth (view-pos reconstruction / reprojection)
//   u_localParam0 = ( 1/proj00, 1/proj11, feedback, historyValid )
//   u_modelViewMatrix     = reproj: current view space -> previous clip
//   u_screenCorrection.xy = 1 / ssrTargetSize      (gl_FragCoord -> [0,1] uv, texel size)
//   u_depthTexRecip.xy    = ratio / depthUploadSize (gl_FragCoord -> _currentDepth tc)

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_curSSR;
SAMPLER_BINDING(1) uniform sampler2D u_history;
SAMPLER_BINDING(2) uniform sampler2D u_currentDepth;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

// Doom 3's fixed near / near-infinite far projection -> linear eye z (same constants
// as ssao.frag / ssr.frag).
const vec2 depth_consts = vec2( 0.33333333, -0.33316667 );

void main() {
	vec2 frag = gl_FragCoord.xy;
	vec4 cur  = texture( u_curSSR, var_TexCoord );
	float raw = texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;

	// sky / no geometry: nothing to reproject, just pass the current value through
	if ( raw >= 0.9994 ) {
		fragColor = cur;
		return;
	}

	// view-space position from depth (identical reconstruction to ssr.frag)
	float vz  = 1.0 / ( raw * depth_consts.x + depth_consts.y );      // negative
	vec2  ndc = frag * ( u_screenCorrection.xy * 2.0 ) - 1.0;
	float d   = -vz;
	vec3  P   = vec3( ndc.x * d * u_localParam0.x, ndc.y * d * u_localParam0.y, vz );

	// reproject into the previous frame: current view space -> previous clip -> uv
	vec4 prevClip = u_modelViewMatrix * vec4( P, 1.0 );
	vec2 prevUV   = ( prevClip.xy / prevClip.w ) * 0.5 + 0.5;

	bool valid = u_localParam0.w > 0.5 && prevClip.w > 0.0
	          && all( greaterThanEqual( prevUV, vec2( 0.0 ) ) )
	          && all( lessThanEqual(    prevUV, vec2( 1.0 ) ) );

	if ( !valid ) {
		fragColor = cur;      // first frame / off-screen: fall back to current, no ghost
		return;
	}

	// variance clipping: first + second moments of the 3x3 current-frame
	// neighbourhood; the clip box is mean +- gamma*stddev per channel
	vec2 texel = u_screenCorrection.xy;
	vec4 m1 = cur;
	vec4 m2 = cur * cur;
	for ( int y = -1; y <= 1; ++y ) {
		for ( int x = -1; x <= 1; ++x ) {
			if ( x == 0 && y == 0 ) {
				continue;			// center tap is cur, already seeded above
			}
			vec4 s = texture( u_curSSR, var_TexCoord + vec2( x, y ) * texel );
			m1 += s;
			m2 += s * s;
		}
	}
	m1 *= ( 1.0 / 9.0 );
	m2 *= ( 1.0 / 9.0 );
	vec4 sigma = sqrt( max( m2 - m1 * m1, vec4( 0.0 ) ) );
	const float gamma = 1.25;
	vec4 hist = clamp( texture( u_history, prevUV ), m1 - gamma * sigma, m1 + gamma * sigma );

	// exponential accumulation: feedback = fraction of history kept per frame. A
	// jittered miss (no hit this frame) against an established reflection keeps the
	// history harder — the miss is usually the jitter stepping over a thin feature,
	// not the reflection disappearing. Real disocclusions are still killed by the
	// clip box: when the whole neighbourhood goes dark, mean and deviation collapse
	// and the clamped history is already ~0 before this blend.
	float feedback = u_localParam0.z;
	if ( cur.a < 0.5 && hist.a > 0.5 ) {
		feedback = max( feedback, 0.96 );
	}
	fragColor = mix( cur, hist, feedback );
}
