/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: the Quake 4 game library's render world (docs/quake4-port.md 5.6). Quake 4's render
// entities and lights carry more than Doom 3's - surfaces hidden by mask, an overlay material,
// a depth hack limited to one view, shadow LOD, a light detail level and global lights, sounds
// as handles - so they are their own types here, which a facade converts to the engine's. Its
// views are Doom 3's renderView_t. It adds effect defs (Raven's BSE) to the world.

#ifndef __RENDERWORLDQ4_H__
#define __RENDERWORLDQ4_H__

#include "renderer/RenderWorld.h"
#include "renderer/Model.h"

class idDeclMaterialType;
class idRenderEffectQ4;		// an effect def, as the effect system runs it (opaque to the game)

const int SHADERPARM_BRIGHTNESS		= 6;		// an effect's overall brightness

// Quake 4's material sorts past Doom 3's (idMaterial::SetSort): before everything, between the
// subviews and the GUIs (Doom 3 numbers those -3 and -2, Quake 4 -4 and -2), after everything
const float SS_MIN					= -10000.0f;
const float SS_PREGUI				= -2.5f;
const float SS_MAX					= 10000.0f;
#define DEFAULT_LIGHT_DETAIL_LEVEL	10.0f		// renderLightQ4_t::detailLevel

// RenderScene's renderFlags
enum {
	RF_NORMAL					= 0,
	RF_IS_EDITOR				= BIT( 0 ),
	RF_IS_FULLSCREEN_2D			= BIT( 1 ),
	RF_NO_GUI					= BIT( 2 ),		// the world without its GUI surfaces
	RF_GUI_ONLY					= BIT( 3 ),		// only the GUI surfaces
	RF_PENUMBRA_MAP				= BIT( 4 ),
	RF_DEFER_COMMAND_SUBMIT		= BIT( 5 ),
	RF_PORTAL_SKY				= BIT( 6 ),		// the portal sky's view
	RF_PRIMARY_VIEW				= BIT( 7 )		// the player's view
};

struct renderEntityQ4_s;
typedef bool ( *deferredEntityCallbackQ4_t )( struct renderEntityQ4_s *, const renderView_t * );

typedef struct renderEntityQ4_s {
	idRenderModel *				hModel;					// NULL only with a callback
	int							entityNum;
	int							bodyId;
	idBounds					bounds;					// for deferred models and MD5s
	deferredEntityCallbackQ4_t	callback;				// creates the model when it is first seen
	void *						callbackData;
	int							suppressSurfaceInViewID;
	int							suppressShadowInViewID;
	int							suppressShadowInLightID;
	int							allowSurfaceInViewID;
	int							suppressSurfaceMask;	// surfaces not drawn (idRenderModel::GetSurfaceMask)
	idVec3						origin;
	idMat3						axis;
	const idMaterial *			customShader;			// every surface drawn with this
	const idMaterial *			referenceShader;		// the light a flare refers to
	const idMaterial *			overlayShader;			// the model drawn again over itself with this (powerups)
	const idDeclSkin *			customSkin;
	int							referenceSoundHandle;	// SOUNDWORLD_GAME emitter for sound-driven shaders
	float						shaderParms[ MAX_ENTITY_SHADER_PARMS ];
	class idUserInterface *		gui[ MAX_RENDERENTITY_GUI ];
	struct renderView_s *		remoteRenderView;		// remote camera surfaces show this view
	int							numJoints;
	idJointMat *				joints;					// the joints that move the vertices
	float						modelDepthHack;			// squashed depth so effects don't clip into walls
	bool						noSelfShadow;			// shadows only on other objects
	bool						noShadow;
	bool						noDynamicInteractions;	// no new light interactions after the level loads
	bool						forceUpdate;
	int							weaponDepthHackInViewID;// squashed depth in this view (the view weapon)
	float						shadowLODDistance;		// past it, no shadow
	int							suppressLOD;
} renderEntityQ4_t;

typedef struct renderLightQ4_s {
	idMat3						axis;					// unit length
	idVec3						origin;
	int							suppressLightInViewID;
	int							allowLightInViewID;
	float						detailLevel;			// DEFAULT_LIGHT_DETAIL_LEVEL; lower ones drop with r_lightDetailLevel
	bool						noShadows;
	bool						noSpecular;
	bool						noDynamicShadows;		// only the static shadows
	bool						pointLight;				// otherwise projected
	bool						parallel;				// lightCenter is the direction to the light at infinity
	bool						globalLight;
	idVec3						lightRadius;			// point light
	idVec3						lightCenter;
	idVec3						target;					// projected light
	idVec3						right;
	idVec3						up;
	idVec3						start;
	idVec3						end;
	idRenderModel *				prelightModel;
	int							lightId;
	const idMaterial *			shader;					// NULL: the default point or projected light
	float						shaderParms[ MAX_ENTITY_SHADER_PARMS ];
	int							referenceSoundHandle;	// SOUNDWORLD_GAME emitter for sound-driven shaders
} renderLightQ4_t;

