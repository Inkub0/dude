// DUDE post-resolve antialiasing: FXAA over the finished 3D view. Runs after the
// 3D view, before 2D/GUI (HUD unaffected). Non-vanilla; GL3/Vulkan backends only,
// opt-in via r_rhiAA. Unlike the hardware MSAA in r_multiSamples this also smooths
// the specular/normal-map shimmer, at the cost of a slight overall softening.
//
// Standard FXAA 3.11-style luma edge blur (Timothy Lottes, NVIDIA). Operates on
// the LDR _currentRender snapshot; sub-pixel edges are found from luma contrast in
// the 3x3 neighbourhood and blurred along the detected edge direction.
//
// u_screenCorrection.xy = content extent in the oversized POT _currentRender
//                         (w/potW, h/potH) — the sampleable region is [0, this].
// u_localParam1.xy       = one screen texel in that same uv space (1/potW, 1/potH).
// u_localParam0.x        = subpixel smoothing amount (r_fxaaStrength): 0 = edge-only,
//                          up to 1 = blend subpixel detail toward its neighbourhood
//                          (chases specular/normal-map shimmer, softens texture a little).

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentRender;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

#define FXAA_SPAN_MAX   8.0
#define FXAA_REDUCE_MUL (1.0 / 8.0)
#define FXAA_REDUCE_MIN (1.0 / 128.0)

const vec3 LUMA = vec3( 0.299, 0.587, 0.114 );

// clamp keeps neighbour taps inside the content region so edge pixels never
// bleed the black POT padding around the copied viewport
vec3 sampleScene( vec2 uv, vec2 lo, vec2 hi ) {
	return texture( u_currentRender, clamp( uv, lo, hi ) ).rgb;
}

void main() {
	vec2 adj = u_screenCorrection.xy;		// content occupies [0, adj]
	vec2 rcp = u_localParam1.xy;			// one screen texel (1/potW, 1/potH)
	vec2 uv  = var_TexCoord * adj;			// actual texture coordinate
	vec2 lo  = 0.5 * rcp;
	vec2 hi  = adj - 0.5 * rcp;

	vec3 rgbNW = sampleScene( uv + vec2( -1.0, -1.0 ) * rcp, lo, hi );
	vec3 rgbNE = sampleScene( uv + vec2(  1.0, -1.0 ) * rcp, lo, hi );
	vec3 rgbSW = sampleScene( uv + vec2( -1.0,  1.0 ) * rcp, lo, hi );
	vec3 rgbSE = sampleScene( uv + vec2(  1.0,  1.0 ) * rcp, lo, hi );
	vec3 rgbM  = sampleScene( uv, lo, hi );

	float lumaNW = dot( rgbNW, LUMA );
	float lumaNE = dot( rgbNE, LUMA );
	float lumaSW = dot( rgbSW, LUMA );
	float lumaSE = dot( rgbSE, LUMA );
	float lumaM  = dot( rgbM,  LUMA );

	float lumaMin = min( lumaM, min( min( lumaNW, lumaNE ), min( lumaSW, lumaSE ) ) );
	float lumaMax = max( lumaM, max( max( lumaNW, lumaNE ), max( lumaSW, lumaSE ) ) );

	// edge direction, perpendicular to the luma gradient
	vec2 dir;
	dir.x = -( ( lumaNW + lumaNE ) - ( lumaSW + lumaSE ) );
	dir.y =  ( ( lumaNW + lumaSW ) - ( lumaNE + lumaSE ) );

	float dirReduce = max( ( lumaNW + lumaNE + lumaSW + lumaSE ) * ( 0.25 * FXAA_REDUCE_MUL ),
	                       FXAA_REDUCE_MIN );
	float rcpDirMin = 1.0 / ( min( abs( dir.x ), abs( dir.y ) ) + dirReduce );
	dir = clamp( dir * rcpDirMin, vec2( -FXAA_SPAN_MAX ), vec2( FXAA_SPAN_MAX ) ) * rcp;

	// two-tap and four-tap blurs along the edge; the four-tap is used unless it
	// pushed luma outside the local min/max (an over-blur), in which case fall
	// back to the safer two-tap
	vec3 rgbA = 0.5 * ( sampleScene( uv + dir * ( 1.0 / 3.0 - 0.5 ), lo, hi )
	                  + sampleScene( uv + dir * ( 2.0 / 3.0 - 0.5 ), lo, hi ) );
	vec3 rgbB = rgbA * 0.5 + 0.25 * ( sampleScene( uv + dir * -0.5, lo, hi )
	                                + sampleScene( uv + dir *  0.5, lo, hi ) );

	float lumaB = dot( rgbB, LUMA );
	vec3 edge = ( lumaB < lumaMin || lumaB > lumaMax ) ? rgbA : rgbB;

	// subpixel aliasing removal — the part that actually chases shimmer. Blend the
	// edge result toward the local low-pass, but only where the centre luma departs
	// from its neighbourhood (i.e. subpixel detail), so flat textures aren't softened.
	// Scaled by u_localParam0.x (r_fxaaStrength); 0 leaves the edge-only result.
	float strength = u_localParam0.x;
	vec3  rgbLowpass = ( rgbNW + rgbNE + rgbSW + rgbSE + rgbM ) * ( 1.0 / 5.0 );
	float lumaAvg    = ( lumaNW + lumaNE + lumaSW + lumaSE ) * 0.25;
	float lumaRange  = max( lumaMax - lumaMin, FXAA_REDUCE_MIN );
	float subpix     = clamp( abs( lumaAvg - lumaM ) / lumaRange, 0.0, 1.0 );
	subpix = subpix * subpix * ( 3.0 - 2.0 * subpix );		// smoothstep ramp
	vec3 result = mix( edge, rgbLowpass, subpix * strength );

	fragColor = vec4( result, 1.0 );
}
