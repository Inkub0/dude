// Translated from glprogs/interaction.vfp (fragment program).
// Texture units preserved from the ARB program.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform samplerCube u_normalCubeMap; // normalization cube map
SAMPLER_BINDING(1) uniform sampler2D u_bumpMap;
SAMPLER_BINDING(2) uniform sampler2D u_lightFalloff;
SAMPLER_BINDING(3) uniform sampler2D u_lightProjection;
SAMPLER_BINDING(4) uniform sampler2D u_diffuseMap;
SAMPLER_BINDING(5) uniform sampler2D u_specularMap;
SAMPLER_BINDING(6) uniform sampler2D u_specularTable;   // specular falloff LUT
SAMPLER_BINDING(7) uniform sampler2DShadow u_shadowMap; // 2D depth map (projected/spot light)
SAMPLER_BINDING(8) uniform samplerCubeShadow u_shadowCube; // cube depth map (point light)
SAMPLER_BINDING(9) uniform sampler2D u_ssao;            // DUDE GTAO buffer (R = ambient visibility)
SAMPLER_BINDING(10) uniform sampler2D u_occlusionMap;   // DUDE baked AO map (R = visibility)

VARY(0) in vec3 var_TexLightVec;
VARY(1) in vec2 var_TexBump;
VARY(2) in vec2 var_TexFalloff;
VARY(3) in vec4 var_TexProjection;
VARY(4) in vec2 var_TexDiffuse;
VARY(5) in vec2 var_TexSpecular;
VARY(6) in vec3 var_TexHalfVec;
VARY(7) in vec4 var_Color;
VARY(8) in vec3 var_TexViewVec;
VARY(9) in vec3 var_ShadowCubeVec;

layout(location = 0) out vec4 fragColor;

// 0 = fully shadowed, 1 = fully lit. u_shadowParms.x selects the technique:
//   0 = none (stencil / unshadowed) -> always lit, vanilla untouched
//   1 = projected/spot: 2D map, reusing the light-projection texgen
//       (var_TexProjection gives the cookie UV, var_TexFalloff.x the axial depth)
//   2 = point/omni: cube map, indexed by the world-space light->frag direction
//       (var_ShadowCubeVec), reference = radial distance / range
// Hardware depth-compare sampler (2x2 PCF); the 2D path adds a 4-tap Poisson spread.
float shadowVisibility() {
	if ( u_shadowParms.x == 0.0 ) {
		return 1.0;
	}
	if ( u_shadowParms.x > 1.5 ) {
		// point light: the caster stored linear radial distance/range as depth, so
		// compare the same quantity here.
		vec3 L = var_ShadowCubeVec;
		float dist = length( L );
		float ref = dist / max( u_shadowParms.w, 1.0 ) - u_shadowParms.z;

		int taps = int( u_specularParms.w + 0.5 );		// cube PCF tap count (r_shadowMapCubePcf)
		if ( taps <= 1 ) {
			return texture( u_shadowCube, vec4( L, ref ) );	// single hardware 2x2 tap
		}

		// Disc PCF: the cube is sampled by direction, so perturb L within its tangent
		// plane by a few texels' worth of angle and average the hardware taps. One cube
		// texel spans ~2*dist/res in world tangent units (a face covers +/-dist at its
		// edge); spread ~2 texels to soften the stair-stepped edge without leaking.
		vec3 up = abs( L.y ) < 0.99 ? vec3( 0.0, 1.0, 0.0 ) : vec3( 1.0, 0.0, 0.0 );
		vec3 tx = normalize( cross( up, L ) );
		vec3 ty = cross( L, tx ) / dist;	// tx is unit and perpendicular to L, so |cross| == dist
		float r = 4.0 * dist * u_shadowParms.y;	// one cube texel (2*dist/res) * ~2 texels spread

		const vec2 disc16[16] = vec2[16](
			vec2( -0.94201624, -0.39906216 ), vec2(  0.94558609, -0.76890725 ),
			vec2( -0.09418410, -0.92938870 ), vec2(  0.34495938,  0.29387760 ),
			vec2( -0.91588581,  0.45771432 ), vec2( -0.81544232, -0.87912464 ),
			vec2( -0.38277543,  0.27676845 ), vec2(  0.97484398,  0.75648379 ),
			vec2(  0.44323325, -0.97511554 ), vec2(  0.53742981, -0.47373420 ),
			vec2( -0.26496911, -0.41893023 ), vec2(  0.79197514,  0.19090188 ),
			vec2( -0.24188840,  0.99706507 ), vec2( -0.81409955,  0.91437590 ),
			vec2(  0.19984126,  0.78641367 ), vec2(  0.14383161, -0.14100790 ) );

		vec3 txr = tx * r;
		vec3 tyr = ty * r;
		float sum = 0.0;
		for ( int i = 0; i < taps; i++ ) {
			sum += texture( u_shadowCube, vec4( L + txr * disc16[i].x + tyr * disc16[i].y, ref ) );
		}
		return sum / float( taps );
	}
	if ( var_TexProjection.w <= 0.0 ) {
		return 1.0;						// behind the light apex -> lit
	}
	vec2 uv = var_TexProjection.xy / var_TexProjection.w;	// == cookie UV, in [0,1]
	if ( uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ) {
		return 1.0;						// outside the shadow frustum -> lit
	}
	float ref = var_TexFalloff.x - u_shadowParms.z;	// falloff depth, biased for acne

	const vec2 poisson[4] = vec2[4](
		vec2( -0.94201624, -0.39906216 ), vec2(  0.94558609, -0.76890725 ),
		vec2( -0.09418410, -0.92938870 ), vec2(  0.34495938,  0.29387760 ) );
	float sum = 0.0;
	for ( int i = 0; i < 4; i++ ) {
		sum += texture( u_shadowMap, vec3( uv + poisson[i] * u_shadowParms.y, ref ) );
	}
	return sum * 0.25;
}

