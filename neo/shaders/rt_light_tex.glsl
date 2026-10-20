// DUDE textured scene-light attenuation (docs/rtx-restir-di.md Stage G): the light's REAL projection cookie
// and falloff ramp, sampled from the bindless texture array at the slots the scene-light buffer carries
// (RhiWorld.cpp RB_RHI_GatherSceneLights projTex / falloffTex), so a world point is attenuated EXACTLY as the
// forward interaction draw attenuates it (interaction.vert texgens + interaction.frag:531):
//   cookie  = textureProj( lightProjection, ( S.p, T.p, 0, Q.p ) )   -> sample at ( S.p/Q.p, T.p/Q.p )
//   falloff = texture( lightFalloff, ( F.p, 0.5 ) )
//   atten   = cookie.rgb * falloff.rgb                                (a coloured gobo colours the light)
// Doom's light images are CLAMP_TO_ZERO (black border): outside the frustum the cookie reads black and
// behind the apex (Q.p <= 0) the projection is invalid, so no analytic frustum gate is needed - the
// textures ARE the light's shape, which is what makes a spot a cone/blob instead of a box beam.
// textureLod 0: the callers run on reconstructed / ray-hit points with no screen-space derivatives.
//
// Needs: rt_light_eval.glsl (the row + the analytic fallback), GL_EXT_nonuniform_qualifier, and the
// descriptor set 2 bindless array (bound once per frame cb by the backend). Fragment / compute alike.
#ifndef DUDE_RT_TEXTURES_DECLARED
#define DUDE_RT_TEXTURES_DECLARED
layout(set = 2, binding = 0) uniform sampler2D u_rtTextures[];
#endif

// Attenuation of light li at p as the light's textures define it; the analytic dudeLightAtten when the row
// carries no cookie (an untextured light, r_rtLightTextures 0, or an image that has not bridged yet).
// Same signature contract as dudeLightAtten plus the colour: returns rgb, 0 = does not reach.
//
// pixelWorld = the world-space size of ONE screen pixel at p (0 = sample mip 0). The forward draw samples the
// cookie with textureProj and the hardware's screen-derivative LOD, and Doom's cookies fade to a black border,
// so on a surface seen at a distance / grazing angle the blurred mips pull that border inward and the light
// reads dimmer than mip 0 does (a white floor under DI was visibly brighter than forward, 2026-10-08). The
// chain rule gives the same footprint without screen derivatives (which a candidate loop cannot take):
// d(st)/d(pixel) = grad_p(st) * pixelWorld, grad_p(s) = (S.xyz - s*Q.xyz)/q, lod = log2(footprint in texels).
// Isotropic (the minor axis), which is what anisotropic filtering keys its LOD on; the falloff ramp stays mip 0.
vec3 dudeLightAttenTex( RtLights lights, uint li, vec3 p, float dl, uint flags, bool frustumGate, bool roundSpot, float pixelWorld ) {
	if ( ( flags & DUDE_LIGHT_EMISSIVE ) != 0u ) { return vec3( 0.0 ); }		// an emitter row: not a frustum light (rtEmissiveShadeRow)
	uint projTex = lights.l[ li ].projTex;
	if ( projTex == 0u ) {
		return vec3( dudeLightAtten( lights, li, p, dl, flags, frustumGate, roundSpot ) );
	}
	vec4  hw = vec4( p, 1.0 );
	float q  = dot( hw, lights.l[ li ].projQ );
	if ( q <= 1e-4 ) { return vec3( 0.0 ); }							// behind the projection apex
	vec2  st = vec2( dot( hw, lights.l[ li ].projS ), dot( hw, lights.l[ li ].projT ) ) / q;
	// EXACT early-out before any fetch: the cookie is CLAMP_TO_ZERO (black border) and the falloff ramp likewise, so
	// outside [0,1] in s, t or the falloff coordinate the textures would return 0 anyway. Most candidates that pass
	// the sphere cull sit outside the light's box, so this skips two divergent bindless fetches for them (the DI
	// gather at 128 candidates measured 3.2 ms with the fetches unconditional, 2026-10-08).
	float f = dot( hw, lights.l[ li ].projF );
	if ( st.x <= 0.0 || st.x >= 1.0 || st.y <= 0.0 || st.y >= 1.0 || f <= 0.0 || f >= 1.0 ) { return vec3( 0.0 ); }
	float lod = 0.0;
	if ( pixelWorld > 0.0 ) {
		vec3  gs = ( lights.l[ li ].projS.xyz - st.x * lights.l[ li ].projQ.xyz ) / q;
		vec3  gt = ( lights.l[ li ].projT.xyz - st.y * lights.l[ li ].projQ.xyz ) / q;
		vec2  texels = vec2( textureSize( u_rtTextures[ nonuniformEXT( projTex - 1u ) ], 0 ) ) * vec2( length( gs ), length( gt ) ) * pixelWorld;
		lod = log2( max( max( texels.x, texels.y ), 1e-6 ) );
	}
	vec3  atten = textureLod( u_rtTextures[ nonuniformEXT( projTex - 1u ) ], st, lod ).rgb;
	uint  falloffTex = lights.l[ li ].falloffTex;
	if ( falloffTex != 0u ) {
		atten *= textureLod( u_rtTextures[ nonuniformEXT( falloffTex - 1u ) ], vec2( f, 0.5 ), 0.0 ).rgb;
	} else {
		// no falloff image bridged: keep the analytic radial / axial term so the light still fades
		atten *= ( ( flags & DUDE_LIGHT_PARALLEL ) != 0u ) ? 1.0 : max( 1.0 - dl / max( lights.l[ li ].radius, 1.0 ), 0.0 );
	}
	return atten;
}
