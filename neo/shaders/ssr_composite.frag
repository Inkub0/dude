// DUDE screen-space reflections — COMPOSITE pass (docs/ssr.md, PBR Phase C.2.1).
// Full-resolution additive pass over the lit opaque scene: upsamples the marched
// (and optionally temporally-accumulated) reflection buffer and weights it by the
// material response computed HERE, at full resolution — Schlick Fresnel from the
// G-buffer normal x gloss window x r_ssrIntensity. Keeping the weighting at full
// res means a half-res march only softens the reflected image, never the
// material/Fresnel edges.
//
// Uniform packing (RB_RHI_ScreenSpaceReflections):
//   u_localParam0.xy      = ( 1/proj00, 1/proj11 ) view-pos reconstruction
//   u_localParam1         = ( 0, intensity, maxRoughness, 0 )
//   u_screenCorrection.xy = 1 / viewSize (gl_FragCoord -> [0,1] uv)
//   u_depthTexRecip.xy    = gl_FragCoord -> _currentDepth texcoord

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_ssr;             // marched reflection (LINEAR upsample)
SAMPLER_BINDING(1) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(2) uniform sampler2D u_normalBuffer;    // xyz = view normal, a = weapon mask
SAMPLER_BINDING(3) uniform sampler2D u_materialBuffer;  // x = roughness, y = metalness

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

// Doom 3's fixed near / near-infinite far projection in GL clip depth -> linear
// eye z (negative). Same constants as ssao.frag / ssr.frag.
const vec2 depth_consts = vec2( 0.33333333, -0.33316667 );

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

	// gloss window: full strength up to 70% of the roughness cutoff, fading to 0 at it
	float maxRough = u_localParam1.z;
	float gloss = 1.0 - smoothstep( maxRough * 0.7, maxRough, rough );
	if ( gloss < 0.004 ) {
		discard;
	}

	// view-space position for NdotV (same reconstruction as ssr.frag)
	float vz  = 1.0 / ( raw * depth_consts.x + depth_consts.y );      // negative
	vec2  ndc = uv * 2.0 - 1.0;
	float d   = -vz;
	vec3  P   = vec3( ndc.x * d * u_localParam0.x, ndc.y * d * u_localParam0.y, vz );
	vec3  N   = normalize( nt.xyz * 2.0 - 1.0 );
	vec3  V   = normalize( -P );

	// Schlick Fresnel: dielectrics (floor tiles) reflect mostly at grazing angles,
	// metals at all angles. F0 0.9 (not albedo — no albedo buffer) keeps untinted
	// metal reflections, which reads right on Doom 3's grey steel.
	float NdotV = clamp( dot( N, V ), 0.0, 1.0 );
	float F0 = mix( 0.04, 0.9, metal );
	float F  = F0 + ( 1.0 - F0 ) * pow( 1.0 - NdotV, 5.0 );

	vec3 refl = texture( u_ssr, uv ).rgb;          // low-res march, bilinear upsample
	fragColor = vec4( refl * ( F * gloss * u_localParam1.y ), 0.0 );
}
