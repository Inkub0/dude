// DUDE parallax occlusion mapping, shared by interaction.frag (forward lighting) and gbuffer.frag (the G-buffer the
// Path Tracing tier lights from), so both shift the surface identically (docs/parallax.md). The includer declares
// u_parallaxMap (unit 11) before including this. u_parallaxParms = (enable, depth, minSteps, maxSteps).

// The height at uv, 1 = top. u_parallaxParms2.y picks the source (RB_RHI_ParallaxSource): 0 = the material's runtime
// field in R (bump texture space), 1 = an ORM / RMAO sidecar's ALPHA (diffuse texture space, docs/pbr-texture-maps.md).
float dudeParallaxHeight( vec2 uv, vec2 dPx, vec2 dPy ) {
	vec4 t = textureGrad( u_parallaxMap, uv, dPx, dPy );
	return ( u_parallaxParms2.y > 0.5 ) ? t.a : t.r;
}

// Per-axis scale of the march depth. A sidecar height's depth is baked relative to the texture WIDTH (texels of relief
// / width, tools/pbr_extract_maps.py), so along v it is width / height of that: a 32 x 256 trim's relief spans 8x fewer
// v units than u units. The runtime field's depth is a tuned, non-physical knob and stays the same on both axes.
vec2 dudeParallaxAxisScale() {
	if ( u_parallaxParms2.y < 0.5 ) {
		return vec2( 1.0 );
	}
	vec2 ts = vec2( textureSize( u_parallaxMap, 0 ) );
	return vec2( 1.0, ts.x / max( ts.y, 1.0 ) );
}

// Carry a UV offset found in one stage's texture space (src: the height's) into another stage's (dst). Every stage's
// coordinates are an affine map of the same surface st, so dst = J_dst * inverse( J_src ) * off, with each Jacobian
// built from that UV's screen derivatives - constant over the surface, exact for a scaled / rotated stage (sofloor1's
// bump is scale 2, 2). The same space (the usual case) returns off untouched; so does an edge-on (degenerate) src.
vec2 dudeParallaxCarry( vec2 off, vec2 srcDx, vec2 srcDy, vec2 dstDx, vec2 dstDy ) {
	if ( srcDx == dstDx && srcDy == dstDy ) {
		return off;
	}
	mat2 js = mat2( srcDx, srcDy );
	float det = determinant( js );
	if ( abs( det ) <= 1e-6 * length( srcDx ) * length( srcDy ) ) {
		return off;
	}
	return mat2( dstDx, dstDy ) * ( inverse( js ) * off );
}

// How many march steps a ray of UV length P needs: one per ~2 texels of the height map or ~2 screen pixels, whichever
// is fewer (a finer march resolves nothing either can show), clamped to [lo, hi]. A fixed count per view angle (the
// old ramp) undersamples a deep field - 16 steps over a grille's 136-texel grazing march jump 8 texels, past whole
// slot walls, so neighbouring pixels land in different layers ("stacked strips") - and oversamples a shallow one.
float dudeParallaxSteps( vec2 P, vec2 dPx, vec2 dPy, float lo, float hi ) {
	float n = length( P * vec2( textureSize( u_parallaxMap, 0 ) ) );
	mat2 j = mat2( dPx, dPy );									// UV per screen pixel
	float det = determinant( j );
	if ( abs( det ) > 1e-6 * length( dPx ) * length( dPy ) ) {
		n = min( n, length( inverse( j ) * P ) );				// the march's length on screen, in pixels
	}
	return clamp( ceil( n * 0.5 ), lo, hi );
}

