// DUDE RT irradiance cache debug overlay (docs/rtx-irradiance-cache.md C1, r_rtgiCacheDebug): for every screen pixel
// reconstruct the world point + normal the gather would hash (the same G-buffer reconstruction as
// restir_di_shade.glsl diSurfaceAt, at full res) and show its cache cell:
//   1 = CELL COLOUR: a stable tint per cell key (proves keying, levels and the staggered bins: cells must be
//       screen-space-constant in size, follow the geometry, and split at the normal-bin faces), dimmed where the
//       cell is not in the cache yet;
//   2 = CACHED VALUE: the cell's learned indirect term, tonemapped x / ( 1 + x ) (black = absent / unresolved);
//   3 = AGE: frames since the cell was last updated over the eviction age, white = about to be evictable, dark red =
//       not in the cache;
//   4 = N: the cell's history length over Nmax - black = NOT in the cache, 15 % grey = just created, white = converged
//       (the floor keeps "young" and "absent" apart: a cell re-cut at a finer level as you approach is young, a hole is
//       absent);
//   5 = ACTIVITY: red = the cell was RESET at its last resolve (the DI gradient saw its light switch), green = CREATED
//       or seeded from its coarser parent at its last resolve (a newly visible surface, a ring of cells re-cut at a
//       finer level while moving, an eviction), yellow = cooling down after a restart / HELD (further reset requests
//       inside the cooldown are a flickering light: the cell blends instead of restarting), grey ramp = steady (N over
//       Nmax), black = absent. In a static scene this view must settle to grey, with yellow only under flickering lights.
//   6 = SERVABLE (C6 tail mode): white where the cell is converged (N >= SEED_N) AND fresh (updated < evictAge ago), so a
//       tail-mode bounce hitting here would read the cell instead of shading live; dim grey otherwise. A static view goes
//       white in ~1 s as cells converge.
// Opaque; written into the pre-tonemap HDR scene like restir_di_debug.frag, so values near 1 read as bright.
//
// Uniform packing (RB_RHI_RtgiCacheDebugOverlay):
//   u_modelViewMatrix     = inverse view (view -> world)
//   u_localParam0         = ( 1/proj00, 1/proj11, Nmax, mode )
//   u_screenCorrection.xy = 1 / viewSize           (gl_FragCoord -> [0,1] uv; normal buffer)
//   u_depthTexRecip.xy    = 1 / depth upload size  (gl_FragCoord -> _currentDepth tc)
//   u_windowCoord.z       = view-Y sign (-1 Vulkan top-down)
//   cache parms           = rtgi_cache.glsl (u_channelMask / u_skinParms / u_contactParms; slot address in u_alphaTest.xy)
//   unit 0 = sealed scene depth (POT), unit 1 = bump-mapped view-space normal G-buffer (a = weapon mask)
#extension GL_EXT_buffer_reference_uvec2 : require

#include "renderparms.glsl"
#include "rt_hash_grid.glsl"
#include "rtgi_cache.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(1) uniform sampler2D u_normalBuffer;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

#define depth_consts DUDE_DEPTH_CONSTS()

