// DUDE ReSTIR DI — RESAMPLE pass (docs/rtx-restir-di.md). Per pixel: draw M candidate lights from the
// scene-light buffer, pick one by weighted reservoir sampling (WRS), trace ONE visibility ray for the
// pick (Stage 2), and write the RESERVOIR - the chosen light index + its unbiased contribution weight W
// + confidence M + visibility - to an RGBA16F target. A separate resolve pass (restir_di_resolve.frag)
// turns the reservoir into shaded radiance; the split is what lets Stage 3 reuse reservoirs across frames
// (temporal) before shading. Stage 3a writes the CURRENT-frame reservoir only (no reuse yet), so the
// resolved image equals Stage 2.
//
// The reservoir stores the light INDEX (not its radiance): the resolve - and, in Stage 3, a reprojected
// neighbour - re-evaluates the light at its own surface point, so a reused sample is shaded correctly.
//
// Stage 1 target: unshadowed diffuse luminance (albedo = 1). Visibility enters only on the chosen sample
// (Stage 2), so rad(y)*W*vis is still unbiased for the shadowed sum. Noisy by design (1-spp) until the
// temporal/spatial reuse + NRD of Stages 3-5. VK + RT hardware (ray_query); G-buffer reconstruction
// mirrors rtao_ray.frag.
//
// Uniform packing (RB_RHI_ReStirDIPass):
//   u_modelViewMatrix     = inverse view (view -> world)
//   u_color.xy            = scene-light buffer device address (bit-cast lo/hi)
//   u_color.z             = light count
//   u_rtParms.xy          = scene TLAS device address (bit-cast lo/hi; 0 = no RT scene -> unshadowed)
//   u_rtParms.z           = shadow-ray origin offset along the surface normal (world units)
//   u_rtParms.w           = max shadow-ray length
//   u_localParam0         = ( 1/proj00, 1/proj11, candidate count M, temporal confidence cap )
//   u_localParam1.x       = frame index (RNG decorrelation)
//   u_localParam1.z       = temporal reuse enabled + history valid this frame (Stage 3b; 0 = current only)
//   u_localParam1.w       = temporal disocclusion reject threshold (relative view-z; cures motion ghosting)
//   u_screenCorrection.xy = 1 / viewSize        (gl_FragCoord -> [0,1] uv; normal buffer)
//   u_depthTexRecip.xy    = gl_FragCoord -> _currentDepth tc
//   u_windowCoord.z       = view-Y sign (-1 Vulkan top-down)
//
// Output (RGBA16F reservoir): ( chosen light index | -1 if none, weight W, confidence M, visibility )
#extension GL_EXT_buffer_reference_uvec2 : require	// build the light-buffer pointer from u_color.xy (uvec2)
#extension GL_EXT_ray_query : require				// Stage 2: one visibility ray for the chosen light

#include "renderparms.glsl"
#include "rt_light_eval.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(1) uniform sampler2D u_normalBuffer;	// xyz = view-space normal, a = weapon/AO mask
SAMPLER_BINDING(2) uniform sampler2D u_prevReservoir;	// Stage 3b: last frame's reservoir (reprojected read)
SAMPLER_BINDING(3) uniform sampler2D u_velocity;		// Stage 3b: per-object screen velocity (reprojection)

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;	// RGBA16F reservoir: ( y|-1, W, M, vis )

// depth -> view z pair for THIS view (cinematics lower the near plane; renderparms.glsl u_depthParms)
#define depth_consts DUDE_DEPTH_CONSTS()

// invalid reservoir sentinel (no light chosen): index < 0
const vec4 RESERVOIR_EMPTY = vec4( -1.0, 0.0, 0.0, 0.0 );

// PCG hash; seed the stream, then draw successive [0,1) uniforms (advance-then-output).
float rnd( inout uint rng ) {
	rng = rng * 747796405u + 2891336453u;
	uint w = ( ( rng >> ( ( rng >> 28u ) + 4u ) ) ^ rng ) * 277803737u;
	w = ( w >> 22u ) ^ w;
	return float( w ) * ( 1.0 / 4294967296.0 );
}

// Stage 2: one hard any-occluder shadow ray from the surface toward the chosen light (the inline
// pattern r_rtAllLights / r_rtSunShadows use - interaction.frag's fast opaque path). 1 = lit, 0 =
// occluded. No TLAS (no RT scene) -> unshadowed (1): never trace against a null acceleration structure.
float traceVisibility( vec3 Pw, vec3 Nw, vec3 ldir, float dl ) {
	if ( u_rtParms.x == 0.0 && u_rtParms.y == 0.0 ) { return 1.0; }		// no RT scene
	float tMax = min( dl - 1.0, u_rtParms.w );
	if ( !( tMax > 0.0 ) ) { return 1.0; }
	rayQueryEXT rq;
	rayQueryInitializeEXT( rq,
		accelerationStructureEXT( uvec2( floatBitsToUint( u_rtParms.x ), floatBitsToUint( u_rtParms.y ) ) ),
		gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, 0xFDu /* skip perforated-caster instances */,
		Pw + Nw * u_rtParms.z, 0.0, ldir, tMax );
	while ( rayQueryProceedEXT( rq ) ) { }
	return ( rayQueryGetIntersectionTypeEXT( rq, true ) == gl_RayQueryCommittedIntersectionNoneEXT ) ? 1.0 : 0.0;
}

