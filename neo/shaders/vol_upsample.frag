// DUDE volumetric lighting (docs/volumetric-lighting.md, V3): DEPTH-AWARE upsample of the low-res march to
// full resolution. A shaft in front of a far wall must not bleed onto a near railing (or the reverse): the
// four surrounding low-res samples are weighted by bilinear position AND by how close the depth each was
// marched to is to this full-res pixel's depth. Feeds the temporal accumulation.
//   unit 0 = low-res march (rgb in-scatter, a transmittance), unit 1 = scene depth
//   u_localParam0      = (lowW, lowH, full px per low-res texel, _)
//   u_depthTexRecip.xy = full-res px -> depth-texture tc
#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_volume;
SAMPLER_BINDING(1) uniform sampler2D u_currentDepth;

VARY(0) in vec2 var_TexCoord;
layout(location = 0) out vec4 fragColor;

#define depth_consts DUDE_DEPTH_CONSTS()

float viewDepth( vec2 fullPx ) {
	float raw = texture( u_currentDepth, fullPx * u_depthTexRecip.xy ).x;
	return -1.0 / ( raw * depth_consts.x + depth_consts.y );			// positive view depth
}

void main() {
	vec2  lowSize = u_localParam0.xy;
	float ratio   = u_localParam0.z;
	vec2  lowPos  = gl_FragCoord.xy / ratio - 0.5;						// position in low-res texel-centre space
	vec2  base    = floor( lowPos );
	vec2  f       = lowPos - base;
	float zc      = viewDepth( gl_FragCoord.xy );

	vec4  sum  = vec4( 0.0 );
	float wsum = 0.0;
	for ( int j = 0; j < 2; j++ ) {
		for ( int i = 0; i < 2; i++ ) {
			vec2  t  = clamp( base + vec2( i, j ), vec2( 0.0 ), lowSize - 1.0 );
			float wb = ( i == 0 ? 1.0 - f.x : f.x ) * ( j == 0 ? 1.0 - f.y : f.y );
			float zt = viewDepth( ( t + 0.5 ) * ratio );					// the depth that low-res pixel marched to
			float wd = 1.0 / ( 0.02 + abs( zt - zc ) / max( zc, 1.0 ) );	// relative depth similarity
			float w  = ( wb + 0.01 ) * wd;									// +eps: a lone depth-matching tap still wins
			sum  += texture( u_volume, ( t + 0.5 ) / lowSize ) * w;
			wsum += w;
		}
	}
	fragColor = sum / wsum;
}
