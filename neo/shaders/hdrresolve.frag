// DUDE HDR resolve (r_hdr). The single float->8-bit step for the whole scene: samples
// the RGBA16F scene buffer and writes it to the 8-bit backbuffer. The HDR target is
// exactly screen-sized, so st 0..1 maps 1:1 and no NPOT correction is needed.
//
// In HDR mode the film-grain + chromatic-aberration post effects are folded in HERE
// (not the separate postprocess pass), so they sample the smooth float buffer instead
// of round-tripping through the 8-bit _currentRender image — which is what re-introduced
// the banding the dither is meant to remove. Order matters: chroma and grain first (they
// re-sample / add signal), then dither as the very last thing before the 8-bit write.
//
// u_localParam0.x = dither strength in 8-bit steps (LSBs); 0 = off
// u_localParam0.y = film grain intensity; 0 = off
// u_localParam0.z = grain time/seed (seconds)
// u_localParam0.w = chromatic aberration strength; 0 = off
// u_windowCoord.zw = aberration center in uv (0.5, 0.5)

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_hdrScene;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

// interleaved gradient noise: high-frequency, blue-noise-like spectrum, no texture
float IGN( vec2 p ) {
	return fract( 52.9829189 * fract( dot( p, vec2( 0.06711056, 0.00583715 ) ) ) );
}

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

	// dither LAST, right before the 8-bit write: uniform [0,1) noise -> triangular PDF
	// in [-1,1] (single-sample remap), scaled to the requested LSB count
	float amount = u_localParam0.x;
	if ( amount > 0.0 ) {
		float n = IGN( gl_FragCoord.xy ) * 2.0 - 1.0;
		float tri = sign( n ) * ( 1.0 - sqrt( max( 0.0, 1.0 - abs( n ) ) ) );
		color += tri * ( amount / 255.0 );
	}

	fragColor = vec4( color, 1.0 );
}
