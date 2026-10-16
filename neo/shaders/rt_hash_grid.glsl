// DUDE world-space ADAPTIVE HASH GRID (docs/rtx-irradiance-cache.md C1; the same helper serves the Markov-chain
// path guiding grids of docs/rtx-mcpg-guiding.md - G0 - so it is written ONCE here and stays payload-agnostic).
//
// A camera-adaptive spatial hash in the spirit of NVIDIA SHARC and merian's hash-grid.slang: a world point P with
// normal N seen from eye E hashes to a cell of width 2 * tanAlphaHalf * |P - E| (a fixed angular footprint on
// screen, so near surfaces get fine cells and far ones coarse), quantized to HG_STEPS_PER_OCTAVE levels per
// distance doubling, with a half-cell STAGGER on odd levels and a normal BIN (cube face, the cube rotated 45
// degrees on odd levels) so opposite faces of a thin wall never share a cell and neighbouring levels never share
// boundaries. The (cell, level, bin) triple is hashed to a 32-bit slot-array index whose probe span of
// HG_PROBE_SLOTS consecutive slots is aligned to the span (one 64 B line for 16 B slots: a warp's probes coalesce)
// and to a 32-bit COLLISION KEY stored in the slot (0 = empty), so two triples hashing to the same span are told
// apart and a mismatch is a miss, never a false hit (short of a 32-bit key collision).
//
// The grid only ever needs two words of a slot: its KEY (first uint) and its STAMP (frame of last update, at a
// caller-given uint offset); the payload between/after them is the cache's own (16 B irradiance slots in
// rtgi_cache.glsl, 40 B vMF states in the guiding grid). Slots are addressed as a flat uint array with `stride`
// uints per slot through HgWords, so one implementation probes every layout.
//
// STAGE-AGNOSTIC: no uniforms, no samplers, no gl_FragCoord - fragment and compute alike. Needs buffer_reference
// (the VK prelude enables GL_EXT_buffer_reference); the including shader constructs the references from uvec2
// device addresses (GL_EXT_buffer_reference_uvec2).

const float HG_STEPS_PER_OCTAVE = 2.0;		// levels per doubling of the eye distance (merian GRID_POWER 2)
const uint  HG_PROBE_SLOTS      = 4u;		// linear probing span; the start index is aligned to it

// ---- level / cell / bin -------------------------------------------------------------------------------------

// Level from the eye distance: the ideal cell width is w = 2 * tanAlphaHalf * dist (an angle alpha on screen);
// level = floor( log2( w ) * steps ). Negative for sub-unit cells near the eye - hashed as an integer like any
// other. The width every point of a level shares is recomputed FROM the level (hgCellWidth), never from the point.
int hgLevel( float dist, float tanAlphaHalf ) {
	float w = max( dist, 1.0 ) * ( 2.0 * tanAlphaHalf );
	return int( floor( log2( max( w, 1e-4 ) ) * HG_STEPS_PER_OCTAVE ) );
}

float hgCellWidth( int level ) {
	return exp2( float( level ) / HG_STEPS_PER_OCTAVE );
}

// Integer cell of p at a level: odd levels are shifted by half a cell on every axis (Instant-NGP style stagger), so
// a point on a cell boundary at one level sits well inside a cell at the next.
ivec3 hgCell( vec3 p, int level, float width ) {
	vec3 off = ( ( level & 1 ) != 0 ) ? vec3( 0.5 * width ) : vec3( 0.0 );
	return ivec3( floor( ( p + off ) / width ) );
}

// Normal bin: the cube face the unit normal points at (0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z). On odd levels the cube
// is rotated 22.5 degrees about Z and then about X, so bin boundaries (like cell boundaries) never coincide between
// neighbouring levels - a normal that flickers between two faces at one level is centred in a face at the next.
// 22.5 and NOT 45 degrees: Doom's world is axis-aligned, and a 45-degree turn lands every wall / floor normal EXACTLY
// on a rotated face edge, where the bump-mapped texels of one cell fall 50/50 on either side at random (a salted
// cell view, half the samples per cell). At 22.5 degrees every axis normal keeps ~20 degrees of margin to any face
// edge at both parities.
uint hgNormalBin( vec3 n, int level ) {
	if ( ( level & 1 ) != 0 ) {
		const float c = 0.92387953, s = 0.38268343;
		n = vec3( c * n.x - s * n.y, s * n.x + c * n.y, n.z );
		n = vec3( n.x, c * n.y - s * n.z, s * n.y + c * n.z );
	}
	vec3 a = abs( n );
	if ( a.x >= a.y && a.x >= a.z ) { return n.x >= 0.0 ? 0u : 1u; }
	if ( a.y >= a.z ) { return n.y >= 0.0 ? 2u : 3u; }
	return n.z >= 0.0 ? 4u : 5u;
}

// ---- hashing -------------------------------------------------------------------------------------------------

// PCG output permutation (the same rounds restir_di.frag's rnd() uses): a full-avalanche 32-bit mix.
uint hgPcg( uint v ) {
	uint s = v * 747796405u + 2891336453u;
	uint w = ( ( s >> ( ( s >> 28u ) + 4u ) ) ^ s ) * 277803737u;
	return ( w >> 22u ) ^ w;
}

