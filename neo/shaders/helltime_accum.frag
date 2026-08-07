// DUDE hell-time / Artifact vision (D3XP FullscreenFX_Helltime) — feedback accumulation.
// A faithful port of the stock recursive _accum zoom-feedback (materials/smf.mtr,
// textures/smf/bloodorb{1,2,3}/ac_capture + cr_capture), which can't accumulate on the RHI
// path for the same reason berserk's _scratch can't (documented, root-cause unknown). So the
// exact same fold runs here into a ping-pong render target instead — the reliable flavour of
// cross-frame feedback. This is berserk_accum.frag generalized with three per-level knobs:
// a tint multiply on the recirculated trail, an inverted radial mask, and a per-frame feedback
// rotation.
//
//   stock ac_capture: draw the previous _accum magnified about the centre (centerscale ~0.99,
//     + per-level rotate/tint), then maskcolor bloodorb3.tga writes its radial alpha.
//   stock cr_capture: blend _currentRender in weighted by that dst-alpha (fresh scene at the
//     centre where alpha is high, trail preserved at the edges where alpha is low).
//   Net: mix( currentScene, tint*prevMagnified, edgeWeight ), edgeWeight = (1 - mask.a).
//
// bloodorb3.tga alpha is ~1 at the centre and ~0 at the edges — the INVERSE of berserk2 — so
// we invert it (edge = 1 - mask) to keep the live scene sharp at the centre and the zoom-trail
// out at the edges (matching the stock dst-alpha gating). centerscale < 1 magnifies the sampled
// history, so features march outward and pile up as the languid hell-time smear.
//
//   unit 0 (u_scratch) = this frame's captured scene (_currentRender)
//   unit 1 (u_history) = the previous accumulation (other ping-pong slot)
//   unit 2 (u_mask)    = textures/smf/bloodorb3.tga (alpha = the radial gate)
//   u_localParam0 = ( centerScale, feedbackStrength, historyValid, maskInvert )
//   u_localParam1 = ( cos(rotPerFrame), sin(rotPerFrame), flipY, - )
//   u_color.rgb   = per-level tint applied to the recirculated trail
//
// flipY (1 on Vulkan, 0 on GL): identical to berserk_accum — _currentRender is captured
// top-down on Vulkan but this pass writes through a flipY viewport, so we flip the working
// coordinate to keep the whole trail in _currentRender space (no per-frame mirror build-up).

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_scratch;
SAMPLER_BINDING(1) uniform sampler2D u_history;
SAMPLER_BINDING(2) uniform sampler2D u_mask;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	const vec2 center = vec2( 0.5, 0.5 );

	// work in _currentRender space (see header): Vulkan flips the working coord so the trail
	// stays in the same orientation as the capture and the feedback stays coherent.
	vec2 q = var_TexCoord;
	if ( u_localParam1.z > 0.5 ) {
		q.y = 1.0 - q.y;
	}

	// _currentRender is POT-oversized: the live scene occupies only [0..u_screenCorrection.xy] of
	// the texture. Scale the read so the trail RT's full 0..1 holds the whole scene (un-squashed),
	// which puts the radial mask/zoom pivot at the true screen centre. u_history (the trail, a
	// full-size RT) and u_mask (a normal texture) stay in plain 0..1 space.
	vec4 cur = texture( u_scratch, q * u_screenCorrection.xy );

	// first frame after (re)allocation / re-entry: no usable history, seed with the clean scene.
	if ( u_localParam0.z < 0.5 ) {
		fragColor = cur;
		return;
	}

	// previous frame rotated (per-frame increment, accumulates through the recursion) then
	// magnified about the centre (centerscale -> sample the history pulled toward the centre so
	// its content grows out). Level 0 (Artifact) uses no rotation; levels 1/2 spin slowly.
	vec2 d = q - center;
	vec2 rd = vec2( d.x * u_localParam1.x - d.y * u_localParam1.y,
	                d.x * u_localParam1.y + d.y * u_localParam1.x );
	vec2 zoomUV = rd * u_localParam0.x + center;
	vec4 prev = texture( u_history, zoomUV );
	prev.rgb *= u_color.rgb;					// per-level tint on the trail

	// radial gate from bloodorb3 alpha; helltime inverts it (edge feedback, sharp centre).
	float m = texture( u_mask, q ).a;
	float edge = ( u_localParam0.w > 0.5 ) ? ( 1.0 - m ) : m;
	float a = clamp( edge * u_localParam0.y, 0.0, 1.0 );

	// gl_dst_alpha, gl_one_minus_dst_alpha  ->  prev*a + cur*(1-a)
	fragColor = mix( cur, prev, a );
}
