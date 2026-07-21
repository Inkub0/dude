/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.cpp for license header)
===========================================================================
*/

// DUDE RHI command executor — Phase 3 Chunk C.
//
// Translates the idTech4 backend command list into RHI calls; this layer is
// what the Vulkan backend will reuse unchanged. Current coverage:
//   - 2D views (menu/console/HUD/loading): full old-style shader stage
//     rendering through the generic program
//   - 3D views: cleared to magenta placeholder (world rendering = Chunk E)
//   - skipped with a one-time notice: custom ARB stages (Chunk F),
//     non-explicit texgen (Chunk E/F), RC_COPY_RENDER (Chunk F)
//
// Two acknowledged impurities, both bridged until Phase 4:
//   - engine textures bind via idImage::Bind() (core-safe since the
//     Image_load.cpp guards), not through DrawArgs::textures
//   - polygon offset is a direct GL call (needs an RHI dynamic state)

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/VertexCache.h"
#include "renderer/Cinematic.h"
#include "renderer/rhi/RHI.h"
#include "renderer/rhi/GL3Local.h"
#include "renderer/rhi/RenderParams.h"
#include "renderer/rhi/ArbParamsBlock.h"
#include "renderer/rhi/MaterialIR.h"

static void RB_RHI_LogOnce( const char *what ) {
	static idStr logged;
	if ( logged.Find( what ) < 0 ) {
		logged += what;
		logged += ";";
		common->Printf( "RHI backend: %s (not rendered yet)\n", what );
	}
}

/*
=============
RB_RHI_BindStageImage

Binds the stage image (or current cinematic frame) on texture unit 0.
=============
*/
static void RB_RHI_BindStageImage( const shaderStage_t *pStage, const float *regs, const viewDef_t *viewDef ) {
	const textureStage_t *texture = &pStage->texture;

	rhi::gl3ActiveTexture( GL_TEXTURE0 );
	backEnd.glState.currenttmu = 0;		// keep idImage::Bind's per-tmu cache honest

	if ( texture->cinematic ) {
		if ( r_skipDynamicTextures.GetBool() ) {
			globalImages->defaultImage->Bind();
			return;
		}
		cinData_t cin = texture->cinematic->ImageForTime( (int)( 1000 * ( viewDef->floatTime + viewDef->renderView.shaderParms[11] ) ) );
		if ( cin.image ) {
			globalImages->cinematicImage->UploadScratch( cin.image, cin.imageWidth, cin.imageHeight );
		} else {
			globalImages->blackImage->Bind();
		}
	} else if ( texture->image ) {
		texture->image->Bind();
	}
}

