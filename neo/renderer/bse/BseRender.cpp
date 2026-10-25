/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: Quake 4's effects (Raven's BSE) - the engine side of the runtime: played effects as
// engine render entities, their particles drawn per view as frame-temporary geometry, and the
// testEffect command. See BseEffect.h and docs/quake4-bse.md 3.
//
// A played effect is a render entity whose model is the shared effect model below, its
// callbackData the effect: the render world links, culls and scissors it like any entity, and
// R_AddModelSurfaces hands it here instead of instantiating a model. Once every entity of the
// view is done, R_AddBseSurfaces builds each visible effect's particles for this view into
// frame-temporary vertices (the deform-particle and decal pattern: nothing is uploaded that
// outlives the frame, on any backend) and adds them as draw surfaces - effects farthest first,
// each effect's segments in file order, so equal-sort translucent surfaces overlap as in retail.
// The effect geometry never reaches ray tracing (a continuous dynamic model) and writes no motion
// vectors (translucent): FSR2's reactive mask covers it as it covers Doom 3's particles.

#include "sys/platform.h"
#include "framework/Common.h"
#include "framework/CmdSystem.h"
#include "framework/Session.h"
#include "framework/GameTitle.h"
#include "renderer/VertexCache.h"
#include "renderer/tr_local.h"
#include "renderer/Model_local.h"
#include "renderer/RenderWorld_local.h"
#include "renderer/bse/BseEffect.h"

/*
===============================================================================

	the effect model: marks an entity as a played effect, draws nothing itself

===============================================================================
*/

class idRenderModelBse : public idRenderModelStatic {
public:
	virtual dynamicModel_t		IsDynamicModel() const { return DM_CONTINUOUS; }
	virtual idRenderModel *		InstantiateDynamicModel( const struct renderEntity_s *ent, const struct viewDef_s *view, idRenderModel *cachedModel ) {
									delete cachedModel;
									return NULL;
								}
	virtual idBounds			Bounds( const struct renderEntity_s *ent ) const {
									return ent != NULL ? ent->bounds : bounds;
								}
};

static idRenderModelBse *		bseModel = NULL;
static idList<idBseEffect *>	bseEffects;			// every played effect
static idList<idBseEffect *>	bseTestEffects;		// testEffect's
static idRandom					bseRandom;

// the last top-level view of each render world: the player's view, for distance attenuation and
// testEffect
typedef struct {
	const idRenderWorld *	world;
	renderView_t			view;
} bseWorldView_t;
static idList<bseWorldView_t>	bseWorldViews;

static const renderView_t *BSE_WorldView( const idRenderWorld *world ) {
	for ( int i = 0; i < bseWorldViews.Num(); i++ ) {
		if ( bseWorldViews[i].world == world ) {
			return &bseWorldViews[i].view;
		}
	}
	return NULL;
}

idRandom &BSE_Random( void ) {
	return bseRandom;
}

float BSE_GameGravity( void ) {
	const float gravity = cvarSystem->GetCVarFloat( "g_gravity" );
	return gravity != 0.0f ? gravity : 1066.0f;		// Quake 4's default
}

bool BSE_ViewOrigin( const idRenderWorld *world, idVec3 &origin ) {
	const renderView_t *view = BSE_WorldView( world );
	if ( view == NULL ) {
		return false;
	}
	origin = view->vieworg;
	return true;
}

/*
===============================================================================

	effects

===============================================================================
*/

// the effect's render entity, if it is still the effect's (a map load frees every entity)
static idRenderEntityLocal *BSE_EntityDef( const idBseEffect *effect ) {
	idRenderWorldLocal *world = static_cast<idRenderWorldLocal *>( effect->world );
	const int handle = effect->entityHandle;
	if ( world == NULL || handle < 0 || handle >= world->entityDefs.Num() ) {
		return NULL;
	}
	idRenderEntityLocal *def = world->entityDefs[handle];
	if ( def == NULL || def->parms.hModel != bseModel || def->parms.callbackData != effect ) {
		return NULL;
	}
	return def;
}

