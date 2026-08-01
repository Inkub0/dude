/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include "sys/platform.h"
#include "idlib/math/Interpolate.h"
#include "framework/Game.h"
#include "renderer/VertexCache.h"
#include "renderer/RenderWorld_local.h"
#include "ui/Window.h"

#include "renderer/tr_local.h"

#include "Model_local.h"


static const float CHECK_BOUNDS_EPSILON = 1.0f;

/*
===========================================================================================

VERTEX CACHE GENERATORS

===========================================================================================
*/

/*
==================
R_CreateAmbientCache

Create it if needed
==================
*/
bool R_CreateAmbientCache( srfTriangles_t *tri, bool needsLighting ) {
	if ( tri->ambientCache ) {
		return true;
	}
	// we are going to use it for drawing, so make sure we have the tangents and normals
	if ( needsLighting && !tri->tangentsCalculated ) {
		R_DeriveTangents( tri );
	}

	vertexCache.Alloc( tri->verts, tri->numVerts * sizeof( tri->verts[0] ), &tri->ambientCache );
	if ( !tri->ambientCache ) {
		return false;
	}
	return true;
}

/*
==================
R_CreateLightingCache

Returns false if the cache couldn't be allocated, in which case the surface should be skipped.
==================
*/
bool R_CreateLightingCache( const idRenderEntityLocal *ent, const idRenderLightLocal *light, srfTriangles_t *tri ) {
	idVec3		localLightOrigin;

	// fogs and blends don't need light vectors
	if ( light->lightShader->IsFogLight() || light->lightShader->IsBlendLight() ) {
		return true;
	}

	// not needed if we have vertex programs
	if ( tr.backEndRendererHasVertexPrograms ) {
		return true;
	}

	R_GlobalPointToLocal( ent->modelMatrix, light->globalLightOrigin, localLightOrigin );

	int	size = tri->ambientSurface->numVerts * sizeof( lightingCache_t );
	lightingCache_t *cache = (lightingCache_t *)_alloca16( size );

#if 1

	SIMDProcessor->CreateTextureSpaceLightVectors( &cache[0].localLightVector, localLightOrigin,
												tri->ambientSurface->verts, tri->ambientSurface->numVerts, tri->indexes, tri->numIndexes );

#else

	bool *used = (bool *)_alloca16( tri->ambientSurface->numVerts * sizeof( used[0] ) );
	memset( used, 0, tri->ambientSurface->numVerts * sizeof( used[0] ) );

	// because the interaction may be a very small subset of the full surface,
	// it makes sense to only deal with the verts used
	for ( int j = 0; j < tri->numIndexes; j++ ) {
		int i = tri->indexes[j];
		if ( used[i] ) {
			continue;
		}
		used[i] = true;

		idVec3 lightDir;
		const idDrawVert *v;

		v = &tri->ambientSurface->verts[i];

		lightDir = localLightOrigin - v->xyz;

		cache[i].localLightVector[0] = lightDir * v->tangents[0];
		cache[i].localLightVector[1] = lightDir * v->tangents[1];
		cache[i].localLightVector[2] = lightDir * v->normal;
	}

#endif

	vertexCache.Alloc( cache, size, &tri->lightingCache );
	if ( !tri->lightingCache ) {
		return false;
	}
	return true;
}

/*
==================
R_CreatePrivateShadowCache

This is used only for a specific light
==================
*/
void R_CreatePrivateShadowCache( srfTriangles_t *tri ) {
	if ( !tri->shadowVertexes ) {
		return;
	}

	vertexCache.Alloc( tri->shadowVertexes, tri->numVerts * sizeof( *tri->shadowVertexes ), &tri->shadowCache );
}

/*
==================
R_CreateVertexProgramShadowCache

This is constant for any number of lights, the vertex program
takes care of projecting the verts to infinity.
==================
*/
void R_CreateVertexProgramShadowCache( srfTriangles_t *tri ) {
	if ( tri->verts == NULL ) {
		return;
	}

	// DG: use Mem_MallocA() instead of _alloca16() to avoid stack overflows with big models
	bool tempOnStack;
	shadowCache_t *temp = (shadowCache_t *)Mem_MallocA( tri->numVerts * 2 * sizeof( shadowCache_t ), tempOnStack );

#if 1

	SIMDProcessor->CreateVertexProgramShadowCache( &temp->xyz, tri->verts, tri->numVerts );

#else

	int numVerts = tri->numVerts;
	const idDrawVert *verts = tri->verts;
	for ( int i = 0; i < numVerts; i++ ) {
		const float *v = verts[i].xyz.ToFloatPtr();
		temp[i*2+0].xyz[0] = v[0];
		temp[i*2+1].xyz[0] = v[0];
		temp[i*2+0].xyz[1] = v[1];
		temp[i*2+1].xyz[1] = v[1];
		temp[i*2+0].xyz[2] = v[2];
		temp[i*2+1].xyz[2] = v[2];
		temp[i*2+0].xyz[3] = 1.0f;		// on the model surface
		temp[i*2+1].xyz[3] = 0.0f;		// will be projected to infinity
	}

#endif

	vertexCache.Alloc( temp, tri->numVerts * 2 * sizeof( shadowCache_t ), &tri->shadowCache );
	Mem_FreeA( temp, tempOnStack );
}

/*
==================
R_SkyboxTexGen
==================
*/
void R_SkyboxTexGen( drawSurf_t *surf, const idVec3 &viewOrg ) {
	int		i;
	idVec3	localViewOrigin;

	R_GlobalPointToLocal( surf->space->modelMatrix, viewOrg, localViewOrigin );

	int numVerts = surf->geo->numVerts;
	int size = numVerts * sizeof( idVec3 );
	idVec3 *texCoords = (idVec3 *) _alloca16( size );

	const idDrawVert *verts = surf->geo->verts;
	for ( i = 0; i < numVerts; i++ ) {
		texCoords[i][0] = verts[i].xyz[0] - localViewOrigin[0];
		texCoords[i][1] = verts[i].xyz[1] - localViewOrigin[1];
		texCoords[i][2] = verts[i].xyz[2] - localViewOrigin[2];
	}

	surf->dynamicTexCoords = vertexCache.AllocFrameTemp( texCoords, size );
}

/*
==================
R_WobbleskyTexGen
==================
*/
void R_WobbleskyTexGen( drawSurf_t *surf, const idVec3 &viewOrg ) {
	int		i;
	idVec3	localViewOrigin;

	const int *parms = surf->material->GetTexGenRegisters();

	float	wobbleDegrees = surf->shaderRegisters[ parms[0] ];
	float	wobbleSpeed = surf->shaderRegisters[ parms[1] ];
	float	rotateSpeed = surf->shaderRegisters[ parms[2] ];

	wobbleDegrees = wobbleDegrees * idMath::PI / 180;
	wobbleSpeed = wobbleSpeed * 2 * idMath::PI / 60;
	rotateSpeed = rotateSpeed * 2 * idMath::PI / 60;

	// very ad-hoc "wobble" transform
	float	transform[16];
	float	a = tr.viewDef->floatTime * wobbleSpeed;
	float	s = sin( a ) * sin( wobbleDegrees );
	float	c = cos( a ) * sin( wobbleDegrees );
	float	z = cos( wobbleDegrees );

	idVec3	axis[3];

	axis[2][0] = c;
	axis[2][1] = s;
	axis[2][2] = z;

	axis[1][0] = -sin( a * 2 ) * sin( wobbleDegrees );
	axis[1][2] = -s * sin( wobbleDegrees );
	axis[1][1] = sqrt( 1.0f - ( axis[1][0] * axis[1][0] + axis[1][2] * axis[1][2] ) );

	// make the second vector exactly perpendicular to the first
	axis[1] -= ( axis[2] * axis[1] ) * axis[2];
	axis[1].Normalize();

	// construct the third with a cross
	axis[0].Cross( axis[1], axis[2] );

	// add the rotate
	s = sin( rotateSpeed * tr.viewDef->floatTime );
	c = cos( rotateSpeed * tr.viewDef->floatTime );

	transform[0] = axis[0][0] * c + axis[1][0] * s;
	transform[4] = axis[0][1] * c + axis[1][1] * s;
	transform[8] = axis[0][2] * c + axis[1][2] * s;

	transform[1] = axis[1][0] * c - axis[0][0] * s;
	transform[5] = axis[1][1] * c - axis[0][1] * s;
	transform[9] = axis[1][2] * c - axis[0][2] * s;

	transform[2] = axis[2][0];
	transform[6] = axis[2][1];
	transform[10] = axis[2][2];

	transform[3] = transform[7] = transform[11] = 0.0f;
	transform[12] = transform[13] = transform[14] = 0.0f;

	R_GlobalPointToLocal( surf->space->modelMatrix, viewOrg, localViewOrigin );

	int numVerts = surf->geo->numVerts;
	int size = numVerts * sizeof( idVec3 );
	idVec3 *texCoords = (idVec3 *) _alloca16( size );

	const idDrawVert *verts = surf->geo->verts;
	for ( i = 0; i < numVerts; i++ ) {
		idVec3 v;

		v[0] = verts[i].xyz[0] - localViewOrigin[0];
		v[1] = verts[i].xyz[1] - localViewOrigin[1];
		v[2] = verts[i].xyz[2] - localViewOrigin[2];

		R_LocalPointToGlobal( transform, v, texCoords[i] );
	}

	surf->dynamicTexCoords = vertexCache.AllocFrameTemp( texCoords, size );
}

/*
=================
R_SpecularTexGen

Calculates the specular coordinates for cards without vertex programs.
=================
*/
static void R_SpecularTexGen( drawSurf_t *surf, const idVec3 &globalLightOrigin, const idVec3 &viewOrg ) {
	const srfTriangles_t *tri;
	idVec3	localLightOrigin;
	idVec3	localViewOrigin;

	R_GlobalPointToLocal( surf->space->modelMatrix, globalLightOrigin, localLightOrigin );
	R_GlobalPointToLocal( surf->space->modelMatrix, viewOrg, localViewOrigin );

	tri = surf->geo;

	// FIXME: change to 3 component?
	int	size = tri->numVerts * sizeof( idVec4 );
	idVec4 *texCoords = (idVec4 *) _alloca16( size );

#if 1

	SIMDProcessor->CreateSpecularTextureCoords( texCoords, localLightOrigin, localViewOrigin,
											tri->verts, tri->numVerts, tri->indexes, tri->numIndexes );

#else

	bool *used = (bool *)_alloca16( tri->numVerts * sizeof( used[0] ) );
	memset( used, 0, tri->numVerts * sizeof( used[0] ) );

	// because the interaction may be a very small subset of the full surface,
	// it makes sense to only deal with the verts used
	for ( int j = 0; j < tri->numIndexes; j++ ) {
		int i = tri->indexes[j];
		if ( used[i] ) {
			continue;
		}
		used[i] = true;

		float ilength;

		const idDrawVert *v = &tri->verts[i];

		idVec3 lightDir = localLightOrigin - v->xyz;
		idVec3 viewDir = localViewOrigin - v->xyz;

		ilength = idMath::RSqrt( lightDir * lightDir );
		lightDir[0] *= ilength;
		lightDir[1] *= ilength;
		lightDir[2] *= ilength;

		ilength = idMath::RSqrt( viewDir * viewDir );
		viewDir[0] *= ilength;
		viewDir[1] *= ilength;
		viewDir[2] *= ilength;

		lightDir += viewDir;

		texCoords[i][0] = lightDir * v->tangents[0];
		texCoords[i][1] = lightDir * v->tangents[1];
		texCoords[i][2] = lightDir * v->normal;
		texCoords[i][3] = 1;
	}

#endif

	surf->dynamicTexCoords = vertexCache.AllocFrameTemp( texCoords, size );
}


