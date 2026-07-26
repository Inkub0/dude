// DUDE GTAO ambient occlusion (docs/ssao-gtao.md). A horizon-search over
// _currentDepth produces a cosine-weighted ambient *visibility* term (stored in R)
// and a view-space *bent normal* (average unoccluded direction, stored in GBA).
// Ambient-only by design: the result is consumed in ambientlight.frag, never on
// direct lights, so it stays correct as lighting changes.
//
// Uniform packing (filled by RB_RHI_SSAOPass):
//   u_localParam0 = ( 1/proj00, 1/proj11, radiusWorld, intensity )
//   u_localParam1 = ( radiusPixelFactor, numSteps, numSlices, bentNormalEnable )
//   u_screenCorrection.xy = 1 / aoTargetSize       (gl_FragCoord -> [0,1] screen uv)
//   u_depthTexRecip.xy    = div / depthUploadSize  (gl_FragCoord -> _currentDepth tc)

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentDepth;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

#define M_PI       3.14159265358979
#define MAX_SLICES 8
#define MAX_STEPS  12

// Doom 3's fixed near / near-infinite far projection in GL clip depth -> linear
// eye z. Same constants the soft-particle pass uses (see softparticle.frag).
const vec2 depth_consts = vec2( 0.33333333, -0.33316667 );

float rawDepth( vec2 frag ) {
	return texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;
}

// view-space position at a fragment (eye looks down -z, so returned z is negative)
vec3 viewPos( vec2 frag ) {
	float raw = min( rawDepth( frag ), 0.9994 );
	float vz  = 1.0 / ( raw * depth_consts.x + depth_consts.y );   // negative
	vec2  uv  = frag * u_screenCorrection.xy;
	vec2  ndc = uv * 2.0 - 1.0;
	float d   = -vz;                                               // positive depth
	return vec3( ndc.x * d * u_localParam0.x, ndc.y * d * u_localParam0.y, vz );
}

// Reconstruct a view-space normal from depth. THE SWAPPABLE SEAM: a future normal
// G-buffer (normal mapping / POM / displacement) replaces just this function and
// everything downstream, incl. the bent normal, inherits it (docs/ssao-gtao.md §4).
// Uses the closer of each neighbour pair to avoid bleeding across silhouettes.
vec3 sampleViewNormal( vec2 frag, vec3 P ) {
	vec3 Pr = viewPos( frag + vec2( 1.0, 0.0 ) );
	vec3 Pl = viewPos( frag - vec2( 1.0, 0.0 ) );
	vec3 Pu = viewPos( frag + vec2( 0.0, 1.0 ) );
	vec3 Pd = viewPos( frag - vec2( 0.0, 1.0 ) );
	vec3 dx = ( abs( Pr.z - P.z ) < abs( Pl.z - P.z ) ) ? ( Pr - P ) : ( P - Pl );
	vec3 dy = ( abs( Pu.z - P.z ) < abs( Pd.z - P.z ) ) ? ( Pu - P ) : ( P - Pd );
	vec3 N = normalize( cross( dx, dy ) );
	if ( dot( N, P ) > 0.0 ) {
		N = -N;                        // face the camera (P points away from eye)
	}
	return N;
}

// interleaved gradient noise — cheap per-pixel dither for slice/step jitter
float ign( vec2 p ) {
	return fract( 52.9829189 * fract( dot( p, vec2( 0.06711056, 0.00583715 ) ) ) );
}