// resampling target pHat(li) at world point Pw/Nw: luminance of light li's UNSHADOWED diffuse
// contribution (Doom model, frustum-gated). Shared by the initial RIS and the re-evaluation of a
// reused (reprojected) sample at the current pixel - a reused light must be shaded for ITS surface.
float lightPHat( RtLights lights, uint li, vec3 Pw, vec3 Nw ) {
	vec3 ldir; float dl;
	dudeLightDir( lights, li, Pw, ldir, dl );
	float ndl = max( dot( Nw, ldir ), 0.0 );
	if ( ndl <= 0.0 ) { return 0.0; }
	float atten = dudeLightAtten( lights, li, Pw, dl, lights.l[ li ].flags, true, false );
	return dot( lights.l[ li ].color * ( atten * ndl ), vec3( 0.2126, 0.7152, 0.0722 ) );
}

// find the current buffer slot of the light with STABLE id `stableId` (flags bits 16..31). The light
// buffer is re-sorted + culled each frame, so a reused reservoir's light has moved; re-find it (or
// return NONE if it was culled this frame - a flickered-off light). nLights is small (per-view).
const uint LIGHT_NONE = 0xFFFFFFFFu;
uint findLightBuf( RtLights lights, uint nLights, uint stableId ) {
	for ( uint k = 0u; k < nLights; k++ ) {
		if ( ( lights.l[ k ].flags >> 16 ) == stableId ) { return k; }
	}
	return LIGHT_NONE;
}

