// DUDE RT GI history-confidence pass (docs/rtx-rtgi.md H6f).
//
// Feeds NRD's IN_DIFF_CONFIDENCE (0..1 per pixel, 1 = trust history, 0 = reject = antilag). NRD's own
// depth/normal disocclusion handles GEOMETRY changes, but not LIGHTING changes that leave geometry intact:
// a lit silhouette sliding under the camera, a light toggling. There the reprojected history is stale but
// passes NRD's geometric test, so ReBLUR keeps accumulating it -> the bright bounce trails the edge (the
// "smearing" of lit elements under small mouselook). This pass detects exactly that: reproject last frame's
// DENOISED GI by the motion vectors, compare its luminance to THIS frame's bounce luminance, and where they
// disagree drop the confidence so NRD rejects history at that pixel only. No flat-area dimming (a global
// antilag rejects everywhere and starves the 1-spp signal); this rejects only where the light actually moved.
//
// Runs at the GI gather/denoise resolution, BEFORE the denoise (which then overwrites u_prevGI). Both the raw
// gather and the denoised output are NRD-YCoCg, so .x is already the luminance (Y) channel - no decode needed.
//
//   u_currentGI (unit 0)  = this frame's raw gather radiance (rtgiRayRT att0), YCoCg
//   u_prevGI    (unit 1)  = last frame's DENOISED GI (GetRtgiOutputImage), YCoCg - still valid this early
//   u_velocity  (unit 2)  = motion vectors at this resolution (currUV - prevUV, +Y up), same as ssr_temporal
//   u_localParam0.x       = sensitivity (r_rtgiConfidence); u_localParam0.y = confidence floor
//   u_localParam0.z       = noise deadzone: relative differences below this are treated as 1-spp noise, not a
//                           real lighting change, so STABLE surfaces keep full confidence (no flat dimming)
//   u_localParam0.w       = H6g checkerboard parity: 0 off, 1 even, 2 odd. When on, only this frame's traced
//                           cells get a computed confidence (read from the packed left half); the rest = 1
//   u_screenCorrection.xy = 1 / (rw, rh) texel size (3x3 tap of the noisy current signal)
//   u_windowCoord.z       = view-Y sign (-1 Vulkan top-down): flip the velocity row like ssr_temporal

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentGI;
SAMPLER_BINDING(1) uniform sampler2D u_prevGI;
SAMPLER_BINDING(2) uniform sampler2D u_velocity;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;	// R8 confidence 0..1

void main() {
	vec2 uv = var_TexCoord;

	// this frame's bounce luminance.
	vec2  texel  = u_screenCorrection.xy;
	int   cbMode = int( u_localParam0.w + 0.5 );		// H6g: 0 = off, 1 = even frame, 2 = odd frame
	float curL;
	if ( cbMode > 0 ) {
		// checkerboard: only THIS frame's traced cells have a fresh sample (the gather packed them into the
		// left half). Reconstructed cells have no new info this frame, so trust history there (confidence 1).
		float px = floor( gl_FragCoord.x ), py = floor( gl_FragCoord.y );
		uint  traced = ( ( uint( px ) ^ uint( py ) ) ^ uint( cbMode - 1 ) ) & 1u;	// 0 = traced (matches the gather's BLACK cells)
		if ( traced != 0u ) { fragColor = vec4( 1.0 ); return; }
		float packedCol = floor( px * 0.5 );			// screen 2*gx+parity was packed into column gx = px>>1
		curL = max( texture( u_currentGI, vec2( ( packedCol + 0.5 ) * texel.x, ( py + 0.5 ) * texel.y ) ).x, 0.0 );
	} else {
		// resScale: the 1-spp gather is noisy, so average a 3x3 neighbourhood (GI is very low-frequency, so
		// this barely blurs) to keep the confidence itself from flickering pixel to pixel.
		curL = 0.0;
		for ( int j = -1; j <= 1; j++ ) {
			for ( int i = -1; i <= 1; i++ ) {
				curL += max( texture( u_currentGI, uv + vec2( float( i ), float( j ) ) * texel ).x, 0.0 );
			}
		}
		curL *= ( 1.0 / 9.0 );
	}

	// reproject to last frame (ssr_temporal.frag convention: velocity = currUV - prevUV in +Y-up UV, flip
	// the row on Vulkan's top-down target).
	vec2 vel = texture( u_velocity, uv ).rg;
	vel.y = ( u_windowCoord.z < 0.0 ) ? -vel.y : vel.y;
	vec2 prevUV = uv - vel;

	float floorC = u_localParam0.y;
	float conf;
	if ( any( lessThan( prevUV, vec2( 0.0 ) ) ) || any( greaterThan( prevUV, vec2( 1.0 ) ) ) ) {
		conf = floorC;										// left the screen last frame: no history to trust
	} else {
		float prevL = max( texture( u_prevGI, prevUV ).x, 0.0 );
		// relative luminance change: 0 when the bounce is unchanged (trust history), ->1 when it jumped
		// (a light moved onto/off this surface). eps keeps dark-on-dark from reading as a big change.
		float d = abs( curL - prevL ) / ( max( curL, prevL ) + 1.0e-3 );
		// deadzone: the current signal is a noisy 1-spp gather, so on a STABLE surface it still differs from
		// the smooth denoised history by the noise level. Subtract that off so only genuine lighting changes
		// (well above the noise) drop confidence — otherwise we'd starve history everywhere (flat dimming).
		d = max( d - u_localParam0.z, 0.0 );
		conf = exp( -u_localParam0.x * d );
		conf = clamp( conf, floorC, 1.0 );					// never fully reject: a floor of history avoids edge "frying"
	}
	fragColor = vec4( conf );
}