//=======================================================================================================

/*
=============
R_SetEntityDefViewEntity

If the entityDef isn't already on the viewEntity list, create
a viewEntity and add it to the list with an empty scissor rect.

This does not instantiate dynamic models for the entity yet.
=============
*/
viewEntity_t *R_SetEntityDefViewEntity( idRenderEntityLocal *def ) {
	viewEntity_t		*vModel;

	if ( def->viewCount == tr.viewCount ) {
		return def->viewEntity;
	}
	def->viewCount = tr.viewCount;

	// set the model and modelview matricies
	vModel = (viewEntity_t *)R_ClearedFrameAlloc( sizeof( *vModel ) );
	vModel->entityDef = def;

	// the scissorRect will be expanded as the model bounds is accepted into visible portal chains
	vModel->scissorRect.Clear();

	// copy the model and weapon depth hack for back-end use
	vModel->modelDepthHack = def->parms.modelDepthHack;
	vModel->weaponDepthHack = def->parms.weaponDepthHack;

	R_AxisToModelMatrix( def->parms.axis, def->parms.origin, vModel->modelMatrix );

	// we may not have a viewDef if we are just creating shadows at entity creation time
	if ( tr.viewDef ) {
		myGlMultMatrix( vModel->modelMatrix, tr.viewDef->worldSpace.modelViewMatrix, vModel->modelViewMatrix );

		vModel->next = tr.viewDef->viewEntitys;
		tr.viewDef->viewEntitys = vModel;
	}

	def->viewEntity = vModel;

	return vModel;
}

/*
====================
R_TestPointInViewLight
====================
*/
static const float INSIDE_LIGHT_FRUSTUM_SLOP = 32;
// this needs to be greater than the dist from origin to corner of near clip plane
static bool R_TestPointInViewLight( const idVec3 &org, const idRenderLightLocal *light ) {
	int		i;
	idVec3	local;

	for ( i = 0 ; i < 6 ; i++ ) {
		float d = light->frustum[i].Distance( org );
		if ( d > INSIDE_LIGHT_FRUSTUM_SLOP ) {
			return false;
		}
	}

	return true;
}

/*
===================
R_PointInFrustum

Assumes positive sides face outward
===================
*/
static bool R_PointInFrustum( idVec3 &p, idPlane *planes, int numPlanes ) {
	for ( int i = 0 ; i < numPlanes ; i++ ) {
		float d = planes[i].Distance( p );
		if ( d > 0 ) {
			return false;
		}
	}
	return true;
}

/*
=============
R_SetLightDefViewLight

If the lightDef isn't already on the viewLight list, create
a viewLight and add it to the list with an empty scissor rect.
=============
*/
viewLight_t *R_SetLightDefViewLight( idRenderLightLocal *light ) {
	viewLight_t *vLight;

	if ( light->viewCount == tr.viewCount ) {
		return light->viewLight;
	}
	light->viewCount = tr.viewCount;

	// add to the view light chain
	vLight = (viewLight_t *)R_ClearedFrameAlloc( sizeof( *vLight ) );
	vLight->lightDef = light;

	// the scissorRect will be expanded as the light bounds is accepted into visible portal chains
	vLight->scissorRect.Clear();

	// calculate the shadow cap optimization states
	vLight->viewInsideLight = R_TestPointInViewLight( tr.viewDef->renderView.vieworg, light );
	if ( !vLight->viewInsideLight ) {
		vLight->viewSeesShadowPlaneBits = 0;
		for ( int i = 0 ; i < light->numShadowFrustums ; i++ ) {
			float d = light->shadowFrustums[i].planes[5].Distance( tr.viewDef->renderView.vieworg );
			if ( d < INSIDE_LIGHT_FRUSTUM_SLOP ) {
				vLight->viewSeesShadowPlaneBits|= 1 << i;
			}
		}
	} else {
		// this should not be referenced in this case
		vLight->viewSeesShadowPlaneBits = 63;
	}

	// see if the light center is in view, which will allow us to cull invisible shadows
	vLight->viewSeesGlobalLightOrigin = R_PointInFrustum( light->globalLightOrigin, tr.viewDef->frustum, 4 );

	// copy data used by backend
	vLight->globalLightOrigin = light->globalLightOrigin;
	vLight->lightProject[0] = light->lightProject[0];
	vLight->lightProject[1] = light->lightProject[1];
	vLight->lightProject[2] = light->lightProject[2];
	vLight->lightProject[3] = light->lightProject[3];
	vLight->fogPlane = light->frustum[5];
	vLight->frustumTris = light->frustumTris;
	vLight->falloffImage = light->falloffImage;
	vLight->lightShader = light->lightShader;
	vLight->shaderRegisters = NULL;		// allocated and evaluated in R_AddLightSurfaces

	// link the view light
	vLight->next = tr.viewDef->viewLights;
	tr.viewDef->viewLights = vLight;

	light->viewLight = vLight;

	return vLight;
}

/*
=================
idRenderWorldLocal::CreateLightDefInteractions

When a lightDef is determined to effect the view (contact the frustum and non-0 light), it will check to
make sure that it has interactions for all the entityDefs that it might possibly contact.

This does not guarantee that all possible interactions for this light are generated, only that
the ones that may effect the current view are generated. so it does need to be called every view.

This does not cause entityDefs to create dynamic models, all work is done on the referenceBounds.

All entities that have non-empty interactions with viewLights will
have viewEntities made for them and be put on the viewEntity list,
even if their surfaces aren't visible, because they may need to cast shadows.

Interactions are usually removed when a entityDef or lightDef is modified, unless the change
is known to not effect them, so there is no danger of getting a stale interaction, we just need to
check that needed ones are created.

An interaction can be at several levels:

Don't interact (but share an area) (numSurfaces = 0)
Entity reference bounds touches light frustum, but surfaces haven't been generated (numSurfaces = -1)
Shadow surfaces have been generated, but light surfaces have not.  The shadow surface may still be empty due to bounds being conservative.
Both shadow and light surfaces have been generated.  Either or both surfaces may still be empty due to conservative bounds.

=================
*/
void idRenderWorldLocal::CreateLightDefInteractions( idRenderLightLocal *ldef ) {
	areaReference_t		*eref;
	areaReference_t		*lref;
	idRenderEntityLocal		*edef;
	portalArea_t	*area;
	idInteraction	*inter;

	for ( lref = ldef->references ; lref ; lref = lref->ownerNext ) {
		area = lref->area;

		// check all the models in this area
		for ( eref = area->entityRefs.areaNext ; eref != &area->entityRefs ; eref = eref->areaNext ) {
			edef = eref->entity;

			// if the entity doesn't have any light-interacting surfaces, we could skip this,
			// but we don't want to instantiate dynamic models yet, so we can't check that on
			// most things

			// if the entity isn't viewed
			if ( tr.viewDef && edef->viewCount != tr.viewCount ) {
				// if the light doesn't cast shadows, skip
				if ( !ldef->lightShader->LightCastsShadows() ) {
					continue;
				}
				// if we are suppressing its shadow in this view, skip
				if ( !r_skipSuppress.GetBool() ) {
					if ( edef->parms.suppressShadowInViewID && edef->parms.suppressShadowInViewID == tr.viewDef->renderView.viewID ) {
						continue;
					}
					if ( edef->parms.suppressShadowInLightID && edef->parms.suppressShadowInLightID == ldef->parms.lightId ) {
						continue;
					}
				}
			}

			// some big outdoor meshes are flagged to not create any dynamic interactions
			// when the level designer knows that nearby moving lights shouldn't actually hit them
			if ( edef->parms.noDynamicInteractions && edef->world->generateAllInteractionsCalled ) {
				continue;
			}

			// if any of the edef's interaction match this light, we don't
			// need to consider it.
			if ( r_useInteractionTable.GetBool() && this->interactionTable ) {
				// allocating these tables may take several megs on big maps, but it saves 3% to 5% of
				// the CPU time.  The table is updated at interaction::AllocAndLink() and interaction::UnlinkAndFree()
				int index = ldef->index * this->interactionTableWidth + edef->index;
				inter = this->interactionTable[ index ];
				if ( inter ) {
					// if this entity wasn't in view already, the scissor rect will be empty,
					// so it will only be used for shadow casting
					if ( !inter->IsEmpty() ) {
						R_SetEntityDefViewEntity( edef );
					}
					continue;
				}
			} else {
				// scan the doubly linked lists, which may have several dozen entries

				// we could check either model refs or light refs for matches, but it is
				// assumed that there will be less lights in an area than models
				// so the entity chains should be somewhat shorter (they tend to be fairly close).
				for ( inter = edef->firstInteraction; inter != NULL; inter = inter->entityNext ) {
					if ( inter->lightDef == ldef ) {
						break;
					}
				}

				// if we already have an interaction, we don't need to do anything
				if ( inter != NULL ) {
					// if this entity wasn't in view already, the scissor rect will be empty,
					// so it will only be used for shadow casting
					if ( !inter->IsEmpty() ) {
						R_SetEntityDefViewEntity( edef );
					}
					continue;
				}
			}

			//
			// create a new interaction, but don't do any work other than bbox to frustum culling
			//
			idInteraction *inter = idInteraction::AllocAndLink( edef, ldef );

			// do a check of the entity reference bounds against the light frustum,
			// trying to avoid creating a viewEntity if it hasn't been already
			float	modelMatrix[16];
			float	*m;

			if ( edef->viewCount == tr.viewCount ) {
				m = edef->viewEntity->modelMatrix;
			} else {
				R_AxisToModelMatrix( edef->parms.axis, edef->parms.origin, modelMatrix );
				m = modelMatrix;
			}

			if ( R_CullLocalBox( edef->referenceBounds, m, 6, ldef->frustum ) ) {
				inter->MakeEmpty();
				continue;
			}

			// we will do a more precise per-surface check when we are checking the entity

			// if this entity wasn't in view already, the scissor rect will be empty,
			// so it will only be used for shadow casting
			R_SetEntityDefViewEntity( edef );
		}
	}
}

//===============================================================================================================

/*
=================
R_LinkLightSurf
=================
*/
void R_LinkLightSurf( const drawSurf_t **link, const srfTriangles_t *tri, const viewEntity_t *space,
				   const idRenderLightLocal *light, const idMaterial *shader, const idScreenRect &scissor, bool viewInsideShadow ) {
	drawSurf_t		*drawSurf;

	if ( !space ) {
		space = &tr.viewDef->worldSpace;
	}

	drawSurf = (drawSurf_t *)R_FrameAlloc( sizeof( *drawSurf ) );

	drawSurf->geo = tri;
	drawSurf->space = space;
	drawSurf->material = shader;
	drawSurf->scissorRect = scissor;
	drawSurf->dsFlags = 0;
	drawSurf->particle_radius = 0.0f; // #3878

	if ( viewInsideShadow ) {
		drawSurf->dsFlags |= DSF_VIEW_INSIDE_SHADOW;
	}

	if ( !shader ) {
		// shadows won't have a shader
		drawSurf->shaderRegisters = NULL;
	} else {
		// process the shader expressions for conditionals / color / texcoords
		const float *constRegs = shader->ConstantRegisters();
		if ( constRegs ) {
			// this shader has only constants for parameters
			drawSurf->shaderRegisters = constRegs;
		} else {
			// FIXME: share with the ambient surface?
			float *regs = (float *)R_FrameAlloc( shader->GetNumRegisters() * sizeof( float ) );
			drawSurf->shaderRegisters = regs;
			shader->EvaluateRegisters( regs, space->entityDef->parms.shaderParms, tr.viewDef, space->entityDef->parms.referenceSound );
		}

		// calculate the specular coordinates if we aren't using vertex programs
		if ( !tr.backEndRendererHasVertexPrograms && !r_skipSpecular.GetBool() ) {
			R_SpecularTexGen( drawSurf, light->globalLightOrigin, tr.viewDef->renderView.vieworg );
			// if we failed to allocate space for the specular calculations, drop the surface
			if ( !drawSurf->dynamicTexCoords ) {
				return;
			}
		}
	}

	// actually link it in
	drawSurf->nextOnLight = *link;
	*link = drawSurf;
}

