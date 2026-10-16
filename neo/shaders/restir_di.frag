// DUDE ReSTIR DI gather — fresh 1-spp (RTGI architecture, docs/rtx-restir-di.md). Per pixel: importance-sample
// ONE light from the in-range set by weighted reservoir sampling - over EVERY light in the buffer by default
// (exhaustive RIS: the pick's only randomness is the choice among the lights that really reach the pixel) or
// over M random candidates (r_rtReStirDICandidates > 0) - trace ONE visibility ray, and shade the unbiased
// direct irradiance rad(y)·W·vis. NO history,
// NO persistent reservoir, NO reprojection - like rtgi_ray.frag this traces fresh every frame and hands ALL
// temporal accumulation to NRD (+ the DI confidence pass for moving-light antilag). That is what kills the
// moving-object trails a hand-rolled reproject leaves.
//
// The weapon is NOT masked out here (unlike RTGI / RTAO / SSR): a moving hole in NRD's input left the world its
// silhouette uncovers each frame without history (a fried halo). Its pixels are shaded like any other and the
// apply / debug overlay mask them at display, so it never receives world DI on screen and never ghosts.
//
// Output is the 3-attachment NRD guide MRT (CreateRenderTargetRtgiMrt), exactly like rtgi_ray.frag so the
// shared REBLUR_DIFFUSE denoise (RtaoDenoise giMode) drops in unchanged:
//   loc 0 = NRD-YCoCg direct irradiance (rgb) + normalized hit distance (a)   [IN_DIFF_RADIANCE_HITDIST]
//   loc 1 = signed linear view z (negative) (.x), RG32F                       [IN_VIEWZ]   + R1: .y = gradient luminance
//           (full float: the gradient rebuilds last frame's sample point from it and must land on the shaded point exactly)
//   loc 2 = world normal, NRD encoding 2 (signed octahedron)                  [IN_NORMAL_ROUGHNESS]
//   loc 3 = SH mode: SH1 = direction * luminance (.xyz)                       [IN_DIFF_SH1] + R1: .w = chosen light ROW + 1
// R1 (Plan R1, restir_di_gradient.frag): the side data in loc 1.y / loc 3.w is what NEXT frame's gradient pass
// re-shades - the chosen light's row in THIS frame's light buffer (the ring keeps it alive a frame) and the fp16
// luminance of its shaded visible contribution (diGradientLuminance). The gradient pass runs BEFORE the gather
// overwrites this MRT, so no ping-pong is needed. 0 = no light chosen.
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
//   u_localParam0         = ( 1/proj00, 1/proj11, candidate count M (0 = EVERY light: exhaustive RIS), hit-distance scale (NRD A = 0.5*this) )
//   u_localParam1.x       = frame index (RNG decorrelation)
//   u_localParam1.y       = r_pbr (apply mode: 1 = GGX metalness workflow as interaction.frag, 0 = Lambert only)
//   u_localParam1.z       = checkerboard parity (0 = off, 1 = even frame, 2 = odd; packs half the pixels left)
//   u_localParam1.w       = APPLY MODE (Stage D, r_rtReStirDIApply): shade with the G-buffer material (units 2/3)
//                           and write the DEMODULATED radiance (restir_di_common.glsl); 0 = irradiance, albedo 1
//   u_diffuseModifier     = apply mode: ( kd = 1 - r_pbrMetalDiffuse, r_pbrSpecScale, r_pbrFireflyClamp, r_pbrEnvScale )
//   u_screenCorrection.xy = 1 / viewSize        (gl_FragCoord -> [0,1] uv; normal buffer)
//   u_depthTexRecip.xy    = gl_FragCoord -> _currentDepth tc
//   u_windowCoord.x       = perforated casters: 1 = every visibility ray takes the grate-aware 0xFF walk (restir_di_shade.glsl), 0 = off
//   u_windowCoord.y       = perforated casters: their shadow strength (r_shadowMapPerforatedStrength)
//   u_windowCoord.z       = view-Y sign (-1 Vulkan top-down)
//   u_windowCoord.w       = r_rtLightTextures: 1 = attenuate with each light's real cookie + falloff textures
//   u_particleRadius.xy   = geometry-table device address (bit-cast; grate candidates -> coverage texture)
//   u_shadowFalloffS.x    = forward light scale (backEnd.lightScale): the DI term is scaled here, not at the apply
//   u_shadowFalloffS.y    = world size of one FULL-RES screen pixel per unit view depth (2/(proj00*viewW)): cookie LOD
//   u_shadowFalloffS.w    = R2 ReLAX MODE (DI is ReLAX-only since R6): loc 0 = LINEAR radiance + RAW hit distance (world units,
//                           RELAX_FrontEnd_PackRadianceAndHitDist) instead of YCoCg + normalized; SH1 is the same direction * luminance
//   u_shadowFalloffS.z    = SH MODE (r_rtReStirDISH): the per-light cosine N.L is NOT applied here; loc 3 carries SH1 =
//                           direction * luminance (NRD REBLUR_FrontEnd_PackSh) and the apply re-resolves the cosine
//                           against the FULL-res bump normal - the shading detail survives the denoise + reduced res
//   --- r_rtReStirDICombineGI (RTGI bounce folded into THIS buffer, one NRD denoise; rtgi_bounce.glsl) ---
//   u_specularParms       = ( combine on/off, fog strength, fog coef (exp), skip fast lights in the bounce )   [fog: rtgi_apply's r_rtgiFogAttenuate]
//   u_pbrParms            = ( gather radius, r_rtgiEmissive, r_rtgiEmissiveClamp, r_rtgiLightScale )
//   u_pbrParms2           = ( ambient floor, shadow-ray gate, eye pullback per unit depth, r_rtgiIntensity )
//   u_occlusionParms      = ( IGN rotation cos, sin, IGN offset x, y )   [CPU-hoisted per frame, as RB_RHI_RtgiPass]
//   --- R3a disocclusion boost (r_rtReStirDIBoost, docs/rtx-restir-di.md Plan R3) ---
//   u_tessParms           = ( K picks (0 = off), boost-ON NRD history (<= 0: trigger off), HOLD frames,
//                             LAST frame's checkerboard parity: 0 = off, 1 / 2 = parity, -1 = NO side data this frame )
//   u_prevMvpMatrix       = LAST frame's world -> view (the expected previous view z of this surface point)
//   unit 4  = LAST frame's denoised DI: .w = NRD's accumulated history length in frames (ReLAX always; ReBLUR with
//             returnHistoryLengthInsteadOfOcclusion), reprojected here through unit 10
//   unit 5  = LAST frame's gather att1 copy ( viewZ, lum )         unit 6 = LAST frame's att2 copy (NRD-packed normal)
//   unit 9  = LAST frame's att3 copy ( SH1, row + 1 [+ 0.5 = that pixel was boosted last frame] )
//   unit 10 = velocity at the gather res (currUV - prevUV, +Y-up)
//   A YOUNG pixel - just revealed (a mover's wake, a screen edge while turning, a cut) or one whose history NRD still
//   counts short - gets K independent reservoir picks from the same candidate weights (one light loop) and K visibility
//   rays, averaged: 1/K the variance exactly where the denoiser would otherwise widen its filter and smear the adjacent
//   shadow across the wake for the whole refill. Hysteresis (the stored bit): once boosted, a pixel stays boosted until
//   its history reaches the OFF threshold, so the ray count never flips while the history hovers around the ON one
//   (the confidence clamp makes it hover). RTXDI / Remix: numDisocclusionBoostSamples, without neighbour reservoirs.
// DI_BOOST: the R3a disocclusion boost is a COMPILED VARIANT (restir_di_boost.frag = this file with DI_BOOST 1): its
// K-pick arrays stay live for every pixel whether or not it is boosted and cost the whole gather ~0.7 ms of occupancy
// (6.8 vs 6.1 ms standing still, 2026-10-12), so the plain gather must not carry them. The CPU picks the program.
#ifndef DI_BOOST
#define DI_BOOST 0
#endif
#extension GL_EXT_buffer_reference_uvec2 : require	// build the light-buffer pointer from u_color.xy (uvec2)
#extension GL_EXT_ray_query : require				// one visibility ray for the chosen light
#extension GL_EXT_nonuniform_qualifier : require		// bindless light textures (rt_light_tex.glsl)
#if DI_BOOST
#extension GL_EXT_terminate_invocation : require		// terminateInvocation: a plain discard compiled to a DEMOTE here, and a demoted
														// invocation keeps running the light loop + rays as a helper (3.5 ms of nothing)
