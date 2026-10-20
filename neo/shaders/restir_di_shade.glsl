// DUDE ReSTIR DI shared shading (docs/rtx-restir-di.md Plan R1). The gather (restir_di.frag) and the gradient pass
// (restir_di_gradient.frag) must shade a light at a surface BIT-FOR-BIT the same way - the gradient is "the same
// light, re-shaded at the reprojected surface under this frame's lights", and any drift between the two would read
// as a lighting change and reset history. So the surface reconstruction, the material response, the light
// evaluation and the visibility ray live here, once. Both shaders bind the same units 0-3 and share the gather's
// uniform packing (restir_di.frag header); the gradient adds its own units / uniforms on top.
//
// Needs (before this include): renderparms.glsl, rt_light_eval.glsl, rt_light_tex.glsl, rt_perforated.glsl,
// restir_di_common.glsl; GL_EXT_ray_query, GL_EXT_buffer_reference_uvec2, GL_EXT_nonuniform_qualifier.

// EMISSIVE MODE (E2): the emissive triangle table the emitter shade below samples (guarded: the gather includes it too)
#include "rt_emissive.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(1) uniform sampler2D u_normalBuffer;	// xyz = view-space normal, a = weapon/AO mask (0 = weapon)
SAMPLER_BINDING(2) uniform sampler2D u_albedoBuffer;	// apply mode: G-buffer att3 receiver albedo (white when absent)
SAMPLER_BINDING(3) uniform sampler2D u_materialBuffer;	// apply mode: G-buffer att1 ( roughness, metalness, spec-map luminance )

#define depth_consts DUDE_DEPTH_CONSTS()

const vec3 DI_LUM = vec3( 0.2126, 0.7152, 0.0722 );

// Everything the per-light shade needs about the receiving surface. Built by diSurfaceAt() from the G-buffer.
struct DiSurface {
	vec3  Pw, Nw, Vw;			// world position / unit normal / unit direction to the camera (Vw only in apply mode)
	float d;					// positive linear view depth (cookie LOD)
	float vz;					// signed linear view z (negative)
	bool  applyMode, pbr;		// shade with the G-buffer material (Stage D) / GGX vs Lambert
	vec3  albedo, B, F0, envFloor;
	float metal, kd, alpha2, NdotV, specGain;
};

