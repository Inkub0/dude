// Translated from glprogs/interaction.vfp (fragment program).
// Texture units preserved from the ARB program.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform samplerCube u_normalCubeMap; // normalization cube map
SAMPLER_BINDING(1) uniform sampler2D u_bumpMap;
SAMPLER_BINDING(2) uniform sampler2D u_lightFalloff;
SAMPLER_BINDING(3) uniform sampler2D u_lightProjection;
SAMPLER_BINDING(4) uniform sampler2D u_diffuseMap;
SAMPLER_BINDING(5) uniform sampler2D u_specularMap;
SAMPLER_BINDING(6) uniform sampler2D u_specularTable;   // specular falloff LUT
SAMPLER_BINDING(7) uniform sampler2DShadow u_shadowMap; // 2D depth map (projected/spot light)
SAMPLER_BINDING(8) uniform samplerCubeShadow u_shadowCube; // cube depth map (point light)
SAMPLER_BINDING(9) uniform sampler2D u_ssao;            // DUDE GTAO buffer (R = ambient visibility)
SAMPLER_BINDING(10) uniform sampler2D u_occlusionMap;   // DUDE baked AO map (R = visibility)
SAMPLER_BINDING(11) uniform sampler2D u_parallaxMap;   // DUDE parallax height map (R = height)
SAMPLER_BINDING(12) uniform samplerCubeShadow u_shadowCubeDyn; // DUDE static/dynamic split (lever B): movers-only cube

VARY(0) in vec3 var_TexLightVec;
VARY(1) in vec2 var_TexBump;
VARY(2) in vec2 var_TexFalloff;
VARY(3) in vec4 var_TexProjection;
VARY(4) in vec2 var_TexDiffuse;
VARY(5) in vec2 var_TexSpecular;
VARY(6) in vec3 var_TexHalfVec;
VARY(7) in vec4 var_Color;
VARY(8) in vec3 var_TexViewVec;
VARY(9) in vec3 var_ShadowCubeVec;
VARY(12) in vec4 var_ShadowProjection; // UNBAKED projection for the 2D shadow lookup

layout(location = 0) out vec4 fragColor;

// 0 = fully shadowed, 1 = fully lit. u_shadowParms.x selects the technique:
//   0 = none (stencil / unshadowed) -> always lit, vanilla untouched
//   1 = projected/spot: 2D map, reusing the light-projection texgen
//       (var_TexProjection gives the cookie UV, var_TexFalloff.x the axial depth)
//   2 = point/omni: cube map, indexed by the world-space light->frag direction
//       (var_ShadowCubeVec), reference = radial distance / range
//   3 = sun (oversize-omni / parallel light): 2D map through a per-view fitted
//       virtual projection; UV like mode 1 but the compare reference is the
//       virtual depth plane (var_ShadowProjection.z) instead of the light falloff
// Every tap is a hardware depth-compare (2x2 bilinear PCF in the TMU); the multi-tap
// kernels below only decide WHERE those taps land.
//
// Tap placement (docs/shadow-research.md item 0): a Vogel spiral rotated per pixel
// (Jimenez, COD:AW SIGGRAPH 2014; Sterna 2018). The first N points of a Vogel spiral are
// evenly distributed for ANY N — unlike a truncated Poisson array — so the per-preset tap
// counts (u_specularParms.w = 1..16) all get uniform disc coverage. The per-pixel rotation
// turns the old repeating-pattern banding (one fixed disc for every pixel) into fine
// dither and multiplies effective sample diversity, which is what lets a lower tap count
// match the old quality.
//
// The rotation source is a white-noise HASH, deliberately NOT Interleaved Gradient Noise:
// IGN is an anisotropic gradient designed to be averaged out by TAA, and without TAA its
// slow-varying diagonal gives stripes of near-equal rotation that beat against the shadow
// texel grid — visible moire (user-observed). The hash is spatially decorrelated, so the
// residual is unstructured grain instead. Deterministic per pixel: still no flicker on a
// static frame. Same hash family as hdrresolve.frag's film grain.
float shadowHash( vec2 p ) {
	vec3 p3 = fract( vec3( p.xyx ) * 0.1031 );
	p3 += dot( p3, p3.yzx + 33.33 );
	return fract( ( p3.x + p3.y ) * p3.z );
}

