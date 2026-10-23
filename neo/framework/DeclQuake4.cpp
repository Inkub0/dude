/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: Quake 4 decl types - see DeclQuake4.h.

#include "sys/platform.h"
#include "idlib/Lexer.h"
#include "framework/Common.h"
#include "framework/DeclQuake4.h"

// "x,y,z": n comma-separated floats
static bool ParseCommaFloats( idLexer &src, float *out, int n ) {
	for ( int i = 0; i < n; i++ ) {
		if ( i > 0 && !src.ExpectTokenString( "," ) ) {
			return false;
		}
		out[i] = src.ParseFloat();
	}
	return true;
}

/*
===============================================================================

	idDeclMaterialType

===============================================================================
*/

size_t idDeclMaterialType::Size( void ) const {
	return sizeof( idDeclMaterialType ) + description.Allocated();
}

const char *idDeclMaterialType::DefaultDefinition( void ) const {
	return "{\n\tdescription\t\"<DEFAULTED>\"\n\trgb\t0,0,0\n}";
}

void idDeclMaterialType::FreeData( void ) {
	description.Clear();
	rgb[0] = rgb[1] = rgb[2] = 0;
}

bool idDeclMaterialType::Parse( const char *text, const int textLength ) {
	idLexer src;
	idToken token;

	src.LoadMemory( text, textLength, GetFileName(), GetLineNum() );
	src.SetFlags( DECL_LEXER_FLAGS );
	src.SkipUntilString( "{" );
	FreeData();

	while ( src.ReadToken( &token ) ) {
		if ( token == "}" ) {
			return true;
		}
		if ( !token.Icmp( "description" ) ) {
			src.ReadToken( &token );
			description = token;
		} else if ( !token.Icmp( "rgb" ) ) {
			float c[3];
			if ( !ParseCommaFloats( src, c, 3 ) ) {
				src.Warning( "materialType '%s': bad rgb", GetName() );
				MakeDefault();
				return false;
			}
			for ( int i = 0; i < 3; i++ ) {
				rgb[i] = (byte)idMath::ClampInt( 0, 255, (int)c[i] );
			}
		} else {
			src.Warning( "materialType '%s': unknown key '%s'", GetName(), token.c_str() );
			MakeDefault();
			return false;
		}
	}
	src.Warning( "materialType '%s': missing }", GetName() );
	MakeDefault();
	return false;
}

/*
===============================================================================

	idDeclLipSync

===============================================================================
*/

size_t idDeclLipSync::Size( void ) const {
	return sizeof( idDeclLipSync ) + description.Allocated() + lipText.Allocated() + visemes.Allocated();
}

const char *idDeclLipSync::DefaultDefinition( void ) const {
	return "{\n\tdescription\t\"<DEFAULTED>\"\n}";
}

void idDeclLipSync::FreeData( void ) {
	description.Clear();
	lipText.Clear();
	visemes.Clear();
}

bool idDeclLipSync::Parse( const char *text, const int textLength ) {
	idLexer src;
	idToken token, value;

	src.LoadMemory( text, textLength, GetFileName(), GetLineNum() );
	src.SetFlags( DECL_LEXER_FLAGS );
	src.SkipUntilString( "{" );
	FreeData();

	while ( src.ReadToken( &token ) ) {
		if ( token == "}" ) {
			return true;
		}
		if ( !token.Icmp( "description" ) ) {
			src.ReadToken( &value );
			description = value;
		} else if ( !token.Icmp( "text" ) ) {
			src.ReadToken( &value );
			lipText = value;
		} else if ( !token.Icmp( "visemes" ) ) {
			idToken language;
			src.ReadToken( &language );
			src.ReadToken( &value );
			visemes.Set( language, value );
		} else {
			src.Warning( "lipSync '%s': unknown key '%s'", GetName(), token.c_str() );
			MakeDefault();
			return false;
		}
	}
	src.Warning( "lipSync '%s': missing }", GetName() );
	MakeDefault();
	return false;
}

/*
===============================================================================

	idDeclPlayback

===============================================================================
*/

size_t idDeclPlayback::Size( void ) const {
	return sizeof( idDeclPlayback ) + frames.Allocated();
}

const char *idDeclPlayback::DefaultDefinition( void ) const {
	return "{\n\tsequence\n\t{\n\t\torigin\t0,0,0\n\t\tframeRate\t15\n\t}\n\tdata\n\t{\n\t}\n}";
}

void idDeclPlayback::FreeData( void ) {
	origin.Zero();
	frameRate = 15.0f;
	frames.Clear();
}

