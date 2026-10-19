// DUDE perforated shadow casters for the RT shadow rays (docs/rtx-shadow-blur.md): the alpha-tested
// grates / fences / see-through floors the shadow-map caster pass punches holes through
// (RB_RHI_SetupCasterCoverage alpha-tests EVERY MC_PERFORATED caster). Two homes in the TLAS, one rule:
// the noShadows grates the shadow-map path lets cast through its perforated override sit in their own
// BLASes under instance mask 0x02; perforated materials that cast on their own (sflooralpha and the
// other floor grates) stay in the solid 0xFF instances - RTAO / reflections / GI must keep seeing them -
// as NON-opaque geometries. Either way a ray that doesn't force gl_RayFlagsOpaque receives their
// triangles as CANDIDATES (opaque geometry is committed by traversal itself, never a candidate), and
// a ray that does force it sees them solid. A candidate is resolved here against the
// material's coverage texture: the hit st comes from the geometry table, the alpha from the bindless
// texture array. Rays that cannot do this trace with cull mask 0xFD and never see a grate.
//
// A solid grate texel is never CONFIRMED as a hit: it only dims the light to
// 1 - r_shadowMapPerforatedStrength and the ray carries on, so a fence doesn't black out a room
// (what the shadow-map path gets from dithering its caster) - deterministic, no noise to filter.
//
// The including shader must enable GL_EXT_ray_query, GL_EXT_buffer_reference_uvec2 and
// GL_EXT_nonuniform_qualifier. Same table / bindless layout as ssr_rt.frag (RR0 / RR4).
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer PerfVertRef { uint w[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer PerfIdxRef  { uint i[]; };
struct PerfGeoDesc { PerfVertRef vb; PerfIdxRef ib; uint stride; uint flags; uint baseColor; uint texIndex; };
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer PerfGeoTable { PerfGeoDesc d[]; };

#ifndef DUDE_RT_TEXTURES_DECLARED		// shared with rt_light_tex.glsl (restir_di.frag includes both)
#define DUDE_RT_TEXTURES_DECLARED
layout(set = 2, binding = 0) uniform sampler2D u_rtTextures[];
#endif

// How close to the light a grate texel may still cast. The shadow-map caster clips at its cube faces'
// 1-unit near plane (RB_RHI_CubeFaceViewProj), so a fixture's own grille casts its pattern there; this is
// that plane. The 8-unit margin that used to live here carved a SPHERE out of every flat grate near a
// bulb - whatever part of the card lay inside it stopped casting, so the shadow read as cast off a
// spherical shell with a round hole under the light: the very dead zone 25c1fd0b cured in the cube
// caster (2026-10-19).
const float DUDE_RT_PERF_NEAR = 1.0;

// Per-pixel gate: the boxes of the grates inside this light's volume ( min.xyz, max.xyz pairs, padded
// on the CPU ). Only a shadow segment that passes through one of them can meet a grate, so only it
// needs the grate-aware ray; everything else keeps the fixed force-opaque ray - the hardware's fastest
// traversal. A slab test per box costs a few dozen flops against a whole slower ray.
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer PerfBoxes { vec4 b[]; };

bool dudeSegmentMeetsPerforated( uvec2 boxAddr, int count, vec3 O, vec3 dir, float tMax ) {
	vec3 inv = 1.0 / mix( dir, vec3( 1e-8 ), lessThan( abs( dir ), vec3( 1e-8 ) ) );
	PerfBoxes boxes = PerfBoxes( boxAddr );
	for ( int i = 0; i < count; i++ ) {
		vec3 t0 = ( boxes.b[ 2 * i ].xyz - O ) * inv;
		vec3 t1 = ( boxes.b[ 2 * i + 1 ].xyz - O ) * inv;
		vec3 tn = min( t0, t1 ), tf = max( t0, t1 );
		float tNear = max( max( tn.x, tn.y ), max( tn.z, 0.0 ) );
		float tFar  = min( min( tf.x, tf.y ), min( tf.z, tMax ) );
		if ( tNear <= tFar ) {
			return true;
		}
	}
	return false;
}

// Is the candidate triangle ( instance custom index ci, geometry gi, primitive prim, barycentrics bc )
// a SOLID texel of a perforated caster? False for holes and for any row that isn't alpha-tested.
bool dudePerforatedSolid( uvec2 geoAddr, uint ci, uint gi, uint prim, vec2 bc ) {
	PerfGeoDesc g = PerfGeoTable( geoAddr ).d[ ci + gi ];
	if ( ( g.flags & 2u ) == 0u || g.texIndex == 0u ) {
		return false;
	}
	uint sw = g.stride >> 2u;
	uint p3 = 3u * prim;
	uint b0 = g.ib.i[ p3 + 0u ] * sw;				// idDrawVert as uints: st at words 3, 4
	uint b1 = g.ib.i[ p3 + 1u ] * sw;
	uint b2 = g.ib.i[ p3 + 2u ] * sw;
	vec2 st0 = vec2( uintBitsToFloat( g.vb.w[ b0 + 3u ] ), uintBitsToFloat( g.vb.w[ b0 + 4u ] ) );
	vec2 st1 = vec2( uintBitsToFloat( g.vb.w[ b1 + 3u ] ), uintBitsToFloat( g.vb.w[ b1 + 4u ] ) );
	vec2 st2 = vec2( uintBitsToFloat( g.vb.w[ b2 + 3u ] ), uintBitsToFloat( g.vb.w[ b2 + 4u ] ) );
	vec2 hitST = ( 1.0 - bc.x - bc.y ) * st0 + bc.x * st1 + bc.y * st2;
	// explicit LOD: no implicit derivatives inside non-uniform control flow
	float alpha = textureLod( u_rtTextures[ nonuniformEXT( g.texIndex - 1u ) ], hitST, 0.0 ).a;
	return alpha >= float( ( g.flags >> 8u ) & 0xFFu ) * ( 1.0 / 255.0 );
}
