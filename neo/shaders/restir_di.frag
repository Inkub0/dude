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
// Output is the 3-attachment NRD guide MRT (CreateRenderTargetRtgiMrt), exactly like rtgi_ray.frag so the
// shared REBLUR_DIFFUSE denoise (RtaoDenoise giMode) drops in unchanged:
//   loc 0 = NRD-YCoCg direct irradiance (rgb) + normalized hit distance (a)   [IN_DIFF_RADIANCE_HITDIST]
//   loc 1 = signed linear view z (negative)                                   [IN_VIEWZ]
//   loc 2 = world normal, NRD encoding 2 (signed octahedron)                  [IN_NORMAL_ROUGHNESS]
// The guides are written for EVERY pixel (incl. sky / weapon), the radiance is zeroed there - mirroring
// rtgi_ray.frag so NRD sees a valid guide field everywhere.
//
// Uniform packing (RB_RHI_ReStirDIPass):
//   u_modelViewMatrix     = inverse view (view -> world)
//   u_color.xy            = scene-light buffer device address (bit-cast lo/hi)
//   u_color.z             = light count
//   u_rtParms.xy          = scene TLAS device address (bit-cast lo/hi; 0 = no RT scene -> unshadowed)
//   u_rtParms.z           = shadow-ray origin offset along the surface normal (world units)
//   u_rtParms.w           = max shadow-ray length
//   u_localParam0         = ( 1/proj00, 1/proj11, candidate count M, hit-distance scale (NRD A = 0.5*this) )
//   u_localParam1.x       = frame index (RNG decorrelation)
//   u_localParam1.z       = checkerboard parity (0 = off, 1 = even frame, 2 = odd; packs half the pixels left)
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

layout(location = 0) out vec4 fragColor;	// IN_DIFF_RADIANCE_HITDIST: NRD-YCoCg direct irradiance (rgb) + normHitDist (a)
layout(location = 1) out vec4 outViewZ;		// IN_VIEWZ: signed linear view z (negative), R16F
layout(location = 2) out vec4 outNormal;	// IN_NORMAL_ROUGHNESS: world normal, NRD encoding 2 (signed octahedron), RGBA16F

// depth -> view z pair for THIS view (cinematics lower the near plane; renderparms.glsl u_depthParms)
#define depth_consts DUDE_DEPTH_CONSTS()

// PCG hash; seed the stream, then draw successive [0,1) uniforms (advance-then-output).
float rnd( inout uint rng ) {
	rng = rng * 747796405u + 2891336453u;
	uint w = ( ( rng >> ( ( rng >> 28u ) + 4u ) ) ^ rng ) * 277803737u;
	w = ( w >> 22u ) ^ w;
	return float( w ) * ( 1.0 / 4294967296.0 );
}

// NRD REBLUR radiance encoding (REBLUR_FrontEnd_PackRadianceAndNormHitDist -> _NRD_LinearToYCoCg): the
// REBLUR_DIFFUSE filter works and OUTPUTS in YCoCg, so IN_DIFF_RADIANCE_HITDIST must be YCoCg too. The
// debug/apply decodes with the exact inverse (restir_di_debug.frag). Same as rtgi_ray.frag.
vec3 nrdLinearToYCoCg( vec3 c ) {
	return vec3( dot( c, vec3( 0.25, 0.5, 0.25 ) ), dot( c, vec3( 0.5, 0.0, -0.5 ) ), dot( c, vec3( -0.25, 0.5, -0.25 ) ) );
}

