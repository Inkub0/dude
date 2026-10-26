/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: the Quake 4 game's render world (quake4/RenderWorldQ4.h) over an engine render world.
// Entities keep the game's own renderEntityQ4_t (GetRenderEntity hands it back, a deferred
// callback is called with it) and reach the renderer converted; the engine calls a trampoline in
// place of the game's callback, which calls the game's on its own copy and converts the result.
// Sound handles become the engine's emitters. Quake 4's portal sky view (RF_PORTAL_SKY) is the
// renderer's portal sky subview (idRenderWorld::SetPortalSkyCamera), as for Doom 3's RoE.
//
// Hidden surfaces (suppressSurfaceMask) go to the engine entity: an MD5 model leaves them out when
// posed, a static model is drawn from a copy without them (R_EntityDefDynamicModel); the ray traced
// tiers' shared model BLASes still hold a static model's hidden surfaces - unseen in the retail
// game, which hides surfaces on MD5 models only (whose posed model the RT tiers use). An
// overlayShader is a second engine entity drawing the model with it.
//
// Effects (Raven's BSE) play in the engine's effect runtime (renderer/bse/BseEffect.h): a handle
// is the game's view of one played effect. Phase Q4 S2: sprites draw; the other particle kinds,
// sounds, lights, decals, physics and view effects come with S3-S6.
//
// Not carried over yet - accepted and dropped, the renderer has no slot for them:
// shadowLODDistance / suppressLOD, a light's detailLevel,
// noDynamicShadows and globalLight; the view weapon's depth hack applies in every view, not just
// weaponDepthHackInViewID's (effects too).

#include "sys/platform.h"
#include "framework/Common.h"
#include "framework/DeclQuake4.h"
#include "renderer/Material.h"
#include "renderer/tr_local.h"
#include "renderer/RenderWorld_local.h"
#include "renderer/bse/BseEffect.h"

#include "quake4/EngineQ4.h"

// a played effect, as the game holds it (opaque to the game)
class idRenderEffectQ4 {
public:
	idBseEffect *			effect;
	const idDeclEffect *	decl;
};

/*
===============================================================================

	idRenderWorldQ4Local

===============================================================================
*/

class idRenderWorldQ4Local : public idRenderWorldQ4 {
public:
							idRenderWorldQ4Local( idRenderWorld *renderWorld );
	virtual					~idRenderWorldQ4Local( void );

	virtual qhandle_t		AddEntityDef( const renderEntityQ4_t *re );
	virtual void			UpdateEntityDef( qhandle_t entityHandle, const renderEntityQ4_t *re );
	virtual void			FreeEntityDef( qhandle_t entityHandle );
	virtual const renderEntityQ4_t *GetRenderEntity( qhandle_t entityHandle ) const;

	virtual qhandle_t		AddLightDef( const renderLightQ4_t *rlight );
	virtual void			UpdateLightDef( qhandle_t lightHandle, const renderLightQ4_t *rlight );
	virtual void			FreeLightDef( qhandle_t lightHandle );

	virtual qhandle_t		AddEffectDef( const renderEffect_t *reffect, int time );
	virtual bool			UpdateEffectDef( qhandle_t effectHandle, const renderEffect_t *reffect, int time );
	virtual void			StopEffectDef( qhandle_t effectHandle );
	virtual const idRenderEffectQ4 *GetEffectDef( qhandle_t effectHandle ) const;
	virtual void			FreeEffectDef( qhandle_t effectHandle );
	virtual bool			EffectDefHasSound( const renderEffect_t *reffect );

	virtual void			ProjectDecalOntoWorld( const idFixedWinding &winding, const idVec3 &projectionOrigin, const bool parallel, const float fadeDepth, const idMaterial *material, const int startTime );
	virtual void			ProjectOverlay( qhandle_t entityHandle, const idPlane localTextureAxis[2], const idMaterial *material );
	virtual void			RemoveDecals( qhandle_t entityHandle );

	virtual void			SetRenderView( const renderView_t *renderView );
	virtual void			RenderScene( const renderView_t *renderView, int renderFlags );
	virtual bool			HasSkybox( int areaNum );
	virtual void			FindVisibleAreas( idVec3 origin, int areaNum, bool *visibleAreas );

