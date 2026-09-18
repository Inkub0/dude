// DUDE RTAO ray pass (docs/rtx-rtao.md H4a): one cosine-weighted hemisphere occlusion ray
// per pixel against the scene TLAS, written as NRD's normalized hit distance — the noisy
// input REBLUR_DIFFUSE_OCCLUSION denoises (H4c). Replaces the GTAO horizon search at the
// RT tier; the lower tiers keep screen-space GTAO.
//
// VK + RT hardware only (ray_query): loaded solely when the device supports KHR_ray_query
// and r_rtao is on (same gating idiom as ssr_rt.frag). View position reconstruction and
// the sky/weapon early-outs mirror ssao.frag; world lift mirrors ssr_rt.frag.
//
// Uniform packing (RB_RHI_RtaoPass):
//   u_rtParms.xy          = TLAS device address (bit-cast lo/hi)
//   u_modelViewMatrix     = inverse view (view -> world)
//   u_localParam0         = ( 1/proj00, 1/proj11, rayRadius, hitDistA )
//   u_localParam1         = ( noisePhase, hitDistB, normalOffset, 0 )
//   u_screenCorrection.xy = 1 / viewSize        (gl_FragCoord -> [0,1] uv)
//   u_depthTexRecip.xy    = gl_FragCoord -> _currentDepth tc
//   u_windowCoord.z       = view-Y sign (+1 GL, -1 Vulkan top-down)
#extension GL_EXT_ray_query : require

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(1) uniform sampler2D u_normalBuffer;   // xyz = view-space normal, a = weapon/AO mask

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

// Doom 3's fixed near / near-infinite far projection in GL clip depth -> linear eye z.
// Same constants as ssao.frag / ssr_rt.frag.
// depth -> view z pair of THIS view (renderparms.glsl u_depthParms): cinematics quarter the near
// plane, so it is not the constant ( 0.33333333, -0.33316667 ) it used to be hard-coded as
#define depth_consts DUDE_DEPTH_CONSTS()

// interleaved gradient noise (ssao.frag) — per-pixel dither, rotated per frame by the
// golden-ratio phase so the denoiser's temporal accumulation sees fresh sample directions
float ign( vec2 p ) {
	return fract( 52.9829189 * fract( dot( p, vec2( 0.06711056, 0.00583715 ) ) ) );
}

// branchless orthonormal basis around n (Duff et al. / Frisvad)
void onb( vec3 n, out vec3 t, out vec3 b ) {
	float s = n.z >= 0.0 ? 1.0 : -1.0;
	float a = -1.0 / ( s + n.z );
	float c = n.x * n.y * a;
	t = vec3( 1.0 + s * n.x * n.x * a, s * c, -s * n.x );
	b = vec3( c, s + n.y * n.y * a, -n.y );
}

void main() {
	vec2  frag = gl_FragCoord.xy;
	float raw  = texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;
	if ( raw >= 0.9994 ) {						// sky / no geometry -> unoccluded
		fragColor = vec4( 1.0 );
		return;
	}
	vec4 nt = texture( u_normalBuffer, frag * u_screenCorrection.xy );
	if ( nt.a < 0.5 ) {							// view weapon: skip AO (mirrors ssao.frag)
		fragColor = vec4( 1.0 );
		return;
	}

	// view-space position (ssao.frag reconstruction, incl. the VK view-Y sign) + normal.
	// HANG GUARD: a degenerate G-buffer normal would normalize() to NaN, and a ray query
	// launched with a NaN direction is undefined behaviour that can hang RT traversal
	// outright (driver kills the context: Xid 109 / VK_ERROR_DEVICE_LOST). Any such pixel
	// reads unoccluded instead of tracing.
	float vz  = 1.0 / ( min( raw, 0.9994 ) * depth_consts.x + depth_consts.y );   // negative
	vec2  ndc = frag * ( u_screenCorrection.xy * 2.0 ) - 1.0;
	float d   = -vz;
	vec3  P   = vec3( ndc.x * d * u_localParam0.x, ndc.y * u_windowCoord.z * d * u_localParam0.y, vz );
	vec3  Nraw = nt.xyz * 2.0 - 1.0;
	float nl2  = dot( Nraw, Nraw );
	if ( !( nl2 > 1e-4 ) ) {					// also catches NaN (comparisons with NaN are false)
		fragColor = vec4( 1.0 );
		return;
	}
	vec3  N   = Nraw * inversesqrt( nl2 );

	// lift to world (u_modelViewMatrix = inverse view), like ssr_rt.frag
	vec3 Pw = ( u_modelViewMatrix * vec4( P, 1.0 ) ).xyz;
	vec3 Nw = normalize( mat3( u_modelViewMatrix ) * N );

	// one cosine-weighted hemisphere direction; two decorrelated IGN streams, both
	// rotated by the per-frame golden-ratio phase (localParam1.x)
	float phase = u_localParam1.x;
	float r1 = fract( ign( frag ) + phase );
	float r2 = fract( ign( frag.yx + vec2( 37.0, 17.0 ) ) + phase );
	float phi  = 6.28318530718 * r1;
	float sinT = sqrt( r2 );
	float cosT = sqrt( 1.0 - r2 );
	vec3 T, B;
	onb( Nw, T, B );
	vec3 dir = T * ( cos( phi ) * sinT ) + B * ( sin( phi ) * sinT ) + Nw * cosT;
	float dl2 = dot( dir, dir );
	if ( !( dl2 > 1e-6 ) ) {					// same NaN/degenerate guard for the ray itself
		fragColor = vec4( 1.0 );
		return;
	}
	dir *= inversesqrt( dl2 );

	// closest hit inside the AO radius (opaque only). Origin biased along the normal so
	// the surface never self-occludes (localParam1.z, world units).
	float radius = u_localParam0.z;
	rayQueryEXT rq;
	rayQueryInitializeEXT( rq,
		accelerationStructureEXT( uvec2( floatBitsToUint( u_rtParms.x ), floatBitsToUint( u_rtParms.y ) ) ),
		gl_RayFlagsOpaqueEXT, 0xFFu, Pw + Nw * u_localParam1.z, 0.0, dir, radius );
	while ( rayQueryProceedEXT( rq ) ) { }

	// NRD REBLUR normalized hit distance: saturate(hitT / (A + |viewZ|*B)) — MUST match the
	// ReblurHitDistanceParameters set on the denoiser (docs/rtx-rtao.md; roughness = 1 for
	// diffuse makes the C term identity). Miss -> 1.0 (unoccluded).
	float nhd = 1.0;
	if ( rayQueryGetIntersectionTypeEXT( rq, true ) == gl_RayQueryCommittedIntersectionTriangleEXT ) {
		float hitT = rayQueryGetIntersectionTEXT( rq, true );
		nhd = clamp( hitT / ( u_localParam0.w + d * u_localParam1.y ), 0.0, 1.0 );
	}
	fragColor = vec4( nhd, 0.0, 0.0, 1.0 );
}
