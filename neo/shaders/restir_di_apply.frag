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
	vec4 alb = texture( u_albedoBuffer, uv );					// .rgb receiver albedo, .a = skin/flesh SSS flag (gbuffer.frag)

	bool shMode = u_localParam1.w > 0.5;
	vec3 di;
	vec3 sh1 = vec3( 0.0 );
	vec3 shGatherNv = vec3( 0.0, 0.0, 1.0 );	// view-space normal the gather effectively used (reduced-res footprint) - the bump RATIO's denominator
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
		// The gather bakes di's cosine from ONE nearest full-res normal at the reduced-res texel CENTRE
		// (restir_di_shade.glsl diSurfaceAt), so di's cosine is blocky at the reduced-res scale. cosGath MUST be sampled
		// at that SAME scale (one normal per reduced texel) to cancel it block-for-block - a bilateral footprint average
		// smooths across the blocks (quilt) and shrinks unit normals (brightness bias). Nearest texel at the centre:
		vec2  gtexel = u_screenCorrection.xy;		// 1 / reduced-res size
		ivec2 nsz    = textureSize( u_normalBuffer, 0 );
		vec2  gcuv   = ( floor( uv / gtexel ) + 0.5 ) * gtexel;
		shGatherNv = texelFetch( u_normalBuffer, clamp( ivec2( floor( gcuv * vec2( nsz ) ) ), ivec2( 0 ), nsz - 1 ), 0 ).xyz * 2.0 - 1.0;
	}
	// remodulate by this pixel's material (the exact factor the gather divided by) and the forward light scale
	vec3 albedo = alb.rgb;
	vec4 mt     = texture( u_materialBuffer, uv );
	bool specReshade = u_localParam1.w > 1.5;		// SH + full-res specular re-shade (r_rtReStirDISpecReshade)
	vec3 lit;
	if ( shMode ) {
		// SH re-resolve as a BRIGHTNESS-NEUTRAL bump RATIO, not a cosine removal+reapply. The gather now stores di = the
		// FULL irradiance (cosine IN, bit-identical to the non-SH buffer) and sh1 = direction*luminance. The OLD scheme
		// removed the gather's cosine (di = radiance, which /N.L BLOWS UP and jumps as RIS repicks lights) then re-applied
		// an absolute cosine from the denoised aggregate direction - the per-pixel vs aggregate mismatch, denoised in
		// SEPARATE buffers, was the "watermelon". Instead modulate di by (N_full.L)/(N_gather.L): the gather's own cosine
		// (N_gather.L, from the reduced-res footprint normal shGatherNv) divides out and the full-res bump normal's cosine
		// multiplies in. On a smooth/native surface N_full == N_gather so the ratio is EXACTLY 1 (= non-SH, cannot band);
		// detail appears only where the full-res normal diverges. A small floor eases the terminator (L1 DC-term spirit)
		// and conditions the ratio as N_gather.L -> 0 (where di -> 0 anyway, so di*detail stays bounded).
		float Y   = dot( di, vec3( 0.2126, 0.7152, 0.0722 ) );
		float len = length( sh1 );
		vec3  cosTermV = vec3( 1.0 );				// the per-channel bump ratio (+ SSS warmth) applied to the irradiance
		vec3  L = vec3( 0.0, 0.0, 1.0 ), Nw = vec3( 0.0, 0.0, 1.0 );
		float k = 0.0, ndl = 0.0, detail = 1.0;
		bool  haveDir = ( Y > 1.0e-5 && len > 1.0e-6 );
		const float amb = 0.12;
		if ( haveDir ) {
			Nw  = normalize( mat3( u_modelViewMatrix ) * ( nt0.xyz * 2.0 - 1.0 ) );
			vec3  Ngw = normalize( mat3( u_modelViewMatrix ) * shGatherNv );	// UNIT, exactly as the gather normalized this reduced-centre normal -> di/cosGath cancels cleanly
			L   = sh1 / len;
			k   = clamp( len / Y, 0.0, 1.0 );			// SH direction concentration, for the spec lobe
			float cosFull = dot( Nw,  L );
			float cosGath = dot( Ngw, L );
			ndl = max( cosFull, 0.0 );
			detail   = clamp( ( max( cosFull, 0.0 ) + amb ) / ( max( cosGath, 0.0 ) + amb ), 0.0, 3.0 );
			cosTermV = vec3( detail );
			// DUDE skin/flesh SSS (docs/skin-shading.md): the red channel reaches further past the terminator (warm, soft
			// shadow edge). Warm the RATIO's numerator per channel (wrap the full-res cosine) so it rides the same re-resolve.
			if ( u_skinParms.x > 0.0 && alb.a > 0.25 ) {
				float rad = u_skinParms.y;
				vec3  w   = ( alb.a > 0.75 ) ? vec3( rad * 1.30, rad * 0.30, rad * 0.15 )	// Hell flesh: deeper, redder
				                             : vec3( rad * 1.00, rad * 0.35, rad * 0.20 );	// face skin
				vec3 cosFullC = ( vec3( cosFull ) + w ) / ( vec3( 1.0 ) + w );
				vec3 detailC  = clamp( ( max( cosFullC, vec3( 0.0 ) ) + amb ) / ( max( cosGath, 0.0 ) + amb ), vec3( 0.0 ), vec3( 3.0 ) );
				cosTermV = mix( vec3( detail ), detailC, u_skinParms.x );
			}
		}
		if ( specReshade ) {
			// The DI buffer is DIFFUSE-ONLY here (the gather shaded albedo-only, demodulated by the diffuse share), and now
			// di = the full IRRADIANCE at the reduced-res normal. di*cosTermV (the bump ratio) re-resolves it to the full-res
			// normal's irradiance. Rebuild the specular at FULL res from the SH direction + the full-res bump normal +
			// material - a GGX lobe exactly as interaction.frag's / the gather's - instead of the flat diffuse term the plain
			// SH path applies to a baked, reduced-res, denoised highlight. This is the sharp glint on pores / wet / metal.
			vec3 Bdiff   = max( albedo * ( 1.0 - mt.g * u_diffuseModifier.x ), vec3( 0.02 ) );	// diffuse share (matches the gather's demod)
			vec3 diffuse = di * Bdiff * cosTermV;
			vec3 spec    = vec3( 0.0 );
			if ( haveDir && mt.b > 0.0 ) {								// mt.b = spec-map luminance: no spec stage -> no highlight
				float dpos    = linDepth( raw );						// positive view depth
				vec2  ndcp    = uv * 2.0 - 1.0;
				vec3  viewPos = vec3( ndcp.x * dpos * u_occlusionParms.x, ndcp.y * dpos * u_occlusionParms.y, -dpos );
				vec3  worldPos = ( u_modelViewMatrix * vec4( viewPos, 1.0 ) ).xyz;
				vec3  Vv      = normalize( u_modelViewMatrix[3].xyz - worldPos );
				vec3  H       = normalize( L + Vv );
				float NdotH   = max( dot( Nw, H ), 0.0 );
				float NdotV   = max( dot( Nw, Vv ), 1.0e-4 );
				float NdotL   = max( ndl, 0.0 );
				float rough   = clamp( mt.r, 0.03, 1.0 );
				float a       = rough * rough;
				float alpha2  = a * a;
				vec3  F0      = mix( vec3( 0.04 ), albedo, mt.g );
				float VdotH   = max( dot( Vv, H ), 0.0 );
				vec3  F       = F0 + ( 1.0 - F0 ) * pow( 1.0 - VdotH, 5.0 );
				float dd      = NdotH * NdotH * ( alpha2 - 1.0 ) + 1.0;
				float D       = alpha2 / max( dd * dd, 1.0e-8 );
				float kg      = 0.5 * sqrt( alpha2 );
				float gvis    = 0.25 / ( ( NdotL * ( 1.0 - kg ) + kg ) * ( NdotV * ( 1.0 - kg ) + kg ) );
				float lobe    = min( D * gvis, u_diffuseModifier.z );	// firefly clamp (r_pbrFireflyClamp), as the gather
				float specGain = mt.b * mix( u_diffuseModifier.y, 1.0, mt.g ) * 2.0;	// spec map * dielectric-weighted scale * 2x
				// roughness-aware concentration: a SMOOTH lobe needs the exact light direction, so fall off with k (a
				// spread/aggregated direction would place a spurious glint); a ROUGH lobe (skin, cloth) integrates light
				// over a wide cone and tolerates the aggregate direction, so it keeps its broad sheen even when k is low -
				// otherwise a face lit by many fills (Betruger, 2026-10-14) loses the skin sheen the forward path sums.
				float concen  = mix( k, 1.0, rough );
				// di = irradiance (= radiance * N_gather.L); di*detail = radiance * N_full.L = the old radiance*NdotL factor,
				// so the cosine stays consistent with the diffuse re-resolve. NdotL remains in gvis (geometric shadowing).
				spec = ( di * detail ) * ( lobe * specGain * concen ) * F;
			}
			lit = ( diffuse + spec ) * u_localParam0.w;
		} else {
			di *= cosTermV;
			vec3 B = diDemodFactor( albedo, mt.g, mt.b, u_diffuseModifier.x, u_diffuseModifier.y );
			lit = di * B * u_localParam0.w;
		}
	} else {
		vec3 B = diDemodFactor( albedo, mt.g, mt.b, u_diffuseModifier.x, u_diffuseModifier.y );
		lit = di * B * u_localParam0.w;
	}

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
