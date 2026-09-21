// DUDE RT glass reflections (docs/rtx-reflections.md RR5c stage 2) — fragment stage, BUMPED.
// Replaces the baked cube sample (bumpyenvironment.frag) with a real ray-traced reflection off a
// bump-perturbed glass surface. Builds the world reflection normal exactly as bumpyenvironment.frag does
// (sample the bump map, rotate the tangent-space normal by the tangent->global rows), then FLATTENS it
// toward the geometric surface normal for ray stability (a raw per-texel bump normal scatters adjacent
// reflection rays into noise / self-intersection), reflects the world view ray, traces the shared scene
// TLAS (the same one ssr_rt + environment_rt use — world surfaces, props, monsters, all attributed since
// RR5/Item 1), and shades the hit through the RR4 bindless material substrate. On a miss it fills with a
// dim sky/ambient gradient (never black). Gated to the RT tier (r_rtReflections) by the backend, which
// loads this program only for VK + ray-query hardware; Nightmare and below keep the vanilla cube.
//
// Uniform packing (RB_RHI_RenderTexgenStage, TG_REFLECT_CUBE + rtGlass bumped): u_rtParms.xy = TLAS addr
// lo/hi, .zw = geo-table addr lo/hi (bit-cast); var_Color = the stage tint (dimming); the bump map is on
// unit 0 (this variant skips the cube, so unit 0 is free — no novel unit-1-without-0 binding). Key light
// + ambient are fixed constants matching ssr_rt / environment_rt's MVP shade.
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference_uvec2 : require	// build the geo-table pointer from u_rtParms.zw
#extension GL_EXT_nonuniform_qualifier : require		// nonuniformEXT index into the bindless texture array

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_bumpMap;

VARY(0) in vec2 var_TexCoord;
VARY(1) in vec3 var_WorldPos;
VARY(2) in vec3 var_ToEyeGlobal;
VARY(3) in vec3 var_ToGlobalRow0;
VARY(4) in vec3 var_ToGlobalRow1;
VARY(5) in vec3 var_ToGlobalRow2;
VARY(6) in vec4 var_Color;

layout(location = 0) out vec4 fragColor;

