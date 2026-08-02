// DUDE post-process pass: film grain + chromatic aberration in one fullscreen
// pass over _currentRender. Runs after the 3D view, before 2D/GUI — the HUD is
// unaffected. Both effects are improvements-menu toggles, default off; an
// effect with strength 0 is an exact passthrough.
//
// u_localParam0.x = film grain intensity   (0 = off; sensible ~0.05..0.15)
// u_localParam0.y = time/seed for animated grain (frame time in seconds)
// u_localParam0.z = chromatic aberration strength (0 = off; ~0.25..1.0)
// u_localParam1.x = grain cell size in pixels (1 = per-pixel, ~1.5-2 = filmic clumps)
// u_screenCorrection.xy = NPOT adjust into _currentRender
// u_windowCoord.zw      = viewport center in uv (usually 0.5, 0.5)

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentRender;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

// small hash noise, stable per pixel per frame
float hash12( vec2 p ) {
	vec3 p3 = fract( vec3( p.xyx ) * 0.1031 );
	p3 += dot( p3, p3.yzx + 33.33 );
	return fract( ( p3.x + p3.y ) * p3.z );
}

void main() {
	vec2 uv = var_TexCoord;
	vec2 adj = u_screenCorrection.xy;

	// chromatic aberration: radial RGB split, growing quadratically towards
	// the edges so the image center stays sharp. With aberration off the offset
	// is exactly zero and the three taps collapse to one fetch.
	vec3 color;
	if ( u_localParam0.z > 0.0 ) {
		vec2  fromCenter = uv - u_windowCoord.zw;
		float caStrength = u_localParam0.z * 0.024 * dot( fromCenter, fromCenter );
		vec2  caOffset = normalize( fromCenter + vec2( 1e-6 ) ) * caStrength;
		color = vec3( texture( u_currentRender, ( uv + caOffset ) * adj ).r,
		              texture( u_currentRender, uv * adj ).g,
		              texture( u_currentRender, ( uv - caOffset ) * adj ).b );
	} else {
		color = texture( u_currentRender, uv * adj ).rgb;
	}

	// film grain: triangular monochrome noise through a filmic beta-curve
	// response. Zero at pure black — the old flat curve greyed the void,
	// because clamping zero-mean noise on black keeps only its positive half —
	// peaking around 25% luminance so dim-but-lit areas carry the grain, then
	// tapering off in highlights like print stock. The 2.18 normalizes the
	// curve's peak to 1 so the intensity cvar keeps its scale.
	if ( u_localParam0.x > 0.0 ) {
		vec2 cell = floor( gl_FragCoord.xy / max( u_localParam1.x, 1.0 ) );
		vec2 seed = vec2( u_localParam0.y * 311.7, u_localParam0.y * 173.3 );
		float noise = 0.5 * ( hash12( cell + seed ) + hash12( cell + seed + vec2( 42.13, 59.71 ) ) ) - 0.5;
		float l = clamp( dot( color, vec3( 0.299, 0.587, 0.114 ) ), 0.0, 1.0 );
		float response = 2.18 * sqrt( l ) * pow( 1.0 - l, 1.5 );
		color += noise * u_localParam0.x * response;
	}

	fragColor = vec4( color, 1.0 );
}
