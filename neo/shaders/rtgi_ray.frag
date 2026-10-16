// DUDE RT GI ray pass (docs/rtx-rtgi.md H6a): one cosine-weighted hemisphere ray per pixel against
// the scene TLAS - but instead of RTAO's occlusion bit, it SHADES the hit cheaply and returns the
// incoming radiance (one indirect diffuse bounce). The ray setup, cosine-hemisphere direction, world
// reconstruction and NRD guides are identical to rtao_ray.frag; the hit-shading is lifted from
// ssr_rt.frag (geo-table fetch -> bindless albedo -> scene-light loop -> emissive).
//
// H6a is a bring-up slice: the raw radiance is only shown by r_rtgiDebug 1. Nothing consumes it in
// normal play yet - the NRD DIFFUSE denoise is H6b, the ambient apply + fake-fill reconcile is H6c,
// the one-dominant-light shadow ray is H6d - so with r_rtgi 0 (default) this pass never runs and the
// shipped RTAO path is untouched. VK + RT hardware only (ray_query).
//
// Uniform packing (RB_RHI_RtgiPass):
//   u_rtParms.xy          = TLAS device address (bit-cast lo/hi)
//   u_rtParms.zw          = geo-table device address (bit-cast lo/hi)   [vs rtao_ray: NEW]
//   u_modelViewMatrix     = inverse view (view -> world)
//   u_localParam0         = ( 1/proj00, 1/proj11, gatherRadius, emissive peak clamp )
//   u_localParam1         = ( frameIndex (unused in-shader since the IGN rotation is CPU-hoisted), shadow-ray gate, checkerboard parity, eye-pullback per unit depth )
//   u_particleRadius      = ( cos(goldenAngle), sin(goldenAngle), hashed per-frame IGN offset x, y )  [CPU-hoisted, frame-constant]
//   u_color               = ( scene-light buffer addr lo/hi bit-cast, light count, ambient floor at the hit )
//   u_diffuseModifier     = ( emissive glow scale, light brightness scale, frustum gate, real-lights enable )
//   u_screenCorrection.xy = 1 / raySize   (gl_FragCoord -> [0,1] uv)
//   u_depthTexRecip.xy    = gl_FragCoord -> _currentDepth tc
//   u_windowCoord.z       = view-Y sign (-1 Vulkan top-down)
//   u_windowCoord.w       = r_rtLightTextures: 1 = attenuate the hit with each light's real cookie + falloff textures
//   --- r_rtgiCache (the RT irradiance cache, docs/rtx-irradiance-cache.md; rtgi_cache.glsl's shared packing) ---
//   u_alphaTest.xy        = cache SLOT buffer device address (bit-cast; 0 = no cache)   [u_particleRadius is the IGN here]
//   u_channelMask         = ( accumulation buffer address lo, hi, slot mask, tan( cell angle / 2 ) )
//   u_skinParms           = ( cached-term scale, frame stamp, flags [1 update, 2 lookup], - )
//   u_contactParms        = ( evict age, per-sample clamp, albedo clamp, - )
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference_uvec2 : require	// construct the geo-table pointer from u_rtParms.zw (uvec2)
#extension GL_EXT_nonuniform_qualifier : require		// RR4: nonuniformEXT index into the bindless texture array

#include "renderparms.glsl"
// scene-light buffer row + evaluation (shared with RT reflections + volumetric lighting)
#include "rt_light_eval.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(1) uniform sampler2D u_normalBuffer;   // xyz = view-space normal, a = weapon/AO mask

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;	// rgb = gathered indirect radiance; a = normalized hit distance (H6b: NRD IN_DIFF_RADIANCE_HITDIST)
layout(location = 1) out vec4 outViewZ;		// IN_VIEWZ: signed linear view z (negative), R16F
layout(location = 2) out vec4 outNormal;	// IN_NORMAL_ROUGHNESS: world normal, NRD encoding 2 (signed octahedron), RGBA16F