// idDrawVert as raw uints (stride 60 B = 15 uints): st @3, normal @5. Same view as ssr_rt / environment_rt.
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer VertRef { uint w[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer IdxRef  { uint i[]; };
struct GeoDesc { VertRef vb; IdxRef ib; uint stride; uint flags; uint baseColor; uint texIndex; };
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer GeoTable { GeoDesc d[]; };

// RR4 bindless materials: the resident texture set as one array, indexed by (texIndex - 1).
layout(set = 2, binding = 0) uniform sampler2D u_rtTextures[];

const vec3  RT_KEYDIR  = vec3( 0.42640f, 0.53300f, 0.72760f );	// normalize(0.4,0.5,0.9) — matches environment_rt
const float RT_AMBIENT = 0.28f;									// ambient term, matches the MVP shade
const vec3  RT_SKY_UP   = vec3( 0.13f, 0.14f, 0.16f );			// reflection points up (ceiling / sky)
const vec3  RT_SKY_DOWN = vec3( 0.07f, 0.08f, 0.09f );			// reflection points down (floor / ground)
const float RT_GRAZE    = 1.0f;									// added reflectivity at grazing angles
// how far to flatten the bump-perturbed normal toward the flat surface normal before tracing. 0 = full
// bump (noisy — every texel scatters its own ray), 1 = mirror-flat (loses the bump entirely). Glass bump
// is usually subtle, so keep most of it but tame the divergence; tune here if the bumped panes shimmer.
const float RT_BUMP_FLATTEN = 0.5f;

void main() {
	// build the world reflection normal exactly as bumpyenvironment.frag: sample the bump map (heightmap
	// packed in .a, mirrored into .x by the compressor), unpack the tangent-space normal, rotate to global.
	vec4 bump = texture( u_bumpMap, var_TexCoord );
	bump.x = bump.a;
	vec3 localNormal = bump.xyz * 2.0 - 1.0;
	mat3 normalMatrix = mat3( var_ToGlobalRow0, var_ToGlobalRow1, var_ToGlobalRow2 );
	vec3 bumpN = normalMatrix * localNormal;
	// the geometric (flat) surface normal in the SAME convention: normalMatrix * tangent-space (0,0,1),
	// which is column 2 == var_ToGlobalRow2. Used for the stable origin bias, the viewer-side flip, and
	// as the flatten target so bumpN and geomN coincide on an un-bumped texel.
	vec3 geomN = var_ToGlobalRow2;

	float gn2 = dot( geomN, geomN );
	float bn2 = dot( bumpN, bumpN );
	if ( !( gn2 > 1e-6 ) ) { fragColor = vec4( 0.0 ); return; }	// degenerate frame guard (mirrors environment_rt)
	vec3 gN = geomN * inversesqrt( gn2 );
	vec3 Nb = ( bn2 > 1e-6 ) ? bumpN * inversesqrt( bn2 ) : gN;	// bump underflow -> fall back to flat
	vec3 N  = normalize( mix( Nb, gN, RT_BUMP_FLATTEN ) );

	vec3 V = normalize( var_ToEyeGlobal );
	if ( dot( gN, V ) < 0.0 ) { N = -N; gN = -gN; }				// reflect off the viewer-facing side (thin/2-sided)
	vec3 R = reflect( -V, N );									// world reflection direction
	vec3 origin = var_WorldPos + gN * 2.0;						// bias along the STABLE flat normal (avoid self-hit)

	// sky gradient for a ray miss (up lighter, down darker) + Schlick grazing boost (like environment_rt)
	vec3  skyFill = mix( RT_SKY_DOWN, RT_SKY_UP, clamp( R.z * 0.5 + 0.5, 0.0, 1.0 ) );
	float ndv     = clamp( dot( N, V ), 0.0, 1.0 );
	float m       = 1.0 - ndv;
	float graze   = 1.0 + RT_GRAZE * ( m * m ) * ( m * m ) * m;	// pow(1-ndv,5) as muls (RR11-style)

	rayQueryEXT rq;
	rayQueryInitializeEXT( rq,
		accelerationStructureEXT( uvec2( floatBitsToUint( u_rtParms.x ), floatBitsToUint( u_rtParms.y ) ) ),
		gl_RayFlagsOpaqueEXT, 0xFDu /* solid rays: skip perforated-caster instances (mask 0x02) */, origin, 0.0, R, 8192.0 );
	while ( rayQueryProceedEXT( rq ) ) { }

	vec3 refl = skyFill;										// default: miss -> sky/ambient fill (never black)
	if ( rayQueryGetIntersectionTypeEXT( rq, true ) == gl_RayQueryCommittedIntersectionTriangleEXT ) {
		uint ci = uint( rayQueryGetIntersectionInstanceCustomIndexEXT( rq, true ) );
		uint gi = uint( rayQueryGetIntersectionGeometryIndexEXT( rq, true ) );
		GeoTable table = GeoTable( uvec2( floatBitsToUint( u_rtParms.z ), floatBitsToUint( u_rtParms.w ) ) );
		GeoDesc g = table.d[ ci + gi ];
		if ( ( g.flags & 1u ) == 0u ) {
			refl = skyFill;										// positions-only/defer row (no attributes) -> sky fill
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
			refl = albedo * ( RT_AMBIENT + ndl );				// MVP shade, matches environment_rt (key light = white)
		}
	}

	// modulate by the stage tint exactly as the RT/cube glass paths do, and keep the pane's own alpha for
	// the dst blend the cube used.
	fragColor = vec4( refl * var_Color.rgb * graze, var_Color.a );
}
