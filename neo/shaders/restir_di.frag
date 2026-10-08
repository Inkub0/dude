// DUDE ReSTIR DI gather — fresh 1-spp (RTGI architecture, docs/rtx-restir-di.md). Per pixel: mask the view
// weapon, importance-sample ONE light from the in-range set by weighted reservoir sampling over M random
// candidates, trace ONE visibility ray, and shade the unbiased direct irradiance rad(y)·W·vis. NO history,
// NO persistent reservoir, NO reprojection - like rtgi_ray.frag this traces fresh every frame and hands ALL
// temporal accumulation to NRD (+ the DI confidence pass for moving-light antilag). That is what kills the
// moving-object trails a hand-rolled reproject leaves.
//
// The weapon is NOT masked out here (unlike RTGI / RTAO / SSR): a moving hole in NRD's input left the world its
// silhouette uncovers each frame without history (a fried halo). Its pixels are shaded like any other and the
// apply / debug overlay mask them at display, so it never receives world DI on screen and never ghosts.
//
// Output is the 3-attachment NRD guide MRT (CreateRenderTargetRtgiMrt), exactly like rtgi_ray.frag so the
// shared REBLUR_DIFFUSE denoise (RtaoDenoise giMode) drops in unchanged:
//   loc 0 = NRD-YCoCg direct irradiance (rgb) + normalized hit distance (a)   [IN_DIFF_RADIANCE_HITDIST]
//   loc 1 = signed linear view z (negative)                                   [IN_VIEWZ]
//   loc 2 = world normal, NRD encoding 2 (signed octahedron)                  [IN_NORMAL_ROUGHNESS]
// The guides are written for EVERY pixel (incl. sky / weapon), the radiance is zeroed there - mirroring
// rtgi_ray.frag so NRD sees a valid guide field everywhere.
//
// Uniform packing (RB_RHI_ReStirDIPass):
//   u_modelViewMatrix     = inverse view (view -> world)
//   u_color.xy            = scene-light buffer device address (bit-cast lo/hi)
//   u_color.z             = light count
//   u_rtParms.xy          = scene TLAS device address (bit-cast lo/hi; 0 = no RT scene -> unshadowed)
//   u_rtParms.z           = shadow-ray origin offset along the surface normal (world units)
//   u_rtParms.w           = max shadow-ray length
//   u_localParam0         = ( 1/proj00, 1/proj11, candidate count M, hit-distance scale (NRD A = 0.5*this) )
//   u_localParam1.x       = frame index (RNG decorrelation)
//   u_localParam1.y       = r_pbr (apply mode: 1 = GGX metalness workflow as interaction.frag, 0 = Lambert only)
//   u_localParam1.z       = checkerboard parity (0 = off, 1 = even frame, 2 = odd; packs half the pixels left)
//   u_localParam1.w       = APPLY MODE (Stage D, r_rtReStirDIApply): shade with the G-buffer material (units 2/3)
//                           and write the DEMODULATED radiance (restir_di_common.glsl); 0 = irradiance, albedo 1
//   u_diffuseModifier     = apply mode: ( kd = 1 - r_pbrMetalDiffuse, r_pbrSpecScale, r_pbrFireflyClamp, r_pbrEnvScale )
//   u_screenCorrection.xy = 1 / viewSize        (gl_FragCoord -> [0,1] uv; normal buffer)
//   u_depthTexRecip.xy    = gl_FragCoord -> _currentDepth tc
//   u_windowCoord.x       = perforated casters: number of per-VIEW grate boxes at u_particleRadius.zw (0 = off)
//   u_windowCoord.y       = perforated casters: their shadow strength (r_shadowMapPerforatedStrength)
//   u_windowCoord.z       = view-Y sign (-1 Vulkan top-down)
//   u_windowCoord.w       = r_rtLightTextures: 1 = attenuate with each light's real cookie + falloff textures
//   u_particleRadius.xy   = geometry-table device address (bit-cast; grate candidates -> coverage texture)
//   u_particleRadius.zw   = per-view perforated box list device address (bit-cast; rt_perforated.glsl)
//   u_shadowFalloffS.x    = forward light scale (backEnd.lightScale): the DI term is scaled here, not at the apply
//   u_shadowFalloffS.y    = world size of one FULL-RES screen pixel per unit view depth (2/(proj00*viewW)): cookie LOD
//   --- r_rtReStirDICombineGI (RTGI bounce folded into THIS buffer, one NRD denoise; rtgi_bounce.glsl) ---
//   u_specularParms       = ( combine on/off, fog strength, fog coef (exp), skip fast lights in the bounce )   [fog: rtgi_apply's r_rtgiFogAttenuate]
//   u_pbrParms            = ( gather radius, r_rtgiEmissive, r_rtgiEmissiveClamp, r_rtgiLightScale )
//   u_pbrParms2           = ( ambient floor, shadow-ray gate, eye pullback per unit depth, r_rtgiIntensity )
//   u_occlusionParms      = ( IGN rotation cos, sin, IGN offset x, y )   [CPU-hoisted per frame, as RB_RHI_RtgiPass]
#extension GL_EXT_buffer_reference_uvec2 : require	// build the light-buffer pointer from u_color.xy (uvec2)
#extension GL_EXT_ray_query : require				// one visibility ray for the chosen light
#extension GL_EXT_nonuniform_qualifier : require		// bindless light textures (rt_light_tex.glsl)

