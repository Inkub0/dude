// DUDE RT emissive table debug overlay (docs/rtx-emissive-triangles.md E1d, r_rtEmissiveTableDebug). Draws the table's
// triangles over the finished view, depth-tested by hand against the sealed scene depth (unit 0) so a wall hides the
// emitters behind it while the emitters themselves - opaque fixtures at exactly the scene depth, pure-additive tubes
// that never wrote depth - pass. Modes (u_localParam0.x):
//   1 = TEXTURE: a stable tint per glow texture (which surfaces are in the table, and which material each belongs to);
//   2 = FLUX: the triangle's flux relative to the table's MEAN (u_localParam0.y = 1 / meanFlux), log ramp:
//       blue = 1/16 of the mean or fainter, green = the mean, red = 4x, white = 16x and above;
//   3 = EMITTED COLOUR: the emitter's MEAN radiance (glow average x stage colour x scale, flat - what E2 lights with).
//   6 / 7 = the Light Editor's look mode (editLights, docs/rtx-light-editor.md L2): 6 = light spheres + the highlighted
//       volume (vertex colour; normal.y = x-ray: drawn faint where the scene hides it), 7 = a surface tint (u_localParam1).
// Opaque (modes 1-3) / alpha-blended (6, 7), into the pre-tonemap HDR scene. (4 = light coverage and 5 = the placed lights
// by census class went with the census, 2026-10-31: look mode shows the lights.)
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
	int  mode  = int( u_localParam0.x + 0.5 );
	bool hidden = gl_FragCoord.z > raw + 2e-5;
	if ( mode == 6 ) {
		// L2 look mode gizmos (light spheres, the highlighted volume): vertex colour; x-ray (normal.y) = faint where hidden
		if ( hidden && var_FluxScaleIndex.y < 0.5 ) { discard; }
		fragColor = vec4( var_Tint.rgb, var_Tint.a * ( hidden ? 0.3 : 0.9 ) );
		return;
	}
	if ( hidden ) { discard; }
	if ( mode == 7 ) {
		fragColor = u_localParam1;												// L2 look mode surface tint (its authored state; dark fixtures included)
		return;
	}

	if ( var_FluxScaleIndex.x <= 0.0 ) { discard; }							// a fixture switched OFF (RT_EMIT_DARK): not an emitter right now

	uint slot  = uint( var_SlotFlags.x + 0.5 );
	uint flags = uint( var_SlotFlags.y + 0.5 ) << 16;
	if ( mode == 1 ) {
		vec3 c = dbgTint( slot );
		if ( ( flags & ( 1u << 16 ) ) != 0u ) { c = mix( c, vec3( 1.0 ), 0.35 * step( 0.5, fract( gl_FragCoord.x * 0.125 ) ) ); }	// animated colour: striped
		fragColor = vec4( c, 1.0 );
	} else if ( mode == 2 ) {
		float rel = var_FluxScaleIndex.x * u_localParam0.y;					// flux / meanFlux
		float t   = clamp( log2( max( rel, 1e-6 ) ) / 8.0 + 0.5, 0.0, 1.0 );	// 1/16 -> 0, 1 -> 0.5, 16 -> 1
		vec3  c   = ( t < 0.5 ) ? mix( vec3( 0.0, 0.0, 1.0 ), vec3( 0.0, 1.0, 0.0 ), t * 2.0 )
		          : ( t < 0.75 ) ? mix( vec3( 0.0, 1.0, 0.0 ), vec3( 1.0, 0.0, 0.0 ), ( t - 0.5 ) * 4.0 )
		          : mix( vec3( 1.0, 0.0, 0.0 ), vec3( 1.0 ), ( t - 0.75 ) * 4.0 );
		fragColor = vec4( c, 1.0 );
	} else {
		fragColor = vec4( var_Tint.rgb * var_FluxScaleIndex.y, 1.0 );				// the MEAN radiance E2 lights with (flat; the vertex colour carries it; every entry is uniform now)
	}
}
