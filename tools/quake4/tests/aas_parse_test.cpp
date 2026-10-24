/*
	Offline test for the engine's AAS file loader (neo/tools/compilers/aas/AASFile.cpp).

	Compiles the loader's own source files, AASFile.cpp and AASFile_sample.cpp, against the real
	idLexer (libidlib.a) with a stub idCommon. The one change: the lexer that opens a file reads it
	from memory instead (a LoadFile macro seen only by those two files). Files are streamed on stdin
	as "name\nlength\n" followed by the bytes; per run it reports files that fail to load or warn,
	the areas, reachability-bearing areas, clusters, dummy files and Quake 4 tactical features.

	Built and run by tools/quake4/test_aas_parse.sh. No game, no window, no GPU.
*/

#include <iostream>
#include <string>

#include "sys/platform.h"
#include "framework/FileSystem.h"
#include "framework/DeclEntityDef.h"
#include "framework/LoadTiming.h"
#include "tools/compilers/aas/AASFile_local.h"

#include "common_stub.h"

idDeclManager *			declManager = NULL;		// idAASSettings::FromDict - not used here
idFileSystem *			fileSystem = NULL;		// idAASFileLocal::Write - not used here
loadTiming_t			loadTiming;
double Sys_MillisecondsPrecise( void ) { return 0.0; }

static std::string fileData;
#define LoadFile( fileName ) LoadMemory( fileData.data(), (int)fileData.size(), fileName )
#include "tools/compilers/aas/AASFile.cpp"
#include "tools/compilers/aas/AASFile_sample.cpp"
#undef LoadFile

int main( int argc, char **argv ) {
	idLib::common = common;
	idLib::Init();

	int files = 0, failed = 0, dummies = 0, withFeatures = 0;
	long long areas = 0, clusters = 0, features = 0, featureIndexes = 0;
	std::string name, length;
	while ( std::getline( std::cin, name ) && std::getline( std::cin, length ) ) {
		fileData.resize( std::stoul( length ) );
		std::cin.read( &fileData[0], fileData.size() );
		files++;
		const int warningsBefore = stubWarnings;
		idAASFileLocal *file = new idAASFileLocal;
		const bool loaded = file->Load( name.c_str(), 0 );
		if ( !loaded || stubWarnings != warningsBefore ) {
			printf( "FAIL %s: %s\n", name.c_str(), loaded ? "warnings while loading" : "not loaded" );
			failed++;
		} else if ( file->IsDummyFile() ) {
			dummies++;
		} else {
			areas += file->GetNumAreas();
			clusters += file->GetNumClusters();
			features += file->GetNumFeatures();
			featureIndexes += file->GetNumFeatureIndexes();
			withFeatures += file->GetNumFeatures() > 0;
		}
		delete file;
	}
	printf( "%d files, %d failed, %d dummies, %lld areas, %lld clusters; %d with tactical features: %lld features, %lld index entries; warnings %d\n",
		files, failed, dummies, areas, clusters, withFeatures, features, featureIndexes, stubWarnings );
	return failed ? 1 : 0;
}
