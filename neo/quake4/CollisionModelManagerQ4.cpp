/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: the Quake 4 game's collision model manager (quake4/CollisionModelQ4.h) over the engine's.
// The game holds models as objects: a model of the map is the engine's handle, kept by name and
// reference counted; a model built from a trace model keeps its trace model and becomes the
// engine's reusable trace-model slot (SetupTrmModel) each time it is collided with, as Doom 3's
// idClipModel does. The map name scoping is the game's bookkeeping - the engine holds one map.
//
// Contacts carry the surface material's own material type. Per-texel hit maps cannot be sampled
// from a collision polygon (docs/quake4-port.md 5.3), as in idTech4A++.

#include "sys/platform.h"
#include "framework/Common.h"
#include "framework/DeclQuake4.h"
#include "renderer/Material.h"

#include "quake4/EngineQ4.h"

/*
===============================================================================

	idCollisionModelQ4Local

===============================================================================
*/

class idCollisionModelQ4Local : public idCollisionModel {
public:
							idCollisionModelQ4Local( const char *name ) : name( name ), handle( -1 ), trm( NULL ), trmMaterial( NULL ), refCount( 1 ) {}
	virtual					~idCollisionModelQ4Local( void ) { delete trm; }

	virtual const char *	GetName( void ) const { return name.c_str(); }
	virtual bool			GetBounds( idBounds &bounds ) const;
	virtual bool			GetContents( int &contents ) const;
	virtual bool			GetVertex( int vertexNum, idVec3 &vertex ) const;
	virtual bool			GetEdge( int edgeNum, idVec3 &start, idVec3 &end ) const;
	virtual bool			GetPolygon( int polygonNum, idFixedWinding &winding ) const;

							// the engine's handle for a collision now (a trace model's is the reusable slot)
	cmHandle_t				Handle( void ) const;

	idStr					name;
	cmHandle_t				handle;			// a map model's, -1 when not loaded
	idTraceModel *			trm;			// a trace model's
	const idMaterial *		trmMaterial;
	int						refCount;		// a map model's holders
};

cmHandle_t idCollisionModelQ4Local::Handle( void ) const {
	if ( trm != NULL ) {
		return collisionModelManager->SetupTrmModel( *trm, trmMaterial );
	}
	return handle;
}

bool idCollisionModelQ4Local::GetBounds( idBounds &bounds ) const {
	if ( trm != NULL ) {
		bounds = trm->bounds;
		return true;
	}
	return handle >= 0 && collisionModelManager->GetModelBounds( handle, bounds );
}

bool idCollisionModelQ4Local::GetContents( int &contents ) const {
	const cmHandle_t h = Handle();
	return h >= 0 && collisionModelManager->GetModelContents( h, contents );
}

bool idCollisionModelQ4Local::GetVertex( int vertexNum, idVec3 &vertex ) const {
	const cmHandle_t h = Handle();
	return h >= 0 && collisionModelManager->GetModelVertex( h, vertexNum, vertex );
}

bool idCollisionModelQ4Local::GetEdge( int edgeNum, idVec3 &start, idVec3 &end ) const {
	const cmHandle_t h = Handle();
	return h >= 0 && collisionModelManager->GetModelEdge( h, edgeNum, start, end );
}

bool idCollisionModelQ4Local::GetPolygon( int polygonNum, idFixedWinding &winding ) const {
	const cmHandle_t h = Handle();
	return h >= 0 && collisionModelManager->GetModelPolygon( h, polygonNum, winding );
}

/*
===============================================================================

	idCollisionModelManagerQ4Local

===============================================================================
*/

class idCollisionModelManagerQ4Local : public idCollisionModelManagerQ4 {
public:
	virtual					~idCollisionModelManagerQ4Local( void );

	virtual void			LoadMap( const idMapFile *mapFile, bool forceReload );
	virtual void			FreeMap( const char *mapName );

	virtual idCollisionModel *LoadModel( const char *mapName, const char *modelName );
	virtual void			PreCacheModel( const char *mapName, const char *modelName );
	virtual void			FreeModel( idCollisionModel *model );
	virtual idCollisionModel *ModelFromTrm( const char *mapName, const char *modelName, const idTraceModel &trm, const idMaterial *material );
	virtual bool			TrmFromModel( const char *mapName, const char *modelName, idTraceModel &trm );

	virtual void			Translation( traceQ4_t *results, const idVec3 &start, const idVec3 &end,
								const idTraceModel *trm, const idMat3 &trmAxis, int contentMask,
								idCollisionModel *model, const idVec3 &modelOrigin, const idMat3 &modelAxis );
	virtual void			Rotation( traceQ4_t *results, const idVec3 &start, const idRotation &rotation,
								const idTraceModel *trm, const idMat3 &trmAxis, int contentMask,
								idCollisionModel *model, const idVec3 &modelOrigin, const idMat3 &modelAxis );
	virtual int				Contents( const idVec3 &start,
								const idTraceModel *trm, const idMat3 &trmAxis, int contentMask,
								idCollisionModel *model, const idVec3 &modelOrigin, const idMat3 &modelAxis );
	virtual int				Contacts( contactInfoQ4_t *contacts, const int maxContacts, const idVec3 &start, const idVec6 &dir, const float depth,
								const idTraceModel *trm, const idMat3 &trmAxis, int contentMask,
								idCollisionModel *model, const idVec3 &modelOrigin, const idMat3 &modelAxis );