void main() {
	// RXGB (DXT5nm) swizzle: x lives in alpha; deliberately NOT renormalized,
	// mip filtering shortens the vector and self-shadows rough surfaces less
	vec4 bump = texture( u_bumpMap, var_TexBump );
	bump.x = bump.a;
	vec3 localNormal = bump.xyz * 2.0 - 1.0;

	// diffuse
	vec4 diffuse = texture( u_diffuseMap, var_TexDiffuse ) * u_diffuseModifier;

	// lightScale: N.L and the depth-map shadow visibility (1 for stencil-shadowed
	// and unshadowed lights, u_shadowParms.x == 0) fold into one scalar that
	// scales the light projection / falloff product below.
	float lightScale;
	vec4 spec;

	if ( u_pbrParms.z > 0.5 ) {
		// DUDE PBR path (docs/pbr-materials.md Phase A): Cook-Torrance GGX with a
		// metalness workflow, superseding the r_shading models while r_pbr is on.
		// Same tangent-space vectors as vanilla, but normalized analytically (the
		// 8-bit normalization cubemap is below GGX's precision needs). Deliberate
		// conventions:
		//  - No 1/pi on Lambert, no pi on the NDF: both cancel against Doom 3's
		//    non-physical light values, so diffuse brightness matches vanilla
		//    exactly at metalness 0.
		//  - The stock specular map has no unique PBR interpretation, so it stays
		//    a per-texel *mask* on the lobe (spec * specMap in the shared combine
		//    below), doubled like vanilla's "spec map * 2" convention so id's
		//    mid-gray authoring means full strength. r_specularScale/r_specularExp
		//    (u_specularParms.xy) are deliberately not read here.
		//  - Toksvig: the bump fetch above is deliberately un-renormalized, so its
		//    sub-unit length measures normal variance over the mip footprint; fold
		//    it into GGX alpha^2 for free per-texel roughness on stock assets.
		float nLen = clamp( length( localNormal ), 1e-4, 1.0 );
		vec3 N = localNormal / nLen;
		vec3 L = normalize( var_TexLightVec );
		vec3 V = normalize( var_TexViewVec );
		vec3 H = normalize( L + V );
		float NdotL = max( dot( N, L ), 0.0 );
		float NdotV = max( dot( N, V ), 1e-4 );
		float NdotH = max( dot( N, H ), 0.0 );
		float VdotH = max( dot( V, H ), 0.0 );

		float rough = clamp( u_pbrParms.y, 0.03, 1.0 );
		float alpha = rough * rough;
		// Toksvig widening with a calibrated baseline (in-game A/B, 2026-07-30):
		// on stock DXT5nm assets the normal-length variance carries a codec-noise
		// floor (compressed normals decode short of unit even at the top mip), and
		// full Toksvig over-widened every highlight. The baseline subtraction
		// cancels that floor while keeping the response to *real* normal variance
		// — seam edges, grate lips, minified detail — which is Toksvig's actual
		// job: those texels are exactly the specular-aliasing "fireflies".
		// The baseline lives in u_localParam1.z (r_pbrToksvigBase, default 0.2,
		// tunable in the Developer tab). Calibration history: 0.5 disabled the
		// mechanism -> white firefly pixels on panel seams; 0.1 killed the
		// fireflies but widened ordinary wall texels (|N| ~0.85-0.9), halving
		// the highlight peak ("no kick" in the Blinn A/B); 0.2 is the split —
		// |N| > ~0.83 stays tight, seams (|N| < ~0.75) keep the widening.
		float variance = max( ( 1.0 - nLen ) / nLen - u_localParam1.z, 0.0 );
		float alpha2 = min( alpha * alpha + variance, 1.0 );

		float metal = u_pbrParms.x;
		vec3 F0 = mix( vec3( 0.04 ), diffuse.rgb, metal );
		vec3 F = F0 + ( 1.0 - F0 ) * pow( 1.0 - VdotH, 5.0 );
		float d = NdotH * NdotH * ( alpha2 - 1.0 ) + 1.0;
		float D = alpha2 / ( d * d );				// GGX NDF, pi folded out (see above)
		float k = 0.5 * sqrt( alpha2 );				// Schlick-GGX k for direct light, widened alpha
		float vis = 0.25 / ( ( NdotL * ( 1.0 - k ) + k ) * ( NdotV * ( 1.0 - k ) + k ) );	// = G / (4 N.L N.V)

		// firefly clamp: GGX's peak (~1/alpha^2, plus the grazing-angle vis
		// blow-up) sits orders of magnitude above the bounded 8-bit-era energy
		// these assets were authored against (vanilla Blinn tops out ~2.4x).
		// Isolated normal-map texels aligning with H spike past the display
		// range and clip to flat white — "rows of white pixels" on panel seams
		// and grate lips. Bounding the scalar lobe restores gradation. The
		// ceiling lives in u_localParam1.w (r_pbrFireflyClamp, default 6:
		// leaves the calibrated dielectric peak ~2.2 at roughness 0.58
		// untouched, tames tight/bare-metal spikes; skin at roughness 0.40
		// rides *at* the ceiling by design — its Blinn-like bounded core).
		float lobe = min( D * vis, u_localParam1.w );

		// u_pbrParms.w = r_pbrSpecScale, the artistic energy knob for this path,
		// folded with the vanilla "spec map * 2" convention like the branch below.
		// Dielectric-weighted: the scale exists to compensate the gamma-space
		// dimming of the physical 4% dielectric F0, but metal F0 comes from the
		// albedo — already display-referred, already bright — so applying the
		// boost there double-counts and blows out bare-metal highlights (the
		// grate-floor screenshot, 2026-07-30). Metals fade to scale 1.
		spec = vec4( lobe * F, 1.0 ) * u_specularModifier
		     * ( mix( u_pbrParms.w, 1.0, metal ) * 2.0 );

		// Phase C.1 environment floor (docs/pbr-materials.md sec. 5): stock D3
		// has no environment probes to reflect (the "ambient cubemap" is a
		// constant direction, not colors), so metals with their diffuse killed
		// went black wherever the GGX alignment missed. Approximate the
		// environment as the current light's own energy arriving from all
		// directions: an F0-tinted, metalness-weighted floor added under the
		// lobe. It rides the shared `light * color` combine below, so it
		// scales with the light's projection/falloff/shadow (metals glow near
		// lights, stay dark in darkness — Doom 3's aesthetic preserved) and is
		// softened by the roughness. u_occlusionParms.w = r_pbrEnvScale.
		spec.rgb += F0 * ( metal * u_occlusionParms.w * ( 1.0 - 0.5 * rough ) );

		// Fresnel-weighted diffuse. Physical PBR kills diffuse entirely on metals
		// (metal 1 -> factor 0), but that pushes the asset's painted colour into
		// reflections stock Doom 3 can't supply, so metals read dark and off-colour.
		// u_pbrParms2.x = kd (= 1 - r_pbrMetalDiffuse) relaxes the kill: kd < 1 keeps
		// that fraction of the albedo colour while the metallic specular still rides
		// on top. kd 1 = physical, kd 0 = full albedo retained (docs sec. 5).
		diffuse.rgb *= ( 1.0 - F ) * ( 1.0 - metal * u_pbrParms2.x );
		lightScale = NdotL * shadowVisibility();
	} else {
		// half angle is normalized with math (matches the ARB program, which
		// deliberately avoided the normalization cubemap here)
		vec3 specularV = normalize( var_TexHalfVec );

		// light vector through the normalization cube map, as the original did
		vec3 lightV = texture( u_normalCubeMap, var_TexLightVec ).xyz * 2.0 - 1.0;

		lightScale = dot( lightV, localNormal ) * shadowVisibility();

		// specular term. Shading model selected by u_specularParms.z:
		//   0 = vanilla dependent LUT read on N.H (faithful default)
		//   1 = analytic Blinn-Phong pow(N.H, exp)
		// u_specularParms.x scales the result (1 = vanilla), .y is the exponent.
		int shadingModel = int( u_specularParms.z + 0.5 );
		if ( shadingModel == 0 ) {
			float sDot = dot( specularV, localNormal );
			spec = texture( u_specularTable, vec2( sDot, sDot ) );
		} else {
			// the analytic model wants a unit normal (localNormal is deliberately
			// left un-renormalized above for the diffuse/LUT path)
			vec3 nSpec = normalize( localNormal );
			float rawDot = max( dot( specularV, nSpec ), 0.0 );
			spec = vec4( pow( rawDot, u_specularParms.y ) );
		}
		// the vanilla "specular map * 2" scale is folded into the scalar factor here
		spec *= u_specularModifier * ( u_specularParms.x * 2.0 );
	}

	vec4 light = textureProj( u_lightProjection, var_TexProjection )
	           * texture( u_lightFalloff, var_TexFalloff )
	           * lightScale;

	vec4 specMap = texture( u_specularMap, var_TexSpecular );

	// DUDE GTAO on direct light (docs/ssao-gtao.md Phase C). Doom 3 is almost all dynamic
	// light with ~no ambient, so occluding the ambient pass alone is invisible; this
	// grounds direct-lit surfaces too. Applied to the diffuse term (and, with specular
	// occlusion on, the specular), scaled by r_ssaoDirectLight -- a light moving into a
	// crease can't re-light AO that's baked into the surface, so keeping it below full is
	// safer. u_localParam0 = (enable, floor, 1/viewW, 1/viewH); u_localParam1 = (direct
	// strength, specular-occlusion toggle). The AO term is floored so it never blackens.
	if ( u_localParam0.x > 0.5 ) {
		float ao = texture( u_ssao, gl_FragCoord.xy * u_localParam0.zw ).r;
		ao = mix( u_localParam0.y, 1.0, ao );
		float aoDirect = mix( 1.0, ao, u_localParam1.x );
		diffuse.rgb *= aoDirect;
		if ( u_localParam1.y > 0.5 ) {
			spec.rgb *= aoDirect;
		}
	}

	// DUDE baked ambient-occlusion map (docs/occlusion-maps.md). Same rationale as SSAO on
	// direct light: a baked-dark crease can't be re-lit by a moving light, so the map's pull
	// on direct diffuse is scaled (u_occlusionParms.z, from r_occlusionMapScale * direct) and
	// stays below full by default. Sampled with the diffuse UV; stacks with SSAO when both on.
	if ( u_occlusionParms.x > 0.5 ) {
		float aoMap = texture( u_occlusionMap, var_TexDiffuse ).r;
		diffuse.rgb *= mix( 1.0, aoMap, u_occlusionParms.z );
	}

	vec4 color = spec * specMap + diffuse;

	// Floor negatives to 0 to match the 8-bit target's fixed-point clamp. N.L (the
	// `light` term above) goes negative on pixels facing away from this light; on the
	// SDR backbuffer that's clamped to 0 before the additive blend, but the RGBA16F
	// HDR target doesn't clamp fragment output, so a negative would *subtract* this
	// (often warm) light and cool-shift normal-mapped models — the r_hdr blue-tint
	// bug. max() restores the [0, inf) floor while keeping HDR's >1 highlights; it's a
	// no-op on the 8-bit path, which already floored here.
	fragColor = max( light * color * var_Color, vec4( 0.0 ) );
}
