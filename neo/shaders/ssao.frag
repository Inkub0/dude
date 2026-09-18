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
//   u_depthTexRecip.z     = SSAO Phase 1 depth-mip max LOD (0 = feature off, raw path)
//   u_depthTexRecip.w     = depth-mip LOD bias (r_ssaoDepthMipBias)

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(1) uniform sampler2D u_normalBuffer;   // DUDE normal G-buffer (view-space, encoded)
// SSAO Phase 1 (docs/ssao-perf-optimization.md): prefiltered LINEAR view-space depth
// mip chain (level 0 = ssao_depthmip.frag, coarser = box averages). The horizon march
// reads a coarser mip for farther steps — the cache win. R = positive linear eye depth.
// When the feature is off (u_depthTexRecip.z < 0.5) this is bound to _currentDepth as an
// unused dummy and the march falls back to the raw-depth path (viewPos).
SAMPLER_BINDING(2) uniform sampler2D u_linearDepthMip;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

#define M_PI       3.14159265358979
#define M_HALF_PI  1.57079632679490
#define MAX_SLICES 8
#define MAX_STEPS  12

// Doom 3's fixed near / near-infinite far projection in GL clip depth -> linear
// eye z. Same constants the soft-particle pass uses (see softparticle.frag).
// depth -> view z pair of THIS view (renderparms.glsl u_depthParms): cinematics quarter the near
// plane, so it is not the constant ( 0.33333333, -0.33316667 ) it used to be hard-coded as
#define depth_consts DUDE_DEPTH_CONSTS()

float rawDepth( vec2 frag ) {
	return texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;
}

// view-space position from a fragment + its (raw) depth. Eye looks down -z, so the
// returned z is negative. Split from rawDepth so the center pixel can reuse the depth
// it already fetched instead of sampling twice.
vec3 viewPosFromRaw( vec2 frag, float raw ) {
	float vz  = 1.0 / ( min( raw, 0.9994 ) * depth_consts.x + depth_consts.y );   // negative
	vec2  ndc = frag * ( u_screenCorrection.xy * 2.0 ) - 1.0;
	float d   = -vz;                                                              // positive depth
	// u_windowCoord.z = view-Y sign: +1 on GL (gl_FragCoord.y is bottom-up, agrees with
	// view +Y), -1 on Vulkan (gl_FragCoord.y is top-down). Without it the reconstructed
	// position Y is flipped vs the G-buffer normal, wrecking AO on floors/ceilings.
	return vec3( ndc.x * d * u_localParam0.x, ndc.y * u_windowCoord.z * d * u_localParam0.y, vz );
}

vec3 viewPos( vec2 frag ) {
	return viewPosFromRaw( frag, rawDepth( frag ) );
}

// SSAO Phase 1: view-space position of a horizon-march tap from the prefiltered LINEAR
// depth mip at an explicit LOD (coarser = farther steps). u_linearDepthMip.r holds the
// positive linear eye depth already, so no per-tap reconstruction division is needed.
vec3 viewPosLin( vec2 frag, float lod ) {
	float d   = textureLod( u_linearDepthMip, frag * u_screenCorrection.xy, lod ).r;   // positive
	vec2  ndc = frag * ( u_screenCorrection.xy * 2.0 ) - 1.0;
	return vec3( ndc.x * d * u_localParam0.x, ndc.y * u_windowCoord.z * d * u_localParam0.y, -d );
}

// One horizon-march occluder tap. With the depth mip on (u_depthTexRecip.z >= 0.5) read
// the mip at `lod`; otherwise the exact raw-depth path (an A/B toggle, r_ssaoDepthMip).
// The branch is on a draw-coherent uniform, so it costs nothing on the GPU.
vec3 viewPosStep( vec2 frag, float lod ) {
	if ( u_depthTexRecip.z >= 0.5 ) {
		return viewPosLin( frag, lod );
	}
	return viewPos( frag );
}