// RR4 bindless materials: the resident texture set as one array, indexed by (texIndex - 1). Set 2 is
// bound once per frame by the backend and populated by SyncBindlessSlot. nonuniformEXT because each
// gather ray can hit a different surface. Declared by rt_light_tex.glsl (which also samples the scene
// lights' cookie / falloff textures from it, Stage G).
#include "rt_light_tex.glsl"
// the RT irradiance cache the bounce hit reads and the primary point feeds (docs/rtx-irradiance-cache.md)
#include "rt_hash_grid.glsl"
#include "rtgi_cache.glsl"
// the bounce itself (trace + hit shading), shared with the ReSTIR DI combined gather
#include "rtgi_bounce.glsl"

// Doom 3's fixed near / near-infinite far projection in GL clip depth -> linear eye z (negative).
#define depth_consts DUDE_DEPTH_CONSTS()

// NRD REBLUR radiance encoding (REBLUR_FrontEnd_PackRadianceAndNormHitDist -> _NRD_LinearToYCoCg):
// REBLUR_DIFFUSE filters and OUTPUTS in YCoCg, so its IN_DIFF_RADIANCE_HITDIST input must be YCoCg too.
// rtgi_apply decodes with the exact inverse. (Feeding linear here shifted the denoised result toward red.)
vec3 nrdLinearToYCoCg( vec3 c ) {
	return vec3( dot( c, vec3( 0.25, 0.5, 0.25 ) ), dot( c, vec3( 0.5, 0.0, -0.5 ) ), dot( c, vec3( -0.25, 0.5, -0.25 ) ) );
}