	virtual int				NumPortals( void ) const;
	virtual qhandle_t		FindPortal( const idBounds &b ) const;
	virtual void			SetPortalState( qhandle_t portal, int blockingBits );
	virtual int				GetPortalState( qhandle_t portal );
	virtual bool			AreasAreConnected( int areaNum1, int areaNum2, portalConnection_t connection );
	virtual int				NumAreas( void ) const;
	virtual int				PointInArea( const idVec3 &point ) const;
	virtual int				BoundsInAreas( const idBounds &bounds, int *areas, int maxAreas ) const;
	virtual int				NumPortalsInArea( int areaNum );
	virtual exitPortal_t	GetPortal( int areaNum, int portalNum );

	virtual guiPoint_t		GuiTrace( qhandle_t entityHandle, const idVec3 start, const idVec3 end ) const;
	virtual bool			ModelTrace( modelTraceQ4_t &trace, qhandle_t entityHandle, const idVec3 &start, const idVec3 &end, const float radius ) const;
	virtual bool			FastWorldTrace( modelTraceQ4_t &trace, const idVec3 &start, const idVec3 &end ) const;

	virtual void			DebugClear( int time );
	virtual void			DebugLine( const idVec4 &color, const idVec3 &start, const idVec3 &end, const int lifetime, const bool depthTest );
	virtual void			DebugArrow( const idVec4 &color, const idVec3 &start, const idVec3 &end, int size, const int lifetime );
	virtual void			DebugWinding( const idVec4 &color, const idWinding &w, const idVec3 &origin, const idMat3 &axis, const int lifetime, const bool depthTest );
	virtual void			DebugCircle( const idVec4 &color, const idVec3 &origin, const idVec3 &dir, const float radius, const int numSteps, const int lifetime, const bool depthTest );
	virtual void			DebugBounds( const idVec4 &color, const idBounds &bounds, const idVec3 &org, const int lifetime, bool depthTest );
	virtual void			DebugBox( const idVec4 &color, const idBox &box, const int lifetime );
	virtual void			DebugFrustum( const idVec4 &color, const idFrustum &frustum, const bool showFromOrigin, const int lifetime );
	virtual void			DebugCone( const idVec4 &color, const idVec3 &apex, const idVec3 &dir, float radius1, float radius2, const int lifetime );
	virtual void			DebugAxis( const idVec3 &origin, const idMat3 &axis );
	virtual void			DebugPolygon( const idVec4 &color, const idWinding &winding, const int lifeTime, const bool depthTest );
	virtual void			DebugFOV( const idVec4 &color, const idVec3 &origin, const idVec3 &dir, float farDot, float farDist, float nearDot, float nearDist, float alpha, int lifetime );
	virtual void			DrawText( const char *text, const idVec3 &origin, float scale, const idVec4 &color, const idMat3 &viewAxis, const int align, const int lifetime, bool depthTest );

	idRenderWorld *			renderWorld;

private:
	// a game entity: the game's own description, and the engine handle it lives under
	struct entityQ4_t {
		renderEntityQ4_t	parms;
		qhandle_t			overlayHandle;		// the engine entity drawing parms.overlayShader, -1 for none
	};

	entityQ4_t *			Entity( qhandle_t entityHandle ) const;
	static void				ToEngine( entityQ4_t *ent, renderEntity_t &out );
	static void				ToEngineOverlay( entityQ4_t *ent, renderEntity_t &out );
	void					SyncEngine( qhandle_t entityHandle, entityQ4_t *ent );

	static bool				EntityCallback( renderEntity_s *engineEntity, const renderView_s *view );
	static bool				OverlayCallback( renderEntity_s *engineEntity, const renderView_s *view );

	idList<entityQ4_t *>	entities;			// by engine entity handle
	idList<idRenderEffectQ4 *>	effects;		// by effect handle; NULL: free

	bool					portalSkyPending;	// the game rendered its portal sky view this frame
	idVec3					portalSkyOrigin;
};

idRenderWorldQ4Local::idRenderWorldQ4Local( idRenderWorld *renderWorld ) :
	renderWorld( renderWorld ),
	portalSkyPending( false ),
	portalSkyOrigin( vec3_origin ) {
}

idRenderWorldQ4Local::~idRenderWorldQ4Local( void ) {
	entities.DeleteContents( true );
	for ( int i = 0; i < effects.Num(); i++ ) {
		if ( effects[i] != NULL ) {
			BSE_FreeEffect( effects[i]->effect );
		}
	}
	effects.DeleteContents( true );
}

