/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

#ifndef __ARBTOGLSL_H__
#define __ARBTOGLSL_H__

// ARB assembly -> GLSL transpiler (second stage; parser in ArbProgram.h).
// Emits shader bodies in the neo/shaders convention: no #version (the
// per-target prelude provides it plus UBO_BINDING/SAMPLER_BINDING/VARY
// macros), uniforms via the ArbParams block (neo/shaders/arbparams.glsl).

#include "renderer/ArbProgram.h"

namespace arb {

struct TranspileResult {
	bool		ok;
	std::string	error;
	std::string	glsl;		// shader body (prepend prelude + arbparams to compile)

	TranspileResult() : ok( false ) {}
};

TranspileResult ToGlsl( const Program &prog );

} // namespace arb

#endif /* !__ARBTOGLSL_H__ */