static void BSE_FillEntity( const idBseEffect *effect, renderEntity_t &ent ) {
	const bseEffectParms_t &parms = effect->owner;
	memset( &ent, 0, sizeof( ent ) );
	ent.hModel = bseModel;
	ent.callbackData = const_cast<idBseEffect *>( effect );
	ent.origin = effect->currentOrigin;
	ent.axis = effect->currentAxis;
	ent.bounds = effect->localBounds;
	if ( !effect->particleBounds.IsCleared() ) {
		ent.bounds.AddBounds( effect->particleBounds );
	}
	ent.noShadow = true;
	ent.noSelfShadow = true;
	ent.noDynamicInteractions = true;
	ent.suppressSurfaceInViewID = parms.suppressSurfaceInViewID;
	ent.allowSurfaceInViewID = parms.allowSurfaceInViewID;
	ent.weaponDepthHack = parms.weaponDepthHack;
	ent.modelDepthHack = parms.modelDepthHack;
	memcpy( ent.shaderParms, parms.shaderParms, sizeof( ent.shaderParms ) );
}

static void BSE_SyncEntity( idBseEffect *effect ) {
	if ( BSE_EntityDef( effect ) == NULL ) {
		return;
	}
	renderEntity_t ent;
	BSE_FillEntity( effect, ent );
	effect->world->UpdateEntityDef( effect->entityHandle, &ent );
}

/*
================
BSE_PlayEffect
================
*/
idBseEffect *BSE_PlayEffect( idRenderWorld *world, const bseEffectParms_t &parms, int timeMs ) {
	if ( !BSE_Enabled() || parms.effect == NULL || world == NULL || bseModel == NULL ) {
		return NULL;
	}
	idBseEffect *effect = new idBseEffect;
	effect->world = world;
	effect->entityHandle = -1;
	effect->serviceTime = timeMs - 1;		// the frame's first view services it
	effect->Init( parms, MS2SEC( timeMs ) );
	if ( BSE_Debug() ) {
		common->Printf( "BSE: '%s' starts at %d%s\n", parms.effect->GetName(), timeMs, parms.loop ? ", looping" : "" );
	}

	renderEntity_t ent;
	BSE_FillEntity( effect, ent );
	effect->entityHandle = world->AddEntityDef( &ent );
	bseEffects.Append( effect );
	return effect;
}

/*
================
BSE_UpdateEffect
================
*/
bool BSE_UpdateEffect( idBseEffect *effect, const bseEffectParms_t &parms, int timeMs ) {
	if ( effect == NULL ) {
		return true;
	}
	if ( timeMs > effect->serviceTime || effect->serviceTime == timeMs ) {
		effect->UpdateFromOwner( parms, MS2SEC( timeMs ) );
		effect->finished = effect->Service( MS2SEC( timeMs ) );
		effect->serviceTime = timeMs;
	}
	BSE_SyncEntity( effect );
	if ( effect->finished && BSE_Debug() ) {
		common->Printf( "BSE: '%s' is over\n", effect->decl->GetName() );
	}
	return effect->finished;
}

void BSE_StopEffect( idBseEffect *effect ) {
	if ( effect != NULL ) {
		effect->Stop();
		if ( BSE_Debug() ) {
			common->Printf( "BSE: '%s' stops\n", effect->decl->GetName() );
		}
	}
}

void BSE_FreeEffect( idBseEffect *effect ) {
	if ( effect == NULL ) {
		return;
	}
	if ( BSE_EntityDef( effect ) != NULL ) {
		effect->world->FreeEntityDef( effect->entityHandle );
	}
	bseEffects.Remove( effect );
	bseTestEffects.Remove( effect );
	delete effect;
}

float BSE_EffectDuration( const idBseEffect *effect ) {
	return effect != NULL ? effect->GetDuration() : 0.0f;
}

float BSE_DeclDuration( const idDeclEffect *decl ) {
	return decl != NULL ? decl->GetMaxDuration() : 0.0f;
}

/*
===============================================================================

	drawing

===============================================================================
*/

