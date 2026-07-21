// Translated from glprogs/environment.vfp (fragment program).

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform samplerCube u_environmentCubeMap;

VARY(0) in vec3 var_Normal;
VARY(1) in vec3 var_ToEye;
VARY(2) in vec4 var_Color;

layout(location = 0) out vec4 fragColor;

void main() {
	vec3 normal = normalize( var_Normal );
	vec3 toEye = normalize( var_ToEye );

	// reflection vector: 2*(E.N)*N - E
	vec3 r = 2.0 * dot( toEye, normal ) * normal - toEye;

	fragColor = texture( u_environmentCubeMap, r ) * var_Color;
}