	virtual void			DebugOutput( const idVec3 &viewOrigin, const idMat3 &viewAxis );
	virtual void			DrawModel( idCollisionModel *model, const idVec3 &modelOrigin, const idMat3 &modelAxis,
								const idVec3 &viewOrigin, const idMat3 &viewAxis, const float radius );
	virtual void			ListModels( void );
	virtual void			ModelInfo( int num );

private:
	idCollisionModelQ4Local *FindMapModel( const char *modelName ) const;

	// the map's models by name; they outlive FreeMap (unloaded, handle -1) because the game may
	// still hold them, and load again when asked for by name
	idList<idCollisionModelQ4Local *> mapModels;
};

/*
===============
ContactToQ4
===============
*/
static void ContactToQ4( const contactInfo_t &src, contactInfoQ4_t &dst ) {
	dst.type = src.type;
	dst.point = src.point;
	dst.normal = src.normal;
	dst.dist = src.dist;
	dst.contents = src.contents;
	dst.material = src.material;
	dst.materialType = src.material != NULL ? static_cast<const idDeclMaterialType *>( src.material->GetMaterialType() ) : NULL;
	dst.modelFeature = src.modelFeature;
	dst.trmFeature = src.trmFeature;
	dst.entityNum = src.entityNum;
	dst.id = src.id;
}

static void TraceToQ4( const trace_t &src, traceQ4_t &dst ) {
	dst.fraction = src.fraction;
	dst.endpos = src.endpos;
	dst.endAxis = src.endAxis;
	if ( src.fraction < 1.0f ) {
		ContactToQ4( src.c, dst.c );
	} else {
		// the engine leaves the contact unset when nothing was hit: never read it
		memset( &dst.c, 0, sizeof( dst.c ) );
	}
}

// a trace that hit nothing, for a model that is not there
static void EmptyTraceQ4( traceQ4_t &dst, const idVec3 &end, const idMat3 &axis ) {
	memset( &dst, 0, sizeof( dst ) );
	dst.fraction = 1.0f;
	dst.endpos = end;
	dst.endAxis = axis;
}

static cmHandle_t ModelHandle( idCollisionModel *model ) {
	return model != NULL ? static_cast<idCollisionModelQ4Local *>( model )->Handle() : -1;
}

idCollisionModelManagerQ4Local::~idCollisionModelManagerQ4Local( void ) {
	mapModels.DeleteContents( true );
}

idCollisionModelQ4Local *idCollisionModelManagerQ4Local::FindMapModel( const char *modelName ) const {
	for ( int i = 0; i < mapModels.Num(); i++ ) {
		if ( mapModels[i]->name.Icmp( modelName ) == 0 ) {
			return mapModels[i];
		}
	}
	return NULL;
}

void idCollisionModelManagerQ4Local::LoadMap( const idMapFile *mapFile, bool forceReload ) {
	if ( forceReload ) {
		collisionModelManager->FreeMap();
		for ( int i = 0; i < mapModels.Num(); i++ ) {
			mapModels[i]->handle = -1;
		}
	}
	collisionModelManager->LoadMap( mapFile );
}

void idCollisionModelManagerQ4Local::FreeMap( const char *mapName ) {
	collisionModelManager->FreeMap();
	for ( int i = 0; i < mapModels.Num(); i++ ) {
		mapModels[i]->handle = -1;
	}
}

idCollisionModel *idCollisionModelManagerQ4Local::LoadModel( const char *mapName, const char *modelName ) {
	if ( modelName == NULL || modelName[0] == '\0' ) {
		return NULL;
	}
	idCollisionModelQ4Local *model = FindMapModel( modelName );
	if ( model == NULL ) {
		model = new idCollisionModelQ4Local( modelName );
		model->refCount = 0;
		mapModels.Append( model );
	}
	if ( model->handle < 0 ) {
		// the world is the map's first model; the engine answers 0 for "not found" as well
		const cmHandle_t h = collisionModelManager->LoadModel( modelName, false );
		if ( h == 0 && idStr::Icmp( modelName, WORLD_MODEL_NAME ) != 0 && idStr::Icmp( modelName, collisionModelManager->GetModelName( 0 ) ) != 0 ) {
			return NULL;
		}
		model->handle = h;
	}
	model->refCount++;
	return model;
}

void idCollisionModelManagerQ4Local::PreCacheModel( const char *mapName, const char *modelName ) {
	collisionModelManager->LoadModel( modelName, true );
}

