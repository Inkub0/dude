// DUDE screen-space reflections — COMPOSITE pass (docs/ssr.md, PBR Phase C.2.1).
// Full-resolution additive pass over the lit opaque scene: upsamples the marched
// (and optionally temporally-accumulated) reflection buffer and weights it by the
// material response computed HERE, at full resolution — Schlick Fresnel from the
// G-buffer normal x gloss window x r_ssrIntensity. Keeping the weighting at full
// res means a half-res march only softens the reflected image, never the
// material/Fresnel edges.
//
// Uniform packing (RB_RHI_ScreenSpaceReflections):
//   u_localParam0.xy      = ( 1/proj00, 1/proj11 ) view-pos reconstruction; .z = roughness fade start (frac of cutoff); .w = firefly clamp (reflected-radiance cap, 0 = off)
//   u_localParam1         = ( 0, intensity, maxRoughness, glossyMaxLod )
//   u_screenCorrection.xy = 1 / viewSize (gl_FragCoord -> [0,1] uv)
//   u_depthTexRecip.xy    = gl_FragCoord -> _currentDepth texcoord
//
// Glossy reflections (r_ssrGlossy): when u_localParam1.w (glossyMaxLod) > 0 the
// reflection sampler is the ssr_colordown mip pyramid, and the reflection is read
// at a roughness-proportional LOD so rough surfaces blur. w == 0 = the sharp path:
// unit 0 is the single-level result buffer and we sample level 0 exactly (unchanged).

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_ssr;             // marched reflection (LINEAR upsample)
SAMPLER_BINDING(1) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(2) uniform sampler2D u_normalBuffer;    // xyz = view normal, a = weapon mask
SAMPLER_BINDING(3) uniform sampler2D u_materialBuffer;  // x = roughness, y = metalness

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	vec2  frag = gl_FragCoord.xy;
	float raw  = texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;
	if ( raw >= 0.9994 ) {
		discard;                                   // sky / no geometry
	}

	vec2 uv = frag * u_screenCorrection.xy;
	vec4 nt = texture( u_normalBuffer, uv );
	if ( nt.a < 0.5 ) {
		discard;                                   // view weapon
	}
	vec4  mt    = texture( u_materialBuffer, uv );
	float rough = mt.x;
	float metal = mt.y;

	// gloss window: full strength up to r_ssrRoughnessFade (localParam0.z) of the cutoff, fading to 0 at it
	float maxRough = u_localParam1.z;
	float gloss = 1.0 - smoothstep( maxRough * u_localParam0.z, maxRough, rough );
	if ( gloss < 0.004 ) {
		discard;
	}

	// View direction for NdotV. The view-space position is P = depth * dir with
	// dir = ( ndc.x/proj00, ndc.y*ySign/proj11, -1 ) and depth > 0, so V = normalize(-P)
	// is INVARIANT to the (positive) depth: this pass only needs the ray's DIRECTION, never
	// its length, so the linear-eye-z divide the other SSR passes compute is unnecessary here
	// (raw is still used for the sky discard above). u_windowCoord.z = view-Y sign (VK flip).
	vec2  ndc = uv * 2.0 - 1.0;
	vec3  N   = normalize( nt.xyz * 2.0 - 1.0 );
	vec3  V   = normalize( vec3( -ndc.x * u_localParam0.x, -ndc.y * u_windowCoord.z * u_localParam0.y, 1.0 ) );

	// Schlick Fresnel: dielectrics (floor tiles) reflect mostly at grazing angles,
	// metals at all angles. F0 0.9 (not albedo — no albedo buffer) keeps untinted
	// metal reflections, which reads right on Doom 3's grey steel.
	float NdotV = clamp( dot( N, V ), 0.0, 1.0 );
	float F0 = mix( 0.04, 0.9, metal );
	float m  = 1.0 - NdotV;
	float m2 = m * m;
	float F  = F0 + ( 1.0 - F0 ) * ( m2 * m2 * m );	// pow(1-NdotV,5) as 3 muls: exact, no exp2/log2

	// reflection colour. Sharp path (glossyMaxLod == 0): a single-level bilinear
	// upsample, byte-identical to the pre-glossy build. Glossy path: sample the
	// reflection mip pyramid at a roughness-proportional LOD and hand-blend the two
	// adjacent levels (the mip RT is LINEAR_MIPMAP_NEAREST, so an explicit two-tap
	// mix gives the trilinear smoothness without a per-backend sampler change).
	float maxLod = u_localParam1.w;
	vec3 refl;
	if ( maxLod > 0.0 ) {
		float lod = clamp( ( rough / maxRough ) * maxLod, 0.0, maxLod );
		float l0  = floor( lod );
		vec3  a   = textureLod( u_ssr, uv, l0 ).rgb;
		vec3  b   = textureLod( u_ssr, uv, min( l0 + 1.0, maxLod ) ).rgb;
		refl = mix( a, b, lod - l0 );
	} else {
		refl = texture( u_ssr, uv ).rgb;           // low-res march, bilinear upsample
	}
	// firefly clamp: cap the reflected HDR luminance so a bright reflected light / GUI / specular
	// highlight can't spike into a hot speckle on the floor (r_ssrFireflyClamp; localParam0.w, 0 = off).
	// Luminance-preserving, so the reflection keeps its hue.
	float fcap = u_localParam0.w;
	if ( fcap > 0.0 ) {
		float lum = dot( refl, vec3( 0.2126, 0.7152, 0.0722 ) );
		if ( lum > fcap ) { refl *= fcap / lum; }
	}
	fragColor = vec4( refl * ( F * gloss * u_localParam1.y ), 0.0 );
}
