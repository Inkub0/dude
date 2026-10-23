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

/*
===============================================================================

	Trace model vs. polygonal model collision detection.

===============================================================================
*/

#include "sys/platform.h"
#include "framework/FileSystem.h"
#include "renderer/Material.h"
#include "renderer/RenderWorld.h"

#include "cm/CollisionModel_local.h"

#define CM_FILE_EXT			"cm"
#define CM_FILEID			"CM"
#define CM_FILEVERSION		"1.00"
// DUDE: Quake 4's revisions (read only; this engine writes "1.00"): "3" for maps and most models,
// "2" for a few models of its last patch (pak019)
#define CM_FILEVERSION_QUAKE4		"3"
#define CM_FILEVERSION_QUAKE4_OLD	"2"

/*
===============================================================================

Writing of collision model file

===============================================================================
*/

void CM_GetNodeBounds( idBounds *bounds, cm_node_t *node );
int CM_GetNodeContents( cm_node_t *node );


/*
================
idCollisionModelManagerLocal::WriteNodes
================
*/
void idCollisionModelManagerLocal::WriteNodes( idFile *fp, cm_node_t *node ) {
	fp->WriteFloatString( "\t( %d %f )\n", node->planeType, node->planeDist );
	if ( node->planeType != -1 ) {
		WriteNodes( fp, node->children[0] );
		WriteNodes( fp, node->children[1] );
	}
}

/*
================
idCollisionModelManagerLocal::CountPolygonMemory
================
*/
int idCollisionModelManagerLocal::CountPolygonMemory( cm_node_t *node ) const {
	cm_polygonRef_t *pref;
	cm_polygon_t *p;
	int memory;

	memory = 0;
	for ( pref = node->polygons; pref; pref = pref->next ) {
		p = pref->p;
		if ( p->checkcount == checkCount ) {
			continue;
		}
		p->checkcount = checkCount;

		memory += sizeof( cm_polygon_t ) + ( p->numEdges - 1 ) * sizeof( p->edges[0] );
	}
	if ( node->planeType != -1 ) {
		memory += CountPolygonMemory( node->children[0] );
		memory += CountPolygonMemory( node->children[1] );
	}
	return memory;
}

/*
================
idCollisionModelManagerLocal::WritePolygons
================
*/
void idCollisionModelManagerLocal::WritePolygons( idFile *fp, cm_node_t *node ) {
	cm_polygonRef_t *pref;
	cm_polygon_t *p;
	int i;

	for ( pref = node->polygons; pref; pref = pref->next ) {
		p = pref->p;
		if ( p->checkcount == checkCount ) {
			continue;
		}
		p->checkcount = checkCount;
		fp->WriteFloatString( "\t%d (", p->numEdges );
		for ( i = 0; i < p->numEdges; i++ ) {
			fp->WriteFloatString( " %d", p->edges[i] );
		}
		fp->WriteFloatString( " ) ( %f %f %f ) %f", p->plane.Normal()[0], p->plane.Normal()[1], p->plane.Normal()[2], p->plane.Dist() );
		fp->WriteFloatString( " ( %f %f %f )", p->bounds[0][0], p->bounds[0][1], p->bounds[0][2] );
		fp->WriteFloatString( " ( %f %f %f )", p->bounds[1][0], p->bounds[1][1], p->bounds[1][2] );
		fp->WriteFloatString( " \"%s\"\n", p->material->GetName() );
	}
	if ( node->planeType != -1 ) {
		WritePolygons( fp, node->children[0] );
		WritePolygons( fp, node->children[1] );
	}
}

/*
================
idCollisionModelManagerLocal::CountBrushMemory
================
*/
int idCollisionModelManagerLocal::CountBrushMemory( cm_node_t *node ) const {
	cm_brushRef_t *bref;
	cm_brush_t *b;
	int memory;

	memory = 0;
	for ( bref = node->brushes; bref; bref = bref->next ) {
		b = bref->b;
		if ( b->checkcount == checkCount ) {
			continue;
		}
		b->checkcount = checkCount;

		memory += sizeof( cm_brush_t ) + ( b->numPlanes - 1 ) * sizeof( b->planes[0] );
	}
	if ( node->planeType != -1 ) {
		memory += CountBrushMemory( node->children[0] );
		memory += CountBrushMemory( node->children[1] );
	}
	return memory;
}