#endif

#include "renderparms.glsl"
#include "rt_light_eval.glsl"
#include "rt_light_tex.glsl"
// grates / fences as shadow casters (same recipe as interaction.frag's inline ray)
#include "rt_perforated.glsl"
#include "restir_di_common.glsl"
// the surface reconstruction, per-light shade and visibility ray, shared with restir_di_gradient.frag (R1)
#include "restir_di_shade.glsl"
// the RTGI bounce (trace + hit shading), shared with rtgi_ray.frag, for the combined DI+GI buffer
#include "rtgi_bounce.glsl"

#if DI_BOOST
// the young test + its inputs, shared with restir_di_young.frag
#include "restir_di_boost.glsl"
#endif

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;	// IN_DIFF_RADIANCE_HITDIST: NRD-YCoCg direct irradiance (rgb) + normHitDist (a)
layout(location = 1) out vec4 outViewZ;		// IN_VIEWZ: signed linear view z (negative) in .x; R1: .y = gradient luminance (RG32F)
layout(location = 2) out vec4 outNormal;	// IN_NORMAL_ROUGHNESS: world normal, NRD encoding 2 (signed octahedron), RGBA16F
layout(location = 3) out vec4 outSh1;		// SH mode: IN_DIFF_SH1 = incoming direction * luminance (.xyz); R1: .w = chosen light row + 1

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