bool idDeclPlayback::Parse( const char *text, const int textLength ) {
	idLexer src;
	idToken token;

	src.LoadMemory( text, textLength, GetFileName(), GetLineNum() );
	src.SetFlags( DECL_LEXER_FLAGS );
	src.SkipUntilString( "{" );
	FreeData();

	while ( src.ReadToken( &token ) ) {
		if ( token == "}" ) {
			return true;
		}
		if ( !token.Icmp( "sequence" ) ) {
			if ( !src.ExpectTokenString( "{" ) ) {
				break;
			}
			while ( src.ReadToken( &token ) && token != "}" ) {
				if ( !token.Icmp( "origin" ) ) {
					if ( !ParseCommaFloats( src, origin.ToFloatPtr(), 3 ) ) {
						break;
					}
				} else if ( !token.Icmp( "frameRate" ) ) {
					frameRate = src.ParseFloat();
				} else {
					src.Warning( "playback '%s': unknown sequence key '%s'", GetName(), token.c_str() );
					MakeDefault();
					return false;
				}
			}
		} else if ( !token.Icmp( "data" ) ) {
			if ( !src.ExpectTokenString( "{" ) ) {
				break;
			}
			// one { } per frame, each with zero or more of pos / ang / rotate
			while ( src.ReadToken( &token ) && token != "}" ) {
				if ( token != "{" ) {
					src.Warning( "playback '%s': expected a frame, found '%s'", GetName(), token.c_str() );
					MakeDefault();
					return false;
				}
				playbackFrame_t frame;
				memset( &frame, 0, sizeof( frame ) );
				while ( src.ReadToken( &token ) && token != "}" ) {
					if ( !token.Icmp( "pos" ) ) {
						frame.flags |= PLAYBACK_POS;
						ParseCommaFloats( src, frame.pos.ToFloatPtr(), 3 );
					} else if ( !token.Icmp( "ang" ) ) {
						frame.flags |= PLAYBACK_ANG;
						ParseCommaFloats( src, frame.ang.ToFloatPtr(), 2 );
					} else if ( !token.Icmp( "rotate" ) ) {
						frame.flags |= PLAYBACK_ROTATE;
						ParseCommaFloats( src, frame.rotate.ToFloatPtr(), 3 );
					} else {
						src.Warning( "playback '%s': unknown frame key '%s'", GetName(), token.c_str() );
						MakeDefault();
						return false;
					}
				}
				frames.Append( frame );
			}
		} else {
			src.Warning( "playback '%s': unknown key '%s'", GetName(), token.c_str() );
			MakeDefault();
			return false;
		}
	}
	src.Warning( "playback '%s': missing }", GetName() );
	MakeDefault();
	return false;
}

/*
===============================================================================

	idDeclEffect

===============================================================================
*/

size_t idDeclEffect::Size( void ) const {
	return sizeof( idDeclEffect );
}

const char *idDeclEffect::DefaultDefinition( void ) const {
	return "{\n\tsize\t0\n}";
}

void idDeclEffect::FreeData( void ) {
	size = 0.0f;
	numSegments = 0;
}

bool idDeclEffect::Parse( const char *text, const int textLength ) {
	idLexer src;
	idToken token;

	src.LoadMemory( text, textLength, GetFileName(), GetLineNum() );
	src.SetFlags( DECL_LEXER_FLAGS );
	src.SkipUntilString( "{" );
	FreeData();

	// top level: "size N" and segments "<type> [name] { ... }" (emitter, spawner, sound,
	// decal, light, shake, delay, trail, doubleVision, tunnel, effect); a segment's body is
	// only brace-checked here
	while ( src.ReadToken( &token ) ) {
		if ( token == "}" ) {
			return true;
		}
		idToken next;
		if ( !src.ReadToken( &next ) ) {
			break;
		}
		// effect properties: "size 449", "cutOffDistance 2048"
		if ( next.type == TT_NUMBER || next == "-" ) {
			src.UnreadToken( &next );
			const float value = src.ParseFloat();
			if ( !token.Icmp( "size" ) ) {
				size = value;
			}
			continue;
		}
		if ( next != "{" ) {		// a named segment: emitter "bubbles2" { ... }
			if ( !src.ReadToken( &next ) || next != "{" ) {
				src.Warning( "effect '%s': segment '%s' has no body", GetName(), token.c_str() );
				MakeDefault();
				return false;
			}
		}
		src.UnreadToken( &next );
		if ( !src.SkipBracedSection() ) {
			src.Warning( "effect '%s': segment '%s' is not closed", GetName(), token.c_str() );
			MakeDefault();
			return false;
		}
		numSegments++;
	}
	src.Warning( "effect '%s': missing }", GetName() );
	MakeDefault();
	return false;
}