/*
================
idCollisionModelManagerLocal::WriteBrushes
================
*/
void idCollisionModelManagerLocal::WriteBrushes( idFile *fp, cm_node_t *node ) {
	cm_brushRef_t *bref;
	cm_brush_t *b;
	int i;

	for ( bref = node->brushes; bref; bref = bref->next ) {
		b = bref->b;
		if ( b->checkcount == checkCount ) {
			continue;
		}
		b->checkcount = checkCount;
		fp->WriteFloatString( "\t%d {\n", b->numPlanes );
		for ( i = 0; i < b->numPlanes; i++ ) {
			fp->WriteFloatString( "\t\t( %f %f %f ) %f\n", b->planes[i].Normal()[0], b->planes[i].Normal()[1], b->planes[i].Normal()[2], b->planes[i].Dist() );
		}
		fp->WriteFloatString( "\t} ( %f %f %f )", b->bounds[0][0], b->bounds[0][1], b->bounds[0][2] );
		fp->WriteFloatString( " ( %f %f %f ) \"%s\"\n", b->bounds[1][0], b->bounds[1][1], b->bounds[1][2], StringFromContents( b->contents ) );
	}
	if ( node->planeType != -1 ) {
		WriteBrushes( fp, node->children[0] );
		WriteBrushes( fp, node->children[1] );
	}
}

/*
================
idCollisionModelManagerLocal::WriteCollisionModel
================
*/
void idCollisionModelManagerLocal::WriteCollisionModel( idFile *fp, cm_model_t *model ) {
	int i, polygonMemory, brushMemory;

	fp->WriteFloatString( "collisionModel \"%s\" {\n", model->name.c_str() );
	// vertices
	fp->WriteFloatString( "\tvertices { /* numVertices = */ %d\n", model->numVertices );
	for ( i = 0; i < model->numVertices; i++ ) {
		fp->WriteFloatString( "\t/* %d */ ( %f %f %f )\n", i, model->vertices[i].p[0], model->vertices[i].p[1], model->vertices[i].p[2] );
	}
	fp->WriteFloatString( "\t}\n" );
	// edges
	fp->WriteFloatString( "\tedges { /* numEdges = */ %d\n", model->numEdges );
	for ( i = 0; i < model->numEdges; i++ ) {
		fp->WriteFloatString( "\t/* %d */ ( %d %d ) %d %d\n", i, model->edges[i].vertexNum[0], model->edges[i].vertexNum[1], model->edges[i].internal, model->edges[i].numUsers );
	}
	fp->WriteFloatString( "\t}\n" );
	// nodes
	fp->WriteFloatString( "\tnodes {\n" );
	WriteNodes( fp, model->node );
	fp->WriteFloatString( "\t}\n" );
	// polygons
	checkCount++;
	polygonMemory = CountPolygonMemory( model->node );
	fp->WriteFloatString( "\tpolygons /* polygonMemory = */ %d {\n", polygonMemory );
	checkCount++;
	WritePolygons( fp, model->node );
	fp->WriteFloatString( "\t}\n" );
	// brushes
	checkCount++;
	brushMemory = CountBrushMemory( model->node );
	fp->WriteFloatString( "\tbrushes /* brushMemory = */ %d {\n", brushMemory );
	checkCount++;
	WriteBrushes( fp, model->node );
	fp->WriteFloatString( "\t}\n" );
	// closing brace
	fp->WriteFloatString( "}\n" );
}

/*
================
idCollisionModelManagerLocal::WriteCollisionModelsToFile
================
*/
void idCollisionModelManagerLocal::WriteCollisionModelsToFile( const char *filename, int firstModel, int lastModel, unsigned int mapFileCRC ) {
	int i;
	idFile *fp;
	idStr name;

	name = filename;
	name.SetFileExtension( CM_FILE_EXT );

	common->Printf( "writing %s\n", name.c_str() );
	// _D3XP was saving to fs_cdpath
	fp = fileSystem->OpenFileWrite( name, "fs_devpath" );
	if ( !fp ) {
		common->Warning( "idCollisionModelManagerLocal::WriteCollisionModelsToFile: Error opening file %s\n", name.c_str() );
		return;
	}

	// write file id and version
	fp->WriteFloatString( "%s \"%s\"\n\n", CM_FILEID, CM_FILEVERSION );
	// write the map file crc
	fp->WriteFloatString( "%u\n\n", mapFileCRC );

	// write the collision models
	for ( i = firstModel; i < lastModel; i++ ) {
		WriteCollisionModel( fp, models[ i ] );
	}

	fileSystem->CloseFile( fp );
}

