// DUDE RT GI one-bounce gather, shared (docs/rtx-rtgi.md H6a-H6d): ONE cosine-weighted hemisphere ray from a world
// point against the scene TLAS, the hit shaded cheaply (geo-table fetch -> bindless albedo -> scene lights with
// their real cookie/falloff textures -> per-pixel emissive -> one dominant-light shadow ray) and the incoming
// radiance returned. Lifted VERBATIM from rtgi_ray.frag so RB_RHI_RtgiPass (its own pass) and the ReSTIR DI
// gather (restir_di.frag, r_rtReStirDICombineGI: direct + indirect in ONE buffer, ONE NRD denoise) share one
// implementation that cannot drift. The caller owns the uniform packing and fills RtgiBounceParms.
//
// Needs: renderparms.glsl, rt_light_eval.glsl, rt_light_tex.glsl, rt_hash_grid.glsl + rtgi_cache.glsl (the irradiance
// cache the hit reads, docs/rtx-irradiance-cache.md), GL_EXT_ray_query, GL_EXT_buffer_reference_uvec2,
// GL_EXT_nonuniform_qualifier.
// idDrawVert as raw uints (stride 60 B = 15 uints): xyz @0, st @3, normal @5. Same view as ssr_rt.frag.
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer VertRef { uint w[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer IdxRef  { uint i[]; };
struct GeoDesc { VertRef vb; IdxRef ib; uint stride; uint flags; uint baseColor; uint texIndex; };	// RR0/RR3/RR4 geometry table row
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer GeoTable { GeoDesc d[]; };

struct RtgiBounceParms {
	uvec2 tlas;				// scene TLAS device address (bit-cast)
	uvec2 geoTable;			// RR0 geometry table device address
	uvec2 lightAddr;		// scene-light buffer device address
	uint  nLights;
	bool  realLights;		// shade the hit with the scene lights (else ambient floor + emissive only)
	bool  gate;				// confine each light to its projection volume
	bool  texturedLights;	// r_rtLightTextures: cookie * falloff attenuation
	bool  shadowRay;		// H6d: one shadow ray toward the dominant light at the hit
	bool  skipForwardLights;	// skip DUDE_LIGHT_DIFORWARD lights (fast-changing: flashlight, flashes, projectiles) - their bounce would trail in the history
	float radius;			// gather ray length
	float emissiveScale;	// r_rtgiEmissive
	float emissiveClamp;	// r_rtgiEmissiveClamp (peak channel)
	float lightScale;		// r_rtgiLightScale
	float ambientFloor;		// u_color.w in RB_RHI_RtgiPass (0 = pure bounce)
	float eyePullback;		// ray origin pulled toward the eye per unit depth (crease-leak fix)
	vec2  ignRot;			// per-frame golden-angle IGN lattice rotation (cos, sin), CPU-hoisted
	vec2  ignOfs;			// per-frame hashed IGN offset, CPU-hoisted
	// RT irradiance cache (docs/rtx-irradiance-cache.md C2): when cacheLookup, the hit also receives the INDIRECT light
	// its cell has learned (albedo(H) * E_cache(H) * scale) - the recursion that makes the one bounce multi-bounce. The
	// caller fills `cache` from the shared uniform packing (rtgi_cache.glsl) and does the UPDATE at its primary point.
	bool  cacheLookup;
	// C6 TAIL mode: when cacheTail, the cell stores the surface's OUTGOING radiance and a converged + fresh cell IS the
	// bounce's path tail - the hit reads L_out * scale + emissive(H) and skips the albedo sample, the light loop and the
	// dominant shadow ray (cheaper gather, and the bounce the denoiser sees is cell + frame averaged, not 1-spp direct).
	// cacheTail and cacheLookup are mutually exclusive in effect: tail mode has no separate indirect add (the C2 block below
	// stands down). Only ever set by the DI gather (diApply && r_rtgiCacheTail); the standalone RTGI pass keeps cacheLookup.
	bool  cacheTail;
	RtgiCacheParms cache;
};

// interleaved gradient noise (ssao.frag): per-pixel dither, denoiser-friendly 3x3 spread.
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

// Trace + shade the bounce for the surface point Pw / normal Nw (world), seen from eyeW at view depth d, dithered
// by the SCREEN pixel sfrag. Returns 0 = no bounce (miss / degenerate; Li = 0), 1 = shaded hit (Li = incoming
// radiance, thit = hit distance), 2 = hit on an unshadeable row (Li = 0; rtgi_ray.frag keeps its alpha-1 output).
// LiCache = the part of Li the irradiance cache may LEARN at the caller's primary point: everything but the direct
// contribution of DUDE_LIGHT_DIFORWARD lights (flashlight, muzzle flashes, projectiles - fast lights whose bounce
// would otherwise lag in the cache for Nmax frames). Equals Li when bp.skipForwardLights (the DI gather).
int rtgiTraceBounce( RtgiBounceParms bp, vec3 Pw, vec3 Nw, vec3 eyeW, float d, vec2 sfrag, out vec3 Li, out float thit, out vec3 outDir, out vec3 LiCache ) {
	Li = vec3( 0.0 );
	LiCache = vec3( 0.0 );
	thit = 0.0;
	outDir = Nw;
	// one cosine-weighted hemisphere direction, IGN re-oriented per frame (rtao_ray.frag: a value-phase
	// slide leaves IGN's 45-degree diagonal in the residue and shows as lines under the res-scale upscale;
	// re-orienting points each frame's residue a different way so the temporal accumulation averages it out).
	// H6g: ROTATE the IGN sample lattice by a per-frame GOLDEN-ANGLE instead of the old discrete 8-way flip
	// (swap + 2 negations). The flip has only 8 orientations, so ~8 traces are needed to average the diagonal
	// out; checkerboard traces each pixel only every OTHER frame, so that was ~16 frames (the "45-degree
	// checkered halo" at short history). A continuous low-discrepancy rotation points the diagonal a fresh way
	// every frame, so the residue averages in a handful of frames — helping the resScale path too.
	// The golden-angle rotation (cos/sin) and the per-frame hashed offset are FRAME-CONSTANT (functions of the
	// frame index only), so they are precomputed on the CPU (RB_RHI_RtgiPass) and delivered in u_particleRadius
	// (.xy = cos/sin of the angle, .zw = the offset) instead of a per-pixel cos+sin+PCG-hash. Mathematically the
	// same lattice; just not recomputed identically for every traced fragment.
	vec2  jc = mat2( bp.ignRot.x, -bp.ignRot.y, bp.ignRot.y, bp.ignRot.x ) * sfrag + bp.ignOfs;
	float r1 = ign( jc );
	float r2 = ign( jc.yx + vec2( 37.0, 17.0 ) );
	float phi  = 6.28318530718 * r1;
	float sinT = sqrt( r2 );
	float cosT = sqrt( 1.0 - r2 );
	vec3 T, B;
	onb( Nw, T, B );
	vec3 dir = T * ( cos( phi ) * sinT ) + B * ( sin( phi ) * sinT ) + Nw * cosT;
	float dl2 = dot( dir, dir );
	if ( !( dl2 > 1e-6 ) ) { return 0; }
	dir *= inversesqrt( dl2 );
	outDir = dir;

	// ray origin pulled toward the eye (rtao_ray.frag crease-leak fix) + a tiny normal lift
		vec3  rayOrg = Pw + normalize( eyeW - Pw ) * ( 0.25 + d * bp.eyePullback ) + Nw * 0.05;
	float radius = bp.radius;
	rayQueryEXT rq;
	rayQueryInitializeEXT( rq,
		accelerationStructureEXT( bp.tlas ),
		gl_RayFlagsOpaqueEXT, 0xFDu /* solid rays: skip perforated-caster instances (mask 0x02) */, rayOrg, 0.0, dir, radius );
	while ( rayQueryProceedEXT( rq ) ) { }
	if ( rayQueryGetIntersectionTypeEXT( rq, true ) != gl_RayQueryCommittedIntersectionTriangleEXT ) {
		return 0;						// miss inside the gather radius: open space contributes no bounce here
	}
	thit = rayQueryGetIntersectionTEXT( rq, true );

	// ---- shade the hit (lifted from ssr_rt.frag): geo-table row -> interpolate -> bindless albedo -> scene lights -> emissive
	uint ci = uint( rayQueryGetIntersectionInstanceCustomIndexEXT( rq, true ) );
	uint gi = uint( rayQueryGetIntersectionGeometryIndexEXT( rq, true ) );
	GeoTable table = GeoTable( bp.geoTable );
	GeoDesc g = table.d[ ci + gi ];
	if ( ( g.flags & 1u ) == 0u ) { return 2; }	// no-attr defer row (mover/prop): not shadeable yet

	uint prim = uint( rayQueryGetIntersectionPrimitiveIndexEXT( rq, true ) );
	vec2 bc   = rayQueryGetIntersectionBarycentricsEXT( rq, true );
	mat4x3 o2w = rayQueryGetIntersectionObjectToWorldEXT( rq, true );
	uint s  = g.stride >> 2u;
	uint p3 = 3u * prim;
	uint b0 = g.ib.i[ p3 + 0u ] * s;
	uint b1 = g.ib.i[ p3 + 1u ] * s;
	uint b2 = g.ib.i[ p3 + 2u ] * s;
	float w0 = 1.0 - bc.x - bc.y;
	vec3 n0 = vec3( uintBitsToFloat( g.vb.w[ b0 + 5u ] ), uintBitsToFloat( g.vb.w[ b0 + 6u ] ), uintBitsToFloat( g.vb.w[ b0 + 7u ] ) );
	vec3 n1 = vec3( uintBitsToFloat( g.vb.w[ b1 + 5u ] ), uintBitsToFloat( g.vb.w[ b1 + 6u ] ), uintBitsToFloat( g.vb.w[ b1 + 7u ] ) );
	vec3 n2 = vec3( uintBitsToFloat( g.vb.w[ b2 + 5u ] ), uintBitsToFloat( g.vb.w[ b2 + 6u ] ), uintBitsToFloat( g.vb.w[ b2 + 7u ] ) );
	vec3 hitN = normalize( mat3( o2w ) * ( w0 * n0 + bc.x * n1 + bc.y * n2 ) );
	vec2 st0 = vec2( uintBitsToFloat( g.vb.w[ b0 + 3u ] ), uintBitsToFloat( g.vb.w[ b0 + 4u ] ) );
	vec2 st1 = vec2( uintBitsToFloat( g.vb.w[ b1 + 3u ] ), uintBitsToFloat( g.vb.w[ b1 + 4u ] ) );
	vec2 st2 = vec2( uintBitsToFloat( g.vb.w[ b2 + 3u ] ), uintBitsToFloat( g.vb.w[ b2 + 4u ] ) );
	vec2 hitST = w0 * st0 + bc.x * st1 + bc.y * st2;

	// world hit (rigid ray: hitWorld = rayOrg + dir*thit). Face the hit normal back toward the ray so a
	// bounce off the inside of a single-sided wall still lights from the correct side. Hoisted above the albedo
	// sample so the C6 tail serve can read the cache and, on a served hit, skip that sample entirely.
	vec3  hitWorld = rayOrg + dir * thit;
	if ( dot( hitN, dir ) > 0.0 ) { hitN = -hitN; }

	// C6 TAIL mode (docs/rtx-irradiance-cache.md C6): if H's cell is SERVABLE (converged + fresh) its stored outgoing
	// radiance L_out IS the bounce - Li = L_out * scale + emissive(H) sampled live - and the albedo fetch, the whole
	// light loop and the dominant shadow ray are all skipped (the cheaper gather, and a cell + frame averaged bounce for
	// NRD instead of a 1-spp direct shade). emissive stays live: the cell excludes the surface's own self-illum (the hit
	// adds it here) so a converged cell of a flickering LED does not freeze its glow. A miss / unresolved cell falls
	// through to the live shade below (today's one bounce), with no cached addition - tail mode has no separate indirect.
	if ( bp.cacheTail ) {
		vec3 Lout;
		if ( rtgiCacheLookupTail( bp.cache, hitWorld, hitN, eyeW, Lout ) ) {
			Li = Lout * bp.cache.scale;
			uint emitT = ( g.flags >> 16 ) & 0xFFFFu;
			if ( emitT != 0u ) {
				vec3  e  = textureLod( u_rtTextures[ nonuniformEXT( emitT - 1u ) ], hitST, 0.0 ).rgb * bp.emissiveScale;
				float ep = max( max( e.r, e.g ), e.b );
				if ( ep > bp.emissiveClamp ) { e *= bp.emissiveClamp / ep; }
				Li += e;
			}
			LiCache = Li;
			return 1;
		}
	}

	// RR4: albedo from the hit's real diffuse (bindless slot texIndex-1), else the RR3 material average.
	// RR16g: baseColor.rgb is the diffuse average (fallback albedo only); self-illum is sampled per-pixel below.
	vec4  bcol = unpackUnorm4x8( g.baseColor );
	vec3  albedo = ( g.texIndex != 0u )
		? texture( u_rtTextures[ nonuniformEXT( g.texIndex - 1u ) ], hitST ).rgb
		: bcol.rgb;

	// Li = outgoing radiance at the hit toward the shading pixel. Diffuse only; the receiver's own albedo
	// and the 1/pi * pi (cosine-sampling) factors are applied at the H6c consume site, so here we return
	// the hit's lit colour. bp.ambientFloor is a small ambient floor (0 keeps it a pure bounce).
	Li = albedo * bp.ambientFloor;
	LiCache = Li;
	vec3  domContrib = vec3( 0.0 );			// H6d: the brightest reaching light's contribution, for one shadow ray
	vec3  domDir = vec3( 0.0 );
	float domDist = 0.0;
	float domLum = -1.0;
	bool  domFast = false;					// the dominant light is a DIFORWARD (fast) light: not part of LiCache
	if ( bp.realLights ) {												// r_rtgi real scene lights
		RtLights lights = RtLights( bp.lightAddr );
		uint nLights = bp.nLights;
		bool gate = bp.gate;
		for ( uint li = 0u; li < nLights; li++ ) {
			if ( gate && !dudeLightInRange( lights, li, hitWorld ) ) { continue; }
			const bool fast = ( lights.l[ li ].flags & DUDE_LIGHT_DIFORWARD ) != 0u;
			if ( bp.skipForwardLights && fast ) { continue; }
			vec3  ldir; float dl;
			dudeLightDir( lights, li, hitWorld, ldir, dl );
			float ndl = max( dot( hitN, ldir ), 0.0 );
			if ( ndl <= 0.0 ) { continue; }
			// the light's shape + colour at the hit: its real cookie * falloff textures (Stage G, u_windowCoord.w),
			// else the analytic frustum attenuation
			vec3 atten = bp.texturedLights
				? dudeLightAttenTex( lights, li, hitWorld, dl, lights.l[ li ].flags, gate, false, 0.0 )	// a ray hit has no screen footprint: mip 0 (as before)
				: vec3( dudeLightAtten( lights, li, hitWorld, dl, lights.l[ li ].flags, gate, false ) );
			if ( atten.r + atten.g + atten.b <= 0.0 ) { continue; }
			vec3 contrib = albedo * lights.l[ li ].color * ( atten * ndl * bp.lightScale );
			Li += contrib;
			if ( !fast ) { LiCache += contrib; }								// a fast light's bounce is never learned
			// track the DOMINANT (brightest) reaching light at this hit for the H6d shadow ray
			float lum = dot( contrib, vec3( 0.299, 0.587, 0.114 ) );
			if ( lum > domLum ) { domLum = lum; domContrib = contrib; domDir = ldir; domDist = dl; domFast = fast; }
		}
	}
	// RR16g: self-illumination sampled PER-PIXEL from the additive glow stage (bindless slot in flags bits
	// 16-31) at the hit st - only the lit texels emit (a panel's LEDs, not the dark metal), so a bounce off a
	// partly-emissive surface carries its real glow pattern instead of a flat per-surface average.
	uint emit = ( g.flags >> 16 ) & 0xFFFFu;
	if ( emit != 0u ) {											// self-illuminated source (neon, light tile, LED readout); not shadowed
		// Small bright emitters are the worst input for a 1-spp gather: a ray either hits one (spike) or
		// misses (zero), and that variance denoises into grain/colour blotches. Clamp the PEAK channel
		// (bp.emissiveClamp = r_rtgiEmissiveClamp), scaling the whole colour so the hue is preserved, so NRD
		// sees a bounded signal it can resolve cleanly. High clamp = effectively off. textureLod 0: a ray hit
		// has no screen-space st derivatives for auto-mip selection.
		vec3  e  = textureLod( u_rtTextures[ nonuniformEXT( emit - 1u ) ], hitST, 0.0 ).rgb * bp.emissiveScale;
		float ep = max( max( e.r, e.g ), e.b );
		if ( ep > bp.emissiveClamp ) { e *= bp.emissiveClamp / ep; }
		Li += e;
		LiCache += e;
	}

	// H6d (docs/rtx-rtgi.md): one shadow ray at the bounce hit toward the DOMINANT reaching light, so a light
	// behind a thin wall stops leaking indirect light through it. Only the brightest light is tested (cheap:
	// one extra ray regardless of light count) and, if occluded, its contribution is removed. u_localParam1.y gates it.
	if ( bp.shadowRay && domLum > 0.0 ) {
		vec3 sOrg = hitWorld + hitN * 0.2 + domDir * 0.05;			// bias off the surface + a touch toward the light
		rayQueryEXT srq;
		rayQueryInitializeEXT( srq,
			accelerationStructureEXT( bp.tlas ),
			gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, 0xFDu, sOrg, 0.0, domDir, max( domDist - 1.0, 0.1 ) );
		while ( rayQueryProceedEXT( srq ) ) { }
		if ( rayQueryGetIntersectionTypeEXT( srq, true ) == gl_RayQueryCommittedIntersectionTriangleEXT ) {
			Li = max( Li - domContrib, vec3( 0.0 ) );				// dominant light occluded: drop its leaked contribution
			if ( !domFast ) { LiCache = max( LiCache - domContrib, vec3( 0.0 ) ); }
		}
	}

	// RT irradiance cache (docs/rtx-irradiance-cache.md C2, indirect-only mode): the INDIRECT light the hit's own cell has
	// learned - what bounced onto H from everywhere else - lights H exactly as its direct term does (albedo * irradiance-like
	// term, Doom's convention: no 1/pi). A miss / unresolved cell adds 0, so a cold cache is the plain one bounce. The albedo
	// is clamped so the recursion E(P) = L(H) + albedo(H) E(H) stays strictly convergent on white materials. SKIPPED in C6
	// tail mode: a servable hit already returned above with the full L_out, and an unservable one must stay the plain one
	// bounce (adding the indirect term on top of a tail-mode cell, whose value is L_out not E_ind, would double-count).
	if ( bp.cacheLookup && !bp.cacheTail ) {
		vec3 cached = min( albedo, vec3( bp.cache.albedoClamp ) ) * rtgiCacheLookup( bp.cache, hitWorld, hitN, eyeW ) * bp.cache.scale;
		Li += cached;
		LiCache += cached;
	}

	return 1;
}
