/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.cpp for license header)
===========================================================================
*/

// DUDE RHI 3D world rendering — Phase 3 Chunk E.
//
// Depth prepass, stencil shadow volumes and per-light interactions,
// mirroring RB_STD_FillDepthBuffer / RB_StencilShadowPass / RB_T_Shadow /
// RB_ARB2_DrawInteractions pass for pass and state bit for state bit.
// The interaction decomposition (RB_CreateSingleDrawInteractions and
// RB_DetermineLightScale) is REUSED from tr_render.cpp — it is pure
// idTech4 semantics; only its draw callback is ours.
//
// Stencil state is driven with direct qgl calls like the legacy path
// (TODO(RHI): becomes pipeline state in the Vulkan backend). Shadow
// volumes always use glStencilOpSeparate — core since GL 2.0.

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/VertexCache.h"
#include "renderer/Model.h"
#include "renderer/rhi/RHI.h"
#include "renderer/rhi/GL3Local.h"
#include "renderer/rhi/RenderParams.h"

extern idCVar r_useCarmacksReverse;		// defined in RenderSystem_init.cpp, no tr_local decl

/*
===================
interaction draw context

RB_CreateSingleDrawInteractions takes a plain function pointer, so the
per-surface stream handles and per-light state travel through this.
===================
*/
static struct {
	rhi::RHI *			r;
	const viewDef_t *	viewDef;
	float				mvp[16];
	rhi::BufferHandle	vb, ib;
	int					vertOfs, idxOfs;
	rhi::ShaderHandle	interactionProg;
	rhi::ShaderHandle	ambientProg;
	int					depthFuncBits;		// GLS_DEPTHFUNC_EQUAL, LESS for translucents
} ictx;

/*
===================
RB_RHI_BindUnit
===================
*/
static void RB_RHI_BindUnit( int unit, idImage *image ) {
	rhi::gl3ActiveTexture( GL_TEXTURE0 + unit );
	backEnd.glState.currenttmu = unit;
	image->Bind();
}