// world normal -> NRD normal/roughness encoding 2 (signed octahedron, z sign in roughness)
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
	// H6g checkerboard: when on, each packed pixel stands for a screen pixel 2*gx + (row+frame) parity at the
	// RTGI resolution (rw x rh, set by r_rtgiResScale) — NRD reconstructs the other half. u_localParam1.z = 0
	// off (resScale path: sfrag == frag), 1 = on even frame, 2 = on odd frame. sfrag is in rw x rh space, and
	// u_screenCorrection (1/rw) / u_depthTexRecip ((w/rw)/uploadW) map it to the buffer + full-res depth just
	// like the resScale path — so checkerboard and resScale compose.
	vec2  sfrag = frag;
	int   cbMode = int( u_localParam1.z + 0.5 );
	if ( cbMode > 0 ) {
		float gx = floor( frag.x ), gy = floor( frag.y );
		float parity = mod( gy + float( cbMode - 1 ), 2.0 );
		sfrag = vec2( gx * 2.0 + parity + 0.5, gy + 0.5 );
	}
	float raw  = texture( u_currentDepth, sfrag * u_depthTexRecip.xy ).x;
	// NRD guides first, for EVERY pixel (mirrors rtao_ray.frag), so H6b can drop the denoise in unchanged.
	// (In checkerboard mode these are ignored — the full-res rtgi_guides pass feeds NRD instead — but writing
	// them is harmless and keeps the resScale path identical.)
	vec4 ntGuide = texture( u_normalBuffer, sfrag * u_screenCorrection.xy );
	outViewZ  = vec4( 1.0 / ( min( raw, 0.9994 ) * depth_consts.x + depth_consts.y ), 0.0, 0.0, 1.0 );
	outNormal = packNrdNormal( ntGuide.xyz * 2.0 - 1.0 );
	// checkerboard: the packed samples live in the LEFT HALF only (NRD reconstructs the rest), so the right
	// half traces nothing — this is where the ray saving comes from (half the pixels, at full resolution).
	if ( cbMode > 0 && frag.x * u_screenCorrection.x >= 0.5 ) { fragColor = vec4( 0.0 ); return; }
	// sky / the view weapon / a degenerate normal gather no indirect bounce -> zero radiance
	if ( raw >= 0.9994 ) { fragColor = vec4( 0.0 ); return; }
	vec4 nt = ntGuide;
	if ( nt.a < 0.5 ) { fragColor = vec4( 0.0 ); return; }

	// view-space position (ssao.frag reconstruction, incl. the VK view-Y sign) + normal. A degenerate
	// G-buffer normal would normalize() to NaN and a NaN ray direction can hang RT traversal (device lost),
	// so any such pixel returns zero radiance instead of tracing.
	float vz  = outViewZ.x;		// identical to the expression written into outViewZ above (negative view z)
	vec2  ndc = sfrag * ( u_screenCorrection.xy * 2.0 ) - 1.0;
	float d   = -vz;
	vec3  P   = vec3( ndc.x * d * u_localParam0.x, ndc.y * u_windowCoord.z * d * u_localParam0.y, vz );
	vec3  Nraw = nt.xyz * 2.0 - 1.0;
	float nl2  = dot( Nraw, Nraw );
	if ( !( nl2 > 1e-4 ) ) { fragColor = vec4( 0.0 ); return; }
	vec3  N   = Nraw * inversesqrt( nl2 );

	// lift to world (u_modelViewMatrix = inverse view)
	vec3 Pw = ( u_modelViewMatrix * vec4( P, 1.0 ) ).xyz;
	vec3 Nw = normalize( mat3( u_modelViewMatrix ) * N );

	// the bounce (rtgi_bounce.glsl): parms from this pass's uniform packing
	RtgiBounceParms bp;
	bp.tlas           = uvec2( floatBitsToUint( u_rtParms.x ), floatBitsToUint( u_rtParms.y ) );
	bp.geoTable       = uvec2( floatBitsToUint( u_rtParms.z ), floatBitsToUint( u_rtParms.w ) );
	bp.lightAddr      = uvec2( floatBitsToUint( u_color.x ), floatBitsToUint( u_color.y ) );
	bp.nLights        = uint( u_color.z );
	bp.realLights     = u_diffuseModifier.w > 0.5;
	bp.gate           = u_diffuseModifier.z > 0.5;
	bp.texturedLights = u_windowCoord.w > 0.5;
	bp.shadowRay      = u_localParam1.y > 0.5;
	bp.skipForwardLights = false;			// RTGI's own pass bounces every light (no forward/DI split here)
	bp.radius         = u_localParam0.z;
	bp.emissiveScale  = u_diffuseModifier.x;
	bp.emissiveClamp  = u_localParam0.w;
	bp.lightScale     = u_diffuseModifier.y;
	bp.ambientFloor   = u_color.w;
	bp.eyePullback    = u_localParam1.w;
	bp.ignRot         = u_particleRadius.xy;
	bp.ignOfs         = u_particleRadius.zw;
	// the irradiance cache (r_rtgiCache): the hit reads its cell, this pixel then teaches its own cell what it saw.
	// This pass bounces the fast (DIFORWARD) lights too, so only LiCache - without them - is learned.
	bp.cache          = rtgiCacheParmsFromUniforms( uvec2( floatBitsToUint( u_alphaTest.x ), floatBitsToUint( u_alphaTest.y ) ) );
	const bool cacheOn = rtgiCachePresent( bp.cache );
	bp.cacheLookup    = cacheOn && bp.cache.lookup;
	const vec3 eyeW   = u_modelViewMatrix[3].xyz;
	vec3  Li; float thit; vec3 bounceDir; vec3 LiCache;
	int   hit = rtgiTraceBounce( bp, Pw, Nw, eyeW, d, sfrag, Li, thit, bounceDir, LiCache );
	if ( cacheOn && bp.cache.update && hit != 2 ) {
		rtgiCacheUpdate( bp.cache, Pw, Nw, eyeW, ( hit == 1 ) ? LiCache : vec3( 0.0 ) );		// a miss teaches 0; an unshadeable row (2) teaches nothing
	}
	if ( hit == 0 ) { fragColor = vec4( 0.0 ); return; }
	if ( hit == 2 ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }
	float radius = bp.radius;

	// H6b: normalized hit distance in alpha for the NRD REBLUR_DIFFUSE denoise — saturate( thit / (A + |viewZ|*B) ),
	// A = 0.5*radius, B = 0.1, MUST match the ReblurHitDistanceParameters set in RB_RHI_RtgiPass. Radiance is
	// packed to NRD's YCoCg in .rgb (rtgi_apply unpacks); max() guards negatives into the denoiser.
	float nhd = clamp( thit / ( 0.5 * radius + d * 0.1 ), 0.0, 1.0 );
	fragColor = vec4( nrdLinearToYCoCg( max( Li, vec3( 0.0 ) ) ), nhd );
}
