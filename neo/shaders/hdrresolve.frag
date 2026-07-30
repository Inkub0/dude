// DUDE HDR resolve (r_hdr). The single float->8-bit step for the whole scene: samples
// the RGBA16F scene buffer and writes it to the 8-bit backbuffer. The HDR target is
// exactly screen-sized, so st 0..1 maps 1:1 and no NPOT correction is needed.
//
// In HDR mode the film-grain + chromatic-aberration post effects are folded in HERE
// (not the separate postprocess pass), so they sample the smooth float buffer instead
// of round-tripping through the 8-bit _currentRender image.
//
// u_localParam0.y = film grain intensity; 0 = off
// u_localParam0.z = grain time/seed (seconds)
// u_localParam0.w = chromatic aberration strength; 0 = off
// u_windowCoord.zw = aberration center in uv (0.5, 0.5)

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_hdrScene;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

// small hash noise, stable per pixel per frame (matches postprocess.frag grain)
float hash12( vec2 p ) {
	vec3 p3 = fract( vec3( p.xyx ) * 0.1031 );
	p3 += dot( p3, p3.yzx + 33.33 );
	return fract( ( p3.x + p3.y ) * p3.z );
}

void main() {
	vec2 uv = var_TexCoord;

	// chromatic aberration: radial RGB split growing quadratically toward the edges,
	// sampling the float HDR buffer directly (same math as postprocess.frag, no NPOT adj)
	vec2  fromCenter = uv - u_windowCoord.zw;
	float caStrength = u_localParam0.w * 0.006 * dot( fromCenter, fromCenter ) * 4.0;
	vec2  caOffset = normalize( fromCenter + vec2( 1e-6 ) ) * caStrength;
	float cr = texture( u_hdrScene, uv + caOffset ).r;
	float cg = texture( u_hdrScene, uv ).g;
	float cb = texture( u_hdrScene, uv - caOffset ).b;
	vec3  color = vec3( cr, cg, cb );

	// film grain: luminance-weighted noise, animated by time; darker areas grain slightly more
	if ( u_localParam0.y > 0.0 ) {
		float grain = hash12( gl_FragCoord.xy + vec2( u_localParam0.z * 311.7, u_localParam0.z * 173.3 ) );
		float lum = dot( color, vec3( 0.299, 0.587, 0.114 ) );
		color += ( grain - 0.5 ) * u_localParam0.y * ( 1.0 - 0.5 * lum );
	}

	fragColor = vec4( color, 1.0 );
}
