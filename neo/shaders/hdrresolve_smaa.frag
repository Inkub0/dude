// DUDE fused SMAA resolve (docs/antialiasing.md). The SMAA 1x neighborhood-blending pass (smaa.glsl)
// with the HDR resolve's film-grain + gamma/brightness tail folded in, so the anti-aliased float
// scene reaches the 8-bit backbuffer in ONE pass — no separate rhiHdrAaRT round-trip (the classic
// path runs the blend into rhiHdrAaRT and then a second hdrresolve pass reads it back).
//
// This pass never does chromatic aberration: chroma runs as a scene-only pass before the HUD
// (RB_RHI_ChromaticAberration), independent of the resolve, so the fused path is used regardless
// of the chroma setting (it used to be disqualified when chroma was on).
//
// For a pixel with zero SMAA blend weight the neighborhood blend is a pass-through, so the result is
// exactly (scene colour -> grain -> gamma) — the same as the classic path.
//
// u_localParam0    = SMAA_RT_METRICS (1/w, 1/h, w, h)   [shared with the vertex stage + the blend]
// u_windowCoord.x  = film grain intensity; 0 = off      [localParam0 is taken by RT_METRICS, and
// u_windowCoord.y  = grain time/seed (seconds)           chroma is off here, so windowCoord is free]
// u_localParam1.x  = grain cell size in pixels (1 = per-pixel)
// u_localParam1.y  = r_brightness (1 = identity)
// u_localParam1.z  = 1.0 / r_gamma (1 = identity)
// u_localParam1.w  = tonemap curve (r_hdrTonemap): 0 off, 1 Reinhard, 2 ACES, 3 AgX, 4 PBR Neutral, 5 DUDE
// u_windowCoord.z  = HDR exposure multiplier (r_hdrExposure); windowCoord.x/y hold the grain parms
// u_color.x        = DUDE tonemap knee  (r_hdrDudeKnee)   [u_color is otherwise unused in this pass]
// u_color.y        = DUDE tonemap desat (r_hdrDudeDesat)
// u_color.z        = DUDE tonemap white-hot tint (r_hdrDudeTint)

#include "renderparms.glsl"
#include "tonemap.glsl"

#define SMAA_GLSL_3 1
#define SMAA_RT_METRICS u_localParam0
#define SMAA_PRESET_HIGH 1
#define SMAA_INCLUDE_VS 0
#include "smaa.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_sceneTex;
SAMPLER_BINDING(1) uniform sampler2D u_blendTex;

VARY(0) in vec2 var_TexCoord;
VARY(1) in vec4 var_SmaaOffset;

layout(location = 0) out vec4 fragColor;

// small hash noise, stable per pixel per frame (matches hdrresolve.frag / postprocess.frag grain)
float hash12( vec2 p ) {
	vec3 p3 = fract( vec3( p.xyx ) * 0.1031 );
	p3 += dot( p3, p3.yzx + 33.33 );
	return fract( ( p3.x + p3.y ) * p3.z );
}

void main() {
	// SMAA 1x neighborhood blend: the anti-aliased scene colour. Pixels with zero weights pass
	// through untouched, exactly as the standalone smaa_blend pass.
	vec3 color = SMAANeighborhoodBlendingPS( var_TexCoord, var_SmaaOffset, u_sceneTex, u_blendTex ).rgb;

	// exposure + tonemap (identical to hdrresolve.frag; exposure lives in windowCoord.z here
	// because localParam0 carries SMAA_RT_METRICS). Mode 0 + exposure 1.0 = passthrough.
	color = DudeTonemap( color, u_windowCoord.z, int( u_localParam1.w + 0.5 ), u_color.x, u_color.y, u_color.z );

	// film grain: identical curve/seed to hdrresolve.frag, only the parm slots differ (intensity +
	// seed live in windowCoord.xy here because localParam0 carries SMAA_RT_METRICS)
	if ( u_windowCoord.x > 0.0 ) {
		vec2 cell = floor( gl_FragCoord.xy / max( u_localParam1.x, 1.0 ) );
		vec2 seed = vec2( u_windowCoord.y * 311.7, u_windowCoord.y * 173.3 );
		float noise = 0.5 * ( hash12( cell + seed ) + hash12( cell + seed + vec2( 42.13, 59.71 ) ) ) - 0.5;
		float l = clamp( dot( color, vec3( 0.299, 0.587, 0.114 ) ), 0.0, 1.0 );
		float response = 2.18 * sqrt( l ) * pow( 1.0 - l, 1.5 );
		color += noise * u_windowCoord.x * response;
	}

	// gamma / brightness: folded in on Vulkan (r_gammaInShader); GL passes identity and keeps its
	// standalone gammabrightness pass. Matches hdrresolve.frag exactly.
	if ( u_localParam1.y != 1.0 || u_localParam1.z != 1.0 ) {
		color = clamp( color * u_localParam1.y, 0.0, 1.0 );
		color = pow( color, vec3( u_localParam1.z ) );
	}

	fragColor = vec4( color, 1.0 );
}
