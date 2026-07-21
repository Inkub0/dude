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

#ifndef __IMMEDIATEMODE_H__
#define __IMMEDIATEMODE_H__

#include "renderer/qgl.h"

/*
Batched replacement for glBegin()/glEnd() geometry submission.

Vertices are collected into a client-side array and drawn with a single
call, so no immediate mode reaches the backend. GL_QUADS and GL_POLYGON
batches are drawn as indexed/fanned triangles, since core profiles don't
support those primitives.

Like GL's current color/texcoord, the color and texcoord set on an
instance persist across Begin()/End() pairs of that instance. Per-vertex
colors are only submitted if a Color*() method was called on the instance;
otherwise the current GL color state applies, matching immediate mode
behavior of untouched call sites. Client state enables found at End() are
restored after the draw, so callers that deliberately disabled e.g. the
texcoord array (common in the debug tools) keep their state.
*/
class idImmediateMode {
public:
	idImmediateMode();

	void	Begin( GLenum mode );
	void	TexCoord2f( float s, float t );
	void	TexCoord2fv( const float *st );
	void	Color3f( float r, float g, float b );
	void	Color4f( float r, float g, float b, float a );
	void	Color3fv( const float *c );
	void	Color4fv( const float *c );
	void	Color3ubv( const byte *c );
	void	Color4ubv( const byte *c );
	void	Vertex2f( float x, float y );
	void	Vertex3f( float x, float y, float z );
	void	Vertex3fv( const float *xyz );
	void	End();

private:
	GLenum	mode;
	float	currentSt[2];
	byte	currentColor[4];
	bool	texCoordUsed;
	bool	colorUsed;
};

#endif /* !__IMMEDIATEMODE_H__ */