/*
======================
R_ClippedLightScissorRectangle
======================
*/
idScreenRect R_ClippedLightScissorRectangle( viewLight_t *vLight ) {
	int i, j;
	const idRenderLightLocal *light = vLight->lightDef;
	idScreenRect r;
	idFixedWinding w;

	r.Clear();

	for ( i = 0 ; i < 6 ; i++ ) {
		const idWinding *ow = light->frustumWindings[i];

		// projected lights may have one of the frustums degenerated
		if ( !ow ) {
			continue;
		}

		// the light frustum planes face out from the light,
		// so the planes that have the view origin on the negative
		// side will be the "back" faces of the light, which must have
		// some fragment inside the portalStack to be visible
		if ( light->frustum[i].Distance( tr.viewDef->renderView.vieworg ) >= 0 ) {
			continue;
		}

		w = *ow;

		// now check the winding against each of the frustum planes
		for ( j = 0; j < 5; j++ ) {
			if ( !w.ClipInPlace( -tr.viewDef->frustum[j] ) ) {
				break;
			}
		}

		// project these points to the screen and add to bounds
		for ( j = 0; j < w.GetNumPoints(); j++ ) {
			idPlane		eye, clip;
			idVec3		ndc;

			R_TransformModelToClip( w[j].ToVec3(), tr.viewDef->worldSpace.modelViewMatrix, tr.viewDef->projectionMatrix, eye, clip );

			if ( clip[3] <= 0.01f ) {
				clip[3] = 0.01f;
			}

			R_TransformClipToDevice( clip, tr.viewDef, ndc );

			float windowX = 0.5f * ( 1.0f + ndc[0] ) * ( tr.viewDef->viewport.x2 - tr.viewDef->viewport.x1 );
			float windowY = 0.5f * ( 1.0f + ndc[1] ) * ( tr.viewDef->viewport.y2 - tr.viewDef->viewport.y1 );

			if ( windowX > tr.viewDef->scissor.x2 ) {
				windowX = tr.viewDef->scissor.x2;
			} else if ( windowX < tr.viewDef->scissor.x1 ) {
				windowX = tr.viewDef->scissor.x1;
			}
			if ( windowY > tr.viewDef->scissor.y2 ) {
				windowY = tr.viewDef->scissor.y2;
			} else if ( windowY < tr.viewDef->scissor.y1 ) {
				windowY = tr.viewDef->scissor.y1;
			}

			r.AddPoint( windowX, windowY );
		}
	}

	// add the fudge boundary
	r.Expand();

	return r;
}

/*
==================
R_CalcLightScissorRectangle

The light screen bounds will be used to crop the scissor rect during
stencil clears and interaction drawing
==================
*/
int	c_clippedLight, c_unclippedLight;

idScreenRect	R_CalcLightScissorRectangle( viewLight_t *vLight ) {
	idScreenRect	r;
	srfTriangles_t *tri;
	idPlane			eye, clip;
	idVec3			ndc;

	if ( vLight->lightDef->parms.pointLight ) {
		idBounds bounds;
		idRenderLightLocal *lightDef = vLight->lightDef;
		tr.viewDef->viewFrustum.ProjectionBounds( idBox( lightDef->parms.origin, lightDef->parms.lightRadius, lightDef->parms.axis ), bounds );
		return R_ScreenRectFromViewFrustumBounds( bounds );
	}

	if ( r_useClippedLightScissors.GetInteger() == 2 ) {
		return R_ClippedLightScissorRectangle( vLight );
	}

	r.Clear();

	tri = vLight->lightDef->frustumTris;
	for ( int i = 0 ; i < tri->numVerts ; i++ ) {
		R_TransformModelToClip( tri->verts[i].xyz, tr.viewDef->worldSpace.modelViewMatrix,
			tr.viewDef->projectionMatrix, eye, clip );

		// if it is near clipped, clip the winding polygons to the view frustum
		if ( clip[3] <= 1 ) {
			c_clippedLight++;
			if ( r_useClippedLightScissors.GetInteger() ) {
				return R_ClippedLightScissorRectangle( vLight );
			} else {
				r.x1 = r.y1 = 0;
				r.x2 = ( tr.viewDef->viewport.x2 - tr.viewDef->viewport.x1 ) - 1;
				r.y2 = ( tr.viewDef->viewport.y2 - tr.viewDef->viewport.y1 ) - 1;
				return r;
			}
		}

		R_TransformClipToDevice( clip, tr.viewDef, ndc );

		float windowX = 0.5f * ( 1.0f + ndc[0] ) * ( tr.viewDef->viewport.x2 - tr.viewDef->viewport.x1 );
		float windowY = 0.5f * ( 1.0f + ndc[1] ) * ( tr.viewDef->viewport.y2 - tr.viewDef->viewport.y1 );

		if ( windowX > tr.viewDef->scissor.x2 ) {
			windowX = tr.viewDef->scissor.x2;
		} else if ( windowX < tr.viewDef->scissor.x1 ) {
			windowX = tr.viewDef->scissor.x1;
		}
		if ( windowY > tr.viewDef->scissor.y2 ) {
			windowY = tr.viewDef->scissor.y2;
		} else if ( windowY < tr.viewDef->scissor.y1 ) {
			windowY = tr.viewDef->scissor.y1;
		}

		r.AddPoint( windowX, windowY );
	}

	// add the fudge boundary
	r.Expand();

	c_unclippedLight++;

	return r;
}

/*
=================
R_AddLightSurfaces

Calc the light shader values, removing any light from the viewLight list
if it is determined to not have any visible effect due to being flashed off or turned off.

Adds entities to the viewEntity list if they are needed for shadow casting.

Add any precomputed shadow volumes.

Removes lights from the viewLights list if they are completely
turned off, or completely off screen.

Create any new interactions needed between the viewLights
and the viewEntitys due to game movement
=================
*/
void R_AddLightSurfaces( void ) {
	viewLight_t		*vLight;
	idRenderLightLocal *light;
	viewLight_t		**ptr;

	// go through each visible light, possibly removing some from the list
	ptr = &tr.viewDef->viewLights;
	while ( *ptr ) {
		vLight = *ptr;
		light = vLight->lightDef;

		const idMaterial	*lightShader = light->lightShader;
		if ( !lightShader ) {
			common->Error( "R_AddLightSurfaces: NULL lightShader" );
		}

		// see if we are suppressing the light in this view
		if ( !r_skipSuppress.GetBool() ) {
			if ( light->parms.suppressLightInViewID
			&& light->parms.suppressLightInViewID == tr.viewDef->renderView.viewID ) {
				*ptr = vLight->next;
				light->viewCount = -1;
				continue;
			}
			if ( light->parms.allowLightInViewID
			&& light->parms.allowLightInViewID != tr.viewDef->renderView.viewID ) {
				*ptr = vLight->next;
				light->viewCount = -1;
				continue;
			}
		}

		// evaluate the light shader registers
		float *lightRegs =(float *)R_FrameAlloc( lightShader->GetNumRegisters() * sizeof( float ) );
		vLight->shaderRegisters = lightRegs;
		lightShader->EvaluateRegisters( lightRegs, light->parms.shaderParms, tr.viewDef, light->parms.referenceSound );

		// if this is a purely additive light and no stage in the light shader evaluates
		// to a positive light value, we can completely skip the light
		if ( !lightShader->IsFogLight() && !lightShader->IsBlendLight() ) {
			int lightStageNum;
			for ( lightStageNum = 0 ; lightStageNum < lightShader->GetNumStages() ; lightStageNum++ ) {
				const shaderStage_t	*lightStage = lightShader->GetStage( lightStageNum );

				// ignore stages that fail the condition
				if ( !lightRegs[ lightStage->conditionRegister ] ) {
					continue;
				}

				const int *registers = lightStage->color.registers;

				// snap tiny values to zero to avoid lights showing up with the wrong color
				if ( lightRegs[ registers[0] ] < 0.001f ) {
					lightRegs[ registers[0] ] = 0.0f;
				}
				if ( lightRegs[ registers[1] ] < 0.001f ) {
					lightRegs[ registers[1] ] = 0.0f;
				}
				if ( lightRegs[ registers[2] ] < 0.001f ) {
					lightRegs[ registers[2] ] = 0.0f;
				}

				// FIXME:	when using the following values the light shows up bright red when using nvidia drivers/hardware
				//			this seems to have been fixed ?
				//lightRegs[ registers[0] ] = 1.5143074e-005f;
				//lightRegs[ registers[1] ] = 1.5483369e-005f;
				//lightRegs[ registers[2] ] = 1.7014690e-005f;

				if ( lightRegs[ registers[0] ] > 0.0f ||
						lightRegs[ registers[1] ] > 0.0f ||
							lightRegs[ registers[2] ] > 0.0f ) {
					break;
				}
			}
			if ( lightStageNum == lightShader->GetNumStages() ) {
				// we went through all the stages and didn't find one that adds anything
				// remove the light from the viewLights list, and change its frame marker
				// so interaction generation doesn't think the light is visible and
				// create a shadow for it
				*ptr = vLight->next;
				light->viewCount = -1;
				continue;
			}
		}

		if ( r_useLightScissors.GetBool() ) {
			// calculate the screen area covered by the light frustum
			// which will be used to crop the stencil cull
			idScreenRect scissorRect = R_CalcLightScissorRectangle( vLight );
			// intersect with the portal crossing scissor rectangle
			vLight->scissorRect.Intersect( scissorRect );

			if ( r_showLightScissors.GetBool() ) {
				R_ShowColoredScreenRect( vLight->scissorRect, light->index );
			}
		}

#if 0
		// this never happens, because CullLightByPortals() does a more precise job
		if ( vLight->scissorRect.IsEmpty() ) {
			// this light doesn't touch anything on screen, so remove it from the list
			*ptr = vLight->next;
			continue;
		}
#endif

		// this one stays on the list
		ptr = &vLight->next;

		// if we are doing a soft-shadow novelty test, regenerate the light with
		// a random offset every time
		if ( r_lightSourceRadius.GetFloat() != 0.0f ) {
			for ( int i = 0 ; i < 3 ; i++ ) {
				light->globalLightOrigin[i] += r_lightSourceRadius.GetFloat() * ( -1 + 2 * (rand()&0xfff)/(float)0xfff );
			}
		}

		// create interactions with all entities the light may touch, and add viewEntities
		// that may cast shadows, even if they aren't directly visible.  Any real work
		// will be deferred until we walk through the viewEntities
		tr.viewDef->renderWorld->CreateLightDefInteractions( light );
		tr.pc.c_viewLights++;

		// fog lights will need to draw the light frustum triangles, so make sure they
		// are in the vertex cache
		if ( lightShader->IsFogLight() ) {
			if ( !light->frustumTris->ambientCache ) {
				if ( !R_CreateAmbientCache( light->frustumTris, false ) ) {
					// skip if we are out of vertex memory
					continue;
				}
			}
			// touch the surface so it won't get purged
			vertexCache.Touch( light->frustumTris->ambientCache );
		}

		// add the prelight shadows for the static world geometry
		if ( light->parms.prelightModel && r_useOptimizedShadows.GetBool() ) {

			if ( !light->parms.prelightModel->NumSurfaces() ) {
				common->Error( "no surfs in prelight model '%s'", light->parms.prelightModel->Name() );
			}

			srfTriangles_t	*tri = light->parms.prelightModel->Surface( 0 )->geometry;
			if ( !tri->shadowVertexes ) {
				common->Error( "R_AddLightSurfaces: prelight model '%s' without shadowVertexes", light->parms.prelightModel->Name() );
			}

			// these shadows will all have valid bounds, and can be culled normally
			if ( r_useShadowCulling.GetBool() ) {
				if ( R_CullLocalBox( tri->bounds, tr.viewDef->worldSpace.modelMatrix, 5, tr.viewDef->frustum ) ) {
					continue;
				}
			}

			// if we have been purged, re-upload the shadowVertexes
			if ( !tri->shadowCache ) {
				R_CreatePrivateShadowCache( tri );
				if ( !tri->shadowCache ) {
					continue;
				}
			}

			// touch the shadow surface so it won't get purged
			vertexCache.Touch( tri->shadowCache );

			if ( !tri->indexCache && r_useIndexBuffers.GetBool() ) {
				vertexCache.Alloc( tri->indexes, tri->numIndexes * sizeof( tri->indexes[0] ), &tri->indexCache, true );
			}
			if ( tri->indexCache ) {
				vertexCache.Touch( tri->indexCache );
			}

			R_LinkLightSurf( &vLight->globalShadows, tri, NULL, light, NULL, vLight->scissorRect, true /* FIXME? */ );
		}
	}
}

