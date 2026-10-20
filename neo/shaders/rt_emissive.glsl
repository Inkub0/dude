// DUDE RT EMISSIVE TRIANGLE TABLE (docs/rtx-emissive-triangles.md E1): the GPU side of the table tr_main.cpp
// R_RtEmissiveBuild writes once per map - every emissive (additive-glow-stage) triangle of the world and the static
// props, including the pure light-source surfaces the caster set excludes, with the flux + geometry an area-light
// sampler needs. E2 draws a triangle proportional to flux (rtEmissiveSample), a uniform point on it, reads the glow
// texture at that point's st, and traces one visibility ray against the ordinary TLAS. Shared by the consumers and the
// debug overlay so the layout never drifts.
//
// Buffer layout (device address, std430): header (16 B: uint count, float totalFlux, uint cdfOffsetBytes, float
// meanFlux), then count x RtEmissiveTri (80 B each), then count floats of CDF (unnormalised running flux sum).
//
// Needs buffer_reference (the VK prelude enables it) + GL_EXT_buffer_reference_uvec2 in the including shader, and
// rt_light_eval.glsl BEFORE it (rtEmissiveShadeRow reads an emitter ROW of the scene-light buffer).

#ifndef RT_EMISSIVE_GLSL
#define RT_EMISSIVE_GLSL

struct RtEmissiveTri {
	vec3  p0;	float flux;			// world-space vertices; flux = area x glow luminance x stage colour x scale
	vec3  p1;	uint  emit;			// bits 0-15 the glow texture's bindless slot + 1 (0 = none), bits 16-31 RT_EMIT_* flags
	vec3  p2;	float scale;		// material intensity scale (r_rtEmissiveScale x the D2 table)
	uint  st0, st1, st2;			// per-vertex st, half2 (unpackHalf2x16)
	uint  tint;						// the stage's constant colour, RGBA8 (unpackUnorm4x8); white when RT_EMIT_ANIMATED; alpha = the DIFFUSEGLOW threshold
	uint  meanRG, meanB;			// the MEAN linear radiance (glow average x stage colour), halves: a uniform emitter lights with this (0 for DIFFUSEGLOW)
	uint  reach;					// float bits: the authored radius of the placed light this emitter replaces (0 = none)
	uint  lightLum;					// float bits: that light's colour luminance (E2 v3 power match, read on the CPU; 0 = none)
};

const uint RT_EMIT_ANIMATED = 1u << 16;		// stage colour is register-driven: the flux is a snapshot, read the live colour
const uint RT_EMIT_ENTITY   = 1u << 17;		// from a static entity model (else worldspawn)
const uint RT_EMIT_TWOSIDED = 1u << 18;		// the material creates back sides: its reversed twin is in the table too
const uint RT_EMIT_INLIGHT  = 1u << 19;		// sits inside a placed light's volume (E5 census: that light is "fixtured")
const uint RT_EMIT_DARK     = 1u << 20;		// the fixture is switched OFF in its current state (skin / parm): zero flux, kept in place
const uint RT_EMIT_DIFFUSEGLOW = 1u << 21;	// the glow slot is the DIFFUSE map: texels brighter than the threshold (tint alpha) emit (flare-inferred)

// The MEAN radiance of a uniform emitter (E2 lights with this: a tile, a tube, an LED panel as a faint flat panel - the
// texel pattern would only add variance to a 1-spp sample; the per-pixel glow seen on the surface itself is the forward draw)
vec3 rtEmissiveMeanRadiance( RtEmissiveTri e ) {
	return vec3( unpackHalf2x16( e.meanRG ), unpackHalf2x16( e.meanB ).x );
}

// The emitted colour of a glow texel: a real glow map as is; a DIFFUSEGLOW entry through its threshold (only the
// bright patch under the flare emits, rescaled to 0..1 above the threshold).
vec3 rtEmissiveTexelToRadiance( RtEmissiveTri e, vec3 texel ) {
	if ( ( e.emit & RT_EMIT_DIFFUSEGLOW ) != 0u ) {
		float th  = unpackUnorm4x8( e.tint ).a;
		float lum = dot( texel, vec3( 0.2126, 0.7152, 0.0722 ) );
		float k   = clamp( ( lum - th ) / max( 1.0 - th, 1e-3 ), 0.0, 1.0 );
		return texel * k;
	}
	return texel * unpackUnorm4x8( e.tint ).rgb;
}

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

