// DUDE RT reflections (docs/rtx-reflections.md RR2): the dynamic layer of the reflection stack. For
// reflective pixels that screen-space SSR could not serve (its marched result is 0 here — off-screen
// or screen-occluded), trace a reflection ray into the scene TLAS and, on a MONSTER hit, shade it from
// the hit's GPU-skinned gpuSkinVB normal (the fetch r_rtReflTest/RR1 validated). Additive over the lit
// scene with the same Fresnel/gloss weighting as ssr_composite, so material response matches SSR.
//
// VK + RT hardware only (ray_query): the backend loads this program solely when the device supports
// KHR_ray_query, and only draws it when r_rtReflections is on. buffer_reference is enabled by the
// prelude; ray_query is added here (same idiom as interaction.frag). Uniform packing (RhiWorld.cpp
// RB_RHI_ScreenSpaceReflections): u_rtParms = { TLAS addr lo/hi, geo-table addr lo/hi } bit-cast;
// u_modelViewMatrix = inverse view (view->world); u_localParam0 = (1/proj00, 1/proj11, roughnessFade,
// RR12 scene-reuse enable); u_localParam1 = (RR14 specOcclusion, intensity, maxRoughness, RR10 bump);
// u_windowCoord = (jitterX, jitterY, view-Y sign (VK), RR12 firefly clamp); u_color = (RR16 light-buffer
// device addr lo/hi, light count, ambient); u_diffuseModifier = (RR16f emissive glow scale, RR16 light
// brightness scale, _, RR16 real-lights enable); u_depthTexRecip/u_screenCorrection size the buffers.
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference_uvec2 : require	// construct the geo-table pointer from u_rtParms.zw (uvec2)
#extension GL_EXT_nonuniform_qualifier : require		// RR4: nonuniformEXT index into the bindless texture array

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_sceneColor;     // RR12: live lit HDR scene (reuse); dummy G-buffer when reuse off
SAMPLER_BINDING(1) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(2) uniform sampler2D u_normalBuffer;   // xyz = view-space normal, a = weapon mask
SAMPLER_BINDING(3) uniform sampler2D u_materialBuffer; // x = roughness, y = metalness
SAMPLER_BINDING(4) uniform sampler2D u_aoBuffer;       // RR14: R = ambient visibility (GTAO / RTAO); white when no AO / off

VARY(0) in vec2 var_TexCoord;
layout(location = 0) out vec4 fragColor;