// a played effect
typedef struct renderEffect_s {
	const idDecl *				declEffect;				// DECL_EFFECT
	float						startTime;
	int							suppressSurfaceInViewID;
	int							allowSurfaceInViewID;
	int							groupID;
	idVec3						origin;
	idMat3						axis;
	idVec3						gravity;
	idVec3						endOrigin;				// a beam's end, when hasEndOrigin
	float						attenuation;
	bool						hasEndOrigin;
	bool						loop;
	bool						ambient;				// from an entity, not an event
	bool						inConnectedArea;
	int							weaponDepthHackInViewID;
	float						modelDepthHack;
	int							referenceSoundHandle;
	float						shaderParms[ MAX_ENTITY_SHADER_PARMS ];
} renderEffect_t;

typedef struct modelTraceQ4_s {
	float						fraction;
	idVec3						point;					// end point, world space
	idVec3						normal;					// of the triangle hit, world space
	const idMaterial *			material;
	const renderEntityQ4_t *	entity;
	int							jointNumber;			// the MD5 joint nearest the triangle hit
	const idDeclMaterialType *	materialType;
} modelTraceQ4_t;

/*
===============================================================================

	A Quake 4 render entity as the engine's (Doom 3's) render entity: what the render world facade
	gives the renderer, and what the game hands a model when it asks it for its bounds or a posed
	instance. The callback, the sound handle and Quake 4's own fields are the facade's business
	(left empty here); a view weapon's depth hack becomes Doom 3's whole-entity one.

===============================================================================
*/

ID_INLINE void R_RenderEntityQ4ToEngine( const renderEntityQ4_t &src, renderEntity_t &dst ) {
	memset( &dst, 0, sizeof( dst ) );
	dst.hModel = src.hModel;
	dst.entityNum = src.entityNum;
	dst.bodyId = src.bodyId;
	dst.bounds = src.bounds;
	dst.callbackData = src.callbackData;
	dst.suppressSurfaceInViewID = src.suppressSurfaceInViewID;
	dst.suppressShadowInViewID = src.suppressShadowInViewID;
	dst.suppressShadowInLightID = src.suppressShadowInLightID;
	dst.allowSurfaceInViewID = src.allowSurfaceInViewID;
	dst.origin = src.origin;
	dst.axis = src.axis;
	dst.customShader = src.customShader;
	dst.referenceShader = src.referenceShader;
	dst.customSkin = src.customSkin;
	memcpy( dst.shaderParms, src.shaderParms, sizeof( dst.shaderParms ) );
	for ( int i = 0; i < MAX_RENDERENTITY_GUI; i++ ) {
		dst.gui[i] = src.gui[i];
	}
	dst.remoteRenderView = src.remoteRenderView;
	dst.numJoints = src.numJoints;
	dst.joints = src.joints;
	dst.modelDepthHack = src.modelDepthHack;
	dst.noSelfShadow = src.noSelfShadow;
	dst.noShadow = src.noShadow;
	dst.noDynamicInteractions = src.noDynamicInteractions;
	dst.weaponDepthHack = src.weaponDepthHackInViewID != 0;
	dst.forceUpdate = src.forceUpdate;
}

// a model's bounds for a Quake 4 render entity (dynamic models size themselves from it)
ID_INLINE idBounds R_ModelBoundsQ4( const idRenderModel *model, const renderEntityQ4_t *re ) {
	if ( re == NULL ) {
		return model->Bounds( NULL );
	}
	renderEntity_t engineEntity;
	R_RenderEntityQ4ToEngine( *re, engineEntity );
	return model->Bounds( &engineEntity );
}

// a model posed for a Quake 4 render entity (the game's gibs and animation tools); the caller
// frees it
ID_INLINE idRenderModel *R_InstantiateDynamicModelQ4( idRenderModel *model, const renderEntityQ4_t *re ) {
	renderEntity_t engineEntity;
	R_RenderEntityQ4ToEngine( *re, engineEntity );
	return model->InstantiateDynamicModel( &engineEntity, NULL, NULL );
}

class idRenderWorldQ4 {
public:
	virtual					~idRenderWorldQ4( void ) {}

	// entities, lights and effects
	virtual qhandle_t		AddEntityDef( const renderEntityQ4_t *re ) = 0;
	virtual void			UpdateEntityDef( qhandle_t entityHandle, const renderEntityQ4_t *re ) = 0;
	virtual void			FreeEntityDef( qhandle_t entityHandle ) = 0;
	virtual const renderEntityQ4_t *GetRenderEntity( qhandle_t entityHandle ) const = 0;

	virtual qhandle_t		AddLightDef( const renderLightQ4_t *rlight ) = 0;
	virtual void			UpdateLightDef( qhandle_t lightHandle, const renderLightQ4_t *rlight ) = 0;
	virtual void			FreeLightDef( qhandle_t lightHandle ) = 0;