// the visible effects R_AddModelSurfaces handed over, with their view (a view rendered while
// another is being built must not draw the other's)
typedef struct {
	const viewDef_t *		view;
	viewEntity_t *			vEntity;
} bseViewEntity_t;
static idList<bseViewEntity_t>	bseViewEntities;

bool R_IsBseEntity( const renderEntity_t *parms ) {
	return bseModel != NULL && parms->hModel == bseModel;
}

void R_AddBseEntity( viewEntity_t *vEntity ) {
	bseViewEntity_t &entry = bseViewEntities.Alloc();
	entry.view = tr.viewDef;
	entry.vEntity = vEntity;
}

typedef struct {
	viewEntity_t *	vEntity;
	float			distance;
} bseSortEntity_t;

static int BSE_SortFarthestFirst( const void *a, const void *b ) {
	const float da = static_cast<const bseSortEntity_t *>( a )->distance;
	const float db = static_cast<const bseSortEntity_t *>( b )->distance;
	return da < db ? 1 : ( da > db ? -1 : 0 );
}

/*
================
R_AddBseEffectSurfaces

One effect's particles for this view: per drawn segment, in file order, a frame-temporary
surface of sprites (other particle kinds: S3).
================
*/
static void R_AddBseEffectSurfaces( viewEntity_t *vEntity ) {
	idRenderEntityLocal *def = vEntity->entityDef;
	idBseEffect *effect = static_cast<idBseEffect *>( def->parms.callbackData );
	const viewDef_t *view = tr.viewDef;

	// as R_AddAmbientDrawsurfs: an entity shown only in, or hidden from, one view
	if ( def->parms.suppressSurfaceInViewID && def->parms.suppressSurfaceInViewID == view->renderView.viewID ) {
		return;
	}
	if ( def->parms.allowSurfaceInViewID && def->parms.allowSurfaceInViewID != view->renderView.viewID ) {
		return;
	}

	// an effect no owner has updated to this time yet (a new one, or testEffect's)
	if ( view->renderView.time > effect->serviceTime ) {
		effect->UpdateFromOwner( effect->owner, MS2SEC( view->renderView.time ) );
		effect->finished = effect->Service( MS2SEC( view->renderView.time ) ) || effect->finished;
		effect->serviceTime = view->renderView.time;
	}
	if ( !BSE_Render() ) {
		return;
	}

	// the viewer's left and up axes in the effect's frame
	const idMat3 &axis = def->parms.axis;
	const idVec3 viewLeft = axis.Transpose() * view->renderView.viewaxis[1];
	const idVec3 viewUp = axis.Transpose() * view->renderView.viewaxis[2];
	const float time = MS2SEC( view->renderView.time );

	for ( int i = 0; i < effect->segments.Num(); i++ ) {
		const idBseSegment &segment = effect->segments[i];
		const idEffectSegment *tmpl = segment.tmpl;
		if ( !tmpl->enabled || !tmpl->hasParticles || segment.particles.Num() == 0 ) {
			continue;
		}
		if ( tmpl->particle.type != EFFECT_PARTICLE_SPRITE ) {
			continue;		// lines, oriented quads, bolts, strips, models: S3
		}
		const idMaterial *material = tmpl->particle.material;
		if ( material == NULL || !material->IsDrawn() ) {
			continue;
		}

		const int maxQuads = segment.particles.Num();
		srfTriangles_t *tri = (srfTriangles_t *)R_ClearedFrameAlloc( sizeof( *tri ) );
		tri->verts = (idDrawVert *)R_FrameAlloc( maxQuads * 4 * sizeof( idDrawVert ) );
		tri->indexes = (glIndex_t *)R_FrameAlloc( maxQuads * 6 * sizeof( glIndex_t ) );
		tri->bounds.Clear();
		const int quads = effect->BuildSprites( segment, time, viewLeft, viewUp, tri->verts, maxQuads, tri->bounds );
		if ( quads == 0 ) {
			continue;
		}
		for ( int q = 0; q < quads; q++ ) {
			glIndex_t *index = tri->indexes + q * 6;
			const int base = q * 4;
			index[0] = base + 0;
			index[1] = base + 1;
			index[2] = base + 2;
			index[3] = base + 0;
			index[4] = base + 2;
			index[5] = base + 3;
		}
		tri->numVerts = quads * 4;
		tri->numIndexes = quads * 6;
		tri->ambientCache = vertexCache.AllocFrameTemp( tri->verts, tri->numVerts * sizeof( idDrawVert ) );
		if ( tri->ambientCache == NULL ) {
			continue;
		}
		R_AddDrawSurf( tri, vEntity, &def->parms, material, vEntity->scissorRect );
	}
}

