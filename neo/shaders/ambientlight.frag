// Translated from glprogs/ambientLight.vfp (fragment program) - but shaded like vanilla Doom 3, which
// lit ambient lights with interaction.vfp (see main).

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform samplerCube u_ambientCubeMap;
SAMPLER_BINDING(1) uniform sampler2D u_bumpMap;
SAMPLER_BINDING(2) uniform sampler2D u_lightFalloff;
SAMPLER_BINDING(3) uniform sampler2D u_lightProjection;
SAMPLER_BINDING(4) uniform sampler2D u_diffuseMap;
SAMPLER_BINDING(5) uniform sampler2D u_rtLight;    // DUDE r_rtgiAmbient floor: the RT lighting buffer (bound only when u_color.x = 2)
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

	// DUDE: Doom 3's own ambient shading. id shipped this program (glprogs/ambientLight.vfp) for an
	// ambient cube of COLOURS but never used it: vanilla draws an ambientLight material with the
	// interaction program, the "_ambient" cube standing in for the normalization cube map - a
	// constant DIRECTION (tr.ambientLightVector), unpacked and dotted with the bump-mapped normal,
	// no specular. Read as a colour (as this shader did) that vector is ( 1.0, 0.11, 0.89 ): every
	// ambient light came out magenta and flat. The cube holds one texel; any direction reads it.
	vec3 ambientDir = texture( u_ambientCubeMap, vec3( 0.0, 0.0, 1.0 ) ).rgb * 2.0 - 1.0;

	// DUDE GTAO C.2 (docs/ssao-gtao.md): bias the shading normal toward the bent normal (average
	// unoccluded direction) so the ambient responds to macro occlusion, not just the surface
	// normal. The bent normal is view-space in the AO buffer's GBA; rotate it to world with the
	// inverse view rotation (v * M == Mt * v, M = world->eye view matrix), then into tangent
	// space: var_ToGlobalRow0..2 are the rows of the tangent->world rotation, so the matrix built
	// from them as COLUMNS is its transpose, world->tangent. u_localParam0.zw give the AO uv
	// (shared with the C.1 occlusion multiply below).
	vec3 shadeNormal = localNormal;
	if ( u_localParam1.x > 0.0 ) {
		vec3 bentView  = texture( u_ssao, gl_FragCoord.xy * u_localParam0.zw ).gba * 2.0 - 1.0;
		vec3 bentWorld = bentView * mat3( u_modelViewMatrix );
		mat3 worldToLocal = mat3( var_ToGlobalRow0, var_ToGlobalRow1, var_ToGlobalRow2 );
		vec3 bentLocal = normalize( worldToLocal * bentWorld );
		shadeNormal = normalize( mix( normalize( localNormal ), bentLocal, u_localParam1.x ) );
	}

	// a negative dot clamps, as the fixed-point colour output of the vanilla program did
	vec3 ambient = vec3( max( dot( ambientDir, shadeNormal ), 0.0 ) );
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

	// RTGI reconcile (docs/rtx-rtgi.md H6c, r_rtgiAmbient): with r_rtgi on the fullscreen GI apply adds real bounce
	// light. u_color.x = 0: GI off, the vanilla ambient. 1 = REPLACE: draw nothing (the GI stands in, or - in the fold
	// mode - carries this light's fill through the bounce). 2 = FLOOR: add only what the RT lighting at this pixel falls
	// short of this light's fill, so indirect = max( RT, ambient ) with no double-counting where the GI is bright. The RT
	// buffer (u_rtLight: albedo-free irradiance, NRD YCoCg unless u_color.z = 1, times u_color.y) is compared through
	// this surface's diffuse, the factor both the RT apply and the ambient above multiply by. Several overlapping ambient
	// lights each take the floor on their own (the sum can exceed a single max) - Doom's ambience is rarely stacked.
	if ( u_color.x > 1.5 ) {
		vec3 rt = vec3( 0.0 );
		if ( u_color.y > 0.0 ) {
			vec3 c = texture( u_rtLight, gl_FragCoord.xy * u_screenCorrection.xy ).rgb;
			if ( u_color.z < 0.5 ) {
				float t = c.x - c.z;
				c = vec3( t + c.y, c.x + c.z, t - c.y );		// NRD YCoCg -> linear (rtgi_apply.frag)
			}
			rt = max( c, vec3( 0.0 ) ) * u_color.y;
		}
		outRgb = max( outRgb - diff * rt, vec3( 0.0 ) );
	} else if ( u_color.x > 0.5 ) {
		outRgb = vec3( 0.0 );
	}

	fragColor = vec4( outRgb, 1.0 );
}
