/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

#ifndef __ARBPARAMSBLOCK_H__
#define __ARBPARAMSBLOCK_H__

// C++ mirror of the transpiled-ARB uniform block in shaders/arbparams.glsl.
// All members are mat4/vec4, so the std140 layout is trivially packed and
// this struct matches byte for byte — asserted below.
//
// env/local spaces are per ARB target (vertex env[1] = global view origin,
// fragment env[1] = window coord), hence the separate per-stage arrays.
// The fill code replicates RB_SetProgramEnvironment(Space) semantics.

namespace rhi {

// Member ORDER matters and is deliberate: on the NVIDIA GL driver a uniform
// block reads back 0 past ~736 bytes (the "UBO tail-read", see
// gl3-ubo-tail-read-bug) — RenderParams (736 B) escapes it, but this 1536 B
// block does not. Every stock transpiled program (heatHaze*, colorProcess) uses
// only vertex program.local[0..1] and fragment program.env[0..1], so those two
// arrays are placed FIRST (offsets 256 / 384, well inside the reliable window)
// and the large, unused-by-shipped-content venv/flocal arrays are pushed to the
// tail. Keep vlocal + fenv ahead of venv or heatHaze glass tints again.
struct ArbParams {
	float	mvpMatrix[16];
	float	modelViewMatrix[16];
	float	projectionMatrix[16];
	float	textureMatrix[16];
	float	vlocal[8][4];		// vertex   program.local[N]   @256
	float	fenv[32][4];		// fragment program.env[N]     @384
	float	flocal[8][4];		// fragment program.local[N]   @896
	float	venv[32][4];		// vertex   program.env[N]      @1024
};

// 4 mat4 (256) + 8 vec4 (128) + 32 vec4 (512) + 8 vec4 (128) + 32 vec4 (512)
// = 1536 bytes, zero padding
static_assert( sizeof( ArbParams ) == 1536, "ArbParams must match the std140 layout of arbparams.glsl" );

} // namespace rhi

#endif /* !__ARBPARAMSBLOCK_H__ */
