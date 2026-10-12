// DUDE ReSTIR DI R1 - temporal GRADIENT pass (docs/rtx-restir-di.md Plan R1, the RTXDI "ComputeGradients" / Q2RTX
// A-SVGF idea): detect LIGHTING changes that leave the geometry intact, which NRD's depth/normal disocclusion
// cannot see, so the confidence pass can drop history only where the light really changed. One thread per 3x3
// stratum of the gather's internal resolution. For every current pixel of the stratum, backproject through the
// velocity to where it was last frame, read what the gather chose THERE (the light's row in last frame's light
// buffer + the fp16 luminance of its shaded visible contribution - restir_di.frag's side outputs, still in the
// gather MRT because this pass runs BEFORE this frame's gather overwrites it), accept it only if the surface is
// the same one (normal dot >= 0.5, relative prev-view-z difference <= 10%: the RTXDI test), and keep the BRIGHTEST
// accepted sample (Q2RTX: "picking the brightest pixel helps prevent bright trails when the light has moved").
// Then map that light to THIS frame's buffer (row -> lightDef -> row, RB_RHI_GatherSceneLights' table; absent =
// a light that switched off / left DI = a 100% change) and RE-SHADE IT at the current surface with the gather's own
// code (restir_di_shade.glsl) + one visibility ray against the current TLAS. The only difference between the two
// luminances is the lighting change. Output (RG16F): x = |L_prev - L_now|, y = max(L_prev, L_now), filtered by
// restir_di_gradient_filter.frag and turned into IN_DIFF_CONFIDENCE by restir_di_gradient_conf.frag.
//
// Shares the gather's uniform packing (restir_di.frag header) + units 0-3, plus:
//   unit 4 = last frame's gather att1 ( viewZ, gradient luminance )           (RG16F, internal res, packed when cb)
//   unit 5 = last frame's gather att2 ( NRD-packed world normal )
//   unit 6 = last frame's gather att3 ( SH1.xyz, chosen light row + 1 )
//   unit 7 = this frame's velocity at the internal res (currUV - prevUV, +Y-up)
//   u_shadowParms.xy  = lightDef -> current row table device address (bit-cast; row + 1, 0 = absent this frame)
//   u_shadowParms.zw  = LAST frame's light-buffer slot device address (bit-cast; the ring keeps it alive)
//   u_tessParms.x     = last frame's checkerboard parity (0 = off, 1 = even, 2 = odd): where a screen pixel's data sits
//   u_tessParms.yz    = gradient resolution ( gw, gh )
//   u_parallaxParms   = ( stratum size, internal rw, internal rh, 0 )
//   u_prevMvpMatrix   = LAST frame's world -> view matrix (the expected previous view z of a current surface point)
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_ray_query : require
#extension GL_EXT_nonuniform_qualifier : require

#include "renderparms.glsl"
#include "rt_light_eval.glsl"
#include "rt_light_tex.glsl"
#include "rt_perforated.glsl"
#include "restir_di_common.glsl"
#include "restir_di_shade.glsl"

SAMPLER_BINDING(4) uniform sampler2D u_prevViewZ;		// ( viewZ, gradient luminance )
SAMPLER_BINDING(5) uniform sampler2D u_prevNormal;	// NRD encoding 2
SAMPLER_BINDING(6) uniform sampler2D u_prevSide;		// ( SH1.xyz, row + 1 )
SAMPLER_BINDING(7) uniform sampler2D u_velocity;

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer DiLightMap { uint row[]; };

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;	// ( |L_prev - L_now|, max( L_prev, L_now ), 0, 1 )

// inverse of restir_di.frag's packNrdNormal (signed octahedron, z sign in .z)
vec3 unpackNrdNormal( vec3 r ) {
	float a = 2.0 * r.x - 1.0;					// n.x + n.y
	float b = 2.0 * r.y - 1.0;					// n.y - n.x
	vec3  n = vec3( 0.5 * ( a - b ), 0.5 * ( a + b ), 0.0 );
	n.z = ( 1.0 - abs( n.x ) - abs( n.y ) ) * ( r.z < 0.5 ? -1.0 : 1.0 );
	float l2 = dot( n, n );
	return ( l2 > 1e-8 ) ? n * inversesqrt( l2 ) : vec3( 0.0, 0.0, 1.0 );
}

