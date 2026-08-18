// HDR eye adaptation (Phase B1, docs/hdr-pipeline.md): compute the adapted exposure into a
// 1x1 ping-pong target. Reads the 1x1 geometric-mean scene luminance and the previous frame's
// exposure, and eases toward a target — the temporal lag IS the eye-adaptation feel. The resolve
// then samples this 1x1 as its exposure instead of the static r_hdrExposure.
//
// Relative model (no hard clamp): r_hdrExposure is the neutral "mid" exposure at the reference
// luminance (key). How many stops the scene sits from the reference maps through a tanh, so as the
// scene darkens the exposure eases UP toward mid + Brighten, and as it brightens it eases DOWN
// toward mid - Darken — self-limiting instead of pinning.
//
//   u_localParam0.x = r_hdrExposure (neutral "mid" exposure)
//   u_localParam0.y = Brighten (max exposure ADDED as the scene darkens)
//   u_localParam0.z = Darken   (max exposure REMOVED as the scene brightens)
//   u_localParam0.w = blend alpha = 1 - exp(-dt / tau)  (0 = frozen, 1 = snap)
//   u_localParam1.x = previous exposure usable (1) or snap to target (0, first frame / reset)
//   u_localParam1.y = luma source mip level (Vulkan single-level view -> 0; GL3 -> coarsest)
//   u_localParam1.z = reference luminance (r_hdrAdaptKey): the scene luminance mapping to mid

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_lumaAvg;       // 1x1 average log-luma (coarsest luma mip)
SAMPLER_BINDING(1) uniform sampler2D u_prevExposure;  // 1x1 previous adapted exposure

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

const float WIDTH = 2.5;   // stops of scene brightness that fill the tanh response (softness)

void main() {
	int   lod  = int( u_localParam1.y + 0.5 );
	float logL = texelFetch( u_lumaAvg, ivec2( 0 ), lod ).r;
	float L    = exp( logL );                                     // geometric-mean luminance

	float mid      = u_localParam0.x;
	float brighten = u_localParam0.y;
	float darken   = u_localParam0.z;
	float Lref     = max( u_localParam1.z, 1e-4 );

	// stops from the reference: >0 brighter than neutral, <0 darker. tanh( -stops/width ) is +1 in
	// the dark (add Brighten) and -1 in the bright (subtract Darken), easing smoothly between.
	float stops = log2( max( L, 1e-4 ) / Lref );
	float t     = tanh( -stops / WIDTH );                         // [-1, 1], + = darker scene
	float amp   = ( t >= 0.0 ) ? brighten : darken;
	float target = max( mid + amp * t, 0.05 );                    // floor keeps exposure positive

	float prev = texelFetch( u_prevExposure, ivec2( 0 ), 0 ).r;
	float adapted = ( u_localParam1.x > 0.5 ) ? mix( prev, target, u_localParam0.w ) : target;
	// how far into the low-light brightening we are (0 at neutral, ~1 at the Brighten cap). The
	// resolve reads this to drive the low-light grain boost AND the low-light desaturation.
	float brightenFrac = clamp( ( adapted - mid ) / max( brighten, 1e-3 ), 0.0, 1.0 );
	// .r = adapted exposure (what the resolve uses); .g = log-luma (debug); .b = brighten fraction.
	fragColor = vec4( adapted, logL, brightenFrac, 1.0 );
}