//===============================================================================================================

/*
==================
R_IssueEntityDefCallback
==================
*/
bool R_IssueEntityDefCallback( idRenderEntityLocal *def ) {
	bool update;
	idBounds	oldBounds;
	const bool checkBounds = r_checkBounds.GetBool();

	if ( checkBounds ) {
		oldBounds = def->referenceBounds;
	}

	def->archived = false;		// will need to be written to the demo file
	tr.pc.c_entityDefCallbacks++;
	if ( tr.viewDef ) {
		update = def->parms.callback( &def->parms, &tr.viewDef->renderView );
	} else {
		update = def->parms.callback( &def->parms, NULL );
	}

	if ( !def->parms.hModel ) {
		common->Error( "R_IssueEntityDefCallback: dynamic entity callback didn't set model" );
		return false;
	}

	if ( checkBounds ) {
		if (	oldBounds[0][0] > def->referenceBounds[0][0] + CHECK_BOUNDS_EPSILON ||
				oldBounds[0][1] > def->referenceBounds[0][1] + CHECK_BOUNDS_EPSILON ||
				oldBounds[0][2] > def->referenceBounds[0][2] + CHECK_BOUNDS_EPSILON ||
				oldBounds[1][0] < def->referenceBounds[1][0] - CHECK_BOUNDS_EPSILON ||
				oldBounds[1][1] < def->referenceBounds[1][1] - CHECK_BOUNDS_EPSILON ||
				oldBounds[1][2] < def->referenceBounds[1][2] - CHECK_BOUNDS_EPSILON ) {
			common->Printf( "entity %i callback extended reference bounds\n", def->index );
		}
	}

	return update;
}

/*
===================
R_EntityDefDynamicModel

Issues a deferred entity callback if necessary.
If the model isn't dynamic, it returns the original.
Returns the cached dynamic model if present, otherwise creates
it and any necessary overlays
===================
*/
idRenderModel *R_EntityDefDynamicModel( idRenderEntityLocal *def ) {
	bool callbackUpdate;

	// allow deferred entities to construct themselves
	if ( def->parms.callback ) {
		callbackUpdate = R_IssueEntityDefCallback( def );
	} else {
		callbackUpdate = false;
	}

	idRenderModel *model = def->parms.hModel;

	if ( !model ) {
		common->Error( "R_EntityDefDynamicModel: NULL model" );
	}

	if ( model->IsDynamicModel() == DM_STATIC ) {
		def->dynamicModel = NULL;
		def->dynamicModelFrameCount = 0;
		return model;
	}

	// continously animating models (particle systems, etc) will have their snapshot updated every single view
	if ( callbackUpdate || ( model->IsDynamicModel() == DM_CONTINUOUS && def->dynamicModelFrameCount != tr.frameCount ) ) {
		R_ClearEntityDefDynamicModel( def );
	}

	// if we don't have a snapshot of the dynamic model, generate it now
	if ( !def->dynamicModel ) {

		// instantiate the snapshot of the dynamic model, possibly reusing memory from the cached snapshot
		def->cachedDynamicModel = model->InstantiateDynamicModel( &def->parms, tr.viewDef, def->cachedDynamicModel );

		if ( def->cachedDynamicModel ) {

			// add any overlays to the snapshot of the dynamic model
			if ( def->overlay && !r_skipOverlays.GetBool() ) {
				def->overlay->AddOverlaySurfacesToModel( def->cachedDynamicModel );
			} else {
				idRenderModelOverlay::RemoveOverlaySurfacesFromModel( def->cachedDynamicModel );
			}

			if ( r_checkBounds.GetBool() ) {
				idBounds b = def->cachedDynamicModel->Bounds();
				if (	b[0][0] < def->referenceBounds[0][0] - CHECK_BOUNDS_EPSILON ||
						b[0][1] < def->referenceBounds[0][1] - CHECK_BOUNDS_EPSILON ||
						b[0][2] < def->referenceBounds[0][2] - CHECK_BOUNDS_EPSILON ||
						b[1][0] > def->referenceBounds[1][0] + CHECK_BOUNDS_EPSILON ||
						b[1][1] > def->referenceBounds[1][1] + CHECK_BOUNDS_EPSILON ||
						b[1][2] > def->referenceBounds[1][2] + CHECK_BOUNDS_EPSILON ) {
					common->Printf( "entity %i dynamic model exceeded reference bounds\n", def->index );
				}
			}
		}

		def->dynamicModel = def->cachedDynamicModel;
		def->dynamicModelFrameCount = tr.frameCount;
	}

	// set model depth hack value
	if ( def->dynamicModel && model->DepthHack() != 0.0f && tr.viewDef ) {
		idPlane eye, clip;
		idVec3 ndc;
		R_TransformModelToClip( def->parms.origin, tr.viewDef->worldSpace.modelViewMatrix, tr.viewDef->projectionMatrix, eye, clip );
		R_TransformClipToDevice( clip, tr.viewDef, ndc );
		def->parms.modelDepthHack = model->DepthHack() * ( 1.0f - ndc.z );
	}

	// FIXME: if any of the surfaces have deforms, create a frame-temporary model with references to the
	// undeformed surfaces.  This would allow deforms to be light interacting.

	return def->dynamicModel;
}

/*
==========================================================================================

EMISSIVE FILL LIGHTS (DUDE enhancement)

Interactive GUI screens (monitors, keypads, wall panels) are drawn purely as self-lit
ambient surfaces: they glow, but in Doom 3's unified lighting model they cast no light
on anything around them, so they read as bright decals pasted onto an unlit wall. To
ground them we spawn a small, shadowless point light per visible GUI surface, sitting
just in front of the screen. These are real world light defs, so all the normal
interaction / backend machinery (including the enhancement backends' shadow maps, which
we disable here) lights the neighbours for free.

Cheapest-viable version: constant tint, size/reach derived from the surface, lights
cached per (entity, material) and reused across frames, garbage-collected a couple of
seconds after a screen stops being drawn. Tinting the light from the screen's actual
content colour is the obvious next improvement.

Only real player views (viewID > 0) manage the set; mirrors/xray/lightgem subviews see
the same world lights for free. Enhancement-only and off by default.
==========================================================================================
*/

struct emissiveReq_t {
	int			entityIndex;	// owning entityDef index, -1 for world geometry
	const void *surfKey;		// material ptr — disambiguates several screens on one entity
	idVec3		center;			// screen centre, world space
	idVec3		normal;			// screen outward normal, world space (unit)
	float		radius;			// reach (world units)
	float		surfRadius;		// screen half-size (for the point-light forward offset)
	idVec3		color;			// final light colour (hue * scale, desaturated)
	bool		freshTint;		// true = colour sampled from the drawn screen this frame; false = culled-surface fallback
	bool		pointLight;		// item glow: plain faint point light instead of the screen cone
};

struct emissiveLight_t {
	int			entityIndex;
	const void *surfKey;
	qhandle_t	handle;
	int			lastSeen;		// renderView.time (ms) the screen was last drawn
	idVec3		center;
	idVec3		normal;
	float		radius;
	idVec3		color;
};

static const int				EMISSIVE_LIGHT_TIMEOUT_MS = 2000;
static idList<emissiveReq_t>	r_emissiveReqs;
static idList<emissiveLight_t>	r_emissiveLightList;
static idRenderWorldLocal *		r_emissiveWorld = NULL;
static int						r_emissiveWorldGen = -1;	// world->defsGeneration our handles belong to

// scratch for the per-view budget selection (r_emissiveLightLimit)
struct emReqScore_t {
	int		idx;
	float	score;	// bigger + nearer = more important
};
static int R_CmpEmReqScore( const void *pa, const void *pb ) {
	const float a = ( (const emReqScore_t *)pa )->score;
	const float b = ( (const emReqScore_t *)pb )->score;
	if ( a < b ) return 1;		// descending: higher score first
	if ( a > b ) return -1;
	return 0;
}

/*
=================
R_MaterialHasCinematic

DUDE: true if the material draws a video/cinematic (a "screen" with no gui). Cheap —
walks the (usually one or two) stages. Only called when emissive lights are enabled.
=================
*/
static bool R_MaterialHasCinematic( const idMaterial *shader ) {
	if ( shader == NULL ) {
		return false;
	}
	const int n = shader->GetNumStages();
	for ( int i = 0; i < n; i++ ) {
		if ( shader->GetStage( i )->texture.cinematic != NULL ) {
			return true;
		}
	}
	return false;
}

