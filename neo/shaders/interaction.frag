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
SAMPLER_BINDING(7) uniform sampler2DShadow u_shadowMap; // depth map of a shadow-mapped light

VARY(0) in vec3 var_TexLightVec;
VARY(1) in vec2 var_TexBump;
VARY(2) in vec2 var_TexFalloff;
VARY(3) in vec4 var_TexProjection;
VARY(4) in vec2 var_TexDiffuse;
VARY(5) in vec2 var_TexSpecular;
VARY(6) in vec3 var_TexHalfVec;
VARY(7) in vec4 var_Color;
VARY(8) in vec3 var_TexViewVec;

layout(location = 0) out vec4 fragColor;

// 0 = fully shadowed, 1 = fully lit. The lookup reuses the light-projection
// texgen: var_TexProjection gives the same (S/Q, T/Q) UV as the light cookie, so
// the shadow aligns exactly with the lit cone, and var_TexFalloff.x is the linear
// distance along the light axis — the very value the caster pass wrote as depth.
// Hardware depth-compare sampler (2x2 PCF) plus a 4-tap Poisson spread for softer
// edges. Faithful default disables this (u_shadowParms.x == 0), so vanilla and
// stencil-shadowed lights are untouched.
float shadowVisibility() {
	if ( u_shadowParms.x == 0.0 ) {
		return 1.0;
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
	vec4 color = texture( u_diffuseMap, var_TexDiffuse ) * u_diffuseModifier;

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
	color = spec * specMap + color;

	fragColor = light * color * var_Color;
}
