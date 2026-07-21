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

#include "sys/platform.h"
#include "idlib/containers/List.h"

#include "renderer/ImmediateMode.h"

struct imVert_t {
	float	xyz[3];
	float	st[2];
	byte	color[4];
};

// shared growable batch storage; rendering is single-threaded
static idList<imVert_t>		imVerts;
static idList<unsigned int>	imIndexes;

static ID_INLINE byte FloatColorToByte( float c ) {
	if ( c <= 0.0f ) {
		return 0;
	}
	if ( c >= 1.0f ) {
		return 255;
	}
	return (byte)( c * 255.0f );
}

idImmediateMode::idImmediateMode() {
	mode = 0;
	currentSt[0] = currentSt[1] = 0.0f;
	currentColor[0] = currentColor[1] = currentColor[2] = currentColor[3] = 255;
	texCoordUsed = false;
	colorUsed = false;
}

void idImmediateMode::Begin( GLenum mode_ ) {
	mode = mode_;
	imVerts.SetNum( 0, false );
}

void idImmediateMode::TexCoord2f( float s, float t ) {
	currentSt[0] = s;
	currentSt[1] = t;
	texCoordUsed = true;
}

void idImmediateMode::TexCoord2fv( const float *st ) {
	TexCoord2f( st[0], st[1] );
}

void idImmediateMode::Color3f( float r, float g, float b ) {
	Color4f( r, g, b, 1.0f );
}

void idImmediateMode::Color4f( float r, float g, float b, float a ) {
	currentColor[0] = FloatColorToByte( r );
	currentColor[1] = FloatColorToByte( g );
	currentColor[2] = FloatColorToByte( b );
	currentColor[3] = FloatColorToByte( a );
	colorUsed = true;
}

void idImmediateMode::Color3fv( const float *c ) {
	Color4f( c[0], c[1], c[2], 1.0f );
}

void idImmediateMode::Color4fv( const float *c ) {
	Color4f( c[0], c[1], c[2], c[3] );
}

void idImmediateMode::Color3ubv( const byte *c ) {
	currentColor[0] = c[0];
	currentColor[1] = c[1];
	currentColor[2] = c[2];
	currentColor[3] = 255;
	colorUsed = true;
}

void idImmediateMode::Color4ubv( const byte *c ) {
	currentColor[0] = c[0];
	currentColor[1] = c[1];
	currentColor[2] = c[2];
	currentColor[3] = c[3];
	colorUsed = true;
}

void idImmediateMode::Vertex2f( float x, float y ) {
	Vertex3f( x, y, 0.0f );
}

void idImmediateMode::Vertex3f( float x, float y, float z ) {
	imVert_t &v = imVerts.Alloc();
	v.xyz[0] = x;
	v.xyz[1] = y;
	v.xyz[2] = z;
	v.st[0] = currentSt[0];
	v.st[1] = currentSt[1];
	v.color[0] = currentColor[0];
	v.color[1] = currentColor[1];
	v.color[2] = currentColor[2];
	v.color[3] = currentColor[3];
}

void idImmediateMode::Vertex3fv( const float *xyz ) {
	Vertex3f( xyz[0], xyz[1], xyz[2] );
}

void idImmediateMode::End() {
	const int numVerts = imVerts.Num();
	if ( numVerts == 0 ) {
		return;
	}

	const bool texArrayWasEnabled = qglIsEnabled( GL_TEXTURE_COORD_ARRAY ) != GL_FALSE;
	const bool colorArrayWasEnabled = qglIsEnabled( GL_COLOR_ARRAY ) != GL_FALSE;

	qglVertexPointer( 3, GL_FLOAT, sizeof( imVert_t ), imVerts[0].xyz );

	if ( texCoordUsed ) {
		if ( !texArrayWasEnabled ) {
			qglEnableClientState( GL_TEXTURE_COORD_ARRAY );
		}
		qglTexCoordPointer( 2, GL_FLOAT, sizeof( imVert_t ), imVerts[0].st );
	} else if ( texArrayWasEnabled ) {
		// don't let a bound texture sample garbage coordinates
		qglDisableClientState( GL_TEXTURE_COORD_ARRAY );
	}

	if ( colorUsed ) {
		if ( !colorArrayWasEnabled ) {
			qglEnableClientState( GL_COLOR_ARRAY );
		}
		qglColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( imVert_t ), imVerts[0].color );
	}

	switch ( mode ) {
		case GL_QUADS: {
			// convert to indexed triangles, quads don't exist in core profiles
			imIndexes.SetNum( 0, false );
			for ( int i = 0; i + 3 < numVerts; i += 4 ) {
				imIndexes.Append( i + 0 );
				imIndexes.Append( i + 1 );
				imIndexes.Append( i + 2 );
				imIndexes.Append( i + 0 );
				imIndexes.Append( i + 2 );
				imIndexes.Append( i + 3 );
			}
			qglDrawElements( GL_TRIANGLES, imIndexes.Num(), GL_UNSIGNED_INT, imIndexes.Ptr() );
			break;
		}
		case GL_POLYGON:
			qglDrawArrays( GL_TRIANGLE_FAN, 0, numVerts );
			break;
		default:
			qglDrawArrays( mode, 0, numVerts );
			break;
	}

	// restore the client state enables we found
	if ( texCoordUsed != texArrayWasEnabled ) {
		if ( texArrayWasEnabled ) {
			qglEnableClientState( GL_TEXTURE_COORD_ARRAY );
		} else {
			qglDisableClientState( GL_TEXTURE_COORD_ARRAY );
		}
	}
	if ( colorUsed && !colorArrayWasEnabled ) {
		qglDisableClientState( GL_COLOR_ARRAY );
	}
}
