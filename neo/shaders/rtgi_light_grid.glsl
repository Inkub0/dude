// DUDE RT WORLD-SPACE LIGHT GRID (docs/rtx-light-grid.md): a ReGIR-style grid of camera-centred cascades. A once-per-
// frame compute pass (light_grid_fill.comp) fills every cell with `slots` light candidates drawn in proportion to their
// IMPORTANCE at that cell (flux x geometry), plus the cell's total importance; a shading pixel then RIS-samples over
// its CELL's candidates instead of over EVERY scene light - O(slots) not O(nLights). This file is the shared cell<->world
// mapping, the buffer layout and the consume helper, so the fill, the DI/bounce consume and the debug view never drift.
//
// Grid: `cascades` cascades, each `res^3` cells, cascade c cell size = cellBase * 2^c, centred on the camera (snapped to
// the cell size so cells do not swim). A world point picks the smallest cascade whose box contains it, then a cell.
//
// Cell record (flat uint buffer `g[]`, stride LG_HEADER + 2*slots uints): [0] count (lights stored, 0 = empty), [1]
// sumImp (float bits: the cell's total in-range importance, the consume pdf denominator), then per slot { uint light
// index, float imp bits }. A cold / empty cell has count 0 -> the consumer falls back to its old path (never wrong).
//
// Needs buffer_reference (the VK prelude enables it) + GL_EXT_buffer_reference_uvec2 in the including shader.

layout(buffer_reference, std430, buffer_reference_align = 16) buffer LgGrid { uint g[]; };

const uint LG_HEADER = 2u;		// uints before the slot pairs: count, sumImp

// per-cell record stride in uints. E2 v2b: with slotsE > 0 an EMISSIVE record follows the light record - [countE]
// [sumImpE] then slotsE x { uint group index, float imp bits } (same shape; empty when the mode is off: slotsE 0).
uint lgStride( uint slots, uint slotsE ) { return LG_HEADER + 2u * slots + ( ( slotsE > 0u ) ? ( LG_HEADER + 2u * slotsE ) : 0u ); }
int  lgEBase( int base, uint slots ) { return base + int( LG_HEADER + 2u * slots ); }

// cheap PCG (self-contained: the fill seeds its reservoirs, the consume picks a slot)
uint lgPcg( uint v ) { uint s = v * 747796405u + 2891336453u; uint w = ( ( s >> ( ( s >> 28u ) + 4u ) ) ^ s ) * 277803737u; return ( w >> 22u ) ^ w; }
float lgRand( inout uint st ) { st = st * 747796405u + 2891336453u; uint w = ( ( st >> ( ( st >> 28u ) + 4u ) ) ^ st ) * 277803737u; return float( ( w >> 22u ) ^ w ) * ( 1.0 / 4294967296.0 ); }

// --- cell <-> world --------------------------------------------------------------------------------------------------
// The (0,0,0) corner of cascade c: the camera snapped to the cascade's cell size, minus half the grid, so the res^3
// cells straddle the camera. Stable as the camera moves within a cell (snap), a half-cell wobble when it crosses one.
vec3 lgCascadeOrigin( vec3 camPos, float cellBase, int res, int c ) {
	float cs = cellBase * float( 1 << c );
	return floor( camPos / cs ) * cs - cs * float( res / 2 );
}

// The smallest cascade whose box contains p, or -1 (beyond the last). Fills cell[0..res) coords when found.
int lgPickCascade( vec3 p, vec3 camPos, float cellBase, int res, int cascades, out ivec3 cell ) {
	cell = ivec3( 0 );
	for ( int c = 0; c < cascades; c++ ) {
		float cs  = cellBase * float( 1 << c );
		vec3  rel = ( p - lgCascadeOrigin( camPos, cellBase, res, c ) ) / cs;
		if ( all( greaterThanEqual( rel, vec3( 0.0 ) ) ) && all( lessThan( rel, vec3( float( res ) ) ) ) ) {
			cell = ivec3( rel );
			return c;
		}
	}
	return -1;
}

uint lgCellIndex( int c, ivec3 cell, int res ) {
	return uint( c ) * uint( res * res * res ) + ( uint( cell.z ) * uint( res ) + uint( cell.y ) ) * uint( res ) + uint( cell.x );
}

// The world centre of a decoded cell (the fill: cellIndex -> c, cell -> centre).
vec3 lgCellCenter( uint cellIndex, vec3 camPos, float cellBase, int res, out int cOut ) {
	uint per = uint( res * res * res );
	int  c   = int( cellIndex / per );
	uint r   = cellIndex - uint( c ) * per;
	ivec3 cell = ivec3( int( r % uint( res ) ), int( ( r / uint( res ) ) % uint( res ) ), int( r / uint( res * res ) ) );
	float cs = cellBase * float( 1 << c );
	cOut = c;
	return lgCascadeOrigin( camPos, cellBase, res, c ) + ( vec3( cell ) + 0.5 ) * cs;
}

