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
//
// Light segments become render lights of the effect's world, updated before each scene (their
// particles evaluated at the scene's time); sound segments play on the game's emitter for the
// effect (its referenceSoundHandle; testEffect allocates its own); decal segments project once.
//
// A segment whose material is lit (Quake 4's model particles: grenades in flight) cannot be lit as
// frame-temporary geometry - light interactions come from an entity's model. Such an effect gets
// a second render entity, its model (idRenderModelBseLit) a snapshot of the lit segments rebuilt
// every frame, as Doom 3's particle models are; it receives lights, casts no shadow.

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
#include "sound/sound.h"
#include "framework/DeclEntityDef.h"
#include "quake4/SoundQ4.h"
#include "quake4/EngineQ4.h"

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
static class idRenderModelBseLit *	bseLitModel = NULL;
static idList<idBseEffect *>	bseEffects;			// every played effect
// testEffect's: no game owner services them; they stop spawning at stopTime (ms) and go once over
typedef struct {
	idBseEffect *	effect;
	int				stopTime;
} bseTestEffect_t;
static idList<bseTestEffect_t>	bseTestEffects;
const int						BSE_TEST_MIN_MSEC = 4000;	// testEffect shows an effect at least this long
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

	sounds and decals (BseEffect.h hooks)

===============================================================================
*/

// the effect's emitter: the game's (renderEffect_t::referenceSoundHandle in its sound world)
static idSoundEmitterQ4 *BSE_SoundEmitter( const idBseEffect *effect ) {
	const int handle = effect->owner.referenceSoundHandle;
	return handle > 0 ? SoundSystemQ4()->EmitterForIndex( SOUNDWORLD_GAME, handle ) : NULL;
}

float BSE_SoundLength( const idSoundShader *shader ) {
	return shader != NULL ? shader->GetTimeLength() : 0.0f;
}

bool BSE_SoundLooping( const idSoundShader *shader ) {
	return shader != NULL && ( shader->GetParms()->soundShaderFlags & SSF_LOOPING ) != 0;
}

void BSE_StartSound( idBseEffect *effect, int channel, const idSoundShader *shader, float diversity, bool looping ) {
	idSoundEmitterQ4 *emitter = BSE_SoundEmitter( effect );
	if ( emitter != NULL && shader != NULL ) {
		emitter->StartSound( shader, (s_channelType)channel, diversity, looping ? SSF_LOOPING : 0 );
	}
}

// the emitter at the effect, with the segment's volume (0: the shader's own) and pitch
void BSE_UpdateSound( idBseEffect *effect, float volume, float freqShift ) {
	idSoundEmitterQ4 *emitter = BSE_SoundEmitter( effect );
	if ( emitter != NULL ) {
		soundShaderParmsQ4_t parms;
		memset( &parms, 0, sizeof( parms ) );
		parms.volume = volume;
		parms.frequencyShift = freqShift;
		emitter->UpdateEmitter( effect->currentOrigin, effect->currentVelocity, 0, &parms );
	}
}

void BSE_StopSound( idBseEffect *effect, int channel ) {
	idSoundEmitterQ4 *emitter = BSE_SoundEmitter( effect );
	if ( emitter != NULL ) {
		emitter->StopSound( (s_channelType)channel );
	}
}

void BSE_ProjectDecal( idBseEffect *effect, const idFixedWinding &winding, const idVec3 &projectionOrigin,
		float depth, const idMaterial *material, int startTimeMs ) {
	if ( effect->world != NULL && material != NULL ) {
		effect->world->ProjectDecalOntoWorld( winding, projectionOrigin, true, depth, material, startTimeMs );
	}
}

/*
===============================================================================

	physics, and requests to the game (BseEffect.h hooks)

	A particle's trace is a query of the game's world (the game's Translation, the call Quake 4's
	effects make), done while effects are serviced - never while a view is being built. What the
	game is asked to do - play an impact, timeout or nested effect, spawn debris, shake or blur the
	player's view - waits in a
	bounded queue for the game's next effect frame bracket (bse->StartFrame / EndFrame), so the
	game spawns its client entities in its own frame and never in the middle of a render.

===============================================================================
*/

// Quake 4's CONTENTS_SOLID | CONTENTS_RENDERMODEL, in the game's numbering (its Material.h): the
// world's solids and the models that clip by their render model
static const int		BSE_TRACE_CONTENTS = BIT( 0 ) | BIT( 11 );

const int				BSE_MAX_EFFECT_REQUESTS = 256;	// waiting effects (one game frame's impacts)
const int				BSE_MAX_DEBRIS_REQUESTS = 128;
const int				BSE_MAX_VIEW_REQUESTS = 64;

typedef enum {
	BSE_REQUEST_EFFECT,				// play an effect once
	BSE_REQUEST_DEBRIS,				// spawn a client moveable
	BSE_REQUEST_VIEW,				// a view effect for the player
	BSE_REQUEST_NUM
} bseRequestKind_t;

typedef struct {
	const idRenderWorld *	world;
	bseRequestKind_t		kind;
	const idDeclEffect *	effect;
	idStr					entityDef;
	idVec3					origin;
	idMat3					axis;
	idVec3					velocity;
	idVec3					angularVelocity;
	int						viewEffect;		// VIEWEFFECT_*
	float					endTime;		// seconds
	float					scale;
} bseRequest_t;

static idList<bseRequest_t>		bseRequests;
static int						bseNumRequests[BSE_REQUEST_NUM];
static const int				bseMaxRequests[BSE_REQUEST_NUM] = { BSE_MAX_EFFECT_REQUESTS, BSE_MAX_DEBRIS_REQUESTS, BSE_MAX_VIEW_REQUESTS };
static int						bseDroppedRequests = 0;
static bool						bseServicingForView = false;	// no traces while a view is built
static int						bseRateRefused = 0;				// bse_debug 2's counters
static int						bseTraces = 0;