/*
=============
RB_RHI_RenderCustomStage

newStage (custom ARB) drawn with its transpiled program pair. The ArbParams
fill mirrors RB_SetProgramEnvironment/Space and the legacy newStage path
(vertexParms -> program.local, fragmentProgramImages -> units).
=============
*/
static void RB_RHI_RenderCustomStage( rhi::RHI *r, const viewDef_t *viewDef, const drawSurf_t *surf,
                                      const shaderStage_t *pStage, rhi::ShaderHandle program, const float mvp[16],
                                      rhi::BufferHandle vb, int vertOfs, rhi::BufferHandle ib, int idxOfs ) {
	const srfTriangles_t *tri = surf->geo;
	const newShaderStage_t *ns = pStage->newStage;
	const float *regs = surf->shaderRegisters;

	rhi::ArbParams ap;
	memset( &ap, 0, sizeof( ap ) );
	memcpy( ap.mvpMatrix, mvp, sizeof( ap.mvpMatrix ) );
	memcpy( ap.modelViewMatrix, surf->space->modelViewMatrix, sizeof( ap.modelViewMatrix ) );
	memcpy( ap.projectionMatrix, viewDef->projectionMatrix, sizeof( ap.projectionMatrix ) );
	ap.textureMatrix[0] = ap.textureMatrix[5] = ap.textureMatrix[10] = ap.textureMatrix[15] = 1.0f;

	// RB_SetProgramEnvironment: env[0] = screen POT correction (both stages),
	// fragment env[1] = window coord scale, fragment env[22] = depth recips;
	// vertex env[1] = global view origin. _currentRender isn't captured yet
	// (Chunk F), so fall back to viewport size while uploadWidth is 0.
	int w = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	int h = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
	int potW = globalImages->currentRenderImage->uploadWidth > 0 ? globalImages->currentRenderImage->uploadWidth : w;
	int potH = globalImages->currentRenderImage->uploadHeight > 0 ? globalImages->currentRenderImage->uploadHeight : h;
	ap.venv[0][0] = ap.fenv[0][0] = (float)w / potW;
	ap.venv[0][1] = ap.fenv[0][1] = (float)h / potH;
	ap.venv[0][3] = ap.fenv[0][3] = 1.0f;
	ap.fenv[1][0] = 1.0f / w;
	ap.fenv[1][1] = 1.0f / h;
	ap.fenv[1][3] = 1.0f;
	if ( globalImages->currentDepthImage->uploadWidth > 0 ) {
		ap.fenv[22][0] = 1.0f / globalImages->currentDepthImage->uploadWidth;
		ap.fenv[22][1] = 1.0f / globalImages->currentDepthImage->uploadHeight;
		ap.fenv[22][2] = (float)potW / globalImages->currentDepthImage->uploadWidth;
		ap.fenv[22][3] = (float)potH / globalImages->currentDepthImage->uploadHeight;
	}
	ap.venv[1][0] = viewDef->renderView.vieworg[0];
	ap.venv[1][1] = viewDef->renderView.vieworg[1];
	ap.venv[1][2] = viewDef->renderView.vieworg[2];
	ap.venv[1][3] = 1.0f;

	// RB_SetProgramEnvironmentSpace: vertex env[5] = view origin in local
	// space, env[6-8] = model matrix rows
	idVec3 localView;
	R_GlobalPointToLocal( surf->space->modelMatrix, viewDef->renderView.vieworg, localView );
	ap.venv[5][0] = localView[0];
	ap.venv[5][1] = localView[1];
	ap.venv[5][2] = localView[2];
	ap.venv[5][3] = 1.0f;
	const float *mm = surf->space->modelMatrix;
	for ( int row = 0; row < 3; row++ ) {
		ap.venv[6 + row][0] = mm[row];
		ap.venv[6 + row][1] = mm[row + 4];
		ap.venv[6 + row][2] = mm[row + 8];
		ap.venv[6 + row][3] = mm[row + 12];
	}

	// vertexParms -> vertex program.local
	for ( int i = 0; i < ns->numVertexParms; i++ ) {
		ap.vlocal[i][0] = regs[ns->vertexParms[i][0]];
		ap.vlocal[i][1] = regs[ns->vertexParms[i][1]];
		ap.vlocal[i][2] = regs[ns->vertexParms[i][2]];
		ap.vlocal[i][3] = regs[ns->vertexParms[i][3]];
	}

	// fragment program images by unit
	for ( int i = 0; i < ns->numFragmentProgramImages; i++ ) {
		if ( ns->fragmentProgramImages[i] ) {
			rhi::gl3ActiveTexture( GL_TEXTURE0 + i );
			backEnd.glState.currenttmu = i;
			ns->fragmentProgramImages[i]->Bind();
		}
	}

	rhi::BufferHandle ub;
	int uniOfs = r->AllocUniforms( &ap, sizeof( ap ), &ub );

	rhi::PipelineDesc pd;
	pd.stateBits = ( pStage->drawStateBits & ~GLS_ATEST_BITS );
	if ( !viewDef->viewEntitys ) {
		pd.stateBits |= GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK;
	}
	pd.shader = program;
	pd.vertexLayout = rhi::VL_DRAWVERT;
	pd.cullType = surf->material->GetCullType();
	r->BindPipeline( pd );

	rhi::DrawArgs da;
	memset( &da, 0, sizeof( da ) );
	da.vertexBuffer = vb;
	da.vertexOffset = vertOfs;
	da.indexBuffer = ib;
	da.firstIndex = idxOfs / (int)sizeof( glIndex_t );
	da.indexCount = tri->numIndexes;
	da.uniformBuffer = ub;
	da.uniformOffset = uniOfs;
	da.uniformSize = sizeof( ap );
	r->Draw( da );

	backEnd.pc.c_drawElements++;
	backEnd.pc.c_drawIndexes += tri->numIndexes;
	backEnd.pc.c_drawVertexes += tri->numVerts;
}

