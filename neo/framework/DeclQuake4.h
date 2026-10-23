/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: Quake 4 decl types the engine owns (docs/quake4-port.md 5.2). Registered only for the
// Quake 4 game title (GameTitle), next to Doom 3's: materialType, lipSync, playback, effect
// (and playerModel, which is an idDeclEntityDef dictionary). Written from the retail files'
// grammar; the accessors are what the Quake 4 game library and the renderer will need, and
// grow with them.

#ifndef __DECLQUAKE4_H__
#define __DECLQUAKE4_H__

#include "idlib/Dict.h"
#include "idlib/containers/List.h"
#include "idlib/math/Vector.h"
#include "framework/DeclManager.h"

/*
===============================================================================

	idDeclMaterialType - materials/types/<name>.mtt

	materialType rock { description "..." rgb 140,100,60 }

	What a surface is made of, for impacts, footsteps and effect variants. The rgb is the
	colour that names this type in a material's per-texel hit map (materialImage).

===============================================================================
*/

class idDeclMaterialType : public idDecl {
public:
	virtual size_t			Size( void ) const;
	virtual const char *	DefaultDefinition( void ) const;
	virtual bool			Parse( const char *text, const int textLength );
	virtual void			FreeData( void );

	const char *			GetDescription( void ) const { return description; }
	const byte *			GetRGB( void ) const { return rgb; }		// 0-255, the hit-map colour

private:
	idStr					description;
	byte					rgb[3];
};

/*
===============================================================================

	idDeclLipSync - lipsync/<name>.lipsync

	lipSync vo_1_1_2_12_1 {
		description	"..."
		text		"#str_300084"
		visemes		"english"	"x4c {furrow} <Lets> l17n EH5q ..."
	}

	Mouth shapes per language for one voice line; the game drives a head's face from it.

===============================================================================
*/

class idDeclLipSync : public idDecl {
public:
	virtual size_t			Size( void ) const;
	virtual const char *	DefaultDefinition( void ) const;
	virtual bool			Parse( const char *text, const int textLength );
	virtual void			FreeData( void );

	const char *			GetDescription( void ) const { return description; }
	const char *			GetText( void ) const { return lipText; }		// usually a #str_ id
	const char *			GetVisemes( const char *language ) const { return visemes.GetString( language, "" ); }
	const idDict &			GetAllVisemes( void ) const { return visemes; }

private:
	idStr					description;
	idStr					lipText;
	idDict					visemes;		// language -> viseme string
};

/*
===============================================================================

	idDeclPlayback - playbacks/<name>.playback

	playback playbacks/medlabs2_fighter_1 {
		sequence { origin 1552.5,-6334.5,809.7  frameRate 15.0 }
		data { { ang 0.0,0.0 } { } { pos 0.0,0.0,-6.3 } { rotate 0,90,0 } ... }
	}

	A recorded path for scripted movers (the flyers in the cinematics): one entry per frame,
	each optionally carrying a position, view angles or a rotation.

===============================================================================
*/

typedef enum {
	PLAYBACK_POS		= BIT(0),
	PLAYBACK_ANG		= BIT(1),
	PLAYBACK_ROTATE		= BIT(2)
} playbackFrameFlags_t;

typedef struct {
	int						flags;			// playbackFrameFlags_t
	idVec3					pos;
	idVec3					ang;			// pitch, yaw (roll 0 when the file gives two)
	idVec3					rotate;
} playbackFrame_t;

class idDeclPlayback : public idDecl {
public:
	virtual size_t			Size( void ) const;
	virtual const char *	DefaultDefinition( void ) const;
	virtual bool			Parse( const char *text, const int textLength );
	virtual void			FreeData( void );

	const idVec3 &			GetOrigin( void ) const { return origin; }
	float					GetFrameRate( void ) const { return frameRate; }
	int						GetNumFrames( void ) const { return frames.Num(); }
	const playbackFrame_t &	GetFrame( int index ) const { return frames[index]; }

private:
	idVec3					origin;
	float					frameRate;
	idList<playbackFrame_t>	frames;
};

/*
===============================================================================

	idDeclEffect - effects/<name>.fx (Raven's BSE)

	For now the decl checks the effect is well formed (balanced, with its top-level
	segments) and counts its segments; the effect grammar and runtime are their own phase
	(docs/quake4-port.md Q4, section 5.5).

===============================================================================
*/

class idDeclEffect : public idDecl {
public:
	virtual size_t			Size( void ) const;
	virtual const char *	DefaultDefinition( void ) const;
	virtual bool			Parse( const char *text, const int textLength );
	virtual void			FreeData( void );

	float					GetSize( void ) const { return size; }			// bounding radius
	int						GetNumSegments( void ) const { return numSegments; }

private:
	float					size;
	int						numSegments;
};

#endif /* !__DECLQUAKE4_H__ */