// world normal -> NRD normal/roughness encoding 2 (signed octahedron, z sign in roughness). Same as rtgi_ray.frag.
vec4 packNrdNormal( vec3 Nraw ) {
	float nl2 = dot( Nraw, Nraw );
	vec3  n;
	if ( !( nl2 > 1e-4 ) ) {
		n = vec3( 0.0, 0.0, 1.0 );
	} else {
		n = normalize( mat3( u_modelViewMatrix ) * ( Nraw * inversesqrt( nl2 ) ) );	// world-space
	}
	n /= abs( n.x ) + abs( n.y ) + abs( n.z );
	vec3 r;
	r.y = n.y * 0.5 + 0.5;
	r.x = n.x * 0.5 + r.y;
	r.y -= n.x * 0.5;
	float roughness = 1.0;										// can't be 0: z carries n.z's sign
	float s = n.z < 0.0 ? -roughness : roughness;
	r.z = s * 0.5 + 0.5;
	return vec4( r, 0.0 );										// w = materialID/3 = 0
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

	// Checkerboard (r_rtReStirDICheckerboard, u_localParam1.z): trace only HALF the pixels at FULL res, packed
	// into the LEFT half of the buffer; NRD reconstructs the rest from the full-res guide pass + IN_MV. sfrag is
	// the SCREEN pixel this buffer cell stands for: 0 = off (sfrag == frag), 1 = even frame, 2 = odd (the traced
	// parity alternates each frame so every pixel refreshes every other frame). Mirrors rtgi_ray.frag.
	vec2  sfrag  = frag;
	int   cbMode = int( u_localParam1.z + 0.5 );
	if ( cbMode > 0 ) {
		float gx = floor( frag.x ), gy = floor( frag.y );
		float parity = mod( gy + float( cbMode - 1 ), 2.0 );
		sfrag = vec2( gx * 2.0 + parity + 0.5, gy + 0.5 );
	}
	float raw  = texture( u_currentDepth, sfrag * u_depthTexRecip.xy ).x;

	// NRD guides first, for EVERY pixel (mirrors rtgi_ray.frag) so the shared REBLUR_DIFFUSE denoise sees a
	// valid guide field even on sky / weapon / degenerate pixels (the radiance is zeroed there below). In
	// checkerboard mode these gather-written guides are ignored (NRD reads the full-res guide pass instead).
	float vz = 1.0 / ( min( raw, 0.9994 ) * depth_consts.x + depth_consts.y );	// negative view z
	vec4  nt = texture( u_normalBuffer, sfrag * u_screenCorrection.xy );
	outViewZ  = vec4( vz, 0.0, 0.0, 1.0 );
	outNormal = packNrdNormal( nt.xyz * 2.0 - 1.0 );

	// checkerboard packs traced samples into the LEFT HALF; the right half traces nothing (NRD reconstructs it)
	if ( cbMode > 0 && frag.x * u_screenCorrection.x >= 0.5 ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }
	if ( raw >= 0.9994 ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }	// sky / no geometry
	if ( nt.a < 0.5 )    { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }	// view weapon: no world DI (masked, like RTGI)

	// view-space position + normal (ssao/rtao reconstruction incl. the VK view-Y sign)
	vec2  ndc = sfrag * ( u_screenCorrection.xy * 2.0 ) - 1.0;
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

	// RNG seed: SCREEN pixel (sfrag, so a checkerboard cell is seeded by the pixel it represents) + frame,
	// forced odd so the first advance never degenerates
	uint rng = ( ( uint( sfrag.x ) * 1973u + uint( sfrag.y ) * 9277u )
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

	// NRD IN_DIFF_RADIANCE_HITDIST: the "hit distance" for direct lighting is the distance to the chosen
	// light - it drives ReBLUR's world-space blur radius (near light -> tight penumbra, far light -> wider),
	// the direct-lighting analogue of the gather-ray distance RTGI feeds. Normalized saturate( dl / (A + d*B) )
	// with A = 0.5 * u_localParam0.w, B = 0.1, MUST match the ReblurHitDistanceParameters in RB_RHI_ReStirDIPass.
	float nhd = clamp( dlChosen / ( 0.5 * u_localParam0.w + d * 0.1 ), 0.0, 1.0 );
	fragColor = vec4( nrdLinearToYCoCg( max( radChosen * ( W * vis ), vec3( 0.0 ) ) ), nhd );
}
