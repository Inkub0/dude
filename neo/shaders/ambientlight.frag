// Translated from glprogs/ambientLight.vfp (fragment program).

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform samplerCube u_ambientCubeMap;
SAMPLER_BINDING(1) uniform sampler2D u_bumpMap;
SAMPLER_BINDING(2) uniform sampler2D u_lightFalloff;
SAMPLER_BINDING(3) uniform sampler2D u_lightProjection;
SAMPLER_BINDING(4) uniform sampler2D u_diffuseMap;

VARY(0) in vec2 var_TexBump;
VARY(1) in vec2 var_TexDiffuse;
VARY(2) in vec2 var_TexFalloff;
VARY(3) in vec4 var_TexProjection;
VARY(4) in vec3 var_ToGlobalRow0;
VARY(5) in vec3 var_ToGlobalRow1;
VARY(6) in vec3 var_ToGlobalRow2;
VARY(7) in vec4 var_Color;

layout(location = 0) out vec4 fragColor;

void main() {
	// local space surface normal, RXGB swizzle
	vec4 bump = texture( u_bumpMap, var_TexBump );
	bump.x = bump.a;
	vec3 localNormal = bump.xyz * 2.0 - 1.0;

	// transform into ambient map space
	vec3 globalNormal = vec3( dot( localNormal, var_ToGlobalRow0 ),
	                          dot( localNormal, var_ToGlobalRow1 ),
	                          dot( localNormal, var_ToGlobalRow2 ) );

	vec4 light = texture( u_ambientCubeMap, globalNormal );

	light *= texture( u_diffuseMap, var_TexDiffuse );
	light *= texture( u_lightFalloff, var_TexFalloff );
	light *= textureProj( u_lightProjection, var_TexProjection );
	light *= u_diffuseModifier;

	// original wrote result.color.xyz only; alpha pinned to 1
	fragColor = vec4( light.xyz * var_Color.xyz, 1.0 );
}