// Reconstruct the surface under SCREEN pixel sfrag (internal-res pixel centre, 0.5 offsets). Returns false for sky /
// a degenerate normal (the caller zeroes its output). Mirrors the gather's original reconstruction exactly.
bool diSurfaceAt( vec2 sfrag, bool applyMode, bool pbr, out DiSurface s, out float rawDepth, out vec4 nt ) {
	// NEAREST texel for the depth and the normal, never a filtered blend: at a silhouette a linear sample mixes the near
	// and far surface into a point floating between them, and the mix shifts with the jitter every frame - so the stored
	// previous point and the current one never agree there, and the R3a boost read every static edge as disoccluded
	// (white edge bands in debug view 7, +1.5 ms of rays standing still, 2026-10-12). Nearest puts every point on a real
	// surface, which the shadow ray origins want anyway; the R1 gradient's same-surface test is exact at edges too.
	ivec2 dsz = textureSize( u_currentDepth, 0 );
	ivec2 nsz = textureSize( u_normalBuffer, 0 );
	rawDepth = texelFetch( u_currentDepth, clamp( ivec2( floor( sfrag * u_depthTexRecip.xy * vec2( dsz ) ) ), ivec2( 0 ), dsz - 1 ), 0 ).x;
	nt = texelFetch( u_normalBuffer, clamp( ivec2( floor( sfrag * u_screenCorrection.xy * vec2( nsz ) ) ), ivec2( 0 ), nsz - 1 ), 0 );
	s.vz = 1.0 / ( min( rawDepth, 0.9994 ) * depth_consts.x + depth_consts.y );	// negative view z
	s.applyMode = applyMode;
	s.pbr = pbr;
	s.albedo = vec3( 1.0 );	s.B = vec3( 1.0 );	s.F0 = vec3( 0.04 );	s.envFloor = vec3( 0.0 );	s.Vw = vec3( 0.0 );
	s.metal = 0.0;	s.kd = 0.0;	s.alpha2 = 1.0;	s.NdotV = 1.0;	s.specGain = 0.0;
	s.Pw = vec3( 0.0 );	s.Nw = vec3( 0.0, 0.0, 1.0 );	s.d = 1.0;
	if ( rawDepth >= 0.9994 ) { return false; }									// sky / no geometry
	vec2  ndc = sfrag * ( u_screenCorrection.xy * 2.0 ) - 1.0;
	s.d = -s.vz;
	vec3  P   = vec3( ndc.x * s.d * u_localParam0.x, ndc.y * u_windowCoord.z * s.d * u_localParam0.y, s.vz );
	vec3  Nraw = nt.xyz * 2.0 - 1.0;
	float nl2  = dot( Nraw, Nraw );
	if ( !( nl2 > 1e-4 ) ) { return false; }									// also catches NaN
	vec3  N   = Nraw * inversesqrt( nl2 );
	s.Pw = ( u_modelViewMatrix * vec4( P, 1.0 ) ).xyz;							// u_modelViewMatrix = inverse view
	s.Nw = normalize( mat3( u_modelViewMatrix ) * N );
	if ( applyMode ) {
		s.albedo = texture( u_albedoBuffer, sfrag * u_screenCorrection.xy ).rgb;
		vec3 mt  = texture( u_materialBuffer, sfrag * u_screenCorrection.xy ).rgb;
		float rough = clamp( mt.r, 0.03, 1.0 );
		s.metal  = mt.g;
		s.kd     = u_diffuseModifier.x;
		// specReshade (u_shadowFalloffS.z == 2): the buffer is DIFFUSE-ONLY, so demodulate by the diffuse share alone -
		// the apply rebuilds the specular at full res. Else the combined diffuse+specular demod (the plain path).
		s.B      = ( u_shadowFalloffS.z > 1.5 )
		         ? max( s.albedo * ( 1.0 - s.metal * s.kd ), vec3( 0.02 ) )
		         : diDemodFactor( s.albedo, s.metal, mt.b, s.kd, u_diffuseModifier.y );
		s.Vw     = normalize( u_modelViewMatrix[3].xyz - s.Pw );				// camera (inverse view translation) -> surface
		s.NdotV  = max( dot( s.Nw, s.Vw ), 1e-4 );
		float a  = rough * rough;
		s.alpha2 = a * a;
		s.F0     = mix( vec3( 0.04 ), s.albedo, s.metal );
		s.specGain = mt.b * mix( u_diffuseModifier.y, 1.0, s.metal ) * 2.0;	// spec map * dielectric-weighted scale * vanilla 2x
		s.envFloor = s.F0 * ( s.metal * u_diffuseModifier.w * ( 1.0 - 0.5 * rough ) ) * mt.b;	// Phase C.1 metal environment floor
	}
	return true;
}

