// SSAO Phase 1 (docs/ssao-perf-optimization.md): linearize the captured projection
// depth (_currentDepth) into positive view-space eye depth, written to R of the AO-res
// mip-chain level 0. ssao_depthdown then max-downsamples this down the chain. A linear
// depth is required for a meaningful reduction (raw projection depth is affine in 1/z,
// so a 2x2 reduction of raw depth is not a depth) — that is why this pass exists.
// ssao.frag reads a coarser mip for farther horizon steps: far taps then touch a small,
// cache-local footprint instead of scattering across full-res _currentDepth.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentDepth;
// normal G-buffer, for the weapon/AO mask in .a (gbuffer.frag writes 0 on the view weapon).
// Bound only when one exists this view (u_windowCoord.x > 0.5); a dummy otherwise, unread.
SAMPLER_BINDING(1) uniform sampler2D u_normalBuffer;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

// same near / near-infinite far GL clip-depth -> linear eye-z constants ssao.frag uses
// depth -> view z pair of THIS view (renderparms.glsl u_depthParms): cinematics quarter the near
// plane, so it is not the constant ( 0.33333333, -0.33316667 ) it used to be hard-coded as
#define depth_consts DUDE_DEPTH_CONSTS()

void main() {
	// gl_FragCoord is at the AO-buffer resolution; u_depthTexRecip.xy maps it into the
	// POT _currentDepth texcoords exactly as ssao.frag's rawDepth() does.
	vec2  frag = gl_FragCoord.xy;

	// Weapon-mask bake: the view weapon must never occlude — its depth-hacked, pulled-close
	// depth casts a false AO halo on the world behind it that slides as the gun sways.
	// Weapon texels become far depth in the whole chain, which the march's radius falloff
	// zeroes exactly like the old per-tap skip; ssao.frag can then drop its per-tap
	// full-res weaponTexel() fetch, the scattered read pattern this mip exists to remove.
	// 60000 sits past the ~30000 sky depth and inside R16F range (max 65504).
	if ( u_windowCoord.x > 0.5 && texture( u_normalBuffer, frag * u_screenCorrection.xy ).a < 0.5 ) {
		fragColor = vec4( 60000.0, 0.0, 0.0, 1.0 );
		return;
	}

	float raw  = texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;
	// vz is negative (eye looks down -z). Store positive linear depth: it keeps half-float
	// precision densest near the camera (where AO matters most) and averages cleanly.
	float vz   = 1.0 / ( min( raw, 0.9994 ) * depth_consts.x + depth_consts.y );
	fragColor  = vec4( -vz, 0.0, 0.0, 1.0 );
}