/*
================
idCollisionModelManagerLocal::WriteCollisionModelForMapEntity
================
*/
bool idCollisionModelManagerLocal::WriteCollisionModelForMapEntity( const idMapEntity *mapEnt, const char *filename, const bool testTraceModel ) {
	idFile *fp;
	idStr name;
	cm_model_t *model;

	SetupHash();
	model = CollisionModelForMapEntity( mapEnt );
	model->name = filename;

	name = filename;
	name.SetFileExtension( CM_FILE_EXT );

	common->Printf( "writing %s\n", name.c_str() );
	fp = fileSystem->OpenFileWrite( name, "fs_devpath" );
	if ( !fp ) {
		common->Printf( "idCollisionModelManagerLocal::WriteCollisionModelForMapEntity: Error opening file %s\n", name.c_str() );
		FreeModel( model );
		return false;
	}

	// write file id and version
	fp->WriteFloatString( "%s \"%s\"\n\n", CM_FILEID, CM_FILEVERSION );
	// write the map file crc
	fp->WriteFloatString( "%u\n\n", 0 );

	// write the collision model
	WriteCollisionModel( fp, model );

	fileSystem->CloseFile( fp );

	if ( testTraceModel ) {
		idTraceModel trm;
		TrmFromModel( model, trm );
	}

	FreeModel( model );

	return true;
}


/*
===============================================================================

Loading of collision model file

===============================================================================
*/

/*
================
idCollisionModelManagerLocal::ParseVertices
================
*/
void idCollisionModelManagerLocal::ParseVertices( idLexer *src, cm_model_t *model ) {
	int i;

	src->ExpectTokenString( "{" );
	model->numVertices = src->ParseInt();
	model->maxVertices = model->numVertices;
	model->vertices = (cm_vertex_t *) Mem_Alloc( model->maxVertices * sizeof( cm_vertex_t ) );
	for ( i = 0; i < model->numVertices; i++ ) {
		src->Parse1DMatrix( 3, model->vertices[i].p.ToFloatPtr() );
		model->vertices[i].side = 0;
		model->vertices[i].sideSet = 0;
		model->vertices[i].checkcount = 0;
	}
	src->ExpectTokenString( "}" );
}

/*
================
idCollisionModelManagerLocal::ParseEdges
================
*/
void idCollisionModelManagerLocal::ParseEdges( idLexer *src, cm_model_t *model ) {
	int i;

	src->ExpectTokenString( "{" );
	model->numEdges = src->ParseInt();
	model->maxEdges = model->numEdges;
	model->edges = (cm_edge_t *) Mem_Alloc( model->maxEdges * sizeof( cm_edge_t ) );
	for ( i = 0; i < model->numEdges; i++ ) {
		src->ExpectTokenString( "(" );
		model->edges[i].vertexNum[0] = src->ParseInt();
		model->edges[i].vertexNum[1] = src->ParseInt();
		src->ExpectTokenString( ")" );
		model->edges[i].side = 0;
		model->edges[i].sideSet = 0;
		model->edges[i].internal = src->ParseInt();
		model->edges[i].numUsers = src->ParseInt();
		model->edges[i].normal = vec3_origin;
		model->edges[i].checkcount = 0;
		model->numInternalEdges += model->edges[i].internal;
	}
	src->ExpectTokenString( "}" );
}

/*
================
idCollisionModelManagerLocal::ParseNodes
================
*/
cm_node_t *idCollisionModelManagerLocal::ParseNodes( idLexer *src, cm_model_t *model, cm_node_t *parent ) {
	cm_node_t *node;

	model->numNodes++;
	node = AllocNode( model, model->numNodes < NODE_BLOCK_SIZE_SMALL ? NODE_BLOCK_SIZE_SMALL : NODE_BLOCK_SIZE_LARGE );
	node->brushes = NULL;
	node->polygons = NULL;
	node->parent = parent;
	src->ExpectTokenString( "(" );
	node->planeType = src->ParseInt();
	node->planeDist = src->ParseFloat();
	src->ExpectTokenString( ")" );
	if ( node->planeType != -1 ) {
		node->children[0] = ParseNodes( src, model, node );
		node->children[1] = ParseNodes( src, model, node );
	}
	return node;
}

