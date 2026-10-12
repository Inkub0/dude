// DUDE ReSTIR DI apply (docs/rtx-restir-di.md Stage D, r_rtReStirDIApply): add the denoised direct lighting into
// the lit HDR scene, scene-wide, as the direct-lighting term of every opaque world surface - the forward per-light
// interaction draws are skipped for the lights DI covers (RB_RHI_DrawWorld), so this is what lights them.
//
// The DI buffer holds the gather's DEMODULATED shade: rad(y)·W·vis / B, where B = diDemodFactor( albedo, metal,
// specMask ) (restir_di_common.glsl). Remultiply by the SAME B from the full-res G-buffer, then by the forward
// path's light scale (backEnd.lightScale = r_lightScale: the gather's light colours are the raw stage colours)
// and the AO-on-direct-light term interaction.frag applies (mix( floor, 1, ao ) blended by r_ssaoDirectLight).
// Upscale as restir_di_debug.frag does (bilinear, or joint-bilateral when reduced-res). Additive ONE|ONE into
// the live rhiHdrRT at the opaque/translucent split, inside FSR2's opaque snapshot (like rtgi_apply). Sky and
// the view weapon (normal-buffer alpha = 0) get nothing: the weapon keeps its forward draws.
//
//   u_src (unit 0)        = DI (NRD-YCoCg, demodulated), denoised when it ran else raw; reduced res allowed
//   u_currentDepth (1)    = sealed scene depth (POT)
//   u_normalBuffer (2)    = full-res view-space normal (xyz, a = weapon/AO mask)
//   u_albedoBuffer (3)    = G-buffer att3 receiver albedo (full res)
//   u_materialBuffer (4)  = G-buffer att1 ( roughness, metalness, spec-map luminance ) (full res)
//   u_ssao (5)            = AO buffer (r = ambient visibility), sampled only when u_localParam1.x > 0.5
//   u_localParam0         = ( bilateral on/off, depth sensitivity, normal power, light scale )
//   u_localParam1         = ( AO enable, AO floor, AO direct-light strength, 0 )
//   u_occlusionParms.z    = baked occlusion-map strength on direct light (r_occlusionMapScale * r_occlusionMapDirect); 0 = off
//   u_occlusionParms.w    = R2: the source is LINEAR (ReLAX output / raw ReLAX-mode gather) instead of NRD-YCoCg
//   u_localParam1.w       = SH MODE: u_sh1 (unit 6) holds the denoised SH1 (direction * luminance); the gather carried no
//                           cosine, so the cosine of the FULL-res bump normal is re-applied here per pixel
//   u_modelViewMatrix     = inverse view (view -> world) - the SH direction is world space, the normal buffer view space
//   u_diffuseModifier.xy  = ( kd = 1 - r_pbrMetalDiffuse, r_pbrSpecScale )   [diDemodFactor inputs]
//   u_screenCorrection.xy = 1 / (rw, rh)  - the reduced-res DI texel in view uv (the 2x2 footprint)
//   u_depthTexRecip.xy    = (w/uploadW, h/uploadH)  - view uv -> depth tc (the POT depth image)

#include "renderparms.glsl"
#include "restir_di_common.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_src;
SAMPLER_BINDING(1) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(2) uniform sampler2D u_normalBuffer;
SAMPLER_BINDING(3) uniform sampler2D u_albedoBuffer;
SAMPLER_BINDING(4) uniform sampler2D u_materialBuffer;
SAMPLER_BINDING(5) uniform sampler2D u_ssao;
SAMPLER_BINDING(6) uniform sampler2D u_sh1;			// SH mode: direction * luminance (denoised OUT_DIFF_SH1, else the raw gather's)

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

#define depth_consts DUDE_DEPTH_CONSTS()

// NRD REBLUR radiance decode (_NRD_YCoCgToLinear), the exact inverse of restir_di.frag's nrdLinearToYCoCg.
vec3 nrdYCoCgToLinear( vec3 c ) {
	if ( u_occlusionParms.w > 0.5 ) { return max( c, vec3( 0.0 ) ); }	// R2: already linear
	float t = c.x - c.z;
	return max( vec3( t + c.y, c.x + c.z, t - c.y ), vec3( 0.0 ) );
}

float linDepth( float raw ) { return -1.0 / ( min( raw, 0.9994 ) * depth_consts.x + depth_consts.y ); }	// positive linear view depth