// 32-bit hash of the (cell, level, bin) triple: each coordinate folded in through a PCG round.
uint hgHash( ivec3 cell, int level, uint bin ) {
	uint h = hgPcg( uint( cell.x ) + 0x9E3779B9u );
	h = hgPcg( h ^ uint( cell.y ) );
	h = hgPcg( h ^ uint( cell.z ) );
	h = hgPcg( h ^ ( uint( level ) * 6u + bin ) );
	return h;
}

// Everything a probe needs about a point: the collision key (never 0 - 0 marks an empty slot), the aligned probe
// start index (slotMask = slot count - 1, a power of two) and the level the point hashed at (callers wanting the
// cell width). The key is a SECOND mix of the triple hash so it is not a function of the slot index.
struct HgKey {
	uint key;
	uint base;
	int  level;
};

// The key of p / n at an EXPLICIT level (callers that walk neighbouring levels: the coarser-level fallback of a lookup,
// the seeding of a freshly cut cell from its coarser parent).
HgKey hgMakeKeyLevel( vec3 p, vec3 n, int level, uint slotMask ) {
	HgKey k;
	k.level = level;
	float width = hgCellWidth( level );
	uint  h = hgHash( hgCell( p, level, width ), level, hgNormalBin( n, level ) );
	k.key  = hgPcg( h ^ 0x68E31DA4u );
	if ( k.key == 0u ) { k.key = 1u; }
	k.base = ( h & slotMask ) & ~( HG_PROBE_SLOTS - 1u );
	return k;
}

HgKey hgMakeKey( vec3 p, vec3 n, vec3 eye, float tanAlphaHalf, uint slotMask ) {
	return hgMakeKeyLevel( p, n, hgLevel( distance( p, eye ), tanAlphaHalf ), slotMask );
}

// A stable visualisation colour for a key (debug views: every cell its own tint), channels in [0.25, 1].
vec3 hgKeyColor( uint key ) {
	uint h = hgPcg( key );
	return vec3( float( h & 255u ), float( ( h >> 8u ) & 255u ), float( ( h >> 16u ) & 255u ) ) * ( 0.75 / 255.0 ) + 0.25;
}

// ---- probing over any slot layout ----------------------------------------------------------------------------

// The slot array seen as flat uints: slot i's key is w[ i * stride ], its stamp w[ i * stride + stampOfs ].
layout(buffer_reference, std430, buffer_reference_align = 16) buffer HgWords { uint w[]; };

// The slot holding k.key within its probe span, or -1. Slots fill from the first empty one of a span and are never
// emptied again short of a full clear (eviction REPLACES a key), so an empty slot ends the search.
int hgFind( HgWords words, uint stride, HgKey k ) {
	for ( uint i = 0u; i < HG_PROBE_SLOTS; i++ ) {
		uint idx = k.base + i;
		uint sk  = words.w[ idx * stride ];
		if ( sk == k.key ) { return int( idx ); }
		if ( sk == 0u ) { return -1; }
	}
	return -1;
}

// Claim a slot for k.key: the slot that already holds it; else the first EMPTY slot of the span, taken with an
// atomicCompSwap so every pixel of a new cell in the same frame lands in ONE slot (a lost race re-checks the winner's
// key); else - when evictAge > 0 - the span's oldest FOREIGN slot provided its stamp is at least evictAge frames old
// (stamp 0 = claimed this frame by a writer that has not stamped yet: never evicted), taken over with a CAS on its
// key. `evicted` tells the caller the payload belongs to the old key and must be treated as fresh; `fresh` that THIS
// invocation created the slot (its CAS won on an empty slot - exactly one caller per new cell sees it, the one that may
// seed the payload). -1 = the span is full of live foreign cells: this update is dropped (the cache is a hint, a dropped
// sample is only a little slower convergence).
int hgClaim( HgWords words, uint stride, uint stampOfs, HgKey k, uint frame, uint evictAge, out bool evicted, out bool fresh ) {
	evicted = false;
	fresh = false;
	uint oldestIdx = 0xFFFFFFFFu;
	uint oldestAge = 0u;
	uint oldestKey = 0u;
	for ( uint i = 0u; i < HG_PROBE_SLOTS; i++ ) {
		uint idx = k.base + i;
		uint sk  = words.w[ idx * stride ];
		if ( sk == k.key ) { return int( idx ); }
		if ( sk == 0u ) {
			uint prev = atomicCompSwap( words.w[ idx * stride ], 0u, k.key );
			if ( prev == 0u ) { fresh = true; return int( idx ); }
			if ( prev == k.key ) { return int( idx ); }					// a sibling pixel created it a moment ago
			sk = prev;													// lost the race to another cell: weigh it for eviction
		}
		uint stamp = words.w[ idx * stride + stampOfs ];
		uint age   = ( stamp == 0u ) ? 0u : ( frame - stamp );
		if ( age > oldestAge ) { oldestAge = age; oldestIdx = idx; oldestKey = sk; }
	}
	if ( evictAge != 0u && oldestIdx != 0xFFFFFFFFu && oldestAge >= evictAge ) {
		uint prev = atomicCompSwap( words.w[ oldestIdx * stride ], oldestKey, k.key );
		if ( prev == oldestKey ) { evicted = true; return int( oldestIdx ); }
		if ( prev == k.key ) { return int( oldestIdx ); }				// a sibling pixel evicted it for us this frame
	}
	return -1;
}