/*
===============
idRenderWorldQ4Local::Entity
===============
*/
idRenderWorldQ4Local::entityQ4_t *idRenderWorldQ4Local::Entity( qhandle_t entityHandle ) const {
	if ( entityHandle < 0 || entityHandle >= entities.Num() ) {
		return NULL;
	}
	return entities[entityHandle];
}

/*
===============
idRenderWorldQ4Local::ToEngine

The engine's render entity for a game entity: the trampoline stands in for the game's callback.
===============
*/
void idRenderWorldQ4Local::ToEngine( entityQ4_t *ent, renderEntity_t &out ) {
	R_RenderEntityQ4ToEngine( ent->parms, out );
	if ( ent->parms.callback != NULL ) {
		out.callback = EntityCallback;
		out.callbackData = ent;
	}
	out.referenceSound = SoundEmitterForHandleQ4( ent->parms.referenceSoundHandle );
}

/*
===============
idRenderWorldQ4Local::EntityCallback

The engine asks for a deferred entity's model: the game's callback updates the game's copy, and
the engine's is made again from it.
===============
*/
bool idRenderWorldQ4Local::EntityCallback( renderEntity_s *engineEntity, const renderView_s *view ) {
	entityQ4_t *ent = static_cast<entityQ4_t *>( engineEntity->callbackData );
	if ( ent == NULL || ent->parms.callback == NULL ) {
		return false;
	}
	const bool changed = ent->parms.callback( &ent->parms, view );
	if ( changed ) {
		// the game may have dropped its callback (the entity is no longer deferred)
		const deferredEntityCallback_t engineCallback = ent->parms.callback != NULL ? EntityCallback : NULL;
		R_RenderEntityQ4ToEngine( ent->parms, *engineEntity );
		engineEntity->callback = engineCallback;
		engineEntity->callbackData = ent;
		engineEntity->referenceSound = SoundEmitterForHandleQ4( ent->parms.referenceSoundHandle );
	}
	return changed;
}

/*
===============
idRenderWorldQ4Local::ToEngineOverlay

Quake 4 draws an entity's overlayShader (powerup shells) as the model again over itself with
that material; here that is a second engine entity, posed from the same joints, casting no shadow.
===============
*/
void idRenderWorldQ4Local::ToEngineOverlay( entityQ4_t *ent, renderEntity_t &out ) {
	R_RenderEntityQ4ToEngine( ent->parms, out );
	out.customShader = ent->parms.overlayShader;
	out.customSkin = NULL;
	out.noShadow = true;
	out.referenceSound = SoundEmitterForHandleQ4( ent->parms.referenceSoundHandle );
	if ( ent->parms.callback != NULL ) {
		// the game's callback runs for the entity itself; the overlay follows what it left
		out.callback = OverlayCallback;
		out.callbackData = ent;
	}
}

bool idRenderWorldQ4Local::OverlayCallback( renderEntity_s *engineEntity, const renderView_s *view ) {
	entityQ4_t *ent = static_cast<entityQ4_t *>( engineEntity->callbackData );
	if ( ent == NULL || ent->parms.hModel == NULL ) {
		return false;
	}
	ToEngineOverlay( ent, *engineEntity );
	return true;
}

/*
===============
idRenderWorldQ4Local::SyncEngine

After the engine entity is added or updated: its hidden surfaces, and the overlay entity.
===============
*/
void idRenderWorldQ4Local::SyncEngine( qhandle_t entityHandle, entityQ4_t *ent ) {
	idRenderWorldLocal *world = static_cast<idRenderWorldLocal *>( renderWorld );
	if ( entityHandle >= 0 && entityHandle < world->entityDefs.Num() && world->entityDefs[entityHandle] != NULL ) {
		world->entityDefs[entityHandle]->suppressSurfaceMask = ent->parms.suppressSurfaceMask;
	}

	const bool wantOverlay = ent->parms.overlayShader != NULL && ( ent->parms.hModel != NULL || ent->parms.callback != NULL );
	if ( wantOverlay ) {
		renderEntity_t overlay;
		ToEngineOverlay( ent, overlay );
		if ( ent->overlayHandle < 0 ) {
			ent->overlayHandle = renderWorld->AddEntityDef( &overlay );
		} else {
			renderWorld->UpdateEntityDef( ent->overlayHandle, &overlay );
		}
		if ( ent->overlayHandle >= 0 && ent->overlayHandle < world->entityDefs.Num() && world->entityDefs[ent->overlayHandle] != NULL ) {
			world->entityDefs[ent->overlayHandle]->suppressSurfaceMask = ent->parms.suppressSurfaceMask;
		}
	} else if ( ent->overlayHandle >= 0 ) {
		renderWorld->FreeEntityDef( ent->overlayHandle );
		ent->overlayHandle = -1;
	}
}