void main() {
	vec2  frag = gl_FragCoord.xy;
	ivec2 dsz  = textureSize( u_currentDepth, 0 );
	ivec2 nsz  = textureSize( u_normalBuffer, 0 );
	float raw  = texelFetch( u_currentDepth, clamp( ivec2( floor( frag * u_depthTexRecip.xy * vec2( dsz ) ) ), ivec2( 0 ), dsz - 1 ), 0 ).x;
	vec4  nt   = texelFetch( u_normalBuffer, clamp( ivec2( floor( frag * u_screenCorrection.xy * vec2( nsz ) ) ), ivec2( 0 ), nsz - 1 ), 0 );
	if ( raw >= 0.9994 ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }		// sky
	float vz = 1.0 / ( min( raw, 0.9994 ) * depth_consts.x + depth_consts.y );
	float d  = -vz;
	vec2  ndc = frag * ( u_screenCorrection.xy * 2.0 ) - 1.0;
	vec3  P   = vec3( ndc.x * d * u_localParam0.x, ndc.y * u_windowCoord.z * d * u_localParam0.y, vz );
	vec3  Nraw = nt.xyz * 2.0 - 1.0;
	float nl2  = dot( Nraw, Nraw );
	if ( !( nl2 > 1e-4 ) ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }
	vec3 Pw  = ( u_modelViewMatrix * vec4( P, 1.0 ) ).xyz;
	vec3 Nw  = normalize( mat3( u_modelViewMatrix ) * ( Nraw * inversesqrt( nl2 ) ) );
	vec3 eye = u_modelViewMatrix[3].xyz;

	RtgiCacheParms cp = rtgiCacheParmsFromUniforms( uvec2( floatBitsToUint( u_alphaTest.x ), floatBitsToUint( u_alphaTest.y ) ) );
	if ( !rtgiCachePresent( cp ) ) { fragColor = vec4( 0.2, 0.0, 0.2, 1.0 ); return; }	// no cache buffers: magenta
	HgKey k  = hgMakeKey( Pw, Nw, eye, cp.tanAlphaHalf, cp.slotMask );
	RtgiCacheSpan span = RtgiCacheSpan( cp.slotAddr );
	uvec4 s0 = span.s[ k.base ], s1 = span.s[ k.base + 1u ], s2 = span.s[ k.base + 2u ], s3 = span.s[ k.base + 3u ];
	uvec4 hit;
	int  idx = rtgiCacheSpanFind( s0, s1, s2, s3, k.key, hit );
	int  mode = int( u_localParam0.w + 0.5 );
	if ( mode == 1 ) {
		fragColor = vec4( hgKeyColor( k.key ) * ( idx >= 0 ? 1.0 : 0.3 ), 1.0 );
		return;
	}
	if ( idx < 0 ) {
		fragColor = ( mode == 3 ) ? vec4( 0.25, 0.0, 0.0, 1.0 ) : vec4( 0.0, 0.0, 0.0, 1.0 );
		return;
	}
	HgSlot s = HgSlot( hit.x, hit.y, hit.z, hit.w );
	if ( mode == 2 ) {
		vec3 e = rtgiCacheUnpackIrr( s );
		fragColor = vec4( e / ( 1.0 + e ), 1.0 );
	} else if ( mode == 3 ) {
		float age = float( cp.frame - s.stamp );
		float ref = ( cp.evictAge > 0u ) ? float( cp.evictAge ) : 128.0;
		fragColor = vec4( vec3( clamp( age / ref, 0.0, 1.0 ) ), 1.0 );
	} else if ( mode == 4 ) {
		float ramp = clamp( float( rtgiCacheN( s ) ) / max( u_localParam0.z, 1.0 ), 0.0, 1.0 );
		fragColor = vec4( vec3( 0.15 + 0.85 * ramp ), 1.0 );
	} else if ( mode == 5 ) {
		uint  flags = rtgiCacheFlags( s );
		float ramp  = clamp( float( rtgiCacheN( s ) ) / max( u_localParam0.z, 1.0 ), 0.0, 1.0 );
		vec3  c     = vec3( 0.15 + 0.5 * ramp );
		if ( ( flags & RTGI_CACHE_RESET_FLAG ) != 0u ) { c = vec3( 1.0, 0.08, 0.08 ); }
		else if ( ( flags & ( RTGI_CACHE_FRESH_BIT | RTGI_CACHE_SEEDED_BIT ) ) != 0u ) { c = vec3( 0.1, 1.0, 0.1 ); }
		else if ( rtgiCacheCooldown( s ) > 0u ) { c = vec3( 0.9, 0.8, 0.1 ); }
		fragColor = vec4( c, 1.0 );
	} else {
		// 6 = SERVABLE (C6): white where this surface's cell would serve a tail-mode bounce hit (converged N >= SEED_N AND
		// fresh: last updated < evictAge ago), dim grey otherwise. Over a static view every lit surface's own cell converges
		// within ~SEED_N resolves, so the view goes white in ~1 s; a cell that cannot serve (young, or stale off-screen) shades live.
		fragColor = vec4( vec3( rtgiCacheServable( cp, hit, k.base + uint( idx ) ) ? 1.0 : 0.1 ), 1.0 );
	}
}