void main() {
	vec2  frag = gl_FragCoord.xy;
	float raw  = texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;
	if ( raw >= 0.9994 ) { fragColor = RESERVOIR_EMPTY; return; }	// sky / no geometry
	vec4 nt = texture( u_normalBuffer, frag * u_screenCorrection.xy );
	// The view weapon is NOT skipped: masking it punched a MOVING hole in the reservoir, so the background
	// it uncovered as it swayed reset to fresh noise every frame (a noisy trailing edge). It participates
	// like any surface (its real near depth is stored, so a world pixel it later reveals is dropped by the
	// depth disocclusion test); the debug overlay superimposes the real weapon, so its own DI is never shown.

	// view-space position + normal (ssao/rtao reconstruction incl. the VK view-Y sign)
	float vz  = 1.0 / ( min( raw, 0.9994 ) * depth_consts.x + depth_consts.y );	// negative
	vec2  ndc = frag * ( u_screenCorrection.xy * 2.0 ) - 1.0;
	float d   = -vz;
	vec3  P   = vec3( ndc.x * d * u_localParam0.x, ndc.y * u_windowCoord.z * d * u_localParam0.y, vz );
	vec3  Nraw = nt.xyz * 2.0 - 1.0;
	float nl2  = dot( Nraw, Nraw );
	if ( !( nl2 > 1e-4 ) ) { fragColor = RESERVOIR_EMPTY; return; }	// also catches NaN
	vec3  N   = Nraw * inversesqrt( nl2 );

	// lift to world (u_modelViewMatrix = inverse view), like ssr_rt.frag / rtao_ray.frag
	vec3 Pw = ( u_modelViewMatrix * vec4( P, 1.0 ) ).xyz;
	vec3 Nw = normalize( mat3( u_modelViewMatrix ) * N );

	RtLights lights = RtLights( uvec2( floatBitsToUint( u_color.x ), floatBitsToUint( u_color.y ) ) );
	uint nLights = uint( u_color.z );
	if ( nLights == 0u ) { fragColor = RESERVOIR_EMPTY; return; }

	uint M = uint( max( u_localParam0.z, 1.0 ) );
	if ( M > nLights ) { M = nLights; }			// no point drawing more candidates than lights

	// RNG seed: pixel + frame, forced odd so the first advance never degenerates
	uint rng = ( ( uint( frag.x ) * 1973u + uint( frag.y ) * 9277u )
	             ^ ( uint( u_localParam1.x ) * 26699u ) ) | 1u;

	// --- initial candidates: weighted reservoir sampling over M uniform picks ---
	// Source pdf = 1/nLights, so a candidate's RIS weight is pHat/pdf = pHat*nLights.
	float wSum       = 0.0;
	float pHatChosen = 0.0;
	uint  liChosen   = 0u;
	for ( uint i = 0u; i < M; i++ ) {
		uint li = uint( rnd( rng ) * float( nLights ) );
		if ( li >= nLights ) { li = nLights - 1u; }		// guard the rare rnd()==1.0
		float pHat = lightPHat( lights, li, Pw, Nw );
		float w    = pHat * float( nLights );
		wSum += w;
		if ( wSum > 0.0 && rnd( rng ) < ( w / wSum ) ) {
			pHatChosen = pHat;
			liChosen   = li;
		}
	}

	// the current reservoir as a mergeable sample. It is identified by the light's STABLE id (flags bits
	// 16..31), not its volatile buffer slot, so temporal reuse survives the per-frame re-sort. combBuf
	// tracks the chosen light's CURRENT slot (for shading). Merge weight = pHat_c * W_c * M_c = wSum.
	float combWSum   = ( pHatChosen > 0.0 ) ? wSum : 0.0;
	float combM      = float( M );
	uint  combStable = ( pHatChosen > 0.0 ) ? ( lights.l[ liChosen ].flags >> 16 ) : 0u;
	uint  combBuf    = liChosen;
	float combPHat   = pHatChosen;
	bool  combValid  = ( pHatChosen > 0.0 );

	// --- Stage 3b: temporal reuse. Reproject in gl_FragCoord (DEVICE) space - the reservoir was written
	// at gl_FragCoord, so a var_TexCoord-built UV reads it vertically flipped. The reused sample is
	// identified by its stable light id and re-found in THIS frame's buffer (its slot moved / may be gone).
	if ( u_localParam1.z > 0.5 ) {
		vec2 vel = texelFetch( u_velocity, ivec2( frag ), 0 ).rg;	// this pixel's velocity (currUV - prevUV, +Y-up)
		vel.y = ( u_windowCoord.z < 0.0 ) ? -vel.y : vel.y;			// -> device (top-down) UV
		vec2 prevFrag = frag - vel / u_screenCorrection.xy;			// step back to last frame's pixel (UV -> pixels)
		ivec2 res = textureSize( u_prevReservoir, 0 );
		if ( prevFrag.x >= 0.0 && prevFrag.y >= 0.0
		     && prevFrag.x < float( res.x ) && prevFrag.y < float( res.y ) ) {
			vec4 pr = texelFetch( u_prevReservoir, ivec2( prevFrag ), 0 );	// nearest: id must not interpolate
			// disocclusion reject: the reprojected surface's stored view z (pr.w) must match this pixel's.
			// A newly-revealed surface - or the near weapon sweeping over the far world - stores a very
			// different z, so dropping it here is the cure for motion / weapon-edge ghosting.
			if ( pr.x >= 0.0 && abs( vz - pr.w ) <= u_localParam1.w * abs( vz ) ) {
				uint idP = uint( pr.x + 0.5 );						// prev sample's STABLE light id
				uint liP = findLightBuf( lights, nLights, idP );	// its slot THIS frame (or NONE if culled)
				if ( liP != LIGHT_NONE ) {
					float M_p    = min( abs( pr.z ), u_localParam0.w );	// |.z| = M (visibility is its sign)
					float pHat_p = lightPHat( lights, liP, Pw, Nw );
					float wp = pHat_p * pr.y * M_p;					// pHat_p * W_p * M_p
					combM += M_p;
					if ( wp > 0.0 ) {
						combWSum += wp;
						if ( combWSum > 0.0 && rnd( rng ) < ( wp / combWSum ) ) {
							combStable = idP; combBuf = liP; combPHat = pHat_p; combValid = true;
						}
					}
				}
			}
		}
	}

	if ( !( combValid && combPHat > 0.0 && combM > 0.0 ) ) { fragColor = RESERVOIR_EMPTY; return; }

	// unbiased combined weight W = combWSum / (combM * pHat(combBuf)); one visibility ray for the FINAL
	// chosen light (by its CURRENT slot). pHat stays UNSHADOWED, so rad*W*vis is unbiased for the shadowed sum.
	float W = combWSum / ( combM * combPHat );
	vec3 ldirF; float dlF;
	dudeLightDir( lights, combBuf, Pw, ldirF, dlF );
	float vis = ( ( lights.l[ combBuf ].flags & DUDE_LIGHT_NOSHADOWS ) != 0u )
		? 1.0 : traceVisibility( Pw, Nw, ldirF, dlF );

	// reservoir: ( STABLE light id, W, M*sign(visibility), this pixel's view z for disocclusion reject )
	fragColor = vec4( float( combStable ), W, ( vis > 0.5 ? combM : -combM ), vz );
}