/*
===================
RB_RHI_DrawInteraction

Callback for RB_CreateSingleDrawInteractions; the RenderParams mapping is
documented member by member in shaders/renderparms.glsl.
===================
*/
static void RB_RHI_DrawInteraction( const drawInteraction_t *din ) {
	rhi::RenderParams parms;
	memset( &parms, 0, sizeof( parms ) );
	memcpy( parms.mvpMatrix, ictx.mvp, sizeof( parms.mvpMatrix ) );

	memcpy( parms.localLightOrigin, din->localLightOrigin.ToFloatPtr(), 16 );
	memcpy( parms.localViewOrigin, din->localViewOrigin.ToFloatPtr(), 16 );
	memcpy( parms.lightProjectionS, din->lightProjection[0].ToFloatPtr(), 16 );
	memcpy( parms.lightProjectionT, din->lightProjection[1].ToFloatPtr(), 16 );
	memcpy( parms.lightProjectionQ, din->lightProjection[2].ToFloatPtr(), 16 );
	memcpy( parms.lightFalloffS, din->lightProjection[3].ToFloatPtr(), 16 );
	memcpy( parms.bumpMatrixS, din->bumpMatrix[0].ToFloatPtr(), 16 );
	memcpy( parms.bumpMatrixT, din->bumpMatrix[1].ToFloatPtr(), 16 );
	memcpy( parms.diffuseMatrixS, din->diffuseMatrix[0].ToFloatPtr(), 16 );
	memcpy( parms.diffuseMatrixT, din->diffuseMatrix[1].ToFloatPtr(), 16 );
	memcpy( parms.specularMatrixS, din->specularMatrix[0].ToFloatPtr(), 16 );
	memcpy( parms.specularMatrixT, din->specularMatrix[1].ToFloatPtr(), 16 );

	switch ( din->vertexColor ) {
	case SVC_IGNORE:
		parms.vertexColorAdd[0] = parms.vertexColorAdd[1] = parms.vertexColorAdd[2] = parms.vertexColorAdd[3] = 1.0f;
		break;
	case SVC_MODULATE:
		parms.vertexColorModulate[0] = parms.vertexColorModulate[1] = parms.vertexColorModulate[2] = parms.vertexColorModulate[3] = 1.0f;
		break;
	case SVC_INVERSE_MODULATE:
		parms.vertexColorModulate[0] = parms.vertexColorModulate[1] = parms.vertexColorModulate[2] = parms.vertexColorModulate[3] = -1.0f;
		parms.vertexColorAdd[0] = parms.vertexColorAdd[1] = parms.vertexColorAdd[2] = parms.vertexColorAdd[3] = 1.0f;
		break;
	}

	memcpy( parms.diffuseModifier, din->diffuseColor.ToFloatPtr(), 16 );
	memcpy( parms.specularModifier, din->specularColor.ToFloatPtr(), 16 );

	// ambientlight.vert rebuilds a tangent-to-global rotation from these
	const float *mm = din->surf->space->modelMatrix;
	for ( int row = 0; row < 3; row++ ) {
		float *dst = row == 0 ? parms.modelMatrixRow0 : ( row == 1 ? parms.modelMatrixRow1 : parms.modelMatrixRow2 );
		dst[0] = mm[row];
		dst[1] = mm[row + 4];
		dst[2] = mm[row + 8];
		dst[3] = mm[row + 12];
	}

	// texture units exactly as RB_ARB2_DrawInteraction / the README table
	RB_RHI_BindUnit( 0, din->ambientLight ? globalImages->ambientNormalMap : globalImages->normalCubeMapImage );
	RB_RHI_BindUnit( 1, din->bumpImage );
	RB_RHI_BindUnit( 2, din->lightFalloffImage );
	RB_RHI_BindUnit( 3, din->lightImage );
	RB_RHI_BindUnit( 4, din->diffuseImage );
	RB_RHI_BindUnit( 5, din->specularImage );
	RB_RHI_BindUnit( 6, globalImages->specularTableImage );

	rhi::BufferHandle ub;
	int uniOfs = ictx.r->AllocUniforms( &parms, sizeof( parms ), &ub );

	rhi::PipelineDesc pd;
	pd.stateBits = GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | ictx.depthFuncBits;
	pd.shader = din->ambientLight ? ictx.ambientProg : ictx.interactionProg;
	pd.vertexLayout = rhi::VL_DRAWVERT;
	pd.cullType = RB_RHI_CullFor( ictx.viewDef, CT_FRONT_SIDED );
	ictx.r->BindPipeline( pd );

	rhi::DrawArgs da;
	memset( &da, 0, sizeof( da ) );
	da.vertexBuffer = ictx.vb;
	da.vertexOffset = ictx.vertOfs;
	da.indexBuffer = ictx.ib;
	da.firstIndex = ictx.idxOfs / (int)sizeof( glIndex_t );
	da.indexCount = din->surf->geo->numIndexes;
	da.uniformBuffer = ub;
	da.uniformOffset = uniOfs;
	da.uniformSize = sizeof( parms );
	ictx.r->Draw( da );

	backEnd.pc.c_drawElements++;
	backEnd.pc.c_drawIndexes += din->surf->geo->numIndexes;
}

/*
===================
RB_RHI_CreateDrawInteractions

Mirrors RB_ARB2_CreateDrawInteractions: state is constant over the chain,
the idTech4 stage decomposition runs unchanged.
===================
*/
static void RB_RHI_CreateDrawInteractions( const drawSurf_t *surf ) {
	for ( ; surf; surf = surf->nextOnLight ) {
		if ( !surf->geo || !surf->geo->ambientCache ) {
			continue;
		}
		RB_RHI_SpaceMvp( ictx.viewDef, surf->space, ictx.mvp );
		RB_RHI_StreamAmbient( ictx.r, surf->geo, ictx.vb, ictx.vertOfs, ictx.ib, ictx.idxOfs );

		// stage decomposition + register evaluation + depth-range hacks;
		// calls RB_RHI_DrawInteraction 1..n times
		RB_CreateSingleDrawInteractions( surf, RB_RHI_DrawInteraction );
	}
}

