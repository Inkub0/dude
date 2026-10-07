// DUDE ReSTIR DI gather — fresh 1-spp (RTGI architecture, docs/rtx-restir-di.md). Per pixel: mask the view
// weapon, importance-sample ONE light from the in-range set by weighted reservoir sampling over M random
// candidates, trace ONE visibility ray, and shade the unbiased direct irradiance rad(y)·W·vis. NO history,
// NO persistent reservoir, NO reprojection - like rtgi_ray.frag this traces fresh every frame and hands ALL
// temporal accumulation to NRD (+ the DI confidence pass for moving-light antilag). That is what kills the
// moving-object trails a hand-rolled reproject leaves.
//
// The weapon is MASKED out (a < 0.5 -> zero), exactly as RTGI / RTAO / SSR do: it is a depth-hacked surface
// that must not receive world DI, and masking it here means it can never ghost. NRD owns the temporal, so
// the background the weapon uncovers is handled by NRD's disocclusion, not left as a hole.
//
// Uniform packing (RB_RHI_ReStirDIPass):
//   u_modelViewMatrix     = inverse view (view -> world)
//   u_color.xy            = scene-light buffer device address (bit-cast lo/hi)
//   u_color.z             = light count
//   u_rtParms.xy          = scene TLAS device address (bit-cast lo/hi; 0 = no RT scene -> unshadowed)
//   u_rtParms.z           = shadow-ray origin offset along the surface normal (world units)
//   u_rtParms.w           = max shadow-ray length
//   u_localParam0         = ( 1/proj00, 1/proj11, candidate count M, unused )
//   u_localParam1.x       = frame index (RNG decorrelation)
//   u_screenCorrection.xy = 1 / viewSize        (gl_FragCoord -> [0,1] uv; normal buffer)
//   u_depthTexRecip.xy    = gl_FragCoord -> _currentDepth tc
//   u_windowCoord.z       = view-Y sign (-1 Vulkan top-down)
#extension GL_EXT_buffer_reference_uvec2 : require	// build the light-buffer pointer from u_color.xy (uvec2)
#extension GL_EXT_ray_query : require				// one visibility ray for the chosen light

#include "renderparms.glsl"
#include "rt_light_eval.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(1) uniform sampler2D u_normalBuffer;	// xyz = view-space normal, a = weapon/AO mask (0 = weapon)

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;	// RGBA16F: linear direct irradiance (rgb), 1 (a)

// depth -> view z pair for THIS view (cinematics lower the near plane; renderparms.glsl u_depthParms)
#define depth_consts DUDE_DEPTH_CONSTS()

// PCG hash; seed the stream, then draw successive [0,1) uniforms (advance-then-output).
float rnd( inout uint rng ) {
	rng = rng * 747796405u + 2891336453u;
	uint w = ( ( rng >> ( ( rng >> 28u ) + 4u ) ) ^ rng ) * 277803737u;
	w = ( w >> 22u ) ^ w;
	return float( w ) * ( 1.0 / 4294967296.0 );
}

// one hard any-occluder shadow ray from the surface toward the chosen light (interaction.frag's fast opaque
// path). 1 = lit, 0 = occluded. No TLAS (no RT scene) -> unshadowed: never trace a null acceleration structure.
float traceVisibility( vec3 Pw, vec3 Nw, vec3 ldir, float dl ) {
	if ( u_rtParms.x == 0.0 && u_rtParms.y == 0.0 ) { return 1.0; }		// no RT scene
	float tMax = min( dl - 1.0, u_rtParms.w );
	if ( !( tMax > 0.0 ) ) { return 1.0; }
	rayQueryEXT rq;
	rayQueryInitializeEXT( rq,
		accelerationStructureEXT( uvec2( floatBitsToUint( u_rtParms.x ), floatBitsToUint( u_rtParms.y ) ) ),
		gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, 0xFDu /* skip perforated-caster instances */,
		Pw + Nw * u_rtParms.z, 0.0, ldir, tMax );
	while ( rayQueryProceedEXT( rq ) ) { }
	return ( rayQueryGetIntersectionTypeEXT( rq, true ) == gl_RayQueryCommittedIntersectionNoneEXT ) ? 1.0 : 0.0;
}

void main() {
	vec2  frag = gl_FragCoord.xy;
	float raw  = texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;
	if ( raw >= 0.9994 ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }	// sky / no geometry
	vec4 nt = texture( u_normalBuffer, frag * u_screenCorrection.xy );
	if ( nt.a < 0.5 ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }	// view weapon: no world DI (masked, like RTGI)

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
	if ( M > nLights ) { M = nLights; }

	// RNG seed: pixel + frame, forced odd so the first advance never degenerates
	uint rng = ( ( uint( frag.x ) * 1973u + uint( frag.y ) * 9277u )
	             ^ ( uint( u_localParam1.x ) * 26699u ) ) | 1u;

	// weighted reservoir sampling over M random candidates. Source pdf = uniform = 1/nLights, so a
	// candidate's RIS weight is pHat/pdf = pHat*nLights. pHat = luminance of the unshadowed contribution.
	// A cheap bounding-sphere cull skips lights that cannot reach the point. The reservoir is TRANSIENT
	// (this frame only) - NRD does the temporal accumulation downstream.
	float wSum        = 0.0;
	float pHatChosen  = 0.0;
	vec3  radChosen   = vec3( 0.0 );
	vec3  ldirChosen  = vec3( 0.0 );
	float dlChosen    = 0.0;
	uint  flagsChosen = 0u;
	for ( uint i = 0u; i < M; i++ ) {
		uint li = uint( rnd( rng ) * float( nLights ) );
		if ( li >= nLights ) { li = nLights - 1u; }		// guard the rare rnd()==1.0
		if ( !dudeLightInRange( lights, li, Pw ) ) { continue; }	// cheap volume-sphere cull

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
			pHatChosen  = pHat;
			radChosen   = rad;
			ldirChosen  = ldir;
			dlChosen    = dl;
			flagsChosen = lights.l[ li ].flags;
		}
	}

	if ( !( pHatChosen > 0.0 ) ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }	// no reaching light

	// unbiased contribution weight W = (1/pHat(y))·(wSum/M); one visibility ray for the chosen light. pHat
	// stays UNSHADOWED, so rad(y)·W·vis is still unbiased for the shadowed sum. NOSHADOWS lights skip the ray.
	float W   = ( wSum / float( M ) ) / pHatChosen;
	float vis = ( ( flagsChosen & DUDE_LIGHT_NOSHADOWS ) != 0u )
		? 1.0 : traceVisibility( Pw, Nw, ldirChosen, dlChosen );

	fragColor = vec4( radChosen * ( W * vis ), 1.0 );
}
