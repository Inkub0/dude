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

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include "sys/platform.h"
#include "idlib/LangDict.h"
#include "framework/DemoFile.h"
#include "ui/DeviceContext.h"
#include "ui/Window.h"
#include "ui/UserInterfaceLocal.h"

#include "ui/RegExp.h"

int idRegister::REGCOUNT[NUMTYPES] = {4, 1, 1, 1, 0, 2, 3, 4};

/*
====================
idRegister::SetToRegs
====================
*/
void idRegister::SetToRegs( float *registers ) {
	int i;
	idVec4 v;
	idVec2 v2;
	idVec3 v3;
	idRectangle rect;

	if ( !enabled || var == NULL || ( var && ( var->GetDict() || !var->GetEval() ) ) ) {
		return;
	}

	switch( type ) {
		case VEC4: {
			v = *static_cast<idWinVec4*>(var);
			break;
		}
		case RECTANGLE: {
			rect = *static_cast<idWinRectangle*>(var);
			v = rect.ToVec4();
			break;
		}
		case VEC2: {
			v2 = *static_cast<idWinVec2*>(var);
			v[0] = v2[0];
			v[1] = v2[1];
			break;
		}
		case VEC3: {
			v3 = *static_cast<idWinVec3*>(var);
			v[0] = v3[0];
			v[1] = v3[1];
			v[2] = v3[2];
			break;
		}
		case FLOAT: {
			v[0] = *static_cast<idWinFloat*>(var);
			break;
		}
		case INT: {
			v[0] = *static_cast<idWinInt*>(var);
			break;
		}
		case BOOL: {
			v[0] = *static_cast<idWinBool*>(var);
			break;
		}
		default: {
			common->FatalError( "idRegister::SetToRegs: bad reg type" );
			break;
		}
	}
	for ( i = 0; i < regCount; i++ ) {
		registers[ regs[ i ] ] = v[i];
	}
}

/*
=================
idRegister::GetFromRegs
=================
*/
void idRegister::GetFromRegs( float *registers ) {
	idVec4 v;
	idRectangle rect;

	if (!enabled || var == NULL || (var && (var->GetDict() || !var->GetEval()))) {
		return;
	}

	for ( int i = 0; i < regCount; i++ ) {
		v[i] = registers[regs[i]];
	}

	switch( type ) {
		case VEC4: {
			*dynamic_cast<idWinVec4*>(var) = v;
			break;
		}
		case RECTANGLE: {
			rect.x = v.x;
			rect.y = v.y;
			rect.w = v.z;
			rect.h = v.w;
			*static_cast<idWinRectangle*>(var) = rect;
			break;
		}
		case VEC2: {
			*static_cast<idWinVec2*>(var) = v.ToVec2();
			break;
		}
		case VEC3: {
			*static_cast<idWinVec3*>(var) = v.ToVec3();
			break;
		}
		case FLOAT: {
			*static_cast<idWinFloat*>(var) = v[0];
			break;
		}
		case INT: {
			*static_cast<idWinInt*>(var) = v[0];
			break;
		}
		case BOOL: {
			*static_cast<idWinBool*>(var) = ( v[0] != 0.0f );
			break;
		}
		default: {
			common->FatalError( "idRegister::GetFromRegs: bad reg type" );
			break;
		}
	}
}

/*
=================
idRegister::ReadFromDemoFile
=================
*/
void idRegister::ReadFromDemoFile(idDemoFile *f) {
	f->ReadBool( enabled );
	f->ReadShort( type );
	f->ReadInt( regCount );
	for ( int i = 0; i < 4; i++ )
		f->ReadUnsignedShort( regs[i] );
	name = f->ReadHashString();
}

/*
=================
idRegister::WriteToDemoFile
=================
*/
void idRegister::WriteToDemoFile( idDemoFile *f ) {
	f->WriteBool( enabled );
	f->WriteShort( type );
	f->WriteInt( regCount );
	for (int i = 0; i < 4; i++)
		f->WriteUnsignedShort( regs[i] );
	f->WriteHashString( name );
}