/*
===============
entities
===============
*/
qhandle_t idRenderWorldQ4Local::AddEntityDef( const renderEntityQ4_t *re ) {
	entityQ4_t *ent = new entityQ4_t;
	ent->parms = *re;
	ent->overlayHandle = -1;

	renderEntity_t engineEntity;
	ToEngine( ent, engineEntity );
	const qhandle_t handle = renderWorld->AddEntityDef( &engineEntity );
	if ( handle < 0 ) {
		delete ent;
		return handle;
	}

	if ( handle >= entities.Num() ) {
		const int oldNum = entities.Num();
		entities.SetNum( handle + 1 );
		for ( int i = oldNum; i < entities.Num(); i++ ) {
			entities[i] = NULL;
		}
	}
	if ( entities[handle] != NULL ) {
		// a stale game entity under a recycled handle (never expected)
		if ( entities[handle]->overlayHandle >= 0 ) {
			renderWorld->FreeEntityDef( entities[handle]->overlayHandle );
		}
		delete entities[handle];
	}
	entities[handle] = ent;
	SyncEngine( handle, ent );
	return handle;
}

void idRenderWorldQ4Local::UpdateEntityDef( qhandle_t entityHandle, const renderEntityQ4_t *re ) {
	if ( entityHandle < 0 ) {
		return;
	}
	if ( entityHandle >= entities.Num() ) {
		const int oldNum = entities.Num();
		entities.SetNum( entityHandle + 1 );
		for ( int i = oldNum; i < entities.Num(); i++ ) {
			entities[i] = NULL;
		}
	}
	entityQ4_t *ent = entities[entityHandle];
	if ( ent == NULL ) {
		ent = entities[entityHandle] = new entityQ4_t;
		ent->overlayHandle = -1;
	}
	ent->parms = *re;

	renderEntity_t engineEntity;
	ToEngine( ent, engineEntity );
	renderWorld->UpdateEntityDef( entityHandle, &engineEntity );
	SyncEngine( entityHandle, ent );
}

void idRenderWorldQ4Local::FreeEntityDef( qhandle_t entityHandle ) {
	renderWorld->FreeEntityDef( entityHandle );
	entityQ4_t *ent = Entity( entityHandle );
	if ( ent != NULL ) {
		if ( ent->overlayHandle >= 0 ) {
			renderWorld->FreeEntityDef( ent->overlayHandle );
		}
		delete ent;
		entities[entityHandle] = NULL;
	}
}

const renderEntityQ4_t *idRenderWorldQ4Local::GetRenderEntity( qhandle_t entityHandle ) const {
	const entityQ4_t *ent = Entity( entityHandle );
	return ent != NULL ? &ent->parms : NULL;
}

/*
===============
lights
===============
*/
void R_RenderLightQ4ToEngine( const renderLightQ4_t &src, renderLight_t &dst ) {
	memset( &dst, 0, sizeof( dst ) );
	dst.axis = src.axis;
	dst.origin = src.origin;
	dst.suppressLightInViewID = src.suppressLightInViewID;
	dst.allowLightInViewID = src.allowLightInViewID;
	dst.noShadows = src.noShadows;
	dst.noSpecular = src.noSpecular;
	dst.pointLight = src.pointLight;
	dst.parallel = src.parallel;
	dst.lightRadius = src.lightRadius;
	dst.lightCenter = src.lightCenter;
	dst.target = src.target;
	dst.right = src.right;
	dst.up = src.up;
	dst.start = src.start;
	dst.end = src.end;
	dst.prelightModel = src.prelightModel;
	dst.lightId = src.lightId;
	dst.shader = src.shader;
	memcpy( dst.shaderParms, src.shaderParms, sizeof( dst.shaderParms ) );
	dst.referenceSound = SoundEmitterForHandleQ4( src.referenceSoundHandle );
}

