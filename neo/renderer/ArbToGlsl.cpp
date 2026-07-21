/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.cpp for license header)
===========================================================================
*/

#include "renderer/ArbToGlsl.h"

#include <cstdio>
#include <set>
#include <sstream>

namespace arb {

// varying locations: texcoord[n] -> n, color -> 8, fogcoord -> 9
static const int VARY_COLOR = 8;
static const int VARY_FOG = 9;

struct Gen {
	const Program	&prog;
	std::ostringstream body;
	std::string		error;
	bool			failed;

	std::set<int>	varyingsTc;		// texcoord varyings used
	bool			varyingColor;
	bool			varyingFog;
	std::set<int>	attribsUsed;	// RefBase values for vp inputs
	std::set<int>	texcoordAttribs;// vertex.texcoord[n] / attrib[8]
	int				samplerTargets[8];	// TexTarget per unit, TT_None if unused
	bool			usesFragCoord;
	bool			writesPosition;	// vp: explicit result.position writes
	bool			writesPointSize;

	Gen( const Program &p ) : prog( p ), failed( false ),
		varyingColor( false ), varyingFog( false ),
		usesFragCoord( false ), writesPosition( false ), writesPointSize( false ) {
		for ( int i = 0; i < 8; i++ ) samplerTargets[i] = TT_None;
	}

	void Fail( const std::string &msg ) {
		if ( !failed ) { failed = true; error = msg; }
	}

	static std::string FloatStr( float f ) {
		char buf[64];
		snprintf( buf, sizeof( buf ), "%.9g", f );
		std::string s = buf;
		// ensure it lexes as float
		if ( s.find( '.' ) == std::string::npos && s.find( 'e' ) == std::string::npos &&
			 s.find( "inf" ) == std::string::npos && s.find( "nan" ) == std::string::npos ) {
			s += ".0";
		}
		return s;
	}

	// scan pass: find inputs/outputs/samplers so declarations can be emitted first
	void Scan() {
		for ( size_t i = 0; i < prog.instructions.size(); i++ ) {
			const Instruction &in = prog.instructions[i];
			if ( in.texUnit >= 0 ) {
				if ( in.texUnit > 7 ) { Fail( "texture unit > 7" ); return; }
				if ( samplerTargets[in.texUnit] != TT_None &&
					 samplerTargets[in.texUnit] != in.texTarget ) {
					Fail( "texture unit used with two different targets" );
					return;
				}
				samplerTargets[in.texUnit] = in.texTarget;
			}
			for ( size_t s = 0; s < in.srcs.size(); s++ ) {
				ScanRef( in.srcs[s].ref, false );
			}
			if ( in.hasDst ) {
				ScanRef( in.dst.ref, true );
			}
		}
	}

	void ScanRef( const Ref &r, bool isDst ) {
		switch ( r.base ) {
			case RB_VertexPosition:
			case RB_VertexNormal:
			case RB_VertexColor:
				attribsUsed.insert( r.base );
				break;
			case RB_VertexTexcoord:
				attribsUsed.insert( r.base );
				break;
			case RB_VertexAttrib:
				if ( r.index == 8 ) attribsUsed.insert( RB_VertexTexcoord );
				else if ( r.index >= 9 && r.index <= 11 ) attribsUsed.insert( 1000 + r.index );
				else Fail( "unsupported vertex.attrib index" );
				break;
			case RB_FragmentPosition:
				usesFragCoord = true;
				break;
			case RB_FragmentTexcoord:
				varyingsTc.insert( r.index );
				break;
			case RB_FragmentColor:
				varyingColor = true;
				break;
			case RB_ResultTexcoord:
				varyingsTc.insert( r.index );
				break;
			case RB_ResultColor:
				if ( prog.kind == PK_Vertex ) varyingColor = true;
				break;
			case RB_ResultPosition:
				if ( isDst ) writesPosition = true;
				break;
			case RB_ResultFogcoord:
				varyingFog = true;
				break;
			case RB_ResultPointsize:
				writesPointSize = true;
				break;
			default:
				break;
		}
	}

	// ---- operand strings ---------------------------------------------------