void main() {
	vec2  uv  = var_TexCoord;
	float raw = texture( u_currentDepth, uv * u_depthTexRecip.xy ).x;
	if ( raw >= 0.9994 ) { fragColor = vec4( 0.0 ); return; }		// sky / no geometry
	vec4 nt0 = texture( u_normalBuffer, uv );
	if ( nt0.a < 0.5 ) { fragColor = vec4( 0.0 ); return; }		// view weapon: lit by its forward draws

	bool shMode = u_localParam1.w > 0.5;
	vec3 di;
	vec3 sh1 = vec3( 0.0 );
	if ( u_localParam0.x > 0.5 ) {
		// Joint-bilateral upscale (restir_di_debug.frag / rtgi_apply.frag): reweight the 2x2 reduced-res footprint by
		// how well each texel's surface matches this pixel's, so the DI stays on the right surface.
		float d0 = linDepth( raw );
		vec3  n0 = nt0.xyz * 2.0 - 1.0;
		vec2  texel = u_screenCorrection.xy;
		vec2  f  = uv / texel - 0.5;
		vec2  b  = floor( f );
		vec2  fr = f - b;
		vec4  bw = vec4( ( 1.0 - fr.x ) * ( 1.0 - fr.y ), fr.x * ( 1.0 - fr.y ), ( 1.0 - fr.x ) * fr.y, fr.x * fr.y );
		vec2  offs[4] = vec2[4]( vec2( 0.5, 0.5 ), vec2( 1.5, 0.5 ), vec2( 0.5, 1.5 ), vec2( 1.5, 1.5 ) );
		vec3  sum = vec3( 0.0 );
		float wsum = 0.0;
		for ( int k = 0; k < 4; k++ ) {
			vec2  gUV = ( b + offs[k] ) * texel;
			float dk  = linDepth( texture( u_currentDepth, gUV * u_depthTexRecip.xy ).x );
			vec3  nk  = texture( u_normalBuffer, gUV ).xyz * 2.0 - 1.0;
			float dw  = exp( -abs( d0 - dk ) / max( d0, 1.0 ) * u_localParam0.y );
			float nw  = pow( max( dot( n0, nk ), 0.0 ), u_localParam0.z );
			float wk  = bw[k] * dw * nw;
			sum  += nrdYCoCgToLinear( texture( u_src, gUV ).rgb ) * wk;
			if ( shMode ) { sh1 += texture( u_sh1, gUV ).xyz * wk; }
			wsum += wk;
		}
		di = ( wsum > 1.0e-4 ) ? sum / wsum : nrdYCoCgToLinear( texture( u_src, uv ).rgb );
		if ( shMode ) { sh1 = ( wsum > 1.0e-4 ) ? sh1 / wsum : texture( u_sh1, uv ).xyz; }
	} else {
		di = nrdYCoCgToLinear( texture( u_src, uv ).rgb );
		if ( shMode ) { sh1 = texture( u_sh1, uv ).xyz; }
	}
	if ( shMode ) {
		// SH resolve: the denoised mean incoming direction (SH1 / luminance) and how concentrated it is (|SH1| / Y,
		// 1 = a single light, 0 = light from everywhere). The gather removed its reduced-res normal's cosine, so the
		// FULL-res bump normal's cosine goes back on here - this is where pores, wrinkles and weave come from - exact
		// for a concentrated direction, easing to a wrapped half-cosine as the light spreads (a mean direction means
		// little then).
		float Y   = dot( di, vec3( 0.2126, 0.7152, 0.0722 ) );
		float len = length( sh1 );
		if ( Y > 1.0e-5 && len > 1.0e-6 ) {
			vec3  L  = sh1 / len;
			vec3  Nw = normalize( mat3( u_modelViewMatrix ) * ( nt0.xyz * 2.0 - 1.0 ) );
			float k  = clamp( len / Y, 0.0, 1.0 );
			float ndl = dot( Nw, L );
			di *= mix( 0.5 + 0.5 * ndl, max( ndl, 0.0 ), k );
		}
	}

	// remodulate by this pixel's material (the exact factor the gather divided by) and the forward light scale
	vec3 albedo = texture( u_albedoBuffer, uv ).rgb;
	vec4 mt     = texture( u_materialBuffer, uv );
	vec3 B      = diDemodFactor( albedo, mt.g, mt.b, u_diffuseModifier.x, u_diffuseModifier.y );
	vec3 lit    = di * B * u_localParam0.w;

	// AO on direct light, as interaction.frag: ao floored, blended in by r_ssaoDirectLight (diffuse AND the
	// specular share here - the two are denoised together in v1)
	if ( u_localParam1.x > 0.5 ) {
		float ao = texture( u_ssao, uv ).r;
		ao = mix( u_localParam1.y, 1.0, ao );
		lit *= mix( 1.0, ao, u_localParam1.z );
	}
	// baked occlusion map (material att1 .a, 1 = none): the forward path's direct-diffuse factor
	// mix( 1, map, scale*direct ) - applied to the combined term here (v1: diffuse + spec denoised together)
	if ( u_occlusionParms.z > 0.0 ) {
		lit *= mix( 1.0, mt.a, u_occlusionParms.z );
	}
	fragColor = vec4( max( lit, vec3( 0.0 ) ), 0.0 );
}
