/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: material guides - see DeclGuides.h.

#include "sys/platform.h"
#include "idlib/Str.h"
#include "idlib/containers/List.h"
#include "idlib/containers/StrList.h"
#include "framework/Common.h"
#include "framework/FileSystem.h"
#include "framework/DeclGuides.h"

#include <string>
#include <vector>

namespace {

const int MAX_GUIDE_PARMS		= 20;	// the SDK's limit
const int MAX_EXPAND_DEPTH		= 4;	// templates that use inlineGuide themselves

struct guideTemplate_t {
	idStr		name;
	idStr		file;
	idStrList	parms;
	idList<int>	parmsLongestFirst;		// so "Glowparm1" is tried before a "Glowparm" prefix
	idStr		body;					// comments removed, newlines kept
};

idList<guideTemplate_t>	templates;

bool IsIdent( char c ) {
	return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '_';
}

bool IsSpace( char c ) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// a keyword may start here: the previous character ends a token
bool AtTokenStart( const char *text, int i ) {
	return i == 0 || IsSpace( text[i - 1] ) || text[i - 1] == '{' || text[i - 1] == '}' || text[i - 1] == ';';
}

// copy of text with // and /* */ comments blanked (newlines kept); strings left intact
std::string StripComments( const char *text, int length ) {
	std::string out;
	out.reserve( length );
	int i = 0;
	while ( i < length ) {
		const char c = text[i];
		if ( c == '"' ) {
			const int start = i++;
			while ( i < length && text[i] != '"' && text[i] != '\n' ) {
				i++;
			}
			if ( i < length && text[i] == '"' ) {
				i++;
			}
			out.append( text + start, i - start );
		} else if ( c == '/' && i + 1 < length && text[i + 1] == '/' ) {
			while ( i < length && text[i] != '\n' ) {
				out += ' ';
				i++;
			}
		} else if ( c == '/' && i + 1 < length && text[i + 1] == '*' ) {
			out += "  ";
			i += 2;
			while ( i < length && !( text[i] == '*' && i + 1 < length && text[i + 1] == '/' ) ) {
				out += ( text[i] == '\n' ) ? '\n' : ' ';
				i++;
			}
			if ( i < length ) {
				out += "  ";
				i += 2;
			}
		} else {
			out += c;
			i++;
		}
	}
	return out;
}

const guideTemplate_t *FindTemplate( const char *name ) {
	for ( int i = 0; i < templates.Num(); i++ ) {
		if ( templates[i].name.Icmp( name ) == 0 ) {
			return &templates[i];
		}
	}
	return NULL;
}

// one argument: trimmed, one pair of surrounding quotes removed
idStr CleanArg( const std::string &raw ) {
	int s = 0, e = (int)raw.size();
	while ( s < e && IsSpace( raw[s] ) ) {
		s++;
	}
	while ( e > s && IsSpace( raw[e - 1] ) ) {
		e--;
	}
	if ( e - s >= 2 && raw[s] == '"' && raw[e - 1] == '"' ) {
		s++;
		e--;
	}
	return idStr( raw.substr( s, e - s ).c_str() );
}

// the template body with each parameter replaced by its argument, in one left-to-right pass
// (an argument that happens to contain a parameter name is never substituted again)
std::string Substitute( const guideTemplate_t &tpl, const idStrList &args ) {
	const char *body = tpl.body.c_str();
	const int length = tpl.body.Length();
	std::string out;
	out.reserve( length + 64 );
	for ( int i = 0; i < length; ) {
		int matched = -1;
		for ( int k = 0; k < tpl.parmsLongestFirst.Num(); k++ ) {
			const idStr &parm = tpl.parms[tpl.parmsLongestFirst[k]];
			if ( parm.Length() && idStr::Cmpn( body + i, parm, parm.Length() ) == 0 ) {
				matched = tpl.parmsLongestFirst[k];
				break;
			}
		}
		if ( matched >= 0 ) {
			if ( matched < args.Num() ) {
				out += args[matched].c_str();
			}
			i += tpl.parms[matched].Length();
		} else {
			out += body[i++];
		}
	}
	return out;
}

bool ExpandText( const char *text, int length, const char *fileName, int firstLine, std::string &out, int depth,
				 std::vector<int> *lineMap );

/*
================
ExpandReference

Parses "<declName> <template>( args )" (guide) or "<template>( args )" (inlineGuide)
starting at text[pos], just past the keyword. On success the expansion is in expansion and
end is the offset just past the closing parenthesis.
================
*/
bool ExpandReference( const char *text, int length, int pos, bool isInline, const char *fileName, int line,
					  int depth, std::string &expansion, int &end ) {
	int p = pos;
	auto skipSpace = [&]() {
		while ( p < length && IsSpace( text[p] ) ) {
			p++;
		}
	};

	std::string declName;
	if ( !isInline ) {
		skipSpace();
		if ( p < length && text[p] == '"' ) {
			p++;
			while ( p < length && text[p] != '"' && text[p] != '\n' ) {
				declName += text[p++];
			}
			if ( p < length && text[p] == '"' ) {
				p++;
			}
		} else {
			while ( p < length && !IsSpace( text[p] ) && text[p] != '(' && text[p] != '{' && text[p] != '}' ) {
				declName += text[p++];
			}
		}
		if ( declName.empty() ) {
			return false;
		}
	}

	skipSpace();
	std::string tplName;
	while ( p < length && IsIdent( text[p] ) ) {
		tplName += text[p++];
	}
	skipSpace();
	if ( tplName.empty() || p >= length || text[p] != '(' ) {
		return false;
	}
	p++;

	// arguments up to the matching ')', split on commas outside quotes and parentheses. Long
	// argument lists continue on the next lines (the depth-sprite guides do); a brace means
	// the reference is malformed.
	idStrList args;
	std::string current;
	int parens = 0;
	bool closed = false;
	while ( p < length ) {
		const char c = text[p];
		if ( c == '"' ) {
			current += text[p++];
			while ( p < length && text[p] != '"' && text[p] != '\n' ) {
				current += text[p++];
			}
			if ( p < length && text[p] == '"' ) {
				current += text[p++];
			}
			continue;
		}
		if ( c == '{' || c == '}' ) {
			break;
		}
		if ( c == '(' ) {
			parens++;
		} else if ( c == ')' ) {
			if ( parens == 0 ) {
				p++;
				closed = true;
				break;
			}
			parens--;
		} else if ( c == ',' && parens == 0 ) {
			args.Append( CleanArg( current ) );
			current.clear();
			p++;
			continue;
		}
		current += c;
		p++;
	}
	if ( !closed ) {
		common->Warning( "%s(%d): malformed %s reference", fileName, line, isInline ? "inlineGuide" : "guide" );
		return false;
	}
	if ( args.Num() > 0 || CleanArg( current ).Length() > 0 ) {
		args.Append( CleanArg( current ) );
	}

	const guideTemplate_t *tpl = FindTemplate( tplName.c_str() );
	if ( tpl == NULL ) {
		common->Warning( "%s(%d): unknown material guide '%s'", fileName, line, tplName.c_str() );
		return false;
	}
	if ( args.Num() != tpl->parms.Num() ) {
		common->Warning( "%s(%d): guide '%s' takes %d argument(s), given %d", fileName, line,
			tpl->name.c_str(), tpl->parms.Num(), args.Num() );
	}

	std::string body = Substitute( *tpl, args );
	if ( depth < MAX_EXPAND_DEPTH ) {
		std::string nested;
		if ( ExpandText( body.c_str(), (int)body.size(), tpl->file, 1, nested, depth + 1, NULL ) ) {
			body.swap( nested );
		}
	}

	if ( isInline ) {
		expansion = body;
	} else {
		expansion = declName + " {" + body + "}";
	}
	end = p;
	return true;
}

/*
================
ExpandText

An expansion keeps the template's newlines: the material parser is line-sensitive
(qer_editorimage skips the rest of its line, renderbump reads it). lineMap, when given,
receives the source line of every output line after the first, so the decl loader can
report the file's own line numbers.
================
*/
bool ExpandText( const char *text, int length, const char *fileName, int firstLine, std::string &out, int depth,
				 std::vector<int> *lineMap ) {
	bool changed = false;
	int braceDepth = 0;
	int line = firstLine;
	out.reserve( out.size() + length + length / 2 );
	auto newLine = [&]( int sourceLine ) {
		if ( lineMap != NULL ) {
			lineMap->push_back( sourceLine );
		}
	};

	int i = 0;
	while ( i < length ) {
		const char c = text[i];

		// comments and strings are copied as they are
		if ( c == '/' && i + 1 < length && ( text[i + 1] == '/' || text[i + 1] == '*' ) ) {
			const int start = i;
			if ( text[i + 1] == '/' ) {
				while ( i < length && text[i] != '\n' ) {
					i++;
				}
			} else {
				i += 2;
				while ( i < length && !( text[i] == '*' && i + 1 < length && text[i + 1] == '/' ) ) {
					if ( text[i] == '\n' ) {
						newLine( ++line );
					}
					i++;
				}
				i = ( i < length ) ? i + 2 : length;
			}
			out.append( text + start, i - start );
			continue;
		}
		if ( c == '"' ) {
			const int start = i++;
			while ( i < length && text[i] != '"' && text[i] != '\n' ) {
				i++;
			}
			if ( i < length && text[i] == '"' ) {
				i++;
			}
			out.append( text + start, i - start );
			continue;
		}

		if ( AtTokenStart( text, i ) && ( c == 'g' || c == 'G' || c == 'i' || c == 'I' ) ) {
			int keywordLength = 0;
			bool isInline = false;
			if ( i + 11 < length && idStr::Icmpn( text + i, "inlineGuide", 11 ) == 0 && !IsIdent( text[i + 11] ) ) {
				keywordLength = 11;
				isInline = true;
			} else if ( braceDepth == 0 && i + 5 < length && idStr::Icmpn( text + i, "guide", 5 ) == 0 && !IsIdent( text[i + 5] ) ) {
				keywordLength = 5;
			}
			if ( keywordLength ) {
				std::string expansion;
				int end = 0;
				if ( ExpandReference( text, length, i + keywordLength, isInline, fileName, line, depth, expansion, end ) ) {
					// the template's lines all belong to the reference's line, then the
					// newlines the reference itself spanned
					out += expansion;
					for ( size_t k = 0; k < expansion.size(); k++ ) {
						if ( expansion[k] == '\n' ) {
							newLine( line );
						}
					}
					for ( int k = i; k < end; k++ ) {
						if ( text[k] == '\n' ) {
							out += '\n';
							newLine( ++line );
						}
					}
					i = end;
					changed = true;
					continue;
				}
			}
		}

		if ( c == '{' ) {
			braceDepth++;
		} else if ( c == '}' ) {
			braceDepth--;
		} else if ( c == '\n' ) {
			newLine( ++line );
		}
		out += c;
		i++;
	}
	return changed;
}

/*
================
LoadTemplates

"guide|inlineGuide <name>( parm, parm ) { body }" definitions in one .guide file.
================
*/
int LoadTemplates( const char *fileName, const char *rawText, int rawLength ) {
	const std::string clean = StripComments( rawText, rawLength );
	const char *text = clean.c_str();
	const int length = (int)clean.size();
	int loaded = 0;

	int i = 0;
	while ( i < length ) {
		if ( text[i] == '"' ) {		// skip strings at file level
			i++;
			while ( i < length && text[i] != '"' && text[i] != '\n' ) {
				i++;
			}
			i++;
			continue;
		}
		int keywordLength = 0;
		if ( AtTokenStart( text, i ) ) {
			if ( i + 11 < length && idStr::Icmpn( text + i, "inlineGuide", 11 ) == 0 && !IsIdent( text[i + 11] ) ) {
				keywordLength = 11;
			} else if ( i + 5 < length && idStr::Icmpn( text + i, "guide", 5 ) == 0 && !IsIdent( text[i + 5] ) ) {
				keywordLength = 5;
			}
		}
		if ( !keywordLength ) {
			i++;
			continue;
		}

		int p = i + keywordLength;
		while ( p < length && IsSpace( text[p] ) ) {
			p++;
		}
		idStr name;
		while ( p < length && IsIdent( text[p] ) ) {
			name += text[p++];
		}
		while ( p < length && IsSpace( text[p] ) ) {
			p++;
		}
		if ( name.Length() == 0 || p >= length || text[p] != '(' ) {
			i = p;
			continue;
		}
		p++;
		guideTemplate_t tpl;
		tpl.name = name;
		tpl.file = fileName;
		idStr parm;
		while ( p < length && text[p] != ')' ) {
			if ( text[p] == ',' ) {
				tpl.parms.Append( CleanArg( parm.c_str() ) );
				parm.Clear();
			} else {
				parm += text[p];
			}
			p++;
		}
		parm = CleanArg( parm.c_str() );
		if ( parm.Length() || tpl.parms.Num() ) {
			tpl.parms.Append( parm );
		}
		p++;	// ')'
		while ( p < length && IsSpace( text[p] ) ) {
			p++;
		}
		if ( p >= length || text[p] != '{' ) {
			common->Warning( "%s: guide '%s' has no body", fileName, name.c_str() );
			i = p;
			continue;
		}
		const int bodyStart = p + 1;
		int depth = 0;
		while ( p < length ) {
			if ( text[p] == '"' ) {
				p++;
				while ( p < length && text[p] != '"' && text[p] != '\n' ) {
					p++;
				}
			} else if ( text[p] == '{' ) {
				depth++;
			} else if ( text[p] == '}' ) {
				if ( --depth == 0 ) {
					break;
				}
			}
			p++;
		}
		if ( p >= length ) {
			common->Warning( "%s: guide '%s' body is not closed", fileName, name.c_str() );
			break;
		}
		tpl.body = clean.substr( bodyStart, p - bodyStart ).c_str();
		i = p + 1;

		if ( tpl.parms.Num() > MAX_GUIDE_PARMS ) {
			common->Warning( "%s: guide '%s' has %d parameters (max %d)", fileName, name.c_str(), tpl.parms.Num(), MAX_GUIDE_PARMS );
		}
		if ( FindTemplate( tpl.name ) != NULL ) {
			common->Warning( "%s: guide '%s' already defined, keeping the first", fileName, name.c_str() );
			continue;
		}
		for ( int k = 0; k < tpl.parms.Num(); k++ ) {
			int at = 0;
			while ( at < tpl.parmsLongestFirst.Num() && tpl.parms[tpl.parmsLongestFirst[at]].Length() >= tpl.parms[k].Length() ) {
				at++;
			}
			tpl.parmsLongestFirst.Insert( k, at );
		}
		templates.Append( tpl );
		loaded++;
	}
	return loaded;
}

}	// anonymous namespace

