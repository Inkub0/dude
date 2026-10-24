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
#include "idlib/math/Angles.h"
#include "idlib/math/Curve.h"
#include "idlib/bv/Bounds.h"
#include "framework/DeclManager.h"
#include "framework/DeclEntityDef.h"
#include "framework/CVarSystem.h"

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

	const idStr &			GetDescription( void ) const { return description; }
	const char *			GetText( void ) const { return lipText; }		// usually a #str_ id
	const char *			GetVisemes( const char *language ) const { return visemes.GetString( language, "" ); }
	const idDict &			GetAllVisemes( void ) const { return visemes; }
							// the game's language's visemes (sys_lang), else English's
	const char *			GetLipSyncData( void ) const {
								return visemes.GetString( cvarSystem->GetCVarString( "sys_lang" ), visemes.GetString( "english", "" ) );
							}

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
		buttons { { 1.25 down 1 } { 2.5 up 1 impulse 13 } ... }
	}

	A recorded path that scripted movers, flyers and cameras play back: one sample per frame,
	1/frameRate apart, each holding a position and view angles - a key a sample leaves out keeps
	the previous sample's value; "ang" is pitch and yaw, "rotate" adds the roll - and the buttons
	and impulses pressed along it. It is evaluated as uniform cubic B-splines through the
	samples, the positions relative to the first, which moves into the origin.

===============================================================================
*/

// what a playback holds, and what GetCurrentData is asked for (Quake 4's values: its monster
// state scripts use them)
#define PBFL_GET_POSITION			BIT( 0 )		// in the file
#define PBFL_GET_ANGLES				BIT( 1 )
#define PBFL_GET_BUTTONS			BIT( 2 )
#define PBFL_GET_VELOCITY			BIT( 4 )		// derived from the positions
#define PBFL_GET_ACCELERATION		BIT( 5 )
#define PBFL_GET_ANGLES_FROM_VEL	BIT( 6 )		// facing the way it moves
#define PBFL_AT_DEST				BIT( 7 )
#define PBFL_RELATIVE_POSITION		BIT( 8 )		// the game plays it from where the entity is

// the events GetCurrentData reports through the data's callback
#define PBCB_NONE					0
#define PBCB_BUTTON_DOWN			1
#define PBCB_BUTTON_UP				2
#define PBCB_IMPULSE				3

class idEntity;
typedef void ( *pbCallback_t )( int type, float time, const void *data );

// a playback's state at a time, for the entity that plays it
class idPlaybackData {
public:
	void					Init( void ) {
								entity = NULL;
								callback = NULL;
								position.Zero();
								velocity.Zero();
								acceleration.Zero();
								angles.Zero();
								changed = button = impulse = 0;
							}

	void					SetPosition( const idVec3 &pos ) { position = pos; }
	void					SetVelocity( const idVec3 &vel ) { velocity = vel; }
	void					SetAcceleration( const idVec3 &accel ) { acceleration = accel; }
	void					SetAngles( const idAngles &ang ) { angles = ang; }
	void					SetChanged( const byte chg ) { changed = chg; }
	void					SetButtons( const byte btn ) { button = btn; }
	void					SetImpulse( const byte imp ) { impulse = imp; }

	const idVec3 &			GetPosition( void ) const { return position; }
	const idVec3 &			GetVelocity( void ) const { return velocity; }
	const idVec3 &			GetAcceleration( void ) const { return acceleration; }
	const idAngles &		GetAngles( void ) const { return angles; }
	byte					GetChanged( void ) const { return changed; }	// the buttons that changed
	byte					GetButtons( void ) const { return button; }
	byte					GetImpulse( void ) const { return impulse; }

	idEntity *				GetEntity( void ) const { return entity; }
							// cb is told of every button change and impulse passed since the last time
	void					SetCallback( idEntity *ent, pbCallback_t cb ) { entity = ent; callback = cb; }
	void					CallCallback( int type, float time ) { if ( callback ) { callback( type, time, this ); } }

private:
	idEntity *				entity;
	pbCallback_t			callback;
	idVec3					position;
	idVec3					velocity;
	idVec3					acceleration;
	idAngles				angles;
	byte					changed;
	byte					button;
	byte					impulse;
};

// the buttons held from a time on, and an impulse given then
typedef struct {
	float					time;
	byte					buttons;
	byte					impulse;
} playbackButtons_t;

class idDeclPlayback : public idDecl {
public:
	virtual size_t			Size( void ) const;
	virtual const char *	DefaultDefinition( void ) const;
	virtual bool			Parse( const char *text, const int textLength );
	virtual void			FreeData( void );

	int						GetFlags( void ) const { return flags; }			// PBFL_GET_* it holds
	bool					GetHasPositions( void ) const { return ( flags & PBFL_GET_POSITION ) != 0; }
	bool					GetHasAngles( void ) const { return ( flags & PBFL_GET_ANGLES ) != 0; }
	bool					GetHasButtons( void ) const { return ( flags & PBFL_GET_BUTTONS ) != 0; }
	float					GetFrameRate( void ) const { return frameRate; }
	const idVec3 &			GetOrigin( void ) const { return origin; }
	float					GetDuration( void ) const { return duration; }		// seconds
	const idBounds &		GetBounds( void ) const { return bounds; }			// of the path, about the origin

							// the state control asks for at localTime (seconds); button changes and
							// impulses after lastTime are passed to pbd's callback. True once localTime
							// is past the end.
	bool					GetCurrentData( int control, float localTime, float lastTime, idPlaybackData *pbd ) const;

							// recording (Quake 4's g_recordPlayback): begin, add what control asks for at
							// localTime, and finish - the path moves to its origin, and the decl's text
							// becomes the recording
	void					StartRecording( void );
	bool					RecordSample( float localTime, int control, const idPlaybackData *pbd );
	bool					FinishRecording( void );

private:
	int						flags;
	float					frameRate;
	float					duration;
	idVec3					origin;
	idBounds				bounds;
	idCurve_UniformCubicBSpline<idVec3>		points;
	idCurve_UniformCubicBSpline<idAngles>	angles;
	idList<playbackButtons_t>				buttons;

	void					MovePathToOrigin( void );
	bool					ParseSequence( idLexer &src );
	bool					ParseData( idLexer &src );
	bool					ParseButtons( idLexer &src );
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

/*
===============================================================================

	idDeclPlayerModel - def/*.def

	playerModel model_player_marine {
		"model"			"model_player_marine"
		"def_head"		"char_marinehead_kane2_client"
		"def_head_ui"	"char_marinehead_kane2"
		"skin"			""
		"team"			"marine"
		"description"	"#str_108001"
	}

	A model the player can be shown with (the multiplayer model list): an entityDef-style
	dictionary, kept whole in dict, with its keys as Quake 4's game reads them. The head offset
	and sounds are keys no retail decl sets ("head_offset", "snd_*").

===============================================================================
*/

class idDeclPlayerModel : public idDeclEntityDef {
public:
	idStr					model;
	idStr					head;					// def_head
	idVec3					headOffset;				// head_offset
	idStr					uiHead;					// def_head_ui: the head the menus show
	idStr					team;					// "marine", "strogg", or none
	idStr					skin;
	idStr					description;			// usually a #str_ id
	idDict					sounds;					// the snd_* keys

	virtual size_t			Size( void ) const;
	virtual const char *	DefaultDefinition( void ) const;
	virtual bool			Parse( const char *text, const int textLength );
	virtual void			FreeData( void );
};

#endif /* !__DECLQUAKE4_H__ */
