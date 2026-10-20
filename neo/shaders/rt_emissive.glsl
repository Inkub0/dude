// DUDE RT EMISSIVE TRIANGLE TABLE (docs/rtx-emissive-triangles.md E1): the GPU side of the table tr_main.cpp
// R_RtEmissiveBuild writes once per map - every emissive (additive-glow-stage) triangle of the world and the static
// props, including the pure light-source surfaces the caster set excludes, with the flux + geometry an area-light
// sampler needs. E2 draws a triangle proportional to flux (rtEmissiveSample), a uniform point on it, reads the glow
// texture at that point's st, and traces one visibility ray against the ordinary TLAS. Shared by the consumers and the
// debug overlay so the layout never drifts.
//
// Buffer layout (device address, std430): header (16 B: uint count, float totalFlux, uint cdfOffsetBytes, float
// meanFlux), then count x RtEmissiveTri (64 B each), then count floats of CDF (unnormalised running flux sum).
//
// Needs buffer_reference (the VK prelude enables it) + GL_EXT_buffer_reference_uvec2 in the including shader.

struct RtEmissiveTri {
	vec3  p0;	float flux;			// world-space vertices; flux = area x glow luminance x stage colour x scale
	vec3  p1;	uint  emit;			// bits 0-15 the glow texture's bindless slot + 1 (0 = none), bits 16-31 RT_EMIT_* flags
	vec3  p2;	float scale;		// material intensity scale (r_rtEmissiveScale x the D2 table)
	uint  st0, st1, st2;			// per-vertex st, half2 (unpackHalf2x16)
	uint  tint;						// the stage's constant colour, RGBA8 (unpackUnorm4x8); white when RT_EMIT_ANIMATED
};

const uint RT_EMIT_ANIMATED = 1u << 16;		// stage colour is register-driven: the flux is a snapshot, read the live colour
const uint RT_EMIT_ENTITY   = 1u << 17;		// from a static entity model (else worldspawn)
const uint RT_EMIT_TWOSIDED = 1u << 18;		// the material creates back sides: its reversed twin is in the table too
const uint RT_EMIT_INLIGHT  = 1u << 19;		// sits inside a placed light's volume (E5 census: that light is "fixtured")

layout(buffer_reference, std430, buffer_reference_align = 16) buffer RtEmissiveHeader { uint count; float totalFlux; uint cdfOffset; float meanFlux; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer RtEmissiveTris   { RtEmissiveTri t[]; };
layout(buffer_reference, std430, buffer_reference_align = 4)  buffer RtEmissiveCdf    { float c[]; };

const uint RT_EMISSIVE_HEADER_BYTES = 16u;

// The table at device address `addr` (uvec2 lo/hi); present() = a table is bound this frame.
struct RtEmissiveTable {
	uvec2 addr;
	uint  count;
	float totalFlux;
	uint  cdfOffset;
};

bool rtEmissivePresent( RtEmissiveTable t ) { return ( t.addr.x | t.addr.y ) != 0u && t.count > 0u; }

// Decode the header once per invocation (the address alone identifies the table).
RtEmissiveTable rtEmissiveOpen( uvec2 addr ) {
	RtEmissiveTable t;
	t.addr = addr;
	t.count = 0u; t.totalFlux = 0.0; t.cdfOffset = 0u;
	if ( ( addr.x | addr.y ) != 0u ) {
		RtEmissiveHeader h = RtEmissiveHeader( addr );
		t.count = h.count;
		t.totalFlux = h.totalFlux;
		t.cdfOffset = h.cdfOffset;
	}
	return t;
}

// 64-bit address + byte offset (carry out of the low word)
uvec2 rtEmissiveOffsetAddr( uvec2 addr, uint bytes ) {
	uint lo = addr.x + bytes;
	return uvec2( lo, addr.y + ( ( lo < addr.x ) ? 1u : 0u ) );
}

RtEmissiveTri rtEmissiveFetch( RtEmissiveTable t, uint i ) {
	RtEmissiveTris tris = RtEmissiveTris( rtEmissiveOffsetAddr( t.addr, RT_EMISSIVE_HEADER_BYTES ) );
	return tris.t[ i ];
}

// Draw a triangle index proportional to flux: u in [0,1) -> binary search of the CDF. Returns the index and the
// triangle's selection probability (flux / totalFlux) in pdf.
uint rtEmissiveSample( RtEmissiveTable t, float u, out float pdf ) {
	RtEmissiveCdf cdf = RtEmissiveCdf( rtEmissiveOffsetAddr( t.addr, t.cdfOffset ) );
	float target = u * t.totalFlux;
	uint lo = 0u, hi = t.count - 1u;
	while ( lo < hi ) {
		uint mid = ( lo + hi ) >> 1u;
		if ( cdf.c[ mid ] <= target ) { lo = mid + 1u; } else { hi = mid; }
	}
	float prev = ( lo > 0u ) ? cdf.c[ lo - 1u ] : 0.0;
	pdf = max( cdf.c[ lo ] - prev, 0.0 ) / max( t.totalFlux, 1e-6 );
	return lo;
}

// Uniform point on a triangle (Osada et al.), its geometric normal (unnormalised cross = 2 x area) and the
// barycentrics for the st interpolation.
vec3 rtEmissiveUniformPoint( RtEmissiveTri e, vec2 xi, out vec3 nUnnorm, out vec3 bary ) {
	float su = sqrt( xi.x );
	bary = vec3( 1.0 - su, su * ( 1.0 - xi.y ), su * xi.y );
	nUnnorm = cross( e.p1 - e.p0, e.p2 - e.p0 );
	return bary.x * e.p0 + bary.y * e.p1 + bary.z * e.p2;
}

vec2 rtEmissiveST( RtEmissiveTri e, vec3 bary ) {
	return bary.x * unpackHalf2x16( e.st0 ) + bary.y * unpackHalf2x16( e.st1 ) + bary.z * unpackHalf2x16( e.st2 );
}
