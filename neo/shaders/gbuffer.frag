// DUDE SSAO normal G-buffer (docs/ssao-gtao.md). RGB = bump-mapped view-space normal
// (encoded to [0,1]) that ssao.frag samples in place of depth-reconstructed normals.
// A = AO mask: 1 for normal surfaces, 0 for the view weapon (u_localParam0.x), so SSAO
// can skip the depth-hacked weapon whose depth confuses the horizon search.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_bumpMap;

VARY(0) in vec2 var_TexBump;
VARY(1) in vec3 var_T;
VARY(2) in vec3 var_B;
VARY(3) in vec3 var_N;

layout(location = 0) out vec4 fragColor;

void main() {
	// RXGB (DXT5nm) swizzle: x lives in alpha (matches interaction.frag / ambientlight.frag)
	vec4 bump = texture( u_bumpMap, var_TexBump );
	bump.x = bump.a;
	vec3 tn = bump.xyz * 2.0 - 1.0;

	// tangent-space bump normal -> view space. Store it as-is: back-face culling already
	// makes visible surfaces' normals face the camera, and the true bumped normal must be
	// kept even where a grazing bump tips its z past zero. Do NOT flip on sign(N.z) — that
	// inverts the whole normal at the horizon and makes it flicker between a value and its
	// opposite ("which axis is correct").
	vec3 N = normalize( var_T * tn.x + var_B * tn.y + var_N * tn.z );

	fragColor = vec4( N * 0.5 + 0.5, u_localParam0.x );   // A = AO mask (0 = weapon, skip)
}