/*
================
DeclGuides::Clear / Num
================
*/
void DeclGuides::Clear( void ) {
	templates.Clear();
}

int DeclGuides::Num( void ) {
	return templates.Num();
}

/*
================
DeclGuides::Load
================
*/
void DeclGuides::Load( void ) {
	Clear();
	idFileList *files = fileSystem->ListFiles( "guides", ".guide", true );
	int numFiles = 0;
	for ( int i = 0; i < files->GetNumFiles(); i++ ) {
		idStr path = idStr( "guides/" ) + files->GetFile( i );
		char *buffer = NULL;
		const int length = fileSystem->ReadFile( path, (void **)&buffer );
		if ( length <= 0 || buffer == NULL ) {
			continue;
		}
		LoadTemplates( path, buffer, length );
		fileSystem->FreeFile( buffer );
		numFiles++;
	}
	fileSystem->FreeFileList( files );
	if ( templates.Num() ) {
		common->Printf( "%d material guides from %d file(s)\n", templates.Num(), numFiles );
	}
}

/*
================
DeclGuides::Expand
================
*/
bool DeclGuides::Expand( const char *text, int length, const char *fileName, idStr &out, idList<int> &lineMap ) {
	if ( templates.Num() == 0 || text == NULL || length <= 0 ) {
		return false;
	}
	// cheap reject: no "guide" anywhere (also covers "inlineGuide")
	if ( idStr::FindText( text, "guide", false, 0, length ) < 0 ) {
		return false;
	}
	std::string expanded;
	std::vector<int> lines( 1, 1 );
	if ( !ExpandText( text, length, fileName, 1, expanded, 0, &lines ) ) {
		return false;
	}
	out = expanded.c_str();
	lineMap.SetNum( (int)lines.size() );
	for ( size_t k = 0; k < lines.size(); k++ ) {
		lineMap[(int)k] = lines[k];
	}
	return true;
}
