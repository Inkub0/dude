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

static const float PLAYBACK_DEFAULT_FRAMERATE = 15.0f;

size_t idDeclPlayback::Size( void ) const {
	return sizeof( idDeclPlayback ) + buttons.Allocated() +
		points.GetNumValues() * ( sizeof( idVec3 ) + sizeof( float ) ) + angles.GetNumValues() * ( sizeof( idAngles ) + sizeof( float ) );
}

const char *idDeclPlayback::DefaultDefinition( void ) const {
	return "{\n\tsequence\n\t{\n\t\torigin\t0,0,0\n\t\tframeRate\t15\n\t}\n\tdata\n\t{\n\t}\n}";
}

void idDeclPlayback::FreeData( void ) {
	flags = 0;
	frameRate = PLAYBACK_DEFAULT_FRAMERATE;
	duration = 0.0f;
	origin.Zero();
	bounds.Clear();
	points.Clear();
	angles.Clear();
	buttons.Clear();
}

// sequence { origin x,y,z  frameRate f  destination ... }
bool idDeclPlayback::ParseSequence( idLexer &src ) {
	idToken token;

	if ( !src.ExpectTokenString( "{" ) ) {
		return false;
	}
	while ( src.ReadToken( &token ) ) {
		if ( token == "}" ) {
			if ( frameRate <= 0.0f ) {
				frameRate = PLAYBACK_DEFAULT_FRAMERATE;
			}
			return true;
		}
		if ( !token.Icmp( "origin" ) ) {
			if ( !ParseCommaFloats( src, origin.ToFloatPtr(), 3 ) ) {
				return false;
			}
		} else if ( !token.Icmp( "frameRate" ) ) {
			frameRate = src.ParseFloat();
		} else if ( !token.Icmp( "destination" ) ) {
			src.SkipRestOfLine();		// the editor's note of where the path leads
		} else {
			src.Warning( "playback '%s': unknown sequence key '%s'", GetName(), token.c_str() );
			return false;
		}
	}
	return false;
}

// data { { pos x,y,z  ang p,y  rotate p,y,r } ... }: a sample per frame; a key a sample leaves out
// keeps the previous sample's value
bool idDeclPlayback::ParseData( idLexer &src ) {
	idToken token;
	idVec3 pos( 0.0f, 0.0f, 0.0f );
	idAngles ang( 0.0f, 0.0f, 0.0f );
	int numSamples = 0;

	if ( !src.ExpectTokenString( "{" ) ) {
		return false;
	}
	while ( src.ReadToken( &token ) ) {
		if ( token == "}" ) {
			duration = numSamples / frameRate;
			return true;
		}
		if ( token != "{" ) {
			src.Warning( "playback '%s': expected a sample, found '%s'", GetName(), token.c_str() );
			return false;
		}
		while ( src.ReadToken( &token ) && token != "}" ) {
			if ( !token.Icmp( "pos" ) ) {
				if ( !ParseCommaFloats( src, pos.ToFloatPtr(), 3 ) ) {
					return false;
				}
				flags |= PBFL_GET_POSITION;
			} else if ( !token.Icmp( "ang" ) ) {
				if ( !ParseCommaFloats( src, ang.ToFloatPtr(), 2 ) ) {
					return false;
				}
				ang.roll = 0.0f;
				flags |= PBFL_GET_ANGLES;
			} else if ( !token.Icmp( "rotate" ) ) {
				if ( !ParseCommaFloats( src, ang.ToFloatPtr(), 3 ) ) {
					return false;
				}
				flags |= PBFL_GET_ANGLES;
			} else if ( !token.Icmp( "down" ) || !token.Icmp( "up" ) || !token.Icmp( "impulse" ) ) {
				src.ParseInt();				// buttons belong in the buttons block
			} else {
				src.Warning( "playback '%s': unknown sample key '%s'", GetName(), token.c_str() );
				return false;
			}
		}
		const float time = numSamples / frameRate;
		points.AddValue( time, pos );
		angles.AddValue( time, ang );
		numSamples++;
	}
	return false;
}

// buttons { { time  down n  up n  impulse n } ... }: from each time on, the buttons held then
bool idDeclPlayback::ParseButtons( idLexer &src ) {
	idToken token;
	byte held = 0;

	if ( !src.ExpectTokenString( "{" ) ) {
		return false;
	}
	while ( src.ReadToken( &token ) ) {
		if ( token == "}" ) {
			return true;
		}
		if ( token != "{" ) {
			src.Warning( "playback '%s': expected a button change, found '%s'", GetName(), token.c_str() );
			return false;
		}
		playbackButtons_t change;
		change.time = src.ParseFloat();
		change.impulse = 0;
		while ( src.ReadToken( &token ) && token != "}" ) {
			if ( !token.Icmp( "down" ) ) {
				held |= (byte)src.ParseInt();
			} else if ( !token.Icmp( "up" ) ) {
				held &= ~(byte)src.ParseInt();
			} else if ( !token.Icmp( "impulse" ) ) {
				change.impulse = (byte)src.ParseInt();
			} else {
				src.Warning( "playback '%s': unknown button key '%s'", GetName(), token.c_str() );
				return false;
			}
		}
		change.buttons = held;
		buttons.Append( change );
		flags |= PBFL_GET_BUTTONS;
	}
	return false;
}