#include "renderparms.glsl"
#include "rt_light_eval.glsl"
#include "rt_light_tex.glsl"
// grates / fences as shadow casters (same recipe as interaction.frag's inline ray)
#include "rt_perforated.glsl"
#include "restir_di_common.glsl"
// the RTGI bounce (trace + hit shading), shared with rtgi_ray.frag, for the combined DI+GI buffer
#include "rtgi_bounce.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(1) uniform sampler2D u_normalBuffer;	// xyz = view-space normal, a = weapon/AO mask (0 = weapon)
SAMPLER_BINDING(2) uniform sampler2D u_albedoBuffer;	// apply mode: G-buffer att3 receiver albedo (white when absent)
SAMPLER_BINDING(3) uniform sampler2D u_materialBuffer;	// apply mode: G-buffer att1 ( roughness, metalness, spec-map luminance )

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;	// IN_DIFF_RADIANCE_HITDIST: NRD-YCoCg direct irradiance (rgb) + normHitDist (a)
layout(location = 1) out vec4 outViewZ;		// IN_VIEWZ: signed linear view z (negative), R16F
layout(location = 2) out vec4 outNormal;	// IN_NORMAL_ROUGHNESS: world normal, NRD encoding 2 (signed octahedron), RGBA16F

// depth -> view z pair for THIS view (cinematics lower the near plane; renderparms.glsl u_depthParms)
#define depth_consts DUDE_DEPTH_CONSTS()

// PCG hash; seed the stream, then draw successive [0,1) uniforms (advance-then-output).
float rnd( inout uint rng ) {
	rng = rng * 747796405u + 2891336453u;
	uint w = ( ( rng >> ( ( rng >> 28u ) + 4u ) ) ^ rng ) * 277803737u;
	w = ( w >> 22u ) ^ w;
	return float( w ) * ( 1.0 / 4294967296.0 );
}

// NRD REBLUR radiance encoding (REBLUR_FrontEnd_PackRadianceAndNormHitDist -> _NRD_LinearToYCoCg): the
// REBLUR_DIFFUSE filter works and OUTPUTS in YCoCg, so IN_DIFF_RADIANCE_HITDIST must be YCoCg too. The
// debug/apply decodes with the exact inverse (restir_di_debug.frag). Same as rtgi_ray.frag.
vec3 nrdLinearToYCoCg( vec3 c ) {
	return vec3( dot( c, vec3( 0.25, 0.5, 0.25 ) ), dot( c, vec3( 0.5, 0.0, -0.5 ) ), dot( c, vec3( -0.25, 0.5, -0.25 ) ) );
}