// The UNSHADOWED, NOT-demodulated contribution of light li at surface s: colour * cookie/falloff * N.L, times the
// material response in apply mode (interaction.frag's PBR combine, per unit light). false = the light cannot reach
// (out of range / fast light on the forward path / N.L <= 0 / zero attenuation); contrib is 0 then.
// targetOnly = compute the RIS TARGET only: the unshadowed irradiance (colour * cookie/falloff * N.L), SKIPPING the
// apply-mode GGX material response. The target function may be any cheap positive approximation (the material is a
// per-pixel-constant-ish factor that barely changes the pick), so the gather weighs its ~38 candidates with this and
// re-shades only the WINNER at full material - the big gather cost lever. false = the full shade (the chosen pixel, the
// boost picks, the gradient), which stays exactly as before.
// The apply-mode material response per unit light (interaction.frag's PBR combine): GGX specular lobe * Fresnel * spec
// gain + the metal environment floor + the Lambert diffuse share. Shared by the placed-light and the emitter shades so
// the two agree bit-for-bit (diShadeLight's original expressions, moved here unchanged).
vec3 diMaterialResponse( in DiSurface s, vec3 ldir, float ndl ) {
	vec3 f;
	if ( u_shadowFalloffS.z > 1.5 ) {
		f = s.albedo * ( 1.0 - s.metal * s.kd );	// specReshade: DIFFUSE only; the apply rebuilds the GGX specular at full res
	} else if ( s.pbr ) {
		vec3  H     = normalize( ldir + s.Vw );
		float NdotH = max( dot( s.Nw, H ), 0.0 );
		float VdotH = max( dot( s.Vw, H ), 0.0 );
		vec3  F     = s.F0 + ( 1.0 - s.F0 ) * pow( 1.0 - VdotH, 5.0 );
		float dd    = NdotH * NdotH * ( s.alpha2 - 1.0 ) + 1.0;
		float D     = s.alpha2 / ( dd * dd );									// GGX NDF, pi folded out
		float k     = 0.5 * sqrt( s.alpha2 );
		float gvis  = 0.25 / ( ( ndl * ( 1.0 - k ) + k ) * ( s.NdotV * ( 1.0 - k ) + k ) );
		float lobe  = min( D * gvis, u_diffuseModifier.z );					// firefly clamp
		f = lobe * F * s.specGain + s.envFloor + s.albedo * ( ( 1.0 - F ) * ( 1.0 - s.metal * s.kd ) );
	} else {
		f = s.albedo;															// legacy: Lambert, no spec (v1)
	}
	return f;
}

bool diShadeLight( RtLights lights, uint li, in DiSurface s, out vec3 contrib, out vec3 ldir, out float dl, out float ndl, bool targetOnly ) {
	contrib = vec3( 0.0 );	ldir = vec3( 0.0, 0.0, 1.0 );	dl = 0.0;	ndl = 0.0;
	if ( !dudeLightInRange( lights, li, s.Pw ) ) { return false; }				// cheap volume-sphere cull
	if ( ( lights.l[ li ].flags & DUDE_LIGHT_DIFORWARD ) != 0u ) { return false; }	// fast-changing light: the forward path lights it
	dudeLightDir( lights, li, s.Pw, ldir, dl );
	ndl = max( dot( s.Nw, ldir ), 0.0 );
	if ( !( ndl > 0.0 ) ) { return false; }
	// the light's shape + colour: its real cookie * falloff textures (Stage G, u_windowCoord.w), else the analytic
	// frustum attenuation. Doom lighting model: no 1/pi.
	vec3 atten = ( u_windowCoord.w > 0.5 )
		? dudeLightAttenTex( lights, li, s.Pw, dl, lights.l[ li ].flags, true, false, s.d * u_shadowFalloffS.y )
		: vec3( dudeLightAtten( lights, li, s.Pw, dl, lights.l[ li ].flags, true, false ) );
	contrib = lights.l[ li ].color * ( atten * ndl );							// irradiance-like: colour * cookie/falloff * N.L
	if ( s.applyMode && !targetOnly ) {
		contrib *= diMaterialResponse( s, ldir, ndl );
	}
	return true;
}