// DUDE parallax occlusion mapping (docs/parallax.md). Marches the height field along the
// tangent-space view ray and returns a UV offset that fakes per-pixel surface relief. The
// height (dudeParallaxHeight: 1 = top, 0 = deepest) is marched as "depth" = 1 - height.
// Fragment-only: no depth is written, so the flat zfill prepass and GLS_DEPTHFUNC_EQUAL
// are untouched -- the whole reason this needs no tessellation-style two-pass matching.
// u_parallaxParms = (enable, depth, minSteps, maxSteps).
// The height map is sampled with textureGrad using the base-UV derivatives (dPx, dPy)
// computed once in uniform control flow: plain texture() inside the march loop derives its
// mip level from the data-dependent per-step UV, which bands into false lines on surfaces
// that recede from the view. Fixed gradients keep the LOD stable across the whole march.
vec2 parallaxUV( vec2 uv, vec3 viewTS, vec2 dPx, vec2 dPy ) {
	float depth = u_parallaxParms.y;
	// max UV shift at full depth, along the view ray's tangent-plane projection
	vec2 P = ( viewTS.xy / max( abs( viewTS.z ), 0.1 ) ) * depth * dudeParallaxAxisScale();
	float nSteps = dudeParallaxSteps( P, dPx, dPy, u_parallaxParms.z, u_parallaxParms.w );
	float layerDepth = 1.0 / nSteps;
	vec2 deltaUV = P * layerDepth;

	float curLayer = 0.0;
	vec2 curUV = uv;
	float curDepthVal = 1.0 - dudeParallaxHeight( curUV, dPx, dPy );
	// constant cap keeps the loop uniform-bounded across drivers; nSteps <= 128 (cvar clamp), and the ray reaches the
	// deepest layer (1) after nSteps, where every height is hit
	for ( int i = 0; i < 128; i++ ) {
		if ( curLayer >= curDepthVal ) {
			break;
		}
		curUV -= deltaUV;
		curDepthVal = 1.0 - dudeParallaxHeight( curUV, dPx, dPy );
		curLayer += layerDepth;
	}
	// refinement: the hit lies between the last step above the field (a) and the first below it (b). Halve that bracket
	// a few times (relief mapping's binary search: sharp slot walls instead of a smeared ramp), then interpolate inside
	// what is left for a smooth hit point.
	vec2  aUV = curUV + deltaUV;
	float aLayer = curLayer - layerDepth;
	float aDepth = 1.0 - dudeParallaxHeight( aUV, dPx, dPy );
	vec2  bUV = curUV;
	float bLayer = curLayer;
	float bDepth = curDepthVal;
	for ( int k = 0; k < 5; k++ ) {
		vec2  mUV = 0.5 * ( aUV + bUV );
		float mLayer = 0.5 * ( aLayer + bLayer );
		float mDepth = 1.0 - dudeParallaxHeight( mUV, dPx, dPy );
		if ( mLayer >= mDepth ) {
			bUV = mUV; bLayer = mLayer; bDepth = mDepth;
		} else {
			aUV = mUV; aLayer = mLayer; aDepth = mDepth;
		}
	}
	float after = bDepth - bLayer;			// <= 0: b is below the field
	float before = aDepth - aLayer;			// > 0: a is above it
	float den = after - before;
	float w = ( abs( den ) > 1e-6 ) ? after / den : 0.0;
	// Re-centre the offset on the height level that stays on the polygon, u_parallaxParms2.z: the raw march recesses
	// everything below the polygon (peak at the surface), which looks like the edge floats above sunken detail.
	// Anchoring a level there puts what is higher proud of the surface and what is lower behind it. The runtime field
	// anchors mid-height (0.5, the calibrated sweet spot of an in-engine A/B on that field); a sidecar height anchors
	// its main face (RB_RHI_ParallaxSource), so a panel's front stays glued to the wall and only its vents move.
	// Still UV-only (no real protrusion, silhouettes stay flat).
	return ( mix( bUV, aUV, w ) - uv ) + P * ( 1.0 - u_parallaxParms2.z );
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
	// the climb from the hit point to the top of the field, sampled like the view march (dudeParallaxSteps) but at
	// half its cap: a second march per lit pixel, and soft contact shadows tolerate coarser sampling far better than
	// the silhouette does
	vec2  climb = ( lightTS.xy / lightTS.z ) * u_parallaxParms.y * hitDepth * dudeParallaxAxisScale();
	float nSteps = dudeParallaxSteps( climb, dPx, dPy, 4.0, max( 0.5 * u_parallaxParms.w, 4.0 ) );
	float layerDepth = hitDepth / nSteps;					// divide the climb to the top into steps
	vec2  deltaUV = climb / nSteps;
	float d = hitDepth;
	float shadow = 0.0;
	for ( int i = 0; i < 64; i++ ) {
		d -= layerDepth;
		if ( d <= 0.0 ) {
			break;
		}
		uv += deltaUV;
		float dm = 1.0 - dudeParallaxHeight( uv, dPx, dPy );
		if ( dm < d ) {										// map surface rises above the ray -> occludes
			shadow = max( shadow, ( d - dm ) * ( 1.0 - float( i ) / nSteps ) );
		}
	}
	return clamp( 1.0 - shadow * 6.0, 0.0, 1.0 );			// gain to bring the soft shadow into range
}