/*
=================
R_QueueEmissiveLight

Called from R_AddDrawSurf for every visible "screen" surface (gui or cinematic). Derives
a world-space fill-light request from the surface geometry, tinted by the caller-supplied
content colour, and queues it; the actual light defs are reconciled once per view in
R_UpdateEmissiveLights.
=================
*/
static void R_QueueEmissiveLight( const srfTriangles_t *tri, const viewEntity_t *space, const idMaterial *shader, const idVec3 &tint, bool freshTint = true ) {
	if ( !tri || !tri->verts || tri->numIndexes < 3 || !space ) {
		return;
	}

	// NPCs and other skeletal characters hold screens (tablets, PDAs) right against their own
	// body, where a point-blank fill light blasts their mesh — and a hand-held device isn't an
	// environment light anyway. Skip emissive lights on animated/skeletal models; world and
	// static-model screens have no joints, so they're unaffected.
	if ( space->entityDef != NULL && space->entityDef->parms.numJoints > 0 ) {
		return;
	}

	// local-space centre, size and face normal (entity matrices are rigid, so world size == local size)
	const idVec3 localCenter = ( tri->bounds[0] + tri->bounds[1] ) * 0.5f;
	const float  surfRadius  = ( tri->bounds[1] - tri->bounds[0] ).Length() * 0.5f;
	if ( surfRadius < 1.0f ) {
		return;		// degenerate / tiny
	}

	// use the authored surface normal (points out of the visible/emitting face); the
	// winding-derived face normal can point into the wall, which fires the cone backwards
	idVec3 localNormal = tri->verts[tri->indexes[0]].normal
	                   + tri->verts[tri->indexes[1]].normal
	                   + tri->verts[tri->indexes[2]].normal;
	if ( localNormal.Normalize() == 0.0f ) {
		const idVec3 &a = tri->verts[tri->indexes[0]].xyz;
		const idVec3 &b = tri->verts[tri->indexes[1]].xyz;
		const idVec3 &c = tri->verts[tri->indexes[2]].xyz;
		localNormal = ( b - a ).Cross( c - a );
		if ( localNormal.Normalize() == 0.0f ) {
			localNormal.Set( 0.0f, 0.0f, 1.0f );
		}
	}

	idVec3 worldCenter, worldNormal;
	R_LocalPointToGlobal( space->modelMatrix, localCenter, worldCenter );
	R_LocalVectorToGlobal( space->modelMatrix, localNormal, worldNormal );
	worldNormal.Normalize();

	// safety net: when we're actually looking at the screen (fresh sample), orient the cone toward
	// the viewer so it can never fire into the mount (the authored normal is occasionally inverted on
	// decal-style screens). Skip this for culled/fallback discovery — there the viewer may be *behind*
	// the screen, and flipping toward them would light its back side; trust the authored outward normal.
	if ( freshTint && tr.viewDef != NULL ) {
		const idVec3 toView = tr.viewDef->renderView.vieworg - worldCenter;
		if ( ( worldNormal * toView ) < 0.0f ) {
			worldNormal = -worldNormal;
		}
	}

	emissiveReq_t &req = r_emissiveReqs.Alloc();
	req.entityIndex = space->entityDef ? space->entityDef->index : -1;
	req.surfKey     = shader;
	req.center      = worldCenter;
	req.normal      = worldNormal;
	req.surfRadius  = surfRadius;
	req.freshTint   = freshTint;
	req.pointLight  = false;

	// Reach scales with screen size, but *sub-linearly* so a big hanging sign doesn't cast light
	// across the whole room. Small/medium screens stay exactly linear (reach == size * multiplier);
	// past a knee the growth rolls off and saturates toward r_emissiveLightMaxReach. The knee tracks
	// the cap (0.45x) so the whole size-response scales when the cap is changed — raise the cap toward
	// 512 for near-linear scaling, lower it to rein big signs in harder.
	const float lin      = surfRadius * r_emissiveLightRadius.GetFloat();	// the old linear reach
	const float maxReach = r_emissiveLightMaxReach.GetFloat();				// asymptote for the biggest screens
	const float knee     = maxReach * 0.45f;								// linear below this, compress above
	float radius;
	if ( lin <= knee ) {
		radius = lin;
	} else {
		const float span   = maxReach - knee;			// > 0 (knee is 0.45x the cap)
		const float excess = lin - knee;
		radius = knee + span * ( excess / ( excess + span ) );	// reciprocal soft-clip: -> maxReach as size grows
	}
	if ( radius < 24.0f )  radius = 24.0f;				// floor so tiny screens still reach the near weapon
	req.radius = radius;

	// normalise to a pure hue (brightness comes from r_emissiveLightScale), then desaturate
	// toward white by (1 - saturation) so the bleed reads as a tint, not a coloured spotlight.
	// This also gives the raw cinematic tint the same hue/brightness behaviour as the gui path.
	idVec3 hue = tint;
	float m = hue.x;
	if ( hue.y > m ) m = hue.y;
	if ( hue.z > m ) m = hue.z;
	if ( m > 0.001f ) {
		hue /= m;
	} else {
		hue.Set( 1.0f, 1.0f, 1.0f );
	}
	const float sat = r_emissiveLightSaturation.GetFloat();
	hue.x = 1.0f + ( hue.x - 1.0f ) * sat;
	hue.y = 1.0f + ( hue.y - 1.0f ) * sat;
	hue.z = 1.0f + ( hue.z - 1.0f ) * sat;

	req.color = hue * r_emissiveLightScale.GetFloat();
}

/*
=================
R_QueueEmissiveScreenFallback

DUDE: discovery path for a screen whose surface is culled this frame (off-frustum, behind us, or
back-facing). Detects a gui/cinematic "screen" the same way R_AddDrawSurf does, but WITHOUT
re-rendering the gui, and queues a fill-light request marked non-fresh with a best-effort tint. This
is what lets a screen light its surroundings before we've looked straight at it, and keep doing so
after we look away. R_UpdateEmissiveLights preserves an already-sampled colour for these (so a
glimpsed screen never flashes to the white fallback), and R_QueueEmissiveLight skips the viewer
normal-flip for them (so a screen discovered from behind still fires out of its front face).
=================
*/
static void R_QueueEmissiveScreenFallback( const srfTriangles_t *tri, const viewEntity_t *space, const idMaterial *shader ) {
	idUserInterface *gui = NULL;
	if ( space->entityDef == NULL ) {
		gui = shader->GlobalGui();
	} else {
		const int guiNum = shader->GetEntityGui() - 1;
		if ( guiNum >= 0 && guiNum < MAX_RENDERENTITY_GUI ) {
			gui = space->entityDef->parms.gui[ guiNum ];
		}
		if ( gui == NULL ) {
			gui = shader->GlobalGui();
		}
	}

	idVec3 tint( 1.0f, 1.0f, 1.0f );
	if ( gui == NULL ) {
		if ( !R_MaterialHasCinematic( shader ) ) {
			return;		// not a screen — nothing to light
		}
		if ( globalImages->cinematicImage != NULL ) {
			tint.Set( globalImages->cinematicImage->averageColor[0],
					  globalImages->cinematicImage->averageColor[1],
					  globalImages->cinematicImage->averageColor[2] );
		}
	}

	R_QueueEmissiveLight( tri, space, shader, tint, false /* fallback: not a fresh sample */ );
}

/*
=================
R_ItemGlowColor

DUDE: derives the glow colour of a self-lit pickup-item surface. Item models keep their
glowing bits (medkit cross light, armor shard fx, powerup blite spheres...) on separate
surfaces whose materials live under models/items/ and consist of additive ambient stages,
so the detection is: material name prefix + at least one enabled add-blend ambient stage.
The colour is the stage colour (register-evaluated, so pulsing rgb tables track) times the
glow texture's mean colour, summed over the glow stages. Returns false for non-glow
materials (plain diffuse item shells included).
=================
*/
static bool R_ItemGlowColor( const idMaterial *shader, const renderEntity_t *parms, idVec3 &out ) {
	if ( shader == NULL || idStr::Icmpn( shader->GetName(), "models/items/", 13 ) != 0 ) {
		return false;
	}

	static float scratchRegs[MAX_EXPRESSION_REGISTERS];	// keep off the stack (cf. R_AddDrawSurf)
	const float *regs = shader->ConstantRegisters();

	out.Zero();
	bool found = false;
	const int n = shader->GetNumStages();
	for ( int i = 0; i < n; i++ ) {
		const shaderStage_t *stage = shader->GetStage( i );
		if ( stage->lighting != SL_AMBIENT ) {
			continue;
		}
		const int blend = stage->drawStateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS );
		if ( blend != ( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE ) ) {
			continue;		// only "blend add" stages emit
		}
		if ( regs == NULL ) {
			// non-constant material (rgb tables, translates): evaluate once with the entity's parms
			shader->EvaluateRegisters( scratchRegs, parms->shaderParms, tr.viewDef, parms->referenceSound );
			regs = scratchRegs;
		}
		if ( !regs[ stage->conditionRegister ] ) {
			continue;
		}
		idVec3 c( regs[ stage->color.registers[0] ],
				  regs[ stage->color.registers[1] ],
				  regs[ stage->color.registers[2] ] );
		if ( stage->texture.image != NULL ) {
			c.x *= stage->texture.image->averageColor[0];
			c.y *= stage->texture.image->averageColor[1];
			c.z *= stage->texture.image->averageColor[2];
		}
		out += c;
		found = true;
	}

	return found && out.LengthSqr() > 1e-6f;
}

// r_itemGlow == 1 maps to this light colour scale — "faint": in a lit area the
// contribution disappears into the existing light, only near-total darkness shows it
static const float ITEM_GLOW_MAX_SCALE = 0.15f;
static const float ITEM_GLOW_RADIUS    = 30.0f;		// world units, per-axis point light radius (user calibration)

/*
=================
R_QueueItemGlowLight

DUDE: queues a faint point light centred on a self-lit item surface (armor, medkits,
ammo lights, powerups) so the glowing bit actually gives off light in the dark. Reuses
the emissive fill-light reconcile/reap machinery; keyed by (entity, material) like the
screens. Colour is always freshly derivable (no gui render needed), so culled surfaces
queue through the exact same path and keep glowing when we look away.
=================
*/
static void R_QueueItemGlowLight( const srfTriangles_t *tri, const viewEntity_t *space, const idMaterial *shader ) {
	if ( !tri || !tri->verts || tri->numIndexes < 3 || !space || !space->entityDef ) {
		return;
	}
	const renderEntity_t &parms = space->entityDef->parms;
	// skip carried copies: the view weapon (flashlight) and items held by skeletal
	// characters (lanterns, belt flashlights) — a point light inside a mesh only blasts it
	if ( parms.numJoints > 0 || parms.weaponDepthHack || parms.modelDepthHack != 0.0f ) {
		return;
	}

	idVec3 tint;
	if ( !R_ItemGlowColor( shader, &parms, tint ) ) {
		return;
	}

	const idVec3 localCenter = ( tri->bounds[0] + tri->bounds[1] ) * 0.5f;
	idVec3 worldCenter;
	R_LocalPointToGlobal( space->modelMatrix, localCenter, worldCenter );

	// normalise to a pure hue: brightness comes from the slider, the texture only sets colour
	idVec3 hue = tint;
	float m = hue.x;
	if ( hue.y > m ) m = hue.y;
	if ( hue.z > m ) m = hue.z;
	if ( m > 0.001f ) {
		hue /= m;
	} else {
		hue.Set( 1.0f, 1.0f, 1.0f );
	}

	emissiveReq_t &req = r_emissiveReqs.Alloc();
	req.entityIndex = space->entityDef->index;
	req.surfKey     = shader;
	req.center      = worldCenter;
	req.normal.Set( 0.0f, 0.0f, 1.0f );	// unused for point lights
	req.surfRadius  = 0.0f;
	req.radius      = ITEM_GLOW_RADIUS;
	req.freshTint   = true;
	req.pointLight  = true;
	req.color       = hue * ( r_itemGlow.GetFloat() * ITEM_GLOW_MAX_SCALE );
}

/*
=================
R_FindEmissiveLight
=================
*/
static emissiveLight_t *R_FindEmissiveLight( int entityIndex, const void *surfKey ) {
	for ( int i = 0; i < r_emissiveLightList.Num(); i++ ) {
		if ( r_emissiveLightList[i].entityIndex == entityIndex && r_emissiveLightList[i].surfKey == surfKey ) {
			return &r_emissiveLightList[i];
		}
	}
	return NULL;
}

