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
// HgSlot.irrBN: bits 0-15 irr.b (f16), 16-25 N (10 bits: Nmax <= 1023), then three flag bits the debug activity view
// reads (r_rtgiCacheDebug 5): 29 SEEDED - set by the gather when it seeds a fresh cell from its coarser parent, consumed
// by the next resolve; 30 FRESH - the last resolve created the cell (N 0 -> 1) or was a seeded cell's first; 31 RESET -
// the last resolve restarted the cell on a reset request (the DI gradient, an eviction).
const uint  RTGI_CACHE_N_SHIFT    = 16u;
const uint  RTGI_CACHE_N_MASK     = 0x3FFu;
const uint  RTGI_CACHE_N_MAX      = 1023u;
const uint  RTGI_CACHE_SEEDED_BIT = 0x20000000u;
const uint  RTGI_CACHE_FRESH_BIT  = 0x40000000u;
const uint  RTGI_CACHE_RESET_FLAG = 0x80000000u;
const uint  RTGI_CACHE_SEED_N     = 4u;				// frames of trust a cell seeded from its coarser parent starts with

// Per-pass constants, packed by the CPU (RhiWorld.cpp RB_RHI_RtgiCachePack) identically for every consumer:
//   u_channelMask  = ( accum address lo, hi [bit-cast], slot mask [bit-cast uint], tanAlphaHalf )
//   u_skinParms    = ( scale, frame stamp [bit-cast uint], flags [bit-cast uint: 1 update, 2 lookup, 4 gradient reset,
//                      8 velocity at unit 10 valid: movers never teach], gradient reset threshold (relative luminance change) )
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

void rtgiCachePackIrr( inout HgSlot s, vec3 irr, uint N, uint flags ) {
	s.irrRG = packHalf2x16( irr.rg );
	s.irrBN = ( packHalf2x16( vec2( irr.b, 0.0 ) ) & 0xFFFFu ) | ( min( N, RTGI_CACHE_N_MAX ) << RTGI_CACHE_N_SHIFT ) | flags;
}

// LOOKUP at a bounce hit (world point P, geometric normal N facing the ray, eye E): the cell's learned indirect term.
// H's own level first; when that cell is absent or not yet resolved, the one-coarser level's cell (a hit lands at a
// distance its surface may never have been a primary point at, and the coarser cell - twice the width - has had twice
// the chance to be learned). One extra probe on a miss, none on a hit; 0 when neither exists.
vec3 rtgiCacheLookup( RtgiCacheParms p, vec3 P, vec3 N, vec3 eye ) {
	HgWords        words = HgWords( p.slotAddr );
	RtgiCacheSlots slots = RtgiCacheSlots( p.slotAddr );
	int level = hgLevel( distance( P, eye ), p.tanAlphaHalf );
	for ( int step = 0; step < 2; step++ ) {
		HgKey k = hgMakeKeyLevel( P, N, level + step, p.slotMask );
		int idx = hgFind( words, RTGI_CACHE_STRIDE, k );
		if ( idx >= 0 ) {
			HgSlot s = slots.s[ idx ];
			if ( rtgiCacheN( s ) > 0u ) { return rtgiCacheUnpackIrr( s ); }
		}
	}
	return vec3( 0.0 );
}

// SEED a just-claimed cell from its one-coarser parent when that has been learned. As the camera approaches a surface
// its cells are re-cut at the finer level, and without this every ring of re-cut cells restarted from a single frame's
// mean (the "swimming" bands in the history view while moving). The parent's value is the best estimate there is; it
// enters with SEED_N frames of trust so the fine cell's own samples take over smoothly. One writer (the claim winner);
// siblings that look the cell up before the write lands read N = 0, which is just a miss for that frame.
void rtgiCacheSeed( RtgiCacheSlots slots, HgWords words, uint slotMask, int idx, vec3 P, vec3 N, int level ) {
	HgKey kc = hgMakeKeyLevel( P, N, level + 1, slotMask );
	int ic = hgFind( words, RTGI_CACHE_STRIDE, kc );
	if ( ic < 0 ) { return; }
	HgSlot sc = slots.s[ ic ];
	if ( rtgiCacheN( sc ) == 0u ) { return; }
	HgSlot sn = slots.s[ idx ];
	rtgiCachePackIrr( sn, rtgiCacheUnpackIrr( sc ), min( rtgiCacheN( sc ), RTGI_CACHE_SEED_N ), RTGI_CACHE_SEEDED_BIT );
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
	HgWords words = HgWords( p.slotAddr );
	bool evicted, fresh;
	int idx = hgClaim( words, RTGI_CACHE_STRIDE, RTGI_CACHE_STAMP_OFS, k, p.frame, p.evictAge, evicted, fresh );
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
		rtgiCacheSeed( slots, words, p.slotMask, idx, P, N, level );
		atomicOr( acc.a[ idx ].count, RTGI_CACHE_EVICT_BIT );
	} else if ( fresh ) {
		rtgiCacheSeed( slots, words, p.slotMask, idx, P, N, level );
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
	int idx = hgFind( HgWords( p.slotAddr ), RTGI_CACHE_STRIDE, k );
	if ( idx < 0 ) { return; }
	atomicOr( RtgiCacheAccums( p.accumAddr ).a[ idx ].count, RTGI_CACHE_RESET_BIT );
}