	// base vec4 expression for a ref (no swizzle/negate)
	std::string RefStr( const Ref &r ) {
		char buf[128];
		switch ( r.base ) {
			case RB_Temp:
				return "t_" + prog.tempNames[r.index];
			case RB_Const:
				return "vec4( " + FloatStr( r.constVal[0] ) + ", " + FloatStr( r.constVal[1] ) +
					   ", " + FloatStr( r.constVal[2] ) + ", " + FloatStr( r.constVal[3] ) + " )";
			case RB_Env:
				if ( r.index > 31 ) { Fail( "program.env index > 31" ); return "vec4(0.0)"; }
				snprintf( buf, sizeof( buf ), "u_env[%d]", r.index );
				return buf;
			case RB_Local:
				if ( r.index > 7 ) { Fail( "program.local index > 7" ); return "vec4(0.0)"; }
				snprintf( buf, sizeof( buf ), "u_local[%d]", r.index );
				return buf;
			case RB_StateMatrixRow: {
				const char *m = "u_mvpMatrix";
				if ( r.matIndex == SM_ModelView ) m = "u_modelViewMatrix";
				else if ( r.matIndex == SM_Projection ) m = "u_projectionMatrix";
				else if ( r.matIndex == SM_Texture ) m = "u_textureMatrix";
				// row n of column-major mat4
				snprintf( buf, sizeof( buf ), "vec4( %s[0][%d], %s[1][%d], %s[2][%d], %s[3][%d] )",
						  m, r.index, m, r.index, m, r.index, m, r.index );
				return buf;
			}
			case RB_VertexPosition:		return "attr_Position";
			case RB_VertexTexcoord:		return "vec4( attr_TexCoord, 0.0, 1.0 )";
			case RB_VertexNormal:		return "vec4( attr_Normal, 1.0 )";
			case RB_VertexColor:		return "attr_Color";
			case RB_VertexAttrib:
				if ( r.index == 8 )  return "vec4( attr_TexCoord, 0.0, 1.0 )";
				if ( r.index == 9 )  return "vec4( attr_Tangent, 1.0 )";
				if ( r.index == 10 ) return "vec4( attr_Bitangent, 1.0 )";
				if ( r.index == 11 ) return "vec4( attr_Normal, 1.0 )";
				Fail( "unsupported vertex.attrib" );
				return "vec4(0.0)";
			case RB_FragmentPosition:	return "gl_FragCoord";
			case RB_FragmentTexcoord:
				snprintf( buf, sizeof( buf ), "var_tc%d", r.index );
				return buf;
			case RB_FragmentColor:		return "var_color";
			default:
				Fail( "unsupported source operand" );
				return "vec4(0.0)";
		}
	}

	std::string SrcStr( const SrcOperand &op ) {
		std::string s = RefStr( op.ref );
		if ( !op.IsIdentitySwizzle() ) {
			static const char comp[4] = { 'x', 'y', 'z', 'w' };
			s = "(" + s + ").";
			for ( int i = 0; i < 4; i++ ) s += comp[op.swizzle[i]];
		}
		if ( op.negate ) {
			s = "-(" + s + ")";
		}
		return s;
	}

	// scalar operand for RCP/RSQ/EX2/...: selected component of the swizzled vec4
	std::string SrcScalar( const SrcOperand &op ) {
		return "(" + SrcStr( op ) + ").x";
	}

	// dst variable name (temps and outputs); position handled by caller
	std::string DstStr( const Ref &r ) {
		char buf[64];
		switch ( r.base ) {
			case RB_Temp:				return "t_" + prog.tempNames[r.index];
			case RB_ResultColor:
				return ( prog.kind == PK_Fragment ) ? "fragColor" : "var_color";
			case RB_ResultTexcoord:
				snprintf( buf, sizeof( buf ), "var_tc%d", r.index );
				return buf;
			case RB_ResultPosition:		return "r_position";
			case RB_ResultFogcoord:		return "var_fog";
			case RB_ResultPointsize:	return "r_pointSize";
			default:
				Fail( "unsupported destination operand" );
				return "t_bad";
		}
	}

	static std::string MaskStr( unsigned mask ) {
		std::string s = ".";
		if ( mask & 1 ) s += 'x';
		if ( mask & 2 ) s += 'y';
		if ( mask & 4 ) s += 'z';
		if ( mask & 8 ) s += 'w';
		return s;
	}

	// emit: { vec4 res = <expr>; [saturate] dst.<mask> = res.<mask>; }
	void EmitAssign( const Instruction &in, const std::string &expr, bool scalar ) {
		std::string res = scalar ? "vec4( " + expr + " )" : expr;
		if ( in.saturate ) {
			res = "clamp( " + res + ", 0.0, 1.0 )";
		}
		std::string dst = DstStr( in.dst.ref );
		if ( in.dst.writeMask == 0xF ) {
			body << "\t" << dst << " = " << res << ";\n";
		} else {
			std::string m = MaskStr( in.dst.writeMask );
			body << "\t{ vec4 res_ = " << res << "; " << dst << m << " = res_" << m << "; }\n";
		}
	}