bool BSE_Trace( idBseEffect *effect, const idVec3 &start, const idVec3 &end, idVec3 &endPos, idVec3 &normal ) {
	idGameQ4 *game = GameQ4();
	if ( game == NULL || bseServicingForView || !GameQ4_MapActive() || effect->world == NULL ) {
		return false;
	}
	bseTraces++;
	traceQ4_t trace;
	idVec3 source = start, dest = end;
	game->Translation( trace, source, dest, NULL, BSE_TRACE_CONTENTS );
	if ( trace.fraction >= 1.0f || trace.c.normal.LengthSqr() < 0.5f ) {
		return false;		// nothing hit (or a translation too long to trace, which has no contact plane)
	}
	endPos = trace.endpos;
	normal = trace.c.normal;
	return true;
}

bool BSE_CanPlayImpactEffect( void ) {
	return BSE_CanPlayRateLimited( BSE_CATEGORY_IMPACT_PARTICLES );
}

/*
================
rate limits

What retail's effect manager does with bse_rateLimit / bse_rateCost (OpenQ4 src/bse and idTech4A++
raven/bse consulted 2026-10-26; the user chose this reading): each category keeps a budget it spends
0.1 x bse_rateCost per effect (retail's costs: impacts and particle impacts 0.1, the rest free).
Once a category has spent more than half of bse_rateLimit, an effect plays only while its cost plus
what is spent stays under a random fraction of the limit - the closer to the limit, the rarer.
bse_rateLimit 0.1 or less: no limit. The budgets pay back 0.1 per 60 Hz game tic (retail: once per
StartFrame, its game tic) - counted in tics since the last payback rather than in calls, so the
limiter cannot shut on a path that never reaches StartFrame (a multiplayer client, later).
================
*/
static const float		bseEffectCosts[BSE_CATEGORY_NUM] = { 0.0f, 0.1f, 0.1f };
const float				BSE_RATE_PAYBACK = 0.1f;		// per game tic
static float			bseEffectRates[BSE_CATEGORY_NUM];
static int				bseRateTic = -1;				// com_ticNumber at the last payback
static idRandom			bseRateRandom;

void BSE_ResetRateTimes( void ) {
	memset( bseEffectRates, 0, sizeof( bseEffectRates ) );
	bseRateTic = com_ticNumber;
}

void BSE_UpdateRateTimes( void ) {
	const int tic = com_ticNumber;
	if ( bseRateTic < 0 || tic < bseRateTic ) {
		bseRateTic = tic;
		return;
	}
	const int tics = tic - bseRateTic;
	if ( tics == 0 ) {
		return;
	}
	bseRateTic = tic;
	for ( int i = 0; i < BSE_CATEGORY_NUM; i++ ) {
		bseEffectRates[i] = Max( 0.0f, bseEffectRates[i] - BSE_RATE_PAYBACK * tics );
	}
}

bool BSE_CanPlayRateLimited( bseCategory_t category ) {
	if ( category <= BSE_CATEGORY_IGNORE || category >= BSE_CATEGORY_NUM ) {
		return true;
	}
	const float limit = bse_rateLimit != NULL ? bse_rateLimit->GetFloat() : 3.0f;
	if ( limit <= 0.1f ) {
		return true;
	}
	BSE_UpdateRateTimes();
	float &spent = bseEffectRates[category];
	const float cost = bseEffectCosts[category] * ( bse_rateCost != NULL ? bse_rateCost->GetFloat() : 1.0f );
	if ( limit * 0.5f < spent && cost + spent > bseRateRandom.RandomFloat() * limit ) {
		bseRateRefused++;
		return false;
	}
	spent += cost;
	return true;
}

bool BSE_Filtered( const char *name, bseCategory_t category ) {
	if ( name == NULL ) {
		return true;
	}
	const char *single = bse_singleEffect != NULL ? bse_singleEffect->GetString() : "";
	if ( single[0] != '\0' && idStr::FindText( name, single, false ) < 0 ) {
		return true;
	}
	return !BSE_CanPlayRateLimited( category );
}

static bseRequest_t *BSE_NewRequest( idBseEffect *effect, bseRequestKind_t kind ) {
	if ( effect->world == NULL || bseNumRequests[kind] >= bseMaxRequests[kind] ) {
		bseDroppedRequests++;
		return NULL;
	}
	bseNumRequests[kind]++;
	bseRequest_t &r = bseRequests.Alloc();
	r.world = effect->world;
	r.kind = kind;
	r.effect = NULL;
	r.entityDef.Clear();
	r.velocity.Zero();
	r.angularVelocity.Zero();
	return &r;
}

void BSE_RequestEffect( idBseEffect *effect, const idDeclEffect *child, const idVec3 &origin, const idMat3 &axis ) {
	if ( child == NULL || child->GetState() == DS_DEFAULTED ) {
		return;		// a missing child effect plays nothing
	}
	bseRequest_t *r = BSE_NewRequest( effect, BSE_REQUEST_EFFECT );
	if ( r != NULL ) {
		r->effect = child;
		r->origin = origin;
		r->axis = axis;
	}
}

void BSE_RequestDebris( idBseEffect *effect, const char *entityDef, const idVec3 &origin, const idMat3 &axis,
		const idVec3 &velocity, const idVec3 &angularVelocity ) {
	bseRequest_t *r = BSE_NewRequest( effect, BSE_REQUEST_DEBRIS );
	if ( r != NULL ) {
		r->entityDef = entityDef;
		r->origin = origin;
		r->axis = axis;
		r->velocity = velocity;
		r->angularVelocity = angularVelocity;
	}
}

