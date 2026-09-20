// DUDE volumetric lighting (docs/volumetric-lighting.md): depth-aware box blur of the LOW-RES march, before
// the upsample. The march offsets its sample depths per pixel with interleaved gradient noise, whose point is
// that every 3x3 neighbourhood holds well-spread offsets - so this blur is what turns the per-pixel dither
// into ~9x (radius 1) or ~25x (radius 2) the effective step count. Without it each noisy low-res texel is
// upsampled into a 4x4-pixel dot that the temporal pass alone (a handful of frames at a responsive
// feedback) cannot hide. Depth-aware so a shaft in front of a far wall does not smear onto a near railing.
//   unit 0 = low-res march (rgb in-scatter, a transmittance), unit 1 = scene depth
//   u_localParam0      = (lowW, lowH, full px per low-res texel, radius in low-res texels)
//   u_depthTexRecip.xy = full-res px -> depth-texture tc
#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_volume;
SAMPLER_BINDING(1) uniform sampler2D u_currentDepth;

VARY(0) in vec2 var_TexCoord;
layout(location = 0) out vec4 fragColor;

#define depth_consts DUDE_DEPTH_CONSTS()

float viewDepthLow( vec2 lowTexel ) {										// depth the march of that low-res pixel ran to
	float raw = texture( u_currentDepth, ( lowTexel + 0.5 ) * u_localParam0.z * u_depthTexRecip.xy ).x;
	return -1.0 / ( raw * depth_consts.x + depth_consts.y );
}

void main() {
	vec2  lowSize = u_localParam0.xy;
	int   radius  = int( u_localParam0.w );
	vec2  c       = floor( gl_FragCoord.xy );
	float zc      = viewDepthLow( c );
	vec4  sum     = vec4( 0.0 );
	float wsum    = 0.0;
	for ( int j = -radius; j <= radius; j++ ) {
		for ( int i = -radius; i <= radius; i++ ) {
			vec2  t  = clamp( c + vec2( i, j ), vec2( 0.0 ), lowSize - 1.0 );
			float zt = viewDepthLow( t );
			// Reject only a real SILHOUETTE (a near railing against a far wall: depth ratio >> 2), never a
			// slope: on a grazing cave wall / receding floor adjacent march texels differ by 20-40 % in
			// depth, and a tight tolerance switched the blur off over the whole area - the raw dither
			// (interleaved gradient noise is visibly STRIPED un-blurred) showed through as a banded patch.
			// The haze in front of a surface barely depends on that surface anyway.
			float rel = abs( zt - zc ) / ( min( zt, zc ) + 16.0 );
			float w   = 1.0 - smoothstep( 0.6, 1.2, rel );
			w = max( w, ( i == 0 && j == 0 ) ? 1.0 : 0.0 );
			sum  += texture( u_volume, ( t + 0.5 ) / lowSize ) * w;
			wsum += w;
		}
	}
	fragColor = sum / wsum;
}