void main() {
	fragColor = vec4( 0.0, 0.0, 0.0, 1.0 );
	const int   stratum = int( u_parallaxParms.x + 0.5 );
	const ivec2 res     = ivec2( int( u_parallaxParms.y + 0.5 ), int( u_parallaxParms.z + 0.5 ) );
	const int   prevCb  = int( u_tessParms.x + 0.5 );
	const bool  applyMode = u_localParam1.w > 0.5;
	const bool  pbr       = u_localParam1.y > 0.5;
	ivec2 base = ivec2( gl_FragCoord.xy ) * stratum;

	// 1. the brightest valid (pixel, previous sample) pair of the stratum
	float bestLum = -1.0;
	uint  bestRow = 0u;
	vec2  bestFrag = vec2( 0.0 );
	for ( int dy = 0; dy < stratum; dy++ ) {
		for ( int dx = 0; dx < stratum; dx++ ) {
			ivec2 cur = base + ivec2( dx, dy );
			if ( cur.x >= res.x || cur.y >= res.y ) { continue; }
			vec2 sfrag = vec2( cur ) + 0.5;
			DiSurface S; float raw; vec4 nt;
			if ( !diSurfaceAt( sfrag, false, false, S, raw, nt ) ) { continue; }	// geometry only here (no material fetch)
			// where was this pixel last frame?
			vec2 uvc = sfrag * u_screenCorrection.xy;
			vec2 vel = texture( u_velocity, uvc ).rg;
			vel.y = ( u_windowCoord.z < 0.0 ) ? -vel.y : vel.y;
			vec2 prevUV = uvc - vel;
			if ( any( lessThan( prevUV, vec2( 0.0 ) ) ) || any( greaterThanEqual( prevUV, vec2( 1.0 ) ) ) ) { continue; }
			ivec2 prevPix = ivec2( floor( prevUV * vec2( res ) ) );
			// checkerboard: last frame only TRACED the cells of its parity, packed into the left half; the others hold nothing
			ivec2 readPix = prevPix;
			if ( prevCb > 0 ) {
				if ( ( prevPix.x & 1 ) != ( ( prevPix.y + prevCb - 1 ) & 1 ) ) { continue; }	// not traced last frame
				readPix.x = prevPix.x >> 1;
			}
			vec4 side = texelFetch( u_prevSide, readPix, 0 );
			if ( side.w < 0.5 ) { continue; }									// no light chosen there
			vec2 pvz = texelFetch( u_prevViewZ, readPix, 0 ).xy;				// ( prev view z, prev luminance )
			vec3 pN  = unpackNrdNormal( texelFetch( u_prevNormal, readPix, 0 ).xyz );
			// same surface? (RTXDI RTXDI_IsValidNeighbor: normal dot >= 0.5, relative depth <= 10%, in the PREVIOUS view)
			if ( dot( S.Nw, pN ) < 0.5 ) { continue; }
			float expectedPrevVz = ( u_prevMvpMatrix * vec4( S.Pw, 1.0 ) ).z;
			if ( abs( expectedPrevVz - pvz.x ) > 0.1 * max( abs( expectedPrevVz ), 1.0 ) ) { continue; }
			if ( pvz.y > bestLum ) {
				bestLum  = pvz.y;
				bestRow  = uint( side.w + 0.5 ) - 1u;
				bestFrag = sfrag;
			}
		}
	}
	if ( bestLum < 0.0 ) { return; }											// nothing reprojectable: no evidence either way

	// 2. the same light in THIS frame's buffer: last frame's row -> lightDef -> this frame's row
	uvec2 tableAddr = uvec2( floatBitsToUint( u_shadowParms.x ), floatBitsToUint( u_shadowParms.y ) );
	uvec2 prevAddr  = uvec2( floatBitsToUint( u_shadowParms.z ), floatBitsToUint( u_shadowParms.w ) );
	if ( ( tableAddr.x | tableAddr.y ) == 0u || ( prevAddr.x | prevAddr.y ) == 0u ) { return; }
	RtLights prevLights = RtLights( prevAddr );
	uint lightDefPlus1 = prevLights.l[ bestRow ].pad0;
	float Lprev = bestLum;
	float Lnow  = 0.0;
	bool  present = false;
	if ( lightDefPlus1 != 0u ) {
		DiLightMap table = DiLightMap( tableAddr );
		uint curRowPlus1 = table.row[ lightDefPlus1 - 1u ];
		if ( curRowPlus1 != 0u ) {
			uint curRow = curRowPlus1 - 1u;
			RtLights lights = RtLights( uvec2( floatBitsToUint( u_color.x ), floatBitsToUint( u_color.y ) ) );
			// 3. re-shade that light at the current surface, the gather's way (material included), + its visibility
			DiSurface S; float raw; vec4 nt;
			if ( diSurfaceAt( bestFrag, applyMode, pbr, S, raw, nt ) ) {
				vec3 contrib, ldir; float dl, ndl;
				present = true;												// the light exists; it may simply not reach any more (L_now = 0)
				if ( diShadeLight( lights, curRow, S, contrib, ldir, dl, ndl ) ) {
					float occDist;
					float vis = ( ( lights.l[ curRow ].flags & DUDE_LIGHT_NOSHADOWS ) != 0u )
						? 1.0 : traceVisibility( S.Pw, S.Nw, ldir, dl, occDist );
					Lnow = diGradientLuminance( contrib, S.B, vis );
				}
			}
		}
	}
	// a light that vanished from DI (switched off, culled, moved to the forward path) is a 100% change (RTXDI)
	if ( !present ) { Lnow = 0.0; }
	fragColor = vec4( abs( Lprev - Lnow ), max( Lprev, Lnow ), 0.0, 1.0 );
}