void BSE_RequestViewEffect( idBseEffect *effect, effectSegmentType_t type, float endTime, float scale ) {
	bseRequest_t *r = BSE_NewRequest( effect, BSE_REQUEST_VIEW );
	if ( r != NULL ) {
		r->viewEffect = type == EFFECT_SEG_DOUBLEVISION ? VIEWEFFECT_DOUBLEVISION : type == EFFECT_SEG_TUNNEL ? VIEWEFFECT_TUNNEL : VIEWEFFECT_SHAKE;
		r->endTime = endTime;
		r->scale = scale;
	}
}

/*
================
BSE_DeliverRequests

The game's effect frame bracket: the waiting child effects (played once, no end origin, white),
debris (client moveables with the entity def's own lifetime) and view effects (the game's player
view takes the end time as its finish time) go to the game, in the order asked.
================
*/
void BSE_DeliverRequests( void ) {
	idGameQ4 *game = GameQ4();
	// bse_debug 2: the effects' counters, once a second
	static int lastReport = 0, requested = 0;
	requested += bseRequests.Num();
	const int now = Sys_Milliseconds();
	if ( BSE_Debug() >= 2 && now - lastReport >= 1000 ) {
		int particles = 0;
		for ( int i = 0; i < bseEffects.Num(); i++ ) {
			particles += bseEffects[i]->CountParticles();
		}
		common->Printf( "BSE: %d effects, %d particles; last second: %d traces, %d requests, %d rate-limited\n",
			bseEffects.Num(), particles, bseTraces, requested, bseRateRefused );
		lastReport = now;
		bseTraces = bseRateRefused = requested = 0;
	}
	if ( bseRequests.Num() == 0 && bseDroppedRequests == 0 ) {
		return;
	}
	idList<bseRequest_t> requests;
	requests.Swap( bseRequests );		// what the game plays may ask for more: next bracket
	const int dropped = bseDroppedRequests;
	memset( bseNumRequests, 0, sizeof( bseNumRequests ) );
	bseDroppedRequests = 0;
	if ( BSE_Debug() && dropped > 0 ) {
		common->Printf( "BSE: %d child effect / debris / view effect requests over the limit were dropped\n", dropped );
	}
	if ( game == NULL || !GameQ4_MapActive() ) {
		return;
	}
	const idVec4 white( 1.0f, 1.0f, 1.0f, 1.0f );
	for ( int i = 0; i < requests.Num(); i++ ) {
		const bseRequest_t &r = requests[i];
		switch ( r.kind ) {
			case BSE_REQUEST_EFFECT:
				game->PlayEffect( r.effect, r.origin, r.axis, false, vec3_origin, false, false, EC_IGNORE, white );
				break;
			case BSE_REQUEST_DEBRIS:
				game->SpawnClientMoveable( r.entityDef.c_str(), 0, r.origin, r.axis, r.velocity, r.angularVelocity );
				break;
			default:
				game->StartViewEffect( r.viewEffect, r.endTime, r.scale );
				break;
		}
	}
}

// an entity def an effect names: its media load with the level (as Quake 4's effects precache them)
static void BSE_PrecacheEntityDef( const char *name ) {
	idGameQ4 *game = GameQ4();
	if ( game == NULL || !GameQ4_MapActive() ) {
		return;
	}
	const idDeclEntityDef *def = static_cast<const idDeclEntityDef *>( declManager->FindType( DECL_ENTITYDEF, name, false ) );
	if ( def != NULL ) {
		game->CacheDictionaryMedia( &def->dict );
	}
}

const srfTriangles_t *BSE_ModelGeometry( const idRenderModel *model ) {
	if ( model == NULL || model->NumSurfaces() <= 0 ) {
		return NULL;
	}
	return model->Surface( 0 )->geometry;
}

// what a segment draws with: its particles' material (a model particle's too, not the model's own)
static const idMaterial *BSE_SegmentMaterial( const idEffectSegment *tmpl ) {
	return tmpl->particle.material;
}

// a segment drawn by the effect's lit entity
static bool BSE_SegmentIsLit( const idEffectSegment *tmpl ) {
	if ( !tmpl->enabled || !tmpl->hasParticles ) {
		return false;
	}
	const idMaterial *material = BSE_SegmentMaterial( tmpl );
	return material != NULL && material->ReceivesLighting();
}

static bool BSE_DeclHasLitSegments( const idDeclEffect *decl ) {
	for ( int i = 0; i < decl->GetNumSegments(); i++ ) {
		if ( BSE_SegmentIsLit( &decl->GetSegment( i ) ) ) {
			return true;
		}
	}
	return false;
}

// an effect no owner has updated to the view's time yet (a new one, or testEffect's) runs to it
static void BSE_ServiceForView( idBseEffect *effect, const viewDef_t *view ) {
	if ( view->renderView.time > effect->serviceTime ) {
		effect->UpdateFromOwner( effect->owner, MS2SEC( view->renderView.time ) );
		bseServicingForView = true;		// no traces mid-render: the next service's 0.1 s window covers this one
		effect->finished = effect->Service( MS2SEC( view->renderView.time ) ) || effect->finished;
		bseServicingForView = false;
		effect->serviceTime = view->renderView.time;
		effect->entityDirty = true;
	}
}

