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

// A caller NEVER loops the whole scene-light buffer per sample: the buffer holds every light near the
// player (up to 256) and even a rejected light costs a 16 B read, so samples x lights x pixels of memory
// traffic stalls the GPU for seconds in a light-dense map (device lost - learned the hard way). Instead it
// builds a short CANDIDATE LIST once per view ray (Route A, dudeVolCullRay) or per froxel cluster (Route B)
// and every sample iterates only that.
const uint DUDE_VOL_MAX_LIGHTS = 16u;

// Candidate lights for the view-ray segment eye + viewDir * [0, dist]: volume bounding sphere vs segment,
// plus the light-mode filter. One pass over the buffer per PIXEL. Returns the count written to idx[].
// lightMode 1 = projected/spot + parallel lights only; 2 = also point lights.
// sph[] returns each candidate's bounding sphere, so the per-sample range test reads a local instead of
// going back to the buffer (12 steps x 16 candidates of 16 B reads per pixel were ~a quarter of the cost).
uint dudeVolCullRay( RtLights lights, uint nLights, uint lightMode, vec3 eye, vec3 viewDir, float dist,
                     out uint idx[ DUDE_VOL_MAX_LIGHTS ], out vec4 sph[ DUDE_VOL_MAX_LIGHTS ] ) {
	uint n = 0u;
	for ( uint li = 0u; li < nLights && n < DUDE_VOL_MAX_LIGHTS; li++ ) {
		vec4  cs = lights.l[ li ].cull;
		vec3  oc = cs.xyz - eye;
		float t  = clamp( dot( oc, viewDir ), 0.0, dist );			// closest point of the segment to the centre
		vec3  d  = oc - viewDir * t;
		if ( dot( d, d ) > cs.w * cs.w ) { continue; }
		if ( lightMode < 2u && ( lights.l[ li ].flags & ( DUDE_LIGHT_PROJECTED | DUDE_LIGHT_PARALLEL ) ) == 0u ) { continue; }
		idx[ n ] = li;
		sph[ n ] = cs;
		n++;
	}
	return n;
}

// In-scattered radiance at p per unit scattering coefficient: sum over the CANDIDATE lights of
// colour * attenuation-inside-the-light-volume * visibility * phase.
//   tlasAddr  scene TLAS device address (only dereferenced when shadows is true)
//   shadows   trace one any-hit visibility ray p -> light (lights flagged NOSHADOWS never trace)
//   pointShadows  also trace for point (omni) lights; false = their faint ambience is unshadowed (cheaper)
//   idx/count the candidate list (dudeVolCullRay, or a froxel cluster list)
//   viewDir   unit eye -> p
//   pointScale brightness of point (omni) lights relative to projected/parallel ones
//   weaponScale brightness of first-person weapon lights (the flashlight fogs the air right at the eye)
vec3 dudeVolInScatter( RtLights lights, uint idx[ DUDE_VOL_MAX_LIGHTS ], vec4 sph[ DUDE_VOL_MAX_LIGHTS ], uint count,
                       uvec2 tlasAddr, bool shadows, bool pointShadows,
                       vec3 p, vec3 viewDir, float g, float pointScale, float weaponScale ) {
	vec3 sum = vec3( 0.0 );
	for ( uint k = 0u; k < count; k++ ) {
		vec3 cd = sph[ k ].xyz - p;
		if ( dot( cd, cd ) > sph[ k ].w * sph[ k ].w ) { continue; }		// outside this light's volume sphere (local read)
		uint li = idx[ k ];
		uint fl = lights.l[ li ].flags;
		vec3  ldir; float dl;
		dudeLightDir( lights, li, p, ldir, dl );
		float atten = dudeLightAtten( lights, li, p, dl, fl, true, true );
		if ( atten <= 0.002 ) { continue; }
		// pointScale: omni fill lights are many and large, so unscaled they drown the authored cones
		float scale = ( ( fl & ( DUDE_LIGHT_PROJECTED | DUDE_LIGHT_PARALLEL ) ) != 0u ) ? 1.0 : pointScale;
		if ( ( fl & DUDE_LIGHT_VIEWWEAPON ) != 0u ) { scale *= weaponScale; }	// the player's own flashlight / muzzle flash
		vec3 contrib = lights.l[ li ].color * ( atten * scale * dudeVolPhase( dot( ldir, viewDir ), g ) );
		// the visibility ray is the expensive part: never trace for a contribution too faint to see
		if ( max( contrib.r, max( contrib.g, contrib.b ) ) < 0.004 ) { continue; }
		bool isPoint = ( fl & ( DUDE_LIGHT_PROJECTED | DUDE_LIGHT_PARALLEL ) ) == 0u;
		if ( shadows && ( fl & DUDE_LIGHT_NOSHADOWS ) == 0u && ( pointShadows || !isPoint ) ) {
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
		sum += contrib;
	}
	return sum;
}
