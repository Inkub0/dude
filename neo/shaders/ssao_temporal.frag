// DUDE GTAO temporal accumulation (docs/ssao-gtao.md). Blends this frame's denoised
// AO+bent normal with the previous frame's result, reprojected by camera motion
// (static world -> no motion vectors needed). Ghosting on disocclusion / fast motion
// is bounded by a TAA-style neighbourhood clamp of the reprojected AO scalar into the
// local current-frame min/max, so stale history can't smear across silhouettes.
//
//   unit 0 (u_curAO)       = this frame's denoised AO (R) + bent normal (GBA)
//   unit 1 (u_history)     = previous frame's resolved AO+bent
//   unit 2 (u_currentDepth)= scene depth (for view-pos reconstruction / reprojection)
//   unit 3 (u_velocity)    = per-object screen-velocity MRT (R1/A2); used when u_localParam1.x > 0.5
//   u_localParam0 = ( 1/proj00, 1/proj11, feedback, historyValid )
//   u_localParam1.x       = 1 -> reproject by velocity (also catches moving objects); 0 -> matrix
//   u_modelViewMatrix     = reproj: current view space -> previous clip (camera-only fallback)
//   u_screenCorrection.xy = 1 / aoTargetSize        (gl_FragCoord -> [0,1] uv, texel size)
//   u_depthTexRecip.xy    = ratio / depthUploadSize (gl_FragCoord -> _currentDepth tc)

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_curAO;
SAMPLER_BINDING(1) uniform sampler2D u_history;
SAMPLER_BINDING(2) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(3) uniform sampler2D u_velocity;      // R1/A2 per-object velocity (gated by u_localParam1.x)

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

// Doom 3's fixed near / near-infinite far projection -> linear eye z (same constants
// as ssao.frag / softparticle.frag).
// depth -> view z pair of THIS view (renderparms.glsl u_depthParms): cinematics quarter the near
// plane, so it is not the constant ( 0.33333333, -0.33316667 ) it used to be hard-coded as
#define depth_consts DUDE_DEPTH_CONSTS()

void main() {
	vec2  frag = gl_FragCoord.xy;
	vec4  cur  = texture( u_curAO, var_TexCoord );
	float raw  = texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;

	// sky / no geometry: nothing to reproject, just pass the current value through
	if ( raw >= 0.9994 ) {
		fragColor = cur;
		return;
	}

	// where this pixel's surface was last frame, in the history buffer's sampling convention.
	vec2 prevUV;
	bool reprojOk;
	if ( u_localParam1.x > 0.5 ) {
		// R1/A2: reproject by the per-object velocity buffer, which unlike the camera-only
		// matrix path also follows MOVING objects. Velocity is currUV - prevUV in +Y-up UV;
		// flip the row on Vulkan's top-down framebuffer, then step back to the history location.
		vec2 vel = texture( u_velocity, var_TexCoord ).rg;
		vel.y = ( u_windowCoord.z < 0.0 ) ? -vel.y : vel.y;
		prevUV = var_TexCoord - vel;
		reprojOk = true;
	} else {
		// view-space position from depth (identical reconstruction to ssao.frag, incl. the
		// u_windowCoord.z view-Y sign: +1 GL, -1 Vulkan, so reprojection matches the normal)
		float vz  = 1.0 / ( raw * depth_consts.x + depth_consts.y );      // negative
		vec2  ndc = frag * ( u_screenCorrection.xy * 2.0 ) - 1.0;
		float d   = -vz;                                                  // positive depth
		vec3  P   = vec3( ndc.x * d * u_localParam0.x, ndc.y * u_windowCoord.z * d * u_localParam0.y, vz );

		// reproject into the previous frame: current view space -> previous clip -> uv.
		// prevUV is GL-convention (y-up); on Vulkan (u_windowCoord.z < 0) flip the row to
		// address the device-native top-down history buffer, or the reprojected history
		// samples vertically mirrored and accumulates an upside-down ghost. Mirrors the
		// same fix in ssr_temporal.frag.
		vec4  prevClip = u_modelViewMatrix * vec4( P, 1.0 );
		prevUV = ( prevClip.xy / prevClip.w ) * 0.5 + 0.5;
		if ( u_windowCoord.z < 0.0 ) {
			prevUV.y = 1.0 - prevUV.y;
		}
		reprojOk = prevClip.w > 0.0;
	}

	bool valid = u_localParam0.w > 0.5 && reprojOk
	          && all( greaterThanEqual( prevUV, vec2( 0.0 ) ) )
	          && all( lessThanEqual(    prevUV, vec2( 1.0 ) ) );

	if ( !valid ) {
		fragColor = cur;      // first frame / off-screen: fall back to current, no ghost
		return;
	}

	// neighbourhood clamp: bound the reprojected history's AO scalar to the local 3x3
	// current-frame AO range, so disocclusion / new occluders reject stale history
	vec2  texel  = u_screenCorrection.xy;
	float aoMin  = cur.r;
	float aoMax  = cur.r;
	for ( int y = -1; y <= 1; ++y ) {
		for ( int x = -1; x <= 1; ++x ) {
			if ( x == 0 && y == 0 ) {
				continue;			// center tap is cur.r, already seeded above
			}
			float s = texture( u_curAO, var_TexCoord + vec2( x, y ) * texel ).r;
			aoMin = min( aoMin, s );
			aoMax = max( aoMax, s );
		}
	}

	vec4 hist = texture( u_history, prevUV );
	hist.r = clamp( hist.r, aoMin, aoMax );

	// exponential accumulation: feedback = fraction of history kept per frame
	vec4 acc = mix( cur, hist, u_localParam0.z );

	// renormalize the accumulated bent normal (as ssao_blur.frag does)
	vec3  bn  = acc.gba * 2.0 - 1.0;
	float bl2 = dot( bn, bn );
	bn = ( bl2 > 1e-8 ) ? ( bn * inversesqrt( bl2 ) ) : vec3( 0.0 );

	fragColor = vec4( acc.r, bn * 0.5 + 0.5 );
}
