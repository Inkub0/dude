/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: the Quake 4 game library's collision interface (docs/quake4-port.md 5.6). Quake 4 hands
// its game collision models as objects rather than integer handles, scopes model lookups by map,
// and gives every contact the material type of the surface hit (from the material, or its
// per-texel hit map). The engine's own collision manager does the work; a facade translates.

#ifndef __COLLISIONMODELQ4_H__
#define __COLLISIONMODELQ4_H__

#include "cm/CollisionModel.h"

class idDeclMaterialType;
class idMapFile;
class idRenderModel;

#define WORLD_MODEL_NAME	"worldMap"		// the world's collision model

// a contact: Doom 3's, plus the material type
struct contactInfoQ4_t {
	contactType_t				type;			// contact type
	idVec3						point;			// point of contact
	idVec3						normal;			// contact plane normal
	float						dist;			// contact plane distance
	int							contents;		// contents at the other side of the surface
	const idMaterial *			material;		// surface material
	const idDeclMaterialType *	materialType;	// what the surface is made of, NULL if unknown
	int							modelFeature;	// contact feature on the model
	int							trmFeature;		// contact feature on the trace model
	int							entityNum;		// entity the contact surface is a part of
	int							id;				// id of the clip model the contact surface is a part of
};

struct traceQ4_t {
	float						fraction;		// fraction of movement completed, 1.0 = nothing hit
	idVec3						endpos;			// final position of the trace model
	idMat3						endAxis;		// final axis of the trace model
	contactInfoQ4_t				c;				// the contact, only valid if fraction < 1.0
};

// a collision model the game holds
class idCollisionModel {
public:
	virtual						~idCollisionModel( void ) {}

	virtual const char *		GetName( void ) const = 0;
	virtual bool				GetBounds( idBounds &bounds ) const = 0;
	virtual bool				GetContents( int &contents ) const = 0;		// of every brush and polygon, ored
	virtual bool				GetVertex( int vertexNum, idVec3 &vertex ) const = 0;
	virtual bool				GetEdge( int edgeNum, idVec3 &start, idVec3 &end ) const = 0;
	virtual bool				GetPolygon( int polygonNum, idFixedWinding &winding ) const = 0;
};

class idCollisionModelManagerQ4 {
public:
	virtual						~idCollisionModelManagerQ4( void ) {}

								// the map's collision models; forceReload reloads them even when the map is the same
	virtual void				LoadMap( const idMapFile *mapFile, bool forceReload ) = 0;
	virtual void				FreeMap( const char *mapName ) = 0;

								// a model of the map, loaded (or found) by name; NULL when there is none
	virtual idCollisionModel *	LoadModel( const char *mapName, const char *modelName ) = 0;
	virtual void				PreCacheModel( const char *mapName, const char *modelName ) = 0;
	virtual void				FreeModel( idCollisionModel *model ) = 0;
								// a model built from a trace model, to collide other trace models with
	virtual idCollisionModel *	ModelFromTrm( const char *mapName, const char *modelName, const idTraceModel &trm, const idMaterial *material ) = 0;
								// a trace model built from a model; false if the model does not make one
	virtual bool				TrmFromModel( const char *mapName, const char *modelName, idTraceModel &trm ) = 0;

								// the first collision of a moving trace model with a model
	virtual void				Translation( traceQ4_t *results, const idVec3 &start, const idVec3 &end,
									const idTraceModel *trm, const idMat3 &trmAxis, int contentMask,
									idCollisionModel *model, const idVec3 &modelOrigin, const idMat3 &modelAxis ) = 0;
	virtual void				Rotation( traceQ4_t *results, const idVec3 &start, const idRotation &rotation,
									const idTraceModel *trm, const idMat3 &trmAxis, int contentMask,
									idCollisionModel *model, const idVec3 &modelOrigin, const idMat3 &modelAxis ) = 0;
								// the contents the trace model touches, 0 in free space
	virtual int					Contents( const idVec3 &start,
									const idTraceModel *trm, const idMat3 &trmAxis, int contentMask,
									idCollisionModel *model, const idVec3 &modelOrigin, const idMat3 &modelAxis ) = 0;
								// the trace model's contacts with the model; their number
	virtual int					Contacts( contactInfoQ4_t *contacts, const int maxContacts, const idVec3 &start, const idVec6 &dir, const float depth,
									const idTraceModel *trm, const idMat3 &trmAxis, int contentMask,
									idCollisionModel *model, const idVec3 &modelOrigin, const idMat3 &modelAxis ) = 0;

								// debugging
	virtual void				DebugOutput( const idVec3 &viewOrigin, const idMat3 &viewAxis ) = 0;
	virtual void				DrawModel( idCollisionModel *model, const idVec3 &modelOrigin, const idMat3 &modelAxis,
									const idVec3 &viewOrigin, const idMat3 &viewAxis, const float radius ) = 0;
	virtual void				ListModels( void ) = 0;
	virtual void				ModelInfo( int num ) = 0;		// -1: all models
};

#endif /* !__COLLISIONMODELQ4_H__ */