/*
================
ParseBlockSize

DUDE: the memory to reserve for a polygons / brushes block, from its header. Doom 3 writes
the byte count (polygons, a comment naming polygonMemory, 123456, {); Quake 4 writes the
element and sub-element counts (numPolygons 41470 and numPolygonEdges 163348, each after a
comment naming it), from which the same byte count follows. 0 = no header numbers (allocate
one by one). The counts, when the header gives them, go to numElements / numSubElements
(else -1) for the caller's check against the rows it reads.
================
*/
static int ParseBlockSize( idLexer *src, int elementSize, int subElementSize, int &numElements, int &numSubElements ) {
	idToken first, second;
	numElements = numSubElements = -1;
	if ( !src->CheckTokenType( TT_NUMBER, 0, &first ) ) {
		return 0;
	}
	if ( !src->CheckTokenType( TT_NUMBER, 0, &second ) ) {
		return first.GetIntValue();
	}
	numElements = first.GetIntValue();
	numSubElements = second.GetIntValue();
	// each element's struct already holds one sub-element (edges[1] / planes[1])
	return numElements * ( elementSize - subElementSize ) + numSubElements * subElementSize;
}

/*
================
CheckBlockCounts

DUDE: a Quake 4 block header counts its rows; one that disagrees with what was read means a
file this loader misreads (or a damaged one), so say so - the load itself goes on, as the
rows are what the collision uses.
================
*/
static void CheckBlockCounts( idLexer *src, const cm_model_t *model, const char *what, const char *subWhat,
		int headerCount, int headerSubCount, int count, int subCount ) {
	if ( headerCount >= 0 && ( headerCount != count || headerSubCount != subCount ) ) {
		common->Warning( "%s: collision model '%s' has %d %s and %d %s, its header says %d and %d",
			src->GetFileName(), model->name.c_str(), count, what, subCount, subWhat, headerCount, headerSubCount );
	}
}

/*
================
idCollisionModelManagerLocal::ParsePolygons
================
*/
void idCollisionModelManagerLocal::ParsePolygons( idLexer *src, cm_model_t *model ) {
	cm_polygon_t *p;
	int i, numEdges;
	idVec3 normal;
	idToken token;

	int headerPolygons, headerEdges;
	const int blockSize = ParseBlockSize( src, sizeof( cm_polygon_t ), sizeof( p->edges[0] ), headerPolygons, headerEdges );
	if ( blockSize > 0 ) {
		model->polygonBlock = (cm_polygonBlock_t *) Mem_Alloc( sizeof( cm_polygonBlock_t ) + blockSize );
		model->polygonBlock->bytesRemaining = blockSize;
		model->polygonBlock->next = ( (byte *) model->polygonBlock ) + sizeof( cm_polygonBlock_t );
	}

	int numRead = 0, numEdgesRead = 0;
	src->ExpectTokenString( "{" );
	while ( !src->CheckTokenString( "}" ) ) {
		// parse polygon
		numEdges = src->ParseInt();
		numRead++;
		numEdgesRead += numEdges;
		p = AllocPolygon( model, numEdges );
		p->numEdges = numEdges;
		src->ExpectTokenString( "(" );
		for ( i = 0; i < p->numEdges; i++ ) {
			p->edges[i] = src->ParseInt();
		}
		src->ExpectTokenString( ")" );
		src->Parse1DMatrix( 3, normal.ToFloatPtr() );
		p->plane.SetNormal( normal );
		p->plane.SetDist( src->ParseFloat() );
		src->Parse1DMatrix( 3, p->bounds[0].ToFloatPtr() );
		src->Parse1DMatrix( 3, p->bounds[1].ToFloatPtr() );
		src->ExpectTokenType( TT_STRING, 0, &token );
		// get material
		p->material = declManager->FindMaterial( token );
		p->contents = p->material->GetContentFlags();
		p->checkcount = 0;
		// DUDE: Quake 4 adds three texture coordinates and, from revision 3, the polygon's
		// primitive number: "( s t ) ( s t ) ( s t ) 1944". Measured against the .map brush
		// faces (tools/quake4/q4_cm_texcoords.py) they are the face's exact coordinates at three
		// points, but which points is not stored: three of the vertices in no fixed order for
		// ~80% of the polygons, points of a piece merged away for the rest - so they cannot place
		// a hit-map (materialImage) sample here. Read past; a trace's material type is the
		// material's own (docs/quake4-port.md §5.3, as idTech4A++ does).
		if ( quake4FileRevision >= 2 ) {
			for ( i = 0; i < 3; i++ ) {
				idVec2 st;
				src->Parse1DMatrix( 2, st.ToFloatPtr() );
			}
			if ( quake4FileRevision >= 3 ) {
				src->ParseInt();
			}
		}
		// filter polygon into tree
		R_FilterPolygonIntoTree( model, model->node, NULL, p );
	}
	CheckBlockCounts( src, model, "polygons", "edges", headerPolygons, headerEdges, numRead, numEdgesRead );
}