qhandle_t idRenderWorldQ4Local::AddLightDef( const renderLightQ4_t *rlight ) {
	renderLight_t engineLight;
	R_RenderLightQ4ToEngine( *rlight, engineLight );
	return renderWorld->AddLightDef( &engineLight );
}

void idRenderWorldQ4Local::UpdateLightDef( qhandle_t lightHandle, const renderLightQ4_t *rlight ) {
	renderLight_t engineLight;
	R_RenderLightQ4ToEngine( *rlight, engineLight );
	renderWorld->UpdateLightDef( lightHandle, &engineLight );
}

void idRenderWorldQ4Local::FreeLightDef( qhandle_t lightHandle ) {
	renderWorld->FreeLightDef( lightHandle );
}

/*
===============
effects
===============
*/
static const idDeclEffect *EffectDeclQ4( const renderEffect_t *reffect ) {
	if ( reffect == NULL || reffect->declEffect == NULL || reffect->declEffect->GetType() != DECL_EFFECT ) {
		return NULL;
	}
	return static_cast<const idDeclEffect *>( reffect->declEffect );
}

// the game's description of a played effect as the effect runtime's
static void R_RenderEffectQ4ToBse( const renderEffect_t &src, const idDeclEffect *decl, bseEffectParms_t &dst ) {
	memset( &dst, 0, sizeof( dst ) );
	dst.effect = decl;
	dst.startTime = src.startTime;
	dst.origin = src.origin;
	dst.axis = src.axis;
	dst.gravity = src.gravity;
	dst.endOrigin = src.endOrigin;
	dst.hasEndOrigin = src.hasEndOrigin;
	dst.attenuation = src.attenuation;
	dst.loop = src.loop;
	dst.ambient = src.ambient;
	dst.inConnectedArea = src.inConnectedArea;
	dst.suppressSurfaceInViewID = src.suppressSurfaceInViewID;
	dst.allowSurfaceInViewID = src.allowSurfaceInViewID;
	dst.weaponDepthHack = src.weaponDepthHackInViewID != 0;
	dst.modelDepthHack = src.modelDepthHack;
	dst.referenceSoundHandle = src.referenceSoundHandle;
	memcpy( dst.shaderParms, src.shaderParms, sizeof( dst.shaderParms ) );
}

qhandle_t idRenderWorldQ4Local::AddEffectDef( const renderEffect_t *reffect, int time ) {
	const idDeclEffect *decl = EffectDeclQ4( reffect );
	if ( decl == NULL ) {
		return -1;
	}
	bseEffectParms_t parms;
	R_RenderEffectQ4ToBse( *reffect, decl, parms );
	idBseEffect *effect = BSE_PlayEffect( renderWorld, parms, time );
	if ( effect == NULL ) {
		return -1;
	}
	int handle = effects.FindNull();
	if ( handle < 0 ) {
		handle = effects.Append( NULL );
	}
	effects[handle] = new idRenderEffectQ4;
	effects[handle]->effect = effect;
	effects[handle]->decl = decl;
	return handle;
}

bool idRenderWorldQ4Local::UpdateEffectDef( qhandle_t effectHandle, const renderEffect_t *reffect, int time ) {
	if ( effectHandle < 0 || effectHandle >= effects.Num() || effects[effectHandle] == NULL ) {
		return true;
	}
	idRenderEffectQ4 *def = effects[effectHandle];
	bseEffectParms_t parms;
	R_RenderEffectQ4ToBse( *reffect, def->decl, parms );
	return BSE_UpdateEffect( def->effect, parms, time );
}

void idRenderWorldQ4Local::StopEffectDef( qhandle_t effectHandle ) {
	if ( effectHandle >= 0 && effectHandle < effects.Num() && effects[effectHandle] != NULL ) {
		BSE_StopEffect( effects[effectHandle]->effect );
	}
}

const idRenderEffectQ4 *idRenderWorldQ4Local::GetEffectDef( qhandle_t effectHandle ) const {
	if ( effectHandle < 0 || effectHandle >= effects.Num() ) {
		return NULL;
	}
	return effects[effectHandle];
}

