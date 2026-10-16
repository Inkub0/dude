// DUDE RT IRRADIANCE CACHE (docs/rtx-irradiance-cache.md): a camera-adaptive hashed world-space cache of the
// INDIRECT light a surface receives, learned from the RT GI bounce and read back at the bounce HIT - the recursion
// that turns the one-bounce gather into multi-bounce GI. Built on rt_hash_grid.glsl (keying + probing); this file
// is the cache's own slot layout, its accumulation protocol and the three operations the passes perform:
//
//   UPDATE  (gather, at the primary point P): atomically add the pixel's bounce estimate Li into P's cell.
//   RESOLVE (rtgi_cache_resolve.comp, once per frame): per touched cell irr = mix( irr, sum / count, alpha ), alpha =
//           max( 1 / N, alphaMin ), N = min( N + 1, Nmax ); clear the sums. (SHARC's accumulate + resolve split.)
//   LOOKUP  (rtgi_bounce.glsl, at the hit H): Li += min( albedo(H), clamp ) * irr(H's cell) * scale. A miss or an
//           unresolved cell adds nothing, so a cold cache is exactly today's one bounce.
//
// Units: Li is what rtgiTraceBounce returns - the hit's lit colour in Doom's lighting model (no 1/pi; the receiver's
// albedo and the cosine-sampling pi cancel at the consume site), so the cached value is the mean of those estimates
// over the cell's pixels and frames: "irradiance-like" in the engine's convention, and adding albedo(H) * E_cache(H)
// at a hit is exactly how the hit's own direct term is formed there. Multi-bounce follows: E(P) = L_direct(H) +
// albedo(H) * E(H), a geometric series in the albedo - the albedo clamp keeps it strictly convergent.
//
// Slot (16 B): key (0 = empty) | stamp (frame of last update) | irr.rg as f16x2 | irr.b as f16 + N (u16) in the
// high half. Accumulation entry (16 B, index-aligned with the slot): r, g, b fixed point x1024 as uints (atomicAdd -
// no float atomics extension) | count, bit 31 = RESET request (the resolve restarts the cell: N = 0).
//
// Needs rt_hash_grid.glsl before this include and GL_EXT_buffer_reference_uvec2 in the including shader.