void idCollisionModelManagerQ4Local::FreeModel( idCollisionModel *model ) {
	if ( model == NULL ) {
		return;
	}
	idCollisionModelQ4Local *local = static_cast<idCollisionModelQ4Local *>( model );
	if ( local->trm != NULL ) {
		delete local;
		return;
	}
	// a map model stays with the engine until the map goes; the object stays for its name
	if ( local->refCount > 0 ) {
		local->refCount--;
	}
}

idCollisionModel *idCollisionModelManagerQ4Local::ModelFromTrm( const char *mapName, const char *modelName, const idTraceModel &trm, const idMaterial *material ) {
	idCollisionModelQ4Local *model = new idCollisionModelQ4Local( modelName );
	model->trm = new idTraceModel( trm );
	model->trmMaterial = material;
	return model;
}

bool idCollisionModelManagerQ4Local::TrmFromModel( const char *mapName, const char *modelName, idTraceModel &trm ) {
	return collisionModelManager->TrmFromModel( modelName, trm );
}

void idCollisionModelManagerQ4Local::Translation( traceQ4_t *results, const idVec3 &start, const idVec3 &end,
		const idTraceModel *trm, const idMat3 &trmAxis, int contentMask,
		idCollisionModel *model, const idVec3 &modelOrigin, const idMat3 &modelAxis ) {
	const cmHandle_t h = ModelHandle( model );
	if ( h < 0 ) {
		EmptyTraceQ4( *results, end, trmAxis );
		return;
	}
	trace_t tr;
	collisionModelManager->Translation( &tr, start, end, trm, trmAxis, contentMask, h, modelOrigin, modelAxis );
	TraceToQ4( tr, *results );
}

void idCollisionModelManagerQ4Local::Rotation( traceQ4_t *results, const idVec3 &start, const idRotation &rotation,
		const idTraceModel *trm, const idMat3 &trmAxis, int contentMask,
		idCollisionModel *model, const idVec3 &modelOrigin, const idMat3 &modelAxis ) {
	const cmHandle_t h = ModelHandle( model );
	if ( h < 0 ) {
		EmptyTraceQ4( *results, start, trmAxis * rotation.ToMat3() );
		return;
	}
	trace_t tr;
	collisionModelManager->Rotation( &tr, start, rotation, trm, trmAxis, contentMask, h, modelOrigin, modelAxis );
	TraceToQ4( tr, *results );
}

int idCollisionModelManagerQ4Local::Contents( const idVec3 &start,
		const idTraceModel *trm, const idMat3 &trmAxis, int contentMask,
		idCollisionModel *model, const idVec3 &modelOrigin, const idMat3 &modelAxis ) {
	const cmHandle_t h = ModelHandle( model );
	if ( h < 0 ) {
		return 0;
	}
	return collisionModelManager->Contents( start, trm, trmAxis, contentMask, h, modelOrigin, modelAxis );
}

int idCollisionModelManagerQ4Local::Contacts( contactInfoQ4_t *contacts, const int maxContacts, const idVec3 &start, const idVec6 &dir, const float depth,
		const idTraceModel *trm, const idMat3 &trmAxis, int contentMask,
		idCollisionModel *model, const idVec3 &modelOrigin, const idMat3 &modelAxis ) {
	const cmHandle_t h = ModelHandle( model );
	if ( h < 0 || maxContacts <= 0 ) {
		return 0;
	}
	contactInfo_t *engineContacts = (contactInfo_t *)_alloca( maxContacts * sizeof( contactInfo_t ) );
	const int num = collisionModelManager->Contacts( engineContacts, maxContacts, start, dir, depth, trm, trmAxis, contentMask, h, modelOrigin, modelAxis );
	for ( int i = 0; i < num; i++ ) {
		ContactToQ4( engineContacts[i], contacts[i] );
	}
	return num;
}

void idCollisionModelManagerQ4Local::DebugOutput( const idVec3 &viewOrigin, const idMat3 &viewAxis ) {
	collisionModelManager->DebugOutput( viewOrigin );
}

void idCollisionModelManagerQ4Local::DrawModel( idCollisionModel *model, const idVec3 &modelOrigin, const idMat3 &modelAxis,
		const idVec3 &viewOrigin, const idMat3 &viewAxis, const float radius ) {
	const cmHandle_t h = ModelHandle( model );
	if ( h >= 0 ) {
		collisionModelManager->DrawModel( h, modelOrigin, modelAxis, viewOrigin, radius );
	}
}

void idCollisionModelManagerQ4Local::ListModels( void ) {
	collisionModelManager->ListModels();
}

void idCollisionModelManagerQ4Local::ModelInfo( int num ) {
	collisionModelManager->ModelInfo( num );
}

/*
===============
CollisionModelManagerQ4
===============
*/
idCollisionModelManagerQ4 *CollisionModelManagerQ4( void ) {
	static idCollisionModelManagerQ4Local collisionModelManagerQ4Local;
	return &collisionModelManagerQ4Local;
}
