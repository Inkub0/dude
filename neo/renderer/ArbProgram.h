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

#ifndef __ARBPROGRAM_H__
#define __ARBPROGRAM_H__

// Parser for ARB_vertex_program 1.0 / ARB_fragment_program 1.0 assembly,
// the shader format used by Doom 3 materials (glprogs/*.vfp). First stage of
// the ARB -> GLSL transpiler (docs/vulkan-port.md, "Mod compatibility scope").
//
// Deliberately engine-independent (std C++ only) so it compiles both into the
// renderer and into the standalone `arbtool` corpus tester.

#include <string>
#include <vector>

namespace arb {

enum ProgramKind {
	PK_Vertex,
	PK_Fragment
};

// what a register reference resolves to (aliases/named params resolved by the parser)
enum RefBase {
	RB_Invalid,
	RB_Temp,				// declared TEMP; index = temp table index
	RB_Address,				// declared ADDRESS (vp only); index = table index
	RB_Const,				// inline or named { ... } constant; value in constVal
	RB_Env,					// program.env[index]
	RB_Local,				// program.local[index]
	RB_StateMatrixRow,		// state.matrix.<matrix>.row[index]; matrix in matIndex
	RB_VertexPosition,		// vertex.position
	RB_VertexTexcoord,		// vertex.texcoord[index]
	RB_VertexNormal,		// vertex.normal
	RB_VertexColor,			// vertex.color
	RB_VertexAttrib,		// vertex.attrib[index]
	RB_FragmentPosition,	// fragment.position
	RB_FragmentTexcoord,	// fragment.texcoord[index]
	RB_FragmentColor,		// fragment.color
	RB_ResultPosition,		// result.position (vp dst)
	RB_ResultColor,			// result.color (dst)
	RB_ResultTexcoord,		// result.texcoord[index] (vp dst)
	RB_ResultFogcoord,		// result.fogcoord (vp dst)
	RB_ResultPointsize		// result.pointsize (vp dst)
};

// state.matrix.<X> selectors for RB_StateMatrixRow
enum StateMatrix {
	SM_MVP,
	SM_ModelView,
	SM_Projection,
	SM_Texture		// state.matrix.texture[matIndex2]
};

struct Ref {
	RefBase		base;
	int			index;			// register / env / local / texcoord number
	int			matIndex;		// StateMatrix enum for RB_StateMatrixRow
	int			matIndex2;		// texture matrix number, row number in `index`
	float		constVal[4];	// RB_Const value (already padded/replicated)

	Ref() : base( RB_Invalid ), index( 0 ), matIndex( 0 ), matIndex2( 0 ) {
		constVal[0] = constVal[1] = constVal[2] = constVal[3] = 0.0f;
	}
};

struct SrcOperand {
	Ref			ref;
	bool		negate;
	int			swizzle[4];		// 0..3 = x..w; identity is {0,1,2,3}

	SrcOperand() : negate( false ) {
		swizzle[0] = 0; swizzle[1] = 1; swizzle[2] = 2; swizzle[3] = 3;
	}
	bool IsIdentitySwizzle() const {
		return swizzle[0] == 0 && swizzle[1] == 1 && swizzle[2] == 2 && swizzle[3] == 3;
	}
	bool IsScalarSwizzle() const {	// .x / .y / .z / .w replicate
		return swizzle[0] == swizzle[1] && swizzle[1] == swizzle[2] && swizzle[2] == swizzle[3];
	}
};

struct DstOperand {
	Ref			ref;
	unsigned	writeMask;		// bit 0..3 = x..w; 0xF = all

	DstOperand() : writeMask( 0xF ) {}
};

// extended swizzle component for SWZ: value 0, 1, or component 0..3, negatable
struct SwzComponent {
	bool		negate;
	int			sel;			// 0..3 = x..w, 4 = literal 0, 5 = literal 1
	SwzComponent() : negate( false ), sel( 4 ) {}
};

enum TexTarget {
	TT_None,
	TT_1D,
	TT_2D,
	TT_3D,
	TT_Cube,
	TT_Rect
};

struct Instruction {
	std::string	opcode;			// canonical, without _SAT
	bool		saturate;
	bool		hasDst;
	DstOperand	dst;
	std::vector<SrcOperand> srcs;
	// TEX/TXP/TXB only:
	int			texUnit;
	TexTarget	texTarget;
	// SWZ only:
	SwzComponent swz[4];
	int			line;			// 1-based source line for diagnostics

	Instruction() : saturate( false ), hasDst( true ), texUnit( -1 ), texTarget( TT_None ), line( 0 ) {}
};

struct Program {
	ProgramKind	kind;
	std::vector<std::string> options;		// OPTION names, e.g. ARB_position_invariant
	std::vector<std::string> tempNames;		// TEMP decls, index = RB_Temp ref index
	std::vector<std::string> addressNames;	// ADDRESS decls (vp)
	std::vector<Instruction> instructions;

	bool HasOption( const char *name ) const;

	Program() : kind( PK_Vertex ) {}
};

struct ParseResult {
	bool		ok;
	std::string	error;			// human-readable, includes line number
	int			errorLine;
	Program		program;

	ParseResult() : ok( false ), errorLine( 0 ) {}
};

// Parse one !!ARBvp1.0 / !!ARBfp1.0 program. `text` starts at (or before) the
// !!ARB header; parsing stops at END.
ParseResult Parse( const std::string &text );

// Split a .vfp file (which may hold a vertex and a fragment program) into
// per-program chunks, each beginning with its !!ARB header.
std::vector<std::string> SplitSections( const std::string &fileText );

} // namespace arb

#endif /* !__ARBPROGRAM_H__ */