// world normal -> NRD normal/roughness encoding 2 (signed octahedron, z sign in roughness). Same as rtgi_ray.frag.
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

// one hard any-occluder shadow ray from the surface toward the chosen light (interaction.frag's inline ray).
// 1 = lit, 0 = occluded, 1 - strength behind a solid grate texel. No TLAS (no RT scene) -> unshadowed: never
// trace a null acceleration structure.
//
// Perforated casters (r_rtPerforatedShadows, rt_perforated.glsl): the forward path decides per LIGHT whether a
// grate sits in its volume; here every pixel picks its own light, so the CPU hands over the boxes of every grate
// inside the union of the scene lights' volumes (u_particleRadius.zw, u_windowCoord.x of them) and a segment
// that passes through none of them keeps the force-opaque ray - the hardware's fastest traversal. Only a segment
// that can meet a grate traces the grate-aware ray and alpha-tests its candidates against the coverage texture.
// occDist (out) = distance to the occluder when the ray is blocked, else the distance to the light: the "hit
// distance" NRD scales its blur radius by. Feeding the LIGHT distance for every pixel told ReBLUR that every
// shadow came from a far source and widened the blur until grate / contact shadows vanished (user, 2026-10-08).
float traceVisibility( vec3 Pw, vec3 Nw, vec3 ldir, float dl, out float occDist ) {
	occDist = dl;
	if ( u_rtParms.x == 0.0 && u_rtParms.y == 0.0 ) { return 1.0; }		// no RT scene
	float tMax = min( dl - 1.0, u_rtParms.w );
	if ( !( tMax > 0.0 ) ) { return 1.0; }
	vec3  O = Pw + Nw * u_rtParms.z;
	uvec2 geoAddr = uvec2( floatBitsToUint( u_particleRadius.x ), floatBitsToUint( u_particleRadius.y ) );
	bool  perfOn  = u_windowCoord.x > 0.5 && u_windowCoord.y > 0.0 && ( geoAddr.x | geoAddr.y ) != 0u;
	if ( perfOn ) {
		perfOn = dudeSegmentMeetsPerforated(
			uvec2( floatBitsToUint( u_particleRadius.z ), floatBitsToUint( u_particleRadius.w ) ),
			int( u_windowCoord.x + 0.5 ), O, ldir, tMax );
	}
	if ( !perfOn ) {
		rayQueryEXT rqo;
		rayQueryInitializeEXT( rqo,
			accelerationStructureEXT( uvec2( floatBitsToUint( u_rtParms.x ), floatBitsToUint( u_rtParms.y ) ) ),
			gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, 0xFDu /* skip perforated-caster instances */,
			O, 0.0, ldir, tMax );
		while ( rayQueryProceedEXT( rqo ) ) { }
		if ( rayQueryGetIntersectionTypeEXT( rqo, true ) == gl_RayQueryCommittedIntersectionNoneEXT ) { return 1.0; }
		occDist = rayQueryGetIntersectionTEXT( rqo, true );
		return 0.0;
	}
	// grate-aware: perforated instances (mask 0x02) arrive as candidates; a solid texel dims to 1 - strength and
	// the ray carries on (min over grates, not a product - a candidate may be reported twice); opaque geometry
	// commits and terminates as before
	rayQueryEXT rq;
	rayQueryInitializeEXT( rq,
		accelerationStructureEXT( uvec2( floatBitsToUint( u_rtParms.x ), floatBitsToUint( u_rtParms.y ) ) ),
		gl_RayFlagsTerminateOnFirstHitEXT, 0xFFu, O, 0.0, ldir, tMax );
	float perfVis = 1.0;
	while ( rayQueryProceedEXT( rq ) ) {
		if ( perfVis == 1.0
				&& rayQueryGetIntersectionTypeEXT( rq, false ) == gl_RayQueryCandidateIntersectionTriangleEXT
				&& rayQueryGetIntersectionTEXT( rq, false ) < dl - 8.0
				&& dudePerforatedSolid( geoAddr,
					uint( rayQueryGetIntersectionInstanceCustomIndexEXT( rq, false ) ),
					uint( rayQueryGetIntersectionGeometryIndexEXT( rq, false ) ),
					uint( rayQueryGetIntersectionPrimitiveIndexEXT( rq, false ) ),
					rayQueryGetIntersectionBarycentricsEXT( rq, false ) ) ) {
			perfVis = 1.0 - u_windowCoord.y;
		}
	}
	if ( rayQueryGetIntersectionTypeEXT( rq, true ) == gl_RayQueryCommittedIntersectionNoneEXT ) { return perfVis; }
	occDist = rayQueryGetIntersectionTEXT( rq, true );
	return 0.0;
}

