// DUDE SSAO normal G-buffer (docs/ssao-gtao.md). RGB = bump-mapped view-space normal
// (encoded to [0,1]) that ssao.frag samples in place of depth-reconstructed normals.
// A = AO mask: 1 for normal surfaces, 0 for the view weapon (u_localParam0.x), so SSAO
// can skip the depth-hacked weapon whose depth confuses the horizon search.
//
// Output 1 (docs/ssr.md, only attached when r_ssr is on): the surface's resolved PBR
// response for the reflection composite — R = roughness, G = metalness (u_pbrParms.yx,
// filled by RB_RHI_NormalPrepass). Without the attachment GL discards the write.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_bumpMap;
SAMPLER_BINDING(1) uniform sampler2D u_coverageMap;   // diffuse alpha for perforated surfaces

VARY(0) in vec2 var_TexBump;
VARY(1) in vec3 var_T;
VARY(2) in vec3 var_B;
VARY(3) in vec3 var_N;
VARY(4) in vec2 var_TexCoverage;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 out_Material;   // SSR: (roughness, metalness, 0, 1)

void main() {
	// Perforated surfaces (grates, cables, foliage) are flat cards whose diffuse alpha masks
	// the visible shape. Punch those texels out of the normal buffer so SSAO sees the geometry
	// behind the card instead of the solid rectangle — the same coverage the depth prepass
	// seals (zfill.frag). Opaque surfaces bind whiteImage with the test disabled
	// (u_alphaTest.y == 0), so the && short-circuits the fetch away.
	if ( u_alphaTest.y != 0.0 && texture( u_coverageMap, var_TexCoverage ).a < u_alphaTest.x ) {
		discard;
	}

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
	out_Material = vec4( u_pbrParms.y, u_pbrParms.x, 0.0, 1.0 );
}