bool idDeclPlayback::Parse( const char *text, const int textLength ) {
	idLexer src;
	idToken token;

	src.LoadMemory( text, textLength, GetFileName(), GetLineNum() );
	src.SetFlags( DECL_LEXER_FLAGS );
	src.SkipUntilString( "{" );
	FreeData();

	while ( src.ReadToken( &token ) ) {
		bool ok = true;
		if ( token == "}" ) {
			MovePathToOrigin();
			return true;
		}
		if ( !token.Icmp( "sequence" ) ) {
			ok = ParseSequence( src );
		} else if ( !token.Icmp( "data" ) ) {
			ok = ParseData( src );
		} else if ( !token.Icmp( "buttons" ) ) {
			ok = ParseButtons( src );
		} else {
			src.Warning( "playback '%s': unknown key '%s'", GetName(), token.c_str() );
			ok = false;
		}
		if ( !ok ) {
			MakeDefault();
			return false;
		}
	}
	src.Warning( "playback '%s': missing }", GetName() );
	MakeDefault();
	return false;
}

// the path relative to its first point, which joins the origin
void idDeclPlayback::MovePathToOrigin( void ) {
	bounds.Clear();
	if ( points.GetNumValues() == 0 ) {
		return;
	}
	const idVec3 first = points.GetValue( 0 );
	origin += first;
	for ( int i = 0; i < points.GetNumValues(); i++ ) {
		const idVec3 p = points.GetValue( i ) - first;
		points.SetValue( i, p );
		bounds.AddPoint( p );
	}
}

bool idDeclPlayback::GetCurrentData( int control, float localTime, float lastTime, idPlaybackData *pbd ) const {
	const bool finished = localTime >= duration;
	if ( pbd == NULL ) {
		return finished;
	}
	const float t = idMath::ClampFloat( 0.0f, duration, localTime );
	const float t0 = Min( idMath::ClampFloat( 0.0f, duration, lastTime ), t );

	if ( ( flags & PBFL_GET_POSITION ) && points.GetNumValues() > 0 ) {
		if ( control & PBFL_GET_POSITION ) {
			pbd->SetPosition( origin + points.GetCurrentValue( t ) );
		}
		if ( control & ( PBFL_GET_VELOCITY | PBFL_GET_ANGLES_FROM_VEL ) ) {
			const idVec3 velocity = points.GetCurrentFirstDerivative( t );
			if ( control & PBFL_GET_VELOCITY ) {
				pbd->SetVelocity( velocity );
			}
			if ( ( control & PBFL_GET_ANGLES_FROM_VEL ) && velocity != vec3_origin ) {
				pbd->SetAngles( velocity.ToAngles() );
			}
		}
		if ( control & PBFL_GET_ACCELERATION ) {
			pbd->SetAcceleration( points.GetCurrentSecondDerivative( t ) );
		}
	}
	if ( ( control & PBFL_GET_ANGLES ) && ( flags & PBFL_GET_ANGLES ) && angles.GetNumValues() > 0 ) {
		pbd->SetAngles( angles.GetCurrentValue( t ) );
	}

	if ( ( control & PBFL_GET_BUTTONS ) && buttons.Num() > 0 ) {
		// the buttons held at lastTime, then every change up to now, in order
		byte held = 0;
		int next = 0;
		while ( next < buttons.Num() && buttons[next].time <= t0 ) {
			held = buttons[next].buttons;
			next++;
		}
		const byte heldBefore = held;
		byte impulse = 0;
		for ( ; next < buttons.Num() && buttons[next].time <= t; next++ ) {
			const playbackButtons_t &change = buttons[next];
			const float when = change.time - t0;		// how far into this stretch it happened
			const byte down = change.buttons & ~held;
			const byte up = held & ~change.buttons;
			if ( down ) {
				pbd->SetChanged( down );
				pbd->SetImpulse( 0 );
				pbd->CallCallback( PBCB_BUTTON_DOWN, when );
			}
			if ( up ) {
				pbd->SetChanged( up );
				pbd->SetImpulse( 0 );
				pbd->CallCallback( PBCB_BUTTON_UP, when );
			}
			if ( change.impulse ) {
				impulse = change.impulse;
				pbd->SetChanged( 0 );
				pbd->SetImpulse( impulse );
				pbd->CallCallback( PBCB_IMPULSE, when );
			}
			held = change.buttons;
		}
		pbd->SetButtons( held );
		pbd->SetChanged( heldBefore ^ held );
		pbd->SetImpulse( impulse );
	}
	return finished;
}

