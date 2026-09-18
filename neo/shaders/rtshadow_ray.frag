// DUDE RT shadow blur, ray pass (r_rtShadowBlur, docs/rtx-shadow-blur.md): for ONE light that
// the engine already serves with hard ray-traced shadows (RT sun shadows, RT moving lights), trace
// that same shadow ray for every screen pixel of the light - toward the light's CENTRE, so it is
// deterministic - and record what the blur needs. The interaction shader then looks the blurred
// result up (mode 5) instead of tracing inline (mode 4): the ray count is the same, it just moves
// from the interaction draws to this pass.
//
// Output (RGBA16F):
//   R = visibility (1 lit / 0 occluded)
//   G = penumbra half-width on screen, HORIZONTAL, in pixels (0 = lit: nothing to spread)
//   B = view distance d (the blur's depth guide; 0 = never traced)
//   A = penumbra half-width on screen, VERTICAL, in pixels
// Half-width = lightRadius * dOccluder / ( dLight - dOccluder ): zero where the caster touches the
// receiver, growing with the gap - projected to pixels at this depth and foreshortened per screen
// axis by the receiver's geometric normal (a floor at a grazing angle blurs far less vertically).
//
// The ray origin is rebuilt from the depth buffer (mode 4 has the interpolated surface position),
// which needs two precautions mode 4 doesn't: the unprojection must include this frame's FSR2
// sub-pixel jitter (the projection's shear terms), and the origin is pulled toward the camera /
// pushed along the normal by the depth buffer's quantization error, which grows with distance -
// without both, far terrain self-shadows in a per-frame flicker.
//
// VK + RT hardware only (ray_query). NaN guards mirror rtao_ray.frag (a ray launched with a NaN
// origin/direction can hang RT traversal: Xid 109 / DEVICE_LOST).
//
// Uniform packing (RB_RHI_RtShadowBlurLight):
//   u_rtParms.xy          = TLAS device address (bit-cast lo/hi)
//   u_rtParms.z           = ray-origin normal offset (r_rtSunShadowOffset, shared with mode 4)
//   u_modelViewMatrix     = inverse view (view -> world)
//   u_localLightOrigin    = WORLD-space light origin
//   u_lightProjectionS/T/Q, u_lightFalloffS = the light's WORLD-space projection planes
//                           (vLight->lightProject[0..3]) - the light-volume test
//   u_localParam0         = ( 1/proj00, 1/proj11, lightRadius, world units -> pixels at view distance 1 )
//   u_localParam1         = ( max half-width in pixels, proj[8], proj[9], 0 )
//   u_screenCorrection.xy = 1 / viewSize
//   u_depthTexRecip.xy    = gl_FragCoord -> _currentDepth tc
#extension GL_EXT_ray_query : require

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(1) uniform sampler2D u_normalBuffer;   // xyz = view-space normal, a = weapon/AO mask

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

// Doom 3's fixed near / near-infinite far projection in GL clip depth -> linear eye z.
// Same constants as ssao.frag / rtao_ray.frag.
const vec2 depth_consts = vec2( 0.33333333, -0.33316667 );