/*
=================
R_FreeAllEmissiveLights
=================
*/
static void R_FreeAllEmissiveLights( idRenderWorldLocal *world ) {
	if ( world ) {
		for ( int i = 0; i < r_emissiveLightList.Num(); i++ ) {
			const qhandle_t h = r_emissiveLightList[i].handle;
			// only free slots that still hold a def — a handle can go stale if the world's
			// defs were torn down behind our back (same-map reload)
			if ( h >= 0 && h < world->lightDefs.Num() && world->lightDefs[h] != NULL ) {
				world->FreeLightDef( h );
			}
		}
	}
	r_emissiveLightList.Clear();
}

/*
=================
R_BuildEmissiveRenderLight

DUDE: fills a renderLight_t for one screen from its request, honouring the projected/
specular/shadow style cvars. Projected (default) is a forward-facing cone whose apex sits
a little behind the screen: it lights the surrounding wall and the space in front, but —
unlike a point light's sphere — spills nothing through to the far side of the mount, which
is what fixes the leak on recessed screens (health stations etc.).
=================
*/
static void R_BuildEmissiveRenderLight( const emissiveReq_t &req, renderLight_t &rl ) {
	memset( &rl, 0, sizeof( rl ) );
	rl.axis = mat3_identity;
	rl.noShadows  = true;		// fill light; shadows here would only fight the real lights
	rl.noSpecular = !r_emissiveLightSpecular.GetBool();
	rl.shaderParms[0] = req.color[0];
	rl.shaderParms[1] = req.color[1];
	rl.shaderParms[2] = req.color[2];
	rl.shaderParms[3] = 1.0f;

	// DUDE item glow: a plain faint point light centred on the item's glowing surface
	if ( req.pointLight ) {
		rl.pointLight = true;
		rl.origin = req.center;
		rl.lightRadius.Set( req.radius, req.radius, req.radius );
		return;
	}

	const float R = req.radius;

	// Forward-facing projected cone. The apex sits a little behind the screen so the cone already
	// has width at the screen plane and lights the wall *around* a flush-mounted panel. The near
	// clip plane, however, is pinned to just behind the screen plane: close enough to catch a wall
	// the screen is mounted flush against, but not the deep back face of a free-standing monitor or
	// the underside of a table/panel. The old tuning slid the near plane 7-13 units behind the
	// screen — that is what lit thin free-standing screens on their back side. Since a projected
	// light's near clip == its falloff-ramp start, the near plane can't be pulled back for a softer
	// ramp without re-opening that leak, so the fade knob now shapes the *far* plane instead.
	const float back     = R * 0.6f;					// apex pulled this far behind the screen (cone spread)
	const float nearBack = 4.0f;						// near clip only this far behind the screen plane
	const float coneHalf = R * r_emissiveLightSpread.GetFloat();	// far-cap half-extent; wide = soft hemisphere-like glow

	idVec3 rightUnit, upUnit;
	req.normal.NormalVectors( rightUnit, upUnit );

	// fade-off: slide the *far* falloff/clip plane. 0.5 reproduces the shipped reach (~R); lower is
	// a tighter, brighter pool that drops off sharply, higher a gentle glow that reaches further.
	const float fade  = r_emissiveLightFalloff.GetFloat();
	const float reach = R * ( 0.5f + fade );			// 0.5R (sharp) .. 1.5R (gentle); 0.5 -> R

	rl.pointLight = false;
	rl.origin = req.center - req.normal * back;			// apex, behind the screen
	rl.target = req.normal * ( back + R );				// far-cap centre (rel. origin) -> sets the cone half-angle
	rl.right  = rightUnit * coneHalf;					// far-cap half-width
	rl.up     = upUnit * coneHalf;						// far-cap half-height
	rl.start  = req.normal * ( back - nearBack );		// near clip / falloff start: just behind the screen plane
	rl.end    = req.normal * ( back + reach );			// far clip / falloff end
}

/*
=================
R_UpdateEmissiveLights

Reconciles this view's queued GUI-screen light requests against the persistent set of
fill lights: creates new ones, follows moved screens, and reaps lights whose screen
hasn't been drawn for a couple of seconds. Called once at the end of R_AddModelSurfaces.
=================
*/
static void R_UpdateEmissiveLights( void ) {
	idRenderWorldLocal *world = tr.viewDef ? tr.viewDef->renderWorld : NULL;

	// only real player views own the set; subviews/lightgem see the same world lights for free
	if ( !tr.viewDef || tr.viewDef->renderView.viewID <= 0 ) {
		r_emissiveReqs.SetNum( 0, false );
		return;
	}

	// a new map means our cached handles belong to defs that have already been torn down — drop
	// them without touching the (freed) light defs. The world *object* is reused across map loads
	// (InitFromMap frees the defs in place), so the pointer alone can't detect a reload: compare
	// the defs generation too, or the stale handles would double-free ~2s after every load.
	if ( world != r_emissiveWorld || ( world && world->defsGeneration != r_emissiveWorldGen ) ) {
		r_emissiveLightList.Clear();
		r_emissiveWorld = world;
		r_emissiveWorldGen = world ? world->defsGeneration : -1;
	}

	const bool enabled = ( r_emissiveSurfaces.GetBool() || r_itemGlow.GetFloat() > 0.0f )
	                     && R_BackendSupportsEnhancements();

	if ( !enabled || !world ) {
		if ( r_emissiveLightList.Num() > 0 ) {
			R_FreeAllEmissiveLights( world );
		}
		r_emissiveReqs.SetNum( 0, false );
		return;
	}

	// These knobs are baked into each light at creation, so changing them won't propagate
	// through the move/colour comparison below (fade-off isn't compared at all, and small
	// saturation nudges fall under the colour threshold) — force a full rebuild when any of
	// them changes. The signature only shifts while a slider is dragged, so steady-state cost
	// is zero; each value gets a well-separated weight so a single-knob change is always seen.
	static float lastStyleSig = -1.0f;
	const float styleSig = ( r_emissiveLightSpecular.GetBool() ? 2.0f : 0.0f )
	                     + r_emissiveLightSpread.GetFloat()     * 8.0f
	                     + r_emissiveLightFalloff.GetFloat()    * 16.0f
	                     + r_emissiveLightScale.GetFloat()      * 64.0f
	                     + r_emissiveLightRadius.GetFloat()     * 256.0f
	                     + r_emissiveLightSaturation.GetFloat() * 1024.0f
	                     + r_itemGlow.GetFloat()               * 4096.0f;
	if ( styleSig != lastStyleSig ) {
		R_FreeAllEmissiveLights( world );
		lastStyleSig = styleSig;
	}

	const int now = tr.viewDef->renderView.time;

	// budget: when more screens want a fill light than the cap allows, keep the most
	// important (big and near); the rest simply aren't refreshed this frame and reap on the
	// normal timeout, which bounds the steady-state count without flicker (cf. r_shadowMapPointLimit)
	static idList<int>			order;
	static idList<emReqScore_t>	scores;
	order.SetNum( 0, false );
	const int limit = r_emissiveLightLimit.GetInteger();
	if ( limit > 0 && r_emissiveReqs.Num() > limit ) {
		const idVec3 viewer = tr.viewDef->renderView.vieworg;
		scores.SetNum( 0, false );
		for ( int i = 0; i < r_emissiveReqs.Num(); i++ ) {
			emReqScore_t &s = scores.Alloc();
			s.idx = i;
			const float dist = ( r_emissiveReqs[i].center - viewer ).Length();
			s.score = r_emissiveReqs[i].radius / ( dist + 1.0f );
		}
		qsort( scores.Ptr(), scores.Num(), sizeof( emReqScore_t ), R_CmpEmReqScore );
		for ( int i = 0; i < limit; i++ ) {
			order.Append( scores[i].idx );
		}
	} else {
		for ( int i = 0; i < r_emissiveReqs.Num(); i++ ) {
			order.Append( i );
		}
	}

	// The default projected-light falloff holds flat then hard-clips at the far plane, so a
	// projected fill light stops abruptly instead of fading. Borrow the point light's falloff
	// (the soft radial ramp) and stamp it on each projected fill light so it eases to nothing.
	idImage *fadeFalloff = NULL;
	{
		const idMaterial *pl = declManager->FindMaterial( "lights/defaultPointLight" );
		if ( pl != NULL ) {
			fadeFalloff = pl->LightFalloffImage();
		}
	}

	for ( int oi = 0; oi < order.Num(); oi++ ) {
		const emissiveReq_t &req = r_emissiveReqs[ order[oi] ];

		qhandle_t handle = -1;
		emissiveLight_t *el = R_FindEmissiveLight( req.entityIndex, req.surfKey );

		if ( el != NULL && !req.freshTint ) {
			// culled-surface fallback for a screen we already have a (properly sampled) light for:
			// leave it exactly as-is and just keep it alive. Its geometry/colour were captured while
			// we were facing it, which is more reliable than anything derivable without re-rendering.
			el->lastSeen = now;
			handle = el->handle;
		} else {
			renderLight_t rl;
			R_BuildEmissiveRenderLight( req, rl );

			if ( el ) {
				// only re-issue the (interaction-invalidating) update when the screen actually
				// moved/rotated or its colour shifted meaningfully — loose thresholds keep
				// flickering/animated screens from rebuilding interactions every frame
				if ( ( el->center - req.center ).LengthSqr() > 1.0f
					|| ( el->normal * req.normal ) < 0.999f
					|| idMath::Fabs( el->radius - req.radius ) > 0.5f
					|| ( el->color - req.color ).LengthSqr() > 0.01f ) {
					world->UpdateLightDef( el->handle, &rl );
					el->center = req.center;
					el->normal = req.normal;
					el->radius = req.radius;
					el->color  = req.color;
				}
				el->lastSeen = now;
				handle = el->handle;
			} else {
				qhandle_t h = world->AddLightDef( &rl );
				if ( h != -1 ) {
					emissiveLight_t &nl = r_emissiveLightList.Alloc();
					nl.entityIndex = req.entityIndex;
					nl.surfKey     = req.surfKey;
					nl.handle      = h;
					nl.lastSeen    = now;
					nl.center      = req.center;
					nl.normal      = req.normal;
					nl.radius      = req.radius;
					nl.color       = req.color;
					handle = h;
				}
			}
		}

		// stamp the fading falloff onto the derived light (re-applied every frame because
		// AddLightDef/UpdateLightDef re-derive it from the light's shader). The viewLight copies
		// light->falloffImage each frame (see R_SetLightDefViewLight), so this takes effect next frame.
		if ( fadeFalloff != NULL && handle >= 0 && handle < world->lightDefs.Num() ) {
			idRenderLightLocal *ldef = world->lightDefs[handle];
			if ( ldef != NULL ) {
				ldef->falloffImage = fadeFalloff;
			}
		}
	}

	// Reap fill lights for screens we haven't drawn recently. A screen surface stops being queued
	// the instant it's frustum- or back-face-culled, but its glow on the surrounding wall can still
	// be in full view — so before ageing a light out, keep it alive whenever its projected volume
	// still overlaps the view frustum. Only screens whose light has genuinely left the view (or been
	// destroyed) then reap on the timeout. Cheap: one box/frustum test per fill light (a handful),
	// no occlusion query — a light hidden behind a wall stays cheap and is culled at render time.
	for ( int i = r_emissiveLightList.Num() - 1; i >= 0; i-- ) {
		emissiveLight_t &el = r_emissiveLightList[i];
		// owner entity gone entirely (picked-up item, removed entity): free its light right
		// away — the frustum keep-alive below would otherwise hold the orphaned glow forever
		const bool ownerGone = el.entityIndex >= 0
			&& ( el.entityIndex >= world->entityDefs.Num() || world->entityDefs[el.entityIndex] == NULL );
		if ( !ownerGone && el.handle >= 0 && el.handle < world->lightDefs.Num() ) {
			const idRenderLightLocal *ldef = world->lightDefs[el.handle];
			if ( ldef != NULL && ldef->frustumTris != NULL
					&& !R_CullLocalBox( ldef->frustumTris->bounds, tr.viewDef->worldSpace.modelMatrix, 5, tr.viewDef->frustum ) ) {
				el.lastSeen = now;		// its illumination is still on-screen -> keep it lit
			}
		}
		const int age = now - el.lastSeen;
		if ( ownerGone || age > EMISSIVE_LIGHT_TIMEOUT_MS || age < 0 ) {
			// NULL-slot guard mirrors R_FreeAllEmissiveLights: never free a handle whose def is gone
			if ( el.handle >= 0 && el.handle < world->lightDefs.Num() && world->lightDefs[el.handle] != NULL ) {
				world->FreeLightDef( el.handle );
			}
			r_emissiveLightList.RemoveIndex( i );
		}
	}

	r_emissiveReqs.SetNum( 0, false );
}

