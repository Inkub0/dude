// DUDE RT glass reflections (docs/rtx-reflections.md RR5c) — fragment stage, unbumped.
// Replaces the baked cube/probe sample (environment.frag) with a real ray-traced reflection: reflect
// the view ray off the glass world normal, trace the shared scene TLAS (the same one ssr_rt uses —
// world surfaces + monsters, all attributed since RR5), and shade the hit through the RR4 bindless
// material substrate. On a miss (glass facing open sky, or a ray that escapes) fill with a dim
// sky/ambient tone rather than black, so a pane never voids out. Gated to the RT tier (r_rtReflections)
// by the backend, which loads this program only for VK + ray-query hardware; Nightmare and below keep
// the vanilla cube.
//
// Uniform packing (RB_RHI_RenderTexgenStage, TG_REFLECT_CUBE + rtGlass): u_rtParms.xy = TLAS addr
// lo/hi, .zw = geo-table addr lo/hi (bit-cast); var_Color = the stage tint (dimming) as in the cube
// path. Key light + ambient are fixed constants matching ssr_rt's MVP shade.
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference_uvec2 : require	// build the geo-table pointer from u_rtParms.zw
#extension GL_EXT_nonuniform_qualifier : require		// nonuniformEXT index into the bindless texture array

#include "renderparms.glsl"

VARY(0) in vec3 var_WorldPos;
VARY(1) in vec3 var_WorldNormal;
VARY(2) in vec3 var_WorldToEye;
VARY(3) in vec4 var_Color;

layout(location = 0) out vec4 fragColor;

// idDrawVert as raw uints (stride 60 B = 15 uints): st @3, normal @5. Same view as ssr_rt.frag.
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer VertRef { uint w[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer IdxRef  { uint i[]; };
struct GeoDesc { VertRef vb; IdxRef ib; uint stride; uint flags; uint baseColor; uint texIndex; };
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer GeoTable { GeoDesc d[]; };

// RR4 bindless materials: the resident texture set as one array, indexed by (texIndex - 1).
layout(set = 2, binding = 0) uniform sampler2D u_rtTextures[];

const vec3  RT_KEYDIR  = vec3( 0.42640f, 0.53300f, 0.72760f );	// normalize(0.4,0.5,0.9) — matches ssr_rt
const float RT_AMBIENT = 0.28f;									// ambient term, matches ssr_rt MVP shade
const vec3  RT_SKYFILL = vec3( 0.10f, 0.11f, 0.13f );			// dim sky/ambient on a ray miss (no cube)

void main() {
	vec3 N = normalize( var_WorldNormal );
	vec3 V = normalize( var_WorldToEye );
	if ( dot( N, V ) < 0.0 ) { N = -N; }				// reflect off the viewer-facing side (glass is thin/2-sided)
	vec3 R = reflect( -V, N );							// world reflection direction
	vec3 origin = var_WorldPos + N * 2.0;				// bias off the surface (same as ssr_rt) to avoid self-hit

	rayQueryEXT rq;
	rayQueryInitializeEXT( rq,
		accelerationStructureEXT( uvec2( floatBitsToUint( u_rtParms.x ), floatBitsToUint( u_rtParms.y ) ) ),
		gl_RayFlagsOpaqueEXT, 0xFFu, origin, 0.0, R, 8192.0 );
	while ( rayQueryProceedEXT( rq ) ) { }

	vec3 refl = RT_SKYFILL;								// default: miss -> sky/ambient fill (never black)
	if ( rayQueryGetIntersectionTypeEXT( rq, true ) == gl_RayQueryCommittedIntersectionTriangleEXT ) {
		uint ci = uint( rayQueryGetIntersectionInstanceCustomIndexEXT( rq, true ) );
		uint gi = uint( rayQueryGetIntersectionGeometryIndexEXT( rq, true ) );
		GeoTable table = GeoTable( uvec2( floatBitsToUint( u_rtParms.z ), floatBitsToUint( u_rtParms.w ) ) );
		GeoDesc g = table.d[ ci + gi ];
		if ( ( g.flags & 1u ) == 0u ) {
			refl = RT_SKYFILL;							// positions-only/defer row (no attributes) -> sky fill
		} else {
			uint prim = uint( rayQueryGetIntersectionPrimitiveIndexEXT( rq, true ) );
			vec2 bc   = rayQueryGetIntersectionBarycentricsEXT( rq, true );
			mat4x3 o2w = rayQueryGetIntersectionObjectToWorldEXT( rq, true );
			uint s  = g.stride >> 2u;
			uint p3 = 3u * prim;
			uint b0 = g.ib.i[ p3 + 0u ] * s;
			uint b1 = g.ib.i[ p3 + 1u ] * s;
			uint b2 = g.ib.i[ p3 + 2u ] * s;
			float w0 = 1.0 - bc.x - bc.y;
			vec3 n0 = vec3( uintBitsToFloat( g.vb.w[ b0 + 5u ] ), uintBitsToFloat( g.vb.w[ b0 + 6u ] ), uintBitsToFloat( g.vb.w[ b0 + 7u ] ) );
			vec3 n1 = vec3( uintBitsToFloat( g.vb.w[ b1 + 5u ] ), uintBitsToFloat( g.vb.w[ b1 + 6u ] ), uintBitsToFloat( g.vb.w[ b1 + 7u ] ) );
			vec3 n2 = vec3( uintBitsToFloat( g.vb.w[ b2 + 5u ] ), uintBitsToFloat( g.vb.w[ b2 + 6u ] ), uintBitsToFloat( g.vb.w[ b2 + 7u ] ) );
			vec3 hitN = normalize( mat3( o2w ) * ( w0 * n0 + bc.x * n1 + bc.y * n2 ) );
			vec2 st0 = vec2( uintBitsToFloat( g.vb.w[ b0 + 3u ] ), uintBitsToFloat( g.vb.w[ b0 + 4u ] ) );
			vec2 st1 = vec2( uintBitsToFloat( g.vb.w[ b1 + 3u ] ), uintBitsToFloat( g.vb.w[ b1 + 4u ] ) );
			vec2 st2 = vec2( uintBitsToFloat( g.vb.w[ b2 + 3u ] ), uintBitsToFloat( g.vb.w[ b2 + 4u ] ) );
			vec2 hitST = w0 * st0 + bc.x * st1 + bc.y * st2;

			float ndl    = max( dot( hitN, RT_KEYDIR ), 0.0 );
			vec3  albedo = ( g.texIndex != 0u )
				? texture( u_rtTextures[ nonuniformEXT( g.texIndex - 1u ) ], hitST ).rgb
				: unpackUnorm4x8( g.baseColor ).rgb;
			refl = albedo * ( RT_AMBIENT + ndl );		// MVP shade, matches ssr_rt (key light colour = white)
		}
	}

	// modulate by the stage tint exactly as the cube path did (environment.frag: cube * var_Color),
	// so brightness/hue track the material's dimming; keep the pane's own alpha for the dst blend.
	fragColor = vec4( refl * var_Color.rgb, var_Color.a );
}
