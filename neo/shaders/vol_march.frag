// DUDE volumetric lighting, Route A (docs/volumetric-lighting.md): low-res ray march of the view ray from
// the eye to the opaque depth, accumulating the light the air scatters toward the camera. The per-sample
// shading lives in vol_scatter.glsl (shared with the future froxel route); this file is only the march.
// Output rgb = in-scattered radiance (HDR), a = transmittance of the scene behind (1 unless r_volExtinction).
//
// VK + ray-query only (RhiWorld.cpp RB_RHI_VolumetricLighting). Uniform packing:
//   u_rtParms.xy       = scene TLAS device address, bit-cast (0 -> shadows forced off by the backend)
//   u_color            = (scene-light buffer address lo/hi bit-cast, light count, fog-volume row count)
//   u_modelViewMatrix  = inverse view (view -> world)
//   u_localParam0      = (1/proj00, 1/proj11, scattering coefficient per world unit, intensity)
//   u_localParam1      = (steps, max march distance, HG anisotropy g, shadows enable)
//   u_windowCoord      = (frame index for the jitter hash, light mode, view-Y sign, extinction scale)
//   u_diffuseModifier   = (point-light scale, first-person weapon light scale, point-light shadows enable,
//                          point-light scale inside stock fog volumes)
//   u_screenCorrection = 1 / target size; u_depthTexRecip = target px -> depth-texture tc
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference_uvec2 : require

#include "renderparms.glsl"
#include "rt_light_eval.glsl"
#include "vol_scatter.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentDepth;

VARY(0) in vec2 var_TexCoord;
layout(location = 0) out vec4 fragColor;

#define depth_consts DUDE_DEPTH_CONSTS()

void main() {
	vec2  frag = gl_FragCoord.xy;
	float raw  = texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;
	float maxDist = u_localParam1.y;
	// view depth to march to: the opaque surface, or the distance cap over sky / far geometry
	float zEnd = ( raw >= 0.9994 ) ? maxDist : min( -1.0 / ( raw * depth_consts.x + depth_consts.y ), maxDist );

	// world-space view ray. dirV is the view-space ray per unit of view depth (z = -1), so its world
	// length converts view depth to distance along the ray (same reconstruction as ssr_rt / rtao_ray).
	vec2  ndc  = frag * u_screenCorrection.xy * 2.0 - 1.0;
	vec3  dirV = vec3( ndc.x * u_localParam0.x, ndc.y * u_windowCoord.z * u_localParam0.y, -1.0 );
	vec3  eyeW = ( u_modelViewMatrix * vec4( 0.0, 0.0, 0.0, 1.0 ) ).xyz;
	vec3  dW   = mat3( u_modelViewMatrix ) * dirV;
	float perZ = length( dW );
	vec3  viewDir = dW / perZ;
	float dist = zEnd * perZ;

	RtLights lights  = RtLights( uvec2( floatBitsToUint( u_color.x ), floatBitsToUint( u_color.y ) ) );
	uint  nLights    = uint( u_color.z );
	uvec2 tlasAddr   = uvec2( floatBitsToUint( u_rtParms.x ), floatBitsToUint( u_rtParms.y ) );
	bool  shadows    = u_localParam1.w > 0.5;
	uint  lightMode  = uint( u_windowCoord.y );
	float sigmaS     = u_localParam0.z;
	float sigmaT     = sigmaS * u_windowCoord.w;
	float g          = u_localParam1.z;
	int   steps      = int( u_localParam1.x );

	// the lights that can touch THIS view ray, culled once - the march below never loops the full buffer
	uint  cand[ DUDE_VOL_MAX_LIGHTS ];
	vec4  candSph[ DUDE_VOL_MAX_LIGHTS ];
	vec2  lit;																// [near, far] of the ray the candidate lights cover
	uint  nCand = dudeVolCullRay( lights, nLights, lightMode, eyeW, viewDir, dist, cand, candSph, lit );
	if ( nCand == 0u || lit.y <= lit.x ) { fragColor = vec4( 0.0, 0.0, 0.0, exp( -sigmaT * dist ) ); return; }	// unlit ray: extinction only
	// stock fog volumes on this ray (rows behind the lights): inside one, the point-light haze thickens
	uint  fogIdx[ DUDE_VOL_MAX_FOGS ];
	vec4  fogSph[ DUDE_VOL_MAX_FOGS ];
	uint  nFogCand = ( lightMode >= 2u )
		? dudeVolCullFogRay( lights, nLights, uint( u_color.w ), eyeW, viewDir, dist, fogIdx, fogSph ) : 0u;

	// Per-pixel, per-FRAME offset of the sample positions inside each step's own slice. Three lessons:
	//  - SPATIALLY it should be interleaved gradient noise: any 3x3 window of IGN holds well-spread values,
	//    so the low-res blur resolves one frame almost completely. White noise does not - at quarter res
	//    its low-frequency energy survives even a 7x7 blur as huge blotchy pixels.
	//  - ACROSS FRAMES it must not be the same pattern shifted (adding a phase to IGN's value = sliding the
	//    pattern): a constant shift made the residue drift up the screen, a jumping one left it striped.
	//  - so each frame uses IGN in a different ORIENTATION (axes swapped / mirrored: 4 distinct stripe
	//    directions) at a hashed offset. Every frame still blurs cleanly; the per-frame residues point in
	//    different directions and average out in the accumulation instead of reinforcing one another.
	uint  fh = uint( u_windowCoord.x ) * 747796405u + 2891336453u;
	fh = ( ( fh >> ( ( fh >> 28u ) + 4u ) ) ^ fh ) * 277803737u;
	fh = ( fh >> 22u ) ^ fh;
	vec2  jc = frag + vec2( float( ( fh >> 3u ) & 1023u ), float( ( fh >> 13u ) & 1023u ) );
	if ( ( fh & 1u ) != 0u ) { jc = jc.yx; }
	if ( ( fh & 2u ) != 0u ) { jc.x = -jc.x; }
	if ( ( fh & 4u ) != 0u ) { jc.y = -jc.y; }
	float jitter = fract( 52.9829189 * fract( dot( jc, vec2( 0.06711056, 0.00583715 ) ) ) );

	// quadratic step distribution over the LIT stretch: denser toward the eye, where a shaft's edge subtends more pixels
	vec3  rgb = vec3( 0.0 );
	float T   = 1.0;
	float inv = 1.0 / float( steps );
	for ( int i = 0; i < steps; i++ ) {
		float b0 = float( i ) * inv;
		float b1 = b0 + inv;
		float a  = b0 + jitter * inv;
		float span = lit.y - lit.x;
		float dt = span * ( b1 * b1 - b0 * b0 );
		vec3  p  = eyeW + viewDir * ( lit.x + span * a * a );
		float pointScale = u_diffuseModifier.x;
		if ( nFogCand > 0u ) {
			pointScale = mix( pointScale, max( pointScale, u_diffuseModifier.w ), dudeVolFogFactor( lights, fogIdx, fogSph, nFogCand, p ) );
		}
		rgb += dudeVolInScatter( lights, cand, candSph, nCand, tlasAddr, shadows, u_diffuseModifier.z > 0.5, p, viewDir, g, pointScale, u_diffuseModifier.y ) * ( T * sigmaS * dt );
		T   *= exp( -sigmaT * dt );
	}
	T *= exp( -sigmaT * ( dist - ( lit.y - lit.x ) ) );					// extinction over the unlit remainder of the ray
	fragColor = vec4( rgb * u_localParam0.w, T );
}
