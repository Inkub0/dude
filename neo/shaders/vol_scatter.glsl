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
                     out uint idx[ DUDE_VOL_MAX_LIGHTS ], out vec4 sph[ DUDE_VOL_MAX_LIGHTS ], out vec2 range ) {
	// range = the stretch [near, far] of the ray that the candidates' spheres cover: the march spends its
	// steps there instead of spreading them over air no light can reach (fewer wasted steps = less noise).
	range = vec2( dist, 0.0 );
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
		float tc   = dot( oc, viewDir );									// unclamped closest approach
		float dc2  = dot( oc, oc ) - tc * tc;
		float half_ = sqrt( max( cs.w * cs.w - dc2, 0.0 ) );				// half chord through the sphere
		range.x = min( range.x, tc - half_ );
		range.y = max( range.y, tc + half_ );
	}
	range = vec2( clamp( range.x, 0.0, dist ), clamp( range.y, 0.0, dist ) );
	return n;
}

// Emissive mode: EMITTER rows (DUDE_LIGHT_EMISSIVE, after the fog rows, strongest at the eye first) join the candidate
// list, up to the slots the lights left free. Same segment-vs-sphere cull; bounce-only rows (emissive mode off) never.
uint dudeVolCullEmitters( RtLights lights, uint firstEm, uint nEm, vec3 eye, vec3 viewDir, float dist,
                          inout uint idx[ DUDE_VOL_MAX_LIGHTS ], inout vec4 sph[ DUDE_VOL_MAX_LIGHTS ], uint n, inout vec2 range ) {
	for ( uint k = 0u; k < nEm && n < DUDE_VOL_MAX_LIGHTS; k++ ) {
		uint  li = firstEm + k;
		uint  fl = lights.l[ li ].flags;
		if ( ( fl & DUDE_LIGHT_EMISSIVE ) == 0u || ( fl & DUDE_LIGHT_BOUNCEONLY ) != 0u ) { continue; }
		vec4  cs = lights.l[ li ].cull;
		vec3  oc = cs.xyz - eye;
		float t  = clamp( dot( oc, viewDir ), 0.0, dist );
		vec3  d  = oc - viewDir * t;
		if ( dot( d, d ) > cs.w * cs.w ) { continue; }
		idx[ n ] = li;
		sph[ n ] = cs;
		n++;
		float tc    = dot( oc, viewDir );
		float dc2   = dot( oc, oc ) - tc * tc;
		float half_ = sqrt( max( cs.w * cs.w - dc2, 0.0 ) );
		range.x = min( range.x, clamp( tc - half_, 0.0, dist ) );
		range.y = max( range.y, clamp( tc + half_, 0.0, dist ) );
	}
	return n;
}

// ---- stock fog volumes: the haze is thicker inside them --------------------------------------------------
// Fog rows sit behind the lights in the same buffer (index firstFog .. firstFog + nFog); color.x carries the
// fog's strength (0.2 thin .. 1 thick, from its own opaque distance). Same two-step shape as the lights:
// cull once per ray, evaluate per sample from the short list.
const uint DUDE_VOL_MAX_FOGS = 4u;

uint dudeVolCullFogRay( RtLights lights, uint firstFog, uint nFog, vec3 eye, vec3 viewDir, float dist,
                        out uint idx[ DUDE_VOL_MAX_FOGS ], out vec4 sph[ DUDE_VOL_MAX_FOGS ] ) {
	uint n = 0u;
	for ( uint k = 0u; k < nFog && n < DUDE_VOL_MAX_FOGS; k++ ) {
		vec4  cs = lights.l[ firstFog + k ].cull;
		vec3  oc = cs.xyz - eye;
		float t  = clamp( dot( oc, viewDir ), 0.0, dist );
		vec3  d  = oc - viewDir * t;
		if ( dot( d, d ) > cs.w * cs.w ) { continue; }
		idx[ n ] = firstFog + k;
		sph[ n ] = cs;
		n++;
	}
	return n;
}

