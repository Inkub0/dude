// DUDE RT shadow blur, tile classification (docs/rtx-shadow-blur.md). A penumbra only exists
// near a shadow EDGE, and edges cover a few percent of the screen - but the blur can't know that
// without looking, and looking is its whole cost (a 33-tap sweep). So the mask is summarized into
// 8x8-pixel tiles first, and the blur early-outs on ONE tile fetch.
//
//   u_localParam0.x = 0: pass 1, ray target -> tiles.  R = any pixel lit, G = any pixel shadowed
//                        with something to spread (half-width > 0)
//                   = 1: pass 2, tiles -> tiles.  R = 1 when the 5x5 tile neighbourhood holds
//                        BOTH - the widest kernel reaches 16 pixels = 2 tiles, so outside such a
//                        neighbourhood no pixel's value can change and the blur passes it through
#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_src;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	ivec2 tp = ivec2( gl_FragCoord.xy );
	ivec2 hi = textureSize( u_src, 0 ) - 1;
	float anyLit = 0.0, anyShadow = 0.0;
	if ( u_localParam0.x < 0.5 ) {
		for ( int y = 0; y < 8; y++ ) {
			for ( int x = 0; x < 8; x++ ) {
				ivec2 q = tp * 8 + ivec2( x, y );
				if ( q.x > hi.x || q.y > hi.y ) {
					continue;
				}
				vec4 t = texelFetch( u_src, q, 0 );
				if ( !( t.b > 0.0 ) ) {
					continue;					// never traced: neither
				}
				if ( t.r > 0.5 ) { anyLit = 1.0; }
				else if ( max( t.g, t.a ) > 0.35 ) { anyShadow = 1.0; }
			}
		}
		fragColor = vec4( anyLit, anyShadow, 0.0, 1.0 );
		return;
	}
	for ( int y = -2; y <= 2; y++ ) {
		for ( int x = -2; x <= 2; x++ ) {
			ivec2 q = tp + ivec2( x, y );
			if ( q.x < 0 || q.y < 0 || q.x > hi.x || q.y > hi.y ) {
				continue;
			}
			vec4 t = texelFetch( u_src, q, 0 );
			anyLit = max( anyLit, t.r );
			anyShadow = max( anyShadow, t.g );
		}
	}
	fragColor = vec4( anyLit * anyShadow, 0.0, 0.0, 1.0 );
}