// idDrawVert as raw uints (stride 60 B = 15 uints): xyz @0, normal @5 (byte 20). Same view as zfill_batch.
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer VertRef { uint w[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer IdxRef  { uint i[]; };
struct GeoDesc { VertRef vb; IdxRef ib; uint stride; uint flags; uint baseColor; uint texIndex; };	// RR0/RR3/RR4 geometry table row
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer GeoTable { GeoDesc d[]; };
struct RtLight { vec3 pos; float radius; vec3 color; uint flags; };		// RR16 light-buffer row (std430, 32 B; flags bit0 = parallel/sun)
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer RtLights { RtLight l[]; };

// RR4 bindless materials: the resident texture set as one array, indexed by (texIndex - 1). Set 2 is
// bound once per frame by the backend and populated by SyncBindlessSlot. nonuniformEXT because the index
// varies per fragment — each reflective pixel can hit a different surface.
layout(set = 2, binding = 0) uniform sampler2D u_rtTextures[];

// Doom 3's fixed near / near-infinite far projection in GL clip depth -> linear eye z (negative).
// Same constants as ssao.frag / ssr.frag / ssr_composite.frag.
// depth -> view z pair of THIS view (renderparms.glsl u_depthParms): the game lowers the near plane
// for cinematic cameras, so it is not the constant ( 0.33333333, -0.33316667 ) it used to be hard-coded as
#define depth_consts DUDE_DEPTH_CONSTS()

void main() {
	// RR6c temporal upscale: jitter the whole reconstruction by a per-frame sub-texel offset
	// (u_windowCoord.xy, in this pass's target texels; Halton, scaled by r_rtReflJitter). This shifts
	// the low-res sample GRID each frame, so the non-integer-upscale beat (the "striped" reflection at
	// e.g. 3/4 res, where the bilinear weights cycle with period 4 over sharp content) lands at a
	// different phase every frame and the FULL-RES temporal history averages it out — while accumulating
	// the sub-pixel-shifted samples toward genuine full-res detail. Everything below reconstructs from
	// this jittered frag, so origin + reflected direction shift coherently.
	vec2 frag = gl_FragCoord.xy + u_windowCoord.xy;
	float raw = texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;
	if ( raw >= 0.9994 ) { fragColor = vec4( 0.0 ); return; }			// sky / no geometry
	vec2 uv = frag * u_screenCorrection.xy;
	vec4 nt = texture( u_normalBuffer, uv );
	if ( nt.a < 0.5 ) { fragColor = vec4( 0.0 ); return; }				// view weapon

	vec4 mt = texture( u_materialBuffer, uv );
	float rough = mt.x;
	float metal = mt.y;
	float maxRough = u_localParam1.z;
	float gloss = 1.0 - smoothstep( maxRough * u_localParam0.z, maxRough, rough );	// .z = r_ssrRoughnessFade (frac of cutoff)
	if ( gloss < 0.004 ) { fragColor = vec4( 0.0 ); return; }			// not reflective enough

	// RR7: NO miss-gate. RT used to defer to SSR here (skip where the screen-space march already
	// reflected), but SSR's coverage PATTERN beats/flickers at a fractional r_ssrResScale (2/3 strobes,
	// 3/4 stripes) and RT inherited it at the gate boundary — "SSR getting in the way". RT now reflects
	// every reflective pixel independently by tracing the scene TLAS; the backend skips SSR's composite
	// when RT renders, so there is no double-reflection to gate against. (u_ssr is retained only as a
	// bound-but-unused slot; the backend keeps a valid image there.)

	// view-space position + normal (same reconstruction as ssr_composite.frag; u_windowCoord.z flips
	// the reconstructed Y on VK's top-down framebuffer so P agrees with the view-space G-buffer normal)
	float vz  = 1.0 / ( raw * depth_consts.x + depth_consts.y );			// negative
	vec2  ndc = uv * 2.0 - 1.0;
	float dd  = -vz;
	vec3  P = vec3( ndc.x * dd * u_localParam0.x, ndc.y * u_windowCoord.z * dd * u_localParam0.y, vz );
	// degenerate-normal hang guard (mirrors rtao_ray.frag): a NaN here reaches
	// rayQueryInitializeEXT's direction below, which is UB that can wedge RT traversal
	vec3  Ndec = nt.xyz * 2.0 - 1.0;
	float nl2  = dot( Ndec, Ndec );
	if ( !( nl2 > 1e-4 ) ) { fragColor = vec4( 0.0 ); return; }
	vec3  N = Ndec * inversesqrt( nl2 );
	vec3  V = normalize( -P );

	// RT reflection normal: blend the GEOMETRIC (bump-free) normal toward the full BUMP normal N by
	// r_rtReflBump (u_localParam1.w). geoN — reconstructed from screen-space depth derivatives — gives a
	// stable planar mirror; N warps the reflection per the normal map exactly like SSR (tile/grout relief
	// distorts the reflected image). The single-ray aliasing the bump normal used to cause (the firefly
	// grid on grout bevels) turned out to be the SSR miss-gate + fractional-res upscale, both now gone
	// (RR7 + integer-res) and any residual is averaged by the temporal upscale — so the bump normal is back
	// on by default. At a SILHOUETTE the depth-derivative geoN is unreliable (spans a discontinuity), so
	// use N there regardless.
	vec3  gN   = cross( dFdx( P ), dFdy( P ) );
	float gLen = length( gN );
	vec3  geoN = ( gLen > 1e-6 ) ? ( gN / gLen ) : N;
	if ( dot( geoN, N ) < 0.0 ) { geoN = -geoN; }
	float bump = clamp( u_localParam1.w, 0.0, 1.0 );
	vec3  Nr   = ( dot( geoN, N ) < 0.2 ) ? N : normalize( mix( geoN, N, bump ) );	// N at silhouettes; bump-warp elsewhere

	// Schlick Fresnel + gloss window, same as ssr_composite (F0 0.9 for metal keeps untinted steel)
	float ndv  = max( dot( Nr, V ), 0.0 );
	float F0   = mix( 0.04, 0.9, metal );
	float m    = 1.0 - ndv;
	float m2   = m * m;
	float fres = F0 + ( 1.0 - F0 ) * ( m2 * m2 * m );					// pow(1-ndv,5) as 3 muls: exact, no exp2/log2
	float weight = fres * gloss * u_localParam1.y;						// * r_ssrIntensity
	if ( weight < 0.002 ) { fragColor = vec4( 0.0 ); return; }

	// RR14 specular occlusion (see ssr_composite.frag for the derivation): roughness-aware AO dim of
	// the reflection, folded into the weight so it is temporally accumulated + blurred with the rest.
	// Applied AFTER the discard gate so a heavily-occluded pixel is culled on the same pre-occlusion
	// threshold as before (identical coverage). u_localParam1.x = r_specOcclusion (0 = off); the
	// backend binds a white AO dummy when no AO ran this view, so ao = 1 -> no change.
	{
		float ao = texture( u_aoBuffer, uv ).r;
		float so = clamp( pow( ndv + ao, exp2( -16.0 * rough - 1.0 ) ) - 1.0 + ao, 0.0, 1.0 );
		weight *= mix( 1.0, so, u_localParam1.x );
	}

	// reflect in view space, lift to world (u_modelViewMatrix = inverse view = view->world)
	vec3 Rv = reflect( -V, Nr );
	vec3 wp = ( u_modelViewMatrix * vec4( P, 1.0 ) ).xyz;
	vec3 wr = normalize( mat3( u_modelViewMatrix ) * Rv );
	vec3 wN = normalize( mat3( u_modelViewMatrix ) * Nr );				// world ray normal (ray-origin bias)
	// (RR6c replaced the RR6a ray-cone jitter with the sample-GRID jitter applied to `frag` at the top:
	// shifting the whole reconstruction both feeds the temporal accumulation AND moves the upscale beat.)

	rayQueryEXT rq;
	rayQueryInitializeEXT( rq,
		accelerationStructureEXT( uvec2( floatBitsToUint( u_rtParms.x ), floatBitsToUint( u_rtParms.y ) ) ),
		gl_RayFlagsOpaqueEXT, 0xFFu, wp + wN * 2.0, 0.0, wr, 8192.0 );
	while ( rayQueryProceedEXT( rq ) ) { }
	if ( rayQueryGetIntersectionTypeEXT( rq, true ) != gl_RayQueryCommittedIntersectionTriangleEXT ) {
		fragColor = vec4( 0.0 ); return;								// reflection ray hit nothing
	}
	uint ci = uint( rayQueryGetIntersectionInstanceCustomIndexEXT( rq, true ) );
	uint gi = uint( rayQueryGetIntersectionGeometryIndexEXT( rq, true ) );
	GeoTable table = GeoTable( uvec2( floatBitsToUint( u_rtParms.z ), floatBitsToUint( u_rtParms.w ) ) );
	GeoDesc g = table.d[ ci + gi ];
	if ( ( g.flags & 1u ) == 0u ) { fragColor = vec4( 0.0 ); return; }	// no-attr defer row (mover/prop) -> not shadeable yet

	// RR12 lit-scene reuse: if this hit is on-screen this frame, the scene already holds its TRUE radiance
	// (real lights/shadows/decals/emissive) - reproject the hit and read it, far more correct than the fixed
	// key light below. On HIGH confidence take it and RETURN, skipping the geometry fetch + bindless sample
	// (the cheap path). u_localParam0.w = r_rtReflSceneReuse AND a sampleable scene target; else conf stays 0
	// and the fixed-key-light fallback runs unchanged. hitView is exact: the ray is rigid, so world t == view
	// t, and the world origin (wp + wN*2) is P + Nr*2 in view space.
	float thit = rayQueryGetIntersectionTEXT( rq, true );			// RR12/RR16: hit distance, shared by reuse reproject + world-hit shade
	float conf = 0.0;
	vec3  sceneCol = vec3( 0.0 );
	if ( u_localParam0.w > 0.5 ) {
		vec3  hitView = P + Nr * 2.0 + Rv * thit;
		float dd2 = -hitView.z;											// forward distance; the reprojection divides by it
		// Guard the divide BEFORE it can blow up: as a hit nears the eye plane the denominator dd2 -> 0, so ruv
		// races off-screen and the coverage would collapse over a hairline of pitch (the "horizontal cut that
		// moves when you look up/down"). Only reproject once dd2 is comfortably positive; the fade below then
		// dissolves reuse into the fallback as a smooth band, never a hard line.
		if ( dd2 > 0.5 ) {
			vec2  ruv = vec2( hitView.x / ( dd2 * u_localParam0.x ),
			                  hitView.y / ( dd2 * u_localParam0.y * u_windowCoord.z ) ) * 0.5 + 0.5;
			// coverage: a WIDE screen-edge fade (10%) + a gentle near-plane fade in view space, so the seam is a
			// gradient a viewer can't localise, not a moving line. (dd2 fade starts well before the divide guard.)
			vec2  ef  = smoothstep( vec2( 0.0 ), vec2( 0.10 ), ruv ) * smoothstep( vec2( 0.0 ), vec2( 0.10 ), 1.0 - ruv );
			float onScreen = ef.x * ef.y * smoothstep( 0.5, 2.0, dd2 );
			if ( onScreen > 0.0 ) {
				// depth agreement: the hit must be the surface the camera sees at ruv (reject in-front occluders)
				vec2  dtc  = ruv * ( u_depthTexRecip.xy / u_screenCorrection.xy );	// normalized uv -> POT depth texcoord
				float sraw = texture( u_currentDepth, dtc ).x;
				float svz  = 1.0 / ( sraw * depth_consts.x + depth_consts.y );		// scene view z (negative)
				float depthOk = 1.0 - smoothstep( 0.02 * dd2, 0.06 * dd2, abs( svz - hitView.z ) );
				float wmask   = step( 0.5, texture( u_normalBuffer, ruv ).a );		// not the view weapon
				conf     = onScreen * depthOk * wmask;
				sceneCol = texture( u_sceneColor, ruv ).rgb;
				float fcap = u_windowCoord.w;								// firefly-clamp the reused HDR radiance (0 = off)
				if ( fcap > 0.0 ) {
					float lum = dot( sceneCol, vec3( 0.2126, 0.7152, 0.0722 ) );
					if ( lum > fcap ) { sceneCol *= fcap / lum; }
				}
			}
		}
	}
	if ( u_localParam0.w > 1.5 ) {									// r_rtReflDebug: green = reuse, red = key-light fallback
		fragColor = vec4( 1.0 - conf, conf, 0.0, 1.0 );
		return;
	}
	if ( conf >= 0.995 ) {
		fragColor = vec4( sceneCol * weight, 1.0 );					// pure reuse: skip the geometry fetch + bindless sample
		return;
	}

	uint prim = uint( rayQueryGetIntersectionPrimitiveIndexEXT( rq, true ) );
	vec2 bc   = rayQueryGetIntersectionBarycentricsEXT( rq, true );
	mat4x3 o2w = rayQueryGetIntersectionObjectToWorldEXT( rq, true );
	uint s  = g.stride >> 2u;
	uint p3 = 3u * prim;
	uint b0 = g.ib.i[ p3 + 0u ] * s;					// base word index of each hit vertex (idDrawVert = s words)
	uint b1 = g.ib.i[ p3 + 1u ] * s;
	uint b2 = g.ib.i[ p3 + 2u ] * s;
	float w0 = 1.0 - bc.x - bc.y;						// third barycentric, shared by normal + texcoord interp
	vec3 n0 = vec3( uintBitsToFloat( g.vb.w[ b0 + 5u ] ), uintBitsToFloat( g.vb.w[ b0 + 6u ] ), uintBitsToFloat( g.vb.w[ b0 + 7u ] ) );
	vec3 n1 = vec3( uintBitsToFloat( g.vb.w[ b1 + 5u ] ), uintBitsToFloat( g.vb.w[ b1 + 6u ] ), uintBitsToFloat( g.vb.w[ b1 + 7u ] ) );
	vec3 n2 = vec3( uintBitsToFloat( g.vb.w[ b2 + 5u ] ), uintBitsToFloat( g.vb.w[ b2 + 6u ] ), uintBitsToFloat( g.vb.w[ b2 + 7u ] ) );
	vec3 hitN = normalize( mat3( o2w ) * ( w0 * n0 + bc.x * n1 + bc.y * n2 ) );
	// RR4: interpolate the hit triangle's texcoords (idDrawVert st at uint offset 3) for the diffuse fetch
	vec2 st0 = vec2( uintBitsToFloat( g.vb.w[ b0 + 3u ] ), uintBitsToFloat( g.vb.w[ b0 + 4u ] ) );
	vec2 st1 = vec2( uintBitsToFloat( g.vb.w[ b1 + 3u ] ), uintBitsToFloat( g.vb.w[ b1 + 4u ] ) );
	vec2 st2 = vec2( uintBitsToFloat( g.vb.w[ b2 + 3u ] ), uintBitsToFloat( g.vb.w[ b2 + 4u ] ) );
	vec2 hitST = w0 * st0 + bc.x * st1 + bc.y * st2;

	// RR4: albedo from the hit surface's real diffuse (bindless slot texIndex-1), else the RR3 material average.
	// RR16f: baseColor.a flags a self-illuminated (glow) surface; baseColor.rgb then carries the glow colour.
	vec4  bcol = unpackUnorm4x8( g.baseColor );
	vec3  albedo = ( g.texIndex != 0u )
		? texture( u_rtTextures[ nonuniformEXT( g.texIndex - 1u ) ], hitST ).rgb
		: bcol.rgb;

	// RR16: shade the hit with the scene's REAL lights, so an OFF-screen-reflected surface is lit correctly
	// (not by a fixed key light) - the fix for r_rtReflSceneReuse's screen-space limitation. World hit =
	// wp + wN*2 + wr*t (the world ray vectors already exist). u_color = (lightBuf addr lo/hi, count, ambient);
	// u_diffuseModifier = (RR16f glow scale, _, _, realLightsEnable). Point-light diffuse approximation (no
	// projected-light frustum or shadow rays yet - RR16c/d). The RR12 on-screen reuse still overrides this.
	vec3  lit = albedo * u_color.w;										// ambient floor
	if ( u_diffuseModifier.w > 0.5 ) {									// r_rtReflRealLights: loop the light buffer
		vec3  hitWorld = wp + wN * 2.0 + wr * thit;
		RtLights lights = RtLights( uvec2( floatBitsToUint( u_color.x ), floatBitsToUint( u_color.y ) ) );
		uint nLights = uint( u_color.z );
		for ( uint li = 0u; li < nLights; li++ ) {
			RtLight L  = lights.l[ li ];
			vec3    d  = L.pos - hitWorld;
			float   dl = length( d );
			vec3    ldir  = ( dl > 1e-3 ) ? ( d / dl ) : vec3( 0.0, 0.0, 1.0 );
			float   atten = ( ( L.flags & 1u ) != 0u ) ? 1.0							// parallel/sun: no distance falloff
			                                            : max( 1.0 - dl / max( L.radius, 1.0 ), 0.0 );
			// linear falloff (Doom's falloff ramp is ~linear; the old quadratic under-lit the mid-range).
			// u_diffuseModifier.y = r_rtReflLightScale compensates the point approximation's missing
			// projected-light central brightness, so reflected surfaces match their on-screen brightness.
			lit += albedo * L.color * ( atten * max( dot( hitN, ldir ), 0.0 ) * u_diffuseModifier.y );
		}
	} else {
		float ndl = max( dot( hitN, normalize( vec3( 0.4, 0.5, 0.9 ) ) ), 0.0 );	// r_rtReflRealLights 0: the old fixed key light
		lit += albedo * ndl;
	}
	// RR16f: a self-illuminated light source (neon tube, ceiling light tile) is lit by nothing above, so add
	// back its own glow (baseColor.rgb) scaled by r_rtReflEmissive - otherwise it goes dark / "missing" in an
	// off-screen reflection where the RR12 scene reuse can't cover it.
	if ( bcol.a > 0.5 ) { lit += bcol.rgb * u_diffuseModifier.x; }
	lit = mix( lit, sceneCol, conf );						// RR12: partial reuse near the frame edge / soft depth boundary
	fragColor = vec4( lit * weight, 1.0 );
}