// the viewer of view in the frame of an entity at origin, axis
static bseView_t BSE_EffectView( const viewDef_t *view, const idVec3 &origin, const idMat3 &axis ) {
	bseView_t v;
	v.origin = axis.Transpose() * ( view->renderView.vieworg - origin );
	v.left = axis.Transpose() * view->renderView.viewaxis[1];
	v.up = axis.Transpose() * view->renderView.viewaxis[2];
	return v;
}

// building space, reused: geometry is copied out to frame or model memory once built
static idBseGeometry			bseGeometry;

/*
===============================================================================

	the lit model: an effect's lit segments as a snapshot model of its second entity

===============================================================================
*/

class idRenderModelBseLit : public idRenderModelStatic {
public:
	virtual dynamicModel_t		IsDynamicModel() const { return DM_CONTINUOUS; }
	virtual idRenderModel *		InstantiateDynamicModel( const struct renderEntity_s *ent, const struct viewDef_s *view, idRenderModel *cachedModel );
	virtual idBounds			Bounds( const struct renderEntity_s *ent ) const {
									return ent != NULL ? ent->bounds : bounds;
								}
};

idRenderModel *idRenderModelBseLit::InstantiateDynamicModel( const struct renderEntity_s *ent, const struct viewDef_s *view, idRenderModel *cachedModel ) {
	delete cachedModel;
	// a model trace, or no effect: nothing to show
	if ( ent == NULL || view == NULL || ent->callbackData == NULL || !BSE_Render() ) {
		return NULL;
	}
	idBseEffect *effect = static_cast<idBseEffect *>( ent->callbackData );
	if ( bseEffects.FindIndex( effect ) < 0 ) {
		return NULL;
	}
	BSE_ServiceForView( effect, view );

	idRenderModelStatic *snapshot = new idRenderModelStatic;
	snapshot->InitEmpty( "_BSE_LIT_SNAPSHOT" );
	const bseView_t viewer = BSE_EffectView( view, ent->origin, ent->axis );
	const float time = MS2SEC( view->renderView.time );
	idBounds bounds;
	bounds.Clear();
	for ( int i = 0; i < effect->segments.Num(); i++ ) {
		const idBseSegment &segment = effect->segments[i];
		if ( segment.particles.Num() == 0 || !BSE_SegmentIsLit( segment.tmpl ) ) {
			continue;
		}
		const idMaterial *material = BSE_SegmentMaterial( segment.tmpl );
		if ( !material->IsDrawn() ) {
			continue;
		}
		bseGeometry.Clear();
		effect->BuildSegment( segment, time, viewer, bseGeometry );
		if ( bseGeometry.NumIndexes() == 0 ) {
			continue;
		}
		srfTriangles_t *tri = R_AllocStaticTriSurf();
		R_AllocStaticTriSurfVerts( tri, bseGeometry.NumVerts() );
		R_AllocStaticTriSurfIndexes( tri, bseGeometry.NumIndexes() );
		memcpy( tri->verts, bseGeometry.verts.Ptr(), bseGeometry.NumVerts() * sizeof( idDrawVert ) );
		memcpy( tri->indexes, bseGeometry.indexes.Ptr(), bseGeometry.NumIndexes() * sizeof( glIndex_t ) );
		tri->numVerts = bseGeometry.NumVerts();
		tri->numIndexes = bseGeometry.NumIndexes();
		tri->bounds = bseGeometry.bounds;
		tri->tangentsCalculated = true;		// the model's own, turned with it
		tri->facePlanesCalculated = false;	// derived when a light needs them
		modelSurface_t surf;
		surf.id = i;
		surf.shader = material;
		surf.geometry = tri;
		snapshot->AddSurface( surf );
		bounds.AddBounds( tri->bounds );
	}
	snapshot->bounds = bounds;
	return snapshot;
}

/*
===============================================================================

	effects

===============================================================================
*/

// a light segment's render light, if its world still has it
static bool BSE_HasLight( const idBseEffect *effect, int handle ) {
	const idRenderWorldLocal *world = static_cast<const idRenderWorldLocal *>( effect->world );
	return world != NULL && handle >= 0 && handle < world->lightDefs.Num() && world->lightDefs[handle] != NULL;
}

static void BSE_FreeLights( idBseEffect *effect ) {
	for ( int i = 0; i < effect->segments.Num(); i++ ) {
		idBseSegment &segment = effect->segments[i];
		if ( segment.lightHandle >= 0 && BSE_HasLight( effect, segment.lightHandle ) ) {
			effect->world->FreeLightDef( segment.lightHandle );
		}
		segment.lightHandle = -1;
	}
}

/*
================
BSE_SyncLights

An effect's light segments as render lights at the scene's time: added when their particle is
born, moved, resized and recoloured while it lives, freed when it dies (or effects are not drawn).
Updated every scene, not only after a service, so they move smoothly between game tics.
================
*/
static void BSE_SyncLights( idBseEffect *effect, float time ) {
	for ( int i = 0; i < effect->segments.Num(); i++ ) {
		idBseSegment &segment = effect->segments[i];
		if ( segment.tmpl->type != EFFECT_SEG_LIGHT ) {
			continue;
		}
		bseLight_t light;
		if ( BSE_Render() && effect->EvaluateLight( segment, time, light ) ) {
			renderLight_t rl;
			memset( &rl, 0, sizeof( rl ) );
			rl.origin = light.origin;
			rl.axis = light.axis;
			rl.lightRadius = light.radius;
			rl.pointLight = true;
			rl.noShadows = !light.shadows;
			rl.noSpecular = !light.specular;
			rl.shader = light.material;
			rl.shaderParms[SHADERPARM_RED] = light.color.x;
			rl.shaderParms[SHADERPARM_GREEN] = light.color.y;
			rl.shaderParms[SHADERPARM_BLUE] = light.color.z;
			if ( segment.lightHandle >= 0 && BSE_HasLight( effect, segment.lightHandle ) ) {
				effect->world->UpdateLightDef( segment.lightHandle, &rl );
			} else {
				segment.lightHandle = effect->world->AddLightDef( &rl );
			}
		} else if ( segment.lightHandle >= 0 ) {
			if ( BSE_HasLight( effect, segment.lightHandle ) ) {
				effect->world->FreeLightDef( segment.lightHandle );
			}
			segment.lightHandle = -1;
		}
	}
}