/*
===================
RB_RHI_StencilShadowPass

Mirrors RB_StencilShadowPass/RB_T_Shadow, always through StencilOpSeparate
(core since GL 2.0; the multi-pass fallbacks are for pre-2.0 hardware the
GL3 backend can't run on anyway).
===================
*/
static void RB_RHI_StencilShadowPass( rhi::RHI *r, const viewDef_t *viewDef, const drawSurf_t *drawSurfs, rhi::ShaderHandle shadowProg ) {
	if ( !r_shadows.GetBool() || !drawSurfs ) {
		return;
	}

	if ( r_shadowPolygonFactor.GetFloat() || r_shadowPolygonOffset.GetFloat() ) {
		qglPolygonOffset( r_shadowPolygonFactor.GetFloat(), -r_shadowPolygonOffset.GetFloat() );
		qglEnable( GL_POLYGON_OFFSET_FILL );
	}

	qglStencilFunc( GL_ALWAYS, 1, 255 );

	const GLenum firstFace = viewDef->isMirror ? GL_FRONT : GL_BACK;
	const GLenum secondFace = viewDef->isMirror ? GL_BACK : GL_FRONT;
	const bool zFail = r_useCarmacksReverse.GetBool();

	rhi::PipelineDesc pd;
	pd.stateBits = GLS_DEPTHMASK | GLS_COLORMASK | GLS_ALPHAMASK | GLS_DEPTHFUNC_LESS;
	pd.shader = shadowProg;
	pd.vertexLayout = rhi::VL_SHADOW;
	pd.cullType = CT_TWO_SIDED;

	const viewEntity_t *currentSpace = NULL;
	float mvp[16];
	idVec4 localLight( 0.0f, 0.0f, 0.0f, 0.0f );

	for ( const drawSurf_t *surf = drawSurfs; surf; surf = surf->nextOnLight ) {
		const srfTriangles_t *tri = surf->geo;

		if ( !tri->shadowCache ) {
			continue;
		}

		if ( surf->space != currentSpace ) {
			currentSpace = surf->space;
			RB_RHI_SpaceMvp( viewDef, surf->space, mvp );
			R_GlobalPointToLocal( surf->space->modelMatrix, backEnd.vLight->globalLightOrigin, localLight.ToVec3() );
		}

		// scissor like RB_RenderDrawSurfChainWithFunction
		if ( r_useScissor.GetBool() && !backEnd.currentScissor.Equals( surf->scissorRect ) ) {
			backEnd.currentScissor = surf->scissorRect;
			r->SetScissor( viewDef->viewport.x1 + backEnd.currentScissor.x1,
			               viewDef->viewport.y1 + backEnd.currentScissor.y1,
			               backEnd.currentScissor.x2 + 1 - backEnd.currentScissor.x1,
			               backEnd.currentScissor.y2 + 1 - backEnd.currentScissor.y1 );
		}

		// front/rear cap requirements — identical logic to RB_T_Shadow
		int numIndexes;
		bool external = false;
		if ( !r_useExternalShadows.GetInteger() ) {
			numIndexes = tri->numIndexes;
		} else if ( r_useExternalShadows.GetInteger() == 2 ) {
			numIndexes = tri->numShadowIndexesNoCaps;
		} else if ( !( surf->dsFlags & DSF_VIEW_INSIDE_SHADOW ) ) {
			numIndexes = tri->numShadowIndexesNoCaps;
			external = true;
		} else if ( !backEnd.vLight->viewInsideLight && !( tri->shadowCapPlaneBits & SHADOW_CAP_INFINITE ) ) {
			if ( backEnd.vLight->viewSeesShadowPlaneBits & tri->shadowCapPlaneBits ) {
				numIndexes = tri->numShadowIndexesNoFrontCaps;
			} else {
				numIndexes = tri->numShadowIndexesNoCaps;
			}
			external = true;
		} else {
			numIndexes = tri->numIndexes;
		}

		rhi::RenderParams parms;
		memset( &parms, 0, sizeof( parms ) );
		memcpy( parms.mvpMatrix, mvp, sizeof( parms.mvpMatrix ) );
		memcpy( parms.localLightOrigin, localLight.ToFloatPtr(), 16 );

		rhi::BufferHandle ub;
		int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

		rhi::BufferHandle vb, ib;
		int vertOfs, idxOfs;
		RB_RHI_StreamShadow( r, tri, vb, vertOfs, ib, idxOfs );

		r->BindPipeline( pd );

		rhi::DrawArgs da;
		memset( &da, 0, sizeof( da ) );
		da.vertexBuffer = vb;
		da.vertexOffset = vertOfs;
		da.indexBuffer = ib;
		da.firstIndex = idxOfs / (int)sizeof( glIndex_t );
		da.indexCount = numIndexes;
		da.uniformBuffer = ub;
		da.uniformOffset = uniOfs;
		da.uniformSize = sizeof( parms );

		if ( !zFail ) {
			// depth-pass with preload for volumes clipped by near/far planes
			if ( !external ) {
				qglStencilOpSeparate( firstFace, GL_KEEP, tr.stencilDecr, tr.stencilDecr );
				qglStencilOpSeparate( secondFace, GL_KEEP, tr.stencilIncr, tr.stencilIncr );
				r->Draw( da );
			}
			qglStencilOpSeparate( firstFace, GL_KEEP, GL_KEEP, tr.stencilIncr );
			qglStencilOpSeparate( secondFace, GL_KEEP, GL_KEEP, tr.stencilDecr );
			r->Draw( da );
		} else {
			// Carmack's Reverse (Z-fail) — patent expired 2019-10-13
			if ( !external ) {
				qglStencilOpSeparate( firstFace, GL_KEEP, tr.stencilDecr, GL_KEEP );
				qglStencilOpSeparate( secondFace, GL_KEEP, tr.stencilIncr, GL_KEEP );
			} else {
				qglStencilOpSeparate( firstFace, GL_KEEP, GL_KEEP, tr.stencilIncr );
				qglStencilOpSeparate( secondFace, GL_KEEP, GL_KEEP, tr.stencilDecr );
			}
			r->Draw( da );
		}

		backEnd.pc.c_shadowElements++;
		backEnd.pc.c_shadowIndexes += numIndexes;
	}

	if ( r_shadowPolygonFactor.GetFloat() || r_shadowPolygonOffset.GetFloat() ) {
		qglDisable( GL_POLYGON_OFFSET_FILL );
	}

	// interactions test against the unshadowed value
	qglStencilFunc( GL_GEQUAL, 128, 255 );
	qglStencilOp( GL_KEEP, GL_KEEP, GL_KEEP );
}