/*
=============
RB_RHI_RenderShaderPasses

Ambient stages of one surface, driven by the Material IR: old-style stages
through the generic program, newStage customs through their transpiled ARB
pairs. Mirrors RB_STD_T_RenderShaderPasses stage for stage.
=============
*/
static void RB_RHI_RenderShaderPasses( rhi::RHI *r, const viewDef_t *viewDef, const drawSurf_t *surf,
                                       const viewEntity_t *&currentSpace, idScreenRect &currentScissor,
                                       float mvp[16] ) {
	const srfTriangles_t *tri = surf->geo;
	const idMaterial *shader = surf->material;

	if ( !shader->HasAmbient() || shader->IsPortalSky() ) {
		return;
	}

	if ( surf->space != currentSpace ) {
		currentSpace = surf->space;
		myGlMultMatrix( surf->space->modelViewMatrix, viewDef->projectionMatrix, mvp );
	}

	if ( r_useScissor.GetBool() && !currentScissor.Equals( surf->scissorRect ) ) {
		currentScissor = surf->scissorRect;
		r->SetScissor( viewDef->viewport.x1 + currentScissor.x1,
		               viewDef->viewport.y1 + currentScissor.y1,
		               currentScissor.x2 + 1 - currentScissor.x1,
		               currentScissor.y2 + 1 - currentScissor.y1 );
	}

	if ( !tri->numIndexes ) {
		return;
	}
	if ( !tri->ambientCache ) {
		common->Printf( "RB_RHI_RenderShaderPasses: !tri->ambientCache\n" );
		return;
	}

	const float *regs = surf->shaderRegisters;

	// TODO(RHI): dynamic state; direct GL is fine for both current backends
	if ( shader->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
		qglEnable( GL_POLYGON_OFFSET_FILL );
		qglPolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
	}

	// stream this surface's frame-temporary geometry once, draw per stage
	const idDrawVert *ac = (idDrawVert *)vertexCache.Position( tri->ambientCache );
	rhi::BufferHandle vb, ib;
	int vertOfs = r->AllocVertices( ac, tri->numVerts * (int)sizeof( idDrawVert ), &vb );
	int idxOfs = r->AllocIndices( tri->indexes, tri->numIndexes * (int)sizeof( glIndex_t ), &ib );

	const rhi::MaterialIR *ir = rhi::IR_Get( shader );

	for ( int k = 0; k < ir->surfaceStages.Num(); k++ ) {
		const rhi::StageIR &si = ir->surfaceStages[k];
		const shaderStage_t *pStage = shader->GetStage( si.stageNum );

		if ( regs[pStage->conditionRegister] == 0 ) {
			continue;
		}
		// skip ( GL_ZERO, GL_ONE ) stages, used for some alpha masks
		if ( ( pStage->drawStateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) == ( GLS_SRCBLEND_ZERO | GLS_DSTBLEND_ONE ) ) {
			continue;
		}
		if ( si.kind == rhi::SK_SKIP ) {
			continue;	// reason logged once at IR build
		}
		if ( si.kind == rhi::SK_CUSTOM_ARB ) {
			if ( si.needsCurrentRender ) {
				RB_RHI_LogOnce( "_currentRender-sampling custom stages" );
				continue;
			}
			RB_RHI_RenderCustomStage( r, viewDef, surf, pStage, si.program, mvp, vb, vertOfs, ib, idxOfs );
			continue;
		}

		float color[4];
		color[0] = regs[pStage->color.registers[0]];
		color[1] = regs[pStage->color.registers[1]];
		color[2] = regs[pStage->color.registers[2]];
		color[3] = regs[pStage->color.registers[3]];

		// skip stages that would draw nothing
		if ( ( pStage->drawStateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) == ( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE )
			&& color[0] <= 0 && color[1] <= 0 && color[2] <= 0 ) {
			continue;
		}
		if ( ( pStage->drawStateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) == ( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA )
			&& color[3] <= 0 ) {
			continue;
		}

		rhi::RenderParams parms;
		memset( &parms, 0, sizeof( parms ) );
		memcpy( parms.mvpMatrix, mvp, sizeof( parms.mvpMatrix ) );

		// texture matrix (same reg evaluation as R_SetDrawInteraction)
		if ( pStage->texture.hasMatrix ) {
			parms.diffuseMatrixS[0] = regs[pStage->texture.matrix[0][0]];
			parms.diffuseMatrixS[1] = regs[pStage->texture.matrix[0][1]];
			parms.diffuseMatrixS[2] = 0.0f;
			parms.diffuseMatrixS[3] = regs[pStage->texture.matrix[0][2]];
			parms.diffuseMatrixT[0] = regs[pStage->texture.matrix[1][0]];
			parms.diffuseMatrixT[1] = regs[pStage->texture.matrix[1][1]];
			parms.diffuseMatrixT[2] = 0.0f;
			parms.diffuseMatrixT[3] = regs[pStage->texture.matrix[1][2]];
			// keep scroll offsets from growing unbounded
			if ( parms.diffuseMatrixS[3] < -40.0f || parms.diffuseMatrixS[3] > 40.0f ) {
				parms.diffuseMatrixS[3] -= (int)parms.diffuseMatrixS[3];
			}
			if ( parms.diffuseMatrixT[3] < -40.0f || parms.diffuseMatrixT[3] > 40.0f ) {
				parms.diffuseMatrixT[3] -= (int)parms.diffuseMatrixT[3];
			}
		} else {
			parms.diffuseMatrixS[0] = 1.0f;
			parms.diffuseMatrixT[1] = 1.0f;
		}

		// vertex color mode: var_Color = (attr_Color*modulate + add) * u_color
		switch ( pStage->vertexColor ) {
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
		parms.color[0] = color[0];
		parms.color[1] = color[1];
		parms.color[2] = color[2];
		parms.color[3] = color[3];

		// fixed-function alpha test bits -> in-shader test (fail if a < ref)
		switch ( pStage->drawStateBits & GLS_ATEST_BITS ) {
		case GLS_ATEST_GE_128:
			parms.alphaTest[0] = 0.5f;
			parms.alphaTest[1] = 1.0f;
			break;
		case GLS_ATEST_EQ_255:
			parms.alphaTest[0] = 1.0f;
			parms.alphaTest[1] = 1.0f;
			break;
		case GLS_ATEST_LT_128:
			RB_RHI_LogOnce( "GLS_ATEST_LT_128 stages" );
			break;
		default:
			break;
		}

		rhi::BufferHandle ub;
		int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

		RB_RHI_BindStageImage( pStage, regs, viewDef );

		// 2D views: legacy disables depth test entirely; equivalent here is
		// depth-always + no depth writes
		rhi::PipelineDesc pd;
		pd.stateBits = ( pStage->drawStateBits & ~GLS_ATEST_BITS );
		if ( !viewDef->viewEntitys ) {
			pd.stateBits |= GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK;
		}
		pd.shader = si.program;
		pd.vertexLayout = rhi::VL_DRAWVERT;
		pd.cullType = shader->GetCullType();
		r->BindPipeline( pd );

		rhi::DrawArgs da;
		memset( &da, 0, sizeof( da ) );
		da.vertexBuffer = vb;
		da.vertexOffset = vertOfs;
		da.indexBuffer = ib;
		da.firstIndex = idxOfs / (int)sizeof( glIndex_t );
		da.indexCount = tri->numIndexes;
		da.uniformBuffer = ub;
		da.uniformOffset = uniOfs;
		da.uniformSize = sizeof( parms );
		r->Draw( da );

		backEnd.pc.c_drawElements++;
		backEnd.pc.c_drawIndexes += tri->numIndexes;
		backEnd.pc.c_drawVertexes += tri->numVerts;
	}

	if ( shader->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
		qglDisable( GL_POLYGON_OFFSET_FILL );
	}
}

/*
=============
RB_RHI_DrawView
=============
*/
static void RB_RHI_DrawView( rhi::RHI *r, viewDef_t *viewDef ) {
	backEnd.viewDef = viewDef;	// engine helpers (cinematics, counters) read this

	// window clipping, matching RB_BeginDrawingView
	r->SetViewport( tr.viewportOffset[0] + viewDef->viewport.x1,
	                tr.viewportOffset[1] + viewDef->viewport.y1,
	                viewDef->viewport.x2 + 1 - viewDef->viewport.x1,
	                viewDef->viewport.y2 + 1 - viewDef->viewport.y1 );
	r->SetScissor( tr.viewportOffset[0] + viewDef->viewport.x1 + viewDef->scissor.x1,
	               tr.viewportOffset[1] + viewDef->viewport.y1 + viewDef->scissor.y1,
	               viewDef->scissor.x2 + 1 - viewDef->scissor.x1,
	               viewDef->scissor.y2 + 1 - viewDef->scissor.y1 );

	if ( viewDef->viewEntitys ) {
		// 3D world view: not rendered until Chunk E. Clear depth/stencil like
		// RB_BeginDrawingView but leave color alone — 3D views (including
		// idRenderWindow menu backgrounds) must not paint over already-drawn
		// GUI surfaces; unrendered world areas show the r_clear color.
		rhi::ClearArgs clear;
		clear.color = false;
		clear.depth = true;
		clear.stencil = true;
		clear.rgba[0] = clear.rgba[1] = clear.rgba[2] = 0.0f; clear.rgba[3] = 1.0f;
		clear.stencilValue = (unsigned char)( 1 << ( glConfig.stencilBits - 1 ) );
		r->BeginPass( &clear );
		r->EndPass();
		RB_RHI_LogOnce( "3D world views" );
		return;
	}

	// 2D view (menu/console/HUD/loading): shader passes over existing contents
	r->BeginPass( NULL );

	const viewEntity_t *currentSpace = NULL;
	idScreenRect currentScissor = viewDef->scissor;
	float mvp[16];

	drawSurf_t **drawSurfs = (drawSurf_t **)&viewDef->drawSurfs[0];
	for ( int i = 0; i < viewDef->numDrawSurfs; i++ ) {
		if ( drawSurfs[i]->material->SuppressInSubview() ) {
			continue;
		}
		RB_RHI_RenderShaderPasses( r, viewDef, drawSurfs[i], currentSpace, currentScissor, mvp );
	}

	r->EndPass();
}

/*
=============
RB_GL3_ExecuteBackEndCommands

RHI-based replacement for RB_ExecuteBackEndCommands (legacy path untouched).
=============
*/
void RB_GL3_ExecuteBackEndCommands( const emptyCommand_t *cmds ) {
	static bool announced = false;
	if ( !announced ) {
		announced = true;
		common->Printf( "GL3 backend: Chunk C - 2D/GUI/console rendering (3D views not drawn yet)\n" );
	}

	rhi::RHI *r = rhi::GetGL3RHI();
	r->BeginFrame( glConfig.vidWidth, glConfig.vidHeight );

	for ( ; cmds; cmds = (const emptyCommand_t *)cmds->next ) {
		switch ( cmds->commandId ) {
		case RC_NOP:
			break;
		case RC_DRAW_VIEW:
			RB_RHI_DrawView( r, ((const drawSurfsCommand_t *)cmds)->viewDef );
			break;
		case RC_SET_BUFFER: {
			// single (back) buffer only; keep frame counter + clear semantics
			// exactly matching RB_SetBuffer (r_clear defaults to 2 = black)
			const setBufferCommand_t *cmd = (const setBufferCommand_t *)cmds;
			backEnd.frameCount = cmd->frameCount;
			if ( r_clear.GetFloat() || idStr::Length( r_clear.GetString() ) != 1
				|| r_lockSurfaces.GetBool() || r_singleArea.GetBool() || r_showOverDraw.GetBool() ) {
				rhi::ClearArgs clear;
				clear.color = true;
				clear.depth = clear.stencil = false;
				clear.stencilValue = 0;
				float c[3];
				if ( sscanf( r_clear.GetString(), "%f %f %f", &c[0], &c[1], &c[2] ) == 3 ) {
					clear.rgba[0] = c[0]; clear.rgba[1] = c[1]; clear.rgba[2] = c[2];
				} else if ( r_clear.GetInteger() == 2 ) {
					clear.rgba[0] = clear.rgba[1] = clear.rgba[2] = 0.0f;
				} else if ( r_showOverDraw.GetBool() ) {
					clear.rgba[0] = clear.rgba[1] = clear.rgba[2] = 1.0f;
				} else {
					clear.rgba[0] = 0.4f; clear.rgba[1] = 0.0f; clear.rgba[2] = 0.25f;
				}
				clear.rgba[3] = 1.0f;
				r->BeginPass( &clear );
				r->EndPass();
			}
			break;
		}
		case RC_COPY_RENDER:
			RB_RHI_LogOnce( "RC_COPY_RENDER (_currentRender copies)" );
			break;
		case RC_SWAP_BUFFERS:
			GLimp_SwapBuffers();
			break;
		default:
			common->Error( "RB_GL3_ExecuteBackEndCommands: bad commandId" );
			break;
		}
	}

	r->EndFrame();
}
