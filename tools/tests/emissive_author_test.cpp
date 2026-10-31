/*
	Offline test of the emissive authoring parsers (neo/renderer/RtEmissiveAuthor.cpp, docs/rtx-light-editor.md):
	the .map light-name scan and the per-map file tokenizer, compiled VERBATIM from the engine source (the
	EMAUTH-TEST-BEGIN / END block, extracted by test_emissive_author.sh) against the real idLexer (libidlib.a).

	stdin: a stream of "<name>\n<length>\n<bytes>" .map files; for each, prints "<name> <lights>" so the driver can
	compare with an independent count. Then runs the tokenizer cases and prints PASS / FAIL.
*/

#include <cstdio>
#include <cstring>
#include <string>
#include <iostream>

#include "sys/platform.h"
#include "idlib/Lexer.h"
#include "idlib/containers/StrList.h"
#include "framework/Common.h"
#include "common_stub.h"

struct emMapLight_t { idStr name; idVec3 origin; };		// as RtEmissiveAuthor.cpp
struct emAuthLook_t { float radiance; bool hasTint; idVec3 tint; int profile; int twoSided; };	// as RtEmissiveAuthor.cpp

#include "emauth_gen.inc"

static int failures = 0;

static void ExpectTokens( const char *line, std::initializer_list<const char *> want ) {
	idStrList got;
	EmAuth_Tokenize( line, got );
	bool ok = got.Num() == (int)want.size();
	int i = 0;
	for ( const char *w : want ) {
		if ( !ok ) { break; }
		ok = got[i].Cmp( w ) == 0;
		i++;
	}
	if ( !ok ) {
		failures++;
		printf( "FAIL tokenize [%s]: got %d words:", line, got.Num() );
		for ( int k = 0; k < got.Num(); k++ ) { printf( " <%s>", got[k].c_str() ); }
		printf( "\n" );
	}
}

int main() {
	idLib::common = common;
	idLib::Init();
	std::string name, len;
	int maps = 0;
	while ( std::getline( std::cin, name ) && std::getline( std::cin, len ) ) {
		const size_t n = std::stoul( len );
		std::string data( n, '\0' );
		std::cin.read( &data[0], (std::streamsize)n );
		idLexer src( LEXFL_NOSTRINGCONCAT | LEXFL_NOSTRINGESCAPECHARS | LEXFL_NOFATALERRORS | LEXFL_ALLOWPATHNAMES );
		src.LoadMemory( data.data(), (int)data.size(), name.c_str() );
		idList<emMapLight_t> lights;
		EmAuth_ScanMapLightsLexer( src, lights );
		printf( "%s %d", name.c_str(), lights.Num() );
		if ( lights.Num() > 0 ) { printf( " %s %.0f %.0f %.0f", lights[0].name.c_str(), lights[0].origin.x, lights[0].origin.y, lights[0].origin.z ); }
		printf( "\n" );
		maps++;
	}
	ExpectTokens( "light \"tim_light_3701\" delete", { "light", "tim_light_3701", "delete" } );
	ExpectTokens( "light @-2688,-2144,282 delete   // a comment", { "light", "@-2688,-2144,282", "delete" } );
	ExpectTokens( "fixture 16e9a41f488c47e1 emit scale 2.000 reach 300 follow \"light_4839\"",
		{ "fixture", "16e9a41f488c47e1", "emit", "scale", "2.000", "reach", "300", "follow", "light_4839" } );
	ExpectTokens( "material \"textures/base_light/striplight3\" emit\r", { "material", "textures/base_light/striplight3", "emit" } );
	ExpectTokens( "\t  // only a comment", {} );
	ExpectTokens( "", {} );
	ExpectTokens( "light \"name with space\" delete", { "light", "name with space", "delete" } );
	// appearance words: parse a line, write it back, parse again - the same look
	{
		const char *lines[] = { "radiance 20 tint 1 0.5 0.25 profile lambert twosided 1", "radiance matched profile 2", "tint 0 0 1 twosided 0", "" };
		for ( const char *line : lines ) {
			idStrList w;
			EmAuth_Tokenize( line, w );
			emAuthLook_t a; EmAuth_ClearLook( a );
			for ( int k = 0; k < w.Num(); k++ ) { if ( !EmAuth_ParseLookWord( w, k, a ) ) { failures++; printf( "FAIL look word '%s' in [%s]\n", w[k].c_str(), line ); } }
			const idStr back = EmAuth_LookWords( a );
			idStrList w2;
			EmAuth_Tokenize( back, w2 );
			emAuthLook_t b; EmAuth_ClearLook( b );
			for ( int k = 0; k < w2.Num(); k++ ) { EmAuth_ParseLookWord( w2, k, b ); }
			const bool same = idMath::Fabs( a.radiance - b.radiance ) < 1e-3f && a.hasTint == b.hasTint && ( !a.hasTint || a.tint.Compare( b.tint, 1e-3f ) )
				&& a.profile == b.profile && a.twoSided == b.twoSided && EmAuth_LookSet( a ) == ( line[0] != '\0' );
			if ( !same ) { failures++; printf( "FAIL look round trip [%s] -> [%s]\n", line, back.c_str() ); }
		}
	}
	printf( "tokenizer: %s (%d failures); %d maps scanned\n", failures ? "FAIL" : "PASS", failures, maps );
	return failures ? 1 : 0;
}