// Reconstruct a view-space normal from depth — the FALLBACK used when the normal G-buffer
// is off (main() reads the G-buffer directly when it's on, docs §4). Accurate 9-tap method
// (atyuwen, https://atyuwen.github.io/posts/normal-reconstruction/): for each axis sample two
// neighbours on EACH side and linearly extrapolate the near pair and the far pair toward the
// centre; the pair whose prediction matches the centre depth is the surface the centre lies on,
// so the derivative is taken from that side. This keeps the normal off the wrong surface at
// silhouettes / decals / thin geometry, where the old best-of-2 pick still smeared and made the
// reconstructed AO look faceted. Raw projection depth is affine in 1/viewZ (linear across a
// plane in screen space), so it's a valid space for the collinearity test.
vec3 sampleViewNormal( vec2 frag, vec3 P ) {
	float c = rawDepth( frag );

	float l1 = rawDepth( frag + vec2( -1.0, 0.0 ) );
	float l2 = rawDepth( frag + vec2( -2.0, 0.0 ) );
	float r1 = rawDepth( frag + vec2(  1.0, 0.0 ) );
	float r2 = rawDepth( frag + vec2(  2.0, 0.0 ) );
	float errL = abs( ( 2.0 * l1 - l2 ) - c );
	float errR = abs( ( 2.0 * r1 - r2 ) - c );
	vec3  dpdx = ( errL < errR ) ? ( P - viewPosFromRaw( frag + vec2( -1.0, 0.0 ), l1 ) )
	                             : ( viewPosFromRaw( frag + vec2(  1.0, 0.0 ), r1 ) - P );

	float u1 = rawDepth( frag + vec2( 0.0,  1.0 ) );
	float u2 = rawDepth( frag + vec2( 0.0,  2.0 ) );
	float d1 = rawDepth( frag + vec2( 0.0, -1.0 ) );
	float d2 = rawDepth( frag + vec2( 0.0, -2.0 ) );
	float errU = abs( ( 2.0 * u1 - u2 ) - c );
	float errD = abs( ( 2.0 * d1 - d2 ) - c );
	vec3  dpdy = ( errU < errD ) ? ( viewPosFromRaw( frag + vec2( 0.0,  1.0 ), u1 ) - P )
	                             : ( P - viewPosFromRaw( frag + vec2( 0.0, -1.0 ), d1 ) );

	vec3 N = normalize( cross( dpdx, dpdy ) );
	if ( dot( N, P ) > 0.0 ) {
		N = -N;                        // face the camera (P points away from eye)
	}
	return N;
}