void idRenderWorldQ4Local::FreeEffectDef( qhandle_t effectHandle ) {
	if ( effectHandle < 0 || effectHandle >= effects.Num() || effects[effectHandle] == NULL ) {
		return;
	}
	BSE_FreeEffect( effects[effectHandle]->effect );
	delete effects[effectHandle];
	effects[effectHandle] = NULL;
}

// whether the game should give the effect a sound emitter: it has sound segments
bool idRenderWorldQ4Local::EffectDefHasSound( const renderEffect_t *reffect ) {
	const idDeclEffect *decl = EffectDeclQ4( reffect );
	return decl != NULL && decl->HasSound();
}

/*
===============
decals and overlays
===============
*/
void idRenderWorldQ4Local::ProjectDecalOntoWorld( const idFixedWinding &winding, const idVec3 &projectionOrigin, const bool parallel, const float fadeDepth, const idMaterial *material, const int startTime ) {
	renderWorld->ProjectDecalOntoWorld( winding, projectionOrigin, parallel, fadeDepth, material, startTime );
}

void idRenderWorldQ4Local::ProjectOverlay( qhandle_t entityHandle, const idPlane localTextureAxis[2], const idMaterial *material ) {
	renderWorld->ProjectOverlay( entityHandle, localTextureAxis, material );
}

void idRenderWorldQ4Local::RemoveDecals( qhandle_t entityHandle ) {
	renderWorld->RemoveDecals( entityHandle );
}

/*
===============
views
===============
*/
void idRenderWorldQ4Local::SetRenderView( const renderView_t *renderView ) {
	renderWorld->SetRenderView( renderView );
}

/*
===============
idRenderWorldQ4Local::RenderScene

The game renders its portal sky camera's view just before the view it shows behind (PlayerView
SingleView): that view becomes the renderer's portal sky subview of the next one.
===============
*/
void idRenderWorldQ4Local::RenderScene( const renderView_t *renderView, int renderFlags ) {
	if ( renderFlags & RF_PORTAL_SKY ) {
		portalSkyPending = true;
		portalSkyOrigin = renderView->vieworg;
		return;
	}
	renderWorld->SetPortalSkyCamera( portalSkyPending, portalSkyOrigin );
	portalSkyPending = false;
	renderWorld->RenderScene( renderView );
}

/*
===============
idRenderWorldQ4Local::HasSkybox

The area shows the portal sky by the map's own per-area data (.proc), not RoE's
CheckAreaForPortalSky (entities whose materials ask for the sky): the game renders its sky
camera only when a visible area answers true.
===============
*/
bool idRenderWorldQ4Local::HasSkybox( int areaNum ) {
	return static_cast<idRenderWorldLocal *>( renderWorld )->AreaShowsSky( areaNum );
}

/*
===============
idRenderWorldQ4Local::FindVisibleAreas

The areas reachable from areaNum through portals that do not block the view. A superset of what
the point sees; the game only asks whether any of them shows sky, and the renderer decides that
for itself per view.
===============
*/
void idRenderWorldQ4Local::FindVisibleAreas( idVec3 origin, int areaNum, bool *visibleAreas ) {
	const int numAreas = renderWorld->NumAreas();
	if ( areaNum < 0 || areaNum >= numAreas ) {
		return;
	}
	for ( int i = 0; i < numAreas; i++ ) {
		if ( i == areaNum || renderWorld->AreasAreConnected( areaNum, i, PS_BLOCK_VIEW ) ) {
			visibleAreas[i] = true;
		}
	}
}

/*
===============
portals and areas
===============
*/
int idRenderWorldQ4Local::NumPortals( void ) const {
	return renderWorld->NumPortals();
}

qhandle_t idRenderWorldQ4Local::FindPortal( const idBounds &b ) const {
	return renderWorld->FindPortal( b );
}

void idRenderWorldQ4Local::SetPortalState( qhandle_t portal, int blockingBits ) {
	renderWorld->SetPortalState( portal, blockingBits );
}

int idRenderWorldQ4Local::GetPortalState( qhandle_t portal ) {
	return renderWorld->GetPortalState( portal );
}

bool idRenderWorldQ4Local::AreasAreConnected( int areaNum1, int areaNum2, portalConnection_t connection ) {
	return renderWorld->AreasAreConnected( areaNum1, areaNum2, connection );
}

