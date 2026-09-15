// DUDE RT reflections (docs/rtx-reflections.md RR2): the dynamic layer of the reflection stack. For
// reflective pixels that screen-space SSR could not serve (its marched result is 0 here — off-screen
// or screen-occluded), trace a reflection ray into the scene TLAS and, on a MONSTER hit, shade it from
// the hit's GPU-skinned gpuSkinVB normal (the fetch r_rtReflTest/RR1 validated). Additive over the lit
// scene with the same Fresnel/gloss weighting as ssr_composite, so material response matches SSR.
//
// VK + RT hardware only (ray_query): the backend loads this program solely when the device supports
// KHR_ray_query, and only draws it when r_rtReflections is on. buffer_reference is enabled by the
// prelude; ray_query is added here (same idiom as interaction.frag). Uniform packing (RhiWorld.cpp
// RB_RHI_ScreenSpaceReflections): u_rtParms = { TLAS addr lo/hi, geo-table addr lo/hi } bit-cast;
// u_modelViewMatrix = inverse view (view->world); u_localParam0.xy = 1/proj00,1/proj11; u_localParam1 =
// (_, intensity, maxRoughness, _); u_windowCoord.z = view-Y sign (VK); u_color = (keyDir.xyz, ambient);
// u_diffuseModifier.rgb = key light colour.
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference_uvec2 : require	// construct the geo-table pointer from u_rtParms.zw (uvec2)

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_ssr;            // SSR march result (rgb 0 = SSR missed this pixel)
SAMPLER_BINDING(1) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(2) uniform sampler2D u_normalBuffer;   // xyz = view-space normal, a = weapon mask
SAMPLER_BINDING(3) uniform sampler2D u_materialBuffer; // x = roughness, y = metalness

VARY(0) in vec2 var_TexCoord;
layout(location = 0) out vec4 fragColor;

