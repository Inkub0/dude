// DUDE RT GI ray pass (docs/rtx-rtgi.md H6a): one cosine-weighted hemisphere ray per pixel against
// the scene TLAS - but instead of RTAO's occlusion bit, it SHADES the hit cheaply and returns the
// incoming radiance (one indirect diffuse bounce). The ray setup, cosine-hemisphere direction, world
// reconstruction and NRD guides are identical to rtao_ray.frag; the hit-shading is lifted from
// ssr_rt.frag (geo-table fetch -> bindless albedo -> scene-light loop -> emissive).
//
// H6a is a bring-up slice: the raw radiance is only shown by r_rtgiDebug 1. Nothing consumes it in
// normal play yet - the NRD DIFFUSE denoise is H6b, the ambient apply + fake-fill reconcile is H6c,
// the one-dominant-light shadow ray is H6d - so with r_rtgi 0 (default) this pass never runs and the
// shipped RTAO path is untouched. VK + RT hardware only (ray_query).
//
// Uniform packing (RB_RHI_RtgiPass):
//   u_rtParms.xy          = TLAS device address (bit-cast lo/hi)
//   u_rtParms.zw          = geo-table device address (bit-cast lo/hi)   [vs rtao_ray: NEW]
//   u_modelViewMatrix     = inverse view (view -> world)
//   u_localParam0         = ( 1/proj00, 1/proj11, gatherRadius, 0 )
//   u_localParam1         = ( frameIndex (hashed for the IGN reorientation), 0, 0, eye-pullback per unit depth )
//   u_color               = ( scene-light buffer addr lo/hi bit-cast, light count, ambient floor at the hit )
//   u_diffuseModifier     = ( emissive glow scale, light brightness scale, frustum gate, real-lights enable )
//   u_screenCorrection.xy = 1 / raySize   (gl_FragCoord -> [0,1] uv)
//   u_depthTexRecip.xy    = gl_FragCoord -> _currentDepth tc
//   u_windowCoord.z       = view-Y sign (-1 Vulkan top-down)
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference_uvec2 : require	// construct the geo-table pointer from u_rtParms.zw (uvec2)
#extension GL_EXT_nonuniform_qualifier : require		// RR4: nonuniformEXT index into the bindless texture array

#include "renderparms.glsl"
// scene-light buffer row + evaluation (shared with RT reflections + volumetric lighting)
#include "rt_light_eval.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(1) uniform sampler2D u_normalBuffer;   // xyz = view-space normal, a = weapon/AO mask

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;	// rgb = gathered indirect radiance; a = normalized hit distance (H6b: NRD IN_DIFF_RADIANCE_HITDIST)
layout(location = 1) out vec4 outViewZ;		// IN_VIEWZ: signed linear view z (negative), R16F
layout(location = 2) out vec4 outNormal;	// IN_NORMAL_ROUGHNESS: world normal, NRD encoding 2 (signed octahedron), RGBA16F

