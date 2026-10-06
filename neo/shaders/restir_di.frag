// DUDE ReSTIR DI — Stage 1 (docs/rtx-restir-di.md): RIS initial-candidate resampling, NO temporal /
// spatial reuse and NO visibility ray yet. One shaded sample per pixel: pick ONE scene light by
// weighted reservoir sampling (WRS) over M candidates drawn uniformly from the scene-light buffer,
// then resolve the unbiased UNSHADOWED direct irradiance (albedo = 1, diffuse-only target).
//
// The output is a 1-spp estimate of the full all-lights sum; its EXPECTATION equals the brute
// r_rtAllLights UNSHADOWED sum (that is the Stage 1b validator). It is NOISY by design — temporal +
// spatial reuse and NRD (Stages 3-5) resolve it; specular enters the target in Stage 6. No reservoir
// SSBO yet: with no reuse the reservoir is a per-pixel local, so the persistent ping-pong buffer only
// arrives with temporal reuse (Stage 3).
//
// VK + RT hardware tier (gated with r_rtReStirDI like the other RT passes), but Stage 1 traces NO ray:
// it needs the light buffer, not the TLAS. G-buffer reconstruction mirrors rtao_ray.frag / ssr_rt.frag.
//
// Uniform packing (RB_RHI_ReStirDIPass):
//   u_modelViewMatrix     = inverse view (view -> world)
//   u_color.xy            = scene-light buffer device address (bit-cast lo/hi)
//   u_color.z             = light count
//   u_localParam0         = ( 1/proj00, 1/proj11, candidate count M, unused )
//   u_localParam1.x       = frame index (RNG decorrelation)
//   u_screenCorrection.xy = 1 / viewSize        (gl_FragCoord -> [0,1] uv; normal buffer)
//   u_depthTexRecip.xy    = gl_FragCoord -> _currentDepth tc
//   u_windowCoord.z       = view-Y sign (-1 Vulkan top-down)
#extension GL_EXT_buffer_reference_uvec2 : require	// build the light-buffer pointer from u_color.xy (uvec2)

#include "renderparms.glsl"
#include "rt_light_eval.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(1) uniform sampler2D u_normalBuffer;	// xyz = view-space normal, a = weapon/AO mask

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;	// RGBA16F: unshadowed direct irradiance (rgb), 1 (a)

// depth -> view z pair for THIS view (cinematics lower the near plane; renderparms.glsl u_depthParms)
#define depth_consts DUDE_DEPTH_CONSTS()

// PCG hash; seed the stream, then draw successive [0,1) uniforms (advance-then-output).
float rnd( inout uint rng ) {
	rng = rng * 747796405u + 2891336453u;
	uint w = ( ( rng >> ( ( rng >> 28u ) + 4u ) ) ^ rng ) * 277803737u;
	w = ( w >> 22u ) ^ w;
	return float( w ) * ( 1.0 / 4294967296.0 );
}

void main() {
	vec2  frag = gl_FragCoord.xy;
	float raw  = texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;
	if ( raw >= 0.9994 ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }	// sky / no geometry
	vec4 nt = texture( u_normalBuffer, frag * u_screenCorrection.xy );
	if ( nt.a < 0.5 ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }	// view weapon: skip

	// view-space position + normal (ssao/rtao reconstruction incl. the VK view-Y sign)
	float vz  = 1.0 / ( min( raw, 0.9994 ) * depth_consts.x + depth_consts.y );	// negative
	vec2  ndc = frag * ( u_screenCorrection.xy * 2.0 ) - 1.0;
	float d   = -vz;
	vec3  P   = vec3( ndc.x * d * u_localParam0.x, ndc.y * u_windowCoord.z * d * u_localParam0.y, vz );
	vec3  Nraw = nt.xyz * 2.0 - 1.0;
	float nl2  = dot( Nraw, Nraw );
	if ( !( nl2 > 1e-4 ) ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }	// also catches NaN
	vec3  N   = Nraw * inversesqrt( nl2 );

	// lift to world (u_modelViewMatrix = inverse view), like ssr_rt.frag / rtao_ray.frag
	vec3 Pw = ( u_modelViewMatrix * vec4( P, 1.0 ) ).xyz;
	vec3 Nw = normalize( mat3( u_modelViewMatrix ) * N );

	RtLights lights = RtLights( uvec2( floatBitsToUint( u_color.x ), floatBitsToUint( u_color.y ) ) );
	uint nLights = uint( u_color.z );
	if ( nLights == 0u ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }

	uint M = uint( max( u_localParam0.z, 1.0 ) );
	if ( M > nLights ) { M = nLights; }			// no point drawing more candidates than lights

	// RNG seed: pixel + frame, forced odd so the first advance never degenerates
	uint rng = ( ( uint( frag.x ) * 1973u + uint( frag.y ) * 9277u )
	             ^ ( uint( u_localParam1.x ) * 26699u ) ) | 1u;

	// weighted reservoir sampling over M candidates. Source pdf = uniform = 1/nLights, so the RIS
	// weight of a candidate is pHat / pdf = pHat * nLights. pHat = luminance of the unshadowed
	// contribution (the resampling target function; diffuse-only for now).
	float wSum       = 0.0;
	float pHatChosen = 0.0;
	vec3  radChosen  = vec3( 0.0 );
	for ( uint i = 0u; i < M; i++ ) {
		uint li = uint( rnd( rng ) * float( nLights ) );
		if ( li >= nLights ) { li = nLights - 1u; }		// guard the rare rnd()==1.0

		vec3 ldir; float dl;
		dudeLightDir( lights, li, Pw, ldir, dl );
		float ndl = max( dot( Nw, ldir ), 0.0 );
		vec3  rad = vec3( 0.0 );
		if ( ndl > 0.0 ) {
			float atten = dudeLightAtten( lights, li, Pw, dl, lights.l[ li ].flags, true, false );
			rad = lights.l[ li ].color * ( atten * ndl );		// Doom lighting model: no 1/pi
		}
		float pHat = dot( rad, vec3( 0.2126, 0.7152, 0.0722 ) );	// luminance target
		float w    = pHat * float( nLights );
		wSum += w;
		if ( wSum > 0.0 && rnd( rng ) < ( w / wSum ) ) {
			pHatChosen = pHat;
			radChosen  = rad;
		}
	}

	// unbiased contribution weight W = (1/pHat(y)) * (wSum / M); the single-sample estimate of the
	// full sum is rad(y) * W (E[.] == sum over all lights of the unshadowed contribution).
	vec3 outRad = vec3( 0.0 );
	if ( pHatChosen > 0.0 ) {
		outRad = radChosen * ( ( wSum / float( M ) ) / pHatChosen );
	}
	fragColor = vec4( outRad, 1.0 );
}
