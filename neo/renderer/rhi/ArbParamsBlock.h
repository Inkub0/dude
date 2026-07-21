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

struct ArbParams {
	float	mvpMatrix[16];
	float	modelViewMatrix[16];
	float	projectionMatrix[16];
	float	textureMatrix[16];
	float	venv[32][4];		// vertex   program.env[N]
	float	fenv[32][4];		// fragment program.env[N]
	float	vlocal[8][4];		// vertex   program.local[N]
	float	flocal[8][4];		// fragment program.local[N]
};

// 4 mat4 (256) + 64 vec4 (1024) + 16 vec4 (256) = 1536 bytes, zero padding
static_assert( sizeof( ArbParams ) == 1536, "ArbParams must match the std140 layout of arbparams.glsl" );

} // namespace rhi

#endif /* !__ARBPARAMSBLOCK_H__ */
