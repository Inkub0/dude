// DUDE weapon-reload depth-of-field. Keeps the near, depth-hacked weapon sharp
// and blurs the world beyond it, scaled by the reload focus envelope the game
// writes to r_weaponReloadFocus (eased 0->1->0 across the reload). A single-pass,
// depth-gated disc blur over the captured scene; runs after the 3D view (weapon
// included), before tonemap/HUD, on both RHI backends and in the HDR float path
// (the capture matches the scene format). Non-vanilla, opt-in.
//
//   unit 0 (u_currentRender) = captured scene colour (POT _currentRender)
//   unit 1 (u_currentDepth)  = captured scene depth  (POT _currentDepth)
//   u_screenCorrection.xy = colour NPOT adjust (viewport uv -> _currentRender)
//   u_localParam0 = ( focus 0..1, blurRadius px, focusStart raw-depth, focusEnd raw-depth )
//   u_localParam1 = ( depthAdjX, depthAdjY, texelX, texelY )
//   u_windowCoord.x = depthFlipV (1 on Vulkan: the depth capture is V-flipped vs colour)

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentRender;
SAMPLER_BINDING(1) uniform sampler2D u_currentDepth;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

const int DOF_TAPS = 32;

// Scene raw depth at a COLOUR-space uv. Vulkan stores the depth capture top-down
// while the colour capture reads upright under the same fullscreen-quad uv, so the
// depth lookup must flip V there (flipV = 0 on GL, where both share orientation).
float sceneDepth( vec2 colUv, vec2 depAdj, float flipV ) {
	vec2 d = vec2( colUv.x, mix( colUv.y, 1.0 - colUv.y, flipV ) );
	return texture( u_currentDepth, d * depAdj ).x;
}

void main() {
	vec2  uv     = var_TexCoord;
	vec2  colAdj = u_screenCorrection.xy;
	vec2  depAdj = u_localParam1.xy;
	vec2  texel  = u_localParam1.zw;
	float flipV  = u_windowCoord.x;

	vec3 sharp = texture( u_currentRender, uv * colAdj ).rgb;

	float focus      = u_localParam0.x;
	float focusStart = u_localParam0.z;
	float focusEnd   = u_localParam0.w;

	float centerDepth = sceneDepth( uv, depAdj, flipV );
	// circle of confusion: 0 at/nearer than the weapon (small raw depth), ramping
	// to 1 across the world beyond, then scaled by the reload focus envelope.
	float coc = smoothstep( focusStart, focusEnd, centerDepth ) * focus;
	if ( coc <= 0.003 ) {
		fragColor = vec4( sharp, 1.0 );		// weapon / no focus -> stay sharp
		return;
	}

	float radius = coc * u_localParam0.y;			// pixels
	const float golden = 2.39996323;				// golden-angle disc
	vec3  accum = vec3( 0.0 );
	float wsum  = 0.0;
	for ( int i = 0; i < DOF_TAPS; i++ ) {
		float t  = ( float( i ) + 0.5 ) / float( DOF_TAPS );
		float rr = sqrt( t ) * radius;				// uniform disc area
		float aa = float( i ) * golden;
		vec2  suv = clamp( uv + vec2( cos( aa ), sin( aa ) ) * rr * texel, 0.0, 1.0 );
		// depth-gate the tap: only world (>= focusStart) contributes, so the sharp
		// foreground weapon never bleeds a halo into the blurred world behind it.
		float tapDepth = sceneDepth( suv, depAdj, flipV );
		float wtap = smoothstep( focusStart * 0.75, focusStart, tapDepth );
		accum += texture( u_currentRender, suv * colAdj ).rgb * wtap;
		wsum  += wtap;
	}
	vec3 blurred = ( wsum > 0.0 ) ? ( accum / wsum ) : sharp;

	fragColor = vec4( mix( sharp, blurred, coc ), 1.0 );
}