int idRenderWorldQ4Local::NumAreas( void ) const {
	return renderWorld->NumAreas();
}

int idRenderWorldQ4Local::PointInArea( const idVec3 &point ) const {
	return renderWorld->PointInArea( point );
}

// Quake 4's boxes can pass Doom 3's 10000-unit check (animations spanning a map): retail takes them
int idRenderWorldQ4Local::BoundsInAreas( const idBounds &bounds, int *areas, int maxAreas ) const {
	return static_cast<idRenderWorldLocal *>( renderWorld )->BoundsInAreasAnySize( bounds, areas, maxAreas );
}

int idRenderWorldQ4Local::NumPortalsInArea( int areaNum ) {
	return renderWorld->NumPortalsInArea( areaNum );
}

exitPortal_t idRenderWorldQ4Local::GetPortal( int areaNum, int portalNum ) {
	return renderWorld->GetPortal( areaNum, portalNum );
}

/*
===============
traces
===============
*/
static const idDeclMaterialType *MaterialTypeQ4( const idMaterial *material ) {
	return material != NULL ? static_cast<const idDeclMaterialType *>( material->GetMaterialType() ) : NULL;
}

guiPoint_t idRenderWorldQ4Local::GuiTrace( qhandle_t entityHandle, const idVec3 start, const idVec3 end ) const {
	return renderWorld->GuiTrace( entityHandle, start, end );
}

bool idRenderWorldQ4Local::ModelTrace( modelTraceQ4_t &trace, qhandle_t entityHandle, const idVec3 &start, const idVec3 &end, const float radius ) const {
	modelTrace_t engineTrace;
	const bool hit = renderWorld->ModelTrace( engineTrace, entityHandle, start, end, radius );
	trace.fraction = engineTrace.fraction;
	trace.point = engineTrace.point;
	trace.normal = engineTrace.normal;
	trace.material = engineTrace.material;
	trace.entity = hit ? GetRenderEntity( entityHandle ) : NULL;
	trace.jointNumber = engineTrace.jointNumber;
	trace.materialType = hit ? MaterialTypeQ4( engineTrace.material ) : NULL;
	return hit;
}

bool idRenderWorldQ4Local::FastWorldTrace( modelTraceQ4_t &trace, const idVec3 &start, const idVec3 &end ) const {
	modelTrace_t engineTrace;
	const bool hit = renderWorld->FastWorldTrace( engineTrace, start, end );
	trace.fraction = engineTrace.fraction;
	trace.point = engineTrace.point;
	trace.normal = engineTrace.normal;
	trace.material = engineTrace.material;
	trace.entity = NULL;			// the world's own surfaces, no game entity
	trace.jointNumber = engineTrace.jointNumber;
	trace.materialType = hit ? MaterialTypeQ4( engineTrace.material ) : NULL;
	return hit;
}

/*
===============
debug drawing
===============
*/
void idRenderWorldQ4Local::DebugClear( int time ) {
	renderWorld->DebugClearLines( time );
	renderWorld->DebugClearPolygons( time );
}

void idRenderWorldQ4Local::DebugLine( const idVec4 &color, const idVec3 &start, const idVec3 &end, const int lifetime, const bool depthTest ) {
	renderWorld->DebugLine( color, start, end, lifetime, depthTest );
}

void idRenderWorldQ4Local::DebugArrow( const idVec4 &color, const idVec3 &start, const idVec3 &end, int size, const int lifetime ) {
	renderWorld->DebugArrow( color, start, end, size, lifetime );
}

void idRenderWorldQ4Local::DebugWinding( const idVec4 &color, const idWinding &w, const idVec3 &origin, const idMat3 &axis, const int lifetime, const bool depthTest ) {
	renderWorld->DebugWinding( color, w, origin, axis, lifetime, depthTest );
}

void idRenderWorldQ4Local::DebugCircle( const idVec4 &color, const idVec3 &origin, const idVec3 &dir, const float radius, const int numSteps, const int lifetime, const bool depthTest ) {
	renderWorld->DebugCircle( color, origin, dir, radius, numSteps, lifetime, depthTest );
}

void idRenderWorldQ4Local::DebugBounds( const idVec4 &color, const idBounds &bounds, const idVec3 &org, const int lifetime, bool depthTest ) {
	renderWorld->DebugBounds( color, bounds, org, lifetime );
}