	// ---- instruction translation ------------------------------------------

	void EmitInstruction( const Instruction &in ) {
		const std::string &op = in.opcode;
		#define S0 SrcStr( in.srcs[0] )
		#define S1 SrcStr( in.srcs[1] )
		#define S2 SrcStr( in.srcs[2] )

		if ( op == "MOV" )		{ EmitAssign( in, S0, false ); return; }
		if ( op == "ADD" )		{ EmitAssign( in, "(" + S0 + " + " + S1 + ")", false ); return; }
		if ( op == "SUB" )		{ EmitAssign( in, "(" + S0 + " - " + S1 + ")", false ); return; }
		if ( op == "MUL" )		{ EmitAssign( in, "(" + S0 + " * " + S1 + ")", false ); return; }
		if ( op == "MAD" )		{ EmitAssign( in, "(" + S0 + " * " + S1 + " + " + S2 + ")", false ); return; }
		if ( op == "DP3" )		{ EmitAssign( in, "dot( (" + S0 + ").xyz, (" + S1 + ").xyz )", true ); return; }
		if ( op == "DP4" )		{ EmitAssign( in, "dot( " + S0 + ", " + S1 + " )", true ); return; }
		if ( op == "DPH" )		{ EmitAssign( in, "( dot( (" + S0 + ").xyz, (" + S1 + ").xyz ) + (" + S1 + ").w )", true ); return; }
		if ( op == "MIN" )		{ EmitAssign( in, "min( " + S0 + ", " + S1 + " )", false ); return; }
		if ( op == "MAX" )		{ EmitAssign( in, "max( " + S0 + ", " + S1 + " )", false ); return; }
		if ( op == "SLT" )		{ EmitAssign( in, "vec4( lessThan( " + S0 + ", " + S1 + " ) )", false ); return; }
		if ( op == "SGE" )		{ EmitAssign( in, "vec4( greaterThanEqual( " + S0 + ", " + S1 + " ) )", false ); return; }
		if ( op == "ABS" )		{ EmitAssign( in, "abs( " + S0 + " )", false ); return; }
		if ( op == "FLR" )		{ EmitAssign( in, "floor( " + S0 + " )", false ); return; }
		if ( op == "FRC" )		{ EmitAssign( in, "fract( " + S0 + " )", false ); return; }
		if ( op == "RCP" )		{ EmitAssign( in, "( 1.0 / " + SrcScalar( in.srcs[0] ) + " )", true ); return; }
		if ( op == "RSQ" )		{ EmitAssign( in, "inversesqrt( abs( " + SrcScalar( in.srcs[0] ) + " ) )", true ); return; }
		if ( op == "EX2" )		{ EmitAssign( in, "exp2( " + SrcScalar( in.srcs[0] ) + " )", true ); return; }
		if ( op == "LG2" )		{ EmitAssign( in, "log2( " + SrcScalar( in.srcs[0] ) + " )", true ); return; }
		if ( op == "POW" )		{ EmitAssign( in, "pow( " + SrcScalar( in.srcs[0] ) + ", " + SrcScalar( in.srcs[1] ) + " )", true ); return; }
		if ( op == "SIN" )		{ EmitAssign( in, "sin( " + SrcScalar( in.srcs[0] ) + " )", true ); return; }
		if ( op == "COS" )		{ EmitAssign( in, "cos( " + SrcScalar( in.srcs[0] ) + " )", true ); return; }
		if ( op == "SCS" ) {
			EmitAssign( in, "vec4( cos( " + SrcScalar( in.srcs[0] ) + " ), sin( " + SrcScalar( in.srcs[0] ) + " ), 0.0, 1.0 )", false );
			return;
		}
		if ( op == "XPD" )		{ EmitAssign( in, "vec4( cross( (" + S0 + ").xyz, (" + S1 + ").xyz ), 0.0 )", false ); return; }
		if ( op == "CMP" ) {	// dst = (a < 0) ? b : c, per component
			EmitAssign( in, "mix( " + S2 + ", " + S1 + ", vec4( lessThan( " + S0 + ", vec4( 0.0 ) ) ) )", false );
			return;
		}
		if ( op == "LRP" ) {	// dst = a*b + (1-a)*c
			EmitAssign( in, "mix( " + S2 + ", " + S1 + ", " + S0 + " )", false );
			return;
		}
		if ( op == "DST" ) {
			EmitAssign( in, "vec4( 1.0, (" + S0 + ").y * (" + S1 + ").y, (" + S0 + ").z, (" + S1 + ").w )", false );
			return;
		}
		if ( op == "LIT" ) {
			body << "\t{ vec4 a_ = " << S0 << "; float p_ = clamp( a_.w, -127.9961, 127.9961 );\n"
				 << "\t  vec4 lit_ = vec4( 1.0, max( a_.x, 0.0 ), ( a_.x > 0.0 ) ? pow( max( a_.y, 0.0 ), p_ ) : 0.0, 1.0 );\n";
			{
				std::string dst = DstStr( in.dst.ref );
				std::string m = ( in.dst.writeMask == 0xF ) ? std::string( "" ) : MaskStr( in.dst.writeMask );
				if ( m.empty() ) body << "\t  " << dst << " = lit_; }\n";
				else body << "\t  " << dst << m << " = lit_" << m << "; }\n";
			}
			return;
		}
		if ( op == "EXP" ) {	// vp approx: x=2^floor(a), y=fract(a), z=2^a, w=1
			body << "\t{ float a_ = " << SrcScalar( in.srcs[0] ) << ";\n";
			std::string expr = "vec4( exp2( floor( a_ ) ), fract( a_ ), exp2( a_ ), 1.0 )";
			std::string dst = DstStr( in.dst.ref );
			std::string m = ( in.dst.writeMask == 0xF ) ? std::string( "" ) : MaskStr( in.dst.writeMask );
			if ( m.empty() ) body << "\t  " << dst << " = " << expr << "; }\n";
			else body << "\t  " << dst << m << " = (" << expr << ")" << m << "; }\n";
			return;
		}
		if ( op == "LOG" ) {	// vp approx: x=floor(log2|a|), y=|a|/2^floor(log2|a|), z=log2|a|, w=1
			body << "\t{ float a_ = abs( " << SrcScalar( in.srcs[0] ) << " ); float l_ = log2( a_ );\n";
			std::string expr = "vec4( floor( l_ ), a_ / exp2( floor( l_ ) ), l_, 1.0 )";
			std::string dst = DstStr( in.dst.ref );
			std::string m = ( in.dst.writeMask == 0xF ) ? std::string( "" ) : MaskStr( in.dst.writeMask );
			if ( m.empty() ) body << "\t  " << dst << " = " << expr << "; }\n";
			else body << "\t  " << dst << m << " = (" << expr << ")" << m << "; }\n";
			return;
		}
		if ( op == "KIL" ) {
			body << "\tif ( any( lessThan( " << S0 << ", vec4( 0.0 ) ) ) ) { discard; }\n";
			return;
		}
		if ( op == "TEX" || op == "TXP" || op == "TXB" ) {
			char sampler[32];
			snprintf( sampler, sizeof( sampler ), "u_tex%d", in.texUnit );
			std::string coord = S0;
			std::string call;
			switch ( in.texTarget ) {
				case TT_2D:
					if ( op == "TXP" )		call = "textureProj( " + std::string( sampler ) + ", " + coord + " )";
					else if ( op == "TXB" )	call = "texture( " + std::string( sampler ) + ", (" + coord + ").xy, (" + coord + ").w )";
					else					call = "texture( " + std::string( sampler ) + ", (" + coord + ").xy )";
					break;
				case TT_Cube:
					call = "texture( " + std::string( sampler ) + ", (" + coord + ").xyz )";
					break;
				case TT_3D:
					if ( op == "TXP" )		call = "textureProj( " + std::string( sampler ) + ", " + coord + " )";
					else					call = "texture( " + std::string( sampler ) + ", (" + coord + ").xyz )";
					break;
				default:
					Fail( "unsupported texture target (1D/RECT)" );
					return;
			}
			EmitAssign( in, call, false );
			return;
		}
		if ( op == "SWZ" ) {
			std::string src = RefStr( in.srcs[0].ref );
			std::string expr = "vec4( ";
			static const char comp[4] = { 'x', 'y', 'z', 'w' };
			for ( int i = 0; i < 4; i++ ) {
				const SwzComponent &sc = in.swz[i];
				std::string c;
				if ( sc.sel == 4 )		c = "0.0";
				else if ( sc.sel == 5 )	c = "1.0";
				else					c = "(" + src + ")." + std::string( 1, comp[sc.sel] );
				if ( sc.negate )		c = "-(" + c + ")";
				expr += c + ( i < 3 ? ", " : " )" );
			}
			EmitAssign( in, expr, false );
			return;
		}
		if ( op == "ARL" ) {
			Fail( "ARL / relative addressing not supported" );
			return;
		}
		Fail( "unhandled opcode " + op );
		#undef S0
		#undef S1
		#undef S2
	}

