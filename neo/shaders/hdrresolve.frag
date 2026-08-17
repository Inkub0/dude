// DUDE HDR resolve (r_hdr). The single float->8-bit step for the whole scene: samples
// the RGBA16F scene buffer and writes it to the 8-bit backbuffer. The HDR target is
// exactly screen-sized, so st 0..1 maps 1:1 and no NPOT correction is needed.
//
// In HDR mode the film-grain + chromatic-aberration post effects are folded in HERE
// (not the separate postprocess pass), so they sample the smooth float buffer instead
// of round-tripping through the 8-bit _currentRender image.
//
// u_localParam0.x = HDR exposure multiplier (r_hdrExposure, applied before the tonemap)
// u_localParam0.y = film grain intensity; 0 = off
// u_localParam0.z = grain time/seed (seconds)
// u_localParam0.w = chromatic aberration strength; 0 = off
// u_localParam1.x = grain cell size in pixels (1 = per-pixel, ~1.5-2 = filmic clumps)
// u_localParam1.y = r_brightness (1 = identity)
// u_localParam1.z = 1.0 / r_gamma (1 = identity)
// u_localParam1.w = tonemap curve (r_hdrTonemap): 0 off, 1 Reinhard, 2 ACES, 3 AgX, 4 PBR Neutral
// u_windowCoord.x  = eye-adaptation flag: >0.5 = use the 1x1 adapted exposure below instead of localParam0.x
// u_windowCoord.zw = aberration center in uv (0.5, 0.5)

#include "renderparms.glsl"
#include "tonemap.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_hdrScene;
SAMPLER_BINDING(1) uniform sampler2D u_adaptedExposure;   // 1x1 eye-adaptation exposure (Phase B1)

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
	// sampling the float HDR buffer directly (same math as postprocess.frag, no NPOT adj).
	// With aberration off the offset is exactly zero and the three taps collapse to the
	// one fetch the plain resolve needs.
	vec3 color;
	if ( u_localParam0.w > 0.0 ) {
		vec2  fromCenter = uv - u_windowCoord.zw;
		float caStrength = u_localParam0.w * 0.024 * dot( fromCenter, fromCenter );
		vec2  caOffset = normalize( fromCenter + vec2( 1e-6 ) ) * caStrength;
		color = vec3( texture( u_hdrScene, uv + caOffset ).r,
		              texture( u_hdrScene, uv ).g,
		              texture( u_hdrScene, uv - caOffset ).b );
	} else {
		color = texture( u_hdrScene, uv ).rgb;
	}

	// exposure + tonemap: map the linear HDR scene into display [0,1] before grain/gamma.
	// Mode 0 with exposure 1.0 is a passthrough, so the default (r_hdrTonemap 0) is
	// bit-identical to the pre-tonemap resolve. Grain then runs on the display-range image.
	// Eye adaptation (Phase B1): when its flag is set, the exposure comes from the 1x1
	// adapted-exposure texture instead of the static r_hdrExposure.
	float exposure = ( u_windowCoord.x > 0.5 ) ? texelFetch( u_adaptedExposure, ivec2( 0 ), 0 ).r
	                                            : u_localParam0.x;
	color = DudeTonemap( color, exposure, int( u_localParam1.w + 0.5 ) );

	// film grain: triangular monochrome noise through a filmic beta-curve response —
	// zero at pure black (no clip-lift greying of the void), peaking around 25%
	// luminance, tapering off in highlights. Same curve as postprocess.frag; the HDR
	// luma is clamped so >1 highlights sit at the (near-zero) white end of the curve.
	if ( u_localParam0.y > 0.0 ) {
		vec2 cell = floor( gl_FragCoord.xy / max( u_localParam1.x, 1.0 ) );
		vec2 seed = vec2( u_localParam0.z * 311.7, u_localParam0.z * 173.3 );
		float noise = 0.5 * ( hash12( cell + seed ) + hash12( cell + seed + vec2( 42.13, 59.71 ) ) ) - 0.5;
		float l = clamp( dot( color, vec3( 0.299, 0.587, 0.114 ) ), 0.0, 1.0 );
		float response = 2.18 * sqrt( l ) * pow( 1.0 - l, 1.5 );
		color += noise * u_localParam0.y * response;
	}

	// gamma / brightness. On Vulkan the r_gammaInShader correction is folded in here
	// (the backend has no separate LDR gamma tail); GL passes identity and keeps its
	// standalone gammabrightness pass, so this is an exact passthrough there. Matches
	// R_SetColorMappings: out = pow( clamp( color * brightness, 0, 1 ), 1/gamma ).
	if ( u_localParam1.y != 1.0 || u_localParam1.z != 1.0 ) {
		color = clamp( color * u_localParam1.y, 0.0, 1.0 );
		color = pow( color, vec3( u_localParam1.z ) );
	}

	fragColor = vec4( color, 1.0 );
}
