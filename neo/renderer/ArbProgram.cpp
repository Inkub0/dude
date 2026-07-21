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

#include "renderer/ArbProgram.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>

namespace arb {

bool Program::HasOption( const char *name ) const {
	for ( size_t i = 0; i < options.size(); i++ ) {
		if ( options[i] == name ) {
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// lexer
// ---------------------------------------------------------------------------

enum TokKind {
	T_Ident,
	T_Number,
	T_Punct,
	T_End
};

struct Token {
	TokKind		kind;
	std::string	text;		// ident / punct text
	double		number;		// T_Number value
	int			line;
};

struct Lexer {
	const std::string	&src;
	size_t				pos;
	int					line;

	Lexer( const std::string &s ) : src( s ), pos( 0 ), line( 1 ) {}

	void SkipWhitespaceAndComments() {
		while ( pos < src.size() ) {
			char c = src[pos];
			if ( c == '\n' ) {
				line++;
				pos++;
			} else if ( c == ' ' || c == '\t' || c == '\r' ) {
				pos++;
			} else if ( c == '#' ) {
				while ( pos < src.size() && src[pos] != '\n' ) {
					pos++;
				}
			} else {
				break;
			}
		}
	}

	Token Next() {
		SkipWhitespaceAndComments();
		Token t;
		t.line = line;
		t.number = 0.0;
		if ( pos >= src.size() ) {
			t.kind = T_End;
			return t;
		}
		char c = src[pos];
		// identifier: letter/_/$ start, then letter/digit/_/$
		if ( isalpha( (unsigned char)c ) || c == '_' || c == '$' ) {
			size_t start = pos;
			while ( pos < src.size() &&
					( isalnum( (unsigned char)src[pos] ) || src[pos] == '_' || src[pos] == '$' ) ) {
				pos++;
			}
			t.kind = T_Ident;
			t.text = src.substr( start, pos - start );
			return t;
		}
		// number: digits [ . digits ] [ e[+-]digits ]
		if ( isdigit( (unsigned char)c ) ) {
			size_t start = pos;
			while ( pos < src.size() && isdigit( (unsigned char)src[pos] ) ) pos++;
			if ( pos < src.size() && src[pos] == '.' &&
				 pos + 1 < src.size() && isdigit( (unsigned char)src[pos + 1] ) ) {
				pos++;
				while ( pos < src.size() && isdigit( (unsigned char)src[pos] ) ) pos++;
			}
			if ( pos < src.size() && ( src[pos] == 'e' || src[pos] == 'E' ) ) {
				size_t save = pos;
				pos++;
				if ( pos < src.size() && ( src[pos] == '+' || src[pos] == '-' ) ) pos++;
				if ( pos < src.size() && isdigit( (unsigned char)src[pos] ) ) {
					while ( pos < src.size() && isdigit( (unsigned char)src[pos] ) ) pos++;
				} else {
					pos = save;		// not an exponent (e.g. "2D" texture target)
				}
			}
			t.kind = T_Number;
			t.text = src.substr( start, pos - start );
			t.number = atof( t.text.c_str() );
			return t;
		}
		// single-char punctuation
		t.kind = T_Punct;
		t.text = std::string( 1, c );
		pos++;
		return t;
	}
};

// ---------------------------------------------------------------------------
// parser
// ---------------------------------------------------------------------------

struct NamedRef {			// PARAM / ATTRIB / OUTPUT / ALIAS resolution entry
	Ref			ref;
};

struct Parser {
	Lexer				lex;
	Token				tok;			// lookahead
	Program				&prog;
	std::string			error;
	int					errorLine;
	bool				failed;

	std::map<std::string, int>		temps;		// name -> index
	std::map<std::string, int>		addresses;	// name -> index
	std::map<std::string, NamedRef>	named;		// PARAM/ATTRIB/OUTPUT/ALIAS name -> ref

	Parser( const std::string &text, Program &p )
		: lex( text ), prog( p ), errorLine( 0 ), failed( false ) {
		tok = lex.Next();
	}

	void Fail( const std::string &msg ) {
		if ( !failed ) {
			failed = true;
			error = msg;
			errorLine = tok.line;
		}
	}

	void Advance()					{ tok = lex.Next(); }

	bool IsPunct( const char *p ) const {
		return tok.kind == T_Punct && tok.text == p;
	}
	bool AcceptPunct( const char *p ) {
		if ( IsPunct( p ) ) { Advance(); return true; }
		return false;
	}
	void ExpectPunct( const char *p ) {
		if ( !AcceptPunct( p ) ) {
			Fail( std::string( "expected '" ) + p + "', got '" + tok.text + "'" );
		}
	}
	std::string ExpectIdent() {
		if ( tok.kind != T_Ident ) {
			Fail( "expected identifier, got '" + tok.text + "'" );
			return "";
		}
		std::string s = tok.text;
		Advance();
		return s;
	}
	// number with optional leading sign
	float ExpectNumber() {
		float sign = 1.0f;
		if ( IsPunct( "-" ) ) { sign = -1.0f; Advance(); }
		else if ( IsPunct( "+" ) ) { Advance(); }
		if ( tok.kind != T_Number ) {
			Fail( "expected number, got '" + tok.text + "'" );
			return 0.0f;
		}
		float v = (float)tok.number;
		Advance();
		return sign * v;
	}
	int ExpectIndex() {				// [ n ]
		ExpectPunct( "[" );
		if ( tok.kind != T_Number ) {
			Fail( "expected array index, got '" + tok.text + "'" );
			return 0;
		}
		int n = (int)tok.number;
		Advance();
		ExpectPunct( "]" );
		return n;
	}
	int OptionalIndex( int def ) {	// [n] if present, else def
		if ( IsPunct( "[" ) ) {
			return ExpectIndex();
		}
		return def;
	}

	// ---- swizzle / write mask ----------------------------------------------

	// two component alphabets: xyzw, and rgba (ARB_fragment_program only);
	// an individual swizzle may not mix them
	static bool IsSwizzleText( const std::string &s ) {
		if ( s.empty() || s.size() > 4 ) return false;
		bool xyzw = true, rgba = true;
		for ( size_t i = 0; i < s.size(); i++ ) {
			char c = s[i];
			if ( c != 'x' && c != 'y' && c != 'z' && c != 'w' ) xyzw = false;
			if ( c != 'r' && c != 'g' && c != 'b' && c != 'a' ) rgba = false;
		}
		return xyzw || rgba;
	}
	static int SwizzleChar( char c ) {
		switch ( c ) {
			case 'x': case 'r': return 0;
			case 'y': case 'g': return 1;
			case 'z': case 'b': return 2;
			default:            return 3;	// w / a
		}
	}

	// optional .swizzle after a source ref
	void ParseOptionalSwizzle( SrcOperand &op ) {
		if ( !IsPunct( "." ) ) return;
		Advance();
		if ( tok.kind != T_Ident || !IsSwizzleText( tok.text ) ) {
			Fail( "expected swizzle after '.', got '" + tok.text + "'" );
			return;
		}
		const std::string &s = tok.text;
		if ( s.size() == 1 ) {
			int c = SwizzleChar( s[0] );
			op.swizzle[0] = op.swizzle[1] = op.swizzle[2] = op.swizzle[3] = c;
		} else if ( s.size() == 4 ) {
			for ( int i = 0; i < 4; i++ ) op.swizzle[i] = SwizzleChar( s[i] );
		} else {
			// 2/3-component selects aren't valid ARB source swizzles
			Fail( "invalid source swizzle '." + s + "' (must be 1 or 4 components)" );
			return;
		}
		Advance();
	}

	// optional .mask after a dst ref
	void ParseOptionalWriteMask( DstOperand &op ) {
		if ( !IsPunct( "." ) ) return;
		Advance();
		if ( tok.kind != T_Ident || !IsSwizzleText( tok.text ) ) {
			Fail( "expected write mask after '.', got '" + tok.text + "'" );
			return;
		}
		unsigned mask = 0;
		int prev = -1;
		for ( size_t i = 0; i < tok.text.size(); i++ ) {
			int c = SwizzleChar( tok.text[i] );
			if ( c <= prev ) {
				Fail( "write mask components must be in xyzw order" );
				return;
			}
			prev = c;
			mask |= 1u << c;
		}
		op.writeMask = mask;
		Advance();
	}

	// ---- references --------------------------------------------------------

	// { a [, b [, c [, d]]] }  with ARB padding: missing y,z = 0, w = 1
	Ref ParseBraceConstant() {
		Ref r;
		r.base = RB_Const;
		ExpectPunct( "{" );
		float v[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		int n = 0;
		v[n++] = ExpectNumber();
		while ( AcceptPunct( "," ) && n < 4 ) {
			v[n++] = ExpectNumber();
		}
		ExpectPunct( "}" );
		for ( int i = 0; i < 4; i++ ) r.constVal[i] = v[i];
		return r;
	}

	Ref ParseVertexRef() {			// after 'vertex' '.'
		Ref r;
		std::string comp = ExpectIdent();
		if ( comp == "position" )		{ r.base = RB_VertexPosition; }
		else if ( comp == "texcoord" )	{ r.base = RB_VertexTexcoord; r.index = OptionalIndex( 0 ); }
		else if ( comp == "normal" )	{ r.base = RB_VertexNormal; }
		else if ( comp == "color" )		{ r.base = RB_VertexColor; }
		else if ( comp == "attrib" )	{ r.base = RB_VertexAttrib; r.index = ExpectIndex(); }
		else { Fail( "unsupported vertex binding 'vertex." + comp + "'" ); }
		return r;
	}

	Ref ParseFragmentRef() {		// after 'fragment' '.'
		Ref r;
		std::string comp = ExpectIdent();
		if ( comp == "position" )		{ r.base = RB_FragmentPosition; }
		else if ( comp == "texcoord" )	{ r.base = RB_FragmentTexcoord; r.index = OptionalIndex( 0 ); }
		else if ( comp == "color" )		{ r.base = RB_FragmentColor; }
		else { Fail( "unsupported fragment binding 'fragment." + comp + "'" ); }
		return r;
	}

	Ref ParseProgramRef() {			// after 'program' '.'
		Ref r;
		std::string comp = ExpectIdent();
		if ( comp == "env" )		{ r.base = RB_Env; r.index = ExpectIndex(); }
		else if ( comp == "local" )	{ r.base = RB_Local; r.index = ExpectIndex(); }
		else { Fail( "unsupported program binding 'program." + comp + "'" ); }
		return r;
	}

	Ref ParseStateRef() {			// after 'state' '.'
		Ref r;
		std::string comp = ExpectIdent();
		if ( comp != "matrix" ) {
			Fail( "unsupported state binding 'state." + comp + "' (only state.matrix.* supported)" );
			return r;
		}
		ExpectPunct( "." );
		std::string mat = ExpectIdent();
		r.base = RB_StateMatrixRow;
		if ( mat == "mvp" )				{ r.matIndex = SM_MVP; }
		else if ( mat == "modelview" )	{ r.matIndex = SM_ModelView; r.matIndex2 = OptionalIndex( 0 ); }
		else if ( mat == "projection" )	{ r.matIndex = SM_Projection; }
		else if ( mat == "texture" )	{ r.matIndex = SM_Texture; r.matIndex2 = OptionalIndex( 0 ); }
		else {
			Fail( "unsupported state matrix 'state.matrix." + mat + "'" );
			return r;
		}
		ExpectPunct( "." );
		std::string rowIdent = ExpectIdent();
		if ( rowIdent != "row" ) {
			Fail( "unsupported state matrix modifier '." + rowIdent + "' (only .row[n] supported)" );
			return r;
		}
		r.index = ExpectIndex();
		return r;
	}

	Ref ParseResultRef() {			// after 'result' '.'
		Ref r;
		std::string comp = ExpectIdent();
		if ( comp == "position" )		{ r.base = RB_ResultPosition; }
		else if ( comp == "color" )		{ r.base = RB_ResultColor; }
		else if ( comp == "texcoord" )	{ r.base = RB_ResultTexcoord; r.index = OptionalIndex( 0 ); }
		else if ( comp == "fogcoord" )	{ r.base = RB_ResultFogcoord; }
		else if ( comp == "pointsize" )	{ r.base = RB_ResultPointsize; }
		else { Fail( "unsupported result binding 'result." + comp + "'" ); }
		return r;
	}

	// full reference: named symbol, register file path, or literal constant.
	// swizzle/mask is handled by the caller (differs for src vs dst).
	Ref ParseRefCore() {
		Ref r;
		if ( IsPunct( "{" ) ) {
			return ParseBraceConstant();
		}
		if ( tok.kind == T_Number ) {
			// bare scalar literal: replicated across all components
			float v = (float)tok.number;
			Advance();
			r.base = RB_Const;
			r.constVal[0] = r.constVal[1] = r.constVal[2] = r.constVal[3] = v;
			return r;
		}
		if ( tok.kind != T_Ident ) {
			Fail( "expected operand, got '" + tok.text + "'" );
			return r;
		}
		std::string name = tok.text;

		if ( name == "vertex" )		{ Advance(); ExpectPunct( "." ); return ParseVertexRef(); }
		if ( name == "fragment" )	{ Advance(); ExpectPunct( "." ); return ParseFragmentRef(); }
		if ( name == "program" )	{ Advance(); ExpectPunct( "." ); return ParseProgramRef(); }
		if ( name == "state" )		{ Advance(); ExpectPunct( "." ); return ParseStateRef(); }
		if ( name == "result" )		{ Advance(); ExpectPunct( "." ); return ParseResultRef(); }

		std::map<std::string, int>::const_iterator ti = temps.find( name );
		if ( ti != temps.end() ) {
			Advance();
			r.base = RB_Temp;
			r.index = ti->second;
			return r;
		}
		ti = addresses.find( name );
		if ( ti != addresses.end() ) {
			Advance();
			r.base = RB_Address;
			r.index = ti->second;
			return r;
		}
		std::map<std::string, NamedRef>::const_iterator ni = named.find( name );
		if ( ni != named.end() ) {
			Advance();
			return ni->second.ref;
		}
		Fail( "undeclared identifier '" + name + "'" );
		return r;
	}

	SrcOperand ParseSrc() {
		SrcOperand op;
		if ( AcceptPunct( "-" ) ) {
			op.negate = true;
		} else if ( AcceptPunct( "+" ) ) {
			// explicit plus, ignore
		}
		// negative scalar literal: '-' consumed above, number follows
		if ( op.negate && tok.kind == T_Number ) {
			float v = (float)tok.number;
			Advance();
			op.negate = false;
			op.ref.base = RB_Const;
			op.ref.constVal[0] = op.ref.constVal[1] = op.ref.constVal[2] = op.ref.constVal[3] = -v;
			ParseOptionalSwizzle( op );
			return op;
		}
		op.ref = ParseRefCore();
		ParseOptionalSwizzle( op );
		return op;
	}

	DstOperand ParseDst() {
		DstOperand op;
		op.ref = ParseRefCore();
		switch ( op.ref.base ) {
			case RB_Temp:
			case RB_Address:
			case RB_ResultPosition:
			case RB_ResultColor:
			case RB_ResultTexcoord:
			case RB_ResultFogcoord:
			case RB_ResultPointsize:
				break;
			default:
				Fail( "destination operand must be a TEMP or result.* register" );
		}
		ParseOptionalWriteMask( op );
		return op;
	}

	// ---- declarations ------------------------------------------------------

	void ParseTempDecl() {			// TEMP a, b, c ;
		do {
			std::string name = ExpectIdent();
			if ( failed ) return;
			temps[name] = (int)prog.tempNames.size();
			prog.tempNames.push_back( name );
		} while ( AcceptPunct( "," ) );
		ExpectPunct( ";" );
	}

	void ParseAddressDecl() {		// ADDRESS a ;
		do {
			std::string name = ExpectIdent();
			if ( failed ) return;
			addresses[name] = (int)prog.addressNames.size();
			prog.addressNames.push_back( name );
		} while ( AcceptPunct( "," ) );
		ExpectPunct( ";" );
	}

	void ParseParamDecl() {			// PARAM name = <const|program.*|state.*> ;
		std::string name = ExpectIdent();
		if ( IsPunct( "[" ) ) {
			Fail( "PARAM arrays are not supported yet ('" + name + "[]')" );
			return;
		}
		ExpectPunct( "=" );
		NamedRef nr;
		if ( IsPunct( "{" ) || tok.kind == T_Number || IsPunct( "-" ) ) {
			if ( IsPunct( "{" ) ) {
				nr.ref = ParseBraceConstant();
			} else {
				float v = ExpectNumber();
				nr.ref.base = RB_Const;
				nr.ref.constVal[0] = nr.ref.constVal[1] = nr.ref.constVal[2] = nr.ref.constVal[3] = v;
			}
		} else {
			nr.ref = ParseRefCore();	// program.env[n], state.matrix..., etc.
		}
		if ( failed ) return;
		named[name] = nr;
		ExpectPunct( ";" );
	}

	void ParseAttribDecl() {		// ATTRIB name = vertex.* / fragment.* ;
		std::string name = ExpectIdent();
		ExpectPunct( "=" );
		NamedRef nr;
		nr.ref = ParseRefCore();
		if ( failed ) return;
		named[name] = nr;
		ExpectPunct( ";" );
	}

	void ParseOutputDecl() {		// OUTPUT name = result.* ;
		std::string name = ExpectIdent();
		ExpectPunct( "=" );
		NamedRef nr;
		nr.ref = ParseRefCore();
		if ( failed ) return;
		named[name] = nr;
		ExpectPunct( ";" );
	}

	void ParseAliasDecl() {			// ALIAS a = b ;
		std::string name = ExpectIdent();
		ExpectPunct( "=" );
		NamedRef nr;
		nr.ref = ParseRefCore();
		if ( failed ) return;
		named[name] = nr;
		ExpectPunct( ";" );
	}

	// ---- instructions ------------------------------------------------------

	// arity by opcode; -1 = unknown
	static int OpcodeArity( const std::string &op ) {
		static const char *arity1[] = { "MOV", "ABS", "FLR", "FRC", "LIT", "RCP", "RSQ",
										"EX2", "LG2", "EXP", "LOG", "SIN", "COS", "SCS", NULL };
		static const char *arity2[] = { "ADD", "SUB", "MUL", "DP3", "DP4", "DPH", "DST",
										"MIN", "MAX", "SLT", "SGE", "XPD", "POW", NULL };
		static const char *arity3[] = { "MAD", "CMP", "LRP", NULL };
		for ( int i = 0; arity1[i]; i++ ) if ( op == arity1[i] ) return 1;
		for ( int i = 0; arity2[i]; i++ ) if ( op == arity2[i] ) return 2;
		for ( int i = 0; arity3[i]; i++ ) if ( op == arity3[i] ) return 3;
		return -1;
	}

	TexTarget ParseTexTarget() {
		// targets: 1D / 2D / 3D (number+ident tokens), CUBE, RECT
		if ( tok.kind == T_Number ) {
			int n = (int)tok.number;
			Advance();
			std::string suffix = ExpectIdent();
			if ( suffix == "D" ) {
				if ( n == 1 ) return TT_1D;
				if ( n == 2 ) return TT_2D;
				if ( n == 3 ) return TT_3D;
			}
			Fail( "unknown texture target" );
			return TT_None;
		}
		std::string t = ExpectIdent();
		if ( t == "CUBE" ) return TT_Cube;
		if ( t == "RECT" ) return TT_Rect;
		Fail( "unknown texture target '" + t + "'" );
		return TT_None;
	}

	void ParseInstruction( const std::string &rawOpcode, int lineNo ) {
		Instruction inst;
		inst.line = lineNo;
		inst.opcode = rawOpcode;

		// _SAT suffix (fragment programs)
		size_t satPos = inst.opcode.rfind( "_SAT" );
		if ( satPos != std::string::npos && satPos == inst.opcode.size() - 4 ) {
			inst.saturate = true;
			inst.opcode = inst.opcode.substr( 0, satPos );
		}

		if ( inst.opcode == "KIL" ) {
			inst.hasDst = false;
			inst.srcs.push_back( ParseSrc() );
			ExpectPunct( ";" );
			if ( !failed ) prog.instructions.push_back( inst );
			return;
		}

		if ( inst.opcode == "TEX" || inst.opcode == "TXP" || inst.opcode == "TXB" ) {
			inst.dst = ParseDst();
			ExpectPunct( "," );
			inst.srcs.push_back( ParseSrc() );
			ExpectPunct( "," );
			std::string texIdent = ExpectIdent();
			if ( texIdent != "texture" ) {
				Fail( "expected 'texture[n]' in " + inst.opcode );
				return;
			}
			inst.texUnit = OptionalIndex( 0 );
			ExpectPunct( "," );
			inst.texTarget = ParseTexTarget();
			ExpectPunct( ";" );
			if ( !failed ) prog.instructions.push_back( inst );
			return;
		}

		if ( inst.opcode == "SWZ" ) {
			inst.dst = ParseDst();
			ExpectPunct( "," );
			SrcOperand src;
			src.ref = ParseRefCore();		// no swizzle on SWZ source
			inst.srcs.push_back( src );
			for ( int i = 0; i < 4; i++ ) {
				ExpectPunct( "," );
				SwzComponent sc;
				if ( AcceptPunct( "-" ) ) sc.negate = true;
				if ( tok.kind == T_Number ) {
					int v = (int)tok.number;
					if ( v == 0 ) sc.sel = 4;
					else if ( v == 1 ) sc.sel = 5;
					else { Fail( "SWZ component literal must be 0 or 1" ); return; }
					Advance();
				} else if ( tok.kind == T_Ident && tok.text.size() == 1 &&
							IsSwizzleText( tok.text ) ) {
					sc.sel = SwizzleChar( tok.text[0] );
					Advance();
				} else {
					Fail( "bad SWZ component '" + tok.text + "'" );
					return;
				}
				inst.swz[i] = sc;
			}
			ExpectPunct( ";" );
			if ( !failed ) prog.instructions.push_back( inst );
			return;
		}

		if ( inst.opcode == "ARL" ) {
			inst.dst = ParseDst();
			ExpectPunct( "," );
			inst.srcs.push_back( ParseSrc() );
			ExpectPunct( ";" );
			if ( !failed ) prog.instructions.push_back( inst );
			return;
		}

		int arity = OpcodeArity( inst.opcode );
		if ( arity < 0 ) {
			Fail( "unknown opcode '" + rawOpcode + "'" );
			return;
		}
		inst.dst = ParseDst();
		for ( int i = 0; i < arity; i++ ) {
			ExpectPunct( "," );
			inst.srcs.push_back( ParseSrc() );
		}
		ExpectPunct( ";" );
		if ( !failed ) prog.instructions.push_back( inst );
	}

	// ---- top level ---------------------------------------------------------

	void Run() {
		while ( !failed ) {
			if ( tok.kind == T_End ) {
				Fail( "unexpected end of program (missing END)" );
				return;
			}
			if ( tok.kind != T_Ident ) {
				Fail( "expected statement, got '" + tok.text + "'" );
				return;
			}
			std::string kw = tok.text;
			int lineNo = tok.line;
			Advance();

			if ( kw == "END" )				{ return; }
			else if ( kw == "OPTION" ) {
				std::string name = ExpectIdent();
				prog.options.push_back( name );
				ExpectPunct( ";" );
			}
			else if ( kw == "TEMP" )		{ ParseTempDecl(); }
			else if ( kw == "PARAM" )		{ ParseParamDecl(); }
			else if ( kw == "ATTRIB" )		{ ParseAttribDecl(); }
			else if ( kw == "OUTPUT" )		{ ParseOutputDecl(); }
			else if ( kw == "ALIAS" )		{ ParseAliasDecl(); }
			else if ( kw == "ADDRESS" )		{ ParseAddressDecl(); }
			else							{ ParseInstruction( kw, lineNo ); }
		}
	}
};

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

ParseResult Parse( const std::string &text ) {
	ParseResult res;

	// find the !!ARB header
	size_t hdr = text.find( "!!ARB" );
	if ( hdr == std::string::npos ) {
		res.error = "no !!ARBvp1.0 / !!ARBfp1.0 header found";
		return res;
	}
	// count preceding lines so diagnostics match the original file
	int startLine = 1;
	for ( size_t i = 0; i < hdr; i++ ) {
		if ( text[i] == '\n' ) startLine++;
	}

	ProgramKind kind;
	size_t bodyStart;
	if ( text.compare( hdr, 10, "!!ARBvp1.0" ) == 0 ) {
		kind = PK_Vertex;
		bodyStart = hdr + 10;
	} else if ( text.compare( hdr, 10, "!!ARBfp1.0" ) == 0 ) {
		kind = PK_Fragment;
		bodyStart = hdr + 10;
	} else {
		res.error = "unrecognized program header";
		res.errorLine = startLine;
		return res;
	}

	res.program.kind = kind;
	std::string body = text.substr( bodyStart );

	Parser parser( body, res.program );
	parser.lex.line = startLine;		// header shares its line with what follows
	parser.tok.line = startLine;
	// re-prime lookahead with corrected line base
	parser.lex.pos = 0;
	parser.lex.line = startLine;
	parser.tok = parser.lex.Next();
	parser.Run();

	if ( parser.failed ) {
		res.error = parser.error;
		res.errorLine = parser.errorLine;
		return res;
	}
	res.ok = true;
	return res;
}

std::vector<std::string> SplitSections( const std::string &fileText ) {
	std::vector<std::string> sections;
	size_t pos = 0;
	while ( true ) {
		size_t start = fileText.find( "!!ARB", pos );
		if ( start == std::string::npos ) {
			break;
		}
		size_t next = fileText.find( "!!ARB", start + 5 );
		size_t end = ( next == std::string::npos ) ? fileText.size() : next;
		sections.push_back( fileText.substr( start, end - start ) );
		pos = end;
	}
	return sections;
}

} // namespace arb