// EMISSIVE MODE (docs/rtx-experimental-lighting.md E2): the unshadowed contribution DENSITY of one sample point on
// emissive triangle `tri` (xi = the uniform point's two randoms) at surface s - per unit EMITTER AREA, so the RIS
// weight divides it by the sample's area-measure pdf (flux share / area). radiance = the glow texel at the point
// (rtEmissiveTexelToRadiance: the stage tint, or the diffuse through its threshold for a flare-inferred patch) x the
// material scale x the mode intensity; geometry = cos at the emitter (two-sided when the material is) x N.L at the
// receiver / d^2 (d clamped at 2 units: a receiver flush with its tube). false = cannot reach / dark / behind.
// Same material response as a placed light in apply mode. Needs rt_emissive.glsl + the bindless u_rtTextures.
// ldirSh = the direction to the emitter's CENTRE: what the SH side channel carries for an emitter winner. The sampled
// point differs per pixel, NRD aggregates those directions into a short vector and the apply's specular re-shade
// reads a short vector as "no concentrated highlight" - the glossy floor went flat under the tiles (user, 2026-10-20).
// The centre keeps the vector long and puts the highlight at the tile's mirror point.
bool diShadeEmitter( RtEmissiveTable t, uint tri, vec2 xi, in DiSurface s, float intensity, out vec3 contrib, out vec3 ldir, out float dl, out float ndl, out float pdfw, out vec3 ldirSh, bool targetOnly ) {
	contrib = vec3( 0.0 );	ldir = vec3( 0.0, 0.0, 1.0 );	dl = 0.0;	ndl = 0.0;	pdfw = 0.0;	ldirSh = vec3( 0.0, 0.0, 1.0 );
	RtEmissiveTri e = rtEmissiveFetch( t, tri );
	if ( !( e.flux > 0.0 ) ) { return false; }									// dark (switched off / gone)
	// the sample point UNIFORM IN SOLID ANGLE from the receiver (Arvo; Q2RTX): the contribution is then per unit solid
	// angle, L x N.L, bounded by the solid angle - no 1/d^2 term at all (it is inside pdfw)
	vec3 Pe, nUnit; bool front;
	if ( !rtEmissiveSampleSolidAngle( e, s.Pw, xi, Pe, nUnit, pdfw, front ) ) { return false; }
	if ( !front && ( e.emit & RT_EMIT_TWOSIDED ) == 0u ) { return false; }	// a one-sided emitter seen from its back
	vec3 d = Pe - s.Pw;
	dl   = length( d );
	if ( !( dl > 1e-3 ) ) { return false; }
	ldir = d / dl;
	ldirSh = normalize( ( e.p0 + e.p1 + e.p2 ) * ( 1.0 / 3.0 ) - s.Pw );
	ndl  = max( dot( s.Nw, ldir ), 0.0 );
	if ( !( ndl > 0.0 ) ) { return false; }
	// nUnit points AT the receiver (the sampler re-winds): cos at the emitter = nUnit . (emitter -> receiver) = -nUnit . ldir.
	// Emission profile sqrt( cos_e ) (Q2RTX's "spotlight"): a tube reads directional, a tile stays broad.
	float cosL = max( -dot( nUnit, ldir ), 0.0 );
	if ( !( cosL > 0.0 ) ) { return false; }
	vec3 L = rtEmissiveMeanRadiance( e ) * ( e.scale * intensity * rtEmissiveReachBoost( e, dl, u_texGen0Q.z, u_texGen0Q.w ) );	// a uniform emitter: its mean radiance, the authored reach
	contrib = L * ( sqrt( cosL ) * ndl );
	if ( s.applyMode && !targetOnly ) {
		contrib *= diMaterialResponse( s, ldir, ndl );
	}
	return true;
}