/*
===================
RB_RHI_FillDepthBuffer

Mirrors RB_STD_FillDepthBuffer/RB_T_FillDepthBuffer through the zfill
program (opaque solid, perforated alpha-tested, subview down-modulate).
===================
*/
static void RB_RHI_FillDepthBuffer( rhi::RHI *r, const viewDef_t *viewDef ) {
	rhi::ShaderHandle zfill = r->LoadShader( "zfill" );

	if ( viewDef->numClipPlanes ) {
		// legacy uses an alpha-notch texgen trick; ours will use
		// gl_ClipDistance when subview polish lands (Chunk G)
		RB_RHI_LogOnce( "subview near-clip planes" );
	}

	qglStencilFunc( GL_ALWAYS, 1, 255 );

	const viewEntity_t *currentSpace = NULL;
	float mvp[16];

	drawSurf_t **drawSurfs = (drawSurf_t **)&viewDef->drawSurfs[0];
	for ( int i = 0; i < viewDef->numDrawSurfs; i++ ) {
		const drawSurf_t *surf = drawSurfs[i];
		const srfTriangles_t *tri = surf->geo;
		const idMaterial *shader = surf->material;

		if ( !shader->IsDrawn() ) {
			continue;
		}
		// translucent surfaces don't put anything in the depth buffer
		if ( shader->Coverage() == MC_TRANSLUCENT ) {
			continue;
		}
		if ( !tri->numIndexes || !tri->ambientCache ) {
			continue;
		}

		const float *regs = surf->shaderRegisters;

		// if all stages of a material have been conditioned off, don't draw
		int stage;
		for ( stage = 0; stage < shader->GetNumStages(); stage++ ) {
			if ( regs[shader->GetStage( stage )->conditionRegister] != 0 ) {
				break;
			}
		}
		if ( stage == shader->GetNumStages() ) {
			continue;
		}

		if ( surf->space != currentSpace ) {
			currentSpace = surf->space;
			RB_RHI_SpaceMvp( viewDef, surf->space, mvp );
		}
		if ( surf->space->weaponDepthHack ) {
			RB_EnterWeaponDepthHack();
		}
		if ( surf->space->modelDepthHack != 0.0f ) {
			RB_EnterModelDepthHack( surf->space->modelDepthHack );
		}

		if ( r_useScissor.GetBool() && !backEnd.currentScissor.Equals( surf->scissorRect ) ) {
			backEnd.currentScissor = surf->scissorRect;
			r->SetScissor( viewDef->viewport.x1 + backEnd.currentScissor.x1,
			               viewDef->viewport.y1 + backEnd.currentScissor.y1,
			               backEnd.currentScissor.x2 + 1 - backEnd.currentScissor.x1,
			               backEnd.currentScissor.y2 + 1 - backEnd.currentScissor.y1 );
		}

		if ( shader->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
			qglEnable( GL_POLYGON_OFFSET_FILL );
			qglPolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
		}

		// subviews down-modulate the color buffer, everything else draws black
		int stateBits;
		float color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		if ( shader->GetSort() == SS_SUBVIEW ) {
			stateBits = GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO | GLS_DEPTHFUNC_LESS;
			color[0] = color[1] = color[2] = 1.0f / backEnd.overBright;
		} else {
			stateBits = GLS_DEPTHFUNC_LESS;
		}

		rhi::BufferHandle vb, ib;
		int vertOfs, idxOfs;
		RB_RHI_StreamAmbient( r, tri, vb, vertOfs, ib, idxOfs );

		rhi::PipelineDesc pd;
		pd.stateBits = stateBits;
		pd.shader = zfill;
		pd.vertexLayout = rhi::VL_DRAWVERT;
		pd.cullType = RB_RHI_CullFor( viewDef, CT_FRONT_SIDED );

		rhi::DrawArgs da;
		memset( &da, 0, sizeof( da ) );
		da.vertexBuffer = vb;
		da.vertexOffset = vertOfs;
		da.indexBuffer = ib;
		da.firstIndex = idxOfs / (int)sizeof( glIndex_t );
		da.indexCount = tri->numIndexes;

		bool drawSolid = ( shader->Coverage() == MC_OPAQUE );

		if ( shader->Coverage() == MC_PERFORATED ) {
			// alpha-tested stages; if none were live, fall back to solid
			bool didDraw = false;
			for ( stage = 0; stage < shader->GetNumStages(); stage++ ) {
				const shaderStage_t *pStage = shader->GetStage( stage );
				if ( !pStage->hasAlphaTest ) {
					continue;
				}
				if ( regs[pStage->conditionRegister] == 0 ) {
					continue;
				}
				didDraw = true;

				color[3] = regs[pStage->color.registers[3]];
				if ( color[3] <= 0.0f ) {
					continue;
				}

				rhi::RenderParams parms;
				memset( &parms, 0, sizeof( parms ) );
				memcpy( parms.mvpMatrix, mvp, sizeof( parms.mvpMatrix ) );
				memcpy( parms.color, color, sizeof( parms.color ) );
				parms.alphaTest[0] = regs[pStage->alphaTestRegister];
				parms.alphaTest[1] = 1.0f;
				if ( pStage->texture.hasMatrix ) {
					parms.diffuseMatrixS[0] = regs[pStage->texture.matrix[0][0]];
					parms.diffuseMatrixS[1] = regs[pStage->texture.matrix[0][1]];
					parms.diffuseMatrixS[3] = regs[pStage->texture.matrix[0][2]];
					parms.diffuseMatrixT[0] = regs[pStage->texture.matrix[1][0]];
					parms.diffuseMatrixT[1] = regs[pStage->texture.matrix[1][1]];
					parms.diffuseMatrixT[3] = regs[pStage->texture.matrix[1][2]];
				} else {
					parms.diffuseMatrixS[0] = 1.0f;
					parms.diffuseMatrixT[1] = 1.0f;
				}

				rhi::BufferHandle ub;
				int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

				RB_RHI_BindUnit( 0, pStage->texture.image );
				r->BindPipeline( pd );
				da.uniformBuffer = ub;
				da.uniformOffset = uniOfs;
				da.uniformSize = sizeof( parms );
				r->Draw( da );
				backEnd.pc.c_drawElements++;
			}
			if ( !didDraw ) {
				drawSolid = true;
			}
		}

		if ( drawSolid ) {
			rhi::RenderParams parms;
			memset( &parms, 0, sizeof( parms ) );
			memcpy( parms.mvpMatrix, mvp, sizeof( parms.mvpMatrix ) );
			memcpy( parms.color, color, sizeof( parms.color ) );
			parms.diffuseMatrixS[0] = 1.0f;
			parms.diffuseMatrixT[1] = 1.0f;

			rhi::BufferHandle ub;
			int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

			RB_RHI_BindUnit( 0, globalImages->whiteImage );
			r->BindPipeline( pd );
			da.uniformBuffer = ub;
			da.uniformOffset = uniOfs;
			da.uniformSize = sizeof( parms );
			r->Draw( da );
			backEnd.pc.c_drawElements++;
		}

		if ( shader->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
			qglDisable( GL_POLYGON_OFFSET_FILL );
		}
		if ( surf->space->weaponDepthHack || surf->space->modelDepthHack != 0.0f ) {
			RB_LeaveDepthHack();
		}
	}

	// make the early depth pass available to shaders (soft particles etc.)
	bool getDepthCapture = r_enableDepthCapture.GetInteger() == 1
		|| ( r_enableDepthCapture.GetInteger() == -1 && r_useSoftParticles.GetBool() );
	if ( getDepthCapture && viewDef->renderView.viewID >= 0 ) {
		globalImages->currentDepthImage->CopyDepthbuffer( viewDef->viewport.x1,
			viewDef->viewport.y1,
			viewDef->viewport.x2 - viewDef->viewport.x1 + 1,
			viewDef->viewport.y2 - viewDef->viewport.y1 + 1, true );
	}
}

