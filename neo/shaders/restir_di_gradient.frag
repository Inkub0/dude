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
// Runs AFTER this frame's gather (R1 rev. 2): both frames' picks are then at hand - this frame's in the gather MRT, last
// frame's in the side copies the DI pass made before the gather - so the stratum takes the brightest sample of EITHER
// frame (RTXDI), and in the common case ("the pixel shaded the same light in both frames") both luminances are the
// gather's own stored, truly-visible values - but they were shaded at two sub-pixel-different (jittered) points, so
// only ONE of them is the truth and the other frame is re-shaded at that point with one ray (see the comparison below).
//
// Shares the gather's uniform packing (restir_di.frag header) + units 0-3, plus:
//   unit 4 = THIS frame's gather att1 ( viewZ, gradient luminance )           (RG16F, internal res, packed when cb)
//   unit 5 = LAST frame's gather att1 (copy)
//   unit 6 = LAST frame's gather att2 ( NRD-packed world normal ) (copy)
//   unit 10 = this frame's velocity at the internal res (currUV - prevUV, +Y-up)
//   unit 11 = LAST frame's gather att3 ( SH1.xyz, chosen light row + 1 [+ 0.5 = R3a boosted] ) (copy)
//   unit 9 = THIS frame's gather att3
//   u_tessParms.w     = THIS frame's checkerboard parity (0 = off, 1 = even, 2 = odd)
//   u_shadowParms.xy  = lightDef -> current row table device address (bit-cast; row + 1, 0 = absent this frame)
//   u_shadowParms.zw  = LAST frame's light-buffer slot device address (bit-cast; the ring keeps it alive)
//   u_parallaxParms2.xy = LAST frame's lightDef -> row table device address (bit-cast): the CURRENT frame's brightest light
//                       looked up in last frame's buffer (the second gradient direction, below)
//   u_tessParms.x     = last frame's checkerboard parity (0 = off, 1 = even, 2 = odd): where a screen pixel's data sits
//   u_tessParms.z     = the confidence pass's darkness bias (r_rtReStirDIGradientBias): weighs the two shadow-edge judgements
//   u_parallaxParms   = ( stratum size, internal rw, internal rh, static threshold px )
//   u_parallaxParms2.w = LAST frame's light count (bounds for the stored rows: a stale slot after a map change must not be
//                       dereferenced - an out-of-bounds buffer_reference read is a GPU page fault = VK device lost)
//   u_tessParms.y     = lightDef -> row table capacity (bounds for the stored lightDef indices)
//   u_prevMvpMatrix   = LAST frame's world -> view matrix (the expected previous view z of a current surface point)
//   u_projectionMatrix= LAST frame's INVERSE view (view -> world): the stored previous pixel's world position + camera
//   u_parallaxParms.w = static threshold in FULL-RES pixels: a surface point that moved less than this many pixels'
//                       worth of world units between the frames is STATIC and is re-shaded at its stored previous
//                       position / normal / camera, so a static world under a static light gives L_now == L_prev
//                       exactly (the FSR2 jitter + reprojection otherwise land the two shades on sub-pixel-different
//                       points - different bump texel, cookie texel, shadow edge - and the power-8 confidence turns
//                       that few-percent mismatch into grey). A mover keeps the current surface (its old position
//                       would be shadowed by its own old self in the current TLAS).
//   RT irradiance cache (docs/rtx-irradiance-cache.md C3, inherited from the gather's packing: u_particleRadius.zw slot
//   address, u_channelMask / u_skinParms / u_contactParms): when flag 4 is set and the stratum's direct light changed by
//   more than the reset threshold, the shaded point's cache cell is asked to restart its history (its cached INDIRECT
//   term is as stale as the direct term that changed).
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_ray_query : require
#extension GL_EXT_nonuniform_qualifier : require

#include "renderparms.glsl"
#include "rt_light_eval.glsl"
#include "rt_light_tex.glsl"
#include "rt_perforated.glsl"
#include "restir_di_common.glsl"
#include "restir_di_shade.glsl"
// the RT irradiance cache's cell reset (C3)
#include "rt_hash_grid.glsl"
#include "rtgi_cache.glsl"

