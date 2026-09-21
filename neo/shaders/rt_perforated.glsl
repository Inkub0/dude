// DUDE perforated shadow casters for the RT shadow rays (docs/rtx-shadow-blur.md): the noShadows
// alpha-tested grates / fences the shadow-map path lets cast through its perforated override. They
// sit in the TLAS as NON-opaque geometry under instance mask 0x02, so a ray that doesn't force
// gl_RayFlagsOpaque receives their triangles as CANDIDATES (ordinary geometry is built opaque and is
// committed by traversal itself, never a candidate). A candidate is resolved here against the
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

layout(set = 2, binding = 0) uniform sampler2D u_rtTextures[];

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