struct HgSlot  { uint key; uint stamp; uint irrRG; uint irrBN; };
struct HgAccum { uint r; uint g; uint b; uint count; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer RtgiCacheSlots  { HgSlot  s[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer RtgiCacheAccums { HgAccum a[]; };

const float RTGI_CACHE_FIXED      = 1024.0;			// accumulation fixed point (x1024: 4096 per channel per cell-frame of headroom)
const uint  RTGI_CACHE_RESET_BIT  = 0x80000000u;	// HgAccum.count: restart this cell at the resolve (the DI gradient saw its light change)
const uint  RTGI_CACHE_EVICT_BIT  = 0x40000000u;	// HgAccum.count: the slot changed hands this frame - drop the (mixed) sums, keep the payload
const uint  RTGI_CACHE_COUNT_MASK = 0x3FFFFFFFu;
const uint  RTGI_CACHE_STRIDE     = 4u;				// uints per slot (HgWords view)
const uint  RTGI_CACHE_STAMP_OFS  = 1u;				// HgSlot.stamp
// HgSlot.irrBN: bits 0-15 irr.b (f16), 16-23 N (8 bits: Nmax <= 255 frames), 24-28 the reset cooldown (5 bits), then
// three flag bits the debug activity view reads (r_rtgiCacheDebug 5): 29 SEEDED - set by the gather when it seeds a
// fresh cell from its coarser parent, consumed by the next resolve; 30 FRESH - the last resolve created the cell (N
// 0 -> 1) or was a seeded cell's first; 31 RESET - the last resolve restarted the cell on a reset request.
const uint  RTGI_CACHE_N_SHIFT    = 16u;
const uint  RTGI_CACHE_N_MASK     = 0xFFu;
const uint  RTGI_CACHE_N_MAX      = 255u;
const uint  RTGI_CACHE_SEEDED_BIT = 0x20000000u;
const uint  RTGI_CACHE_FRESH_BIT  = 0x40000000u;
const uint  RTGI_CACHE_RESET_FLAG = 0x80000000u;
const uint  RTGI_CACHE_SEED_N     = 4u;				// frames of trust a cell seeded from its coarser parent starts with
// The RESET COOLDOWN: resolves left since the cell last restarted, counted down by the resolve from the push-constant
// value the CPU derives from a DURATION (RTGI_CACHE_COOL_MS, 250 ms, through the low-passed frame rate - the boundary
// between a one-shot change to follow and a periodic one to average is a frequency, so it must not shrink at 144 Hz).
// A reset request arriving inside it re-arms it without restarting: a light that changes every frame (an authored
// flicker / strobe table, a swinging cookie) would otherwise pin the cell at one frame's mean forever; held, the cell
// blends the changes and converges to the light's average, which is what a cache of a flickering light should hold. A
// one-shot change (a switch) restarts at once.
const uint  RTGI_CACHE_COOL_SHIFT  = 24u;
const uint  RTGI_CACHE_COOL_MASK   = 0x1Fu;

// Per-pass constants, packed by the CPU (RhiWorld.cpp RB_RHI_RtgiCachePack) identically for every consumer:
//   u_channelMask  = ( accum address lo, hi [bit-cast], slot mask [bit-cast uint], tanAlphaHalf )
//   u_skinParms    = ( scale, frame stamp [bit-cast uint], flags [bit-cast uint: 1 update, 2 lookup, 4 gradient reset,
//                      8 velocity at unit 10 valid: movers never teach, 16 TAIL mode: the cell holds L_out and a converged
//                      cell IS the bounce's path tail], gradient reset threshold (relative luminance change) )
//   u_contactParms = ( evict age in frames (0 = never), per-sample peak clamp, albedo clamp at the hit, reset darkness bias )
// The SLOT address differs per consumer (restir_di.frag + the gradient: u_particleRadius.zw; rtgi_ray.frag and the
// debug overlay: u_alphaTest.xy), so it is passed in.
struct RtgiCacheParms {
	uvec2 slotAddr;
	uvec2 accumAddr;
	uint  slotMask;
	float tanAlphaHalf;
	float scale;
	uint  frame;
	uint  evictAge;
	float sampleClamp;
	float albedoClamp;
	float resetThresh;
	float resetBias;		// darkness bias in the reset test's denominator: changes of a near-black direct term never reset
	bool  update;
	bool  lookup;
	bool  gradientReset;
	bool  moverTest;		// the per-object velocity is bound (unit 10): a pixel moving beyond the camera's own motion is a mover
	bool  tail;				// C6 TAIL mode: cells store L_out = albedo*(E_dir+E_ind); a converged+fresh cell serves the bounce hit
};

// (compute consumers bind the buffers directly and have no RenderParams block: they define RTGI_CACHE_NO_UNIFORMS)
#ifndef RTGI_CACHE_NO_UNIFORMS
RtgiCacheParms rtgiCacheParmsFromUniforms( uvec2 slotAddr ) {
	RtgiCacheParms p;
	p.slotAddr     = slotAddr;
	p.accumAddr    = uvec2( floatBitsToUint( u_channelMask.x ), floatBitsToUint( u_channelMask.y ) );
	p.slotMask     = floatBitsToUint( u_channelMask.z );
	p.tanAlphaHalf = u_channelMask.w;
	p.scale        = u_skinParms.x;
	p.frame        = floatBitsToUint( u_skinParms.y );
	uint flags     = floatBitsToUint( u_skinParms.z );
	p.update        = ( flags & 1u ) != 0u;
	p.lookup        = ( flags & 2u ) != 0u;
	p.gradientReset = ( flags & 4u ) != 0u;
	p.moverTest     = ( flags & 8u ) != 0u;
	p.tail          = ( flags & 16u ) != 0u;
	p.resetThresh  = u_skinParms.w;
	p.evictAge     = uint( max( u_contactParms.x, 0.0 ) + 0.5 );
	p.sampleClamp  = u_contactParms.y;
	p.albedoClamp  = u_contactParms.z;
	p.resetBias    = u_contactParms.w;
	return p;
}
#endif

bool rtgiCachePresent( RtgiCacheParms p ) {
	return ( p.slotAddr.x | p.slotAddr.y ) != 0u && ( p.accumAddr.x | p.accumAddr.y ) != 0u;
}

vec3 rtgiCacheUnpackIrr( HgSlot s ) {
	return vec3( unpackHalf2x16( s.irrRG ), unpackHalf2x16( s.irrBN ).x );
}

uint rtgiCacheN( HgSlot s ) {
	return ( s.irrBN >> RTGI_CACHE_N_SHIFT ) & RTGI_CACHE_N_MASK;
}

uint rtgiCacheFlags( HgSlot s ) {
	return s.irrBN & ( RTGI_CACHE_SEEDED_BIT | RTGI_CACHE_FRESH_BIT | RTGI_CACHE_RESET_FLAG );
}

uint rtgiCacheCooldown( HgSlot s ) {
	return ( s.irrBN >> RTGI_CACHE_COOL_SHIFT ) & RTGI_CACHE_COOL_MASK;
}

// flags = the activity bits OR the cooldown already shifted into place ( cool << RTGI_CACHE_COOL_SHIFT )
void rtgiCachePackIrr( inout HgSlot s, vec3 irr, uint N, uint flags ) {
	s.irrRG = packHalf2x16( irr.rg );
	s.irrBN = ( packHalf2x16( vec2( irr.b, 0.0 ) ) & 0xFFFFu ) | ( min( N, RTGI_CACHE_N_MAX ) << RTGI_CACHE_N_SHIFT ) | flags;
}

// ---- fused probing: a span's four slots in ONE memory round trip ---------------------------------------------------
// The generic hgFind / hgClaim (rt_hash_grid.glsl, kept for the guiding grids) read one key word at a time: up to four
// DEPENDENT loads per probe, each a full memory round trip, in the latency-bound gather - and the payload a fifth. The
// cache's 16 B slots are one uvec4 each, so a whole span is four INDEPENDENT 16 B loads the compiler issues together:
// the probe and the payload in one round trip (the DI gather's cache share measured 22 % with the chained version).
layout(buffer_reference, std430, buffer_reference_align = 16) buffer RtgiCacheSpan { uvec4 s[]; };

uint rtgiCacheNv( uvec4 s )   { return ( s.w >> RTGI_CACHE_N_SHIFT ) & RTGI_CACHE_N_MASK; }
vec3 rtgiCacheIrrv( uvec4 s ) { return vec3( unpackHalf2x16( s.z ), unpackHalf2x16( s.w ).x ); }

// C6 TAIL mode: a cell may SERVE the bounce hit (its stored L_out replaces the live shading) only once it is both
// CONVERGED - N >= SEED_N, the trust a seeded cell is born with, so a cell cut this instant does not bounce one frame's
// 1-spp mean - and FRESH: last updated less than evictAge frames ago, so a stale off-screen cell cannot keep bouncing
// old light through a door it is no longer seen from (an unserved stale cell falls back to live shading, never wrong).
bool rtgiCacheServable( RtgiCacheParms p, uvec4 s ) {
	return rtgiCacheNv( s ) >= RTGI_CACHE_SEED_N && ( p.evictAge == 0u || ( p.frame - s.y ) < p.evictAge );
}

// the slot of `key` among a span's four preloaded slots: 0..3, or -1 = absent (slots fill contiguously from the first
// empty one and are never emptied short of a full clear, so an empty slot ends the search)
int rtgiCacheSpanFind( uvec4 s0, uvec4 s1, uvec4 s2, uvec4 s3, uint key, out uvec4 hit ) {
	hit = uvec4( 0u );
	if ( s0.x == key ) { hit = s0; return 0; }
	if ( s0.x == 0u ) { return -1; }
	if ( s1.x == key ) { hit = s1; return 1; }
	if ( s1.x == 0u ) { return -1; }
	if ( s2.x == key ) { hit = s2; return 2; }
	if ( s2.x == 0u ) { return -1; }
	if ( s3.x == key ) { hit = s3; return 3; }
	return -1;
}

// Claim P's slot for key k: the slot that already holds it, else the first empty slot of the span (atomicCompSwap: every
// pixel of a new cell in a frame lands in one slot; `fresh` = this invocation created it), else - when evictAge > 0 - the
// oldest foreign slot whose stamp is at least evictAge frames old (stamp 0 = claimed this frame, never evicted), taken
// over with a CAS (`evicted` = the payload belongs to the old key). -1 = the span is full of live foreign cells: the
// sample is dropped. The span is preloaded (one round trip); the CASes only follow when needed.
int rtgiCacheClaim( RtgiCacheParms p, HgKey k, out bool evicted, out bool fresh ) {
	evicted = false;
	fresh = false;
	RtgiCacheSpan span  = RtgiCacheSpan( p.slotAddr );
	HgWords       words = HgWords( p.slotAddr );
	// four named slots, fully unrolled: an indexed uvec4 s[4] array landed in LOCAL memory (the gather's pipeline
	// statistics showed 64 B of it - r_vkPipelineStats, 2026-10-16); named registers do not
	uvec4 s0 = span.s[ k.base ], s1 = span.s[ k.base + 1u ], s2 = span.s[ k.base + 2u ], s3 = span.s[ k.base + 3u ];
	if ( s0.x == k.key ) { return int( k.base ); }						// existing (a key sits in a span at most once)
	if ( s1.x == k.key ) { return int( k.base ) + 1; }
	if ( s2.x == k.key ) { return int( k.base ) + 2; }
	if ( s3.x == k.key ) { return int( k.base ) + 3; }
	uint oldestIdx = 0xFFFFFFFFu;
	uint oldestAge = 0u;
	uint oldestKey = 0u;
	// per slot: empty -> CAS (won = fresh; a sibling won = ours; another cell won = weigh it, unstamped); else weigh
	// the foreign cell for eviction by its stamp
#define RTGI_CACHE_CLAIM_SLOT( S, I ) \
	{ \
		uint idx = k.base + uint( I ); \
		uint sk = S.x; \
		uint stamp = S.y; \
		if ( sk == 0u ) { \
			uint prev = atomicCompSwap( words.w[ idx * RTGI_CACHE_STRIDE ], 0u, k.key ); \
			if ( prev == 0u ) { fresh = true; return int( idx ); } \
			if ( prev == k.key ) { return int( idx ); } \
			sk = prev; \
			stamp = 0u; \
		} \
		uint age = ( stamp == 0u ) ? 0u : ( p.frame - stamp ); \
		if ( age > oldestAge ) { oldestAge = age; oldestIdx = idx; oldestKey = sk; } \
	}
	RTGI_CACHE_CLAIM_SLOT( s0, 0 )
	RTGI_CACHE_CLAIM_SLOT( s1, 1 )
	RTGI_CACHE_CLAIM_SLOT( s2, 2 )
	RTGI_CACHE_CLAIM_SLOT( s3, 3 )
#undef RTGI_CACHE_CLAIM_SLOT
	if ( p.evictAge != 0u && oldestIdx != 0xFFFFFFFFu && oldestAge >= p.evictAge ) {
		uint prev = atomicCompSwap( words.w[ oldestIdx * RTGI_CACHE_STRIDE ], oldestKey, k.key );
		if ( prev == oldestKey ) { evicted = true; return int( oldestIdx ); }
		if ( prev == k.key ) { return int( oldestIdx ); }				// a sibling pixel evicted it for us this frame
	}
	return -1;
}

// LOOKUP at a bounce hit (world point P, geometric normal N facing the ray, eye E): the cell's learned indirect term.
// H's own level first; when that cell is absent or not yet resolved, the one-coarser level's cell (a hit lands at a
// distance its surface may never have been a primary point at, and the coarser cell - twice the width - has had twice
// the chance to be learned). BOTH spans are loaded together: eight independent 16 B loads, one round trip, whether the
// own level hits or not (a miss is the common case at H). 0 when neither exists.
vec3 rtgiCacheLookup( RtgiCacheParms p, vec3 P, vec3 N, vec3 eye ) {
	int   level = hgLevel( distance( P, eye ), p.tanAlphaHalf );
	HgKey k0 = hgMakeKeyLevel( P, N, level, p.slotMask );
	HgKey k1 = hgMakeKeyLevel( P, N, level + 1, p.slotMask );
	RtgiCacheSpan span = RtgiCacheSpan( p.slotAddr );
	uvec4 a0 = span.s[ k0.base ], a1 = span.s[ k0.base + 1u ], a2 = span.s[ k0.base + 2u ], a3 = span.s[ k0.base + 3u ];
	uvec4 b0 = span.s[ k1.base ], b1 = span.s[ k1.base + 1u ], b2 = span.s[ k1.base + 2u ], b3 = span.s[ k1.base + 3u ];
	uvec4 hit;
	if ( rtgiCacheSpanFind( a0, a1, a2, a3, k0.key, hit ) >= 0 && rtgiCacheNv( hit ) > 0u ) { return rtgiCacheIrrv( hit ); }
	if ( rtgiCacheSpanFind( b0, b1, b2, b3, k1.key, hit ) >= 0 && rtgiCacheNv( hit ) > 0u ) { return rtgiCacheIrrv( hit ); }
	return vec3( 0.0 );
}

// LOOKUP in TAIL mode (C6): the cell's stored OUTGOING radiance L_out = albedo*(E_dir+E_ind), emissive excluded. Returns
// true + L_out when H's cell (own level, else the one-coarser fallback) is SERVABLE (converged + fresh, rtgiCacheServable);
// the bounce then takes L_out * scale + emissive(H) and skips the albedo sample, the light loop and the dominant shadow
// ray. false = no servable cell either level: the caller shades the hit live (today's one bounce), no cached addition.
// BOTH spans load together (eight independent 16 B loads, one round trip), as the indirect-only lookup above.
bool rtgiCacheLookupTail( RtgiCacheParms p, vec3 P, vec3 N, vec3 eye, out vec3 Lout ) {
	Lout = vec3( 0.0 );
	int   level = hgLevel( distance( P, eye ), p.tanAlphaHalf );
	HgKey k0 = hgMakeKeyLevel( P, N, level, p.slotMask );
	HgKey k1 = hgMakeKeyLevel( P, N, level + 1, p.slotMask );
	RtgiCacheSpan span = RtgiCacheSpan( p.slotAddr );
	uvec4 a0 = span.s[ k0.base ], a1 = span.s[ k0.base + 1u ], a2 = span.s[ k0.base + 2u ], a3 = span.s[ k0.base + 3u ];
	uvec4 b0 = span.s[ k1.base ], b1 = span.s[ k1.base + 1u ], b2 = span.s[ k1.base + 2u ], b3 = span.s[ k1.base + 3u ];
	uvec4 hit;
	if ( rtgiCacheSpanFind( a0, a1, a2, a3, k0.key, hit ) >= 0 && rtgiCacheServable( p, hit ) ) { Lout = rtgiCacheIrrv( hit ); return true; }
	if ( rtgiCacheSpanFind( b0, b1, b2, b3, k1.key, hit ) >= 0 && rtgiCacheServable( p, hit ) ) { Lout = rtgiCacheIrrv( hit ); return true; }
	return false;
}

// SEED a just-claimed cell from its one-coarser parent when that has been learned. As the camera approaches a surface
// its cells are re-cut at the finer level, and without this every ring of re-cut cells restarted from a single frame's
// mean (the "swimming" bands in the history view while moving). The parent's value is the best estimate there is; it
// enters with SEED_N frames of trust so the fine cell's own samples take over smoothly. One writer (the claim winner);
// siblings that look the cell up before the write lands read N = 0, which is just a miss for that frame.
void rtgiCacheSeed( RtgiCacheParms p, int idx, vec3 P, vec3 N, int level ) {
	HgKey kc = hgMakeKeyLevel( P, N, level + 1, p.slotMask );
	RtgiCacheSpan span = RtgiCacheSpan( p.slotAddr );
	uvec4 c0 = span.s[ kc.base ], c1 = span.s[ kc.base + 1u ], c2 = span.s[ kc.base + 2u ], c3 = span.s[ kc.base + 3u ];
	uvec4 hit;
	if ( rtgiCacheSpanFind( c0, c1, c2, c3, kc.key, hit ) < 0 || rtgiCacheNv( hit ) == 0u ) { return; }
	HgSlot sn = HgSlot( 0u, 0u, 0u, 0u );
	rtgiCachePackIrr( sn, rtgiCacheIrrv( hit ), min( rtgiCacheNv( hit ), RTGI_CACHE_SEED_N ), RTGI_CACHE_SEEDED_BIT );
	RtgiCacheSlots slots = RtgiCacheSlots( p.slotAddr );
	slots.s[ idx ].irrRG = sn.irrRG;
	slots.s[ idx ].irrBN = sn.irrBN;
}

// UPDATE at a primary point (world P, normal N, eye E) with this pixel's bounce estimate Li (0 for a miss: open
// directions ARE part of the cell's cosine-lobe average). Claims the cell's slot (first use / eviction), stamps it
// and adds the clamped sample to the accumulation entry; the resolve folds it into the cell at the end of the pass.
// The caller keeps MOVERS and the view weapon out (rtgiCacheIsMover): a moving mesh sweeps new world positions every
// frame and would mint a thousand garbage cells a frame, filling the cache in seconds; movers still LOOK UP.
void rtgiCacheUpdate( RtgiCacheParms p, vec3 P, vec3 N, vec3 eye, vec3 Li ) {
	int   level = hgLevel( distance( P, eye ), p.tanAlphaHalf );
	HgKey k     = hgMakeKeyLevel( P, N, level, p.slotMask );
	bool evicted, fresh;
	int idx = rtgiCacheClaim( p, k, evicted, fresh );
	if ( idx < 0 ) { return; }											// span full of live cells: drop this sample
	RtgiCacheSlots  slots = RtgiCacheSlots( p.slotAddr );
	RtgiCacheAccums acc   = RtgiCacheAccums( p.accumAddr );
	slots.s[ idx ].stamp = p.frame;										// every writer of a frame stores the same value
	if ( evicted ) {
		// the slot's value belongs to the cell that lived here before: zero it so a lookup this frame does not read the
		// old cell as ours, seed from our parent if it exists, and tell the resolve the slot changed hands - this frame's
		// sums mix the old cell's samples with ours and are dropped; the cell starts from the payload left here
		slots.s[ idx ].irrRG = 0u;
		slots.s[ idx ].irrBN = 0u;
		rtgiCacheSeed( p, idx, P, N, level );
		atomicOr( acc.a[ idx ].count, RTGI_CACHE_EVICT_BIT );
	} else if ( fresh ) {
		rtgiCacheSeed( p, idx, P, N, level );
	}
	vec3  v  = max( Li, vec3( 0.0 ) );
	float pk = max( max( v.r, v.g ), v.b );
	if ( pk > p.sampleClamp ) { v *= p.sampleClamp / pk; }				// a 1-spp firefly must not own the cell for Nmax frames
	uvec3 fx = uvec3( v * RTGI_CACHE_FIXED + 0.5 );
	atomicAdd( acc.a[ idx ].r, fx.x );
	atomicAdd( acc.a[ idx ].g, fx.y );
	atomicAdd( acc.a[ idx ].b, fx.z );
	atomicAdd( acc.a[ idx ].count, 1u );
}

// Is the primary pixel at uv (internal-res [0,1]) on a MOVER? Compare where the per-object velocity says the point
// was last frame with where the CAMERA's own motion alone would have put it (last frame's view matrix, this view's
// projection): a static surface agrees within the jitter, a walking NPC / door / platform disagrees by many pixels.
// prevVel = the velocity texel (currUV - prevUV, +Y-up, view-Y sign already applied by the caller).
bool rtgiCacheIsMover( vec2 uv, vec2 vel, vec3 P, mat4 prevWorldToView, vec2 invProj, float viewYSign, vec2 pixelSize ) {
	vec3  pv    = ( prevWorldToView * vec4( P, 1.0 ) ).xyz;
	float dprev = max( -pv.z, 1e-3 );
	vec2  prevNdcCam = vec2( pv.x / ( dprev * invProj.x ), pv.y * viewYSign / ( dprev * invProj.y ) );
	vec2  prevUvCam  = prevNdcCam * 0.5 + 0.5;
	vec2  dpx = ( ( uv - vel ) - prevUvCam ) / pixelSize;				// disagreement in internal-res pixels
	return dot( dpx, dpx ) > 9.0;										// > 3 px beyond the camera's motion (the jitter is < 1)
}

// RESET request (C3, the R1 gradient): the direct light at P changed this frame, so P's cell restarts its history at
// the next resolve (N = 0: the next sample replaces the stale mean instead of being 1/N of it). Only an existing cell
// can be reset; an absent one has nothing stale to forget.
void rtgiCacheRequestReset( RtgiCacheParms p, vec3 P, vec3 N, vec3 eye ) {
	HgKey k = hgMakeKey( P, N, eye, p.tanAlphaHalf, p.slotMask );
	RtgiCacheSpan span = RtgiCacheSpan( p.slotAddr );
	uvec4 s0 = span.s[ k.base ], s1 = span.s[ k.base + 1u ], s2 = span.s[ k.base + 2u ], s3 = span.s[ k.base + 3u ];
	uvec4 hit;
	int i = rtgiCacheSpanFind( s0, s1, s2, s3, k.key, hit );
	if ( i < 0 ) { return; }
	atomicOr( RtgiCacheAccums( p.accumAddr ).a[ int( k.base ) + i ].count, RTGI_CACHE_RESET_BIT );
}