// one of the effect's render entities, if it is still the effect's (a map load frees every entity)
static idRenderEntityLocal *BSE_EntityDef( const idBseEffect *effect, int handle, const idRenderModel *model ) {
	idRenderWorldLocal *world = static_cast<idRenderWorldLocal *>( effect->world );
	if ( world == NULL || handle < 0 || handle >= world->entityDefs.Num() ) {
		return NULL;
	}
	idRenderEntityLocal *def = world->entityDefs[handle];
	if ( def == NULL || def->parms.hModel != model || def->parms.callbackData != effect ) {
		return NULL;
	}
	return def;
}

static void BSE_FillEntity( const idBseEffect *effect, renderEntity_t &ent, bool lit ) {
	const bseEffectParms_t &parms = effect->owner;
	memset( &ent, 0, sizeof( ent ) );
	ent.hModel = lit ? static_cast<idRenderModel *>( bseLitModel ) : static_cast<idRenderModel *>( bseModel );
	ent.callbackData = const_cast<idBseEffect *>( effect );
	ent.origin = effect->currentOrigin;
	ent.axis = effect->currentAxis;
	ent.bounds = effect->localBounds;
	if ( !effect->particleBounds.IsCleared() ) {
		ent.bounds.AddBounds( effect->particleBounds );
	}
	ent.noShadow = true;
	ent.noSelfShadow = true;
	ent.noDynamicInteractions = !lit;
	ent.suppressSurfaceInViewID = parms.suppressSurfaceInViewID;
	ent.allowSurfaceInViewID = parms.allowSurfaceInViewID;
	ent.weaponDepthHack = parms.weaponDepthHack;
	ent.modelDepthHack = parms.modelDepthHack;
	memcpy( ent.shaderParms, parms.shaderParms, sizeof( ent.shaderParms ) );
}

static void BSE_SyncEntity( idBseEffect *effect ) {
	renderEntity_t ent;
	if ( BSE_EntityDef( effect, effect->entityHandle, bseModel ) != NULL ) {
		BSE_FillEntity( effect, ent, false );
		effect->world->UpdateEntityDef( effect->entityHandle, &ent );
	}
	if ( BSE_EntityDef( effect, effect->litHandle, bseLitModel ) != NULL ) {
		BSE_FillEntity( effect, ent, true );
		effect->world->UpdateEntityDef( effect->litHandle, &ent );
	}
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
	// bse_singleEffect: only the effects it names play, whoever plays them (as retail's PlayEffect)
	if ( BSE_Filtered( parms.effect->GetName(), BSE_CATEGORY_IGNORE ) ) {
		return NULL;
	}
	BSE_ToolsEffectPlayed( parms.effect );
	idBseEffect *effect = new idBseEffect;
	effect->world = world;
	effect->entityHandle = -1;
	effect->litHandle = -1;
	effect->entityDirty = false;
	effect->serviceTime = timeMs - 1;		// the frame's first view services it
	effect->Init( parms, MS2SEC( timeMs ) );
	if ( BSE_Debug() ) {
		static int plays = 0;
		common->Printf( "BSE: Play %d: %s at %g\n", ++plays, parms.effect->GetName(), MS2SEC( timeMs ) );
	}

	renderEntity_t ent;
	BSE_FillEntity( effect, ent, false );
	effect->entityHandle = world->AddEntityDef( &ent );
	if ( BSE_DeclHasLitSegments( parms.effect ) ) {
		BSE_FillEntity( effect, ent, true );
		effect->litHandle = world->AddEntityDef( &ent );
	}
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
	effect->entityDirty = true;		// its entities follow before the next scene (BSE_SyncWorld)
	if ( effect->finished && BSE_Debug() ) {
		common->Printf( "BSE: '%s' is over\n", effect->decl->GetName() );
	}
	return effect->finished;
}

/*
================
BSE_SyncWorld

The entities of world's effects that were serviced since their last update, with their particles'
bounds: done once before a scene renders rather than every game tic, so game tics that render
nothing (a skipped cinematic runs a minute of them in one frame) pay only for the simulation.
First, testEffect's effects run to the scene's time (timeMs): no game owner services them.
================
*/
void BSE_SyncWorld( idRenderWorld *world, int timeMs ) {
	BSE_SweepFrame( world, timeMs );		// bseSweep's next batch, when one is running and due

	// testEffect's run to the scene's time (no game owner does), stop on time and go once over
	for ( int i = 0; i < bseTestEffects.Num(); i++ ) {
		idBseEffect *effect = bseTestEffects[i].effect;
		if ( effect->world != world ) {
			continue;
		}
		if ( !effect->stopped && timeMs >= bseTestEffects[i].stopTime ) {
			BSE_StopEffect( effect );
		}
		if ( BSE_UpdateEffect( effect, effect->owner, timeMs ) ) {
			BSE_FreeEffect( effect );		// drops it from bseTestEffects
			i--;
		}
	}
	const float time = MS2SEC( timeMs );
	for ( int i = 0; i < bseEffects.Num(); i++ ) {
		idBseEffect *effect = bseEffects[i];
		if ( effect->world != world ) {
			continue;
		}
		if ( effect->entityDirty ) {
			effect->UpdateBounds();
			BSE_SyncEntity( effect );
			effect->entityDirty = false;
		}
		BSE_SyncLights( effect, time );
		// bse_showBounds: the effect's bounds (yellow: what the renderer culls it by), 2: and its live
		// particles' (cyan)
		const int showBounds = bse_showBounds != NULL ? bse_showBounds->GetInteger() : 0;
		if ( showBounds > 0 ) {
			world->DebugBox( colorYellow, idBox( effect->localBounds, effect->currentOrigin, effect->currentAxis ) );
			if ( showBounds > 1 && !effect->particleBounds.IsCleared() ) {
				world->DebugBox( colorCyan, idBox( effect->particleBounds, effect->currentOrigin, effect->currentAxis ) );
			}
		}
	}
}

