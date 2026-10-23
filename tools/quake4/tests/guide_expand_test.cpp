/*
	Offline test for neo/framework/DeclGuides.cpp (Quake 4 material guides).

	Compiles the engine's own DeclGuides.cpp against libidlib.a with a stub idCommon, loads
	every guides/*.guide template and expands every materials/*.mtr file from an extracted
	copy of the retail paks, writing the expanded text for tools/quake4/check_guides.py to
	compare against an independent Python expansion. No game, no window, no GPU.

	Built and run by tools/quake4/test_guides.sh.
*/

#include "framework/DeclGuides.cpp"		// the anonymous-namespace helpers become visible here

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "common_stub.h"

idFileSystem *			fileSystem = NULL;		// DeclGuides::Load is not used: templates are fed directly

// files under dir (recursively, like the Quake 4 decl folder scan), relative to dir
static void ListTree( const std::string &dir, const std::string &rel, const char *ext, std::vector<std::string> &out ) {
	DIR *d = opendir( ( dir + "/" + rel ).c_str() );
	if ( !d ) {
		return;
	}
	while ( struct dirent *e = readdir( d ) ) {
		const std::string n = e->d_name;
		if ( n == "." || n == ".." ) {
			continue;
		}
		const std::string path = rel.empty() ? n : rel + "/" + n;
		struct stat st;
		if ( stat( ( dir + "/" + path ).c_str(), &st ) == 0 && S_ISDIR( st.st_mode ) ) {
			ListTree( dir, path, ext, out );
		} else if ( n.size() > strlen( ext ) && n.compare( n.size() - strlen( ext ), strlen( ext ), ext ) == 0 ) {
			out.push_back( path );
		}
	}
	closedir( d );
}

static std::vector<std::string> ListDir( const std::string &dir, const char *ext ) {
	std::vector<std::string> out;
	ListTree( dir, "", ext, out );
	std::sort( out.begin(), out.end() );
	return out;
}

static std::string ReadAll( const std::string &path ) {
	std::ifstream f( path, std::ios::binary );
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

int main( int argc, char **argv ) {
	if ( argc != 3 ) {
		fprintf( stderr, "usage: %s <extracted q4base dir> <output dir>\n", argv[0] );
		return 1;
	}
	idLib::common = common;
	idLib::Init();

	const std::string in = argv[1], out = argv[2];
	for ( const std::string &g : ListDir( in + "/guides", ".guide" ) ) {
		const std::string text = ReadAll( in + "/guides/" + g );
		LoadTemplates( ( "guides/" + g ).c_str(), text.c_str(), (int)text.size() );
	}
	printf( "templates: %d\n", DeclGuides::Num() );

	mkdir( ( out + "/materials" ).c_str(), 0755 );
	int expandedFiles = 0, files = 0;
	for ( const std::string &m : ListDir( in + "/materials", ".mtr" ) ) {
		const std::string text = ReadAll( in + "/materials/" + m );
		idStr expanded;
		idList<int> lineMap;
		files++;
		const bool changed = DeclGuides::Expand( text.c_str(), (int)text.size(), ( "materials/" + m ).c_str(), expanded, lineMap );
		const std::string dst = out + "/materials/" + m;
		for ( size_t k = ( out + "/materials/" ).size(); ( k = dst.find( '/', k ) ) != std::string::npos; k++ ) {
			mkdir( dst.substr( 0, k ).c_str(), 0755 );
		}
		std::ofstream o( dst, std::ios::binary );
		if ( changed ) {
			expandedFiles++;
			o.write( expanded.c_str(), expanded.Length() );
			// the source line of every expanded line, for check_guides.py
			std::ofstream l( dst + ".lines", std::ios::binary );
			for ( int k = 0; k < lineMap.Num(); k++ ) {
				l << lineMap[k] << "\n";
			}
		} else {
			o.write( text.data(), text.size() );
		}
	}
	printf( "mtr files: %d, with guides: %d, warnings: %d\n", files, expandedFiles, stubWarnings );
	idLib::ShutDown();
	return stubWarnings ? 3 : 0;
}