/*
================
R_AddBseSurfaces

The view's visible effects, farthest first.
================
*/
void R_AddBseSurfaces( void ) {
	if ( !tr.viewDef->isSubview && tr.viewDef->renderWorld != NULL ) {
		bseWorldView_t *entry = NULL;
		for ( int i = 0; i < bseWorldViews.Num(); i++ ) {
			if ( bseWorldViews[i].world == tr.viewDef->renderWorld ) {
				entry = &bseWorldViews[i];
			}
		}
		if ( entry == NULL ) {
			entry = &bseWorldViews.Alloc();
			entry->world = tr.viewDef->renderWorld;
		}
		entry->view = tr.viewDef->renderView;
	}
	if ( bseViewEntities.Num() == 0 ) {
		return;
	}
	bseSortEntity_t *sorted = (bseSortEntity_t *)R_FrameAlloc( bseViewEntities.Num() * sizeof( bseSortEntity_t ) );
	int num = 0;
	for ( int i = 0; i < bseViewEntities.Num(); i++ ) {
		if ( bseViewEntities[i].view == tr.viewDef ) {
			sorted[num].vEntity = bseViewEntities[i].vEntity;
			sorted[num].distance = ( sorted[num].vEntity->entityDef->parms.origin - tr.viewDef->renderView.vieworg ).LengthSqr();
			num++;
			bseViewEntities.RemoveIndex( i-- );
		}
	}
	qsort( sorted, num, sizeof( sorted[0] ), BSE_SortFarthestFirst );
	for ( int i = 0; i < num; i++ ) {
		R_AddBseEffectSurfaces( sorted[i].vEntity );
	}
}

/*
===============================================================================

	testEffect

===============================================================================
*/

static void BSE_ClearTestEffects( void ) {
	while ( bseTestEffects.Num() > 0 ) {
		BSE_FreeEffect( bseTestEffects[0] );
	}
}

/*
================
BSE_TestEffect_f

testEffect <effect> [once]: plays an effect 80 units in front of the view, facing it - where the
game's "spawn func_fx fx <effect>" puts one, so the two compare - looping unless "once".
testEffect alone removes it.
================
*/
static void BSE_TestEffect_f( const idCmdArgs &args ) {
	BSE_ClearTestEffects();
	if ( args.Argc() < 2 ) {
		common->Printf( "usage: testEffect <effect> [once]   (no effect: remove the test effect)\n" );
		return;
	}
	const idDeclEffect *decl = static_cast<const idDeclEffect *>( declManager->FindType( DECL_EFFECT, args.Argv( 1 ), false ) );
	if ( decl == NULL ) {
		common->Printf( "testEffect: no effect '%s'\n", args.Argv( 1 ) );
		return;
	}
	idRenderWorld *world = session != NULL ? session->rw : NULL;
	const renderView_t *shown = world != NULL ? BSE_WorldView( world ) : NULL;
	if ( shown == NULL ) {
		common->Printf( "testEffect: no map is shown\n" );
		return;
	}

	const renderView_t &view = *shown;
	const float yaw = view.viewaxis[0].ToYaw();
	bseEffectParms_t parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.effect = decl;
	parms.startTime = MS2SEC( view.time );
	parms.origin = view.vieworg + idAngles( 0.0f, yaw, 0.0f ).ToForward() * 80.0f;
	parms.axis = idAngles( 0.0f, yaw + 180.0f, 0.0f ).ToMat3();
	parms.gravity.Set( 0.0f, 0.0f, -BSE_GameGravity() );
	parms.attenuation = 1.0f;
	parms.loop = !( args.Argc() > 2 && !idStr::Icmp( args.Argv( 2 ), "once" ) );
	parms.inConnectedArea = true;
	parms.referenceSoundHandle = -1;
	parms.shaderParms[SHADERPARM_RED] = parms.shaderParms[SHADERPARM_GREEN] = parms.shaderParms[SHADERPARM_BLUE] = parms.shaderParms[SHADERPARM_ALPHA] = 1.0f;
	parms.shaderParms[6] = 1.0f;		// brightness
	parms.shaderParms[SHADERPARM_TIMEOFFSET] = MS2SEC( view.time );

	idBseEffect *effect = BSE_PlayEffect( world, parms, view.time );
	if ( effect == NULL ) {
		common->Printf( "testEffect: '%s' does not play (bse_enabled?)\n", decl->GetName() );
		return;
	}
	bseTestEffects.Append( effect );
	common->Printf( "testEffect: '%s'%s, %d segments, %g s, radius %g\n", decl->GetName(), parms.loop ? " looping" : "",
		decl->GetNumSegments(), decl->GetMaxDuration(), effect->boundsRadius );
}

