/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

#ifndef __AOBAKE_GPU_H__
#define __AOBAKE_GPU_H__

// GPU tracer for the AO baker (docs/occlusion-maps.md): the per-texel hemisphere integration of
// aobake.cpp run as a Vulkan ray-query compute shader (shaders/aobake.comp) on a headless device of
// its own -- independent of the renderer backend, so it works under GL3, Vulkan or xvfb alike. Only
// in DHEWM3_VULKAN builds. The interface is plain arrays so the offline test
// (tools/test_aobake_gpu.sh) links this file alone.

// One texel to integrate (std430: two vec4s). w components unused.
struct aoGpuSample_t {
	float	pos[4];		// object-space surface point
	float	nrm[4];		// interpolated vertex normal (normalized on the GPU)
};

// Open (or reuse) the GPU tracer. Reference counted: a bakeAOFolder holds it across every model so
// the device is created once. Returns false when no Vulkan device with ray query is usable; the
// reason is printed once per acquisition chain and the caller bakes on the CPU instead. Every
// Acquire, successful or not, is paired with one Release; the device goes away with the last.
bool	AOGpu_Acquire( void );
void	AOGpu_Release( void );

// Upload a model's occluders: numTris triangles, 9 floats each. Wind every triangle so that
// ( v1 - v0 ) x ( v2 - v0 ) is its outward normal (the one the CPU tracer culls by): Vulkan calls a
// triangle front-facing when its vertices run counter-clockwise as seen from the ray origin, so with
// this winding its back-face cull rejects exactly the faces the CPU skips (tools/aobake test checks).
bool	AOGpu_SetOccluders( const float *xyz, int numTris );
// Upload the shared hemisphere directions (xyz per ray, tangent space, +Z = normal).
bool	AOGpu_SetHemisphere( const float *xyz, int rays );
// Integrate `count` samples against the current occluders and hemisphere. Writes visibility
// (1 = open, 0 = occluded; distance falloff applied) and the buried flag per sample. Returns false
// on a device error -- the tracer is then unusable until re-acquired and the caller falls back.
bool	AOGpu_Trace( const aoGpuSample_t *samples, int count, float maxDist, float bias, float contact,
		             float *outVis, unsigned char *outBuried );

#endif /* !__AOBAKE_GPU_H__ */