// one hard any-occluder shadow ray from the surface toward the chosen light (interaction.frag's inline ray).
// 1 = lit, 0 = occluded, 1 - strength behind a solid grate texel. No TLAS (no RT scene) -> unshadowed: never
// trace a null acceleration structure.
//
// Perforated casters (r_rtPerforatedShadows, rt_perforated.glsl): the forward inline ray's grate-aware walk, taken
// by EVERY pixel while grates are on - the perforated instances (mask 0x02) arrive as alpha-tested candidates and
// opaque geometry commits as before. The forward path gates this per LIGHT on a CPU box list; that does not fit
// DI, where every pixel picks its own light: a per-view list of every grate triangle in the lights' union ran to
// 1199 boxes in a busy room (2026-10-16), no cap holds that, and a slab loop over it would cost more than the
// walk it exists to avoid. The hardware BVH over the grate triangles IS the gate: a segment that meets no grate
// never leaves the RT core (no candidates). Measured (A/B r_rtPerforatedShadows 0/1, 40-light room): this walk on
// every pixel costs NOTHING within noise (gather 4.57 -> 4.55 ms), where the box list cost +0.36 ms (the overflow
// box sat in the LAST slot, so every ray ran all 64 slab iterations - 128 buffer loads - in a low-occupancy
// shader) and a separate force-opaque "gate" ray over the 0x02 subset cost +0.34 ms (a second dependent launch per
// unblocked pixel, repeating the walk's own traversal). u_windowCoord.x = grates on (1/0), .y = strength,
// u_particleRadius.xy = geo table.
// occDist (out) = distance to the occluder when the ray is blocked, else the distance to the light: the "hit
// distance" NRD scales its blur radius by. Feeding the LIGHT distance for every pixel told ReBLUR that every
// shadow came from a far source and widened the blur until grate / contact shadows vanished (user, 2026-10-08).
#define DI_TLAS accelerationStructureEXT( uvec2( floatBitsToUint( u_rtParms.x ), floatBitsToUint( u_rtParms.y ) ) )
// The hybrid shadow blur's per-light blur scale (RB_RHI_RtShadowBlurLightRadius: 3 * r_rtShadowBlurIntensity for a fixture,
// grown with a big omni's volume, one degree of arc for a parallel sun), packed by RB_RHI_GatherSceneLights as a half
// float in the light's flags bits 16-31. 0 = the blur is off: hard grate texels, as before.
float diLightBlurScale( uint flags ) {
	return unpackHalf2x16( flags >> 16u ).x;
}
float traceVisibility( vec3 Pw, vec3 Nw, vec3 ldir, float dl, float blurScale, out float occDist ) {
	occDist = dl;
	if ( u_rtParms.x == 0.0 && u_rtParms.y == 0.0 ) { return 1.0; }		// no RT scene
	float tMax = min( dl - 1.0, u_rtParms.w );
	if ( !( tMax > 0.0 ) ) { return 1.0; }
	vec3  O = Pw + Nw * u_rtParms.z;
	uvec2 geoAddr = uvec2( floatBitsToUint( u_particleRadius.x ), floatBitsToUint( u_particleRadius.y ) );
	bool  perfOn  = u_windowCoord.x > 0.5 && u_windowCoord.y > 0.0 && ( geoAddr.x | geoAddr.y ) != 0u;
	if ( !perfOn ) {
		rayQueryEXT rqo;
		rayQueryInitializeEXT( rqo, DI_TLAS, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
			0xFDu /* skip perforated-caster instances */, O, 0.0, ldir, tMax );
		while ( rayQueryProceedEXT( rqo ) ) { }
		if ( rayQueryGetIntersectionTypeEXT( rqo, true ) == gl_RayQueryCommittedIntersectionNoneEXT ) { return 1.0; }
		occDist = rayQueryGetIntersectionTEXT( rqo, true );
		return 0.0;
	}
	// grate-aware: a solid texel dims to 1 - strength and the ray carries on (min over grates, not a product - a
	// candidate may be reported twice); opaque geometry commits and terminates as before.
	//
	// r_rtShadowBlur parity (u_specularModifier.x = on, .y = r_rtShadowBlurCurve): the hybrid tier spreads a grate's
	// shadow over halfW = blurScale * g^curve, g = dOcc / ( dL - dOcc ), in screen space after its ray. DI has no
	// per-light mask to blur, so the same width is applied at the CASTER: the receiver's penumbra disc, projected
	// back along the light cone onto the grate plane, is a disc of radius halfW * ( 1 - dOcc / dL ) around the hit,
	// and dudePerforatedCoverage averages the coverage over it through the texture's mips. Deterministic, so the
	// gradient pass re-shades bit-identically, and noise-free for NRD. blurScale 0 = the hard texel test.
	rayQueryEXT rq;
	rayQueryInitializeEXT( rq, DI_TLAS, gl_RayFlagsTerminateOnFirstHitEXT, 0xFFu, O, 0.0, ldir, tMax );
	float strength = u_windowCoord.y;
	float floorVis = 1.0 - strength;
	bool  soft     = u_specularModifier.x > 0.5 && blurScale > 0.0;
	float perfVis  = 1.0;
	float tPerf    = dl;
	while ( rayQueryProceedEXT( rq ) ) {
		// fully dimmed already (a solid texel, or enough partial ones): further grates can't darken it more
		if ( perfVis > floorVis + 1e-4
				&& rayQueryGetIntersectionTypeEXT( rq, false ) == gl_RayQueryCandidateIntersectionTriangleEXT ) {
			float tc = rayQueryGetIntersectionTEXT( rq, false );
			if ( tc < dl - DUDE_RT_PERF_NEAR ) {
				float foot = 0.0;
				if ( soft ) {
					float g = tc / max( dl - tc, 1e-3 );
					foot = blurScale * pow( g, max( u_specularModifier.y, 0.05 ) ) * ( 1.0 - tc / dl );
				}
				float f = dudePerforatedCoverage( geoAddr,
					uint( rayQueryGetIntersectionInstanceCustomIndexEXT( rq, false ) ),
					uint( rayQueryGetIntersectionGeometryIndexEXT( rq, false ) ),
					uint( rayQueryGetIntersectionPrimitiveIndexEXT( rq, false ) ),
					rayQueryGetIntersectionBarycentricsEXT( rq, false ), foot );
				float v = 1.0 - strength * f;
				if ( v < perfVis ) { perfVis = v; tPerf = tc; }
			}
		}
	}
	if ( rayQueryGetIntersectionTypeEXT( rq, true ) == gl_RayQueryCommittedIntersectionNoneEXT ) {
		// dimmed by a grate and nothing solid behind it: the grate IS the occluder NRD sizes its blur by
		// (rtshadow_ray.frag sizes its penumbra the same way). Handing it the light distance instead told
		// ReBLUR every grate shadow came from a far source and smeared the pattern at full radius.
		if ( perfVis < 1.0 ) { occDist = tPerf; }
		return perfVis;
	}
	occDist = rayQueryGetIntersectionTEXT( rq, true );
	return 0.0;
}