void idDeclPlayback::StartRecording( void ) {
	const float rate = frameRate > 0.0f ? frameRate : PLAYBACK_DEFAULT_FRAMERATE;
	FreeData();
	frameRate = rate;
}

bool idDeclPlayback::RecordSample( float localTime, int control, const idPlaybackData *pbd ) {
	if ( pbd == NULL ) {
		return false;
	}
	if ( control & PBFL_GET_POSITION ) {
		points.AddValue( localTime, pbd->GetPosition() );
		flags |= PBFL_GET_POSITION;
	}
	if ( control & PBFL_GET_ANGLES ) {
		angles.AddValue( localTime, pbd->GetAngles() );
		flags |= PBFL_GET_ANGLES;
	}
	if ( control & PBFL_GET_BUTTONS ) {
		const byte held = buttons.Num() ? buttons[buttons.Num() - 1].buttons : 0;
		if ( pbd->GetButtons() != held || pbd->GetImpulse() ) {
			playbackButtons_t change;
			change.time = localTime;
			change.buttons = pbd->GetButtons();
			change.impulse = pbd->GetImpulse();
			buttons.Append( change );
			flags |= PBFL_GET_BUTTONS;
		}
	}
	duration = Max( duration, localTime );
	return true;
}

bool idDeclPlayback::FinishRecording( void ) {
	MovePathToOrigin();

	// the recording as the decl's text, one sample per frame
	idStr text = va( "playback %s\n{\n\tsequence\n\t{\n\t\torigin\t%s\n\t\tframeRate\t%g\n\t}\n\tdata\n\t{\n",
					GetName(), idStr( va( "%g,%g,%g", origin.x, origin.y, origin.z ) ).c_str(), frameRate );
	const int numSamples = idMath::Ftoi( idMath::Ceil( duration * frameRate ) );
	for ( int i = 0; i < numSamples; i++ ) {
		const float t = i / frameRate;
		text += "\t\t{";
		if ( flags & PBFL_GET_POSITION ) {
			const idVec3 p = points.GetCurrentValue( t );
			text += va( " pos %g,%g,%g", p.x, p.y, p.z );
		}
		if ( flags & PBFL_GET_ANGLES ) {
			const idAngles a = angles.GetCurrentValue( t );
			text += va( " rotate %g,%g,%g", a.pitch, a.yaw, a.roll );
		}
		text += " }\n";
	}
	text += "\t}\n";
	if ( buttons.Num() ) {
		text += "\tbuttons\n\t{\n";
		byte held = 0;
		for ( int i = 0; i < buttons.Num(); i++ ) {
			const playbackButtons_t &change = buttons[i];
			text += va( "\t\t{ %g", change.time );
			if ( change.buttons & ~held ) {
				text += va( " down %d", change.buttons & ~held );
			}
			if ( held & ~change.buttons ) {
				text += va( " up %d", held & ~change.buttons );
			}
			if ( change.impulse ) {
				text += va( " impulse %d", change.impulse );
			}
			text += " }\n";
			held = change.buttons;
		}
		text += "\t}\n";
	}
	text += "}\n";

	SetText( text );
	return ReplaceSourceFileText();
}

/*
===============================================================================

	idDeclPlayerModel

===============================================================================
*/

size_t idDeclPlayerModel::Size( void ) const {
	return idDeclEntityDef::Size() + sizeof( idDeclPlayerModel ) - sizeof( idDeclEntityDef ) + model.Allocated() + head.Allocated() +
		uiHead.Allocated() + team.Allocated() + skin.Allocated() + description.Allocated() + sounds.Allocated();
}

const char *idDeclPlayerModel::DefaultDefinition( void ) const {
	return "{\n\t\"model\"\t\"model_player_marine\"\n}";
}

void idDeclPlayerModel::FreeData( void ) {
	idDeclEntityDef::FreeData();
	model.Clear();
	head.Clear();
	headOffset.Zero();
	uiHead.Clear();
	team.Clear();
	skin.Clear();
	description.Clear();
	sounds.Clear();
}

bool idDeclPlayerModel::Parse( const char *text, const int textLength ) {
	FreeData();
	if ( !idDeclEntityDef::Parse( text, textLength ) ) {
		return false;
	}
	model = dict.GetString( "model" );
	head = dict.GetString( "def_head" );
	headOffset = dict.GetVector( "head_offset" );
	uiHead = dict.GetString( "def_head_ui" );
	team = dict.GetString( "team" );
	skin = dict.GetString( "skin" );
	description = dict.GetString( "description" );
	for ( const idKeyValue *kv = dict.MatchPrefix( "snd_" ); kv; kv = dict.MatchPrefix( "snd_", kv ) ) {
		sounds.Set( kv->GetKey(), kv->GetValue() );
	}
	return true;
}