// i-th of n Vogel-spiral disc points, rotated by phi. Golden angle 2.39996323.
vec2 vogelDisc( int i, int n, float phi ) {
	float r = sqrt( ( float( i ) + 0.5 ) / float( n ) );
	float theta = 2.39996323 * float( i ) + phi;
	return r * vec2( cos( theta ), sin( theta ) );
}

// Sample one point-light shadow cube by direction L with radial reference ref (dist = |L|).
// Hardware 2x2 depth-compare, optionally widened to a rotated-Vogel disc PCF
// (u_specularParms.w taps). Factored out so the static and dynamic (lever B) cubes
// filter identically. 0 = shadowed, 1 = lit.
float sampleCubeShadow( samplerCubeShadow cube, vec3 L, float ref, float dist ) {
	int taps = int( u_specularParms.w + 0.5 );
	if ( taps <= 1 ) {
		return texture( cube, vec4( L, ref ) );		// single hardware 2x2 tap
	}
	// Disc PCF: perturb L within its tangent plane by a few texels' worth of angle and average.
	vec3 up = abs( L.y ) < 0.99 ? vec3( 0.0, 1.0, 0.0 ) : vec3( 1.0, 0.0, 0.0 );
	vec3 tx = normalize( cross( up, L ) );
	vec3 ty = cross( L, tx ) / dist;	// tx is unit and perpendicular to L, so |cross| == dist
	float r = 4.0 * dist * u_shadowParms.y;	// one cube texel (2*dist/res) * ~2 texels spread

	float phi = 6.2831853 * shadowHash( gl_FragCoord.xy );
	vec3 txr = tx * r;
	vec3 tyr = ty * r;
	float sum = 0.0;
	for ( int i = 0; i < taps; i++ ) {
		vec2 d = vogelDisc( i, taps, phi );
		sum += texture( cube, vec4( L + txr * d.x + tyr * d.y, ref ) );
	}
	return sum / float( taps );
}

