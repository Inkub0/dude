// DUDE post-process pass: film grain + chromatic aberration in one fullscreen
// pass over _currentRender. Runs after the 3D view, before 2D/GUI — the HUD is
// unaffected. Both effects are improvements-menu toggles, default off; an
// effect with strength 0 is an exact passthrough.
//
// u_localParam0.x = film grain intensity   (0 = off; sensible ~0.05..0.15)
// u_localParam0.y = time/seed for animated grain (frame time in seconds)
// u_localParam0.z = chromatic aberration strength (0 = off; ~0.25..1.0)
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
	vec2 center = u_windowCoord.zw;
	vec2 fromCenter = uv - center;

	// chromatic aberration: radial RGB split, growing quadratically towards
	// the edges so the image center stays sharp
	float caStrength = u_localParam0.z * 0.006 * dot( fromCenter, fromCenter ) * 4.0;
	vec2 caOffset = normalize( fromCenter + vec2( 1e-6 ) ) * caStrength;

	vec2 adj = u_screenCorrection.xy;
	float r = texture( u_currentRender, ( uv + caOffset ) * adj ).r;
	float g = texture( u_currentRender, uv * adj ).g;
	float b = texture( u_currentRender, ( uv - caOffset ) * adj ).b;
	vec3 color = vec3( r, g, b );

	// film grain: luminance-weighted noise, animated by time; darker areas
	// grain slightly more, like film stock
	float grain = hash12( gl_FragCoord.xy + vec2( u_localParam0.y * 311.7, u_localParam0.y * 173.3 ) );
	float lum = dot( color, vec3( 0.299, 0.587, 0.114 ) );
	color += ( grain - 0.5 ) * u_localParam0.x * ( 1.0 - 0.5 * lum );

	fragColor = vec4( color, 1.0 );
}
