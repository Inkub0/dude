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
		vec3 ty = normalize( cross( L, tx ) );
		float r = ( 2.0 * dist * u_shadowParms.y ) * 2.0;	// one cube texel * ~2 texels spread

		const vec2 disc16[16] = vec2[16](
			vec2( -0.94201624, -0.39906216 ), vec2(  0.94558609, -0.76890725 ),
			vec2( -0.09418410, -0.92938870 ), vec2(  0.34495938,  0.29387760 ),
			vec2( -0.91588581,  0.45771432 ), vec2( -0.81544232, -0.87912464 ),
			vec2( -0.38277543,  0.27676845 ), vec2(  0.97484398,  0.75648379 ),
			vec2(  0.44323325, -0.97511554 ), vec2(  0.53742981, -0.47373420 ),
			vec2( -0.26496911, -0.41893023 ), vec2(  0.79197514,  0.19090188 ),
			vec2( -0.24188840,  0.99706507 ), vec2( -0.81409955,  0.91437590 ),
			vec2(  0.19984126,  0.78641367 ), vec2(  0.14383161, -0.14100790 ) );

		float sum = 0.0;
		for ( int i = 0; i < taps; i++ ) {
			vec3 off = tx * disc16[i].x * r + ty * disc16[i].y * r;
			sum += texture( u_shadowCube, vec4( L + off, ref ) );
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
	// half angle is normalized with math (matches the ARB program, which
	// deliberately avoided the normalization cubemap here)
	vec3 specularV = normalize( var_TexHalfVec );

	// light vector through the normalization cube map, as the original did
	vec3 lightV = texture( u_normalCubeMap, var_TexLightVec ).xyz * 2.0 - 1.0;

	// RXGB (DXT5nm) swizzle: x lives in alpha; deliberately NOT renormalized,
	// mip filtering shortens the vector and self-shadows rough surfaces less
	vec4 bump = texture( u_bumpMap, var_TexBump );
	bump.x = bump.a;
	vec3 localNormal = bump.xyz * 2.0 - 1.0;

	vec4 light = vec4( dot( lightV, localNormal ) );

	// modulate by the light projection and falloff
	light *= textureProj( u_lightProjection, var_TexProjection );
	light *= texture( u_lightFalloff, var_TexFalloff );

	// shadow-mapped lights attenuate the light term by depth-map visibility;
	// stencil-shadowed and unshadowed lights leave it at 1 (u_shadowParms.x == 0)
	light *= shadowVisibility();

	// diffuse
	vec4 diffuse = texture( u_diffuseMap, var_TexDiffuse ) * u_diffuseModifier;

	// specular term. Shading model selected by u_specularParms.z:
	//   0 = vanilla dependent LUT read on N.H (faithful default)
	//   1 = analytic Blinn-Phong pow(N.H, exp)
	//   2 = analytic Phong pow(R.V, exp)
	// u_specularParms.x scales the result (1 = vanilla), .y is the exponent.
	int shadingModel = int( u_specularParms.z + 0.5 );
	vec4 spec;
	if ( shadingModel == 0 ) {
		float sDot = dot( specularV, localNormal );
		spec = texture( u_specularTable, vec2( sDot, sDot ) );
	} else {
		// analytic models want a unit normal (localNormal is deliberately left
		// un-renormalized above for the diffuse/LUT path)
		vec3 nSpec = normalize( localNormal );
		float rawDot;
		if ( shadingModel == 2 ) {
			vec3 R = reflect( -lightV, nSpec );
			rawDot = max( dot( R, normalize( var_TexViewVec ) ), 0.0 );
		} else {
			rawDot = max( dot( specularV, nSpec ), 0.0 );
		}
		spec = vec4( pow( rawDot, u_specularParms.y ) );
	}
	spec *= u_specularModifier * u_specularParms.x;
	vec4 specMap = texture( u_specularMap, var_TexSpecular ) * 2.0;

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

	fragColor = light * color * var_Color;
}
