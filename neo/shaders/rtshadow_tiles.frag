// DUDE RT shadow blur, tile classification (docs/rtx-shadow-blur.md). A penumbra only exists
// near a shadow EDGE, and only as far from it as that shadow's own half-width - typically a few
// pixels. The blur can't know either without looking, and looking is its whole cost. So the mask
// is summarized into 8x8-pixel tiles first; the blur then early-outs on ONE tile fetch, and where
// it does run it sweeps only as far as the widest shadow that can actually reach the tile.
//
//   u_localParam0.x = 0: pass 1, ray target -> tiles.
//                        R = any pixel lit, G = any pixel shadowed with something to spread,
//                        B = the widest half-width in the tile / HW_MAX
//                   = 1: pass 2, tiles -> tiles.
//                        G = the widest half-width among the tiles whose shadows REACH this tile
//                            (itself; a neighbour n tiles away only if its half-width spans the
//                            ( n - 1 ) * 8 pixel gap) / HW_MAX - the blur's sweep length
//                        (searched out to HW_MAX / 8 = 3 tiles - 49 fetches, at 1/64 of the pixels)
//                        R = 1 when such a shadow exists AND a lit pixel lies within that reach:
//                            anywhere else no pixel's value can change
#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_src;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

const float HW_MAX = 24.0;		// = K in rtshadow_blur.frag
const int   TILE_REACH = 3;		// HW_MAX / 8: how many tiles away the widest shadow still reaches

void main() {
	ivec2 tp = ivec2( gl_FragCoord.xy );
	ivec2 hi = textureSize( u_src, 0 ) - 1;
	if ( u_localParam0.x < 0.5 ) {
		// u_localParam0.y = 1: read the ray pass's RG8 SUMMARY (rtshadow_ray.frag out_summary), 2
		// bytes/texel (R = visibility, G = half-width / HW_MAX, G >= sentinel = never traced) instead
		// of the 8-byte RGBA16F ray target. 0 = the old path (reads the RGBA16F: B = traced flag).
		float anyLit = 0.0, maxHWn = 0.0;
		bool  fromSummary = u_localParam0.y > 0.5;
		for ( int y = 0; y < 8; y++ ) {
			for ( int x = 0; x < 8; x++ ) {
				ivec2 q = tp * 8 + ivec2( x, y );
				if ( q.x > hi.x || q.y > hi.y ) {
					continue;
				}
				vec4 t = texelFetch( u_src, q, 0 );
				if ( fromSummary ) {
					if ( t.g > 0.998 ) {
						continue;				// the untraced sentinel: neither lit nor shadowed
					}
					// the width is already normalised (/ HW_MAX) and 0 for a lit pixel, so a plain
					// max reproduces "max over shadowed pixels" without a separate visibility test
					if ( t.r > 0.001 ) { anyLit = 1.0; }
					maxHWn = max( maxHWn, t.g );
				} else {
					if ( !( t.b > 0.0 ) ) {
						continue;				// never traced: neither
					}
					// a grate's dimmed shadow (0 < r < 1) is both: light to spread into a darker
					// neighbour, and a shadow to spread into a lit one
					if ( t.r > 0.001 ) { anyLit = 1.0; }
					if ( t.r < 0.999 ) { maxHWn = max( maxHWn, max( t.g, t.a ) / HW_MAX ); }
				}
			}
		}
		// ceil to the 8-bit grid so the stored width never under-reports
		float hwEnc = ( maxHWn * HW_MAX > 0.35 ) ? ceil( min( maxHWn, 1.0 ) * 255.0 ) / 255.0 : 0.0;
		fragColor = vec4( anyLit, ( hwEnc > 0.0 ) ? 1.0 : 0.0, hwEnc, 1.0 );
		return;
	}
	float reachHW = 0.0;
	for ( int y = -TILE_REACH; y <= TILE_REACH; y++ ) {
		for ( int x = -TILE_REACH; x <= TILE_REACH; x++ ) {
			ivec2 q = tp + ivec2( x, y );
			if ( q.x < 0 || q.y < 0 || q.x > hi.x || q.y > hi.y ) {
				continue;
			}
			float hw  = texelFetch( u_src, q, 0 ).b * HW_MAX;
			float gap = float( max( max( abs( x ), abs( y ) ) - 1, 0 ) * 8 );
			if ( hw > gap ) {
				reachHW = max( reachHW, hw );
			}
		}
	}
	float anyLit = 0.0;
	if ( reachHW > 0.0 ) {
		int tr = clamp( int( ceil( reachHW / 8.0 ) ), 1, TILE_REACH );
		for ( int y = -tr; y <= tr; y++ ) {
			for ( int x = -tr; x <= tr; x++ ) {
				ivec2 q = clamp( tp + ivec2( x, y ), ivec2( 0 ), hi );
				anyLit = max( anyLit, texelFetch( u_src, q, 0 ).r );
			}
		}
	}
	fragColor = vec4( anyLit, reachHW / HW_MAX, 0.0, 1.0 );
}