/*
=================
idRegister::WriteToSaveGame
=================
*/
void idRegister::WriteToSaveGame( idFile *savefile ) {
	int len;

	savefile->Write( &enabled, sizeof( enabled ) );
	savefile->Write( &type, sizeof( type ) );
	savefile->Write( &regCount, sizeof( regCount ) );
	savefile->Write( &regs[0], sizeof( regs ) );

	len = name.Length();
	savefile->Write( &len, sizeof( len ) );
	savefile->Write( name.c_str(), len );

	var->WriteToSaveGame( savefile );
}

/*
================
idRegister::ReadFromSaveGame
================
*/
void idRegister::ReadFromSaveGame( idFile *savefile ) {
	int len;

	savefile->Read( &enabled, sizeof( enabled ) );
	savefile->Read( &type, sizeof( type ) );
	savefile->Read( &regCount, sizeof( regCount ) );
	savefile->Read( &regs[0], sizeof( regs ) );

	savefile->Read( &len, sizeof( len ) );
	name.Fill( ' ', len );
	savefile->Read( &name[0], len );

	var->ReadFromSaveGame( savefile );
}

/*
====================
idRegisterList::AddReg
====================
*/
void idRegisterList::AddReg( const char *name, int type, idVec4 data, idWindow *win, idWinVar *var ) {
	if ( FindReg( name ) == NULL ) {
		assert( type >= 0 && type < idRegister::NUMTYPES );
		int numRegs = idRegister::REGCOUNT[type];
		idRegister *reg = new idRegister( name, type );
		reg->var = var;
		for ( int i = 0; i < numRegs; i++ ) {
			reg->regs[i] = win->ExpressionConstant(data[i]);
		}
		int hash = regHash.GenerateKey( name, false );
		regHash.Add( hash, regs.Append( reg ) );
	}
}

/*
====================
idRegisterList::AddReg
====================
*/
void idRegisterList::AddReg( const char *name, int type, idParser *src, idWindow *win, idWinVar *var ) {
	idRegister* reg;

	reg = FindReg( name );

	if ( reg == NULL ) {
		assert(type >= 0 && type < idRegister::NUMTYPES);
		int numRegs = idRegister::REGCOUNT[type];
		reg = new idRegister( name, type );
		reg->var = var;
		if ( type == idRegister::STRING ) {
			idToken tok;
			if ( src->ReadToken( &tok ) ) {
				tok = common->GetLanguageDict()->GetString( tok );
				var->Init( tok, win );
			}
		} else {
			for ( int i = 0; i < numRegs; i++ ) {
				reg->regs[i] = win->ParseExpression(src, NULL);
				if ( i < numRegs-1 ) {
					src->ExpectTokenString(",");
				}
			}
		}
		int hash = regHash.GenerateKey( name, false );
		regHash.Add( hash, regs.Append( reg ) );
	} else {
		int numRegs = idRegister::REGCOUNT[type];
		reg->var = var;
		if ( type == idRegister::STRING ) {
			idToken tok;
			if ( src->ReadToken( &tok ) ) {
				var->Init( tok, win );
			}
		} else {
			for ( int i = 0; i < numRegs; i++ ) {
				reg->regs[i] = win->ParseExpression( src, NULL );
				if ( i < numRegs-1 ) {
					src->ExpectTokenString(",");
				}
			}
		}
	}
}

/*
====================
idRegisterList::GetFromRegs
====================
*/
void idRegisterList::GetFromRegs(float *registers) {
	for ( int i = 0; i < regs.Num(); i++ ) {
		regs[i]->GetFromRegs( registers );
	}
}

/*
====================
idRegisterList::SetToRegs
====================
*/

void idRegisterList::SetToRegs( float *registers ) {
	int i;
	for ( i = 0; i < regs.Num(); i++ ) {
		regs[i]->SetToRegs( registers );
	}
}

/*
====================
idRegisterList::FindReg
====================
*/
idRegister *idRegisterList::FindReg( const char *name ) {
	int hash = regHash.GenerateKey( name, false );
	for ( int i = regHash.First( hash ); i != -1; i = regHash.Next( i ) ) {
		if ( regs[i]->name.Icmp( name ) == 0 ) {
			return regs[i];
		}
	}
	return NULL;
}

/*
====================
idRegisterList::Reset
====================
*/
void idRegisterList::Reset() {
	regs.DeleteContents( true );
	regHash.Clear();
}

