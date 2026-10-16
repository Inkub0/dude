// DUDE RT light-grid debug overlay (docs/rtx-light-grid.md L1, r_rtLightGridDebug): for every screen pixel reconstruct
// the world point the consumer would use (the same G-buffer reconstruction as the cache debug / restir_di_shade) and
// show its light-grid cell:
//   1 = CASCADE: a tint per cascade index (which cascade covers this point), black beyond the last cascade (fallback);
//   2 = COUNT: candidates stored in the cell over the slot count - black = empty cell (the consumer falls back), white = full;
//   3 = DOMINANT LIGHT: a stable tint of the cell's highest-importance stored light (neighbouring cells should agree);
//   4 = IMPORTANCE: the cell's sumImp, tonemapped x/(1+x).
// Opaque, written into the pre-tonemap HDR scene so values near 1 read bright.
//
// Uniform packing (RB_RHI_LightGridDebugOverlay):
//   u_modelViewMatrix     = inverse view (view -> world); the camera world pos is [3].xyz
//   u_localParam0         = ( 1/proj00, 1/proj11, -, mode )
//   u_screenCorrection.xy = 1 / viewSize ; u_depthTexRecip.xy = 1 / depth upload size ; u_windowCoord.z = view-Y sign
//   u_occlusionParms      = grid vec4 (gridAddr lo/hi, cellBase, res|cascades<<8|slots<<16) - rtgi_light_grid.glsl lgParmsFromVec4
//   unit 0 = sealed scene depth (POT), unit 1 = view-space normal G-buffer
#extension GL_EXT_buffer_reference_uvec2 : require

#include "renderparms.glsl"
#include "rtgi_light_grid.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(1) uniform sampler2D u_normalBuffer;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

#define depth_consts DUDE_DEPTH_CONSTS()

// a stable tint in [0.25,1] for an integer id (cascade index, light index)
vec3 lgTint( uint id ) {
	uint h = lgPcg( id * 2654435761u + 1u );
	return vec3( float( h & 255u ), float( ( h >> 8u ) & 255u ), float( ( h >> 16u ) & 255u ) ) * ( 0.75 / 255.0 ) + 0.25;
}

void main() {
	vec2  frag = gl_FragCoord.xy;
	ivec2 dsz  = textureSize( u_currentDepth, 0 );
	float raw  = texelFetch( u_currentDepth, clamp( ivec2( floor( frag * u_depthTexRecip.xy * vec2( dsz ) ) ), ivec2( 0 ), dsz - 1 ), 0 ).x;
	if ( raw >= 0.9994 ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }		// sky
	float vz  = 1.0 / ( min( raw, 0.9994 ) * depth_consts.x + depth_consts.y );
	float d   = -vz;
	vec2  ndc = frag * ( u_screenCorrection.xy * 2.0 ) - 1.0;
	vec3  P   = vec3( ndc.x * d * u_localParam0.x, ndc.y * u_windowCoord.z * d * u_localParam0.y, vz );
	vec3  Pw  = ( u_modelViewMatrix * vec4( P, 1.0 ) ).xyz;
	vec3  eye = u_modelViewMatrix[3].xyz;

	LgParms p = lgParmsFromVec4( u_occlusionParms, eye );
	int  mode = int( u_localParam0.w + 0.5 );

	ivec3 cell;
	int   c = lgPickCascade( Pw, p.camPos, p.cellBase, p.res, p.cascades, cell );
	if ( c < 0 ) { fragColor = vec4( 0.03, 0.0, 0.06, 1.0 ); return; }				// beyond the grid: the consumer falls back (dim magenta)

	if ( mode == 1 ) {
		// distinct hue per cascade so the nesting reads clearly: 0 red, 1 green, 2 blue, 3 yellow
		vec3 casc[4] = vec3[4]( vec3( 0.9, 0.2, 0.2 ), vec3( 0.2, 0.9, 0.2 ), vec3( 0.2, 0.4, 0.95 ), vec3( 0.9, 0.9, 0.2 ) );
		fragColor = vec4( casc[ clamp( c, 0, 3 ) ], 1.0 );
		return;
	}

	LgGrid grid = LgGrid( p.gridAddr );
	int    base = int( lgCellIndex( c, cell, p.res ) * lgStride( p.slots ) );
	uint   n    = lgCount( grid, base );
	if ( n == 0u ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }				// empty cell (no in-range light): black

	if ( mode == 2 ) {
		float ramp = clamp( float( n ) / max( float( p.slots ), 1.0 ), 0.0, 1.0 );
		fragColor = vec4( vec3( ramp ), 1.0 );
	} else if ( mode == 3 ) {
		uint  bestLight = 0u;
		float bestImp = -1.0;
		for ( uint s = 0u; s < n; s++ ) {
			float imp = lgSlotImp( grid, base, s );
			if ( imp > bestImp ) { bestImp = imp; bestLight = lgSlotLight( grid, base, s ); }
		}
		fragColor = vec4( lgTint( bestLight ), 1.0 );
	} else {
		float s = lgSumImp( grid, base );
		fragColor = vec4( vec3( s / ( 1.0 + s ) ), 1.0 );
	}
}
