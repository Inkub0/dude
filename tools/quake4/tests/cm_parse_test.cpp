/*
	Offline test for the engine's .cm loader (neo/cm/CollisionModel_files.cpp).

	Runs the loader's own code - copied verbatim into cm_parse_gen.inc by cm_extract.py - with
	the real idLexer (libidlib.a) over collision files streamed on stdin as "name\nlength\n"
	followed by the bytes, and reports per run: files that fail to load, models, polygons,
	brushes, the contents bits seen, and how well each model's preallocated block fits what it
	holds (a block smaller than its rows sends the rest to one-by-one allocations). Only the
	BSP filtering and bounds/edge-normal passes are stubbed: they run after parsing.

	Built and run by tools/quake4/test_cm_parse.sh. No game, no window, no GPU.
*/

#include <stdexcept>
#include <string>
#include <iostream>

#include "sys/platform.h"
#include "framework/FileSystem.h"
#include "renderer/Material.h"
#include "renderer/RenderWorld.h"
#define private public		// the loader's state is private to the collision manager
#include "cm/CollisionModel_local.h"
#undef private

#include "common_stub.h"

struct LoadError : std::runtime_error {
	using std::runtime_error::runtime_error;
};
static void ThrowLoadError( const char *message ) {
	throw LoadError( message );
}

// every polygon gets this material: contents 0, so polygon contents come out 0 as with an
// empty material, which is what the loader would see for a material it cannot find
alignas( 64 ) static unsigned char materialStorage[sizeof( idMaterial )];
static struct {
	const idMaterial *FindMaterial( const char *name, bool makeDefault = true ) { return (const idMaterial *)materialStorage; }
} declManagerStub;

static std::string fileData;
class MemoryLexer : public idLexer {
public:
	MemoryLexer( const char *name ) : idLexer() { LoadMemory( fileData.data(), (int)fileData.size(), name ); }
};

static long long polygons, brushes, brushesWithPrimitive;
static int contentsSeen;

void idCollisionModelManagerLocal::R_FilterPolygonIntoTree( cm_model_t *, cm_node_t *, cm_polygonRef_t *, cm_polygon_t *p ) {
	polygons++;
	contentsSeen |= p->contents;
}
void idCollisionModelManagerLocal::R_FilterBrushIntoTree( cm_model_t *, cm_node_t *, cm_brushRef_t *, cm_brush_t *b ) {
	brushes++;
	contentsSeen |= b->contents;
	brushesWithPrimitive += b->primitiveNum != 0;
}
void idCollisionModelManagerLocal::CalculateEdgeNormals( cm_model_t *, cm_node_t * ) {}
void CM_GetNodeBounds( idBounds *bounds, cm_node_t * ) { bounds->Clear(); }
int CM_GetNodeContents( cm_node_t * ) { return 0; }

#define declManager ( &declManagerStub )
#include "cm_parse_gen.inc"
#undef declManager

// bytes of a model's rows that did not fit its block, and the block bytes left unused
template< class blockType >
static void BlockFit( const blockType *block, int memory, long long &outside, long long &unused ) {
	if ( block != NULL ) {
		outside += memory - ( block->next - ( (const byte *)block + sizeof( blockType ) ) );
		unused += block->bytesRemaining;
	}
}

int main( int argc, char **argv ) {
	idLib::common = common;
	idLib::Init();
	stubErrorHandler = ThrowLoadError;

	// the manager is never constructed (its vtable needs the whole module): the loader only
	// touches the plain members set here
	idCollisionModelManagerLocal *cm = (idCollisionModelManagerLocal *)calloc( 1, sizeof( idCollisionModelManagerLocal ) );
	cm->maxModels = MAX_SUBMODELS;
	cm->models = (cm_model_t **)calloc( MAX_SUBMODELS + 1, sizeof( cm_model_t * ) );

	int files = 0, failed = 0;
	long long models = 0, outside = 0, unused = 0, worstOutside = 0;
	std::string name, length, worstName;
	while ( std::getline( std::cin, name ) && std::getline( std::cin, length ) ) {
		fileData.resize( std::stoul( length ) );
		std::cin.read( &fileData[0], fileData.size() );
		files++;
		cm->numModels = 0;
		bool loaded = false;
		try {
			loaded = cm->LoadCollisionModelFile( name.c_str(), 0 );
			if ( !loaded ) {
				printf( "FAIL %s: not loaded\n", name.c_str() );
			}
		} catch ( const LoadError &e ) {
			printf( "FAIL %s: %s\n", name.c_str(), e.what() );
		}
		long long fileOutside = 0;
		for ( int i = 0; i < cm->numModels; i++ ) {
			cm_model_t *m = cm->models[i];
			BlockFit( m->polygonBlock, m->polygonMemory, fileOutside, unused );
			BlockFit( m->brushBlock, m->brushMemory, fileOutside, unused );
			Mem_Free( m->vertices );
			Mem_Free( m->edges );
			Mem_Free( m->polygonBlock );
			Mem_Free( m->brushBlock );
			for ( cm_nodeBlock_t *block = m->nodeBlocks; block != NULL; ) {
				cm_nodeBlock_t *next = block->next;
				Mem_Free( block );
				block = next;
			}
			delete m;		// rows allocated outside the blocks are left: a test run is short
			models++;
		}
		failed += !loaded;
		outside += fileOutside;
		if ( fileOutside > worstOutside ) {
			worstOutside = fileOutside;
			worstName = name;
		}
	}
	printf( "%d files, %d failed, %lld models, %lld polygons, %lld brushes (%lld with a primitive number), contents bits 0x%x, warnings %d\n",
		files, failed, models, polygons, brushes, brushesWithPrimitive, contentsSeen, stubWarnings );
	printf( "block fit: %lld bytes of rows allocated outside their block (most: %s, %lld), %lld block bytes unused\n",
		outside, worstName.empty() ? "-" : worstName.c_str(), worstOutside, unused );
	return failed ? 1 : 0;
}