SAMPLER_BINDING(4) uniform sampler2D u_curViewZ;		// this frame: ( viewZ, gradient luminance )
SAMPLER_BINDING(5) uniform sampler2D u_prevViewZ;		// last frame: ( viewZ, gradient luminance )
SAMPLER_BINDING(6) uniform sampler2D u_prevNormal;	// last frame: NRD encoding 2
// units 10 / 11, NOT 7 / 8: the interaction draws take their shadow map / shadow cube (depth-compare samplers) from
// whatever units 7 / 8 hold, and an unshadowed light never rebinds them - a colour image left there made every
// DI-covered light's weapon draw an invalid descriptor (the "weapon goes dark under DI" bug, 2026-10-12)
SAMPLER_BINDING(10) uniform sampler2D u_velocity;
SAMPLER_BINDING(11) uniform sampler2D u_prevSide;		// last frame: ( SH1.xyz, row + 1 )
SAMPLER_BINDING(9) uniform sampler2D u_curSide;		// this frame: ( SH1.xyz, row + 1 )

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer DiLightMap { uint row[]; };

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;	// ( |L_prev - L_now|, max( L_prev, L_now ), 0, 1 )

void main() {
	fragColor = vec4( 0.0, 0.0, 0.0, 1.0 );
	const int   stratum = int( u_parallaxParms.x + 0.5 );
	const ivec2 res     = ivec2( int( u_parallaxParms.y + 0.5 ), int( u_parallaxParms.z + 0.5 ) );
	const int   prevCb  = int( u_tessParms.x + 0.5 );
	const int   curCb   = int( u_tessParms.w + 0.5 );
	const vec2  fullRes = vec2( 2.0 * u_localParam0.x / u_shadowFalloffS.y ) * vec2( 1.0, float( res.y ) / float( res.x ) );	// full-res viewport (px)
	const bool  applyMode = u_localParam1.w > 0.5;
	const bool  pbr       = u_localParam1.y > 0.5;
	const float staticPx  = u_parallaxParms.w;
	ivec2 base = ivec2( gl_FragCoord.xy ) * stratum;
	const bool  shadowSel = u_parallaxParms2.z > 0.5;		// r_rtReStirDIGradientShadow

	uvec2 tableAddr = uvec2( floatBitsToUint( u_shadowParms.x ), floatBitsToUint( u_shadowParms.y ) );
	uvec2 prevAddr  = uvec2( floatBitsToUint( u_shadowParms.z ), floatBitsToUint( u_shadowParms.w ) );
	uvec2 prevTableAddr = uvec2( floatBitsToUint( u_parallaxParms2.x ), floatBitsToUint( u_parallaxParms2.y ) );
	if ( ( tableAddr.x | tableAddr.y ) == 0u || ( prevAddr.x | prevAddr.y ) == 0u ) { return; }
	RtLights lights     = RtLights( uvec2( floatBitsToUint( u_color.x ), floatBitsToUint( u_color.y ) ) );
	RtLights prevLights = RtLights( prevAddr );
	const uint nLights    = uint( u_color.z );
	const uint nPrevLights = uint( max( u_parallaxParms2.w, 0.0 ) + 0.5 );
	const uint tableCap   = uint( max( u_tessParms.y, 0.0 ) + 0.5 );

	// SHADOW EDGES (r_rtReStirDIGradientShadow): the brightest pick below sees a shadow ARRIVING (last frame's lit value,
	// re-shaded under the current scene, comes out 0) but not one LEAVING: there the brightest sample is THIS frame's lit
	// value (or, under the checkerboard, some other light's), and its previous value can only be estimated with the
	// CURRENT scene - the occluder has already gone, both sides read lit, and the dark history behind a walking NPC was
	// never dropped (its shadow smeared). So each stratum ALSO judges its DARKEST previous sample that had a light, on a
	// static surface: on a trailing edge that is the pixel whose light was blocked last frame, and its stored value
	// carries that real previous visibility - re-shaded at its stored point under the current scene it reads lit now,
	// an exact change. In a region that stays shadowed or stays lit it re-shades to its stored value: no change, no
	// false reset. Needs no current sample of the same pixel (none exists under the checkerboard: a still pixel is traced
	// every OTHER frame). One extra shade + ray per stratum; the bigger relative change of the two judgements wins.
	float dkPrevLum = 1e30;
	uint  dkPrevRow = 0u;
	vec2  dkFrag = vec2( 0.0 );
	vec3  dkPrevP = vec3( 0.0 ), dkPrevN = vec3( 0.0, 0.0, 1.0 );
	float dkPrevVz = -1.0;

	// 1. the brightest sample of EITHER frame among the stratum's pixels that have a valid temporal position
	float bestScore = -1.0;
	bool  bestIsCur = false;
	float bestPrevLum = 0.0, bestCurLum = 0.0;
	uint  bestPrevRow = 0u, bestCurRow = 0u;		// row + 1 (0 = no light chosen)
	vec2  bestFrag = vec2( 0.0 );
	vec3  bestPrevP = vec3( 0.0 ), bestPrevN = vec3( 0.0, 0.0, 1.0 );
	float bestPrevVz = -1.0;
	bool  bestStatic = false;
	for ( int dy = 0; dy < stratum; dy++ ) {
		for ( int dx = 0; dx < stratum; dx++ ) {
			ivec2 cur = base + ivec2( dx, dy );
			if ( cur.x >= res.x || cur.y >= res.y ) { continue; }
			vec2 sfrag = vec2( cur ) + 0.5;
			DiSurface S; float raw; vec4 nt;
			if ( !diSurfaceAt( sfrag, false, false, S, raw, nt ) ) { continue; }	// geometry only here (no material fetch)

			// this frame's pick at this pixel (under checkerboard only the traced cells, packed left)
			float curLum = -1.0;
			uint  curRowPlus1 = 0u;
			{
				bool  traced = true;
				ivec2 rp = cur;
				if ( curCb > 0 ) {
					traced = ( ( cur.x & 1 ) == ( ( cur.y + curCb - 1 ) & 1 ) );
					rp.x = cur.x >> 1;
				}
				if ( traced ) {
					vec4 side = texelFetch( u_curSide, rp, 0 );
					curLum = texelFetch( u_curViewZ, rp, 0 ).y;				// 0 when no light was chosen (side.w = 0)
					curRowPlus1 = uint( floor( side.w + 0.1 ) );			// R3a keeps its boost reason in the fraction (0.25 / 0.5 / 0.75): floor it off
				}
			}

			// last frame's pick where this pixel was (velocity reprojection + the RTXDI same-surface test)
			vec2 uvc = sfrag * u_screenCorrection.xy;
			vec2 vel = texture( u_velocity, uvc ).rg;
			vel.y = ( u_windowCoord.z < 0.0 ) ? -vel.y : vel.y;
			vec2 prevUV = uvc - vel;
			if ( any( lessThan( prevUV, vec2( 0.0 ) ) ) || any( greaterThanEqual( prevUV, vec2( 1.0 ) ) ) ) { continue; }
			ivec2 prevPix = ivec2( floor( prevUV * vec2( res ) ) );
			ivec2 readPix = prevPix;
			if ( prevCb > 0 ) {
				if ( ( prevPix.x & 1 ) != ( ( prevPix.y + prevCb - 1 ) & 1 ) ) { continue; }	// not traced last frame
				readPix.x = prevPix.x >> 1;
			}
			vec2 pvz = texelFetch( u_prevViewZ, readPix, 0 ).xy;				// ( prev view z, prev luminance )
			vec3 pN  = unpackNrdNormal( texelFetch( u_prevNormal, readPix, 0 ).xyz );
			if ( dot( S.Nw, pN ) < 0.5 ) { continue; }
			float expectedPrevVz = ( u_prevMvpMatrix * vec4( S.Pw, 1.0 ) ).z;
			if ( abs( expectedPrevVz - pvz.x ) > 0.1 * max( abs( expectedPrevVz ), 1.0 ) ) { continue; }
			vec4  pside = texelFetch( u_prevSide, readPix, 0 );
			uint  prevRowPlus1 = uint( floor( pside.w + 0.1 ) );		// R3a boost reason in the fraction: floor it off (a +0.25 guard misread 0.75 as the NEXT row -
																		// a false light change, confidence 0, NRD history pinned, the pixel boosted forever: red trails)
			float prevLum = ( prevRowPlus1 != 0u ) ? pvz.y : 0.0;			// a valid surface with no light last frame counts as dark

			// the stored sample's world position: last frame's SCREEN pixel centre at its stored view z through last
			// frame's inverse view (the same reconstruction the gather used, so a static point lands exactly)
			vec2  pndc = ( vec2( prevPix ) + 0.5 ) * ( u_screenCorrection.xy * 2.0 ) - 1.0;
			float pd   = -pvz.x;
			vec3  Pv   = vec3( pndc.x * pd * u_localParam0.x, pndc.y * u_windowCoord.z * pd * u_localParam0.y, pvz.x );
			vec3  Pprev = ( u_projectionMatrix * vec4( Pv, 1.0 ) ).xyz;
			// STATIC. Screen form (below): the surface moved less than N full-res pixels on screen - true only while the
			// camera holds still, so every turn sent all pixels down the approximate mover path. World form (shadowSel):
			// the motion vector matches where the CAMERA alone would have put this point last frame (last frame's view,
			// same projection), i.e. only the camera moved - the exact previous-point re-shade holds while turning too.
			bool isStatic = length( vel * fullRes ) <= staticPx;
			if ( shadowSel ) {
				vec3  Pc  = ( u_prevMvpMatrix * vec4( S.Pw, 1.0 ) ).xyz;
				float pdc = -Pc.z;
				isStatic = false;
				if ( pdc > 1e-3 ) {
					vec2 camUV = vec2( Pc.x / ( pdc * u_localParam0.x ), Pc.y / ( pdc * u_windowCoord.z * u_localParam0.y ) ) * 0.5 + 0.5;
					isStatic = length( ( camUV - prevUV ) * fullRes ) <= staticPx;
				}
			}
			// the darkest-previous candidate: static, a light was chosen there last frame (a valid row, not an emitter)
			if ( shadowSel && isStatic && prevRowPlus1 != 0u && prevRowPlus1 <= nPrevLights
					&& ( prevLights.l[ prevRowPlus1 - 1u ].flags & DUDE_LIGHT_EMISSIVE ) == 0u && prevLum < dkPrevLum ) {
				dkPrevLum = prevLum;
				dkPrevRow = prevRowPlus1;
				dkFrag    = sfrag;
				dkPrevP   = Pprev;	dkPrevN = pN;	dkPrevVz = pvz.x;
			}

			float score = max( prevLum, max( curLum, 0.0 ) );
			if ( score > bestScore ) {
				bestScore   = score;
				bestIsCur   = ( curLum > prevLum );
				bestPrevLum = prevLum;
				bestCurLum  = max( curLum, 0.0 );
				bestPrevRow = prevRowPlus1;
				bestCurRow  = ( curLum >= 0.0 ) ? curRowPlus1 : 0u;
				bestFrag    = sfrag;
				bestPrevP   = Pprev;
				bestPrevN   = pN;
				bestPrevVz  = pvz.x;
				// STATIC (isStatic above): screen motion, or with shadowSel the camera-only reprojection. NOT a world-distance
				// test against the stored point: the jitter-corrected reprojection often lands on the NEIGHBOURING pixel, and on
				// a floor or wall at a grazing angle one pixel apart is many "pixel sizes" of surface, which flagged most static
				// floors as movers whenever FSR2 was on (and the mover path compares two different points - view 5 flicker,
				// 2026-10-12). Both forms compare UVs of the same pixel, never two points.
				bestStatic  = isStatic;
			}
		}
	}
	if ( bestScore < 0.0 ) { return; }											// nothing reprojectable: no evidence either way
	if ( bestPrevRow != 0u && bestPrevRow > nPrevLights ) { return; }		// stale / corrupt side data: no evidence
	if ( bestCurRow  != 0u && bestCurRow  > nLights )     { return; }
	// E2 v3: an EMITTER row (DUDE_LIGHT_EMISSIVE) is never re-shaded here - its shade draws a fresh sample point every
	// frame (rt_emissive.glsl), which would read as a lighting change and reset the history under every emitter. An
	// emitter's state changes (a fixture switched off) reach the table, not the gradient: no evidence either way.
	if ( ( bestCurRow != 0u && ( lights.l[ bestCurRow - 1u ].flags & DUDE_LIGHT_EMISSIVE ) != 0u )
			|| ( bestPrevRow != 0u && ( prevLights.l[ bestPrevRow - 1u ].flags & DUDE_LIGHT_EMISSIVE ) != 0u ) ) { return; }

	// the same light in both frames? (slot AND creation serial: a freed slot is reused at once)
	bool sameLight = false;
	if ( bestPrevRow != 0u && bestCurRow != 0u ) {
		sameLight = lights.l[ bestCurRow - 1u ].pad0 == prevLights.l[ bestPrevRow - 1u ].pad0
		         && lights.l[ bestCurRow - 1u ].pad1 == prevLights.l[ bestPrevRow - 1u ].pad1;
	}

	// Both luminances MUST come from the SAME world point and the same ray. The two frames' stored samples sit on
	// sub-pixel-different points (the FSR2 jitter), and at a hard shadow edge their one-ray visibilities flip: a 100 %
	// change every frame -> the confidence floor -> a history reset along every shadow edge (rev. 2's stored-vs-stored
	// shortcut did exactly that; NRD's validation showed it as flicker along every edge). So exactly ONE stored value is
	// the truth and the other frame is re-shaded AT THAT VALUE'S POINT with one ray:
	//   - the winning light is LAST frame's pick (or the same light on a static surface): Lprev = stored; Lnow = the
	//     light's CURRENT row shaded now at the stored previous point (exact reconstruction, a moved occluder shows);
	//     a mover is valued at the current point instead (its old point is stale; the current TLAS approximates);
	//   - the winning light is THIS frame's pick (or the same light on a mover): Lnow = stored; Lprev = the light's
	//     PREVIOUS row shaded at the CURRENT point (the current TLAS approximates its previous visibility).
	// A light absent from the other frame's buffer (switched on / off, entered / left DI) is 0 there: a 100 % change.
	const bool wIsPrev     = sameLight ? bestStatic : !bestIsCur;		// which stored value is the truth
	const bool atPrevPoint = wIsPrev && bestStatic;
	DiSurface S; float raw; vec4 nt;
	if ( !diSurfaceAt( bestFrag, applyMode, pbr, S, raw, nt ) ) { return; }
	DiSurface Sp = S;
	if ( atPrevPoint ) {
		Sp.Pw = bestPrevP;
		Sp.Nw = bestPrevN;
		Sp.d  = -bestPrevVz;
		if ( applyMode ) {
			Sp.Vw    = normalize( u_projectionMatrix[3].xyz - Sp.Pw );
			Sp.NdotV = max( dot( Sp.Nw, Sp.Vw ), 1e-4 );
		}
	}
	float Lprev = 0.0, Lnow = 0.0, occDist;
	// RT irradiance cache (C3), built here because both branches below record whether the winning light is ABSENT from
	// the other frame's buffer (a switch, or it entered / left DI). That only counts when the light actually LIT the
	// surface - its stored luminance above the darkness bias: a stratum with no pick on either side is dark, not changed
	// (counting those reset every dark corner every frame and held the whole room yellow, 2026-10-16).
	RtgiCacheParms cp = rtgiCacheParmsFromUniforms( uvec2( floatBitsToUint( u_particleRadius.z ), floatBitsToUint( u_particleRadius.w ) ) );
	bool  cacheRowAbsent = false;
	if ( wIsPrev ) {
		// last frame's light (its row this frame: the same pick, else through this frame's table), valued now
		Lprev = bestPrevLum;
		uint curRowPlus1 = 0u;
		if ( sameLight ) {
			curRowPlus1 = bestCurRow;
		} else {
			uint defPlus1 = ( bestPrevRow != 0u ) ? prevLights.l[ bestPrevRow - 1u ].pad0 : 0u;
			if ( defPlus1 != 0u && defPlus1 <= tableCap ) {
				DiLightMap table = DiLightMap( tableAddr );
				uint rp1 = table.row[ defPlus1 - 1u ];
				if ( rp1 != 0u && rp1 <= nLights && lights.l[ rp1 - 1u ].pad1 == prevLights.l[ bestPrevRow - 1u ].pad1 ) { curRowPlus1 = rp1; }
			}
		}
		cacheRowAbsent = ( bestPrevRow != 0u && curRowPlus1 == 0u && bestPrevLum > cp.resetBias );	// last frame's light is gone now
		if ( curRowPlus1 != 0u ) {
			vec3 c, ld; float dl, ndl;
			if ( diShadeLight( lights, curRowPlus1 - 1u, Sp, c, ld, dl, ndl, false ) ) {
				float vis = ( ( lights.l[ curRowPlus1 - 1u ].flags & DUDE_LIGHT_NOSHADOWS ) != 0u )
					? 1.0 : traceVisibility( Sp.Pw, Sp.Nw, ld, dl, diLightBlurScale( lights.l[ curRowPlus1 - 1u ].flags ), occDist );
				Lnow = diGradientLuminance( c, Sp.B, vis );
			}
		}
	} else {
		// this frame's light (its row last frame: the same pick, else through last frame's table), valued in the previous frame
		Lnow = bestCurLum;
		uint prevRowPlus1 = 0u;
		if ( sameLight ) {
			prevRowPlus1 = bestPrevRow;
		} else {
			uint defPlus1 = ( bestCurRow != 0u ) ? lights.l[ bestCurRow - 1u ].pad0 : 0u;
			if ( defPlus1 != 0u && defPlus1 <= tableCap && ( prevTableAddr.x | prevTableAddr.y ) != 0u ) {
				DiLightMap prevTable = DiLightMap( prevTableAddr );
				uint rp1 = prevTable.row[ defPlus1 - 1u ];
				if ( rp1 != 0u && rp1 <= nPrevLights && prevLights.l[ rp1 - 1u ].pad1 == lights.l[ bestCurRow - 1u ].pad1 ) { prevRowPlus1 = rp1; }
			}
		}
		cacheRowAbsent = ( bestCurRow != 0u && prevRowPlus1 == 0u && bestCurLum > cp.resetBias );	// this frame's light was not there last frame
		if ( prevRowPlus1 != 0u ) {
			vec3 c, ld; float dl, ndl;
			if ( diShadeLight( prevLights, prevRowPlus1 - 1u, Sp, c, ld, dl, ndl, false ) ) {
				float vis = ( ( prevLights.l[ prevRowPlus1 - 1u ].flags & DUDE_LIGHT_NOSHADOWS ) != 0u )
					? 1.0 : traceVisibility( Sp.Pw, Sp.Nw, ld, dl, diLightBlurScale( prevLights.l[ prevRowPlus1 - 1u ].flags ), occDist );		// current TLAS: an approximation for movers
				Lprev = diGradientLuminance( c, Sp.B, vis );
			}
		}
	}
	// RT irradiance cache (docs/rtx-irradiance-cache.md C3): the direct light at this stratum's surface CHANGED - a light
	// switched, a swinging light's footprint moved on - so the INDIRECT term its cell has accumulated is stale too. Ask
	// the resolve to restart the cell (N = 0: the next frame's samples replace the old mean instead of being 1/N of it).
	// The threshold sits above the DI dead zone, so Doom's authored 5-15 % flicker tables never reset a cell every frame;
	// a light switching off / on is a 100 % change of its own value regardless. The darkness bias in the denominator
	// (as the confidence pass's, but larger) keeps a near-black direct term from resetting on its own noise: a change
	// of a light that contributes almost nothing cannot have moved the indirect term either. One request per changed
	// stratum (3x3 pixels) is plenty: a cell is ~10 pixels wide at the default cell angle.
	// EXACT comparisons only: when LAST frame's light wins on a pixel that moved more than the static threshold on
	// screen (any turn or walk), the two luminances come from two different points a pixel apart (the stored previous
	// point vs the current one - the mover approximation above), and on bump-mapped, cookie-lit surfaces that differs
	// by 35 % routinely: the whole view flashed red in the activity view while turning (user, 2026-10-16; confirmed by
	// the r_rtgiCacheReset 0 A/B). The DI confidence absorbs that through its dead zone + filter; a cell reset must not.
	// The other direction (THIS frame's pick wins) shades both frames at the same point and is exact even while
	// moving, so a light switching ON still resets on the move; a switch-OFF while moving is left to the history blend.
	// LIGHT changes only, never VISIBILITY changes: an NPC's shadow sweeping a wall flips the one-ray visibility of a
	// whole region every frame (a static camera watching a walking marine showed resets over all the walls), but the
	// indirect term a moving shadow displaces is second-order - resetting the cell for it only injects noise. The
	// gradient can tell the two apart: a light that switched off LEFT the buffer (its row is absent in the other
	// frame, 100 % change, reset), while an occluded light is still present with a zero visibility. So: reset when the
	// light is absent on either side, or when it is LIT on both sides and its lit value changed by the threshold (a
	// dimming, a brightening, a swinging cookie); a side that is zero with the light present is a shadow - no reset.
	{
		const bool exact   = !( wIsPrev && !bestStatic );
		const bool litBoth = Lprev > 0.0 && Lnow > 0.0;
		const bool changed = cacheRowAbsent
		                   || ( litBoth && abs( Lprev - Lnow ) > cp.resetThresh * ( max( Lprev, Lnow ) + cp.resetBias ) );
		if ( cp.gradientReset && rtgiCachePresent( cp ) && exact && changed ) {
			rtgiCacheRequestReset( cp, Sp.Pw, Sp.Nw, u_modelViewMatrix[3].xyz );
		}
	}
	// SHADOW EDGES (above): judge the darkest previous sample too - last frame's light (its row now, through this frame's
	// table) re-shaded at the stored point under the current scene - and keep whichever judgement shows the bigger change
	// relative to its brightness (the confidence pass's darkness bias, u_tessParms.z, so a dark-on-dark wobble never
	// outbids a real change of the brightest sample). After the cache block: the cache resets on LIGHT changes only.
	if ( shadowSel && dkPrevRow != 0u && any( notEqual( dkFrag, bestFrag ) ) ) {
		uint defPlus1 = prevLights.l[ dkPrevRow - 1u ].pad0;
		uint dkCurRow = 0u;
		if ( defPlus1 != 0u && defPlus1 <= tableCap ) {
			DiLightMap table = DiLightMap( tableAddr );
			uint rp1 = table.row[ defPlus1 - 1u ];
			if ( rp1 != 0u && rp1 <= nLights && lights.l[ rp1 - 1u ].pad1 == prevLights.l[ dkPrevRow - 1u ].pad1 ) { dkCurRow = rp1; }
		}
		DiSurface Sd; float rawD; vec4 ntD;
		if ( dkCurRow != 0u && diSurfaceAt( dkFrag, applyMode, pbr, Sd, rawD, ntD ) ) {
			Sd.Pw = dkPrevP;
			Sd.Nw = dkPrevN;
			Sd.d  = -dkPrevVz;
			if ( applyMode ) {
				Sd.Vw    = normalize( u_projectionMatrix[3].xyz - Sd.Pw );
				Sd.NdotV = max( dot( Sd.Nw, Sd.Vw ), 1e-4 );
			}
			float dNow = 0.0;
			vec3 c, ld; float dl, ndl;
			if ( diShadeLight( lights, dkCurRow - 1u, Sd, c, ld, dl, ndl, false ) ) {
				float vis = ( ( lights.l[ dkCurRow - 1u ].flags & DUDE_LIGHT_NOSHADOWS ) != 0u )
					? 1.0 : traceVisibility( Sd.Pw, Sd.Nw, ld, dl, diLightBlurScale( lights.l[ dkCurRow - 1u ].flags ), occDist );
				dNow = diGradientLuminance( c, Sd.B, vis );
			}
			const float bias = max( u_tessParms.z, 1e-4 );
			float relB = abs( Lprev - Lnow ) / ( max( Lprev, Lnow ) + bias );
			float relD = abs( dkPrevLum - dNow ) / ( max( dkPrevLum, dNow ) + bias );
			if ( relD > relB ) {
				Lprev = dkPrevLum;
				Lnow  = dNow;
			}
		}
	}
	fragColor = vec4( abs( Lprev - Lnow ), max( Lprev, Lnow ), 0.0, 1.0 );
}
