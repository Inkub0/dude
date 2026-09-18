// DUDE SSR glass reflections (docs/rtx-reflections.md RR5c-SSR) — fragment stage, unbumped.
// Screen-space reflection ON the glass surface: reflect the view ray off the glass view-space normal and
// MARCH the scene depth buffer (ssr.frag's armed-crossing march), then sample the lit scene at the hit —
// reflecting the live on-screen room in place of the baked cube/probe. Off-screen rays fade to a dim
// ambient (SSR is screen-space; it can't show what's not on screen). Output modulates by the stage tint
// in the same dst_alpha blend slot the cube used. Selected by the backend on the SSR tier (r_ssr on, RT
// off, unbumped glass); u_currentRender + u_currentDepth are the SSR pass's captures for this view.
//
// A nice property vs floor SSR: the glass itself is translucent and NOT in the depth buffer, so the march
// can't self-hit the pane — the depth buffer holds only the opaque scene the reflection should find.
//
// Uniform packing (RB_RHI_RenderTexgenStage, ssrGlass): u_projectionMatrix = view->clip; u_localParam0 =
// ( maxDistance, thickness, steps, 0 ); u_screenCorrection.xy = 1/viewSize, .zw = view/POT scale for the
// _currentRender sample; u_depthTexRecip.xy = gl_FragCoord -> depth tc; u_windowCoord.z = view-Y sign (VK).

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentRender;	// lit opaque scene snapshot (SSR pass captured it)
SAMPLER_BINDING(1) uniform sampler2D u_currentDepth;

VARY(0) in vec3 var_ViewPos;
VARY(1) in vec3 var_ViewNormal;
VARY(2) in vec4 var_Color;

layout(location = 0) out vec4 fragColor;

#define MAX_MARCH_STEPS  48
#define REFINE_STEPS     4

// depth -> view z pair of THIS view (renderparms.glsl u_depthParms): the game lowers the near plane
// for cinematic cameras, so it is not the constant ( 0.33333333, -0.33316667 ) it used to be hard-coded as
#define depth_consts DUDE_DEPTH_CONSTS()
const vec3  SKY_FILL = vec3( 0.10, 0.11, 0.13 );			// dim ambient on a miss / at the screen edge
const float SKY_GRAZE = 1.0;								// grazing Fresnel boost (matches environment_rt)

float viewZFromRaw( float raw ) {
	return 1.0 / ( min( raw, 0.9994 ) * depth_consts.x + depth_consts.y );	// negative
}
float rawDepth( vec2 frag ) {
	return texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;
}
// GL row (bottom-up) -> device framebuffer row: identity on GL, flipped on Vulkan (top-down).
float deviceRow( float glRow ) {
	return ( u_windowCoord.z > 0.0 ) ? glRow : ( 1.0 - glRow );
}
// project a view-space point to fragment coords (the reduced perspective multiply from ssr.frag; SSR runs
// on primary views whose projection is a standard perspective matrix, so the dropped terms are exactly 0).
vec3 projectToFrag( vec3 viewPos ) {
	float cw = u_projectionMatrix[2][3] * viewPos.z;
	if ( cw <= 0.0 ) { return vec3( -1.0 ); }
	float cx = u_projectionMatrix[0][0] * viewPos.x + u_projectionMatrix[2][0] * viewPos.z;
	float cy = u_projectionMatrix[1][1] * viewPos.y + u_projectionMatrix[2][1] * viewPos.z;
	vec2 uv01 = ( vec2( cx, cy ) / cw ) * 0.5 + 0.5;
	uv01.y = deviceRow( uv01.y );
	return vec3( uv01 / u_screenCorrection.xy, 1.0 );
}
float ign( vec2 p ) {
	return fract( 52.9829189 * fract( dot( p, vec2( 0.06711056, 0.00583715 ) ) ) );
}

void main() {
	vec3 P = var_ViewPos;
	vec3 N = normalize( var_ViewNormal );
	vec3 V = normalize( -P );
	if ( dot( N, V ) < 0.0 ) { N = -N; }				// reflect off the viewer-facing side (glass is 2-sided)
	vec3 R = reflect( -V, N );

	float maxDist   = u_localParam0.x;
	float thickness = u_localParam0.y;
	int   steps     = int( u_localParam0.z );
	float stepLen   = maxDist / float( max( steps, 1 ) );

	float ndv   = clamp( dot( N, V ), 0.0, 1.0 );
	float mm    = 1.0 - ndv;
	float graze = 1.0 + SKY_GRAZE * ( mm * mm ) * ( mm * mm ) * mm;

	// jittered linear march with the armed-crossing test (ssr.frag): a hit needs an earlier sample IN
	// FRONT of the surface, then this one behind it by less than the thickness. Per-fragment jitter hides
	// the step banding. The glass pane is not in the depth buffer, so the march never self-hits it.
	float tPrev = stepLen * fract( ign( gl_FragCoord.xy ) );
	float t     = tPrev + stepLen;
	float tHit  = -1.0;
	bool  armed = false;
	for ( int i = 0; i < MAX_MARCH_STEPS; i++ ) {
		if ( i >= steps ) { break; }
		vec3 rayPos = P + R * t;
		if ( rayPos.z > -1.0 ) { break; }				// crossed the near plane
		vec3 pf = projectToFrag( rayPos );
		if ( pf.z < 0.0 ) { break; }					// behind the eye
		vec2 suv = pf.xy * u_screenCorrection.xy;
		if ( suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0 ) { break; }	// left the screen
		float sceneZ = viewZFromRaw( rawDepth( pf.xy ) );
		float dz = sceneZ - rayPos.z;					// > 0: ray is behind the surface here
		if ( armed && dz > 0.0 && dz < thickness + stepLen ) { tHit = t; break; }
		if ( dz <= 0.0 ) { armed = true; }				// seen in front; a crossing can now hit
		tPrev = t;
		t += stepLen;
	}

	vec3 refl = SKY_FILL;								// default: miss (off-screen) -> dim ambient, never black
	if ( tHit >= 0.0 ) {
		// binary refinement between the last miss and the hit
		float lo = tPrev, hi = tHit;
		for ( int i = 0; i < REFINE_STEPS; i++ ) {
			float mid = 0.5 * ( lo + hi );
			vec3  rp  = P + R * mid;
			float dz  = viewZFromRaw( rawDepth( projectToFrag( rp ).xy ) ) - rp.z;
			if ( dz > 0.0 ) { hi = mid; } else { lo = mid; }
		}
		vec3 pfHit = projectToFrag( P + R * hi );
		vec2 hitUv = pfHit.xy * u_screenCorrection.xy;
		// screen-edge + range fade to the ambient (SSR data ends at the viewport, and far hits are unreliable)
		vec2  eDist = min( hitUv, 1.0 - hitUv );
		float edge  = smoothstep( 0.0, 0.08, min( eDist.x, eDist.y ) );
		float range = 1.0 - clamp( hi / maxDist, 0.0, 1.0 );
		// sample the lit scene at the hit. crUv: device row -> the bottom-up GL layout of _currentRender
		// (the two flips cancel on GL, apply once on VK), POT-scaled by screenCorrection.zw.
		vec2 crUv  = vec2( hitUv.x, deviceRow( hitUv.y ) ) * u_screenCorrection.zw;
		vec3 scene = texture( u_currentRender, crUv ).rgb;
		refl = mix( SKY_FILL, scene, edge * range );
	}

	// modulate by the stage tint exactly as the cube path did, keep the pane's own alpha for the dst blend.
	fragColor = vec4( refl * var_Color.rgb * graze, var_Color.a );
}
