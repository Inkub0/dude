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
//   u_localParam1         = ( frameIndex (hashed for the per-frame IGN reorientation), hitDistB, normalOffset, 0 )
//   u_screenCorrection.xy = 1 / viewSize        (gl_FragCoord -> [0,1] uv)
//   u_depthTexRecip.xy    = gl_FragCoord -> _currentDepth tc
//   u_windowCoord.z       = view-Y sign (+1 GL, -1 Vulkan top-down)
#extension GL_EXT_ray_query : require

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(1) uniform sampler2D u_normalBuffer;   // xyz = view-space normal, a = weapon/AO mask

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;
// NRD guide inputs, written by THIS pass (it already fetches depth + the G-buffer normal for every pixel)
// instead of two more fullscreen passes (the former rtao_viewz / rtao_pack shaders, removed 2026-09-21):
layout(location = 1) out vec4 outViewZ;		// IN_VIEWZ: signed linear view z (negative), R16F
layout(location = 2) out vec4 outNormal;	// IN_NORMAL_ROUGHNESS: world normal, NRD encoding 2 (signed octahedron), RGBA16F

// Doom 3's fixed near / near-infinite far projection in GL clip depth -> linear eye z.
// Same constants as ssao.frag / ssr_rt.frag.
// depth -> view z pair of THIS view (renderparms.glsl u_depthParms): the game lowers the near plane
// for cinematic cameras, so it is not the constant ( 0.33333333, -0.33316667 ) it used to be hard-coded as
#define depth_consts DUDE_DEPTH_CONSTS()

// branchless orthonormal basis around n (Duff et al. / Frisvad)
void onb( vec3 n, out vec3 t, out vec3 b ) {
	float s = n.z >= 0.0 ? 1.0 : -1.0;
	float a = -1.0 / ( s + n.z );
	float c = n.x * n.y * a;
	t = vec3( 1.0 + s * n.x * n.x * a, s * c, -s * n.x );
	b = vec3( c, s + n.y * n.y * a, -n.y );
}

// world normal -> NRD normal/roughness encoding 2 (NRD_NORMAL_ENCODING 2: signed octahedron, z sign in roughness)
vec4 packNrdNormal( vec3 Nraw ) {
	float nl2 = dot( Nraw, Nraw );
	vec3  n;
	if ( !( nl2 > 1e-4 ) ) {
		n = vec3( 0.0, 0.0, 1.0 );
	} else {
		n = normalize( mat3( u_modelViewMatrix ) * ( Nraw * inversesqrt( nl2 ) ) );	// world-space
	}
	n /= abs( n.x ) + abs( n.y ) + abs( n.z );
	vec3 r;
	r.y = n.y * 0.5 + 0.5;
	r.x = n.x * 0.5 + r.y;
	r.y -= n.x * 0.5;
	float roughness = 1.0;										// can't be 0: z carries n.z's sign
	float s = n.z < 0.0 ? -roughness : roughness;
	r.z = s * 0.5 + 0.5;
	return vec4( r, 0.0 );										// w = materialID/3 = 0
}

void main() {
	vec2  frag = gl_FragCoord.xy;
	float raw  = texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;
	// guides first, for EVERY pixel (sky, weapon and degenerate-normal early-outs included) - exactly what
	// the two standalone guide passes wrote
	vec4 ntGuide = texture( u_normalBuffer, frag * u_screenCorrection.xy );
	outViewZ  = vec4( 1.0 / ( min( raw, 0.9994 ) * depth_consts.x + depth_consts.y ), 0.0, 0.0, 1.0 );
	outNormal = packNrdNormal( ntGuide.xyz * 2.0 - 1.0 );
	if ( raw >= 0.9994 ) {						// sky / no geometry -> unoccluded
		fragColor = vec4( 1.0 );
		return;
	}
	vec4 nt = ntGuide;
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

	// one cosine-weighted hemisphere direction. r1/r2 = a Cranley-Patterson-rotated plastic-constant (Roberts)
	// R2 low-discrepancy sequence, identical to the PT GI bounce (rtgi_bounce.glsl): each pixel gets its OWN R2
	// pair advanced by the frame index, phase-shifted by a per-pixel white-noise offset so neighbouring pixels
	// are DECORRELATED - no coherent structure for NRD's spatial filter to keep. This REPLACED interleaved
	// gradient noise (IGN), whose intrinsic 45-degree diagonal survived NRD and, once the denoised AO was
	// upscaled from a reduced r_rtaoResScale, showed as thick oblique lines on small far details. The per-frame
	// IGN REORIENTATION only pointed that residue a different way each frame rather than removing the lattice.
	// u_localParam1.x = frame index (frameCount & 0xFFFF), the same bounded counter the GI bounce advances by.
	uvec2 ip = uvec2( frag );
	uint  h  = ( ip.x * 747796405u + 2891336453u );
	h = ( ( h >> ( ( h >> 28u ) + 4u ) ) ^ h ) * 277803737u; h = ( h >> 22u ) ^ h;
	h = ( ( h + ip.y ) * 747796405u + 2891336453u );
	h = ( ( h >> ( ( h >> 28u ) + 4u ) ) ^ h ) * 277803737u; h = ( h >> 22u ) ^ h;
	vec2  cp = vec2( float( h & 0xFFFFu ), float( ( h >> 16 ) & 0xFFFFu ) ) * ( 1.0 / 65536.0 );
	float r1 = fract( cp.x + u_localParam1.x * 0.7548776662466927 );	// + n / plastic
	float r2 = fract( cp.y + u_localParam1.x * 0.5698402909980532 );	// + n / plastic^2
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
	// Ray origin. Lifting it off the surface along the (bump-mapped) NORMAL leaks at every concave crease:
	// within the lift distance of an adjoining face - sooner where the normal map's bevel tilts the normal
	// toward it - the origin lands BEHIND that face, world faces are single-sided, the hemisphere looks into
	// the void and the rays miss: a thin WHITE line exactly where the contact should be darkest. Pull the
	// origin back toward the EYE instead (u_localParam1.w per unit depth): the camera sees this point, so the segment
	// back to it is unobstructed by construction and the origin can never be inside geometry. The pull-back
	// grows with depth like the depth buffer's own error; a tiny normal lift remains against self-hits.
	vec3  eyeW   = ( u_modelViewMatrix * vec4( 0.0, 0.0, 0.0, 1.0 ) ).xyz;
	vec3  rayOrg = Pw + normalize( eyeW - Pw ) * ( 0.25 + d * u_localParam1.w ) + Nw * 0.05;
	float radius = u_localParam0.z;
	rayQueryEXT rq;
	rayQueryInitializeEXT( rq,
		accelerationStructureEXT( uvec2( floatBitsToUint( u_rtParms.x ), floatBitsToUint( u_rtParms.y ) ) ),
		gl_RayFlagsOpaqueEXT, 0xFDu /* solid rays: skip perforated-caster instances (mask 0x02), only an alpha-testing ray may hit those */, rayOrg, 0.0, dir, radius );
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