float shadowVisibility() {
	if ( u_shadowParms.x == 0.0 ) {
		return 1.0;
	}
	// Slope-scaled depth bias. A constant bias can't span the receiver's depth
	// change across one shadow texel once the light grazes the surface (small
	// angle between the light ray and the plane), so grazing floors/walls keep
	// banded acne that only changes stripe *width* when the flat bias is retuned.
	// Scale the bias by tan(theta) of the geometric surface-to-light angle: the
	// tangent-space geometric normal is (0,0,1), so the normalized light vector's
	// z is cos(theta) -- bump-map independent, keeping the added bias smooth.
	// r_shadowMapSlopeBias (u_pbrParms2.y) is the strength; 0 restores the old
	// constant bias. cos is floored so the tan term stays bounded at true grazing.
	float cosT = clamp( normalize( var_TexLightVec ).z, 0.15, 1.0 );
	float biasScale = 1.0 + u_pbrParms2.y * ( sqrt( 1.0 - cosT * cosT ) / cosT );
	float depthBias = u_shadowParms.z * biasScale;
	if ( u_shadowParms.x > 1.5 && u_shadowParms.x < 2.5 ) {
		// point light: the caster stored linear radial distance/range as depth, so
		// compare the same quantity here.
		vec3 L = var_ShadowCubeVec;
		float dist = length( L );
		float ref = dist / max( u_shadowParms.w, 1.0 ) - depthBias;
		float vis = sampleCubeShadow( u_shadowCube, L, ref, dist );
		// DUDE static/dynamic split (lever B): when this light has a dynamic (movers-only) cube
		// layer, min the two — the nearest occluder across both is identical to one combined cube.
		// u_pbrParms2.z = hasDynamicLayer (0 = no dynamic layer -> the second sample is skipped).
		if ( u_pbrParms2.z > 0.5 ) {
			vis = min( vis, sampleCubeShadow( u_shadowCubeDyn, L, ref, dist ) );
		}
		return vis;
	}
	if ( var_ShadowProjection.w <= 0.0 ) {
		return 1.0;						// behind the light apex -> lit
	}
	// raw (unbaked) projection UV: the shadow map was rendered with the raw light
	// projection, so sampling with var_TexProjection (which bakes in a rotating fan
	// gobo's texture matrix) would slide the shadow across a static depth field.
	vec2 uv = var_ShadowProjection.xy / var_ShadowProjection.w;	// in [0,1]
	if ( uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ) {
		return 1.0;						// outside the shadow frustum -> lit
	}
	// compare reference: the ordinary 2D path reuses the light's falloff texgen (the
	// caster wrote the same plane as depth); the sun path (mode 3 — a per-view virtual
	// projection over an oversize-omni/parallel light) has no usable light falloff, so
	// its reference is the virtual projection's own depth plane (var_ShadowProjection.z,
	// from u_shadowFalloffS — again the exact plane its caster pass wrote). The sun ref
	// is CLAMPED to the map's depth range: receivers beyond the fitted region get ref 1
	// ("lit unless a real in-range caster is nearer"). VK shadow targets are float depth,
	// which the spec compares UNCLAMPED — without this, everything past the far plane
	// reads ref > 1 vs a cleared 1.0 map and turns into a false shadow curtain (GL3's
	// unorm depth clamps implicitly, masking the bug on that backend).
	float ref = ( ( u_shadowParms.x > 2.5 ) ? clamp( var_ShadowProjection.z, 0.0, 1.0 ) : var_TexFalloff.x ) - depthBias;

	// 4-tap rotated-Vogel spread (same placement scheme as the cube path above)
	float phi = 6.2831853 * shadowHash( gl_FragCoord.xy );
	float sum = 0.0;
	for ( int i = 0; i < 4; i++ ) {
		sum += texture( u_shadowMap, vec3( uv + vogelDisc( i, 4, phi ) * u_shadowParms.y, ref ) );
	}
	return sum * 0.25;
}

// DUDE parallax occlusion mapping (docs/parallax.md). Marches the height field along the
// tangent-space view ray and returns a UV offset that fakes per-pixel surface relief. The
// height map's R channel is the surface height (1 = top, 0 = deepest); we march in "depth"
// = 1 - height. Fragment-only: no depth is written, so the flat zfill prepass and
// GLS_DEPTHFUNC_EQUAL are untouched -- the whole reason this needs no tessellation-style
// two-pass matching. u_parallaxParms = (enable, depth, minSteps, maxSteps).
// The height map is sampled with textureGrad using the base-UV derivatives (dPx, dPy)
// computed once in uniform control flow: plain texture() inside the march loop derives its
// mip level from the data-dependent per-step UV, which bands into false lines on surfaces
// that recede from the view. Fixed gradients keep the LOD stable across the whole march.
vec2 parallaxUV( vec2 uv, vec3 viewTS, vec2 dPx, vec2 dPy ) {
	float depth = u_parallaxParms.y;
	// step count ramps with view angle: cheap head-on, more taps at grazing where the
	// swimming is worst. viewTS.z = cos(angle between view ray and surface normal).
	float nSteps = mix( u_parallaxParms.w, u_parallaxParms.z, clamp( abs( viewTS.z ), 0.0, 1.0 ) );
	float layerDepth = 1.0 / nSteps;
	// max UV shift at full depth, along the view ray's tangent-plane projection
	vec2 P = ( viewTS.xy / max( abs( viewTS.z ), 0.1 ) ) * depth;
	vec2 deltaUV = P * layerDepth;

	float curLayer = 0.0;
	vec2 curUV = uv;
	float curDepthVal = 1.0 - textureGrad( u_parallaxMap, curUV, dPx, dPy ).r;
	// constant cap keeps the loop uniform-bounded across drivers; nSteps <= 32 (cvar clamp)
	for ( int i = 0; i < 32; i++ ) {
		if ( curLayer >= curDepthVal ) {
			break;
		}
		curUV -= deltaUV;
		curDepthVal = 1.0 - textureGrad( u_parallaxMap, curUV, dPx, dPy ).r;
		curLayer += layerDepth;
	}
	// occlusion refinement: interpolate between the last two layers for a smooth hit point
	vec2 prevUV = curUV + deltaUV;
	float after = curDepthVal - curLayer;
	float before = ( 1.0 - textureGrad( u_parallaxMap, prevUV, dPx, dPy ).r ) - ( curLayer - layerDepth );
	float w = after / ( after - before );
	// Re-centre the offset on the height field's mid-level (P * 0.5): the raw march recesses
	// everything below the polygon (peak at the surface), which looks like the edge floats
	// above sunken detail. Biasing to mid-height puts peaks proud of the average and troughs
	// below it -- natural at silhouette edges. Still UV-only (no real protrusion, silhouettes
	// stay flat); 0.5 is the calibrated sweet spot (in-engine A/B).
	return ( mix( curUV, prevUV, w ) - uv ) + P * 0.5;
}

