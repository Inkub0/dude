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

#ifndef __DECLTABLE_H__
#define __DECLTABLE_H__

#include "idlib/containers/List.h"
#include "idlib/math/Math.h"
#include "framework/DeclManager.h"

/*
===============================================================================

	tables are used to map a floating point input value to a floating point
	output value, with optional wrap / clamp and interpolation

===============================================================================
*/

class idDeclTable : public idDecl {
public:
	virtual size_t			Size( void ) const;
	virtual const char *	DefaultDefinition( void ) const;
	virtual bool			Parse( const char *text, const int textLength );
	virtual void			FreeData( void );

	float					TableLookup( float index ) const;
							// DUDE: the lowest / highest value (Quake 4's effect bounds scale by them)
	float					GetMinValue( void ) const;
	float					GetMaxValue( void ) const;

private:
	bool					clamp;
	bool					snap;
	idList<float>			values;
};

// inline so a game library can call it (it links no engine code; Quake 4's vehicles use it)
ID_INLINE float idDeclTable::TableLookup( float index ) const {
	int iIndex;
	float iFrac;

	int domain = values.Num() - 1;

	if ( domain <= 1 ) {
		return 1.0f;
	}

	if ( clamp ) {
		index *= (domain-1);
		if ( index >= domain - 1 ) {
			return values[domain - 1];
		} else if ( index <= 0 ) {
			return values[0];
		}
		iIndex = idMath::Ftoi( index );
		iFrac = index - iIndex;
	} else {
		index *= domain;

		if ( index < 0 ) {
			index += domain * idMath::Ceil( -index / domain );
		}

		iIndex = idMath::FtoiFast( idMath::Floor( index ) );
		iFrac = index - iIndex;
		iIndex = iIndex % domain;
	}

	if ( !snap ) {
		// we duplicated the 0 index at the end at creation time, so we
		// don't need to worry about wrapping the filter
		return values[iIndex] * ( 1.0f - iFrac ) + values[iIndex + 1] * iFrac;
	}

	return values[iIndex];
}

ID_INLINE float idDeclTable::GetMinValue( void ) const {
	float value = values.Num() ? values[0] : 0.0f;
	for ( int i = 1; i < values.Num(); i++ ) {
		if ( values[i] < value ) {
			value = values[i];
		}
	}
	return value;
}

ID_INLINE float idDeclTable::GetMaxValue( void ) const {
	float value = values.Num() ? values[0] : 0.0f;
	for ( int i = 1; i < values.Num(); i++ ) {
		if ( values[i] > value ) {
			value = values[i];
		}
	}
	return value;
}

#endif /* !__DECLTABLE_H__ */