void BSE_StopEffect( idBseEffect *effect ) {
	if ( effect != NULL ) {
		effect->Stop();
		if ( BSE_Debug() ) {
			common->Printf( "BSE: Stop: %s\n", effect->decl->GetName() );
		}
	}
}

void BSE_FreeEffect( idBseEffect *effect ) {
	if ( effect == NULL ) {
		return;
	}
	if ( BSE_Debug() ) {
		common->Printf( "BSE: Free: %s\n", effect->decl->GetName() );
	}
	BSE_ToolsEffectFreed( effect );
	if ( BSE_EntityDef( effect, effect->entityHandle, bseModel ) != NULL ) {
		effect->world->FreeEntityDef( effect->entityHandle );
	}
	if ( BSE_EntityDef( effect, effect->litHandle, bseLitModel ) != NULL ) {
		effect->world->FreeEntityDef( effect->litHandle );
	}
	BSE_FreeLights( effect );
	bseEffects.Remove( effect );
	for ( int i = 0; i < bseTestEffects.Num(); i++ ) {
		if ( bseTestEffects[i].effect == effect ) {
			// testEffect's own emitter: its sounds play out
			if ( effect->owner.referenceSoundHandle > 0 ) {
				SoundSystemQ4()->FreeSoundEmitter( SOUNDWORLD_GAME, effect->owner.referenceSoundHandle, false );
			}
			bseTestEffects.RemoveIndex( i-- );
		}
	}
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

// geometry as a frame-temporary draw surface of the entity
static void R_AddBseGeometry( viewEntity_t *vEntity, const idMaterial *material, const idBseGeometry &geo, bool soft = false ) {
	if ( geo.NumIndexes() == 0 ) {
		return;
	}
	idRenderEntityLocal *def = vEntity->entityDef;
	srfTriangles_t *tri = (srfTriangles_t *)R_ClearedFrameAlloc( sizeof( *tri ) );
	tri->numVerts = geo.NumVerts();
	tri->numIndexes = geo.NumIndexes();
	tri->verts = (idDrawVert *)R_FrameAlloc( tri->numVerts * sizeof( idDrawVert ) );
	tri->indexes = (glIndex_t *)R_FrameAlloc( tri->numIndexes * sizeof( glIndex_t ) );
	memcpy( tri->verts, geo.verts.Ptr(), tri->numVerts * sizeof( idDrawVert ) );
	memcpy( tri->indexes, geo.indexes.Ptr(), tri->numIndexes * sizeof( glIndex_t ) );
	tri->bounds = geo.bounds;
	tri->ambientCache = vertexCache.AllocFrameTemp( tri->verts, tri->numVerts * sizeof( idDrawVert ) );
	if ( tri->ambientCache == NULL ) {
		return;
	}
	// DUDE bse_softParticles (opt-in): sprites and oriented quads fade against the scene's depth, as
	// Doom 3's particles do with r_useSoftParticles, over the segment's average particle size
	float softRadius = -1.0f;
	if ( soft && geo.sizeCount > 0 && bse_softParticles != NULL && bse_softParticles->GetBool() ) {
		softRadius = Max( 1.0f, geo.sizeSum / geo.sizeCount );
	}
	R_AddDrawSurf( tri, vEntity, &def->parms, material, vEntity->scissorRect, softRadius );
}

/*
================
R_AddBseEffectSurfaces

One effect's particles for this view: per drawn segment, in file order, a frame-temporary
surface of its particles, then one of their motion trails. Lit segments are the lit entity's.
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

	BSE_ServiceForView( effect, view );
	if ( !BSE_Render() ) {
		return;
	}

	const bseView_t viewer = BSE_EffectView( view, def->parms.origin, def->parms.axis );
	const float time = MS2SEC( view->renderView.time );
	for ( int i = 0; i < effect->segments.Num(); i++ ) {
		const idBseSegment &segment = effect->segments[i];
		const idEffectSegment *tmpl = segment.tmpl;
		if ( !tmpl->enabled || !tmpl->hasParticles || segment.particles.Num() == 0 ) {
			continue;
		}
		const idMaterial *material = BSE_SegmentMaterial( tmpl );
		if ( material != NULL && material->IsDrawn() && !material->ReceivesLighting() ) {
			bseGeometry.Clear();
			effect->BuildSegment( segment, time, viewer, bseGeometry );
			R_AddBseGeometry( vEntity, material, bseGeometry, true );
		}
		const idMaterial *trailMaterial = tmpl->particle.trailMaterial;
		if ( tmpl->particle.trailType == EFFECT_TRAIL_MOTION && trailMaterial != NULL && trailMaterial->IsDrawn() ) {
			bseGeometry.Clear();
			effect->BuildMotionTrails( segment, time, viewer, bseGeometry );
			R_AddBseGeometry( vEntity, trailMaterial, bseGeometry );
		}
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

const idList<idBseEffect *> &BSE_PlayingEffects( void ) {
	return bseEffects;
}

const renderView_t *BSE_ShownView( const idRenderWorld *world ) {
	return BSE_WorldView( world );
}

void BSE_ClearTestEffects( void ) {
	while ( bseTestEffects.Num() > 0 ) {
		BSE_FreeEffect( bseTestEffects[0].effect );
	}
}

/*
================
BSE_TestEffect_f

testEffect <effect> [once]: plays an effect 80 units in front of the view, facing it - where the
game's "spawn func_fx fx <effect>" puts one, so the two compare - looping unless "once", for 4
seconds or its whole play when that is longer; then it stops and its particles die out. A beam
(an effect reaching to an end origin) ends 256 units to the viewer's right; the end is printed,
for func_fx's "endOrigin". An effect with a decal (an impact) goes on the surface in view, facing out
of it. testEffect alone removes it at once.
================
*/
static void BSE_TestEffect_f( const idCmdArgs &args ) {
	BSE_ClearTestEffects();
	if ( args.Argc() < 2 ) {
		common->Printf( "usage: testEffect <effect> [once]   (shown for 4 s or its whole play; no effect: remove it now)\n" );
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
	const bool loop = !( args.Argc() > 2 && !idStr::Icmp( args.Argv( 2 ), "once" ) );
	const int showMsec = Max( BSE_TEST_MIN_MSEC, (int)idMath::Ceil( decl->GetMaxDuration() * 1000.0f ) );
	if ( BSE_StartTestEffect( decl, *shown, loop, vec2_origin, showMsec, true ) == NULL ) {
		common->Printf( "testEffect: '%s' does not play (bse_enabled? bse_singleEffect?)\n", decl->GetName() );
	}
}

/*
================
BSE_StartTestEffect

testEffect's play: 80 units in front of the view facing it (offset along the view's right and up
axes), where the game's "spawn func_fx" puts one; an effect with a decal on the surface in view,
facing out of it; a beam ending 256 units to the viewer's right; a sound emitter of its own.
On show for showMsec (then it stops spawning and its particles die out).
================
*/
idBseEffect *BSE_StartTestEffect( const idDeclEffect *decl, const renderView_t &view, bool loop, const idVec2 &offset,
		int showMsec, bool verbose ) {
	idRenderWorld *world = session != NULL ? session->rw : NULL;
	if ( world == NULL || decl == NULL ) {
		return NULL;
	}
	const float yaw = view.viewaxis[0].ToYaw();
	bseEffectParms_t parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.effect = decl;
	parms.startTime = MS2SEC( view.time );
	parms.origin = view.vieworg + idAngles( 0.0f, yaw, 0.0f ).ToForward() * 80.0f - view.viewaxis[1] * offset.x + view.viewaxis[2] * offset.y;
	parms.axis = idAngles( 0.0f, yaw + 180.0f, 0.0f ).ToMat3();
	parms.gravity.Set( 0.0f, 0.0f, -BSE_GameGravity() );
	parms.attenuation = 1.0f;
	parms.loop = loop;
	parms.inConnectedArea = true;
	// a sound emitter of its own (a game effect's comes from the game)
	parms.referenceSoundHandle = decl->HasSound() ? SoundSystemQ4()->AllocSoundEmitter( SOUNDWORLD_GAME ) : -1;
	parms.shaderParms[SHADERPARM_RED] = parms.shaderParms[SHADERPARM_GREEN] = parms.shaderParms[SHADERPARM_BLUE] = parms.shaderParms[SHADERPARM_ALPHA] = 1.0f;
	parms.shaderParms[6] = 1.0f;		// brightness
	parms.shaderParms[SHADERPARM_TIMEOFFSET] = MS2SEC( view.time );
	// an effect leaving a decal (an impact) goes on the surface in view, facing out of it, as the game
	// plays impacts: its decal reaches only 8 units into what is behind it
	bool decal = false;
	for ( int i = 0; i < decl->GetNumSegments(); i++ ) {
		decal = decal || decl->GetSegment( i ).type == EFFECT_SEG_DECAL;
	}
	if ( decal ) {
		modelTrace_t trace;
		// along the view (testEffect), or towards a placement off the view's centre (bseSweep)
		const idVec3 dir = offset == vec2_origin ? view.viewaxis[0] : ( parms.origin - view.vieworg ).ToNormal();
		if ( world->Trace( trace, view.vieworg, view.vieworg + dir * 2048.0f, 0.0f, true ) ) {
			parms.origin = trace.point + trace.normal;
			parms.axis = trace.normal.ToMat3();
			if ( verbose ) {
				common->Printf( "testEffect: on the surface in view at %s\n", parms.origin.ToString( 0 ) );
			}
		} else if ( verbose ) {
			common->Printf( "testEffect: no surface in view for its decal; played in the air\n" );
		}
	}
	if ( decl->UsesEndOrigin() ) {
		parms.hasEndOrigin = true;
		parms.endOrigin = parms.origin - view.viewaxis[1] * 256.0f;
		if ( verbose ) {
			common->Printf( "testEffect: beam from %s to endOrigin \"%s\"\n", parms.origin.ToString( 0 ), parms.endOrigin.ToString( 0 ) );
		}
	}

	idBseEffect *effect = BSE_PlayEffect( world, parms, view.time );
	if ( effect == NULL ) {
		if ( parms.referenceSoundHandle > 0 ) {
			SoundSystemQ4()->FreeSoundEmitter( SOUNDWORLD_GAME, parms.referenceSoundHandle, true );
		}
		return NULL;
	}
	bseTestEffect_t &test = bseTestEffects.Alloc();
	test.effect = effect;
	test.stopTime = view.time + showMsec;
	if ( verbose ) {
		common->Printf( "testEffect: '%s'%s, %d segments, %g s per play, radius %g; shown for %g s\n", decl->GetName(),
			parms.loop ? " looping" : " once", decl->GetNumSegments(), decl->GetMaxDuration(), effect->boundsRadius, MS2SEC( showMsec ) );
	}
	return effect;
}

bool BSE_TestEffectsPlaying( void ) {
	return bseTestEffects.Num() > 0;
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
		bseLitModel = new idRenderModelBseLit;
		bseLitModel->InitEmpty( "_BSE_EFFECT_LIT" );
	}
	bseRandom.SetSeed( 0x5eed );

	// Quake 4 only: the other titles have no effect decls (and keep their cvar list as it was)
	if ( GameTitle::Get().title != GAME_TITLE_QUAKE4 ) {
		return;
	}
	if ( bse_enabled == NULL ) {
		// created now that the title is known (idCVar registers itself once the static list is in)
		// the help texts are retail's (its executable's strings), with DUDE's notes in brackets
		bse_enabled			= new idCVar( "bse_enabled", "1", CVAR_RENDERER | CVAR_BOOL, "set to false to disable all effects" );
		bse_render			= new idCVar( "bse_render", "1", CVAR_RENDERER | CVAR_BOOL, "disable effect rendering" );
		bse_debug			= new idCVar( "bse_debug", "0", CVAR_RENDERER | CVAR_INTEGER,
								"display debug info about effect [0 off; 1 on: plays, loops, stops, frees; 2 extra: and once a second the effects, particles, traces, requests and rate-limited effects]", 0.0f, 2.0f );
		bse_showBounds		= new idCVar( "bse_showBounds", "0", CVAR_RENDERER | CVAR_INTEGER,
								"display debug bounding boxes effect [1: each effect's bounds, yellow; 2: and its live particles', cyan]", 0.0f, 2.0f );
		bse_physics			= new idCVar( "bse_physics", "1", CVAR_RENDERER | CVAR_BOOL, "disable effect physics" );
		bse_debris			= new idCVar( "bse_debris", "1", CVAR_RENDERER | CVAR_BOOL, "disable effect debris" );
		bse_scale			= new idCVar( "bse_scale", "1", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
								"effect scalability amount [0-1: emitters spawn between their lowest and highest count by it, and a segment shows from its detail level up]", 0.0f, 1.0f );
		bse_singleEffect	= new idCVar( "bse_singleEffect", "", CVAR_RENDERER,
								"set to the name of the effect that is only played [any effect whose name contains it, case ignored]", idCmdSystem::ArgCompletion_Decl<DECL_EFFECT> );
		bse_maxParticles	= new idCVar( "bse_maxParticles", "2048", CVAR_RENDERER | CVAR_INTEGER, "max number of particles allowed per segment", 0.0f, 2048.0f );
		bse_rateLimit		= new idCVar( "bse_rateLimit", "3.0", CVAR_RENDERER | CVAR_FLOAT,
								"rate limit for spawned effects [what impact effects may spend at once; 0.1 or less: no limit]" );
		bse_rateCost		= new idCVar( "bse_rateCost", "1", CVAR_RENDERER | CVAR_FLOAT, "rate cost multiplier for spawned effects" );
		bse_softParticles	= new idCVar( "bse_softParticles", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL,
								"DUDE: effect sprites fade where they meet the scene, like Doom 3's soft particles (non-vanilla; GL3/Vulkan with r_useSoftParticles' depth capture)" );
	}
	BSE_ResetRateTimes();
	BSE_InitTools();
	effectPrecacheEntityDef = BSE_PrecacheEntityDef;
	cmdSystem->AddCommand( "testEffect", BSE_TestEffect_f, CMD_FL_RENDERER | CMD_FL_CHEAT,
		"Quake 4: plays an effect in front of the view for 4 s or its whole play, as 'spawn func_fx fx <effect>' does - testEffect <effect> [once]",
		idCmdSystem::ArgCompletion_Decl<DECL_EFFECT> );
}

void BSE_WorldFreed( idRenderWorld *world ) {
	for ( int i = 0; i < bseEffects.Num(); i++ ) {
		if ( bseEffects[i]->world == world ) {
			bseEffects[i]->world = NULL;
			bseEffects[i]->entityHandle = -1;
			bseEffects[i]->litHandle = -1;
			for ( int j = 0; j < bseEffects[i]->segments.Num(); j++ ) {
				bseEffects[i]->segments[j].lightHandle = -1;		// the world's lights went with it
			}
		}
	}
	for ( int i = 0; i < bseWorldViews.Num(); i++ ) {
		if ( bseWorldViews[i].world == world ) {
			bseWorldViews.RemoveIndex( i-- );
		}
	}
	for ( int i = 0; i < bseRequests.Num(); i++ ) {
		if ( bseRequests[i].world == world ) {
			bseNumRequests[bseRequests[i].kind]--;
			bseRequests.RemoveIndex( i-- );
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
	bseRequests.Clear();
	memset( bseNumRequests, 0, sizeof( bseNumRequests ) );
	bseDroppedRequests = 0;
	effectPrecacheEntityDef = NULL;
	cmdSystem->RemoveCommand( "testEffect" );
	BSE_ShutdownTools();
	bseWorldViews.Clear();
	delete bseModel;
	bseModel = NULL;
	delete bseLitModel;
	bseLitModel = NULL;
	bseGeometry.verts.Clear();
	bseGeometry.indexes.Clear();
}
