// DUDE berserk vision — feedback accumulation, a faithful port of the stock ARB
// material textures/decals/berserk (materials/decals.mtr). That material can't
// accumulate on the RHI path because its recursive _scratch capture doesn't feed back,
// so the exact same two operations run here into a ping-pong render target instead:
//
//   stock stage 0 (maskcolor, map berserk2, rotate time*3, translate 0.1): writes the
//     rotating radial mask texture's ALPHA to the destination alpha. berserk2's alpha is
//     a radial gradient — ~0 at the centre, ~0.8 at the edges.
//   stock stage 1 (blend gl_dst_alpha,gl_one_minus_dst_alpha; map _scratch;
//     centerscale 0.97): out = _scratch_prev*dstAlpha + scene*(1-dstAlpha), with the
//     previous frame magnified ~3% about the centre.
//
// Net: mix( currentScene, prevFrameMagnified, maskAlpha ). Low alpha at the centre keeps
// the live scene sharp there; high alpha at the edges recirculates the magnified previous
// frame, so features march outward and pile up as the radial "streak zoom".
//
//   unit 0 (u_scratch) = this frame's captured scene (_scratch)
//   unit 1 (u_history) = the previous accumulation (other ping-pong slot)
//   unit 2 (u_mask)    = textures/decals/berserk2 (alpha = the radial gate)
//   u_localParam0 = ( centerScale, feedbackStrength, historyValid, - )
//   u_localParam1 = ( cos(maskAngle), sin(maskAngle), flipY, - )
//
// flipY (1 on Vulkan, 0 on GL): _scratch is captured top-down on Vulkan but this pass writes
// through a flipY viewport, so a plain read-at-texcoord / write-flipped fullscreen pass adds
// one vertical flip per frame — the feedback would accumulate scene + flip(scene) (a mirror).
// We work in "_scratch space" by flipping the working coordinate on Vulkan, so the whole trail
// (scene inject, history feedback and output) stays in the same orientation as _scratch, and
// the display quad — already correct for _scratch — shows it right on both backends. GL is
// already consistent (no flipY), so flipY = 0 leaves it untouched.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_scratch;
SAMPLER_BINDING(1) uniform sampler2D u_history;
SAMPLER_BINDING(2) uniform sampler2D u_mask;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	const vec2 center = vec2( 0.5, 0.5 );

	// work in _scratch space (see header): Vulkan flips the working coord so the trail
	// ends up in the same orientation as _scratch and the history feedback stays coherent.
	vec2 q = var_TexCoord;
	if ( u_localParam1.z > 0.5 ) {
		q.y = 1.0 - q.y;
	}

	// _scratch is a POT full-res capture; the scene fills only [0,shiftScale] of it, so
	// screen-correct the scene sample (localParam0.w / localParam1.w). The history + mask
	// stay in the trail's full [0,1] space below.
	vec4 cur = texture( u_scratch, q * vec2( u_localParam0.w, u_localParam1.w ) );

	// first frame after (re)allocation: no usable history, seed with the clean scene.
	if ( u_localParam0.z < 0.5 ) {
		fragColor = cur;
		return;
	}

	// stock stage 1: previous frame magnified ~3% about the centre (centerscale 0.97 ->
	// sample the history at texcoords pulled toward the centre, so its content grows out).
	vec2 zoomUV = ( q - center ) * u_localParam0.x + center;
	vec4 prev = texture( u_history, zoomUV );

	// stock stage 0: berserk2 alpha as the dst-alpha gate, rotated about the centre and
	// nudged by translate 0.1 (clamp addressing at the edges, like the material).
	vec2 d = q - center;
	vec2 maskUV = vec2( d.x * u_localParam1.x - d.y * u_localParam1.y,
	                    d.x * u_localParam1.y + d.y * u_localParam1.x ) + center + vec2( 0.1, 0.1 );
	float a = texture( u_mask, maskUV ).a * u_localParam0.y;

	// gl_dst_alpha, gl_one_minus_dst_alpha  ->  prev*a + cur*(1-a)
	fragColor = mix( cur, prev, a );
}