// The per-pixel value the gradient compares across frames: the chosen light's shaded, DEMODULATED, visible
// contribution at the forward light scale - what the direct term feeds NRD, before the RIS weight (which is a
// common factor of both frames and cancels in the normalized gradient). Rounded through fp16 on BOTH sides
// (RTXDI: avoids precision false positives), which is also the storage format.
float diGradientLuminance( vec3 contrib, vec3 B, float vis ) {
	float l = dot( contrib / B, DI_LUM ) * vis * u_shadowFalloffS.x;
	return unpackHalf2x16( packHalf2x16( vec2( l, 0.0 ) ) ).x;
}

// NRD normal encoding 2 (signed octahedron, z sign in .z) -> world normal: the exact inverse of the gather's packNrdNormal.
// Used by the gradient (R1) and the boost (R3a) to read last frame's stored normal.
vec3 unpackNrdNormal( vec3 r ) {
	float a = 2.0 * r.x - 1.0;					// n.x + n.y
	float b = 2.0 * r.y - 1.0;					// n.y - n.x
	vec3  n = vec3( 0.5 * ( a - b ), 0.5 * ( a + b ), 0.0 );
	n.z = ( 1.0 - abs( n.x ) - abs( n.y ) ) * ( r.z < 0.5 ? -1.0 : 1.0 );
	float l2 = dot( n, n );
	return ( l2 > 1e-8 ) ? n * inversesqrt( l2 ) : vec3( 0.0, 0.0, 1.0 );
}