/*
===============================================================================

	init / shutdown

===============================================================================
*/

void BSE_Init( void ) {
	if ( bseModel == NULL ) {
		bseModel = new idRenderModelBse;
		bseModel->InitEmpty( "_BSE_EFFECT" );
	}
	bseRandom.SetSeed( 0x5eed );

	// Quake 4 only: the other titles have no effect decls (and keep their cvar list as it was)
	if ( GameTitle::Get().title != GAME_TITLE_QUAKE4 ) {
		return;
	}
	if ( bse_enabled == NULL ) {
		// created now that the title is known (idCVar registers itself once the static list is in)
		bse_enabled			= new idCVar( "bse_enabled", "1", CVAR_RENDERER | CVAR_BOOL, "Quake 4 effects: play them (0: no effect starts)" );
		bse_render			= new idCVar( "bse_render", "1", CVAR_RENDERER | CVAR_BOOL, "Quake 4 effects: draw them" );
		bse_scale			= new idCVar( "bse_scale", "1", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
								"Quake 4 effects: detail 0-1 - emitters spawn between their lowest and highest count by it, and a segment shows from its detail level up", 0.0f, 1.0f );
		bse_maxParticles	= new idCVar( "bse_maxParticles", "2048", CVAR_RENDERER | CVAR_INTEGER, "Quake 4 effects: the most particles one segment holds", 0.0f, 2048.0f );
		bse_debug			= new idCVar( "bse_debug", "0", CVAR_RENDERER | CVAR_INTEGER, "Quake 4 effects: 1 = log effects as they start, loop, stop and end" );
	}
	cmdSystem->AddCommand( "testEffect", BSE_TestEffect_f, CMD_FL_RENDERER | CMD_FL_CHEAT,
		"Quake 4: plays an effect in front of the view, as 'spawn func_fx fx <effect>' does - testEffect <effect> [once]",
		idCmdSystem::ArgCompletion_Decl<DECL_EFFECT> );
}

void BSE_WorldFreed( idRenderWorld *world ) {
	for ( int i = 0; i < bseEffects.Num(); i++ ) {
		if ( bseEffects[i]->world == world ) {
			bseEffects[i]->world = NULL;
			bseEffects[i]->entityHandle = -1;
		}
	}
	for ( int i = 0; i < bseWorldViews.Num(); i++ ) {
		if ( bseWorldViews[i].world == world ) {
			bseWorldViews.RemoveIndex( i-- );
		}
	}
}

void BSE_Shutdown( void ) {
	// the owners (the game's render world facade) are gone by now, and so are the worlds
	for ( int i = 0; i < bseEffects.Num(); i++ ) {
		delete bseEffects[i];
	}
	bseEffects.Clear();
	bseTestEffects.Clear();
	bseViewEntities.Clear();
	cmdSystem->RemoveCommand( "testEffect" );
	bseWorldViews.Clear();
	delete bseModel;
	bseModel = NULL;
}