/*
===================
RB_RHI_DrawWorld

Depth prepass + stencil shadows + per-light interactions, following the
RB_STD_DrawView / RB_ARB2_DrawInteractions pass order.
===================
*/
void RB_RHI_DrawWorld( rhi::RHI *r, viewDef_s *viewDef ) {
	rhi::ShaderHandle shadowProg = r->LoadShader( "shadow" );

	ictx.r = r;
	ictx.viewDef = viewDef;
	ictx.interactionProg = r->LoadShader( "interaction" );
	ictx.ambientProg = r->LoadShader( "ambientlight" );

	// sets backEnd.lightScale/overBright, read by the reused
	// RB_CreateSingleDrawInteractions
	RB_DetermineLightScale();

	// depth prepass with stencil test enabled for invariance with the
	// shadowed passes (matches RB_STD_FillDepthBuffer)
	qglEnable( GL_STENCIL_TEST );
	backEnd.currentScissor = viewDef->scissor;

	RB_RHI_FillDepthBuffer( r, viewDef );

	// per-light shadowing and adding (matches RB_ARB2_DrawInteractions)
	if ( !r_skipInteractions.GetBool() ) {
		for ( viewLight_t *vLight = viewDef->viewLights; vLight; vLight = vLight->next ) {
			backEnd.vLight = vLight;

			// fogging / blend lights happen after shader passes (Chunk F)
			if ( vLight->lightShader->IsFogLight() || vLight->lightShader->IsBlendLight() ) {
				continue;
			}
			if ( !vLight->localInteractions && !vLight->globalInteractions && !vLight->translucentInteractions ) {
				continue;
			}

			// clear the stencil buffer for this light's scissor if shadowed
			if ( vLight->globalShadows || vLight->localShadows ) {
				backEnd.currentScissor = vLight->scissorRect;
				if ( r_useScissor.GetBool() ) {
					r->SetScissor( viewDef->viewport.x1 + backEnd.currentScissor.x1,
					               viewDef->viewport.y1 + backEnd.currentScissor.y1,
					               backEnd.currentScissor.x2 + 1 - backEnd.currentScissor.x1,
					               backEnd.currentScissor.y2 + 1 - backEnd.currentScissor.y1 );
				}
				qglClear( GL_STENCIL_BUFFER_BIT );
			} else {
				qglStencilFunc( GL_ALWAYS, 128, 255 );
			}

			ictx.depthFuncBits = GLS_DEPTHFUNC_EQUAL;
			RB_RHI_StencilShadowPass( r, viewDef, vLight->globalShadows, shadowProg );
			RB_RHI_CreateDrawInteractions( vLight->localInteractions );
			RB_RHI_StencilShadowPass( r, viewDef, vLight->localShadows, shadowProg );
			RB_RHI_CreateDrawInteractions( vLight->globalInteractions );

			// translucent surfaces never get stencil shadowed
			if ( r_skipTranslucent.GetBool() ) {
				continue;
			}
			qglStencilFunc( GL_ALWAYS, 128, 255 );
			ictx.depthFuncBits = GLS_DEPTHFUNC_LESS;
			RB_RHI_CreateDrawInteractions( vLight->translucentInteractions );
		}
	}
	backEnd.vLight = NULL;

	// shader passes run with stencil satisfied everywhere
	qglStencilFunc( GL_ALWAYS, 128, 255 );
}
