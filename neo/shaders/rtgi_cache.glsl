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
const uint  RTGI_CACHE_RESET_BIT  = 0x80000000u;	// HgAccum.count: restart this cell at the resolve
const uint  RTGI_CACHE_COUNT_MASK = 0x7FFFFFFFu;
const uint  RTGI_CACHE_STRIDE     = 4u;				// uints per slot (HgWords view)
const uint  RTGI_CACHE_STAMP_OFS  = 1u;				// HgSlot.stamp

// Per-pass constants, packed by the CPU (RhiWorld.cpp RB_RHI_RtgiCachePack) identically for every consumer:
//   u_channelMask  = ( accum address lo, hi [bit-cast], slot mask [bit-cast uint], tanAlphaHalf )
//   u_skinParms    = ( scale, frame stamp [bit-cast uint], flags [bit-cast uint: 1 update, 2 lookup, 4 gradient reset],
//                      gradient reset threshold (relative luminance change) )
//   u_contactParms = ( evict age in frames (0 = never), per-sample peak clamp, albedo clamp at the hit, spare )
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
	bool  update;
	bool  lookup;
	bool  gradientReset;
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
	p.resetThresh  = u_skinParms.w;
	p.evictAge     = uint( max( u_contactParms.x, 0.0 ) + 0.5 );
	p.sampleClamp  = u_contactParms.y;
	p.albedoClamp  = u_contactParms.z;
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
	return s.irrBN >> 16u;
}

void rtgiCachePackIrr( inout HgSlot s, vec3 irr, uint N ) {
	s.irrRG = packHalf2x16( irr.rg );
	s.irrBN = ( packHalf2x16( vec2( irr.b, 0.0 ) ) & 0xFFFFu ) | ( min( N, 65535u ) << 16u );
}

// LOOKUP at a bounce hit (world point P, geometric normal N facing the ray, eye E): the cell's learned indirect term,
// 0 when the cell is absent or claimed but not yet resolved. 1-4 coalesced 16 B reads.
vec3 rtgiCacheLookup( RtgiCacheParms p, vec3 P, vec3 N, vec3 eye ) {
	HgKey k = hgMakeKey( P, N, eye, p.tanAlphaHalf, p.slotMask );
	int idx = hgFind( HgWords( p.slotAddr ), RTGI_CACHE_STRIDE, k );
	if ( idx < 0 ) { return vec3( 0.0 ); }
	RtgiCacheSlots slots = RtgiCacheSlots( p.slotAddr );
	HgSlot s = slots.s[ idx ];
	if ( rtgiCacheN( s ) == 0u ) { return vec3( 0.0 ); }
	return rtgiCacheUnpackIrr( s );
}

// UPDATE at a primary point (world P, normal N, eye E) with this pixel's bounce estimate Li (0 for a miss: open
// directions ARE part of the cell's cosine-lobe average). Claims the cell's slot (first use / eviction), stamps it
// and adds the clamped sample to the accumulation entry; the resolve folds it into the cell at the end of the pass.
void rtgiCacheUpdate( RtgiCacheParms p, vec3 P, vec3 N, vec3 eye, vec3 Li ) {
	HgKey k = hgMakeKey( P, N, eye, p.tanAlphaHalf, p.slotMask );
	bool evicted;
	int idx = hgClaim( HgWords( p.slotAddr ), RTGI_CACHE_STRIDE, RTGI_CACHE_STAMP_OFS, k, p.frame, p.evictAge, evicted );
	if ( idx < 0 ) { return; }											// span full of live cells: drop this sample
	RtgiCacheSlots  slots = RtgiCacheSlots( p.slotAddr );
	RtgiCacheAccums acc   = RtgiCacheAccums( p.accumAddr );
	slots.s[ idx ].stamp = p.frame;										// every writer of a frame stores the same value
	if ( evicted ) {
		// the slot's value belongs to the cell that lived here before: zero it so a lookup this frame does not read
		// the old cell as ours, and ask the resolve to restart the history from this frame's sums alone
		slots.s[ idx ].irrRG = 0u;
		slots.s[ idx ].irrBN = 0u;
		atomicOr( acc.a[ idx ].count, RTGI_CACHE_RESET_BIT );
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

// RESET request (C3, the R1 gradient): the direct light at P changed this frame, so P's cell restarts its history at
// the next resolve (N = 0: the next sample replaces the stale mean instead of being 1/N of it). Only an existing cell
// can be reset; an absent one has nothing stale to forget.
void rtgiCacheRequestReset( RtgiCacheParms p, vec3 P, vec3 N, vec3 eye ) {
	HgKey k = hgMakeKey( P, N, eye, p.tanAlphaHalf, p.slotMask );
	int idx = hgFind( HgWords( p.slotAddr ), RTGI_CACHE_STRIDE, k );
	if ( idx < 0 ) { return; }
	atomicOr( RtgiCacheAccums( p.accumAddr ).a[ idx ].count, RTGI_CACHE_RESET_BIT );
}
