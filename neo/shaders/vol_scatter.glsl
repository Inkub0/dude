// DUDE volumetric lighting - the per-sample shading (docs/volumetric-lighting.md section 2): the light that
// the medium at world point p scatters toward the eye. Shared by BOTH routes: Route A's fragment ray march
// (vol_march.frag) calls it per step today; Route B's froxel inject kernel will call it per froxel.
//
// STAGE-AGNOSTIC ON PURPOSE (same contract as rt_light_eval.glsl): no gl_FragCoord, samplers, uniforms or
// derivatives - everything arrives as arguments, so a compute shader includes this file unchanged.
// Needs: rt_light_eval.glsl, GL_EXT_ray_query, GL_EXT_buffer_reference_uvec2.

// Henyey-Greenstein phase function, scaled by 4*pi so an isotropic medium (g = 0) returns 1 and
// r_volDensity keeps one meaning at every anisotropy. cosTheta = dot( point->light, eye->point ):
// +1 looking straight toward the lamp through the medium (forward scatter, the brightest case for g > 0).
float dudeVolPhase( float cosTheta, float g ) {
	float g2 = g * g;
	float d  = max( 1.0 + g2 - 2.0 * g * cosTheta, 1e-4 );
	return ( 1.0 - g2 ) / ( d * sqrt( d ) );
}

// In-scattered radiance at p per unit scattering coefficient: sum over the scene lights of
// colour * attenuation-inside-the-light-volume * visibility * phase.
//   tlasAddr  scene TLAS device address (only dereferenced when shadows is true)
//   shadows   trace one any-hit visibility ray p -> light (lights flagged NOSHADOWS never trace)
//   lightMode 1 = projected/spot + parallel lights only (the authored "feature" lights); 2 = also point lights
//   viewDir   unit eye -> p
vec3 dudeVolInScatter( RtLights lights, uint nLights, uvec2 tlasAddr, bool shadows, uint lightMode,
                       vec3 p, vec3 viewDir, float g ) {
	vec3 sum = vec3( 0.0 );
	for ( uint li = 0u; li < nLights; li++ ) {
		if ( !dudeLightInRange( lights, li, p ) ) { continue; }
		uint fl = lights.l[ li ].flags;
		if ( lightMode < 2u && ( fl & ( DUDE_LIGHT_PROJECTED | DUDE_LIGHT_PARALLEL ) ) == 0u ) { continue; }
		vec3  ldir; float dl;
		dudeLightDir( lights, li, p, ldir, dl );
		float atten = dudeLightAtten( lights, li, p, dl, fl, true );
		if ( atten <= 0.002 ) { continue; }
		if ( shadows && ( fl & DUDE_LIGHT_NOSHADOWS ) == 0u ) {
			// cheapest ray there is: opaque-only, first hit ends it. Stop short of the light so the
			// fixture's own geometry does not shadow its light; a parallel light's origin is far away.
			float tmax = min( dl - 4.0, 16384.0 );
			if ( tmax > 1.0 ) {
				rayQueryEXT rq;
				rayQueryInitializeEXT( rq, accelerationStructureEXT( tlasAddr ),
					gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, 0xFFu, p, 0.0, ldir, tmax );
				while ( rayQueryProceedEXT( rq ) ) { }
				if ( rayQueryGetIntersectionTypeEXT( rq, true ) != gl_RayQueryCommittedIntersectionNoneEXT ) { continue; }
			}
		}
		sum += lights.l[ li ].color * ( atten * dudeVolPhase( dot( ldir, viewDir ), g ) );
	}
	return sum;
}