void idRenderWorldQ4Local::DebugBox( const idVec4 &color, const idBox &box, const int lifetime ) {
	renderWorld->DebugBox( color, box, lifetime );
}

void idRenderWorldQ4Local::DebugFrustum( const idVec4 &color, const idFrustum &frustum, const bool showFromOrigin, const int lifetime ) {
	renderWorld->DebugFrustum( color, frustum, showFromOrigin, lifetime );
}

void idRenderWorldQ4Local::DebugCone( const idVec4 &color, const idVec3 &apex, const idVec3 &dir, float radius1, float radius2, const int lifetime ) {
	renderWorld->DebugCone( color, apex, dir, radius1, radius2, lifetime );
}

void idRenderWorldQ4Local::DebugAxis( const idVec3 &origin, const idMat3 &axis ) {
	renderWorld->DebugAxis( origin, axis );
}

void idRenderWorldQ4Local::DebugPolygon( const idVec4 &color, const idWinding &winding, const int lifeTime, const bool depthTest ) {
	renderWorld->DebugPolygon( color, winding, lifeTime, depthTest );
}

/*
===============
idRenderWorldQ4Local::DebugFOV

A field of view as the cone between its near and far distances, drawn as lines (the alpha of
Raven's filled version is not kept).
===============
*/
void idRenderWorldQ4Local::DebugFOV( const idVec4 &color, const idVec3 &origin, const idVec3 &dir, float farDot, float farDist, float nearDot, float nearDist, float alpha, int lifetime ) {
	const float farRadius = farDist * idMath::Sqrt( idMath::ClampFloat( 0.0f, 1.0f, 1.0f - farDot * farDot ) ) / idMath::ClampFloat( 0.01f, 1.0f, farDot );
	const float nearRadius = nearDist * idMath::Sqrt( idMath::ClampFloat( 0.0f, 1.0f, 1.0f - nearDot * nearDot ) ) / idMath::ClampFloat( 0.01f, 1.0f, nearDot );
	renderWorld->DebugCone( color, origin + dir * nearDist, dir * ( farDist - nearDist ), nearRadius, farRadius, lifetime );
}

void idRenderWorldQ4Local::DrawText( const char *text, const idVec3 &origin, float scale, const idVec4 &color, const idMat3 &viewAxis, const int align, const int lifetime, bool depthTest ) {
	renderWorld->DrawText( text, origin, scale, color, viewAxis, align, lifetime, depthTest );
}

/*
===============================================================================

	access

===============================================================================
*/

static idList<idRenderWorldQ4Local *> renderWorldsQ4;

idRenderWorldQ4 *RenderWorldQ4( idRenderWorld *renderWorld ) {
	if ( renderWorld == NULL ) {
		return NULL;
	}
	for ( int i = 0; i < renderWorldsQ4.Num(); i++ ) {
		if ( renderWorldsQ4[i]->renderWorld == renderWorld ) {
			return renderWorldsQ4[i];
		}
	}
	idRenderWorldQ4Local *world = new idRenderWorldQ4Local( renderWorld );
	renderWorldsQ4.Append( world );
	return world;
}

void FreeRenderWorldsQ4( void ) {
	renderWorldsQ4.DeleteContents( true );
}

/*
===============================================================================

	idEffectManagerQ4Local - the game's questions to the effect system. Filtering and rate
	limits come with phase Q4 S7 (bse_singleEffect, bse_rateLimit): until then every effect plays.

===============================================================================
*/

class idEffectManagerQ4Local : public idEffectManagerQ4 {
public:
	virtual bool			Filtered( const char *name, effectCategory_t category ) { return false; }
	virtual bool			CanPlayRateLimited( effectCategory_t category ) { return true; }
	virtual float			EffectDuration( const idRenderEffectQ4 *def ) {
								if ( def == NULL ) {
									return 0.0f;
								}
								return def->effect != NULL ? BSE_EffectDuration( def->effect ) : BSE_DeclDuration( def->decl );
							}
	virtual void			StartFrame( void ) {}
	virtual void			EndFrame( void ) {}
};

idEffectManagerQ4 *EffectManagerQ4( void ) {
	static idEffectManagerQ4Local effectManagerQ4Local;
	return &effectManagerQ4Local;
}