// idDrawVert as raw uints (stride 60 B = 15 uints): xyz @0, st @3, normal @5. Same view as ssr_rt.frag.
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer VertRef { uint w[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer IdxRef  { uint i[]; };
struct GeoDesc { VertRef vb; IdxRef ib; uint stride; uint flags; uint baseColor; uint texIndex; };	// RR0/RR3/RR4 geometry table row
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer GeoTable { GeoDesc d[]; };
// RR4 bindless materials: the resident texture set as one array, indexed by (texIndex - 1). Set 2 is
// bound once per frame by the backend and populated by SyncBindlessSlot. nonuniformEXT because each
// gather ray can hit a different surface.
layout(set = 2, binding = 0) uniform sampler2D u_rtTextures[];

// Doom 3's fixed near / near-infinite far projection in GL clip depth -> linear eye z (negative).
#define depth_consts DUDE_DEPTH_CONSTS()

// interleaved gradient noise (ssao.frag): per-pixel dither, denoiser-friendly 3x3 spread.
float ign( vec2 p ) {
	return fract( 52.9829189 * fract( dot( p, vec2( 0.06711056, 0.00583715 ) ) ) );
}

// branchless orthonormal basis around n (Duff et al. / Frisvad)
void onb( vec3 n, out vec3 t, out vec3 b ) {
	float s = n.z >= 0.0 ? 1.0 : -1.0;
	float a = -1.0 / ( s + n.z );
	float c = n.x * n.y * a;
	t = vec3( 1.0 + s * n.x * n.x * a, s * c, -s * n.x );
	b = vec3( c, s + n.y * n.y * a, -n.y );
}

// world normal -> NRD normal/roughness encoding 2 (signed octahedron, z sign in roughness)
vec4 packNrdNormal( vec3 Nraw ) {
	float nl2 = dot( Nraw, Nraw );
	vec3  n;
	if ( !( nl2 > 1e-4 ) ) {
		n = vec3( 0.0, 0.0, 1.0 );
	} else {
		n = normalize( mat3( u_modelViewMatrix ) * ( Nraw * inversesqrt( nl2 ) ) );	// world-space
	}
	n /= abs( n.x ) + abs( n.y ) + abs( n.z );
	vec3 r;
	r.y = n.y * 0.5 + 0.5;
	r.x = n.x * 0.5 + r.y;
	r.y -= n.x * 0.5;
	float roughness = 1.0;										// can't be 0: z carries n.z's sign
	float s = n.z < 0.0 ? -roughness : roughness;
	r.z = s * 0.5 + 0.5;
	return vec4( r, 0.0 );										// w = materialID/3 = 0
}

void main() {
	vec2  frag = gl_FragCoord.xy;
	float raw  = texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;
	// NRD guides first, for EVERY pixel (mirrors rtao_ray.frag), so H6b can drop the denoise in unchanged
	vec4 ntGuide = texture( u_normalBuffer, frag * u_screenCorrection.xy );
	outViewZ  = vec4( 1.0 / ( min( raw, 0.9994 ) * depth_consts.x + depth_consts.y ), 0.0, 0.0, 1.0 );
	outNormal = packNrdNormal( ntGuide.xyz * 2.0 - 1.0 );
	// sky / the view weapon / a degenerate normal gather no indirect bounce -> zero radiance
	if ( raw >= 0.9994 ) { fragColor = vec4( 0.0 ); return; }
	vec4 nt = ntGuide;
	if ( nt.a < 0.5 ) { fragColor = vec4( 0.0 ); return; }

	// view-space position (ssao.frag reconstruction, incl. the VK view-Y sign) + normal. A degenerate
	// G-buffer normal would normalize() to NaN and a NaN ray direction can hang RT traversal (device lost),
	// so any such pixel returns zero radiance instead of tracing.
	float vz  = 1.0 / ( min( raw, 0.9994 ) * depth_consts.x + depth_consts.y );   // negative
	vec2  ndc = frag * ( u_screenCorrection.xy * 2.0 ) - 1.0;
	float d   = -vz;
	vec3  P   = vec3( ndc.x * d * u_localParam0.x, ndc.y * u_windowCoord.z * d * u_localParam0.y, vz );
	vec3  Nraw = nt.xyz * 2.0 - 1.0;
	float nl2  = dot( Nraw, Nraw );
	if ( !( nl2 > 1e-4 ) ) { fragColor = vec4( 0.0 ); return; }
	vec3  N   = Nraw * inversesqrt( nl2 );

	// lift to world (u_modelViewMatrix = inverse view)
	vec3 Pw = ( u_modelViewMatrix * vec4( P, 1.0 ) ).xyz;
	vec3 Nw = normalize( mat3( u_modelViewMatrix ) * N );

	// one cosine-weighted hemisphere direction, IGN re-oriented per frame (rtao_ray.frag: a value-phase
	// slide leaves IGN's 45-degree diagonal in the residue and shows as lines under the res-scale upscale;
	// re-orienting points each frame's residue a different way so the temporal accumulation averages it out)
	uint  fh = uint( u_localParam1.x ) * 747796405u + 2891336453u;
	fh = ( ( fh >> ( ( fh >> 28u ) + 4u ) ) ^ fh ) * 277803737u;
	fh = ( fh >> 22u ) ^ fh;
	vec2  jc = frag + vec2( float( ( fh >> 3u ) & 1023u ), float( ( fh >> 13u ) & 1023u ) );
	if ( ( fh & 1u ) != 0u ) { jc = jc.yx; }
	if ( ( fh & 2u ) != 0u ) { jc.x = -jc.x; }
	if ( ( fh & 4u ) != 0u ) { jc.y = -jc.y; }
	float r1 = ign( jc );
	float r2 = ign( jc.yx + vec2( 37.0, 17.0 ) );
	float phi  = 6.28318530718 * r1;
	float sinT = sqrt( r2 );
	float cosT = sqrt( 1.0 - r2 );
	vec3 T, B;
	onb( Nw, T, B );
	vec3 dir = T * ( cos( phi ) * sinT ) + B * ( sin( phi ) * sinT ) + Nw * cosT;
	float dl2 = dot( dir, dir );
	if ( !( dl2 > 1e-6 ) ) { fragColor = vec4( 0.0 ); return; }
	dir *= inversesqrt( dl2 );

	// ray origin pulled toward the eye (rtao_ray.frag crease-leak fix) + a tiny normal lift
	vec3  eyeW   = ( u_modelViewMatrix * vec4( 0.0, 0.0, 0.0, 1.0 ) ).xyz;
	vec3  rayOrg = Pw + normalize( eyeW - Pw ) * ( 0.25 + d * u_localParam1.w ) + Nw * 0.05;
	float radius = u_localParam0.z;
	rayQueryEXT rq;
	rayQueryInitializeEXT( rq,
		accelerationStructureEXT( uvec2( floatBitsToUint( u_rtParms.x ), floatBitsToUint( u_rtParms.y ) ) ),
		gl_RayFlagsOpaqueEXT, 0xFDu /* solid rays: skip perforated-caster instances (mask 0x02) */, rayOrg, 0.0, dir, radius );
	while ( rayQueryProceedEXT( rq ) ) { }
	if ( rayQueryGetIntersectionTypeEXT( rq, true ) != gl_RayQueryCommittedIntersectionTriangleEXT ) {
		fragColor = vec4( 0.0 );		// miss inside the gather radius: open space contributes no bounce here
		return;
	}
	float thit = rayQueryGetIntersectionTEXT( rq, true );

	// ---- shade the hit (lifted from ssr_rt.frag): geo-table row -> interpolate -> bindless albedo -> scene lights -> emissive
	uint ci = uint( rayQueryGetIntersectionInstanceCustomIndexEXT( rq, true ) );
	uint gi = uint( rayQueryGetIntersectionGeometryIndexEXT( rq, true ) );
	GeoTable table = GeoTable( uvec2( floatBitsToUint( u_rtParms.z ), floatBitsToUint( u_rtParms.w ) ) );
	GeoDesc g = table.d[ ci + gi ];
	if ( ( g.flags & 1u ) == 0u ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }	// no-attr defer row (mover/prop): not shadeable yet

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

	// RR4: albedo from the hit's real diffuse (bindless slot texIndex-1), else the RR3 material average.
	vec4  bcol = unpackUnorm4x8( g.baseColor );
	vec3  albedo = ( g.texIndex != 0u )
		? texture( u_rtTextures[ nonuniformEXT( g.texIndex - 1u ) ], hitST ).rgb
		: bcol.rgb;

	// world hit (rigid ray: hitWorld = rayOrg + dir*thit). Face the hit normal back toward the ray so a
	// bounce off the inside of a single-sided wall still lights from the correct side.
	vec3  hitWorld = rayOrg + dir * thit;
	if ( dot( hitN, dir ) > 0.0 ) { hitN = -hitN; }

	// Li = outgoing radiance at the hit toward the shading pixel. Diffuse only; the receiver's own albedo
	// and the 1/pi * pi (cosine-sampling) factors are applied at the H6c consume site, so here we return
	// the hit's lit colour. u_color.w is a small ambient floor (0 keeps it a pure bounce).
	vec3  Li = albedo * u_color.w;
	if ( u_diffuseModifier.w > 0.5 ) {									// r_rtgi real scene lights
		RtLights lights = RtLights( uvec2( floatBitsToUint( u_color.x ), floatBitsToUint( u_color.y ) ) );
		uint nLights = uint( u_color.z );
		bool gate = u_diffuseModifier.z > 0.5;
		for ( uint li = 0u; li < nLights; li++ ) {
			if ( gate && !dudeLightInRange( lights, li, hitWorld ) ) { continue; }
			vec3  ldir; float dl;
			dudeLightDir( lights, li, hitWorld, ldir, dl );
			float ndl = max( dot( hitN, ldir ), 0.0 );
			if ( ndl <= 0.0 ) { continue; }
			float atten = dudeLightAtten( lights, li, hitWorld, dl, lights.l[ li ].flags, gate, false );
			if ( atten <= 0.0 ) { continue; }
			// H6d will trace one shadow ray toward the dominant light here; H6a is diffuse-only (leaks like ssr_rt)
			Li += albedo * lights.l[ li ].color * ( atten * ndl * u_diffuseModifier.y );
		}
	}
	if ( bcol.a > 0.5 ) { Li += bcol.rgb * u_diffuseModifier.x; }	// self-illuminated source (neon, light tile)

	// H6b: normalized hit distance in alpha for the NRD REBLUR_DIFFUSE denoise — saturate( thit / (A + |viewZ|*B) ),
	// A = 0.5*radius, B = 0.1, MUST match the ReblurHitDistanceParameters set in RB_RHI_RtgiPass. Radiance is fed
	// LINEAR in .rgb (consumed linearly by rtgi_apply; NRD's YCoCg anti-firefly packing is a later refinement).
	fragColor = vec4( Li, clamp( thit / ( 0.5 * radius + d * 0.1 ), 0.0, 1.0 ) );
}
