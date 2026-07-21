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

VARY(0) in vec3 var_TexLightVec;
VARY(1) in vec2 var_TexBump;
VARY(2) in vec2 var_TexFalloff;
VARY(3) in vec4 var_TexProjection;
VARY(4) in vec2 var_TexDiffuse;
VARY(5) in vec2 var_TexSpecular;
VARY(6) in vec3 var_TexHalfVec;
VARY(7) in vec4 var_Color;

layout(location = 0) out vec4 fragColor;

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

	// diffuse
	vec4 color = texture( u_diffuseMap, var_TexDiffuse ) * u_diffuseModifier;

	// specular: dependent LUT read on N.H, modulated by specular map * 2
	float sDot = dot( specularV, localNormal );
	vec4 spec = texture( u_specularTable, vec2( sDot, sDot ) ) * u_specularModifier;
	vec4 specMap = texture( u_specularMap, var_TexSpecular ) * 2.0;
	color = spec * specMap + color;

	fragColor = light * color * var_Color;
}
