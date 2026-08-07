/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.cpp for license header)
===========================================================================
*/

// DUDE Material IR — see MaterialIR.h for the design contract.
//
// Custom ARB resolution: newStage programs were recorded by name during
// material parse (R_FindARBProgram is upload-free on core profiles); here
// the glprogs/*.vfp source is read back, run through the in-engine
// ARB->GLSL transpiler (ArbProgram/ArbToGlsl — the same code arbtool and
// the Phase 2 validation corpus exercise), and compiled into the GL3
// program cache under "arb/<vp>+<fp>". On any failure the stage degrades
// to a plain generic textured stage (degrade, don't crash — the mod-compat
// policy in neo/shaders/README.md).

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "framework/FileSystem.h"
#include "renderer/ArbProgram.h"
#include "renderer/ArbToGlsl.h"
#include "renderer/rhi/RHI.h"
#include "renderer/rhi/GL3Local.h"
#include "renderer/rhi/MaterialIR.h"

namespace rhi {

static idList<MaterialIR *>	irList;
static idHashIndex			irHash;

/*
=============
IR_TranspileSection

Extracts the !!ARBvp/!!ARBfp section from a glprogs file and transpiles it.
=============
*/
static bool IR_TranspileSection( const char *fileName, const char *header, std::string &glslOut, idStr &err ) {
	void *buf = NULL;
	int len = fileSystem->ReadFile( va( "glprogs/%s", fileName ), &buf, NULL );
	if ( len < 0 || !buf ) {
		err = va( "glprogs/%s not found", fileName );
		return false;
	}
	idStr text;
	text.Append( (const char *)buf, len );
	fileSystem->FreeFile( buf );

	const char *start = strstr( text.c_str(), header );
	if ( !start ) {
		err = va( "%s not found in glprogs/%s", header, fileName );
		return false;
	}

	arb::ParseResult parsed = arb::Parse( std::string( start ) );
	if ( !parsed.ok ) {
		err = va( "parse: %s", parsed.error.c_str() );
		return false;
	}
	arb::TranspileResult transpiled = arb::ToGlsl( parsed.program );
	if ( !transpiled.ok ) {
		err = va( "transpile: %s", transpiled.error.c_str() );
		return false;
	}
	glslOut = transpiled.glsl;
	return true;
}

/*
=============
IR_ResolveCustomArb

vp/fp glprogs file names -> linked GL3 program (cached), 0 on failure.
=============
*/
static ShaderHandle IR_ResolveCustomArb( const char *vpFile, const char *fpFile, const char *materialName ) {
	std::string vertGlsl, fragGlsl;
	idStr err;

	// DUDE Phase 4 M2: the Vulkan backend has no runtime GLSL→SPIR-V compiler
	// (SPIR-V is built at build time), so transpiled custom ARB programs can't
	// be materialized there yet — degrade to the generic stage (the caller's
	// existing fallback). Runtime shader compilation is planned with the
	// _currentRender effects (M5).
	if ( GetActiveBackendType() == BT_VULKAN ) {
		static bool warned = false;
		if ( !warned ) {
			warned = true;
			common->Printf( "VK IR: custom ARB stages degrade to generic until runtime SPIR-V lands (M5)\n" );
		}
		return 0;
	}

	if ( !IR_TranspileSection( vpFile, "!!ARBvp", vertGlsl, err ) ) {
		common->Warning( "GL3 IR: %s: vertex program %s: %s", materialName, vpFile, err.c_str() );
		return 0;
	}
	if ( !IR_TranspileSection( fpFile, "!!ARBfp", fragGlsl, err ) ) {
		common->Warning( "GL3 IR: %s: fragment program %s: %s", materialName, fpFile, err.c_str() );
		return 0;
	}

	return GL3_FindProgramFromSource( va( "arb/%s+%s", vpFile, fpFile ), vertGlsl.c_str(), fragGlsl.c_str() );
}

/*
=============
IR_VkBuiltinForArb

Phase 4 M5: the Vulkan backend has no runtime SPIR-V compiler, but the stock
custom-ARB programs were hand-translated to builtin GLSL during Phase 2 and
cross-verified against the transpiler output (scripts/crossdiff_shaders.py,
120/120 numeric matches) — map them to those builtins instead of skipping.
The remaining (d3xp/mod) customs still degrade to SK_SKIP until runtime
SPIR-V lands with the d3xp backend flip.
=============
*/
static const char *IR_VkBuiltinForArb( const char *vpFile, const char *fpFile ) {
	if ( vpFile == NULL || fpFile == NULL ) {
		return NULL;
	}
	// Two stock materials (textures/sfx/vppinch_bfgbolt, textures/sfx/vpsphere)
	// split the pair: the heatHazeWithMaskAndVertex.vfp *vertex* program with
	// the heatHazeWithMask.vfp *fragment* program. That has its own combined
	// builtin (maskvertex VS + mask FS, vertex color emitted but unread).
	if ( idStr::Icmp( vpFile, "heatHazeWithMaskAndVertex.vfp" ) == 0
	  && idStr::Icmp( fpFile, "heatHazeWithMask.vfp" ) == 0 ) {
		return "heathaze_maskvertex_mask";
	}
	if ( idStr::Icmp( vpFile, fpFile ) != 0 ) {
		return NULL;	// the remaining stock customs pair the same .vfp for both stages
	}
	if ( idStr::Icmp( vpFile, "heatHaze.vfp" ) == 0 ) {
		return "heathaze";
	}
	if ( idStr::Icmp( vpFile, "heatHazeWithMask.vfp" ) == 0 ) {
		return "heathaze_mask";
	}
	if ( idStr::Icmp( vpFile, "heatHazeWithMaskAndVertex.vfp" ) == 0 ) {
		return "heathaze_maskvertex";
	}
	if ( idStr::Icmp( vpFile, "colorProcess.vfp" ) == 0 ) {
		return "colorprocess";
	}
	return NULL;
}

/*
=============
IR_Build
=============
*/
static MaterialIR *IR_Build( const idMaterial *material ) {
	MaterialIR *ir = new MaterialIR;
	ir->material = material;

	// backend-neutral (Phase 4 M2): LoadShader is GL3_FindProgram on the GL3
	// backend and the SPIR-V module cache on Vulkan
	ShaderHandle generic = GetRHI()->LoadShader( "generic" );

	for ( int i = 0; i < material->GetNumStages(); i++ ) {
		const shaderStage_t *stage = material->GetStage( i );
		if ( stage->lighting != SL_AMBIENT ) {
			continue;	// interaction stages: read from idMaterial in Chunk E
		}

		StageIR s;
		s.stageNum = i;
		s.kind = SK_GENERIC;
		s.program = generic;
		s.needsCurrentRender = false;
		s.texgen = TG_EXPLICIT;

		if ( stage->newStage ) {
			const newShaderStage_t *ns = stage->newStage;
			if ( ns->megaTexture ) {
				s.kind = SK_SKIP;
				common->Printf( "GL3 IR: %s: stage %d skipped (megaTexture)\n", material->GetName(), i );
			} else {
				const char *vpFile = R_ARBProgramName( ns->vertexProgram, GL_VERTEX_PROGRAM_ARB );
				const char *fpFile = R_ARBProgramName( ns->fragmentProgram, GL_FRAGMENT_PROGRAM_ARB );
				const bool vk = GetActiveBackendType() == BT_VULKAN;
				const char *builtin = vk ? IR_VkBuiltinForArb( vpFile, fpFile ) : NULL;
				ShaderHandle prog = 0;
				if ( builtin != NULL ) {
					prog = GetRHI()->LoadShader( builtin );
					if ( prog ) {
						s.kind = SK_BUILTIN_ARB;
					}
				} else if ( vpFile && fpFile ) {
					prog = IR_ResolveCustomArb( vpFile, fpFile, material->GetName() );
					if ( prog ) {
						s.kind = SK_CUSTOM_ARB;
					}
				}
				if ( prog ) {
					s.program = prog;
					for ( int img = 0; img < ns->numFragmentProgramImages; img++ ) {
						if ( ns->fragmentProgramImages[img] == globalImages->currentRenderImage ) {
							s.needsCurrentRender = true;	// Chunk F: RC_COPY_RENDER
						}
					}
				} else if ( vk ) {
					// a newStage has no stage image (its textures live in
					// fragmentProgramImages), so the generic fallback comes out
					// solid white — skip the stage entirely until runtime
					// SPIR-V lands (the d3xp flip); invisible beats a white square
					s.kind = SK_SKIP;
					common->Printf( "VK IR: %s: stage %d custom ARB skipped (no builtin translation, runtime SPIR-V pending)\n", material->GetName(), i );
				} else {
					// degrade, don't crash: draw as a plain generic stage
					common->Printf( "GL3 IR: %s: stage %d custom ARB degraded to generic\n", material->GetName(), i );
				}
			}
		} else if ( stage->texture.texgen != TG_EXPLICIT ) {
			// fixed-function texgens: each maps to a dedicated program the
			// backend drives (RB_RHI_RenderTexgenStage). The texcoord math the
			// old path baked (screen projection, cube directions, sky/wobble
			// vectors) is done in those shaders / the backend uniforms.
			s.texgen = stage->texture.texgen;
			const char *prog = NULL;
			switch ( stage->texture.texgen ) {
			case TG_SCREEN:
			case TG_SCREEN2:
				// portal sky: blit the pre-rendered sky from _currentRender
				prog = "portalsky";
				s.needsCurrentRender = true;
				break;
			case TG_REFLECT_CUBE:
				// per-pixel cube reflection, bumped if the material has a bump stage
				prog = material->GetBumpStage() ? "bumpyenvironment" : "environment";
				break;
			case TG_SKYBOX_CUBE:
			case TG_WOBBLESKY_CUBE:
				prog = "skybox";
				break;
			case TG_DIFFUSE_CUBE:
				prog = "diffusecube";
				break;
			default:
				break;	// TG_GLASSWARP: needs scratch-image plumbing, degrade below
			}
			ShaderHandle prg = prog ? GetRHI()->LoadShader( prog ) : 0;
			if ( prg ) {
				s.kind = SK_TEXGEN;
				s.program = prg;
			} else {
				s.kind = SK_SKIP;
				common->Printf( "GL3 IR: %s: stage %d skipped (texgen %d, no program)\n", material->GetName(), i, stage->texture.texgen );
			}
		}

		ir->surfaceStages.Append( s );
	}

	return ir;
}

/*
=============
IR_Get
=============
*/
const MaterialIR *IR_Get( const idMaterial *material ) {
	int key = (int)( ( (uintptr_t)material >> 4 ) & 0x7fffffff );
	for ( int i = irHash.First( key ); i != -1; i = irHash.Next( i ) ) {
		if ( irList[i]->material == material ) {
			return irList[i];
		}
	}
	MaterialIR *ir = IR_Build( material );
	int index = irList.Append( ir );
	irHash.Add( key, index );
	return ir;
}

/*
=============
IR_Purge
=============
*/
void IR_Purge() {
	for ( int i = 0; i < irList.Num(); i++ ) {
		delete irList[i];
	}
	irList.Clear();
	irHash.Free();
}

} // namespace rhi
