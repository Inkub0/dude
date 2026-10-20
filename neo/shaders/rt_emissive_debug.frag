// DUDE RT emissive table debug overlay (docs/rtx-emissive-triangles.md E1d, r_rtEmissiveTableDebug). Draws the table's
// triangles over the finished view, depth-tested by hand against the sealed scene depth (unit 0) so a wall hides the
// emitters behind it while the emitters themselves - opaque fixtures at exactly the scene depth, pure-additive tubes
// that never wrote depth - pass. Modes (u_localParam0.x):
//   1 = TEXTURE: a stable tint per glow texture (which surfaces are in the table, and which material each belongs to);
//   2 = FLUX: the triangle's flux relative to the table's MEAN (u_localParam0.y = 1 / meanFlux), log ramp:
//       blue = 1/16 of the mean or fainter, green = the mean, red = 4x, white = 16x and above;
//   3 = EMITTED COLOUR: the emitter's MEAN radiance (glow average x stage colour x scale, flat - what E2 lights with); a
//       flare-inferred patch shows its diffuse through the threshold instead.
//   4 = LIGHT COVERAGE (the E5 hybrid question): green = the triangle sits inside a placed light's volume (that light is
//       "fixtured" and would be dropped when the emitters light the scene), red = no placed light covers it (this
//       emitter would be the only light there).
// Opaque, into the pre-tonemap HDR scene.
//
// Uniform packing (RB_RHI_RtEmissiveDebugOverlay): u_mvpMatrix = world -> clip (vertex stage); u_localParam0 =
// ( mode, 1/meanFlux, -, - ); u_depthTexRecip.xy = 1 / depth upload size; unit 0 = sealed scene depth (POT);
// set 2 = the backend's bindless texture table (the glow textures by slot).
#extension GL_EXT_nonuniform_qualifier : require

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentDepth;
layout(set = 2, binding = 0) uniform sampler2D u_rtTextures[];

VARY(0) in vec2 var_TexCoord;
VARY(1) in vec3 var_FluxScaleIndex;
VARY(2) flat in vec3 var_SlotFlags;
VARY(3) in vec4 var_Tint;

layout(location = 0) out vec4 fragColor;

vec3 dbgTint( uint id ) {
	uint s = id * 2654435761u + 1u;
	uint w = ( ( s >> ( ( s >> 28u ) + 4u ) ) ^ s ) * 277803737u;
	uint h = ( w >> 22u ) ^ w;
	return vec3( float( h & 255u ), float( ( h >> 8u ) & 255u ), float( ( h >> 16u ) & 255u ) ) * ( 0.75 / 255.0 ) + 0.25;
}

void main() {
	// hand depth test against the sealed scene depth (the overlay runs after the scene, with no depth attachment)
	ivec2 dsz = textureSize( u_currentDepth, 0 );
	float raw = texelFetch( u_currentDepth, clamp( ivec2( floor( gl_FragCoord.xy * u_depthTexRecip.xy * vec2( dsz ) ) ), ivec2( 0 ), dsz - 1 ), 0 ).x;
	if ( gl_FragCoord.z > raw + 2e-5 ) { discard; }

	if ( var_FluxScaleIndex.x <= 0.0 ) { discard; }							// a fixture switched OFF (RT_EMIT_DARK): not an emitter right now

	int  mode  = int( u_localParam0.x + 0.5 );
	uint slot  = uint( var_SlotFlags.x + 0.5 );
	uint flags = uint( var_SlotFlags.y + 0.5 ) << 16;
	if ( mode == 1 ) {
		vec3 c = dbgTint( slot );
		if ( ( flags & ( 1u << 16 ) ) != 0u ) { c = mix( c, vec3( 1.0 ), 0.35 * step( 0.5, fract( gl_FragCoord.x * 0.125 ) ) ); }	// animated colour: striped
		if ( ( flags & ( 1u << 21 ) ) != 0u ) { c = mix( c, vec3( 0.0 ), 0.5 * step( 0.5, fract( gl_FragCoord.x * 0.0625 ) ) * step( 0.5, fract( gl_FragCoord.y * 0.0625 ) ) ); }	// flare-inferred: checkered
		fragColor = vec4( c, 1.0 );
	} else if ( mode == 2 ) {
		float rel = var_FluxScaleIndex.x * u_localParam0.y;					// flux / meanFlux
		float t   = clamp( log2( max( rel, 1e-6 ) ) / 8.0 + 0.5, 0.0, 1.0 );	// 1/16 -> 0, 1 -> 0.5, 16 -> 1
		vec3  c   = ( t < 0.5 ) ? mix( vec3( 0.0, 0.0, 1.0 ), vec3( 0.0, 1.0, 0.0 ), t * 2.0 )
		          : ( t < 0.75 ) ? mix( vec3( 0.0, 1.0, 0.0 ), vec3( 1.0, 0.0, 0.0 ), ( t - 0.5 ) * 4.0 )
		          : mix( vec3( 1.0, 0.0, 0.0 ), vec3( 1.0 ), ( t - 0.75 ) * 4.0 );
		fragColor = vec4( c, 1.0 );
	} else if ( mode == 4 ) {
		fragColor = ( ( flags & ( 1u << 19 ) ) != 0u ) ? vec4( 0.1, 0.9, 0.2, 1.0 ) : vec4( 0.95, 0.15, 0.1, 1.0 );
	} else {
		vec3 e = ( slot != 0u ) ? textureLod( u_rtTextures[ nonuniformEXT( slot - 1u ) ], var_TexCoord, 0.0 ).rgb : vec3( 1.0 );
		if ( ( flags & ( 1u << 21 ) ) != 0u ) {										// flare-inferred: the diffuse through its threshold
			float th  = var_Tint.a;
			float lum = dot( e, vec3( 0.2126, 0.7152, 0.0722 ) );
			e *= clamp( ( lum - th ) / max( 1.0 - th, 1e-3 ), 0.0, 1.0 );
			fragColor = vec4( e * var_FluxScaleIndex.y, 1.0 );
		} else {
			fragColor = vec4( var_Tint.rgb * var_FluxScaleIndex.y, 1.0 );			// the MEAN radiance E2 lights with (flat; the vertex colour carries it)
		}
	}
}