// 0 outside every fog volume .. the fog's strength well inside one (soft-edged box, like a light volume)
float dudeVolFogFactor( RtLights lights, uint idx[ DUDE_VOL_MAX_FOGS ], vec4 sph[ DUDE_VOL_MAX_FOGS ], uint count, vec3 p ) {
	float f = 0.0;
	for ( uint k = 0u; k < count; k++ ) {
		vec3 cd = sph[ k ].xyz - p;
		if ( dot( cd, cd ) > sph[ k ].w * sph[ k ].w ) { continue; }
		uint  li = idx[ k ];
		vec4  hw = vec4( p, 1.0 );
		float q  = dot( hw, lights.l[ li ].projQ );
		if ( q <= 1e-4 ) { continue; }
		vec3  stf = vec3( dot( hw, lights.l[ li ].projS ) / q, dot( hw, lights.l[ li ].projT ) / q, dot( hw, lights.l[ li ].projF ) );
		vec3  e   = smoothstep( vec3( 0.0 ), vec3( 0.15 ), min( stf, vec3( 1.0 ) - stf ) );
		f = max( f, e.x * e.y * e.z * lights.l[ li ].color.x );
	}
	return f;
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
//   emissiveScale emissive mode: an EMITTER row's haze relative to the point lights it replaced (r_volEmissiveScale)
vec3 dudeVolInScatter( RtLights lights, uint idx[ DUDE_VOL_MAX_LIGHTS ], vec4 sph[ DUDE_VOL_MAX_LIGHTS ], uint count,
                       uvec2 tlasAddr, bool shadows, bool pointShadows,
                       vec3 p, vec3 viewDir, float g, float pointScale, float weaponScale, float emissiveScale ) {
	vec3 sum = vec3( 0.0 );
	for ( uint k = 0u; k < count; k++ ) {
		vec3 cd = sph[ k ].xyz - p;
		if ( dot( cd, cd ) > sph[ k ].w * sph[ k ].w ) { continue; }		// outside this light's volume sphere (local read)
		uint li = idx[ k ];
		uint fl = lights.l[ li ].flags;
		if ( ( fl & DUDE_LIGHT_EMISSIVE ) != 0u ) {
			// an EMITTER row as a small light at its centroid, by the shade's own far field (rtEmissiveTargetRow): its row
			// colour is its irradiance proxy at its reach R (16 L A / R^2), so L A = colour R^2 / 16, over d^2 with the reach
			// boost and fade, the near field floored at its bound radius; its follow flicker (projF.z: its colour already carries
			// an animated glow's tint) and the omni scale of
			// the point lights it replaced. Shadowed like a point light, the ray stopping OUTSIDE the fixture's bound sphere
			// (a tube's or a bulb's own surface must not shadow its light).
			vec3  toC  = lights.l[ li ].cull.xyz - p;					// the centroid (an emitter row's pos is its cone axis)
			float dc   = length( toC );
			if ( !( dc > 1e-3 ) ) { continue; }
			vec3  edir = toC / dc;
			float R    = max( lights.l[ li ].radius, 1.0 );
			float br   = max( lights.l[ li ].cull.w - lights.l[ li ].radius, 0.0 );
			float knee = lights.l[ li ].projQ.z;
			float boost = ( knee > 0.0 ) ? max( dc / knee, 1.0 ) : 1.0;
			float fade = ( lights.l[ li ].projQ.w > 0.5 ) ? 1.0 : 1.0 - smoothstep( 0.75 * R, R, max( dc - br, 0.0 ) );
			float d2   = max( dc * dc, max( br * br, 1.0 ) );
			vec3  ec   = lights.l[ li ].color * ( ( R * R / 16.0 ) * boost * fade / d2 * max( lights.l[ li ].projF.z, 0.0 )
			                                      * pointScale * emissiveScale * dudeVolPhase( dot( edir, viewDir ), g ) );
			if ( max( ec.r, max( ec.g, ec.b ) ) < 0.004 ) { continue; }
			if ( shadows && pointShadows && ( fl & DUDE_LIGHT_NOSHADOWS ) == 0u ) {
				float tmax = min( dc - br - 2.0, 16384.0 );
				if ( tmax > 1.0 ) {
					rayQueryEXT rq;
					dudeRayQueryInit( rq, accelerationStructureEXT( tlasAddr ),
						gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, 0xFDu, p, 0.0, edir, tmax );
					while ( rayQueryProceedEXT( rq ) ) { }
					if ( rayQueryGetIntersectionTypeEXT( rq, true ) != gl_RayQueryCommittedIntersectionNoneEXT ) { continue; }
				}
			}
			sum += ec;
			continue;
		}
		vec3  ldir; float dl;
		dudeLightDir( lights, li, p, ldir, dl );
		float atten = dudeLightAtten( lights, li, p, dl, fl, true, true );
		if ( atten <= 0.002 ) { continue; }
		// pointScale: omni fill lights are many and large, so unscaled they drown the authored cones
		float scale = ( ( fl & ( DUDE_LIGHT_PROJECTED | DUDE_LIGHT_PARALLEL ) ) != 0u ) ? 1.0 : pointScale;
		if ( ( fl & DUDE_LIGHT_VIEWWEAPON ) != 0u ) { scale *= weaponScale; }	// the player's own flashlight / muzzle flash
		scale *= float( ( fl >> 8 ) & 255u ) * ( 1.0 / 255.0 );					// huge ambience lights fade out of the haze (set by the gather)
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
				dudeRayQueryInit( rq, accelerationStructureEXT( tlasAddr ),
					gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, 0xFDu /* solid rays: skip perforated-caster instances (mask 0x02), only an alpha-testing ray may hit those */, p, 0.0, ldir, tmax );
				while ( rayQueryProceedEXT( rq ) ) { }
				if ( rayQueryGetIntersectionTypeEXT( rq, true ) != gl_RayQueryCommittedIntersectionNoneEXT ) { continue; }
			}
		}
		sum += contrib;
	}
	return sum;
}