void main() {
	vec2  frag = gl_FragCoord.xy;

	// Checkerboard (r_rtReStirDICheckerboard, u_localParam1.z): trace only HALF the pixels at FULL res, packed
	// into the LEFT half of the buffer; NRD reconstructs the rest from the full-res guide pass + IN_MV. sfrag is
	// the SCREEN pixel this buffer cell stands for: 0 = off (sfrag == frag), 1 = even frame, 2 = odd (the traced
	// parity alternates each frame so every pixel refreshes every other frame). Mirrors rtgi_ray.frag.
	vec2  sfrag  = frag;
	int   cbMode = int( u_localParam1.z + 0.5 );
	if ( cbMode > 0 ) {
		float gx = floor( frag.x ), gy = floor( frag.y );
		float parity = mod( gy + float( cbMode - 1 ), 2.0 );
		sfrag = vec2( gx * 2.0 + parity + 0.5, gy + 0.5 );
	}
	float raw  = texture( u_currentDepth, sfrag * u_depthTexRecip.xy ).x;

	// NRD guides first, for EVERY pixel (mirrors rtgi_ray.frag) so the shared REBLUR_DIFFUSE denoise sees a
	// valid guide field even on sky / weapon / degenerate pixels (the radiance is zeroed there below). In
	// checkerboard mode these gather-written guides are ignored (NRD reads the full-res guide pass instead).
	float vz = 1.0 / ( min( raw, 0.9994 ) * depth_consts.x + depth_consts.y );	// negative view z
	vec4  nt = texture( u_normalBuffer, sfrag * u_screenCorrection.xy );
	outViewZ  = vec4( vz, 0.0, 0.0, 1.0 );
	outNormal = packNrdNormal( nt.xyz * 2.0 - 1.0 );

	// checkerboard packs traced samples into the LEFT HALF; the right half traces nothing (NRD reconstructs it)
	if ( cbMode > 0 && frag.x * u_screenCorrection.x >= 0.5 ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }
	if ( raw >= 0.9994 ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }	// sky / no geometry
	// The view weapon (nt.a < 0.5) is NOT masked here: a moving hole in the denoiser's input leaves the world its
	// silhouette uncovers each frame with no history (fried halo). Its pixels get a DI of their own (depth-hacked,
	// never displayed) so NRD sees a continuous field; the apply and the debug overlay mask it at DISPLAY.

	// view-space position + normal (ssao/rtao reconstruction incl. the VK view-Y sign)
	vec2  ndc = sfrag * ( u_screenCorrection.xy * 2.0 ) - 1.0;
	float d   = -vz;
	vec3  P   = vec3( ndc.x * d * u_localParam0.x, ndc.y * u_windowCoord.z * d * u_localParam0.y, vz );
	vec3  Nraw = nt.xyz * 2.0 - 1.0;
	float nl2  = dot( Nraw, Nraw );
	if ( !( nl2 > 1e-4 ) ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }	// also catches NaN
	vec3  N   = Nraw * inversesqrt( nl2 );

	// lift to world (u_modelViewMatrix = inverse view), like ssr_rt.frag / rtao_ray.frag
	vec3 Pw = ( u_modelViewMatrix * vec4( P, 1.0 ) ).xyz;
	vec3 Nw = normalize( mat3( u_modelViewMatrix ) * N );

	RtLights lights = RtLights( uvec2( floatBitsToUint( u_color.x ), floatBitsToUint( u_color.y ) ) );
	uint nLights = uint( u_color.z );
	if ( nLights == 0u ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }

	uint M = uint( max( u_localParam0.z, 1.0 ) );
	if ( M > nLights ) { M = nLights; }

	// Stage D apply mode: the full material response of THIS pixel, from the G-buffer, so the reservoir target
	// and the shaded sample are the forward interaction.frag equation (PBR branch: GGX metalness workflow with
	// the same conventions - no 1/pi, lobe clamp, dielectric-weighted spec scale, metal env floor, Fresnel-
	// weighted diffuse with the kd relax; no Toksvig since the G-buffer normal is unit). What is NOT here (v1):
	// the diffuse/specular stage colours + vertex colour (assumed 1), baked occlusion maps, the legacy non-PBR
	// specular LUT (Lambert only when r_pbr is off). The output is DEMODULATED by diDemodFactor (the apply
	// remultiplies), so NRD denoises a texture-free signal.
	const bool applyMode = u_localParam1.w > 0.5;
	const bool pbr       = u_localParam1.y > 0.5;
	vec3  albedo = vec3( 1.0 );
	vec3  B      = vec3( 1.0 );
	vec3  F0     = vec3( 0.04 );
	vec3  Vw     = vec3( 0.0 );
	vec3  envFloor = vec3( 0.0 );
	float metal = 0.0, kd = 0.0, alpha2 = 1.0, NdotV = 1.0, specGain = 0.0;
	if ( applyMode ) {
		albedo   = texture( u_albedoBuffer, sfrag * u_screenCorrection.xy ).rgb;
		vec3 mt  = texture( u_materialBuffer, sfrag * u_screenCorrection.xy ).rgb;
		float rough = clamp( mt.r, 0.03, 1.0 );
		metal    = mt.g;
		kd       = u_diffuseModifier.x;
		B        = diDemodFactor( albedo, metal, mt.b, kd, u_diffuseModifier.y );
		Vw       = normalize( u_modelViewMatrix[3].xyz - Pw );			// camera (inverse view translation) -> surface
		NdotV    = max( dot( Nw, Vw ), 1e-4 );
		float a  = rough * rough;
		alpha2   = a * a;
		F0       = mix( vec3( 0.04 ), albedo, metal );
		specGain = mt.b * mix( u_diffuseModifier.y, 1.0, metal ) * 2.0;	// spec map * dielectric-weighted scale * vanilla 2x
		envFloor = F0 * ( metal * u_diffuseModifier.w * ( 1.0 - 0.5 * rough ) ) * mt.b;	// Phase C.1 metal environment floor (rides the spec map)
	}

	// RNG seed: SCREEN pixel (sfrag, so a checkerboard cell is seeded by the pixel it represents) + frame,
	// forced odd so the first advance never degenerates
	uint rng = ( ( uint( sfrag.x ) * 1973u + uint( sfrag.y ) * 9277u )
	             ^ ( uint( u_localParam1.x ) * 26699u ) ) | 1u;

	// weighted reservoir sampling over M random candidates. Source pdf = uniform = 1/nLights, so a
	// candidate's RIS weight is pHat/pdf = pHat*nLights. pHat = luminance of the unshadowed contribution.
	// A cheap bounding-sphere cull skips lights that cannot reach the point. The reservoir is TRANSIENT
	// (this frame only) - NRD does the temporal accumulation downstream.
	float wSum        = 0.0;
	float pHatChosen  = 0.0;
	vec3  radChosen   = vec3( 0.0 );
	vec3  ldirChosen  = vec3( 0.0 );
	float dlChosen    = 0.0;
	uint  flagsChosen = 0u;
	for ( uint i = 0u; i < M; i++ ) {
		uint li = uint( rnd( rng ) * float( nLights ) );
		if ( li >= nLights ) { li = nLights - 1u; }		// guard the rare rnd()==1.0
		if ( !dudeLightInRange( lights, li, Pw ) ) { continue; }	// cheap volume-sphere cull
		if ( ( lights.l[ li ].flags & DUDE_LIGHT_DIFORWARD ) != 0u ) { continue; }	// fast-changing light: the forward path lights it (no history lag)

		vec3 ldir; float dl;
		dudeLightDir( lights, li, Pw, ldir, dl );
		float ndl = max( dot( Nw, ldir ), 0.0 );
		vec3  rad = vec3( 0.0 );
		float pHat = 0.0;
		if ( ndl > 0.0 ) {
			// the light's shape + colour: its real cookie * falloff textures (Stage G, u_windowCoord.w), else the
			// analytic frustum attenuation. Doom lighting model: no 1/pi.
			vec3 atten = ( u_windowCoord.w > 0.5 )
				? dudeLightAttenTex( lights, li, Pw, dl, lights.l[ li ].flags, true, false, d * u_shadowFalloffS.y )
				: vec3( dudeLightAtten( lights, li, Pw, dl, lights.l[ li ].flags, true, false ) );
			vec3 contrib = lights.l[ li ].color * ( atten * ndl );		// irradiance-like: colour * cookie/falloff * N.L
			if ( applyMode ) {
				// the material response for THIS light direction (interaction.frag's PBR combine, per unit light)
				vec3 f;
				if ( pbr ) {
					vec3  H     = normalize( ldir + Vw );
					float NdotH = max( dot( Nw, H ), 0.0 );
					float VdotH = max( dot( Vw, H ), 0.0 );
					vec3  F     = F0 + ( 1.0 - F0 ) * pow( 1.0 - VdotH, 5.0 );
					float dd    = NdotH * NdotH * ( alpha2 - 1.0 ) + 1.0;
					float D     = alpha2 / ( dd * dd );								// GGX NDF, pi folded out
					float k     = 0.5 * sqrt( alpha2 );
					float gvis  = 0.25 / ( ( ndl * ( 1.0 - k ) + k ) * ( NdotV * ( 1.0 - k ) + k ) );
					float lobe  = min( D * gvis, u_diffuseModifier.z );				// firefly clamp
					f = lobe * F * specGain + envFloor + albedo * ( ( 1.0 - F ) * ( 1.0 - metal * kd ) );
				} else {
					f = albedo;														// legacy: Lambert, no spec (v1)
				}
				contrib *= f;
			}
			pHat = dot( contrib, vec3( 0.2126, 0.7152, 0.0722 ) );		// luminance target on the SHADED contribution
			rad  = contrib / B;											// demodulated (B = 1 outside apply mode)
		}
		float w    = pHat * float( nLights );
		wSum += w;
		if ( wSum > 0.0 && rnd( rng ) < ( w / wSum ) ) {
			pHatChosen  = pHat;
			radChosen   = rad;
			ldirChosen  = ldir;
			dlChosen    = dl;
			flagsChosen = lights.l[ li ].flags;
		}
	}

	if ( !( pHatChosen > 0.0 ) && u_specularParms.x < 0.5 ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }	// no reaching light (and no bounce to add)

	// unbiased contribution weight W = (1/pHat(y))·(wSum/M); one visibility ray for the chosen light. pHat
	// stays UNSHADOWED, so rad(y)·W·vis is still unbiased for the shadowed sum. NOSHADOWS lights skip the ray.
	float W   = ( pHatChosen > 0.0 ) ? ( wSum / float( M ) ) / pHatChosen : 0.0;
	float occDist = dlChosen;
	float vis = ( !( pHatChosen > 0.0 ) || ( flagsChosen & DUDE_LIGHT_NOSHADOWS ) != 0u )
		? 1.0 : traceVisibility( Pw, Nw, ldirChosen, dlChosen, occDist );

	// the direct term, at the forward path's light scale (lightColor = lightScale * stage colour there)
	vec3 outRad = max( radChosen * ( W * vis * u_shadowFalloffS.x ), vec3( 0.0 ) );

	// r_rtReStirDICombineGI: the ONE-bounce indirect term from the same pixel, folded into the same buffer so ONE
	// NRD denoise serves direct + indirect (the bounce's own pass stands down). Finished exactly as rtgi_apply.frag
	// would: albedo (the receiver's, 1 outside apply mode) * Li * r_rtgiIntensity, faded through the view's fog
	// (pow( exp( depth * coef ), strength )), then demodulated by the same B as the direct term so the apply's
	// remultiply is exact for both. Its own hit distance is NOT fed to NRD (the direct term's light distance
	// drives the blur radius for both - the shared-denoise compromise).
	if ( u_specularParms.x > 0.5 ) {
		RtgiBounceParms bp;
		bp.tlas           = uvec2( floatBitsToUint( u_rtParms.x ), floatBitsToUint( u_rtParms.y ) );
		bp.geoTable       = uvec2( floatBitsToUint( u_particleRadius.x ), floatBitsToUint( u_particleRadius.y ) );
		bp.lightAddr      = uvec2( floatBitsToUint( u_color.x ), floatBitsToUint( u_color.y ) );
		bp.nLights        = nLights;
		bp.realLights     = nLights > 0u;
		bp.gate           = true;
		bp.texturedLights = u_windowCoord.w > 0.5;
		bp.shadowRay      = u_pbrParms2.y > 0.5;
		bp.skipForwardLights = u_specularParms.w > 0.5;	// r_rtReStirDIDynamicForward: a fast light has NO bounce rather than a trailing one
		bp.radius         = u_pbrParms.x;
		bp.emissiveScale  = u_pbrParms.y;
		bp.emissiveClamp  = u_pbrParms.z;
		bp.lightScale     = u_pbrParms.w;
		bp.ambientFloor   = u_pbrParms2.x;
		bp.eyePullback    = u_pbrParms2.z;
		bp.ignRot         = u_occlusionParms.xy;
		bp.ignOfs         = u_occlusionParms.zw;
		vec3 Li; float thit;
		if ( rtgiTraceBounce( bp, Pw, Nw, u_modelViewMatrix[3].xyz, d, sfrag, Li, thit ) == 1 ) {
			vec3 gi = Li * u_pbrParms2.w;
			if ( u_specularParms.y > 0.0 ) {
				gi *= pow( exp( d * u_specularParms.z ), u_specularParms.y );		// rtgi_apply's fog transmittance
			}
			outRad += max( albedo * gi, vec3( 0.0 ) ) / B;
		}
	}

	// NRD IN_DIFF_RADIANCE_HITDIST: the "hit distance" for direct lighting is the distance to the chosen
	// light - it drives ReBLUR's world-space blur radius (near light -> tight penumbra, far light -> wider),
	// the direct-lighting analogue of the gather-ray distance RTGI feeds. Normalized saturate( dl / (A + d*B) )
	// with A = 0.5 * u_localParam0.w, B = 0.1, MUST match the ReblurHitDistanceParameters in RB_RHI_ReStirDIPass.
	// occluder distance when shadowed (tight blur for near casters: grates, contact), light distance when lit
	float nhd = clamp( occDist / ( 0.5 * u_localParam0.w + d * 0.1 ), 0.0, 1.0 );
	fragColor = vec4( nrdLinearToYCoCg( outRad ), nhd );
}