// DUDE parallax self-shadowing (docs/parallax.md). From the marched surface point, step
// toward the light through the height field; where the field rises above the ray the point
// is in its own shadow. Soft: keep the deepest distance-weighted penetration. lightTS = the
// tangent-space light vector (toward the light); hitDepth = 1 - height at the surface point.
// Returns light visibility, 1 = lit.
float parallaxShadow( vec2 uv, vec3 lightTS, float hitDepth, vec2 dPx, vec2 dPy ) {
	if ( lightTS.z <= 0.05 ) {
		return 1.0;			// light at/below the surface horizon -- N.L already darkens it
	}
	// half the view march's step count: this is a second march per lit pixel and soft
	// contact shadows tolerate coarser sampling far better than the silhouette does.
	float nSteps = max( 0.5 * mix( u_parallaxParms.w, u_parallaxParms.z, clamp( lightTS.z, 0.0, 1.0 ) ), 4.0 );
	float layerDepth = hitDepth / nSteps;					// divide the climb to the top into steps
	vec2  deltaUV = ( lightTS.xy / lightTS.z ) * u_parallaxParms.y * layerDepth;
	float d = hitDepth;
	float shadow = 0.0;
	for ( int i = 0; i < 32; i++ ) {
		d -= layerDepth;
		if ( d <= 0.0 ) {
			break;
		}
		uv += deltaUV;
		float dm = 1.0 - textureGrad( u_parallaxMap, uv, dPx, dPy ).r;
		if ( dm < d ) {										// map surface rises above the ray -> occludes
			shadow = max( shadow, ( d - dm ) * ( 1.0 - float( i ) / nSteps ) );
		}
	}
	return clamp( 1.0 - shadow * 6.0, 0.0, 1.0 );			// gain to bring the soft shadow into range
}