// True if this fragment is the view weapon, per the AO mask stored in the normal
// G-buffer's alpha (gbuffer.frag writes 0 there, 1 elsewhere). Used to drop the weapon
// as an OCCLUDER during the horizon search: the gun's depth-hacked, pulled-close depth
// otherwise casts a false AO halo on the world behind it that slides/jitters as the
// weapon sways. Only sampled on the RAW-depth march (normal buffer bound, depth mip
// off): with the mip on, ssao_depthmip.frag bakes weapon texels as far depth instead,
// so the march never pays this scattered full-res fetch per tap.
bool weaponTexel( vec2 frag ) {
	return texture( u_normalBuffer, frag * u_screenCorrection.xy ).a < 0.5;
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

	vec3 P = viewPosFromRaw( frag, raw );        // reuse the depth we just fetched

	bool useNormalBuffer = u_windowCoord.x > 0.5;

	vec3 N;
	if ( useNormalBuffer ) {
		// normal G-buffer: xyz = bump-mapped view normal, a = AO mask. The mask is 0 on
		// the view weapon, whose depth-hacked depth confuses the horizon search into
		// reading far background geometry — skip SSAO there (leave it fully unoccluded).
		vec4 nt = texture( u_normalBuffer, frag * u_screenCorrection.xy );
		if ( nt.a < 0.5 ) {
			fragColor = vec4( 1.0, 0.5, 0.5, 1.0 );
			return;
		}
		N = normalize( nt.xyz * 2.0 - 1.0 );
	} else {
		N = sampleViewNormal( frag, P );         // depth-reconstruct fallback
	}
	vec3 V = normalize( -P );                    // toward the eye

	// per-tap weapon exclusion is only needed on the raw-depth march: the depth mip
	// (u_depthTexRecip.z >= 0.5) carries the weapon bake from ssao_depthmip.frag, so
	// there the mask fetch — a scattered full-res read per tap that defeated the mip's
	// cache win — is skipped entirely. Draw-coherent uniforms, free on the GPU.
	bool perTapMask = useNormalBuffer && u_depthTexRecip.z < 0.5;

	int   numSlices = int( u_localParam1.z );
	int   numSteps  = int( u_localParam1.y );
	float radius    = u_localParam0.z;
	float invR2     = 1.0 / ( radius * radius );
	bool  bentOn    = u_localParam1.w >= 0.5;    // coherent across the draw

	float pixelRadius = u_localParam1.x / max( -P.z, 1e-3 );   // radiusPixFactor / d
	pixelRadius = clamp( pixelRadius, 2.0, 512.0 );
	float stepPix = pixelRadius / float( numSteps );

	// hoisted loop invariants. u_windowCoord.y is a per-frame jitter phase (0 when
	// temporal accumulation is off): rotating the noise each frame makes the horizon
	// search sample different directions/steps, giving the temporal pass distinct
	// frames to average into a higher-quality result (docs/ssao-gtao.md §12). With it
	// 0 this is exactly the old ign() dither, so the non-temporal path is unchanged.
	float sliceStep = M_PI / float( numSlices );
	float noise     = fract( ign( frag ) + u_windowCoord.y );
	float noise05   = noise + 0.5;

	float visibility = 0.0;
	vec3  bentN      = vec3( 0.0 );
	float totalW     = 0.0;

	for ( int s = 0; s < MAX_SLICES; ++s ) {
		if ( s >= numSlices ) {
			break;
		}
		float phi = ( float( s ) + noise ) * sliceStep;
		vec2  dir = vec2( cos( phi ), sin( phi ) );

		// slice plane spanned by V and the screen-space direction; an in-plane tangent
		// (perpendicular to V, toward +dir). planeN is unit and perpendicular to V, so
		// cross(planeN, V) is already unit — no normalize needed. dir.y carries the same
		// view-Y sign (u_windowCoord.z) so the frag-space march maps to the right view dir.
		vec3 sliceDir = vec3( dir.x, dir.y * u_windowCoord.z, 0.0 );
		vec3 planeN   = normalize( cross( V, sliceDir ) );
		vec3 tangent  = cross( planeN, V );

		// horizon cosines relative to V on each side, distance-attenuated so far
		// occluders raise the horizon less than near ones
		float cH_pos = -1.0, cH_neg = -1.0;
		for ( int t = 0; t < MAX_STEPS; ++t ) {
			if ( t >= numSteps ) {
				break;
			}
			float distPix = ( float( t ) + noise05 ) * stepPix;
			vec2  duv     = dir * distPix;

			// SSAO Phase 1: read a coarser depth mip for farther steps (cache win). LOD
			// grows with the tap's screen distance; near taps stay on mip 0 where the
			// contact detail matters. depthTexRecip.z = max LOD (0 = off), .w = bias.
			float lod = clamp( log2( max( distPix * u_depthTexRecip.w, 1.0 ) ), 0.0, u_depthTexRecip.z );

			// Drop occluders that land on the view weapon so the depth-hacked gun never
			// darkens the world behind it (the source of the sway/move jitter). Checked
			// per tap only on the raw-depth march (perTapMask); the mip path bakes the
			// exclusion, and the depth-reconstruct fallback has no mask to consult.
			vec2  sp  = frag + duv;
			if ( !perTapMask || !weaponTexel( sp ) ) {
				vec3  Dp  = viewPosStep( sp, lod ) - P;
				float l2p = dot( Dp, Dp );
				if ( l2p > 1e-6 ) {
					float ca = dot( Dp, V ) * inversesqrt( l2p );
					float fo = clamp( 1.0 - l2p * invR2, 0.0, 1.0 );
					cH_pos = max( cH_pos, ca * fo );
				}
			}
			vec2  sn  = frag - duv;
			if ( !perTapMask || !weaponTexel( sn ) ) {
				vec3  Dn  = viewPosStep( sn, lod ) - P;
				float l2n = dot( Dn, Dn );
				if ( l2n > 1e-6 ) {
					float ca = dot( Dn, V ) * inversesqrt( l2n );
					float fo = clamp( 1.0 - l2n * invR2, 0.0, 1.0 );
					cH_neg = max( cH_neg, ca * fo );
				}
			}
		}

		// projected normal in the slice plane
		vec3  projN = N - planeN * dot( N, planeN );
		float pl2   = dot( projN, projN );
		if ( pl2 < 1e-8 ) {
			continue;
		}
		float invPl   = inversesqrt( pl2 );
		vec3  projNn  = projN * invPl;
		float projLen = pl2 * invPl;                 // == length( projN )

		// cos/sin of the projected-normal angle n are just its components in the
		// orthonormal (V, tangent) basis — projNn is unit and lies in that plane, so no
		// separate cos()/sin() calls are needed (atan still gives the angle for the clamp)
		float cosN = dot( projNn, V );
		float sinN = dot( projNn, tangent );
		float n    = atan( sinN, cosN );

		// signed horizon angles from V, clamped to the hemisphere around the normal
		float hPos = acos( clamp( cH_pos, -1.0, 1.0 ) );
		float hNeg = -acos( clamp( cH_neg, -1.0, 1.0 ) );
		hPos = n + min( hPos - n,  M_HALF_PI );
		hNeg = n + max( hNeg - n, -M_HALF_PI );

		// GTAO analytic inner integral (cosine-weighted visibility); both sides
		// summed with the shared cosN / sinN terms factored out
		float a = 0.25 * ( 2.0 * cosN + 2.0 * ( hPos + hNeg ) * sinN
		                 - cos( 2.0 * hPos - n ) - cos( 2.0 * hNeg - n ) );

		visibility += projLen * a;
		totalW     += projLen;

		// bent normal: mid-horizon direction (only computed if it will be used)
		if ( bentOn ) {
			float bAng = 0.5 * ( hPos + hNeg );
			bentN += ( V * cos( bAng ) + tangent * sin( bAng ) ) * projLen;
		}
	}

	float vis = ( totalW > 1e-4 ) ? ( visibility / totalW ) : 1.0;
	vis = clamp( vis, 0.0, 1.0 );
	vis = pow( vis, max( u_localParam0.w, 0.0 ) );      // intensity

	vec3 bn = N;
	if ( bentOn ) {
		float bl2 = dot( bentN, bentN );
		bn = ( bl2 > 1e-8 ) ? ( bentN * inversesqrt( bl2 ) ) : N;
	}

	fragColor = vec4( vis, bn * 0.5 + 0.5 );
}