void main() {
	vec2  frag = gl_FragCoord.xy;
	float raw  = rawDepth( frag );
	if ( raw >= 0.9994 ) {                       // sky / no geometry -> unoccluded
		fragColor = vec4( 1.0, 0.5, 0.5, 1.0 );
		return;
	}

	vec3 P = viewPos( frag );
	vec3 N = sampleViewNormal( frag, P );
	vec3 V = normalize( -P );                    // toward the eye

	int   numSlices = int( u_localParam1.z );
	int   numSteps  = int( u_localParam1.y );
	float radius    = u_localParam0.z;
	float invR2     = 1.0 / ( radius * radius );

	float pixelRadius = u_localParam1.x / max( -P.z, 1e-3 );   // radiusPixFactor / d
	pixelRadius = clamp( pixelRadius, 2.0, 512.0 );
	float stepPix = pixelRadius / float( numSteps );

	float noise = ign( frag );

	float visibility = 0.0;
	vec3  bentN      = vec3( 0.0 );
	float totalW     = 0.0;

	for ( int s = 0; s < MAX_SLICES; ++s ) {
		if ( s >= numSlices ) {
			break;
		}
		float phi = ( float( s ) + noise ) * ( M_PI / float( numSlices ) );
		vec2  dir = vec2( cos( phi ), sin( phi ) );

		// slice plane spanned by V and the screen-space direction; an in-plane
		// tangent (perpendicular to V, roughly toward +dir)
		vec3 sliceDir = vec3( dir, 0.0 );
		vec3 planeN   = normalize( cross( V, sliceDir ) );
		vec3 tangent  = normalize( cross( planeN, V ) );

		// horizon cosines relative to V on each side, distance-attenuated so far
		// occluders raise the horizon less than near ones
		float cH_pos = -1.0, cH_neg = -1.0;
		for ( int t = 0; t < MAX_STEPS; ++t ) {
			if ( t >= numSteps ) {
				break;
			}
			float off = ( float( t ) + 0.5 + noise ) * stepPix;

			vec3  Dp  = viewPos( frag + dir * off ) - P;
			float l2p = dot( Dp, Dp );
			if ( l2p > 1e-6 ) {
				float ca = dot( Dp, V ) * inversesqrt( l2p );
				float fo = clamp( 1.0 - l2p * invR2, 0.0, 1.0 );
				cH_pos = max( cH_pos, ca * fo );
			}
			vec3  Dn  = viewPos( frag - dir * off ) - P;
			float l2n = dot( Dn, Dn );
			if ( l2n > 1e-6 ) {
				float ca = dot( Dn, V ) * inversesqrt( l2n );
				float fo = clamp( 1.0 - l2n * invR2, 0.0, 1.0 );
				cH_neg = max( cH_neg, ca * fo );
			}
		}

		// signed horizon angles from V within the slice plane
		float hPos =  acos( clamp( cH_pos, -1.0, 1.0 ) );    // +tangent side
		float hNeg = -acos( clamp( cH_neg, -1.0, 1.0 ) );    // -tangent side

		// projected normal in the slice plane; signed angle n from V
		vec3  projN   = N - planeN * dot( N, planeN );
		float projLen = length( projN );
		if ( projLen < 1e-4 ) {
			continue;
		}
		vec3  projNn = projN / projLen;
		float n = atan( dot( projNn, tangent ), dot( projNn, V ) );

		// clamp horizons to the hemisphere around the normal
		hPos = n + min( hPos - n,  0.5 * M_PI );
		hNeg = n + max( hNeg - n, -0.5 * M_PI );

		// GTAO analytic inner integral (cosine-weighted visibility), summed per side
		float cosN = cos( n );
		float sinN = sin( n );
		float a = 0.25 * ( -cos( 2.0 * hPos - n ) + cosN + 2.0 * hPos * sinN )
		        + 0.25 * ( -cos( 2.0 * hNeg - n ) + cosN + 2.0 * hNeg * sinN );

		visibility += projLen * a;
		totalW     += projLen;

		// bent normal: the mid-horizon direction in the slice plane
		float bAng = 0.5 * ( hPos + hNeg );
		bentN += ( V * cos( bAng ) + tangent * sin( bAng ) ) * projLen;
	}

	float vis = ( totalW > 1e-4 ) ? ( visibility / totalW ) : 1.0;
	vis = clamp( vis, 0.0, 1.0 );
	vis = pow( vis, max( u_localParam0.w, 0.0 ) );      // intensity

	vec3 bn = ( length( bentN ) > 1e-4 ) ? normalize( bentN ) : N;
	if ( u_localParam1.w < 0.5 ) {
		bn = N;                                          // bent normal disabled
	}

	fragColor = vec4( vis, bn * 0.5 + 0.5 );
}