void main() {
	fragColor = vec4( 1.0, 0.0, 0.0, 0.0 );		// every early-out = lit
	vec2  frag = gl_FragCoord.xy;
	float raw  = texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;
	if ( raw >= 0.9994 ) {						// sky / no geometry
		return;
	}
	vec4 nt = texture( u_normalBuffer, frag * u_screenCorrection.xy );
	if ( nt.a < 0.5 ) {
		// view weapon: its depth lives in the compressed depth-hack range, so the world position
		// can't be rebuilt from it. Weapon surfaces never sample the mask - they keep mode 4.
		return;
	}

	float vz  = 1.0 / ( min( raw, 0.9994 ) * depth_consts.x + depth_consts.y );   // negative
	float d   = -vz;
	fragColor.b = min( d, 65000.0 );			// lit or not, this pixel has a surface the blur can weigh
	vec2  ndc = frag * ( u_screenCorrection.xy * 2.0 ) - 1.0;
	// jitter-exact unprojection (VK: top-down window y, hence the sign): ndc.x = P00*x/d - P8
	vec3  P   = vec3( ( ndc.x + u_localParam1.y ) * d * u_localParam0.x,
	                  ( -ndc.y + u_localParam1.z ) * d * u_localParam0.y, vz );
	// depth quantization: ~2e-8 * d^2 world units along the view ray, doubled by the float32
	// cancellation in the linearization - err in FRONT of the surface, never behind it
	P *= max( 1.0 - 6e-8 * d * d / max( length( P ), 1.0 ), 0.5 );
	// geometric normal from the depth derivatives, taken before any further branching
	vec3  gN   = cross( dFdx( P ), dFdy( P ) );
	vec3  Nraw = nt.xyz * 2.0 - 1.0;
	float nl2  = dot( Nraw, Nraw );
	if ( !( nl2 > 1e-4 ) ) {
		return;
	}
	vec3 N  = Nraw * inversesqrt( nl2 );
	vec3 Pw = ( u_modelViewMatrix * vec4( P, 1.0 ) ).xyz;
	vec3 Nw = normalize( mat3( u_modelViewMatrix ) * N );

	// outside the light's volume nothing is lit by it, so nothing needs a shadow
	vec4  Pw4 = vec4( Pw, 1.0 );
	float q = dot( Pw4, u_lightProjectionQ );
	if ( !( q > 0.0 ) ) {
		return;
	}
	float s = dot( Pw4, u_lightProjectionS ) / q;
	float t = dot( Pw4, u_lightProjectionT ) / q;
	float f = dot( Pw4, u_lightFalloffS );
	const float M = 0.02;
	if ( s < -M || s > 1.0 + M || t < -M || t > 1.0 + M || f < -M || f > 1.0 + M ) {
		return;
	}

	vec3  O    = Pw + Nw * ( u_rtParms.z + 5e-4 * d );
	vec3  toL  = u_localLightOrigin.xyz - O;
	float dL2  = dot( toL, toL );
	if ( !( dL2 > 1.0 ) ) {						// on top of the light (also catches NaN)
		return;
	}
	float dL   = sqrt( dL2 );
	vec3  Ldir = toL / dL;

	// surfaces facing away from the light receive exactly zero from it (the interaction multiplies
	// by N.L): no ray. Bump normal AND geometric normal must agree, so only whole faces are skipped.
	float gLen = length( gN );
	vec3  gn   = ( gLen > 1e-6 ) ? gN / gLen : vec3( 0.0, 0.0, 1.0 );		// view space
	if ( dot( Nw, Ldir ) < -0.05 ) {
		vec3 gNw = mat3( u_modelViewMatrix ) * gn;
		if ( dot( gNw, Nw ) < 0.0 ) { gNw = -gNw; }
		if ( gLen > 1e-6 && dot( gNw, Ldir ) < -0.15 ) {
			return;
		}
	}

	// the mode-4 ray, except CLOSEST hit instead of first hit: the penumbra is sized by the
	// occluder nearest the receiver. Stops 1 unit short of the light, like mode 4.
	rayQueryEXT rq;
	rayQueryInitializeEXT( rq,
		accelerationStructureEXT( uvec2( floatBitsToUint( u_rtParms.x ), floatBitsToUint( u_rtParms.y ) ) ),
		gl_RayFlagsOpaqueEXT, 0xFFu, O, 0.0, Ldir, min( max( dL - 1.0, 0.01 ), 100000.0 ) );
	while ( rayQueryProceedEXT( rq ) ) { }
	if ( rayQueryGetIntersectionTypeEXT( rq, true ) != gl_RayQueryCommittedIntersectionTriangleEXT ) {
		return;									// reached the light: lit
	}

	float dOcc = rayQueryGetIntersectionTEXT( rq, true );
	float halfW = u_localParam0.z * dOcc / max( dL - dOcc, 1e-3 );
	float rPx  = min( halfW * u_localParam0.w / max( d, 1.0 ), u_localParam1.x );
	// per screen axis, the extent of a disc lying in the receiver's plane: r * sqrt( 1 - n.axis^2 )
	vec2  ext  = sqrt( max( vec2( 1.0 ) - gn.xy * gn.xy, vec2( 0.04 ) ) );
	fragColor.r = 0.0;
	fragColor.g = rPx * ext.x;
	fragColor.a = rPx * ext.y;
}