void main() {
	// DUDE parallax occlusion mapping: march the height map along the tangent-space view
	// ray and shift all surface UVs by the result before any surface fetch. Off (enable 0)
	// leaves the vanilla UVs untouched. Applied to bump/diffuse/specular alike, which share
	// the surface parametrisation (docs/parallax.md).
	vec2 uvBump = var_TexBump;
	vec2 uvDiffuse = var_TexDiffuse;
	vec2 uvSpecular = var_TexSpecular;
	float parallaxSelfShadow = 1.0;
	if ( u_parallaxParms.x > 0.5 ) {
		// base-UV derivatives, taken once in uniform control flow so the height-map fetches
		// inside the marches use a stable mip level (see parallaxUV) instead of banding.
		vec2 dPx = dFdx( var_TexBump );
		vec2 dPy = dFdy( var_TexBump );
		vec2 off = parallaxUV( var_TexBump, normalize( var_TexViewVec ), dPx, dPy );
		uvBump += off;
		uvDiffuse += off;
		uvSpecular += off;
		// self-shadow the direct light: march from the hit point toward the light
		if ( u_parallaxParms2.x > 0.0 ) {
			float hitDepth = 1.0 - textureGrad( u_parallaxMap, uvBump, dPx, dPy ).r;
			float vis = parallaxShadow( uvBump, normalize( var_TexLightVec ), hitDepth, dPx, dPy );
			parallaxSelfShadow = mix( 1.0, vis, u_parallaxParms2.x );
		}
	}

	// RXGB (DXT5nm) swizzle: x lives in alpha; deliberately NOT renormalized,
	// mip filtering shortens the vector and self-shadows rough surfaces less
	vec4 bump = texture( u_bumpMap, uvBump );
	bump.x = bump.a;
	vec3 localNormal = bump.xyz * 2.0 - 1.0;

	// diffuse
	vec4 diffuse = texture( u_diffuseMap, uvDiffuse ) * u_diffuseModifier;

	// lightScale: N.L and the depth-map shadow visibility (1 for stencil-shadowed
	// and unshadowed lights, u_shadowParms.x == 0) fold into one scalar that
	// scales the light projection / falloff product below.
	float lightScale;
	vec4 spec;

	if ( u_pbrParms.z > 0.5 ) {
		// DUDE PBR path (docs/pbr-materials.md Phase A): Cook-Torrance GGX with a
		// metalness workflow, superseding the r_shading models while r_pbr is on.
		// Same tangent-space vectors as vanilla, but normalized analytically (the
		// 8-bit normalization cubemap is below GGX's precision needs). Deliberate
		// conventions:
		//  - No 1/pi on Lambert, no pi on the NDF: both cancel against Doom 3's
		//    non-physical light values, so diffuse brightness matches vanilla
		//    exactly at metalness 0.
		//  - The stock specular map has no unique PBR interpretation, so it stays
		//    a per-texel *mask* on the lobe (spec * specMap in the shared combine
		//    below), doubled like vanilla's "spec map * 2" convention so id's
		//    mid-gray authoring means full strength. r_specularScale/r_specularExp
		//    (u_specularParms.xy) are deliberately not read here.
		//  - Toksvig: the bump fetch above is deliberately un-renormalized, so its
		//    sub-unit length measures normal variance over the mip footprint; fold
		//    it into GGX alpha^2 for free per-texel roughness on stock assets.
		float nLen = clamp( length( localNormal ), 1e-4, 1.0 );
		vec3 N = localNormal / nLen;
		vec3 L = normalize( var_TexLightVec );
		vec3 V = normalize( var_TexViewVec );
		vec3 H = normalize( L + V );
		float NdotL = max( dot( N, L ), 0.0 );
		float NdotV = max( dot( N, V ), 1e-4 );
		float NdotH = max( dot( N, H ), 0.0 );
		float VdotH = max( dot( V, H ), 0.0 );

		float rough = clamp( u_pbrParms.y, 0.03, 1.0 );
		float alpha = rough * rough;
		// Toksvig widening with a calibrated baseline (in-game A/B, 2026-07-30):
		// on stock DXT5nm assets the normal-length variance carries a codec-noise
		// floor (compressed normals decode short of unit even at the top mip), and
		// full Toksvig over-widened every highlight. The baseline subtraction
		// cancels that floor while keeping the response to *real* normal variance
		// — seam edges, grate lips, minified detail — which is Toksvig's actual
		// job: those texels are exactly the specular-aliasing "fireflies".
		// The baseline lives in u_localParam1.z (r_pbrToksvigBase, default 0.2,
		// tunable in the Developer tab). Calibration history: 0.5 disabled the
		// mechanism -> white firefly pixels on panel seams; 0.1 killed the
		// fireflies but widened ordinary wall texels (|N| ~0.85-0.9), halving
		// the highlight peak ("no kick" in the Blinn A/B); 0.2 is the split —
		// |N| > ~0.83 stays tight, seams (|N| < ~0.75) keep the widening.
		float variance = max( ( 1.0 - nLen ) / nLen - u_localParam1.z, 0.0 );
		float alpha2 = min( alpha * alpha + variance, 1.0 );

		float metal = u_pbrParms.x;
		vec3 F0 = mix( vec3( 0.04 ), diffuse.rgb, metal );
		vec3 F = F0 + ( 1.0 - F0 ) * pow( 1.0 - VdotH, 5.0 );
		float d = NdotH * NdotH * ( alpha2 - 1.0 ) + 1.0;
		float D = alpha2 / ( d * d );				// GGX NDF, pi folded out (see above)
		float k = 0.5 * sqrt( alpha2 );				// Schlick-GGX k for direct light, widened alpha
		float vis = 0.25 / ( ( NdotL * ( 1.0 - k ) + k ) * ( NdotV * ( 1.0 - k ) + k ) );	// = G / (4 N.L N.V)

		// firefly clamp: GGX's peak (~1/alpha^2, plus the grazing-angle vis
		// blow-up) sits orders of magnitude above the bounded 8-bit-era energy
		// these assets were authored against (vanilla Blinn tops out ~2.4x).
		// Isolated normal-map texels aligning with H spike past the display
		// range and clip to flat white — "rows of white pixels" on panel seams
		// and grate lips. Bounding the scalar lobe restores gradation. The
		// ceiling lives in u_localParam1.w (r_pbrFireflyClamp, default 6:
		// leaves the calibrated dielectric peak ~2.2 at roughness 0.58
		// untouched, tames tight/bare-metal spikes; skin at roughness 0.40
		// rides *at* the ceiling by design — its Blinn-like bounded core).
		float lobe = min( D * vis, u_localParam1.w );

		// u_pbrParms.w = r_pbrSpecScale, the artistic energy knob for this path,
		// folded with the vanilla "spec map * 2" convention like the branch below.
		// Dielectric-weighted: the scale exists to compensate the gamma-space
		// dimming of the physical 4% dielectric F0, but metal F0 comes from the
		// albedo — already display-referred, already bright — so applying the
		// boost there double-counts and blows out bare-metal highlights (the
		// grate-floor screenshot, 2026-07-30). Metals fade to scale 1.
		spec = vec4( lobe * F, 1.0 ) * u_specularModifier
		     * ( mix( u_pbrParms.w, 1.0, metal ) * 2.0 );

		// Phase C.1 environment floor (docs/pbr-materials.md sec. 5): stock D3
		// has no environment probes to reflect (the "ambient cubemap" is a
		// constant direction, not colors), so metals with their diffuse killed
		// went black wherever the GGX alignment missed. Approximate the
		// environment as the current light's own energy arriving from all
		// directions: an F0-tinted, metalness-weighted floor added under the
		// lobe. It rides the shared `light * color` combine below, so it
		// scales with the light's projection/falloff/shadow (metals glow near
		// lights, stay dark in darkness — Doom 3's aesthetic preserved) and is
		// softened by the roughness. u_occlusionParms.w = r_pbrEnvScale.
		spec.rgb += F0 * ( metal * u_occlusionParms.w * ( 1.0 - 0.5 * rough ) );

		// Fresnel-weighted diffuse. Physical PBR kills diffuse entirely on metals
		// (metal 1 -> factor 0), but that pushes the asset's painted colour into
		// reflections stock Doom 3 can't supply, so metals read dark and off-colour.
		// u_pbrParms2.x = kd (= 1 - r_pbrMetalDiffuse) relaxes the kill: kd < 1 keeps
		// that fraction of the albedo colour while the metallic specular still rides
		// on top. kd 1 = physical, kd 0 = full albedo retained (docs sec. 5).
		diffuse.rgb *= ( 1.0 - F ) * ( 1.0 - metal * u_pbrParms2.x );
		lightScale = NdotL * shadowVisibility();
	} else {
		// half angle is normalized with math (matches the ARB program, which
		// deliberately avoided the normalization cubemap here)
		vec3 specularV = normalize( var_TexHalfVec );

		// light vector through the normalization cube map, as the original did
		vec3 lightV = texture( u_normalCubeMap, var_TexLightVec ).xyz * 2.0 - 1.0;

		lightScale = dot( lightV, localNormal ) * shadowVisibility();

		// specular term. Shading model selected by u_specularParms.z:
		//   0 = vanilla dependent LUT read on N.H (faithful default)
		//   1 = analytic Blinn-Phong pow(N.H, exp)
		// u_specularParms.x scales the result (1 = vanilla), .y is the exponent.
		int shadingModel = int( u_specularParms.z + 0.5 );
		if ( shadingModel == 0 ) {
			float sDot = dot( specularV, localNormal );
			spec = texture( u_specularTable, vec2( sDot, sDot ) );
		} else {
			// the analytic model wants a unit normal (localNormal is deliberately
			// left un-renormalized above for the diffuse/LUT path)
			vec3 nSpec = normalize( localNormal );
			float rawDot = max( dot( specularV, nSpec ), 0.0 );
			spec = vec4( pow( rawDot, u_specularParms.y ) );
		}
		// the vanilla "specular map * 2" scale is folded into the scalar factor here
		spec *= u_specularModifier * ( u_specularParms.x * 2.0 );
	}

	vec4 light = textureProj( u_lightProjection, var_TexProjection )
	           * texture( u_lightFalloff, var_TexFalloff )
	           * lightScale * parallaxSelfShadow;

	vec4 specMap = texture( u_specularMap, uvSpecular );

	// DUDE GTAO on direct light (docs/ssao-gtao.md Phase C). Doom 3 is almost all dynamic
	// light with ~no ambient, so occluding the ambient pass alone is invisible; this
	// grounds direct-lit surfaces too. Applied to the diffuse term (and, with specular
	// occlusion on, the specular), scaled by r_ssaoDirectLight -- a light moving into a
	// crease can't re-light AO that's baked into the surface, so keeping it below full is
	// safer. u_localParam0 = (enable, floor, 1/viewW, 1/viewH); u_localParam1 = (direct
	// strength, specular-occlusion toggle). The AO term is floored so it never blackens.
	if ( u_localParam0.x > 0.5 ) {
		float ao = texture( u_ssao, gl_FragCoord.xy * u_localParam0.zw ).r;
		ao = mix( u_localParam0.y, 1.0, ao );
		float aoDirect = mix( 1.0, ao, u_localParam1.x );
		diffuse.rgb *= aoDirect;
		if ( u_localParam1.y > 0.5 ) {
			spec.rgb *= aoDirect;
		}
	}

	// DUDE baked ambient-occlusion map (docs/occlusion-maps.md). Same rationale as SSAO on
	// direct light: a baked-dark crease can't be re-lit by a moving light, so the map's pull
	// on direct diffuse is scaled (u_occlusionParms.z, from r_occlusionMapScale * direct) and
	// stays below full by default. Sampled with the diffuse UV; stacks with SSAO when both on.
	if ( u_occlusionParms.x > 0.5 ) {
		float aoMap = texture( u_occlusionMap, uvDiffuse ).r;
		diffuse.rgb *= mix( 1.0, aoMap, u_occlusionParms.z );
	}

	vec4 color = spec * specMap + diffuse;

	// Floor negatives to 0 to match the 8-bit target's fixed-point clamp. N.L (the
	// `light` term above) goes negative on pixels facing away from this light; on the
	// SDR backbuffer that's clamped to 0 before the additive blend, but the RGBA16F
	// HDR target doesn't clamp fragment output, so a negative would *subtract* this
	// (often warm) light and cool-shift normal-mapped models — the r_hdr blue-tint
	// bug. max() restores the [0, inf) floor while keeping HDR's >1 highlights; it's a
	// no-op on the 8-bit path, which already floored here.
	fragColor = max( light * color * var_Color, vec4( 0.0 ) );
}
