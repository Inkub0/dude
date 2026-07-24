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

// GL 3.0 core enum; not always pulled in by the legacy gl.h include path
#ifndef GL_CLIP_DISTANCE0
#define GL_CLIP_DISTANCE0 0x3000
#endif

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

	// shadow mapping (DUDE Phase 3.5). The current light either uses a shadow map
	// (lightShadowMapped) or the stencil path. The shadow lookup reuses the light-
	// projection texgen (S/T/Q + falloff), so no matrix is carried here.
	// shadowImage is the depth texture bound on unit 7 for the interaction lookup.
	bool				lightShadowMapped;
	rhi::ImageHandle	shadowImage;
} ictx;

// Persistent shadow-map render target, kept across frames and recreated only
// when the resolution cvar changes or the context is lost (vid_restart returns
// a fresh backend, so a stale handle just fails GetRenderTargetImage and we
// remake it). One target reused serially by every shadow-mapped light in a frame.
static rhi::RenderTargetHandle rhiShadowMap = 0;
static int rhiShadowMapSize = 0;

/*
===================
RB_RHI_BindUnit
===================
*/
static void RB_RHI_BindUnit( int unit, idImage *image ) {
	// Skip the active-unit switch and rebind when this image is already bound
	// on this unit. Under one light the normalization cube, light falloff /
	// projection and specular-table maps are identical across every surface —
	// only bump/diffuse/specular change — so most of the 7 per-draw binds in
	// the interaction pass are redundant. idImage::Bind already caches the
	// glBindTexture, but the glActiveTexture around it was issued regardless.
	const tmu_t *tmu = &backEnd.glState.tmu[unit];
	if ( image->texnum != idImage::TEXTURE_NOT_LOADED ) {
		if ( ( image->type == TT_2D   && tmu->current2DMap   == image->texnum ) ||
		     ( image->type == TT_CUBIC && tmu->currentCubeMap == image->texnum ) ||
		     ( image->type == TT_3D   && tmu->current3DMap   == image->texnum ) ) {
			return;
		}
	}
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

	// DUDE Phase 3.5 specular tuning (interaction.frag). Defaults reproduce
	// vanilla: scale 1, shading model 0 (the N.H lookup table). Only consumed by
	// the regular interaction shader; the ambientLight shader ignores it.
	parms.specularParms[0] = r_specularScale.GetFloat();
	parms.specularParms[1] = r_specularExp.GetFloat();
	parms.specularParms[2] = (float)r_shading.GetInteger();
	parms.specularParms[3] = 0.0f;

	// shadow mapping (DUDE Phase 3.5): only the regular interaction shader samples
	// the depth map — the ambientLight pass has no shadow term. The lookup reuses
	// the light-projection texgen already filled above (lightProjection[]), so no
	// extra matrix is needed here. Left zero (memset) for stencil / unshadowed
	// lights -> u_shadowParms.x == 0 -> visibility 1.
	if ( ictx.lightShadowMapped && !din->ambientLight ) {
		parms.shadowParms[0] = 1.0f;
		parms.shadowParms[1] = ( rhiShadowMapSize > 0 ) ? 1.0f / (float)rhiShadowMapSize : 0.0f;
		parms.shadowParms[2] = r_shadowMapBias.GetFloat();
	}

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
	if ( ictx.lightShadowMapped && !din->ambientLight ) {
		da.textures[7] = ictx.shadowImage;	// depth map for u_shadowMap (unit 7)
	}
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

	// subview near-clip plane (mirror / camera views): the legacy path forces
	// the alpha test to fail behind the plane with an alpha-notch texgen trick
	// (RB_T_FillDepthBuffer). On core we clip geometrically with
	// gl_ClipDistance[0], written by zfill.vert. Only the depth fill needs it:
	// the interaction and opaque material passes are depth-EQUAL, so they
	// inherit the clip exactly like the legacy notch. The plane is transformed
	// into each surface's model space, matching R_GlobalPlaneToLocal() there.
	const bool useClipPlane = viewDef->numClipPlanes > 0;
	if ( useClipPlane ) {
		qglEnable( GL_CLIP_DISTANCE0 );
	}

	qglStencilFunc( GL_ALWAYS, 1, 255 );

	const viewEntity_t *currentSpace = NULL;
	float mvp[16];
	float localClipPlane[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

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
		// sky surfaces (portal sky, skybox) render at the far plane and must
		// NOT seal depth here — sealing is what makes forceOpaque portal-sky
		// occlude the geometry in front of it (see the sky shaders' z = w)
		texgen_t tg = shader->Texgen();
		if ( tg == TG_SCREEN || tg == TG_SCREEN2 || tg == TG_SKYBOX_CUBE || tg == TG_WOBBLESKY_CUBE ) {
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
			if ( useClipPlane ) {
				idPlane local;
				R_GlobalPlaneToLocal( surf->space->modelMatrix, viewDef->clipPlanes[0], local );
				localClipPlane[0] = local[0];
				localClipPlane[1] = local[1];
				localClipPlane[2] = local[2];
				localClipPlane[3] = local[3];
			}
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
				memcpy( parms.clipPlane, localClipPlane, sizeof( parms.clipPlane ) );
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
			memcpy( parms.clipPlane, localClipPlane, sizeof( parms.clipPlane ) );
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

	if ( useClipPlane ) {
		qglDisable( GL_CLIP_DISTANCE0 );
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
RB_RHI_ShadowCasterChain

Renders one interaction chain's depth from the light's point of view through the
shadow_sm program. The light-projection planes are transformed into each surface's
model space exactly like the interaction pass, so shadow_sm.vert projects to the
same cookie UV and writes the linear falloff as depth.
===================
*/
static void RB_RHI_ShadowCasterChain( rhi::RHI *r, const drawSurf_t *surf, rhi::ShaderHandle prog ) {
	for ( ; surf; surf = surf->nextOnLight ) {
		const srfTriangles_t *tri = surf->geo;
		if ( !tri || !tri->ambientCache || !tri->numIndexes ) {
			continue;
		}

		// only cast from surfaces the frontend's shadow rules allow (mirroring
		// Interaction.cpp): the material must cast, the entity must not be
		// noShadow, and per-view / per-light shadow suppression applies — the
		// latter is what keeps the player's own first-person weapon from casting
		// a shadow in the player's view (suppressShadowInViewID == this view),
		// while it still would in a mirror
		if ( surf->material && !surf->material->SurfaceCastsShadow() ) {
			continue;
		}
		const idRenderEntityLocal *edef = surf->space->entityDef;
		if ( edef ) {
			if ( edef->parms.noShadow ) {
				continue;
			}
			if ( !r_skipSuppress.GetBool() ) {
				if ( edef->parms.suppressShadowInViewID
				     && edef->parms.suppressShadowInViewID == backEnd.viewDef->renderView.viewID ) {
					continue;
				}
				if ( backEnd.vLight->lightDef
				     && edef->parms.suppressShadowInLightID
				     && edef->parms.suppressShadowInLightID == backEnd.vLight->lightDef->parms.lightId ) {
					continue;
				}
			}
		}

		rhi::RenderParams parms;
		memset( &parms, 0, sizeof( parms ) );
		idPlane lp;
		R_GlobalPlaneToLocal( surf->space->modelMatrix, backEnd.vLight->lightProject[0], lp );
		memcpy( parms.lightProjectionS, lp.ToFloatPtr(), 16 );
		R_GlobalPlaneToLocal( surf->space->modelMatrix, backEnd.vLight->lightProject[1], lp );
		memcpy( parms.lightProjectionT, lp.ToFloatPtr(), 16 );
		R_GlobalPlaneToLocal( surf->space->modelMatrix, backEnd.vLight->lightProject[2], lp );
		memcpy( parms.lightProjectionQ, lp.ToFloatPtr(), 16 );
		R_GlobalPlaneToLocal( surf->space->modelMatrix, backEnd.vLight->lightProject[3], lp );
		memcpy( parms.lightFalloffS, lp.ToFloatPtr(), 16 );

		rhi::BufferHandle vb, ib;
		int vertOfs, idxOfs;
		RB_RHI_StreamAmbient( r, tri, vb, vertOfs, ib, idxOfs );

		rhi::BufferHandle ub;
		int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

		// caster face selection (r_shadowMapCull): rendering only back faces
		// ("second-depth") keeps directly-lit front faces out of the map, which
		// is the standard cure for grazing-angle self-shadow acne. 0/1/2 map to
		// front / back / two-sided so the right winding can be picked live.
		int smCull = CT_BACK_SIDED;
		if ( r_shadowMapCull.GetInteger() == 0 ) {
			smCull = CT_FRONT_SIDED;
		} else if ( r_shadowMapCull.GetInteger() == 2 ) {
			smCull = CT_TWO_SIDED;
		}

		rhi::PipelineDesc pd;
		pd.stateBits = GLS_DEPTHFUNC_LESS;			// depth write on; color discarded (drawbuffer NONE)
		pd.shader = prog;
		pd.vertexLayout = rhi::VL_DRAWVERT;
		pd.cullType = smCull;
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

		backEnd.pc.c_shadowElements++;
	}
}

/*
===================
RB_RHI_ShadowMapPass

Renders the current light's occluder depth into the shared shadow-map target.
Returns false (→ caller uses the stencil path) if the target can't be created.
===================
*/
static bool RB_RHI_ShadowMapPass( rhi::RHI *r, viewLight_t *vLight, rhi::ShaderHandle prog ) {
	const int size = idMath::ClampInt( 256, 4096, r_shadowMapSize.GetInteger() );
	// (re)create the target on first use or a resolution change; a stale handle
	// after vid_restart returns image 0, which also triggers a rebuild
	if ( rhiShadowMap == 0 || rhiShadowMapSize != size || r->GetRenderTargetImage( rhiShadowMap ) == 0 ) {
		if ( rhiShadowMap ) {
			r->DestroyRenderTarget( rhiShadowMap );
			rhiShadowMap = 0;
		}
		rhiShadowMap = r->CreateRenderTarget( rhi::IF_DEPTH24, size, size );
		rhiShadowMapSize = size;
	}
	if ( rhiShadowMap == 0 ) {
		return false;
	}

	rhi::ClearArgs clear;
	memset( &clear, 0, sizeof( clear ) );
	clear.depth = true;
	r->BeginTargetPass( rhiShadowMap, &clear );

	// no polygon offset: shadow_sm writes gl_FragDepth, which polygon offset does
	// not affect — the depth-compare bias (r_shadowMapBias) does the acne control
	RB_RHI_ShadowCasterChain( r, vLight->globalInteractions, prog );
	RB_RHI_ShadowCasterChain( r, vLight->localInteractions, prog );

	r->EndPass();		// restores the backbuffer + the main view's viewport
	return true;
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
	rhi::ShaderHandle shadowMapProg = r->LoadShader( "shadow_sm" );

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
	// r_shadowMapDebug counts how each lit light was classified this view
	int dbgLit = 0, dbgProjected = 0, dbgShadowMapped = 0, dbgPoint = 0,
	    dbgParallel = 0, dbgNoShadow = 0, dbgNoLightDef = 0;
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
			dbgLit++;
			if ( !vLight->lightDef ) {
				dbgNoLightDef++;
			} else if ( vLight->lightDef->parms.parallel ) {
				dbgParallel++;
			} else if ( vLight->lightDef->parms.pointLight ) {
				dbgPoint++;
			} else {
				dbgProjected++;
				if ( !( vLight->globalShadows || vLight->localShadows ) ) {
					dbgNoShadow++;
				}
			}

			// DUDE Phase 3.5: choose the shadow technique for this light. Shadow
			// maps handle projected/spot lights that cast shadows; point and
			// parallel lights fall back to stencil (not yet implemented), which is
			// exactly the free per-light mixing. Reading lightDef->parms here is a
			// read-only frontend query (no SMP in this backend path).
			ictx.lightShadowMapped = false;
			ictx.shadowImage = 0;
			const bool castsShadows = ( vLight->globalShadows || vLight->localShadows );
			if ( r_shadowMapping.GetBool() && castsShadows && vLight->lightDef
			     && !vLight->lightDef->parms.pointLight && !vLight->lightDef->parms.parallel ) {
				if ( RB_RHI_ShadowMapPass( r, vLight, shadowMapProg ) ) {
					ictx.lightShadowMapped = true;
					ictx.shadowImage = r->GetRenderTargetImage( rhiShadowMap );
					dbgShadowMapped++;
				}
			}

			// scissor to this light (both paths); clear stencil only for the
			// stencil path — the shadow-map path never tests stencil
			if ( castsShadows ) {
				backEnd.currentScissor = vLight->scissorRect;
				if ( r_useScissor.GetBool() ) {
					r->SetScissor( viewDef->viewport.x1 + backEnd.currentScissor.x1,
					               viewDef->viewport.y1 + backEnd.currentScissor.y1,
					               backEnd.currentScissor.x2 + 1 - backEnd.currentScissor.x1,
					               backEnd.currentScissor.y2 + 1 - backEnd.currentScissor.y1 );
				}
				if ( !ictx.lightShadowMapped ) {
					qglClear( GL_STENCIL_BUFFER_BIT );
				}
			}
			if ( !castsShadows || ictx.lightShadowMapped ) {
				// stencil always passes; visibility comes from the map (if any)
				qglStencilFunc( GL_ALWAYS, 128, 255 );
			}

			ictx.depthFuncBits = GLS_DEPTHFUNC_EQUAL;
			if ( ictx.lightShadowMapped ) {
				RB_RHI_CreateDrawInteractions( vLight->localInteractions );
				RB_RHI_CreateDrawInteractions( vLight->globalInteractions );
			} else {
				RB_RHI_StencilShadowPass( r, viewDef, vLight->globalShadows, shadowProg );
				RB_RHI_CreateDrawInteractions( vLight->localInteractions );
				RB_RHI_StencilShadowPass( r, viewDef, vLight->localShadows, shadowProg );
				RB_RHI_CreateDrawInteractions( vLight->globalInteractions );
			}

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

	if ( r_shadowMapDebug.GetBool() ) {
		common->Printf( "shadowMap: %d lit lights | projected %d (shadow-mapped %d, no-shadow %d) | point %d | parallel %d | no-lightDef %d | r_shadowMapping %d\n",
		                dbgLit, dbgProjected, dbgShadowMapped, dbgNoShadow,
		                dbgPoint, dbgParallel, dbgNoLightDef, r_shadowMapping.GetInteger() );
	}

	// shader passes run with stencil satisfied everywhere
	qglStencilFunc( GL_ALWAYS, 128, 255 );
}

/*
=============================================================================

FOG AND BLEND LIGHTS

Fog and blend lights were the last fixed-function passes in the classic
renderer. They dual-texture straight to the framebuffer (projection/falloff
or fog ramp/enter) instead of interacting with the surface material, driven
by the fog/blendlight GLSL programs. This mirrors RB_STD_FogAllLights /
RB_FogPass / RB_BlendLight state bit for state bit; the texgen planes the old
path fed glTexGen now travel through RenderParams::texGen*.

=============================================================================
*/

/*
===================
RB_RHI_SetSurfScissor
===================
*/
static void RB_RHI_SetSurfScissor( rhi::RHI *r, const viewDef_t *viewDef, const drawSurf_t *surf ) {
	if ( r_useScissor.GetBool() && !backEnd.currentScissor.Equals( surf->scissorRect ) ) {
		backEnd.currentScissor = surf->scissorRect;
		r->SetScissor( viewDef->viewport.x1 + backEnd.currentScissor.x1,
		               viewDef->viewport.y1 + backEnd.currentScissor.y1,
		               backEnd.currentScissor.x2 + 1 - backEnd.currentScissor.x1,
		               backEnd.currentScissor.y2 + 1 - backEnd.currentScissor.y1 );
	}
}

/*
===================
RB_RHI_BlendLightChain

One surface chain for a blend-light stage. Mirrors RB_T_BlendLight: the
light's projection planes go to unit 0 (S/T/Q), the falloff plane to unit 1
(S; T is the constant 0.5 baked into blendlight.vert). backEnd.vLight must be
the current light.
===================
*/
static void RB_RHI_BlendLightChain( rhi::RHI *r, const viewDef_t *viewDef, const drawSurf_t *surf,
                                    rhi::ShaderHandle prog, int stateBits, const float color[4],
                                    idImage *projectionImage, idImage *falloffImage ) {
	for ( ; surf; surf = surf->nextOnLight ) {
		const srfTriangles_t *tri = surf->geo;
		if ( !tri->ambientCache ) {
			continue;
		}

		float mvp[16];
		RB_RHI_SpaceMvp( viewDef, surf->space, mvp );

		// project the light frustum planes into this surface's local space
		idPlane lightProject[4];
		for ( int i = 0; i < 4; i++ ) {
			R_GlobalPlaneToLocal( surf->space->modelMatrix, backEnd.vLight->lightProject[i], lightProject[i] );
		}

		rhi::RenderParams parms;
		memset( &parms, 0, sizeof( parms ) );
		memcpy( parms.mvpMatrix, mvp, sizeof( parms.mvpMatrix ) );
		memcpy( parms.color, color, sizeof( parms.color ) );
		memcpy( parms.texGen0S, lightProject[0].ToFloatPtr(), 16 );
		memcpy( parms.texGen0T, lightProject[1].ToFloatPtr(), 16 );
		memcpy( parms.texGen0Q, lightProject[2].ToFloatPtr(), 16 );
		memcpy( parms.texGen1S, lightProject[3].ToFloatPtr(), 16 );

		RB_RHI_SetSurfScissor( r, viewDef, surf );

		rhi::BufferHandle ub;
		int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

		rhi::BufferHandle vb, ib;
		int vertOfs, idxOfs;
		RB_RHI_StreamAmbient( r, tri, vb, vertOfs, ib, idxOfs );

		RB_RHI_BindUnit( 0, projectionImage );
		RB_RHI_BindUnit( 1, falloffImage );

		rhi::PipelineDesc pd;
		pd.stateBits = stateBits;
		pd.shader = prog;
		pd.vertexLayout = rhi::VL_DRAWVERT;
		pd.cullType = RB_RHI_CullFor( viewDef, CT_FRONT_SIDED );
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
	}
}

/*
===================
RB_RHI_BlendLight

Mirrors RB_BlendLight: for each live stage, dual-texture the projection and
falloff to the framebuffer over the light's interaction surfaces.
===================
*/
static void RB_RHI_BlendLight( rhi::RHI *r, const viewDef_t *viewDef, viewLight_t *vLight ) {
	if ( r_skipBlendLights.GetBool() || ( !vLight->globalInteractions && !vLight->localInteractions ) ) {
		return;
	}

	rhi::ShaderHandle prog = r->LoadShader( "blendlight" );
	const idMaterial *lightShader = vLight->lightShader;
	const float *regs = vLight->shaderRegisters;

	for ( int i = 0; i < lightShader->GetNumStages(); i++ ) {
		const shaderStage_t *stage = lightShader->GetStage( i );

		if ( !regs[stage->conditionRegister] ) {
			continue;
		}

		// texture-matrix (scrolling) blend-light projections are essentially
		// unused by stock content; drawn without the matrix until folded into
		// the projection planes (would need RB_BakeTextureMatrixIntoTexgen)
		if ( stage->texture.hasMatrix ) {
			RB_RHI_LogOnce( "blend light texture matrix" );
		}

		int stateBits = GLS_DEPTHMASK | stage->drawStateBits | GLS_DEPTHFUNC_EQUAL;

		// get the modulate values from the light, including alpha (unlike normal lights)
		float color[4];
		color[0] = regs[stage->color.registers[0]];
		color[1] = regs[stage->color.registers[1]];
		color[2] = regs[stage->color.registers[2]];
		color[3] = regs[stage->color.registers[3]];

		RB_RHI_BlendLightChain( r, viewDef, vLight->globalInteractions, prog, stateBits, color,
		                        stage->texture.image, vLight->falloffImage );
		RB_RHI_BlendLightChain( r, viewDef, vLight->localInteractions, prog, stateBits, color,
		                        stage->texture.image, vLight->falloffImage );
	}
}

/*
===================
RB_RHI_FogChain

One surface chain for a fog light. Mirrors RB_T_BasicFog: unit 0 is the fog
distance ramp (S = per-surface plane, T = constant 0.5), unit 1 the enter
fade (S = constant per viewer, T = per-surface top plane).
===================
*/
static void RB_RHI_FogChain( rhi::RHI *r, const viewDef_t *viewDef, const drawSurf_t *surf,
                             rhi::ShaderHandle prog, int stateBits, int cull, const float color[4],
                             const idPlane &fogPlane0, const idPlane &fogPlane2, float enterS ) {
	for ( ; surf; surf = surf->nextOnLight ) {
		const srfTriangles_t *tri = surf->geo;
		if ( !tri->ambientCache ) {
			continue;
		}

		float mvp[16];
		RB_RHI_SpaceMvp( viewDef, surf->space, mvp );

		rhi::RenderParams parms;
		memset( &parms, 0, sizeof( parms ) );
		memcpy( parms.mvpMatrix, mvp, sizeof( parms.mvpMatrix ) );
		memcpy( parms.color, color, sizeof( parms.color ) );

		idPlane local;
		// unit 0 S: fog distance ramp (+0.5 to center the 128px ramp texture)
		R_GlobalPlaneToLocal( surf->space->modelMatrix, fogPlane0, local );
		local[3] += 0.5f;
		memcpy( parms.texGen0S, local.ToFloatPtr(), 16 );
		// unit 0 T: constant 0.5
		parms.texGen0T[3] = 0.5f;
		// unit 1 T: enter fade, per-surface top plane (+FOG_ENTER)
		R_GlobalPlaneToLocal( surf->space->modelMatrix, fogPlane2, local );
		local[3] += FOG_ENTER;
		memcpy( parms.texGen1T, local.ToFloatPtr(), 16 );
		// unit 1 S: enter fade, constant per viewer
		parms.texGen1S[3] = enterS;

		RB_RHI_SetSurfScissor( r, viewDef, surf );

		rhi::BufferHandle ub;
		int uniOfs = r->AllocUniforms( &parms, sizeof( parms ), &ub );

		rhi::BufferHandle vb, ib;
		int vertOfs, idxOfs;
		RB_RHI_StreamAmbient( r, tri, vb, vertOfs, ib, idxOfs );

		RB_RHI_BindUnit( 0, globalImages->fogImage );
		RB_RHI_BindUnit( 1, globalImages->fogEnterImage );

		rhi::PipelineDesc pd;
		pd.stateBits = stateBits;
		pd.shader = prog;
		pd.vertexLayout = rhi::VL_DRAWVERT;
		pd.cullType = RB_RHI_CullFor( viewDef, cull );
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
	}
}

/*
===================
RB_RHI_FogLight

Mirrors RB_FogPass: dual-texture the fog ramp (unit 0) and enter fade
(unit 1) over the light's interaction surfaces, then the light frustum with
DEPTHFUNC_LESS so the far side of the fog volume is filled.
===================
*/
static void RB_RHI_FogLight( rhi::RHI *r, viewDef_t *viewDef, viewLight_t *vLight ) {
	const srfTriangles_t *frustumTris = vLight->frustumTris;
	// if we ran out of vertex cache memory, skip it
	if ( !frustumTris->ambientCache ) {
		return;
	}

	const idMaterial *lightShader = vLight->lightShader;
	const float *regs = vLight->shaderRegisters;
	// assume fog shaders have only a single stage
	const shaderStage_t *stage = lightShader->GetStage( 0 );

	float lightColor[4];
	lightColor[0] = regs[stage->color.registers[0]];
	lightColor[1] = regs[stage->color.registers[1]];
	lightColor[2] = regs[stage->color.registers[2]];
	lightColor[3] = regs[stage->color.registers[3]];

	// fog.frag builds alpha from the two ramps and multiplies rgb by u_color;
	// the old path set qglColor3fv so alpha stays 1 (density lives in
	// lightColor[3], and drives the ramp slope below — not u_color.a)
	float color[4] = { lightColor[0], lightColor[1], lightColor[2], 1.0f };

	// calculate the falloff planes
	float a;
	// if they left the default value on, set a fog distance of 500
	if ( lightColor[3] <= 1.0f ) {
		a = -0.5f / DEFAULT_FOG_DISTANCE;
	} else {
		// otherwise, distance = alpha color
		a = -0.5f / lightColor[3];
	}

	// unit-0 distance plane from the eye-space Z row of the world modelview
	const float *mv = viewDef->worldSpace.modelViewMatrix;
	idPlane fogPlane0( a * mv[2], a * mv[6], a * mv[10], a * mv[14] );

	// unit-1 enter fade: the "top" fade plane, and the eye's distance to it
	idPlane fogPlane2( 0.001f * vLight->fogPlane[0], 0.001f * vLight->fogPlane[1],
	                   0.001f * vLight->fogPlane[2], 0.001f * vLight->fogPlane[3] );
	float s = viewDef->renderView.vieworg * fogPlane2.Normal() + fogPlane2[3];
	float enterS = FOG_ENTER + s;

	rhi::ShaderHandle prog = r->LoadShader( "fog" );

	int stateEqual = GLS_DEPTHMASK | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_EQUAL;
	RB_RHI_FogChain( r, viewDef, vLight->globalInteractions, prog, stateEqual, CT_FRONT_SIDED, color, fogPlane0, fogPlane2, enterS );
	RB_RHI_FogChain( r, viewDef, vLight->localInteractions, prog, stateEqual, CT_FRONT_SIDED, color, fogPlane0, fogPlane2, enterS );

	// the light frustum bounding planes aren't in the depth buffer, so use
	// DEPTHFUNC_LESS instead of EQUAL and draw the volume's far (back) side
	drawSurf_t ds;
	memset( &ds, 0, sizeof( ds ) );
	ds.space = &viewDef->worldSpace;
	ds.geo = frustumTris;
	ds.scissorRect = viewDef->scissor;
	int stateLess = GLS_DEPTHMASK | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_LESS;
	RB_RHI_FogChain( r, viewDef, &ds, prog, stateLess, CT_BACK_SIDED, color, fogPlane0, fogPlane2, enterS );
}

/*
===================
RB_RHI_FogAllLights

Mirrors RB_STD_FogAllLights: after the interaction and ambient passes, add
every fog and blend light. Runs with the stencil test disabled (the classic
path guarantees no double-fogging by scissor alone).
===================
*/
void RB_RHI_FogAllLights( rhi::RHI *r, viewDef_t *viewDef ) {
	// note: r_skipFogLights skips the whole function (blend lights too) exactly
	// as legacy RB_STD_FogAllLights; r_skipBlendLights is checked per blend light
	if ( r_skipFogLights.GetBool() || r_showOverDraw.GetInteger() != 0 || viewDef->isXraySubview ) {
		return;
	}

	qglDisable( GL_STENCIL_TEST );

	for ( viewLight_t *vLight = viewDef->viewLights; vLight; vLight = vLight->next ) {
		backEnd.vLight = vLight;

		if ( vLight->lightShader->IsFogLight() ) {
			RB_RHI_FogLight( r, viewDef, vLight );
		} else if ( vLight->lightShader->IsBlendLight() ) {
			RB_RHI_BlendLight( r, viewDef, vLight );
		}
	}
	backEnd.vLight = NULL;

	qglEnable( GL_STENCIL_TEST );
}