// Draw a triangle INSIDE a group's contiguous range [first, first + num) in proportion to flux: the global CDF
// restricted to the range. pdf = the triangle's flux share WITHIN the group.
uint rtEmissiveSampleInGroup( RtEmissiveTable t, uint first, uint num, float u, out float pdf ) {
	RtEmissiveCdf cdf = RtEmissiveCdf( rtEmissiveOffsetAddr( t.addr, t.cdfOffset ) );
	float base  = ( first > 0u ) ? cdf.c[ first - 1u ] : 0.0;
	float last  = cdf.c[ first + num - 1u ];
	float total = last - base;
	pdf = 0.0;
	if ( !( total > 0.0 ) ) { return first; }
	float target = base + u * total;
	uint lo = first, hi = first + num - 1u;
	while ( lo < hi ) {
		uint mid = ( lo + hi ) >> 1u;
		if ( cdf.c[ mid ] <= target ) { lo = mid + 1u; } else { hi = mid; }
	}
	float prev = ( lo > 0u ) ? cdf.c[ lo - 1u ] : 0.0;
	pdf = max( cdf.c[ lo ] - prev, 0.0 ) / total;
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

// Sample the triangle UNIFORMLY IN SOLID ANGLE as seen from P (Arvo 1995, Q2RTX light_lists.h sample_projected_triangle):
// the pdf w.r.t. solid angle is 1 / (spherical area), so a sample's value L x N.L x (1/pdfw) is bounded by the solid
// angle - no 1/d^2 blow-up for a receiver near the emitter. Returns false when the spherical triangle is degenerate
// (P in the triangle's plane, or the triangle too small / far to resolve). nUnit = the triangle's unit normal
// (cross( p1 - p0, p2 - p0 ) - the engine's face normal is the opposite sign).
// ORIENTATION: Arvo's construction (as Q2RTX wrote it) expects the triangle wound CLOCKWISE as seen from P, i.e.
// cross( p1 - p0, p2 - p0 ) pointing AT P (o < 0). Our table's emitters wind the other way when they face the receiver
// (the engine's face normal is cross( p2 - p0, p1 - p0 )), so the raw sampler got a MIRRORED triangle and its
// directions missed the emitter - the sparse salt-and-pepper DI of 2026-10-20. The vertices are re-wound here so the
// routine always sees its convention; nUnit is returned pointing AT P, and `front` says whether the emitter's FACE
// (the side that emits, one-sided) looks at P.
bool rtEmissiveSampleSolidAngle( RtEmissiveTri e, vec3 P, vec2 rnd, out vec3 Pe, out vec3 nUnit, out float pdfw, out bool front ) {
	vec3 n = cross( e.p1 - e.p0, e.p2 - e.p0 );
	float nl = length( n );
	Pe = e.p0; nUnit = vec3( 0.0, 0.0, 1.0 ); pdfw = 0.0; front = false;
	if ( !( nl > 1e-8 ) ) { return false; }
	nUnit = n / nl;
	vec3 v0 = e.p0 - P, v1 = e.p1 - P, v2 = e.p2 - P;
	float o = dot( nUnit, v0 );											// signed distance of the plane
	if ( abs( o ) < 1e-3 ) { return false; }
	// the face normal -n points at P exactly when o > 0: that is the emitting side facing the receiver
	front = o > 0.0;
	if ( o > 0.0 ) {													// re-wind to Arvo's convention: n at P, o < 0
		vec3 t = v1; v1 = v2; v2 = t;
		nUnit = -nUnit;
		o = -o;
	}
	vec3 A = normalize( v0 ), B = normalize( v1 ), C = normalize( v2 );
	vec3 cross_BC = cross( B, C );
	vec3 norm_AB = normalize( cross( A, B ) ), norm_BC = normalize( cross_BC ), norm_CA = normalize( cross( C, A ) );
	float cos_c = dot( A, B );
	float cos_alpha = dot( norm_AB, -norm_CA );
	float area = 2.0 * atan( abs( dot( A, cross_BC ) ), 1.0 + cos_c + dot( B, C ) + dot( A, C ) );
	if ( !( area > 1e-7 ) ) { return false; }
	float new_area = rnd.x * area;
	float sin_alpha = sqrt( max( 1.0 - cos_alpha * cos_alpha, 0.0 ) );
	float sin_new_area = sin( new_area ), cos_new_area = cos( new_area );
	float p = sin_new_area * cos_alpha - cos_new_area * sin_alpha;
	float q = cos_new_area * cos_alpha + sin_new_area * sin_alpha;
	float u = q - cos_alpha;
	float v = p + sin_alpha * cos_c;
	float denom = ( v * p + u * q ) * sin_alpha;
	float cos_b = clamp( ( ( v * q - u * p ) * cos_alpha - v ) / ( abs( denom ) > 1e-8 ? denom : 1e-8 ), -1.0, 1.0 );
	vec3 CA = C - dot( C, A ) * A;
	float cal = length( CA );
	if ( !( cal > 1e-8 ) ) { return false; }
	vec3 new_C = cos_b * A + sqrt( max( 1.0 - cos_b * cos_b, 0.0 ) ) * ( CA / cal );
	float z = 1.0 - rnd.y * ( 1.0 - dot( new_C, B ) );
	vec3 CB = new_C - dot( new_C, B ) * B;
	float cbl = length( CB );
	if ( !( cbl > 1e-8 ) ) { return false; }
	vec3 dir = z * B + sqrt( max( 1.0 - z * z, 0.0 ) ) * ( CB / cbl );
	float dn = dot( nUnit, dir );
	if ( !( abs( dn ) > 1e-6 ) ) { return false; }
	Pe = P + dir * ( o / dn );
	pdfw = 1.0 / area;
	return !( any( isnan( Pe ) ) || isnan( pdfw ) );
}

// The solid angle of the triangle from P (its importance, Q2RTX spherical_tri_area without the Phong share): 0 when
// entirely below the receiver's horizon (n = receiver normal) or facing away (one-sided).
float rtEmissiveSolidAngle( RtEmissiveTri e, vec3 P, vec3 n, bool twoSided ) {
	vec3 v0 = e.p0 - P, v1 = e.p1 - P, v2 = e.p2 - P;
	if ( dot( n, v0 ) <= 0.0 && dot( n, v1 ) <= 0.0 && dot( n, v2 ) <= 0.0 ) { return 0.0; }
	vec3 g = cross( e.p1 - e.p0, e.p2 - e.p0 );							// = -faceNormal: the emitter faces P when dot( g, v ) > 0
	if ( !twoSided && dot( g, v0 ) <= 0.0 && dot( g, v1 ) <= 0.0 && dot( g, v2 ) <= 0.0 ) { return 0.0; }
	vec3 A = normalize( v0 ), B = normalize( v1 ), C = normalize( v2 );
	return max( 2.0 * atan( abs( dot( A, cross( B, C ) ) ), 1.0 + dot( A, B ) + dot( B, C ) + dot( A, C ) ) - 1e-5, 0.0 );
}

vec2 rtEmissiveST( RtEmissiveTri e, vec3 bary ) {
	return bary.x * unpackHalf2x16( e.st0 ) + bary.y * unpackHalf2x16( e.st1 ) + bary.z * unpackHalf2x16( e.st2 );
}

// --- E2 v3: an EMITTER ROW of the scene-light buffer (docs/rtx-experimental-lighting.md "E2 v3") -------------------------
// RhiWorld.cpp RB_RHI_GatherSceneLights appends one DUDE_LIGHT_EMISSIVE row per emissive surface run: pos = centroid,
// radius = REACH, cull = the bounding sphere + 2 x reach, colour = the irradiance proxy the light grid ranks by; the
// projection planes carry projS.w = r_rtEmissiveIntensity, projT.xy = the table address, projQ.x/y = first triangle /
// count (bits), projQ.z = the reach-boost knee (0 = pure inverse square). The row is self-describing: any consumer that
// has the light buffer can shade it with this one function and no further uniforms.

// PCG, self-contained (the fill / grid have their own copies)
float rtEmissiveRand( inout uint st ) { st = st * 747796405u + 2891336453u; uint w = ( ( st >> ( ( st >> 28u ) + 4u ) ) ^ st ) * 277803737u; return float( ( w >> 22u ) ^ w ) * ( 1.0 / 4294967296.0 ); }
// The sample's seed: the row, the receiving POINT and the frame - DETERMINISTIC for (row, point, frame), so the RIS loop's
// target and the winner's re-shade after it draw the SAME sample point (the reservoir weights must match the shade), and
// a new point every frame for NRD to average.
uint rtEmissiveSeed( uint li, vec3 P, float frame ) {
	uvec3 pb = floatBitsToUint( P );
	return ( ( li + 1u ) * 2654435761u ) ^ ( pb.x * 73856093u ) ^ ( pb.y * 19349663u ) ^ ( pb.z * 83492791u ) ^ ( uint( frame ) * 26699u ) | 1u;
}

// The unshadowed irradiance ESTIMATE of emitter row li at surface point P (normal N): one triangle of the run drawn by
// flux (the CDF restricted to the run), one point on it uniform in solid angle (Arvo), value f / p = radiance x emission
// profile x N.L x (solid angle / flux share). Unbiased for the row's total, so the DI reservoir can hold an emitter row
// next to a placed light with pdf 1 exactly like any other candidate. ldir / dl = the SAMPLED point (the visibility ray).
// ldir / dl are the SAMPLED point (the visibility ray); the receiver cosine comes from the authored light origin when there is one.
// Reach (E6): the Doom 3 light this emitter replaced fell off LINEARLY to its radius; a solid-angle estimate falls with the
// inverse square. Past the knee (radius / 4) the sample is boosted by d / knee so the far field goes ~1/d - the reach the
// mapper lit the hall for - and fades out over the last quarter of the reach (the row's cull sphere, a Doom light's extent).
// false = cannot reach (culled / dark / behind / below the horizon). Every stored index is checked against the table's
// count: a row of another table (a stale frame across a rebuild) shades nothing rather than reading off the end.
bool rtEmissiveShadeRow( RtLights lights, uint li, vec3 P, vec3 N, uint seed, out vec3 contrib, out vec3 ldir, out float dl, out float ndl ) {
	contrib = vec3( 0.0 );	ldir = vec3( 0.0, 0.0, 1.0 );	dl = 0.0;	ndl = 0.0;
	uvec2 tAddr     = uvec2( floatBitsToUint( lights.l[ li ].projT.x ), floatBitsToUint( lights.l[ li ].projT.y ) );
	uint  first     = floatBitsToUint( lights.l[ li ].projQ.x );
	uint  num       = floatBitsToUint( lights.l[ li ].projQ.y );
	float knee      = lights.l[ li ].projQ.z;
	float intensity = lights.l[ li ].projS.w;
	float radius    = lights.l[ li ].radius;
	RtEmissiveTable t = rtEmissiveOpen( tAddr );
	if ( !rtEmissivePresent( t ) || num == 0u || first >= t.count || num > t.count - first ) { return false; }
	uint  rng = seed;
	float pIn;
	uint  ti = rtEmissiveSampleInGroup( t, first, num, rtEmissiveRand( rng ), pIn );
	if ( ti < first || ti >= first + num || !( pIn > 0.0 ) ) { return false; }
	RtEmissiveTri e = rtEmissiveFetch( t, ti );
	if ( !( e.flux > 0.0 ) ) { return false; }										// dark (switched off / gone)
	vec3 Pe, nUnit; float pdfw; bool front;
	vec2 xi = vec2( rtEmissiveRand( rng ), rtEmissiveRand( rng ) );
	if ( !rtEmissiveSampleSolidAngle( e, P, xi, Pe, nUnit, pdfw, front ) ) { return false; }
	if ( !front && ( e.emit & RT_EMIT_TWOSIDED ) == 0u ) { return false; }		// a one-sided emitter seen from its back
	vec3 d = Pe - P;
	dl   = length( d );
	if ( !( dl > 1e-3 ) || !( pdfw > 0.0 ) ) { return false; }
	ldir = d / dl;
	// the receiver cosine: against the replaced light's AUTHORED ORIGIN (projF.x: pos = that origin) when the census paired
	// one - a flush ceiling strip lights the wall under it at a grazing angle, the mapper's light sat out in the room and
	// lit that wall; the forward path cosined against that origin, so does this. Else the sampled point.
	vec3 ndir = ( lights.l[ li ].projF.x > 0.5 ) ? normalize( lights.l[ li ].pos - P ) : ldir;
	ndl  = max( dot( N, ndir ), 0.0 );
	if ( !( ndl > 0.0 ) ) { return false; }
	float cosL = max( -dot( nUnit, ldir ), 0.0 );								// nUnit points AT P (the sampler re-winds)
	if ( !( cosL > 0.0 ) ) { return false; }
	float boost = ( knee > 0.0 ) ? max( dl / knee, 1.0 ) : 1.0;
	float fade  = 1.0 - smoothstep( 0.75 * radius, radius, dl );				// out at ONE reach: the row's cull sphere, a Doom light's extent
	vec3  L = rtEmissiveMeanRadiance( e ) * ( e.scale * intensity * boost * fade );
	contrib = L * ( sqrt( cosL ) * ndl / ( pIn * pdfw ) );						// f / p: the row's irradiance estimate
	return !( any( isnan( contrib ) ) );
}

#endif // RT_EMISSIVE_GLSL