	// ---- whole program -----------------------------------------------------

	std::string Generate() {
		Scan();
		if ( failed ) return "";

		std::ostringstream out;
		out << "// transpiled from ARB " << ( prog.kind == PK_Vertex ? "vertex" : "fragment" )
			<< " program by ArbToGlsl (DUDE)\n\n#include \"arbparams.glsl\"\n\n";

		bool isVp = ( prog.kind == PK_Vertex );

		if ( isVp ) {
			// attributes
			out << "layout(location = 0) in vec4 attr_Position;\n";
			if ( attribsUsed.count( RB_VertexTexcoord ) )	out << "layout(location = 1) in vec2 attr_TexCoord;\n";
			if ( attribsUsed.count( RB_VertexNormal ) || attribsUsed.count( 1000 + 11 ) )
				out << "layout(location = 2) in vec3 attr_Normal;\n";
			if ( attribsUsed.count( 1000 + 9 ) )			out << "layout(location = 3) in vec3 attr_Tangent;\n";
			if ( attribsUsed.count( 1000 + 10 ) )			out << "layout(location = 4) in vec3 attr_Bitangent;\n";
			if ( attribsUsed.count( RB_VertexColor ) )		out << "layout(location = 5) in vec4 attr_Color;\n";
			// varyings out
			for ( std::set<int>::iterator it = varyingsTc.begin(); it != varyingsTc.end(); ++it )
				out << "VARY(" << *it << ") out vec4 var_tc" << *it << ";\n";
			if ( varyingColor )	out << "VARY(" << VARY_COLOR << ") out vec4 var_color;\n";
			if ( varyingFog )	out << "VARY(" << VARY_FOG << ") out vec4 var_fog;\n";
		} else {
			for ( std::set<int>::iterator it = varyingsTc.begin(); it != varyingsTc.end(); ++it )
				out << "VARY(" << *it << ") in vec4 var_tc" << *it << ";\n";
			if ( varyingColor )	out << "VARY(" << VARY_COLOR << ") in vec4 var_color;\n";
			if ( varyingFog )	out << "VARY(" << VARY_FOG << ") in vec4 var_fog;\n";
			out << "layout(location = 0) out vec4 fragColor;\n";
		}
		// samplers
		for ( int i = 0; i < 8; i++ ) {
			if ( samplerTargets[i] == TT_None ) continue;
			const char *st = "sampler2D";
			if ( samplerTargets[i] == TT_Cube ) st = "samplerCube";
			else if ( samplerTargets[i] == TT_3D ) st = "sampler3D";
			out << "SAMPLER_BINDING(" << i << ") uniform " << st << " u_tex" << i << ";\n";
		}

		// main
		out << "\nvoid main() {\n";
		for ( size_t i = 0; i < prog.tempNames.size(); i++ )
			out << "\tvec4 t_" << prog.tempNames[i] << " = vec4( 0.0 );\n";
		if ( isVp && writesPosition )	out << "\tvec4 r_position = vec4( 0.0 );\n";
		if ( isVp && writesPointSize )	out << "\tvec4 r_pointSize = vec4( 0.0 );\n";
		if ( isVp ) {
			// ARB default texcoord components (0,0,0,1); harmless, more defined
			for ( std::set<int>::iterator it = varyingsTc.begin(); it != varyingsTc.end(); ++it )
				out << "\tvar_tc" << *it << " = vec4( 0.0, 0.0, 0.0, 1.0 );\n";
		}

		for ( size_t i = 0; i < prog.instructions.size(); i++ ) {
			EmitInstruction( prog.instructions[i] );
			if ( failed ) return "";
		}
		out << body.str();

		if ( isVp ) {
			if ( prog.HasOption( "ARB_position_invariant" ) || !writesPosition ) {
				out << "\tgl_Position = u_mvpMatrix * attr_Position;\n";
			} else {
				out << "\tgl_Position = r_position;\n";
			}
			if ( writesPointSize )
				out << "\tgl_PointSize = r_pointSize.x;\n";
		}
		out << "}\n";
		return out.str();
	}
};

TranspileResult ToGlsl( const Program &prog ) {
	TranspileResult res;
	Gen g( prog );
	std::string glsl = g.Generate();
	if ( g.failed ) {
		res.error = g.error;
		return res;
	}
	res.ok = true;
	res.glsl = glsl;
	return res;
}

} // namespace arb
