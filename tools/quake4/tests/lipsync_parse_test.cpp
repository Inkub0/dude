/*
	Offline test for Quake 4's lip-sync phrases (game/LipSync.cpp rvLipSyncData in the SDK):
	reads every viseme string, streamed on stdin as "name\nlength\n" followed by the bytes, with
	the real idLexer (libidlib.a, the lexer the game library links) under the game's own flags,
	and walks it as the game does - "<" phrase ">", "{" emotion "}", else a viseme. A phrase
	the lexer splits or drops (a localized word it took for white space) fails as it would in
	the game, where the failure is a fatal error that ends the map. Prints the failures per
	language and exits 1 on any.

	Built and run by tools/quake4/test_lipsync_parse.sh. No game, no window.
*/

#include <map>
#include <string>
#include <iostream>

#include "sys/platform.h"
#include "idlib/Lexer.h"
#include "framework/Common.h"

#include "common_stub.h"

int main( int argc, char **argv ) {
	idLib::common = common;
	idLib::Init();

	std::map<std::string, int> total, failed;
	std::string name, lengthLine;
	while ( std::getline( std::cin, name ) && std::getline( std::cin, lengthLine ) ) {
		const size_t length = std::stoul( lengthLine );
		std::string data( length, '\0' );
		std::cin.read( &data[0], length );
		const std::string language = name.substr( name.rfind( '/' ) + 1 );

		idLexer src;
		src.SetFlags( LEXFL_ALLOWPATHNAMES | LEXFL_ALLOWNUMBERNAMES | LEXFL_NOERRORS | LEXFL_NOWARNINGS | LEXFL_NOFATALERRORS );
		src.LoadMemory( data.data(), (int)data.size(), name.c_str() );
		idToken token;
		bool ok = true;
		while ( ok && src.ReadToken( &token ) ) {
			if ( token == "<" ) {
				ok = src.ReadToken( &token ) && src.ReadToken( &token ) && token == ">";
			} else if ( token == "{" ) {
				ok = src.ReadToken( &token ) && src.ReadToken( &token ) && token == "}";
			}
		}
		total[language]++;
		if ( !ok ) {
			if ( failed[language]++ < 3 ) {
				printf( "  FAIL %s\n", name.c_str() );
			}
		}
	}

	int failures = 0;
	for ( const auto &t : total ) {
		printf( "%-10s %5d viseme strings, %d fail\n", t.first.c_str(), t.second, failed[t.first] );
		failures += failed[t.first];
	}
	return failures > 0 ? 1 : 0;
}