/*
================
idCollisionModelManagerLocal::ParseBrushes
================
*/
void idCollisionModelManagerLocal::ParseBrushes( idLexer *src, cm_model_t *model ) {
	cm_brush_t *b;
	int i, numPlanes;
	idVec3 normal;
	idToken token;

	int headerBrushes, headerPlanes;
	const int blockSize = ParseBlockSize( src, sizeof( cm_brush_t ), sizeof( b->planes[0] ), headerBrushes, headerPlanes );
	if ( blockSize > 0 ) {
		model->brushBlock = (cm_brushBlock_t *) Mem_Alloc( sizeof( cm_brushBlock_t ) + blockSize );
		model->brushBlock->bytesRemaining = blockSize;
		model->brushBlock->next = ( (byte *) model->brushBlock ) + sizeof( cm_brushBlock_t );
	}

	int numRead = 0, numPlanesRead = 0;
	src->ExpectTokenString( "{" );
	while ( !src->CheckTokenString( "}" ) ) {
		// parse brush
		numPlanes = src->ParseInt();
		numRead++;
		numPlanesRead += numPlanes;
		b = AllocBrush( model, numPlanes );
		b->numPlanes = numPlanes;
		src->ExpectTokenString( "{" );
		for ( i = 0; i < b->numPlanes; i++ ) {
			src->Parse1DMatrix( 3, normal.ToFloatPtr() );
			b->planes[i].SetNormal( normal );
			b->planes[i].SetDist( src->ParseFloat() );
		}
		src->ExpectTokenString( "}" );
		src->Parse1DMatrix( 3, b->bounds[0].ToFloatPtr() );
		src->Parse1DMatrix( 3, b->bounds[1].ToFloatPtr() );
		src->ReadToken( &token );
		if ( token.type == TT_NUMBER ) {
			b->contents = token.GetIntValue();		// old .cm files use a single integer
		} else {
			b->contents = ContentsFromString( token );
		}
		b->checkcount = 0;
		b->primitiveNum = 0;
		// DUDE: Quake 4 revision 3 ends the row with the brush's primitive number ("aas_solid" 1944)
		if ( quake4FileRevision >= 3 ) {
			b->primitiveNum = src->ParseInt();
		}
		// filter brush into tree
		R_FilterBrushIntoTree( model, model->node, NULL, b );
	}
	CheckBlockCounts( src, model, "brushes", "planes", headerBrushes, headerPlanes, numRead, numPlanesRead );
}

