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
vec3 dudeLightAttenTex( RtLights lights, uint li, vec3 p, float dl, uint flags, bool frustumGate, bool roundSpot ) {
	uint projTex = lights.l[ li ].projTex;
	if ( projTex == 0u ) {
		return vec3( dudeLightAtten( lights, li, p, dl, flags, frustumGate, roundSpot ) );
	}
	vec4  hw = vec4( p, 1.0 );
	float q  = dot( hw, lights.l[ li ].projQ );
	if ( q <= 1e-4 ) { return vec3( 0.0 ); }							// behind the projection apex
	vec2  st = vec2( dot( hw, lights.l[ li ].projS ), dot( hw, lights.l[ li ].projT ) ) / q;
	vec3  atten = textureLod( u_rtTextures[ nonuniformEXT( projTex - 1u ) ], st, 0.0 ).rgb;
	uint  falloffTex = lights.l[ li ].falloffTex;
	if ( falloffTex != 0u ) {
		float f = dot( hw, lights.l[ li ].projF );
		atten *= textureLod( u_rtTextures[ nonuniformEXT( falloffTex - 1u ) ], vec2( f, 0.5 ), 0.0 ).rgb;
	} else {
		// no falloff image bridged: keep the analytic radial / axial term so the light still fades
		atten *= ( ( flags & DUDE_LIGHT_PARALLEL ) != 0u ) ? 1.0 : max( 1.0 - dl / max( lights.l[ li ].radius, 1.0 ), 0.0 );
	}
	return atten;
}
