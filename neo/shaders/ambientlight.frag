// Translated from glprogs/ambientLight.vfp (fragment program).

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform samplerCube u_ambientCubeMap;
SAMPLER_BINDING(1) uniform sampler2D u_bumpMap;
SAMPLER_BINDING(2) uniform sampler2D u_lightFalloff;
SAMPLER_BINDING(3) uniform sampler2D u_lightProjection;
SAMPLER_BINDING(4) uniform sampler2D u_diffuseMap;
SAMPLER_BINDING(9) uniform sampler2D u_ssao;   // DUDE GTAO buffer (R = ambient visibility)
SAMPLER_BINDING(10) uniform sampler2D u_occlusionMap; // DUDE baked AO map (R = visibility)

VARY(0) in vec2 var_TexBump;
VARY(1) in vec2 var_TexDiffuse;
VARY(2) in vec2 var_TexFalloff;
VARY(3) in vec4 var_TexProjection;
VARY(4) in vec3 var_ToGlobalRow0;
VARY(5) in vec3 var_ToGlobalRow1;
VARY(6) in vec3 var_ToGlobalRow2;
VARY(7) in vec4 var_Color;

layout(location = 0) out vec4 fragColor;

void main() {
	// Reconstruct local normal (RXGB swizzle: bump.a -> x)
	vec4 bump = texture( u_bumpMap, var_TexBump );
	vec3 localNormal = vec3( bump.a, bump.y, bump.z ) * 2.0 - 1.0;

	// Transform into ambient map space using a 3x3 matrix multiply
	mat3 toGlobal = mat3( var_ToGlobalRow0, var_ToGlobalRow1, var_ToGlobalRow2 );
	vec3 globalNormal = toGlobal * localNormal;

	// DUDE GTAO C.2 (docs/ssao-gtao.md): bias the ambient sampling direction toward the
	// bent normal (average unoccluded direction) so the ambient responds to macro
	// occlusion, not just the surface normal. The bent normal is view-space in the AO
	// buffer's GBA; rotate it to world with the inverse view rotation (v * M == Mᵀ * v,
	// M = world->eye view matrix), then blend by u_localParam1.x. u_localParam0.zw give
	// the AO uv (shared with the C.1 occlusion multiply below).
	vec3 shadeNormal = globalNormal;
	if ( u_localParam1.x > 0.0 ) {
		vec3 bentView  = texture( u_ssao, gl_FragCoord.xy * u_localParam0.zw ).gba * 2.0 - 1.0;
		vec3 bentWorld = bentView * mat3( u_modelViewMatrix );
		shadeNormal = normalize( mix( globalNormal, bentWorld, u_localParam1.x ) );
	}

	// Do all lookups as rgb-only where possible and combine multiplies
	vec3 ambient = texture( u_ambientCubeMap, shadeNormal ).rgb;
	vec3 diff = texture( u_diffuseMap, var_TexDiffuse ).rgb;
	vec3 falloff = texture( u_lightFalloff, var_TexFalloff ).rgb;
	vec3 proj = textureProj( u_lightProjection, var_TexProjection ).rgb;

	vec3 modifier = vec3( u_diffuseModifier );
	vec3 color = var_Color.xyz;

	vec3 outRgb = ambient * diff * falloff * proj * modifier * color;

	// DUDE GTAO (docs/ssao-gtao.md Phase C): occlude the ambient term only. This pass
	// is additive (one of possibly several ambient lights), so scaling each ambient
	// contribution is equivalent to scaling their sum. u_localParam0.x enables it;
	// .y is the floor (fully-occluded darkens to this, never to black — the anti-crush
	// countermeasure for a dark game); .zw map gl_FragCoord to the AO buffer's uv.
	if ( u_localParam0.x > 0.5 ) {
		float ao = texture( u_ssao, gl_FragCoord.xy * u_localParam0.zw ).r;
		ao = mix( u_localParam0.y, 1.0, ao );
		outRgb *= ao;
	}

	// DUDE baked ambient-occlusion map (docs/occlusion-maps.md). A per-material AO texture
	// (from the surface's occlusionmap stage), applied to the ambient term exactly like the
	// screen-space SSAO above; the two stack multiplicatively when both are on. Sampled with
	// the diffuse UV. u_occlusionParms.x gates it, .y is the strength (mix toward the map).
	if ( u_occlusionParms.x > 0.5 ) {
		float aoMap = texture( u_occlusionMap, var_TexDiffuse ).r;
		outRgb *= mix( 1.0, aoMap, u_occlusionParms.y );
	}

	fragColor = vec4( outRgb, 1.0 );
}