/*
=================
R_AddDrawSurf
=================
*/
void R_AddDrawSurf( const srfTriangles_t *tri, const viewEntity_t *space, const renderEntity_t *renderEntity,
					const idMaterial *shader, const idScreenRect &scissor, const float soft_particle_radius )
{
	drawSurf_t		*drawSurf;
	const float		*shaderParms;
	static float	refRegs[MAX_EXPRESSION_REGISTERS];	// don't put on stack, or VC++ will do a page touch
	float			generatedShaderParms[MAX_ENTITY_SHADER_PARMS];

	drawSurf = (drawSurf_t *)R_FrameAlloc( sizeof( *drawSurf ) );
	drawSurf->geo = tri;
	drawSurf->space = space;
	drawSurf->material = shader;
	drawSurf->scissorRect = scissor;
	drawSurf->sort = shader->GetSort() + tr.sortOffset;

	if ( soft_particle_radius != -1.0f )	// #3878
	{
		drawSurf->dsFlags = DSF_SOFT_PARTICLE;
		drawSurf->particle_radius = soft_particle_radius;
	}
	else
	{
		drawSurf->dsFlags = 0;
		drawSurf->particle_radius = 0.0f;
	}

	// bumping this offset each time causes surfaces with equal sort orders to still
	// deterministically draw in the order they are added
	tr.sortOffset += 0.000001f;

	// if it doesn't fit, resize the list
	if ( tr.viewDef->numDrawSurfs == tr.viewDef->maxDrawSurfs ) {
		drawSurf_t	**old = tr.viewDef->drawSurfs;
		int			count;

		if ( tr.viewDef->maxDrawSurfs == 0 ) {
			tr.viewDef->maxDrawSurfs = INITIAL_DRAWSURFS;
			count = 0;
		} else {
			count = tr.viewDef->maxDrawSurfs * sizeof( tr.viewDef->drawSurfs[0] );
			tr.viewDef->maxDrawSurfs *= 2;
		}
		tr.viewDef->drawSurfs = (drawSurf_t **)R_FrameAlloc( tr.viewDef->maxDrawSurfs * sizeof( tr.viewDef->drawSurfs[0] ) );
		if(count > 0)
			memcpy( tr.viewDef->drawSurfs, old, count );
	}
	tr.viewDef->drawSurfs[tr.viewDef->numDrawSurfs] = drawSurf;
	tr.viewDef->numDrawSurfs++;

	// process the shader expressions for conditionals / color / texcoords
	const float	*constRegs = shader->ConstantRegisters();
	if ( constRegs ) {
		// shader only uses constant values
		drawSurf->shaderRegisters = constRegs;
	} else {
		float *regs = (float *)R_FrameAlloc( shader->GetNumRegisters() * sizeof( float ) );
		drawSurf->shaderRegisters = regs;

		// a reference shader will take the calculated stage color value from another shader
		// and use that for the parm0-parm3 of the current shader, which allows a stage of
		// a light model and light flares to pick up different flashing tables from
		// different light shaders
		if ( renderEntity->referenceShader ) {
			// evaluate the reference shader to find our shader parms
			const shaderStage_t *pStage;

			renderEntity->referenceShader->EvaluateRegisters( refRegs, renderEntity->shaderParms, tr.viewDef, renderEntity->referenceSound );
			pStage = renderEntity->referenceShader->GetStage(0);

			memcpy( generatedShaderParms, renderEntity->shaderParms, sizeof( generatedShaderParms ) );
			generatedShaderParms[0] = refRegs[ pStage->color.registers[0] ];
			generatedShaderParms[1] = refRegs[ pStage->color.registers[1] ];
			generatedShaderParms[2] = refRegs[ pStage->color.registers[2] ];

			shaderParms = generatedShaderParms;
		} else {
			// evaluate with the entityDef's shader parms
			shaderParms = renderEntity->shaderParms;
		}

		float oldFloatTime = 0.0f;
		int oldTime = 0;

		if ( space->entityDef && space->entityDef->parms.timeGroup ) {
			oldFloatTime = tr.viewDef->floatTime;
			oldTime = tr.viewDef->renderView.time;

			tr.viewDef->floatTime = game->GetTimeGroupTime( space->entityDef->parms.timeGroup ) * 0.001;
			tr.viewDef->renderView.time = game->GetTimeGroupTime( space->entityDef->parms.timeGroup );
		}

		shader->EvaluateRegisters( regs, shaderParms, tr.viewDef, renderEntity->referenceSound );

		if ( space->entityDef && space->entityDef->parms.timeGroup ) {
			tr.viewDef->floatTime = oldFloatTime;
			tr.viewDef->renderView.time = oldTime;
		}
	}

	// check for deformations
	R_DeformDrawSurf( drawSurf );

	// skybox surfaces need a dynamic texgen
	switch( shader->Texgen() ) {
		case TG_SKYBOX_CUBE:
			R_SkyboxTexGen( drawSurf, tr.viewDef->renderView.vieworg );
			break;
		case TG_WOBBLESKY_CUBE:
			R_WobbleskyTexGen( drawSurf, tr.viewDef->renderView.vieworg );
			break;
	}

	// check for gui surfaces
	idUserInterface	*gui = NULL;

	if ( !space->entityDef ) {
		gui = shader->GlobalGui();
	} else {
		int guiNum = shader->GetEntityGui() - 1;
		if ( guiNum >= 0 && guiNum < MAX_RENDERENTITY_GUI ) {
			gui = renderEntity->gui[ guiNum ];
		}
		if ( gui == NULL ) {
			gui = shader->GlobalGui();
		}
	}

	if ( gui ) {
		// force guis on the fast time
		float oldFloatTime;
		int oldTime;

		oldFloatTime = tr.viewDef->floatTime;
		oldTime = tr.viewDef->renderView.time;

		tr.viewDef->floatTime = game->GetTimeGroupTime( 1 ) * 0.001;
		tr.viewDef->renderView.time = game->GetTimeGroupTime( 1 );

		idBounds ndcBounds;

		if ( !R_PreciseCullSurface( drawSurf, ndcBounds ) ) {
			// did we ever use this to forward an entity color to a gui that didn't set color?
//			memcpy( tr.guiShaderParms, shaderParms, sizeof( tr.guiShaderParms ) );
			R_RenderGuiSurf( gui, drawSurf );

			// DUDE: queue a small fill light so this screen bleeds onto its surroundings
			// (right after the redraw, while its sampled content colour is fresh)
			if ( r_emissiveSurfaces.GetBool() && tr.viewDef->renderView.viewID > 0 && R_BackendSupportsEnhancements() ) {
				idVec3 tint( 1.0f, 1.0f, 1.0f );
				if ( tr_guiEmissiveColorValid ) {
					tint = tr_guiEmissiveColor;
				}
				R_QueueEmissiveLight( tri, space, shader, tint );
			}
		} else if ( r_emissiveSurfaces.GetBool() && tr.viewDef->renderView.viewID > 0 && R_BackendSupportsEnhancements() ) {
			// DUDE: gui is inside the frustum but back-facing/edge-on, so it wasn't re-rendered — still
			// queue its fill light (non-fresh: white fallback for a never-seen screen, or the cached tint
			// preserved in R_UpdateEmissiveLights) so a screen we've turned away from keeps lighting the
			// wall in front of it instead of switching off.
			R_QueueEmissiveLight( tri, space, shader, idVec3( 1.0f, 1.0f, 1.0f ), false );
		}

		tr.viewDef->floatTime = oldFloatTime;
		tr.viewDef->renderView.time = oldTime;
	}

	// DUDE: cinematic/video-material screens carry no gui but are clearly "screens" — give
	// them the same fill light, tinted (best-effort) from the shared cinematic image, which
	// our UploadScratch hook keeps averaged. Multiple different cinematics in view share that
	// image, so the tint is approximate for the rarer multi-screen case.
	if ( gui == NULL && r_emissiveSurfaces.GetBool() && tr.viewDef->renderView.viewID > 0
			&& R_BackendSupportsEnhancements() && R_MaterialHasCinematic( shader ) ) {
		idVec3 tint( 1.0f, 1.0f, 1.0f );
		if ( globalImages->cinematicImage != NULL ) {
			tint.Set( globalImages->cinematicImage->averageColor[0],
					  globalImages->cinematicImage->averageColor[1],
					  globalImages->cinematicImage->averageColor[2] );
		}
		R_QueueEmissiveLight( tri, space, shader, tint );
	}

	// we can't add subviews at this point, because that would
	// increment tr.viewCount, messing up the rest of the surface
	// adds for this view
}