// idDrawVert as raw uints (stride 60 B = 15 uints): xyz @0, normal @5 (byte 20). Same view as zfill_batch.
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer VertRef { uint w[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer IdxRef  { uint i[]; };
struct GeoDesc { VertRef vb; IdxRef ib; uint stride; uint flags; uint baseColor; uint pad; };	// RR0/RR3 geometry table row
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer GeoTable { GeoDesc d[]; };

// Doom 3's fixed near / near-infinite far projection in GL clip depth -> linear eye z (negative).
// Same constants as ssao.frag / ssr.frag / ssr_composite.frag.
const vec2 depth_consts = vec2( 0.33333333, -0.33316667 );

void main() {
	vec2 frag = gl_FragCoord.xy;
	float raw = texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;
	if ( raw >= 0.9994 ) { fragColor = vec4( 0.0 ); return; }			// sky / no geometry
	vec2 uv = frag * u_screenCorrection.xy;
	vec4 nt = texture( u_normalBuffer, uv );
	if ( nt.a < 0.5 ) { fragColor = vec4( 0.0 ); return; }				// view weapon

	vec4 mt = texture( u_materialBuffer, uv );
	float rough = mt.x;
	float metal = mt.y;
	float maxRough = u_localParam1.z;
	float gloss = 1.0 - smoothstep( maxRough * 0.7, maxRough, rough );
	if ( gloss < 0.004 ) { fragColor = vec4( 0.0 ); return; }			// not reflective enough

	// SSR already reflected here (screen-space hit) -> don't double-reflect; RT only fills SSR's misses
	if ( dot( texture( u_ssr, uv ).rgb, vec3( 1.0 ) ) > 0.0001 ) { fragColor = vec4( 0.0 ); return; }

	// view-space position + normal (same reconstruction as ssr_composite.frag; u_windowCoord.z flips
	// the reconstructed Y on VK's top-down framebuffer so P agrees with the view-space G-buffer normal)
	float vz  = 1.0 / ( raw * depth_consts.x + depth_consts.y );			// negative
	vec2  ndc = uv * 2.0 - 1.0;
	float dd  = -vz;
	vec3  P = vec3( ndc.x * dd * u_localParam0.x, ndc.y * u_windowCoord.z * dd * u_localParam0.y, vz );
	vec3  N = normalize( nt.xyz * 2.0 - 1.0 );
	vec3  V = normalize( -P );

	// Schlick Fresnel + gloss window, same as ssr_composite (F0 0.9 for metal keeps untinted steel)
	float ndv  = max( dot( N, V ), 0.0 );
	float F0   = mix( 0.04, 0.9, metal );
	float fres = F0 + ( 1.0 - F0 ) * pow( 1.0 - ndv, 5.0 );
	float weight = fres * gloss * u_localParam1.y;						// * r_ssrIntensity
	if ( weight < 0.002 ) { fragColor = vec4( 0.0 ); return; }

	// reflect in view space, lift to world (u_modelViewMatrix = inverse view = view->world)
	vec3 Rv = reflect( -V, N );
	vec3 wp = ( u_modelViewMatrix * vec4( P, 1.0 ) ).xyz;
	vec3 wr = normalize( mat3( u_modelViewMatrix ) * Rv );
	vec3 wN = normalize( mat3( u_modelViewMatrix ) * N );				// world surface normal (ray-origin bias)

	rayQueryEXT rq;
	rayQueryInitializeEXT( rq,
		accelerationStructureEXT( uvec2( floatBitsToUint( u_rtParms.x ), floatBitsToUint( u_rtParms.y ) ) ),
		gl_RayFlagsOpaqueEXT, 0xFFu, wp + wN * 2.0, 0.0, wr, 8192.0 );
	while ( rayQueryProceedEXT( rq ) ) { }
	if ( rayQueryGetIntersectionTypeEXT( rq, true ) != gl_RayQueryCommittedIntersectionTriangleEXT ) {
		fragColor = vec4( 0.0 ); return;								// reflection ray hit nothing
	}
	uint ci = uint( rayQueryGetIntersectionInstanceCustomIndexEXT( rq, true ) );
	uint gi = uint( rayQueryGetIntersectionGeometryIndexEXT( rq, true ) );
	GeoTable table = GeoTable( uvec2( floatBitsToUint( u_rtParms.z ), floatBitsToUint( u_rtParms.w ) ) );
	GeoDesc g = table.d[ ci + gi ];
	if ( ( g.flags & 1u ) == 0u ) { fragColor = vec4( 0.0 ); return; }	// static hit -> SSR/probes own it

	uint prim = uint( rayQueryGetIntersectionPrimitiveIndexEXT( rq, true ) );
	vec2 bc   = rayQueryGetIntersectionBarycentricsEXT( rq, true );
	mat4x3 o2w = rayQueryGetIntersectionObjectToWorldEXT( rq, true );
	uint s  = g.stride >> 2u;
	uint i0 = g.ib.i[ 3u * prim + 0u ];
	uint i1 = g.ib.i[ 3u * prim + 1u ];
	uint i2 = g.ib.i[ 3u * prim + 2u ];
	vec3 n0 = vec3( uintBitsToFloat( g.vb.w[ i0*s + 5u ] ), uintBitsToFloat( g.vb.w[ i0*s + 6u ] ), uintBitsToFloat( g.vb.w[ i0*s + 7u ] ) );
	vec3 n1 = vec3( uintBitsToFloat( g.vb.w[ i1*s + 5u ] ), uintBitsToFloat( g.vb.w[ i1*s + 6u ] ), uintBitsToFloat( g.vb.w[ i1*s + 7u ] ) );
	vec3 n2 = vec3( uintBitsToFloat( g.vb.w[ i2*s + 5u ] ), uintBitsToFloat( g.vb.w[ i2*s + 6u ] ), uintBitsToFloat( g.vb.w[ i2*s + 7u ] ) );
	vec3 hitN = normalize( mat3( o2w ) * ( ( 1.0 - bc.x - bc.y ) * n0 + bc.x * n1 + bc.y * n2 ) );

	// RR3 shade: the hit surface's material average colour (geo-table baseColor) lit by a fixed key
	// light + ambient. The interpolated hit NORMAL (the RR1-validated fetch) gives the monster a shaded
	// 3D look; per-texel diffuse texture is RR4 (needs the bindless material substrate).
	vec3  keyDir = normalize( u_color.xyz );
	float ndl    = max( dot( hitN, keyDir ), 0.0 );
	vec3  albedo = unpackUnorm4x8( g.baseColor ).rgb;
	vec3  lit    = albedo * ( vec3( u_color.w ) + ndl * u_diffuseModifier.rgb );
	fragColor = vec4( lit * weight, 1.0 );
}