/*
================
idCollisionModelManagerLocal::ParseCollisionModel
================
*/
bool idCollisionModelManagerLocal::ParseCollisionModel( idLexer *src ) {
	cm_model_t *model;
	idToken token;

	if ( numModels >= MAX_SUBMODELS ) {
		common->Error( "LoadModel: no free slots" );
		return false;
	}
	model = AllocModel();
	models[numModels ] = model;
	numModels++;
	// parse the file
	src->ExpectTokenType( TT_STRING, 0, &token );
	model->name = token;
	// DUDE: Quake 4 writes a number after the name (collisionModel "worldMap" 10234 {)
	src->CheckTokenType( TT_NUMBER, 0, &token );
	src->ExpectTokenString( "{" );
	while ( !src->CheckTokenString( "}" ) ) {

		src->ReadToken( &token );

		if ( token == "vertices" ) {
			ParseVertices( src, model );
			continue;
		}

		if ( token == "edges" ) {
			ParseEdges( src, model );
			continue;
		}

		if ( token == "nodes" ) {
			src->ExpectTokenString( "{" );
			model->node = ParseNodes( src, model, NULL );
			src->ExpectTokenString( "}" );
			continue;
		}

		if ( token == "polygons" ) {
			ParsePolygons( src, model );
			continue;
		}

		if ( token == "brushes" ) {
			ParseBrushes( src, model );
			continue;
		}

		src->Error( "ParseCollisionModel: bad token \"%s\"", token.c_str() );
	}
	// calculate edge normals
	checkCount++;
	CalculateEdgeNormals( model, model->node );
	// get model bounds from brush and polygon bounds
	CM_GetNodeBounds( &model->bounds, model->node );
	// get model contents
	model->contents = CM_GetNodeContents( model->node );
	// total memory used by this model
	model->usedMemory = model->numVertices * sizeof(cm_vertex_t) +
						model->numEdges * sizeof(cm_edge_t) +
						model->polygonMemory +
						model->brushMemory +
						model->numNodes * sizeof(cm_node_t) +
						model->numPolygonRefs * sizeof(cm_polygonRef_t) +
						model->numBrushRefs * sizeof(cm_brushRef_t);

	return true;
}

/*
================
idCollisionModelManagerLocal::LoadCollisionModelFile
================
*/
bool idCollisionModelManagerLocal::LoadCollisionModelFile( const char *name, unsigned int mapFileCRC ) {
	idStr fileName;
	idToken token;
	idLexer *src;
	unsigned int crc;

	// load it
	fileName = name;
	fileName.SetFileExtension( CM_FILE_EXT );
	src = new idLexer( fileName );
	src->SetFlags( LEXFL_NOSTRINGCONCAT | LEXFL_NODOLLARPRECOMPILE );
	if ( !src->IsLoaded() ) {
		delete src;
		return false;
	}

	if ( !src->ExpectTokenString( CM_FILEID ) ) {
		common->Warning( "%s is not an CM file.", fileName.c_str() );
		delete src;
		return false;
	}

	// DUDE: Doom 3's "1.00" or Quake 4's "2" / "3" - the same sections, with Quake 4's additions
	// read where they appear (ParseCollisionModel, ParsePolygons, ParseBrushes)
	if ( !src->ReadToken( &token ) ) {
		token = "";
	}
	if ( token == CM_FILEVERSION_QUAKE4 ) {
		quake4FileRevision = 3;
	} else if ( token == CM_FILEVERSION_QUAKE4_OLD ) {
		quake4FileRevision = 2;
	} else if ( token == CM_FILEVERSION ) {
		quake4FileRevision = 0;
	} else {
		common->Warning( "%s has version %s instead of %s", fileName.c_str(), token.c_str(), CM_FILEVERSION );
		delete src;
		return false;
	}

	if ( !src->ExpectTokenType( TT_NUMBER, TT_INTEGER, &token ) ) {
		common->Warning( "%s has no map file CRC", fileName.c_str() );
		delete src;
		return false;
	}

	// a Quake 4 file's CRC comes from Raven's tools, which this engine's map CRC need not
	// reproduce; it ships with its .map, so it is taken as current (a stale one would make the
	// engine rebuild the collision from the .map, which Quake 4's brushes are not meant for)
	crc = token.GetUnsignedIntValue();
	if ( mapFileCRC && crc != mapFileCRC && quake4FileRevision == 0 ) {
		common->Printf( "%s is out of date\n", fileName.c_str() );
		delete src;
		return false;
	}

	// parse the file
	while ( 1 ) {
		if ( !src->ReadToken( &token ) ) {
			break;
		}

		if ( token == "collisionModel" ) {
			if ( !ParseCollisionModel( src ) ) {
				delete src;
				return false;
			}
			continue;
		}

		src->Error( "idCollisionModelManagerLocal::LoadCollisionModelFile: bad token \"%s\"", token.c_str() );
	}

	delete src;

	return true;
}