// --- consume (L2/L3): the cell that lights world point p, and a candidate draw --------------------------------------
// Parameters the DI gather / bounce need, packed by the CPU (RhiWorld.cpp RB_RHI_LightGridPack) into spare uniform
// floats; addresses bit-cast. present() = the grid is live this frame.
struct LgParms {
	uvec2 gridAddr;
	vec3  camPos;
	float cellBase;
	int   res;
	int   cascades;
	uint  slots;
	uint  slotsE;		// E2 v2b: emissive group slots per cell (0 = none)
};

bool lgPresent( LgParms p ) { return ( p.gridAddr.x | p.gridAddr.y ) != 0u && p.slots > 0u; }

// Build LgParms from the packed uniform vec4 (RB_RHI_LightGridPack: gridAddr lo/hi, cellBase, res|cascades<<8|slots<<16)
// plus the camera world position (every RT pass has it as u_modelViewMatrix[3].xyz, so it is not packed).
LgParms lgParmsFromVec4( vec4 v, vec3 camPos ) {
	LgParms p;
	p.gridAddr  = uvec2( floatBitsToUint( v.x ), floatBitsToUint( v.y ) );
	p.camPos    = camPos;
	p.cellBase  = v.z;
	uint packed = floatBitsToUint( v.w );
	p.res       = int( packed & 0xFFu );
	p.cascades  = int( ( packed >> 8 ) & 0xFu );
	p.slots     = ( packed >> 16 ) & 0x3Fu;
	p.slotsE    = ( packed >> 24 ) & 0x3Fu;
	return p;
}

// The cell record base (uint offset) for world point p, or -1 (beyond the grid / caller falls back).
int lgCellBase( LgParms p, vec3 world ) {
	ivec3 cell;
	int c = lgPickCascade( world, p.camPos, p.cellBase, p.res, p.cascades, cell );
	if ( c < 0 ) { return -1; }
	return int( lgCellIndex( c, cell, p.res ) * lgStride( p.slots, p.slotsE ) );
}

uint  lgCount( LgGrid grid, int base )            { return grid.g[ base ]; }
float lgSumImp( LgGrid grid, int base )           { return uintBitsToFloat( grid.g[ base + 1 ] ); }
uint  lgSlotLight( LgGrid grid, int base, uint s ){ return grid.g[ base + int( LG_HEADER + 2u * s ) ]; }
float lgSlotImp( LgGrid grid, int base, uint s )  { return uintBitsToFloat( grid.g[ base + int( LG_HEADER + 2u * s + 1u ) ] ); }

// E2 v2b: the cell's emissive record (groups) - same accessors over the E base
uint  lgCountE( LgGrid grid, int eBase )            { return grid.g[ eBase ]; }
float lgSumImpE( LgGrid grid, int eBase )           { return uintBitsToFloat( grid.g[ eBase + 1 ] ); }
uint  lgSlotGroup( LgGrid grid, int eBase, uint s ) { return grid.g[ eBase + int( LG_HEADER + 2u * s ) ]; }
float lgSlotImpE( LgGrid grid, int eBase, uint s )  { return uintBitsToFloat( grid.g[ eBase + int( LG_HEADER + 2u * s + 1u ) ] ); }
// draw one GROUP from the cell in proportion to its stored importance; q = its selection pdf. false = none stored.
bool lgDrawGroup( LgGrid grid, int eBase, inout uint rng, out uint groupIndex, out float q ) {
	groupIndex = 0u; q = 0.0;
	uint n = lgCountE( grid, eBase );
	if ( n == 0u ) { return false; }
	float sum = lgSumImpE( grid, eBase );
	if ( !( sum > 0.0 ) ) { return false; }
	float r   = lgRand( rng ) * sum;
	float acc = 0.0;
	for ( uint s = 0u; s < n; s++ ) {
		float imp = lgSlotImpE( grid, eBase, s );
		acc += imp;
		if ( r < acc || s + 1u == n ) { groupIndex = lgSlotGroup( grid, eBase, s ); q = imp / sum; return q > 0.0; }
	}
	return false;
}

// One candidate from the cell, drawn IMPORTANCE-PROPORTIONALLY among its stored lights (the cell holds the top-N by
// importance; the fill is deterministic, this draw provides the per-pixel variation the denoiser averages). Returns the
// chosen light index and its selection pdf q = imp/sumStored, for the consume's RIS weight pHat/q. false = empty cell.
bool lgDrawCandidate( LgGrid grid, int base, inout uint rng, out uint lightIndex, out float q ) {
	lightIndex = 0u; q = 0.0;
	uint n = lgCount( grid, base );
	if ( n == 0u ) { return false; }
	float sum = lgSumImp( grid, base );
	if ( !( sum > 0.0 ) ) { return false; }
	float r   = lgRand( rng ) * sum;
	float acc = 0.0;
	for ( uint s = 0u; s < n; s++ ) {
		float imp = lgSlotImp( grid, base, s );
		acc += imp;
		if ( r < acc || s + 1u == n ) { lightIndex = lgSlotLight( grid, base, s ); q = imp / sum; return q > 0.0; }
	}
	return false;
}
