// DUDE scene-light evaluation, shared by every pass that lights an arbitrary WORLD point from the per-view
// scene-light buffer (RhiWorld.cpp RB_RHI_GatherSceneLights): RT reflections (ssr_rt.frag, RR16) and
// volumetric lighting (vol_scatter.glsl, docs/volumetric-lighting.md). One implementation so the two can
// never drift apart.
//
// STAGE-AGNOSTIC ON PURPOSE: no gl_FragCoord, no samplers, no uniforms, no derivatives - only the buffer
// reference + a world position in, numbers out. A compute kernel (the Route B froxel inject) includes this
// file unchanged. Needs buffer_reference (the VK prelude enables it).
//
// Row layout (std430, 128 B). cull = bounding sphere (centre, radius) of the light VOLUME, first in the row
// so a light that cannot reach the point costs one 16 B read (a whole-row copy per light per pixel measured
// +1.5 ms - always read fields individually). projS/T/Q/F = the light's world-space lightProject planes:
// s = S.p/Q.p, t = T.p/Q.p, falloff f = F.p, all in [0,1] inside the volume (a spot's pyramid, a point
// light's BOX). projTex / falloffTex = bindless slot+1 of the light's projection cookie / falloff ramp
// (0 = none); sampled by rt_light_tex.glsl (needs set 2) - this file stays sampler-free.
// pad0 = lightDef index + 1 (0 = not a light: fog rows), pad1 = the def's creation serial (ReSTIR DI gradient, Plan R1:
// same slot + same serial = the same light; a freed slot is reused at once)
struct RtLight { vec4 cull; vec3 pos; float radius; vec3 color; uint flags; vec4 projS; vec4 projT; vec4 projQ; vec4 projF; uint projTex; uint falloffTex; uint pad0; uint pad1; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer RtLights { RtLight l[]; };

const uint DUDE_LIGHT_PARALLEL  = 1u;	// sun-style: no distance falloff
const uint DUDE_LIGHT_PROJECTED = 2u;	// spot: falloff runs along the frustum axis, lightRadius unused
const uint DUDE_LIGHT_NOSHADOWS = 4u;	// the light casts no shadows in stock either: skip visibility rays
const uint DUDE_LIGHT_FOGVOLUME  = 16u;	// not a light: a stock fog volume row (stored AFTER the lights, outside the light count)
const uint DUDE_LIGHT_VIEWWEAPON = 8u;	// first-person weapon light: the player's flashlight, muzzle flashes
const uint DUDE_LIGHT_DIFORWARD  = 32u;	// ReSTIR DI skips it (r_rtReStirDIDynamicForward): a fast-changing light the forward path lights at zero lag
// flags bits 8-15 = volumetric haze scale * 255; bits 16-31 = the hybrid shadow blur's per-light blur scale as a half
// float (ReSTIR DI soft grate coverage, restir_di_shade.glsl diLightBlurScale; 0 while r_rtShadowBlur is off)

// Can light li reach world point p at all? (bounding sphere of its volume; reads 16 B)
bool dudeLightInRange( RtLights lights, uint li, vec3 p ) {
	vec4 cs = lights.l[ li ].cull;
	vec3 cd = cs.xyz - p;
	return dot( cd, cd ) <= cs.w * cs.w;
}

// Unit direction p -> light and the distance to it.
void dudeLightDir( RtLights lights, uint li, vec3 p, out vec3 ldir, out float dl ) {
	vec3 d = lights.l[ li ].pos - p;
	dl   = length( d );
	ldir = ( dl > 1e-3 ) ? ( d / dl ) : vec3( 0.0, 0.0, 1.0 );
}

// Attenuation of light li at p, 0 = does not reach. Linear radial falloff (Doom's falloff ramp is ~linear);
// with frustumGate the term is confined to the light's projection volume, soft-edged as every Doom light
// texture fades to black at its border (uniform inside: the gobo/falloff TEXTURES are not sampled). A spot
// drops the radial term for an axial 1-f start->end falloff and rejects points behind its apex.
// roundSpot: window a projected light radially in (s,t) instead of per axis - a round cone. Doom's spot
// textures are overwhelmingly round blobs on a black square; until the projection TEXTURE itself is sampled
// the square pyramid is right for surface lighting (RR16c, verified) but reads as a box beam in the air.
float dudeLightAtten( RtLights lights, uint li, vec3 p, float dl, uint flags, bool frustumGate, bool roundSpot ) {
	float atten = ( ( flags & DUDE_LIGHT_PARALLEL ) != 0u ) ? 1.0
	            : max( 1.0 - dl / max( lights.l[ li ].radius, 1.0 ), 0.0 );
	if ( frustumGate ) {
		vec4  hw = vec4( p, 1.0 );
		float q  = dot( hw, lights.l[ li ].projQ );
		if ( q <= 1e-4 ) { return 0.0; }										// behind a spot's apex
		vec3  stf = vec3( dot( hw, lights.l[ li ].projS ) / q, dot( hw, lights.l[ li ].projT ) / q, dot( hw, lights.l[ li ].projF ) );
		vec3  e   = smoothstep( vec3( 0.0 ), vec3( 0.15 ), min( stf, vec3( 1.0 ) - stf ) );	// 0 at/outside a face
		if ( ( flags & DUDE_LIGHT_PROJECTED ) != 0u ) {
			float win = e.x * e.y;
			if ( roundSpot ) {
				float rr = length( stf.xy * 2.0 - 1.0 );						// 0 on the axis, 1 at the frustum side
				win = 1.0 - smoothstep( 0.55, 1.0, rr );
			}
			atten = win * step( 0.0, stf.z ) * clamp( 1.0 - stf.z, 0.0, 1.0 );	// spot: axial falloff
		} else {
			atten *= e.x * e.y * e.z;											// point/parallel: clipped to the light box
		}
	}
	return atten;
}
