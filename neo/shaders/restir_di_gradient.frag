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
// TWO DIRECTIONS (RTXDI: "the brightest of the current or previous sample"): re-shading last frame's choice under this
// frame's lights catches a light LEAVING a surface (its contribution drops to 0) but never a light ARRIVING (last
// frame's choice there was some other, unchanged light) - the leading edge of a flashlight sweep, the lit footprint of a
// light switching on, which is exactly where NRD's history is stale and bright-lags. So the pass also finds the
// brightest light reaching the stratum's pixel NOW and shades it with its PREVIOUS-frame parameters (last frame's
// buffer row, found through last frame's table) at the stored previous surface; whichever of the two samples is
// brighter decides the stratum. Both directions use the current TLAS for visibility (no previous TLAS is kept).
//
// Shares the gather's uniform packing (restir_di.frag header) + units 0-3, plus:
//   unit 4 = last frame's gather att1 ( viewZ, gradient luminance )           (RG16F, internal res, packed when cb)
//   unit 5 = last frame's gather att2 ( NRD-packed world normal )
//   unit 6 = last frame's gather att3 ( SH1.xyz, chosen light row + 1 )
//   unit 7 = this frame's velocity at the internal res (currUV - prevUV, +Y-up)
//   u_shadowParms.xy  = lightDef -> current row table device address (bit-cast; row + 1, 0 = absent this frame)
//   u_shadowParms.zw  = LAST frame's light-buffer slot device address (bit-cast; the ring keeps it alive)
//   u_parallaxParms2.xy = LAST frame's lightDef -> row table device address (bit-cast): the CURRENT frame's brightest light
//                       looked up in last frame's buffer (the second gradient direction, below)
//   u_tessParms.x     = last frame's checkerboard parity (0 = off, 1 = even, 2 = odd): where a screen pixel's data sits
//   u_tessParms.yz    = gradient resolution ( gw, gh )
//   u_parallaxParms   = ( stratum size, internal rw, internal rh, 0 )
//   u_prevMvpMatrix   = LAST frame's world -> view matrix (the expected previous view z of a current surface point)
//   u_projectionMatrix= LAST frame's INVERSE view (view -> world): the stored previous pixel's world position + camera
//   u_parallaxParms.w = static threshold in FULL-RES pixels: a surface point that moved less than this many pixels'
//                       worth of world units between the frames is STATIC and is re-shaded at its stored previous
//                       position / normal / camera, so a static world under a static light gives L_now == L_prev
//                       exactly (the FSR2 jitter + reprojection otherwise land the two shades on sub-pixel-different
//                       points - different bump texel, cookie texel, shadow edge - and the power-8 confidence turns
//                       that few-percent mismatch into grey). A mover keeps the current surface (its old position
//                       would be shadowed by its own old self in the current TLAS).
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
	vec3  bestPrevP = vec3( 0.0 ), bestPrevN = vec3( 0.0, 0.0, 1.0 );
	float bestPrevVz = -1.0;
	bool  bestStatic = false;
	const float staticPx = u_parallaxParms.w;
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
				// the stored sample's world position: last frame's SCREEN pixel centre at its stored view z through last
				// frame's inverse view (the same reconstruction the gather used, so a static point lands exactly)
				vec2  pndc = ( vec2( prevPix ) + 0.5 ) * ( u_screenCorrection.xy * 2.0 ) - 1.0;
				float pd   = -pvz.x;
				vec3  Pv   = vec3( pndc.x * pd * u_localParam0.x, pndc.y * u_windowCoord.z * pd * u_localParam0.y, pvz.x );
				vec3  Pprev = ( u_projectionMatrix * vec4( Pv, 1.0 ) ).xyz;
				bestLum  = pvz.y;
				bestRow  = uint( side.w + 0.5 ) - 1u;
				bestFrag = sfrag;
				bestPrevP = Pprev;
				bestPrevN = pN;
				bestPrevVz = pvz.x;
				bestStatic = length( S.Pw - Pprev ) <= staticPx * S.d * u_shadowFalloffS.y;	// moved less than N pixels' worth
			}
		}
	}
	if ( bestLum < 0.0 ) { return; }											// nothing reprojectable: no evidence either way

	uvec2 tableAddr = uvec2( floatBitsToUint( u_shadowParms.x ), floatBitsToUint( u_shadowParms.y ) );
	uvec2 prevAddr  = uvec2( floatBitsToUint( u_shadowParms.z ), floatBitsToUint( u_shadowParms.w ) );
	uvec2 prevTableAddr = uvec2( floatBitsToUint( u_parallaxParms2.x ), floatBitsToUint( u_parallaxParms2.y ) );
	if ( ( tableAddr.x | tableAddr.y ) == 0u || ( prevAddr.x | prevAddr.y ) == 0u ) { return; }
	RtLights lights     = RtLights( uvec2( floatBitsToUint( u_color.x ), floatBitsToUint( u_color.y ) ) );
	RtLights prevLights = RtLights( prevAddr );
	DiLightMap table    = DiLightMap( tableAddr );
	uint nLights = uint( u_color.z );

	// the stratum's pixel, now (S) and as it was last frame (Sp: the exact stored point when static, else the same)
	DiSurface S; float raw; vec4 nt;
	if ( !diSurfaceAt( bestFrag, applyMode, pbr, S, raw, nt ) ) { return; }
	DiSurface Sp = S;
	if ( bestStatic ) {
		// static geometry: the EXACT stored point with its stored normal from last frame's camera position (the material
		// comes from the current pixel = the same surface). Only the LIGHTS can differ between the two shades.
		Sp.Pw = bestPrevP;
		Sp.Nw = bestPrevN;
		Sp.d  = -bestPrevVz;
		if ( applyMode ) {
			Sp.Vw    = normalize( u_projectionMatrix[3].xyz - Sp.Pw );
			Sp.NdotV = max( dot( Sp.Nw, Sp.Vw ), 1e-4 );
		}
	}

	// 2. the second direction's candidate: the brightest light reaching this pixel NOW (unshadowed)
	uint  curRow = 0u;
	float curLum = -1.0;
	vec3  curContrib = vec3( 0.0 ), curLdir = vec3( 0.0, 0.0, 1.0 );
	float curDl = 0.0;
	if ( ( prevTableAddr.x | prevTableAddr.y ) != 0u ) {
		for ( uint li = 0u; li < nLights; li++ ) {
			vec3 c, ld; float dl, ndl;
			if ( !diShadeLight( lights, li, S, c, ld, dl, ndl ) ) { continue; }
			float l = diGradientLuminance( c, S.B, 1.0 );
			if ( l > curLum ) { curLum = l; curRow = li; curContrib = c; curLdir = ld; curDl = dl; }
		}
	}

	float Lprev, Lnow;
	float occDist;
	if ( curLum > bestLum ) {
		// 3a. CURRENT sample, shaded in the previous frame: this light's previous-frame row (position / colour / cookie
		// planes as they were) at the previous surface. Absent last frame (switched on) -> 0 -> a 100% change.
		Lnow = diGradientLuminance( curContrib, S.B,
			( ( lights.l[ curRow ].flags & DUDE_LIGHT_NOSHADOWS ) != 0u ) ? 1.0 : traceVisibility( S.Pw, S.Nw, curLdir, curDl, occDist ) );
		Lprev = 0.0;
		uint defPlus1 = lights.l[ curRow ].pad0;
		if ( defPlus1 != 0u ) {
			DiLightMap prevTable = DiLightMap( prevTableAddr );
			uint prevRowPlus1 = prevTable.row[ defPlus1 - 1u ];
			// same slot AND same creation serial (pad1): a freed lightDef slot is reused at once (a weapon switch lands the
			// flashlight in the old weapon light's slot), and a different light there is "absent", not "unchanged"
			if ( prevRowPlus1 != 0u && prevLights.l[ prevRowPlus1 - 1u ].pad1 == lights.l[ curRow ].pad1 ) {
				vec3 c, ld; float dl, ndl;
				if ( diShadeLight( prevLights, prevRowPlus1 - 1u, Sp, c, ld, dl, ndl ) ) {
					float vis = ( ( prevLights.l[ prevRowPlus1 - 1u ].flags & DUDE_LIGHT_NOSHADOWS ) != 0u )
						? 1.0 : traceVisibility( Sp.Pw, Sp.Nw, ld, dl, occDist );
					Lprev = diGradientLuminance( c, Sp.B, vis );
				}
			}
		}
	} else {
		// 3b. PREVIOUS sample, shaded now: last frame's row -> lightDef -> this frame's row, re-shaded at the (exact when
		// static) previous surface under THIS frame's lights. Gone from DI (switched off, culled, moved to the forward
		// path) -> 0 -> a 100% change.
		Lprev = bestLum;
		Lnow  = 0.0;
		uint lightDefPlus1 = prevLights.l[ bestRow ].pad0;
		if ( lightDefPlus1 != 0u ) {
			uint curRowPlus1 = table.row[ lightDefPlus1 - 1u ];
			if ( curRowPlus1 != 0u && lights.l[ curRowPlus1 - 1u ].pad1 == prevLights.l[ bestRow ].pad1 ) {	// same light, not a slot reuse
				vec3 c, ld; float dl, ndl;
				if ( diShadeLight( lights, curRowPlus1 - 1u, Sp, c, ld, dl, ndl ) ) {
					float vis = ( ( lights.l[ curRowPlus1 - 1u ].flags & DUDE_LIGHT_NOSHADOWS ) != 0u )
						? 1.0 : traceVisibility( Sp.Pw, Sp.Nw, ld, dl, occDist );
					Lnow = diGradientLuminance( c, Sp.B, vis );
				}
			}
		}
	}
	fragColor = vec4( abs( Lprev - Lnow ), max( Lprev, Lnow ), 0.0, 1.0 );
}