/*
====================
idRegisterList::ReadFromSaveGame
====================
*/
void idRegisterList::ReadFromDemoFile(idDemoFile *f) {
	int c;

	f->ReadInt( c );
	regs.DeleteContents( true );
	for ( int i = 0; i < c; i++ ) {
		idRegister *reg = new idRegister;
		reg->ReadFromDemoFile( f );
		regs.Append( reg );
	}
}

/*
====================
idRegisterList::ReadFromSaveGame
====================
*/
void idRegisterList::WriteToDemoFile(idDemoFile *f) {
	int c = regs.Num();

	f->WriteInt( c );
	for ( int i = 0 ; i < c; i++ ) {
		regs[i]->WriteToDemoFile(f);
	}
}

/*
=====================
idRegisterList::WriteToSaveGame
=====================
*/
void idRegisterList::WriteToSaveGame( idFile *savefile ) {
	int i, num;

	num = regs.Num();
	savefile->Write( &num, sizeof( num ) );

	for ( i = 0; i < num; i++ ) {
		regs[i]->WriteToSaveGame( savefile );
	}
}

/*
====================
idRegisterList::ReadFromSaveGame

DUDE: matched by name. A register's expression registers (regs, regCount) and type are what the
GUI file parsed to, so they stay as parsed; whether it is enabled (a script's `set` turns it off)
and its variable's value are the state. A register the file doesn't have now - a widescreen pack's
anchors (cstAnchor ...) on a window the stock file leaves bare - is read past, its value's size
known from its type; one only the file has keeps its parsed state. A saved register must name a
register variable with its type (idWindow::RegisterVars), or be a float or vec4 the file defines,
or the state is out of step with the file (see GuiRestore_Failed).
====================
*/
void idRegisterList::ReadFromSaveGame( idFile *savefile ) {
	int i, num;

	if ( GuiRestore_Failed() ) {
		return;
	}
	savefile->Read( &num, sizeof( num ) );
	if ( !GuiRestore_Count( savefile, num, sizeof( bool ) + sizeof( short ) + sizeof( int ) + 4 * sizeof( unsigned short ) + sizeof( int ) + 1 ) ) {
		return;
	}
	for ( i = 0; i < num && !GuiRestore_Failed(); i++ ) {
		ReadSavedRegister( savefile );
	}
}

/*
====================
idRegisterList::ReadSavedRegister

One register as idRegister::WriteToSaveGame wrote it, into the one of this list it names. See
ReadFromSaveGame.
====================
*/
void idRegisterList::ReadSavedRegister( idFile *savefile ) {
	bool enabled;
	short type;
	int regCount;
	unsigned short regs[4];
	idStr name;

	savefile->Read( &enabled, sizeof( enabled ) );
	savefile->Read( &type, sizeof( type ) );
	savefile->Read( &regCount, sizeof( regCount ) );
	savefile->Read( &regs[0], sizeof( regs ) );
	if ( !GuiRestore_ReadString( savefile, name ) ) {
		return;
	}

	// a register variable (idWindow::RegisterVars), or a float or vec4 the file defines
	bool known = false;
	for ( int i = 0; i < idWindow::NumRegisterVars && !known; i++ ) {
		known = name.Icmp( idWindow::RegisterVars[i].name ) == 0 && type == idWindow::RegisterVars[i].type;
	}
	if ( !known && ( type == idRegister::FLOAT || type == idRegister::VEC4 ) && name.Length() > 0 && name.Length() < MAX_STRING_CHARS ) {
		known = true;
		for ( int i = 0; i < name.Length() && known; i++ ) {
			known = name[i] > ' ' && name[i] < 127;
		}
	}
	if ( !known ) {
		GuiRestore_Fail( "register \"%s\" of type %d saved", name.c_str(), (int)type );
		return;
	}

	idRegister *reg = FindReg( name );
	if ( reg != NULL && reg->type == type && reg->var != NULL ) {
		reg->enabled = enabled;
		reg->var->ReadFromSaveGame( savefile );
		return;
	}

	// not in the file now: its value read past, as the variable of its type reads it
	idWinVec4 vec4;
	idWinFloat f;
	idWinBool b;
	idWinInt n;
	idWinStr str;
	idWinVec2 vec2;
	idWinVec3 vec3;
	idWinRectangle rect;
	idWinVar *skip[idRegister::NUMTYPES] = { &vec4, &f, &b, &n, &str, &vec2, &vec3, &rect };
	skip[type]->ReadFromSaveGame( savefile );
}
