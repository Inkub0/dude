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

In addition, the Doom 3 Source Code is also subject to certain additional terms.
You should have received a copy of these additional terms immediately following
the terms and conditions of the GNU General Public License which accompanied the
Doom 3 Source Code.  If not, please request a copy in writing from id Software
at the address below.

If you have questions concerning this license or the applicable additional
terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc.,
Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

// arbtool: standalone tester for the ARB assembly parser (and, later, the
// ARB -> GLSL transpiler). Feeds the mod-shader corpus through the parser:
//
//   arbtool parse file1.vfp [file2.vp ...]
//
// Exit code 0 iff every program in every file parses.

#include "renderer/ArbProgram.h"
#include "renderer/ArbToGlsl.h"

#include <cstdio>
#include <fstream>
#include <sstream>

static bool ReadFile( const char *path, std::string &out ) {
	std::ifstream f( path, std::ios::binary );
	if ( !f ) {
		return false;
	}
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

// transpile every program in the file, write <outdir>/<stem>.vert/.frag
static int TranspileFile( const char *path, const char *outDir ) {
	std::string text;
	if ( !ReadFile( path, text ) ) {
		fprintf( stderr, "FAIL  %s: cannot read file\n", path );
		return 1;
	}
	// stem = basename without extension
	std::string stem = path;
	size_t slash = stem.find_last_of( "/\\" );
	if ( slash != std::string::npos ) stem = stem.substr( slash + 1 );
	size_t dot = stem.find_last_of( '.' );
	if ( dot != std::string::npos ) stem = stem.substr( 0, dot );

	int fails = 0;
	std::vector<std::string> sections = arb::SplitSections( text );
	for ( size_t s = 0; s < sections.size(); s++ ) {
		arb::ParseResult pr = arb::Parse( sections[s] );
		const char *ext = ( pr.program.kind == arb::PK_Vertex ) ? "vert" : "frag";
		if ( !pr.ok ) {
			fprintf( stderr, "FAIL  %s [%s] parse line %d: %s\n", path, ext, pr.errorLine, pr.error.c_str() );
			fails++;
			continue;
		}
		arb::TranspileResult tr = arb::ToGlsl( pr.program );
		if ( !tr.ok ) {
			fprintf( stderr, "FAIL  %s [%s] transpile: %s\n", path, ext, tr.error.c_str() );
			fails++;
			continue;
		}
		std::string outPath = std::string( outDir ) + "/" + stem + "." + ext;
		std::ofstream f( outPath.c_str(), std::ios::binary );
		if ( !f ) {
			fprintf( stderr, "FAIL  cannot write %s\n", outPath.c_str() );
			fails++;
			continue;
		}
		f << tr.glsl;
		printf( "ok    %s -> %s\n", path, outPath.c_str() );
	}
	return fails;
}

int main( int argc, char **argv ) {
	if ( argc >= 4 && std::string( argv[1] ) == "glsl" ) {
		// arbtool glsl <outdir> <file.vfp> [...]
		int fails = 0;
		for ( int i = 3; i < argc; i++ ) {
			fails += TranspileFile( argv[i], argv[2] );
		}
		printf( "\n%d failures\n", fails );
		return fails == 0 ? 0 : 1;
	}
	if ( argc < 3 || std::string( argv[1] ) != "parse" ) {
		fprintf( stderr, "usage: arbtool parse <file.vfp> [...]\n"
						 "       arbtool glsl <outdir> <file.vfp> [...]\n" );
		return 2;
	}

	int filesTotal = 0, programsTotal = 0, programsFailed = 0;

	for ( int i = 2; i < argc; i++ ) {
		std::string text;
		if ( !ReadFile( argv[i], text ) ) {
			fprintf( stderr, "FAIL  %s: cannot read file\n", argv[i] );
			programsFailed++;
			continue;
		}
		filesTotal++;

		std::vector<std::string> sections = arb::SplitSections( text );
		if ( sections.empty() ) {
			fprintf( stderr, "FAIL  %s: no ARB programs found\n", argv[i] );
			programsFailed++;
			continue;
		}

		for ( size_t s = 0; s < sections.size(); s++ ) {
			programsTotal++;
			arb::ParseResult res = arb::Parse( sections[s] );
			const char *kind = ( res.program.kind == arb::PK_Vertex ) ? "vp" : "fp";
			if ( res.ok ) {
				printf( "ok    %s [%s] %d instructions, %d temps, %d options\n",
						argv[i], kind,
						(int)res.program.instructions.size(),
						(int)res.program.tempNames.size(),
						(int)res.program.options.size() );
			} else {
				programsFailed++;
				fprintf( stderr, "FAIL  %s [%s] line %d: %s\n",
						 argv[i], kind, res.errorLine, res.error.c_str() );
			}
		}
	}

	printf( "\n%d files, %d programs, %d failures\n",
			filesTotal, programsTotal, programsFailed );
	return programsFailed == 0 ? 0 : 1;
}