							// -1 when the effect cannot play (the game then drops it)
	virtual qhandle_t		AddEffectDef( const renderEffect_t *reffect, int time ) = 0;
							// true when the effect has finished (the game then frees it)
	virtual bool			UpdateEffectDef( qhandle_t effectHandle, const renderEffect_t *reffect, int time ) = 0;
	virtual void			StopEffectDef( qhandle_t effectHandle ) = 0;		// stops emitting, lets it die out
	virtual const idRenderEffectQ4 *GetEffectDef( qhandle_t effectHandle ) const = 0;
	virtual void			FreeEffectDef( qhandle_t effectHandle ) = 0;
	virtual bool			EffectDefHasSound( const renderEffect_t *reffect ) = 0;

	// decals and overlays
	virtual void			ProjectDecalOntoWorld( const idFixedWinding &winding, const idVec3 &projectionOrigin, const bool parallel, const float fadeDepth, const idMaterial *material, const int startTime ) = 0;
	virtual void			ProjectOverlay( qhandle_t entityHandle, const idPlane localTextureAxis[2], const idMaterial *material ) = 0;
	virtual void			RemoveDecals( qhandle_t entityHandle ) = 0;

	// views
	virtual void			SetRenderView( const renderView_t *renderView ) = 0;	// for the sound system and portal sky
	virtual void			RenderScene( const renderView_t *renderView, int renderFlags = RF_NORMAL ) = 0;
	virtual bool			HasSkybox( int areaNum ) = 0;
							// which areas a point in areaNum sees (visibleAreas: one per area)
	virtual void			FindVisibleAreas( idVec3 origin, int areaNum, bool *visibleAreas ) = 0;

	// portals and areas
	virtual int				NumPortals( void ) const = 0;
	virtual qhandle_t		FindPortal( const idBounds &b ) const = 0;
	virtual void			SetPortalState( qhandle_t portal, int blockingBits ) = 0;
	virtual int				GetPortalState( qhandle_t portal ) = 0;
	virtual bool			AreasAreConnected( int areaNum1, int areaNum2, portalConnection_t connection ) = 0;
	virtual int				NumAreas( void ) const = 0;
	virtual int				PointInArea( const idVec3 &point ) const = 0;
	virtual int				BoundsInAreas( const idBounds &bounds, int *areas, int maxAreas ) const = 0;
	virtual int				NumPortalsInArea( int areaNum ) = 0;
	virtual exitPortal_t	GetPortal( int areaNum, int portalNum ) = 0;

	// traces
	virtual guiPoint_t		GuiTrace( qhandle_t entityHandle, const idVec3 start, const idVec3 end ) const = 0;
	virtual bool			ModelTrace( modelTraceQ4_t &trace, qhandle_t entityHandle, const idVec3 &start, const idVec3 &end, const float radius ) const = 0;
							// against the world model only
	virtual bool			FastWorldTrace( modelTraceQ4_t &trace, const idVec3 &start, const idVec3 &end ) const = 0;

	// debug drawing
	virtual void			DebugClear( int time ) = 0;		// 0 clears every line and text
	virtual void			DebugLine( const idVec4 &color, const idVec3 &start, const idVec3 &end, const int lifetime = 0, const bool depthTest = false ) = 0;
	virtual void			DebugArrow( const idVec4 &color, const idVec3 &start, const idVec3 &end, int size, const int lifetime = 0 ) = 0;
	virtual void			DebugWinding( const idVec4 &color, const idWinding &w, const idVec3 &origin, const idMat3 &axis, const int lifetime = 0, const bool depthTest = false ) = 0;
	virtual void			DebugCircle( const idVec4 &color, const idVec3 &origin, const idVec3 &dir, const float radius, const int numSteps, const int lifetime = 0, const bool depthTest = false ) = 0;
	virtual void			DebugBounds( const idVec4 &color, const idBounds &bounds, const idVec3 &org = vec3_origin, const int lifetime = 0, bool depthTest = false ) = 0;
	virtual void			DebugBox( const idVec4 &color, const idBox &box, const int lifetime = 0 ) = 0;
	virtual void			DebugFrustum( const idVec4 &color, const idFrustum &frustum, const bool showFromOrigin = false, const int lifetime = 0 ) = 0;
	virtual void			DebugCone( const idVec4 &color, const idVec3 &apex, const idVec3 &dir, float radius1, float radius2, const int lifetime = 0 ) = 0;
	virtual void			DebugAxis( const idVec3 &origin, const idMat3 &axis ) = 0;
	virtual void			DebugPolygon( const idVec4 &color, const idWinding &winding, const int lifeTime = 0, const bool depthTest = false ) = 0;
							// a field of view cone between the near and far dot/distance
	virtual void			DebugFOV( const idVec4 &color, const idVec3 &origin, const idVec3 &dir, float farDot, float farDist, float nearDot = 1.0f, float nearDist = 0.0f, float alpha = 0.3f, int lifetime = 0 ) = 0;
	virtual void			DrawText( const char *text, const idVec3 &origin, float scale, const idVec4 &color, const idMat3 &viewAxis, const int align = 1, const int lifetime = 0, bool depthTest = false ) = 0;
};

#endif /* !__RENDERWORLDQ4_H__ */
