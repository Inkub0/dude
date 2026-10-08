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
SAMPLER_BINDING(2) uniform sampler2D u_diffuseAlbedo; // RTGI (docs/rtx-rtgi.md H6c): surface diffuse, written to out_Albedo
SAMPLER_BINDING(3) uniform sampler2D u_specularMask;  // ReSTIR DI (docs/rtx-restir-di.md Stage D): the surface's specular map, luminance -> out_Material.b

VARY(0) in vec2 var_TexBump;
VARY(1) in vec3 var_T;
VARY(2) in vec3 var_B;
VARY(3) in vec3 var_N;
VARY(4) in vec2 var_TexCoverage;
// motion vectors (R1/A2): perspective-correct current/previous clip for the per-pixel velocity
// write. Present on every gbuffer draw; out_Velocity is discarded on the 1-/2-MRT SSAO/SSR-only
// targets that carry no 3rd attachment.
VARY(7) in vec4 var_CurClip;
VARY(8) in vec4 var_PrevClip;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 out_Material;   // SSR: (roughness, metalness, spec-map luminance [DI apply only, else 0], 1)
layout(location = 2) out vec2 out_Velocity;   // R1/A2 motion vector: currUV - prevUV, +Y-up
layout(location = 3) out vec4 out_Albedo;     // RTGI H6c: receiver albedo; only attached on the 4-MRT RTGI prepass (else GL discards the write)

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
	// .b = the specular map's luminance (ReSTIR DI Stage D: the deferred DI shade masks its GGX lobe with it, as
	// interaction.frag masks with spec * specMap). Sampled with the diffuse UV (var_TexCoverage), only on the
	// 4-MRT albedo prepass (u_localParam0.y) - black otherwise, and nothing else reads .b.
	float specLum = ( u_localParam0.y > 0.5 ) ? dot( texture( u_specularMask, var_TexCoverage ).rgb, vec3( 0.2126, 0.7152, 0.0722 ) ) : 0.0;
	out_Material = vec4( u_pbrParms.y, u_pbrParms.x, specLum, 1.0 );

	// per-pixel screen velocity (docs/fsr-temporal-pipeline.md R1/A2): divide each interpolated
	// clip by its OWN w so large world triangles reproject correctly, take the NDC delta, scale
	// by 0.5 into [0,1] UV units. Canonical +Y-up direction currUV - prevUV: the SSAO/SSR
	// consumers fetch history at currentUV - velocity; the FSR2 dispatch (C2) applies the top-left
	// Y-flip via motionVectorScale. u_localParam1.xy cancels the per-frame projection jitter (R1/B;
	// 0 when r_temporalJitter is off) so the jitter never registers as motion.
	// Zero velocity by BRANCHING, not by multiplying by a mask (prevUV = xy/w is NaN/Inf when w is 0,
	// and NaN * 0.0 is still NaN, which poisoned every consumer — FSR2 black weapon + possible
	// device-lost, smeared SSAO/RTAO), when either:
	//   - the clip w is degenerate: a dummy/zero previous MVP (first sighting / despawn gap) or a
	//     vertex on/behind the near plane. This alone covers the NaN sources.
	//   - u_localParam0.z is set: the caller force-zeroes this surface's velocity. Used ONLY for the
	//     depth-hacked view weapon when r_weaponMotionVectors is off (the old behaviour). Default the
	//     weapon gets REAL motion vectors — its depth hack touches Z only, so xy/w velocity is valid,
	//     and without it FSR2 garbles the weapon's bob/sway and the GUI displays on it. The AO/weapon
	//     mask (u_localParam0.x -> fragColor.a) is a SEPARATE signal and is unchanged either way.
	if ( u_localParam0.z > 0.5 || var_CurClip.w <= 0.0 || var_PrevClip.w <= 0.0 ) {
		out_Velocity = vec2( 0.0 );
	} else {
		vec2 curUV  = var_CurClip.xy  / var_CurClip.w;
		vec2 prevUV = var_PrevClip.xy / var_PrevClip.w;
		out_Velocity = ( curUV - prevUV ) * 0.5 + u_localParam1.xy;
	}

	// RTGI receiver albedo (docs/rtx-rtgi.md H6c): the surface's diffuse colour, so the fullscreen GI
	// apply can shade albedo * gathered-irradiance per pixel. u_localParam0.y gates it (1 only on the
	// 4-MRT RTGI prepass); var_TexCoverage is the diffuse-matrix UV (filled for every surface there).
	// On the 1/2/3-attachment prepass targets there is no 4th attachment, so the write is discarded.
	out_Albedo = ( u_localParam0.y > 0.5 ) ? vec4( texture( u_diffuseAlbedo, var_TexCoverage ).rgb, 1.0 ) : vec4( 0.0 );
}
