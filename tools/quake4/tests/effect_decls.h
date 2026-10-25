/*
	The effects folder's files as the decl manager splits them (idDeclManagerLocal::ParseFile),
	for the offline effect tests in this directory: "effect" is the only decl type that appears
	there, any other first word is the name of a decl of the folder's default type. Include after
	common_stub.h.
*/

#ifndef DUDE_TOOLS_QUAKE4_EFFECT_DECLS_H
#define DUDE_TOOLS_QUAKE4_EFFECT_DECLS_H

#include <string>
#include <vector>

typedef struct {
	idStr		name;
	int			line;
	int			offset;
	int			length;
} declText_t;

// returns the warnings the split gave (stray text between decls)
static int SplitDecls( const char *fileName, const std::string &data, std::vector<declText_t> &decls ) {
	idLexer src;
	idToken token;
	const int warningsBefore = stubWarnings;

	src.LoadMemory( data.data(), (int)data.size(), fileName );
	src.SetFlags( DECL_LEXER_FLAGS );
	while ( 1 ) {
		const int startMarker = src.GetFileOffset();
		const int sourceLine = src.GetLineNum();
		if ( !src.ReadToken( &token ) ) {
			break;
		}
		if ( token.Icmp( "effect" ) ) {
			if ( !token.Icmp( "{" ) ) {
				src.Warning( "Missing decl name" );
				src.SkipBracedSection( false );
				continue;
			}
			src.UnreadToken( &token );
		}
		if ( !src.ReadToken( &token ) ) {
			src.Warning( "Type without definition at end of file" );
			break;
		}
		if ( !token.Icmp( "{" ) ) {
			src.Warning( "Missing decl name" );
			src.SkipBracedSection( false );
			continue;
		}
		const idStr name = token;
		if ( !src.ReadToken( &token ) ) {
			src.Warning( "Type without definition at end of file" );
			break;
		}
		if ( token != "{" ) {
			src.Warning( "Expecting '{' but found '%s'", token.c_str() );
			continue;
		}
		src.UnreadToken( &token );
		src.SkipBracedSection();

		declText_t decl;
		decl.name = name;
		decl.line = sourceLine;
		decl.offset = startMarker;
		decl.length = src.GetFileOffset() - startMarker;
		decls.push_back( decl );
	}
	return stubWarnings - warningsBefore;
}

#endif
