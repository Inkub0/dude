// Translated from glprogs/bumpyEnvironment.vfp (fragment program).

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform samplerCube u_environmentCubeMap;
SAMPLER_BINDING(1) uniform sampler2D u_bumpMap;

VARY(0) in vec2 var_TexCoord;
VARY(1) in vec3 var_ToEyeGlobal;
VARY(2) in vec3 var_ToGlobalRow0;
VARY(3) in vec3 var_ToGlobalRow1;
VARY(4) in vec3 var_ToGlobalRow2;
VARY(5) in vec4 var_Color;

layout(location = 0) out vec4 fragColor;

void main() {
	// normal map with RXGB swizzle, normalized to full scale
	vec4 bump = texture( u_bumpMap, var_TexCoord );
	bump.x = bump.a;
	vec3 localNormal = normalize( bump.xyz * 2.0 - 1.0 );

	// transform to global space
	vec3 globalNormal = vec3( dot( localNormal, var_ToGlobalRow0 ),
	                          dot( localNormal, var_ToGlobalRow1 ),
	                          dot( localNormal, var_ToGlobalRow2 ) );

	vec3 globalEye = normalize( var_ToEyeGlobal );

	// reflection vector
	vec3 r = 2.0 * dot( globalEye, globalNormal ) * globalNormal - globalEye;

	// original wrote rgb only (vertex color multiply was commented out)
	fragColor = vec4( texture( u_environmentCubeMap, r ).xyz, 1.0 );
}