/*
===============
R_AddAmbientDrawsurfs

Adds surfaces for the given viewEntity
Walks through the viewEntitys list and creates drawSurf_t for each surface of
each viewEntity that has a non-empty scissorRect
===============
*/
static void R_AddAmbientDrawsurfs( viewEntity_t *vEntity ) {
	int					i, total;
	idRenderEntityLocal	*def;
	srfTriangles_t		*tri;
	idRenderModel		*model;
	const idMaterial	*shader;

	def = vEntity->entityDef;

	if ( def->dynamicModel ) {
		model = def->dynamicModel;
	} else {
		model = def->parms.hModel;
	}

	// add all the surfaces
	total = model->NumSurfaces();
	for ( i = 0 ; i < total ; i++ ) {
		const modelSurface_t	*surf = model->Surface( i );

		// for debugging, only show a single surface at a time
		if ( r_singleSurface.GetInteger() >= 0 && i != r_singleSurface.GetInteger() ) {
			continue;
		}

		tri = surf->geometry;
		if ( !tri ) {
			continue;
		}
		if ( !tri->numIndexes ) {
			continue;
		}
		shader = surf->shader;
		shader = R_RemapShaderBySkin( shader, def->parms.customSkin, def->parms.customShader );

		R_GlobalShaderOverride( &shader );

		if ( !shader ) {
			continue;
		}
		if ( !shader->IsDrawn() ) {
			continue;
		}

		// debugging tool to make sure we are have the correct pre-calculated bounds
		if ( r_checkBounds.GetBool() ) {
			int j, k;
			for ( j = 0 ; j < tri->numVerts ; j++ ) {
				for ( k = 0 ; k < 3 ; k++ ) {
					if ( tri->verts[j].xyz[k] > tri->bounds[1][k] + CHECK_BOUNDS_EPSILON
						|| tri->verts[j].xyz[k] < tri->bounds[0][k] - CHECK_BOUNDS_EPSILON ) {
						common->Printf( "bad tri->bounds on %s:%s\n", def->parms.hModel->Name(), shader->GetName() );
						break;
					}
					if ( tri->verts[j].xyz[k] > def->referenceBounds[1][k] + CHECK_BOUNDS_EPSILON
						|| tri->verts[j].xyz[k] < def->referenceBounds[0][k] - CHECK_BOUNDS_EPSILON ) {
						common->Printf( "bad referenceBounds on %s:%s\n", def->parms.hModel->Name(), shader->GetName() );
						break;
					}
				}
				if ( k != 3 ) {
					break;
				}
			}
		}

		if ( !R_CullLocalBox( tri->bounds, vEntity->modelMatrix, 5, tr.viewDef->frustum ) ) {

			def->visibleCount = tr.viewCount;

			// make sure we have an ambient cache
			if ( !R_CreateAmbientCache( tri, shader->ReceivesLighting() ) ) {
				// don't add anything if the vertex cache was too full to give us an ambient cache
				return;
			}
			// touch it so it won't get purged
			vertexCache.Touch( tri->ambientCache );

			if ( r_useIndexBuffers.GetBool() && !tri->indexCache ) {
				vertexCache.Alloc( tri->indexes, tri->numIndexes * sizeof( tri->indexes[0] ), &tri->indexCache, true );
			}
			if ( tri->indexCache ) {
				vertexCache.Touch( tri->indexCache );
			}

			// Soft Particles -- SteveL #3878
			float particle_radius = -1.0f;		// Default = disallow softening, but allow modelDepthHack if specified in the decl.
			if ( R_BackendSupportsEnhancements()        // DUDE: non-vanilla, GL3/Vulkan only
				&& r_useSoftParticles.GetBool() && r_enableDepthCapture.GetInteger() != 0
				&& !shader->ReceivesLighting()          // don't soften surfaces that are meant to be solid
				&& tr.viewDef->renderView.viewID >= 0 ) // Skip during "invisible" rendering passes (e.g. lightgem)
			{
				const idRenderModelPrt* prt = dynamic_cast<const idRenderModelPrt*>( def->parms.hModel ); // yuck.
				if ( prt )
				{
					particle_radius = prt->SofteningRadius( surf->id );
				}
			}

			// add the surface for drawing
			R_AddDrawSurf( tri, vEntity, &vEntity->entityDef->parms, shader, vEntity->scissorRect, particle_radius );

			// DUDE: self-lit item bits (armor, medkits, powerups) give off a faint glow
			if ( r_itemGlow.GetFloat() > 0.0f && tr.viewDef->renderView.viewID > 0
					&& R_BackendSupportsEnhancements() ) {
				R_QueueItemGlowLight( tri, vEntity, shader );
			}

			// ambientViewCount is used to allow light interactions to be rejected
			// if the ambient surface isn't visible at all
			tri->ambientViewCount = tr.viewCount;
		} else if ( tr.viewDef->renderView.viewID > 0 && R_BackendSupportsEnhancements()
				&& ( r_emissiveSurfaces.GetBool() || r_itemGlow.GetFloat() > 0.0f ) ) {
			// DUDE: this surface is off-screen/behind us this frame, so it never reaches R_AddDrawSurf.
			// If it's a screen, still queue its fill light (best-effort, no gui re-render) so emissive
			// surfaces light their surroundings before/without us looking straight at them. Lights whose
			// glow truly leaves the view still reap on the normal timeout (see R_UpdateEmissiveLights).
			if ( r_emissiveSurfaces.GetBool() ) {
				R_QueueEmissiveScreenFallback( tri, vEntity, shader );
			}
			// same for item glow: the colour needs no gui render, so the culled path is identical
			if ( r_itemGlow.GetFloat() > 0.0f ) {
				R_QueueItemGlowLight( tri, vEntity, shader );
			}
		}
	}

	// add the lightweight decal surfaces
	for ( idRenderModelDecal *decal = def->decals; decal; decal = decal->Next() ) {
		decal->AddDecalDrawSurf( vEntity );
	}
}

/*
==================
R_CalcEntityScissorRectangle
==================
*/
idScreenRect R_CalcEntityScissorRectangle( viewEntity_t *vEntity ) {
	idBounds bounds;
	idRenderEntityLocal *def = vEntity->entityDef;

	tr.viewDef->viewFrustum.ProjectionBounds( idBox( def->referenceBounds, def->parms.origin, def->parms.axis ), bounds );

	return R_ScreenRectFromViewFrustumBounds( bounds );
}

/*
===================
R_AddModelSurfaces

Here is where dynamic models actually get instantiated, and necessary
interactions get created.  This is all done on a sort-by-model basis
to keep source data in cache (most likely L2) as any interactions and
shadows are generated, since dynamic models will typically be lit by
two or more lights.
===================
*/
void R_AddModelSurfaces( void ) {
	viewEntity_t		*vEntity;
	idInteraction		*inter, *next;
	idRenderModel		*model;

	// clear the ambient surface list
	tr.viewDef->numDrawSurfs = 0;
	tr.viewDef->maxDrawSurfs = 0;	// will be set to INITIAL_DRAWSURFS on R_AddDrawSurf

	// go through each entity that is either visible to the view, or to
	// any light that intersects the view (for shadows)
	for ( vEntity = tr.viewDef->viewEntitys; vEntity; vEntity = vEntity->next ) {

		// DUDE glass probes (docs/ssr.md): keep the view weapon out of probe
		// captures — its depth-hacked, screen-filling model would smear across
		// the baked cubemap faces (the player body is already suppressed by the
		// unchanged viewID)
		if ( tr.takingEnvProbe && vEntity->weaponDepthHack ) {
			continue;
		}

		if ( r_useEntityScissors.GetBool() ) {
			// calculate the screen area covered by the entity
			idScreenRect scissorRect = R_CalcEntityScissorRectangle( vEntity );
			// intersect with the portal crossing scissor rectangle
			vEntity->scissorRect.Intersect( scissorRect );

			if ( r_showEntityScissors.GetBool() ) {
				R_ShowColoredScreenRect( vEntity->scissorRect, vEntity->entityDef->index );
			}
		}

		float oldFloatTime = 0.0f;
		int oldTime = 0;

		game->SelectTimeGroup( vEntity->entityDef->parms.timeGroup );

		if ( vEntity->entityDef->parms.timeGroup ) {
			oldFloatTime = tr.viewDef->floatTime;
			oldTime = tr.viewDef->renderView.time;

			tr.viewDef->floatTime = game->GetTimeGroupTime( vEntity->entityDef->parms.timeGroup ) * 0.001;
			tr.viewDef->renderView.time = game->GetTimeGroupTime( vEntity->entityDef->parms.timeGroup );
		}

		if ( tr.viewDef->isXraySubview && vEntity->entityDef->parms.xrayIndex == 1 ) {
			if ( vEntity->entityDef->parms.timeGroup ) {
				tr.viewDef->floatTime = oldFloatTime;
				tr.viewDef->renderView.time = oldTime;
			}
			continue;
		} else if ( !tr.viewDef->isXraySubview && vEntity->entityDef->parms.xrayIndex == 2 ) {
			if ( vEntity->entityDef->parms.timeGroup ) {
				tr.viewDef->floatTime = oldFloatTime;
				tr.viewDef->renderView.time = oldTime;
			}
			continue;
		}

		// Don't let particle entities re-instantiate their dynamic model during non-visible 
		// views (in TDM, the light gem render) -- SteveL #3970
		if ( tr.viewDef->renderView.viewID < 0
			&& dynamic_cast<const idRenderModelPrt*>( vEntity->entityDef->parms.hModel ) != NULL ) // yuck.
		{
			continue;
		}

		// add the ambient surface if it has a visible rectangle
		if ( !vEntity->scissorRect.IsEmpty() ) {
			model = R_EntityDefDynamicModel( vEntity->entityDef );
			if ( model == NULL || model->NumSurfaces() <= 0 ) {
				if ( vEntity->entityDef->parms.timeGroup ) {
					tr.viewDef->floatTime = oldFloatTime;
					tr.viewDef->renderView.time = oldTime;
				}
				continue;
			}

			R_AddAmbientDrawsurfs( vEntity );
			tr.pc.c_visibleViewEntities++;
		} else {
			tr.pc.c_shadowViewEntities++;
		}

		//
		// for all the entity / light interactions on this entity, add them to the view
		//
		if ( tr.viewDef->isXraySubview ) {
			if ( vEntity->entityDef->parms.xrayIndex == 2 ) {
				for ( inter = vEntity->entityDef->firstInteraction; inter != NULL && !inter->IsEmpty(); inter = next ) {
					next = inter->entityNext;
					if ( inter->lightDef->viewCount != tr.viewCount ) {
						continue;
					}
					inter->AddActiveInteraction();
				}
			}
		} else {
			// all empty interactions are at the end of the list so once the
			// first is encountered all the remaining interactions are empty
			for ( inter = vEntity->entityDef->firstInteraction; inter != NULL && !inter->IsEmpty(); inter = next ) {
				next = inter->entityNext;

				// skip any lights that aren't currently visible
				// this is run after any lights that are turned off have already
				// been removed from the viewLights list, and had their viewCount cleared
				if ( inter->lightDef->viewCount != tr.viewCount ) {
					continue;
				}
				inter->AddActiveInteraction();
			}
		}

		if ( vEntity->entityDef->parms.timeGroup ) {
			tr.viewDef->floatTime = oldFloatTime;
			tr.viewDef->renderView.time = oldTime;
		}

	}

	// DUDE: reconcile the GUI-screen fill lights collected during the surface pass
	R_UpdateEmissiveLights();
}

/*
=====================
R_RemoveUnecessaryViewLights
=====================
*/
void R_RemoveUnecessaryViewLights( void ) {
	viewLight_t		*vLight;

	// go through each visible light
	for ( vLight = tr.viewDef->viewLights ; vLight ; vLight = vLight->next ) {
		// if the light didn't have any lit surfaces visible, there is no need to
		// draw any of the shadows.  We still keep the vLight for debugging
		// draws
		if ( !vLight->localInteractions && !vLight->globalInteractions && !vLight->translucentInteractions ) {
			vLight->localShadows = NULL;
			vLight->globalShadows = NULL;
		}
	}

	if ( r_useShadowSurfaceScissor.GetBool() ) {
		// shrink the light scissor rect to only intersect the surfaces that will actually be drawn.
		// This doesn't seem to actually help, perhaps because the surface scissor
		// rects aren't actually the surface, but only the portal clippings.
		for ( vLight = tr.viewDef->viewLights ; vLight ; vLight = vLight->next ) {
			const drawSurf_t	*surf;
			idScreenRect	surfRect;

			if ( !vLight->lightShader->LightCastsShadows() ) {
				continue;
			}

			surfRect.Clear();

			for ( surf = vLight->globalInteractions ; surf ; surf = surf->nextOnLight ) {
				surfRect.Union( surf->scissorRect );
			}
			for ( surf = vLight->localShadows ; surf ; surf = surf->nextOnLight ) {
				const_cast<drawSurf_t *>(surf)->scissorRect.Intersect( surfRect );
			}

			for ( surf = vLight->localInteractions ; surf ; surf = surf->nextOnLight ) {
				surfRect.Union( surf->scissorRect );
			}
			for ( surf = vLight->globalShadows ; surf ; surf = surf->nextOnLight ) {
				const_cast<drawSurf_t *>(surf)->scissorRect.Intersect( surfRect );
			}

			for ( surf = vLight->translucentInteractions ; surf ; surf = surf->nextOnLight ) {
				surfRect.Union( surf->scissorRect );
			}

			vLight->scissorRect.Intersect( surfRect );
		}
	}
}
