/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

#ifndef __MATERIALIR_H__
#define __MATERIALIR_H__

// DUDE Material IR (docs/vulkan-port.md Phase 2.5) — Phase 3 Chunk D.
//
// Deliberately NOT a universal material system: idMaterial remains the
// authoritative description of a Doom 3 material (stages, registers,
// blend state, interaction classification — idTech4 semantics). The IR is
// a thin per-material cache of the decisions the RHI backends make once:
// which GLSL program draws each ambient stage, custom ARB programs
// resolved through the runtime transpiler, and skip/degrade verdicts.
// Everything runtime-dynamic (register evaluation, conditions, colors)
// keeps reading idMaterial directly at draw time.
//
// Interaction stages (SL_BUMP/SL_DIFFUSE/SL_SPECULAR) are not duplicated
// here — the Chunk E interaction path reads idMaterial's classification.

#include "idlib/containers/List.h"
#include "idlib/containers/HashIndex.h"
#include "renderer/rhi/RHI.h"

class idMaterial;

namespace rhi {

enum stageKind_t {
	SK_GENERIC,			// old-style stage through the generic program
	SK_CUSTOM_ARB,		// newStage drawn with its transpiled ARB program pair
	SK_SKIP				// not renderable yet (reason logged once at IR build)
};

struct StageIR {
	int				stageNum;			// index into material->GetStage() — never a cached pointer
	stageKind_t		kind;
	ShaderHandle	program;			// generic, or the transpiled pair
	bool			needsCurrentRender;	// samples _currentRender (deferred until Chunk F)
};

struct MaterialIR {
	const idMaterial *	material;
	idList<StageIR>		surfaceStages;	// SL_AMBIENT stages, in material order
};

// lazy build + cache; pointers stay valid until IR_Purge
const MaterialIR *	IR_Get( const idMaterial *material );

// drops all IRs (reloadShaders, vid_restart) — rebuilt lazily
void				IR_Purge();

} // namespace rhi

#endif /* !__MATERIALIR_H__ */