// R3a: is this pixel YOUNG - does it get the boosted picks this frame? (header: u_tessParms, u_prevMvpMatrix, units 4/5/6/9/10)
void main() {
	vec2  frag = gl_FragCoord.xy;
	outSh1 = vec4( 0.0 );							// SH mode: no light -> no direction (every early-out below leaves this)

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
	// Surface reconstruction + (apply mode) material, shared with the gradient pass (restir_di_shade.glsl). The NRD
	// guides are written first, for EVERY pixel (mirrors rtgi_ray.frag) so the shared REBLUR_DIFFUSE denoise sees a
	// valid guide field even on sky / weapon / degenerate pixels (the radiance is zeroed there below). In
	// checkerboard mode these gather-written guides are ignored (NRD reads the full-res guide pass instead).
	const bool applyMode = u_localParam1.w > 0.5;
	const bool pbr       = u_localParam1.y > 0.5;
	// cheap RIS target (r_rtReStirDICheapTarget): weigh the ~38 candidates with the unshadowed irradiance (no GGX) and
	// full-shade only the winner. u_localParam1.w encodes it: 0 = debug/no-apply, 1 = apply + cheap target, 2 = apply + full.
	const bool cheapTarget = u_localParam1.w > 0.5 && u_localParam1.w < 1.5;
	DiSurface S;
	float raw;
	vec4  nt;
	const bool surfaceOk = diSurfaceAt( sfrag, applyMode, pbr, S, raw, nt );
	outViewZ  = vec4( S.vz, 0.0, 0.0, 1.0 );						// .y (gradient luminance) filled at the end
	outNormal = packNrdNormal( nt.xyz * 2.0 - 1.0 );

	// checkerboard packs traced samples into the LEFT HALF; the right half traces nothing (NRD reconstructs it)
	if ( cbMode > 0 && frag.x * u_screenCorrection.x >= 0.5 ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }
	if ( !surfaceOk ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }	// sky / no geometry / degenerate normal
	// The view weapon (nt.a < 0.5) is NOT masked here: a moving hole in the denoiser's input leaves the world its
	// silhouette uncovers each frame with no history (fried halo). Its pixels get a DI of their own (depth-hacked,
	// never displayed) so NRD sees a continuous field; the apply and the debug overlay mask it at DISPLAY.
	vec3  Pw = S.Pw;
	vec3  Nw = S.Nw;
	float d  = S.d;
	vec3  albedo = S.albedo;
	vec3  B      = S.B;

	RtLights lights = RtLights( uvec2( floatBitsToUint( u_color.x ), floatBitsToUint( u_color.y ) ) );
	uint nLights = uint( u_color.z );
	if ( nLights == 0u ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }

	// Candidate set (u_localParam0.z = r_rtReStirDICandidates): 0 (default) = EXHAUSTIVE - enumerate every light in
	// the buffer once (candidate pdf 1, RIS weight = pHat); M > 0 = draw M candidates uniformly WITH replacement
	// (pdf 1/nLights, weight pHat*nLights), the textbook RTXDI recipe for thousands of lights. With <= 256 lights,
	// the 16 B sphere cull and the exact box pre-reject in dudeLightAttenTex, enumeration costs about what the
	// bounce's light loop costs (rtgi_bounce.glsl) and removes the candidate variance entirely: 32 uniform draws
	// from a ~200-light buffer (a lit corridor) reached the dominant light at only ~15% of the pixels, as a ~6x
	// spike that NRD's anti-firefly then clipped - the skyway "sun" through the windows read noticeably dimmer
	// than forward (user, 2026-10-09). M >= nLights enumerates too (a draw with replacement never beats it).
	uint M = uint( max( u_localParam0.z, 0.0 ) + 0.5 );
	bool exhaustive = ( M == 0u ) || ( M >= nLights );
	uint draws = exhaustive ? nLights : M;

	// RNG seed: SCREEN pixel (sfrag, so a checkerboard cell is seeded by the pixel it represents) + frame,
	// forced odd so the first advance never degenerates
	uint rng = ( ( uint( sfrag.x ) * 1973u + uint( sfrag.y ) * 9277u )
	             ^ ( uint( u_localParam1.x ) * 26699u ) ) | 1u;

	// weighted reservoir sampling over the candidates: every light once (exhaustive, pdf 1 -> RIS weight pHat)
	// or M uniform draws (pdf 1/nLights -> weight pHat*nLights). pHat = luminance of the unshadowed contribution.
	// A cheap bounding-sphere cull skips lights that cannot reach the point. The reservoir is TRANSIENT
	// (this frame only) - NRD does the temporal accumulation downstream.
	const bool shMode = u_shadowFalloffS.z > 0.5;
	// R3a: how many independent picks (and visibility rays) this pixel gets - K where it is young, else 1 (diBoostWanted)
#if DI_BOOST
	// SECOND PASS over the gather's targets (restir.di.boost): this program runs after the plain gather and only the
	// YOUNG pixels survive the discard below - everyone else keeps the plain gather's output. The plain program carries
	// none of this code: with the boost compiled into the one gather the whole pass cost ~2.5 ms more on EVERY pixel,
	// boosted or not (2026-10-12), so the boost lives in its own program and its own (mostly discarded) pass.
	#define DI_BOOST_MAX 4
	// never the view weapon (nt.a < 0.5): its DI is shaded for the denoiser's continuity but never displayed, and its sway
	// keeps its history short - K rays there would be pure waste (view 7 showed it solid red)
	const int kMax = clamp( int( u_tessParms.x + 0.5 ), 2, DI_BOOST_MAX );
	// CLUSTERED young only: the young pass wrote every pixel's reason; a pixel is boosted when it is young AND at least 5 of
	// its 3x3 neighbours are. Isolated specks (NRD's own edge resets, thin-geometry jitter) are never worth K rays, and -
	// decisive for the cost - a warp with a single surviving lane still runs the whole light loop and the rays for all 32
	// lanes' time, so scattered specks along every edge priced the pass at ~3 ms standing still (2026-10-12). Clusters
	// (wakes, entry bands) keep whole warps alive together; speck-only warps terminate as one.
	int boostWhy = 0;
	{
		ivec2 ysz = textureSize( u_youngIn, 0 );							// the young pass's output for THIS frame ( reason / 4, age / 64 )
		ivec2 yp  = clamp( ivec2( floor( frag ) ), ivec2( 0 ), ysz - 1 );			// the young pass is laid out exactly like this MRT
		boostWhy = int( floor( texelFetch( u_youngIn, yp, 0 ).x * 4.0 + 0.5 ) );
		if ( boostWhy != 0 ) {
			int n = 0;
			for ( int dy = -1; dy <= 1; dy++ ) {
				for ( int dx = -1; dx <= 1; dx++ ) {
					ivec2 q = clamp( yp + ivec2( dx, dy ), ivec2( 0 ), ysz - 1 );
					if ( texelFetch( u_youngIn, q, 0 ).x > 0.125 ) { n++; }
				}
			}
			if ( n < 5 ) { boostWhy = 0; }
		}
	}
	if ( boostWhy == 0 ) { terminateInvocation; }								// not (clustered) young: the plain gather's result stands
	const bool boostedNow = true;
	const int K = kMax;
	// the extra picks keep only their light INDEX (+1; 0 = none); the picked lights are re-shaded after the loop
	// (deterministic, so pHat matches the loop's)
	uint pickB[DI_BOOST_MAX];
	for ( int k = 0; k < DI_BOOST_MAX; k++ ) { pickB[k] = 0u; }
#else
	const int  K = 1;
	const bool boostedNow = false;
	const int  boostWhy = 0;
#endif

	float wSum        = 0.0;
	float pHatChosen  = 0.0;					// the TARGET (cheap irradiance or full shade) of the chosen, for the RIS weight W
	uint  rowChosen   = 0u;						// R1: chosen light's row + 1 (0 = none)
	for ( uint i = 0u; i < draws; i++ ) {
		uint li = i;
		if ( !exhaustive ) {
			li = uint( rnd( rng ) * float( nLights ) );
			if ( li >= nLights ) { li = nLights - 1u; }	// guard the rare rnd()==1.0
		}
		vec3  ldir, contrib;
		float dl, ndl;
		float pHat = 0.0;
		// RIS TARGET only: the cheap unshadowed irradiance (no GGX material) when cheapTarget, else the full shade. The
		// winner is re-shaded at full material once AFTER the loop, so a pixel pays ~38 cheap targets + 1 full GGX instead
		// of ~38 full GGX evals. false = cannot reach (culled / forward-path / N.L <= 0).
		if ( diShadeLight( lights, li, S, contrib, ldir, dl, ndl, cheapTarget ) ) {
			pHat = dot( contrib, vec3( 0.2126, 0.7152, 0.0722 ) );		// luminance of the target
		}
		float w    = exhaustive ? pHat : pHat * float( nLights );	// pHat / candidate pdf
		wSum += w;
		if ( wSum > 0.0 && rnd( rng ) < ( w / wSum ) ) {
			pHatChosen  = pHat;
			rowChosen   = li + 1u;
		}
#if DI_BOOST
		// R3a: the extra independent picks share the candidate weights (same wSum), each with its own random draws
		for ( int k = 1; k < K; k++ ) {
			if ( wSum > 0.0 && rnd( rng ) < ( w / wSum ) ) { pickB[k] = li + 1u; }
		}
#endif
	}

	if ( !( pHatChosen > 0.0 ) && u_specularParms.x < 0.5 ) {
		outSh1.w = 0.25 * float( boostWhy );											// view 7: an unlit young pixel still ran the loop
		fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return;									// no reaching light (and no bounce to add)
	}

	// full-shade the WINNER once (targetOnly = false): its real GGX contribution drives the radiance + the gradient
	// luminance, and its ldir/dl the visibility ray. In full-target mode this reproduces the loop's shade exactly.
	vec3  radChosen     = vec3( 0.0 );
	vec3  contribChosen = vec3( 0.0 );			// R1: the chosen light's shaded contribution (with cosine), for the gradient luminance
	vec3  ldirChosen    = vec3( 0.0 );
	float dlChosen      = 0.0;
	uint  flagsChosen   = 0u;
	if ( pHatChosen > 0.0 ) {
		float ndlC;
		diShadeLight( lights, rowChosen - 1u, S, contribChosen, ldirChosen, dlChosen, ndlC, false );
		radChosen   = ( shMode ? contribChosen / max( ndlC, 0.05 ) : contribChosen ) / B;	// demodulated (B = 1 outside apply mode)
		flagsChosen = lights.l[ rowChosen - 1u ].flags;
	}

	// unbiased contribution weight W = (1/pHat(y))·(wSum/M): pHat is the TARGET (cheap or full), f(y) = the full shade
	// above, so rad(y)·W·vis stays unbiased. One visibility ray for the chosen light; NOSHADOWS lights skip it.
	float W   = ( pHatChosen > 0.0 ) ? ( wSum / ( exhaustive ? 1.0 : float( draws ) ) ) / pHatChosen : 0.0;
	float occDist = dlChosen;
	float vis = ( !( pHatChosen > 0.0 ) || ( flagsChosen & DUDE_LIGHT_NOSHADOWS ) != 0u )
		? 1.0 : traceVisibility( Pw, Nw, ldirChosen, dlChosen, occDist );

	// the direct term, at the forward path's light scale (lightColor = lightScale * stage colour there)
	vec3 outRad = max( radChosen * ( W * vis * u_shadowFalloffS.x ), vec3( 0.0 ) );
	// R3a: average the extra picks' unbiased estimates with the first (each rad_k * W_k * vis_k is unbiased on its own);
	// shDirect = the averaged direct SH1 term (direction * luminance) the SH block below uses instead of the single pick's
	vec3 shDirect = ldirChosen * dot( outRad, vec3( 0.2126, 0.7152, 0.0722 ) );
#if DI_BOOST
	if ( K > 1 ) {
		vec3 sumRad = outRad;
		vec3 sumSh  = shDirect;
		for ( int k = 1; k < K; k++ ) {
			if ( pickB[k] == 0u ) { continue; }
			uint lk = pickB[k] - 1u;
			vec3 c, ld; float dl, ndl;
			// the TARGET pHat (cheap or full, consistent with wSum) for Wk, then the full shade for the radiance
			if ( !diShadeLight( lights, lk, S, c, ld, dl, ndl, cheapTarget ) ) { continue; }
			float pH = dot( c, vec3( 0.2126, 0.7152, 0.0722 ) );
			if ( !( pH > 0.0 ) ) { continue; }
			vec3  cF = c;	float ndlF = ndl;
			if ( cheapTarget ) { vec3 ldF; float dlF; diShadeLight( lights, lk, S, cF, ldF, dlF, ndlF, false ); }
			vec3  rk = ( shMode ? cF / max( ndlF, 0.05 ) : cF ) / B;
			float Wk = ( wSum / ( exhaustive ? 1.0 : float( draws ) ) ) / pH;
			float od;
			float visK = ( ( lights.l[ lk ].flags & DUDE_LIGHT_NOSHADOWS ) != 0u ) ? 1.0 : traceVisibility( Pw, Nw, ld, dl, od );
			rk = max( rk * ( Wk * visK * u_shadowFalloffS.x ), vec3( 0.0 ) );
			sumRad += rk;
			sumSh  += ld * dot( rk, vec3( 0.2126, 0.7152, 0.0722 ) );
		}
		outRad   = sumRad / float( K );
		shDirect = sumSh / float( K );
	}
#endif
	// R1 side outputs for next frame's gradient pass: which light this pixel shaded (row + 1 in THIS frame's light
	// buffer) and how bright its visible contribution was (fp16, restir_di_shade.glsl). The RIS weight W is left out:
	// it multiplies both frames alike and cancels in the normalized gradient. R3a keeps its "boosted" bit in the row
	// id's fraction (+0.25 x reason, exact in fp16 below 512 rows); readers floor it off.
	if ( pHatChosen > 0.0 ) {
		outViewZ.y = diGradientLuminance( contribChosen, B, vis );
	}
	outSh1.w = float( rowChosen ) + 0.25 * float( boostWhy );			// fraction = the boost reason (0 = not boosted), row 0 = no light:
																		// recorded for UNLIT young pixels too (they ran the loop; view 7 must show them)
	vec3 shDir  = vec3( 0.0 );										// SH mode: accumulated direction * luminance

	// r_rtReStirDICombineGI: the ONE-bounce indirect term from the same pixel, folded into the same buffer so ONE
	// NRD denoise serves direct + indirect (the bounce's own pass stands down). Finished exactly as rtgi_apply.frag
	// would: albedo (the receiver's, 1 outside apply mode) * Li * r_rtgiIntensity, faded through the view's fog
	// (pow( exp( depth * coef ), strength )), then demodulated by the same B as the direct term so the apply's
	// remultiply is exact for both. Its own hit distance is NOT fed to NRD (the direct term's light distance
	// drives the blur radius for both - the shared-denoise compromise).
	if ( u_specularParms.x > 0.5 ) {
		RtgiBounceParms bp;
		bp.tlas           = uvec2( floatBitsToUint( u_rtParms.x ), floatBitsToUint( u_rtParms.y ) );
		bp.geoTable       = uvec2( floatBitsToUint( u_particleRadius.x ), floatBitsToUint( u_particleRadius.y ) );
		bp.lightAddr      = uvec2( floatBitsToUint( u_color.x ), floatBitsToUint( u_color.y ) );
		bp.nLights        = nLights;
		bp.realLights     = nLights > 0u;
		bp.gate           = true;
		bp.texturedLights = u_windowCoord.w > 0.5;
		bp.shadowRay      = u_pbrParms2.y > 0.5;
		bp.skipForwardLights = u_specularParms.w > 0.5;	// r_rtReStirDIDynamicForward: a fast light has NO bounce rather than a trailing one
		bp.radius         = u_pbrParms.x;
		bp.emissiveScale  = u_pbrParms.y;
		bp.emissiveClamp  = u_pbrParms.z;
		bp.lightScale     = u_pbrParms.w;
		bp.ambientFloor   = u_pbrParms2.x;
		bp.eyePullback    = u_pbrParms2.z;
		bp.ignRot         = u_occlusionParms.xy;
		bp.ignOfs         = u_occlusionParms.zw;
		vec3 Li; float thit; vec3 bounceDir;
		if ( rtgiTraceBounce( bp, Pw, Nw, u_modelViewMatrix[3].xyz, d, sfrag, Li, thit, bounceDir ) == 1 ) {
			vec3 gi = Li * u_pbrParms2.w;
			if ( u_specularParms.y > 0.0 ) {
				gi *= pow( exp( d * u_specularParms.z ), u_specularParms.y );		// rtgi_apply's fog transmittance
			}
			vec3 giRad = max( albedo * gi, vec3( 0.0 ) ) / B;
			if ( shMode ) {
				// the cosine-sampled bounce carries this normal's cosine implicitly: remove it (floored) so the apply's
				// re-applied cosine of the full-res normal is not doubled; its direction is the sample direction
				giRad /= max( dot( Nw, bounceDir ), 0.05 );
				shDir += bounceDir * dot( giRad, vec3( 0.2126, 0.7152, 0.0722 ) );
			}
			outRad += giRad;
		}
	}
	if ( shMode ) {
		// NRD SH1 for the direct term: direction * luminance (REBLUR_FrontEnd_PackSh: c1 = direction * Y)
		shDir += shDirect;										// (R3a: the K-averaged term when boosted)
		outSh1.xyz = shDir;										// .w keeps the R1 row id
	}

	// NRD IN_DIFF_RADIANCE_HITDIST: the "hit distance" for direct lighting is the distance to the chosen
	// light - it drives ReBLUR's world-space blur radius (near light -> tight penumbra, far light -> wider),
	// the direct-lighting analogue of the gather-ray distance RTGI feeds. Normalized saturate( dl / (A + d*B) )
	// with A = 0.5 * u_localParam0.w, B = 0.1, MUST match the ReblurHitDistanceParameters in RB_RHI_ReStirDIPass.
	// occluder distance when shadowed (tight blur for near casters: grates, contact), light distance when lit
	float nhd = clamp( occDist / ( 0.5 * u_localParam0.w + d * 0.1 ), 0.0, 1.0 );
	// R2: ReLAX takes linear radiance + the raw hit distance (RELAX_FrontEnd_PackRadianceAndHitDist); ReBLUR YCoCg + normalized
	fragColor = ( u_shadowFalloffS.w > 0.5 ) ? vec4( outRad, min( occDist, 65504.0 ) ) : vec4( nrdLinearToYCoCg( outRad ), nhd );
}
