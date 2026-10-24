/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: the Quake 4 game library's sound interface (docs/quake4-port.md 5.6). Quake 4's game
// never holds a sound world: it names one by id (SOUNDWORLD_GAME, the menu, the editor) and
// holds emitters as integer handles in it, which the sound system turns into emitters on
// demand. Its emitters take a velocity (doppler), and its parameters carry a pitch shift and
// the wet/dry mix. The sound shaders themselves are Doom 3's decl (sound/sound.h), with the
// Quake 4 data Q2 parses.

#ifndef __SOUNDQ4_H__
#define __SOUNDQ4_H__

#include "sound/sound.h"

class idFile;

// the sound worlds the game names
#define SOUNDWORLD_ANY			-1
#define SOUNDWORLD_NONE			0
#define SOUNDWORLD_GAME			1
#define SOUNDWORLD_MENU			2
#define SOUNDWORLD_EDITOR		3
#define SOUNDWORLD_MAX			4

const int SOUND_CLASS_MUSICAL	= 3;		// the class the music plays in (soundClass 0..SOUND_MAX_CLASSES-1)

// a play's parameters: Doom 3's, and the attenuated volume, pitch shift and wet/dry mix
typedef struct soundShaderParmsQ4_s {
	float					minDistance;
	float					maxDistance;
	float					volume;					// dB
	float					attenuatedVolume;
	float					shakes;
	int						soundShaderFlags;		// SSF_*
	int						soundClass;				// for fading classes of sounds together
	float					frequencyShift;			// 1 = the sample's own pitch
	float					wetLevel;				// reverb send, 0..1
	float					dryLevel;				// direct level, 0..1
} soundShaderParmsQ4_t;

// a game entity's sound: its emitter is a handle in SOUNDWORLD_GAME
typedef struct refSoundQ4_s {
	int						referenceSoundHandle;	// from AllocSoundEmitter, 0 until one is needed
	idVec3					origin;
	idVec3					velocity;				// for doppler
	int						listenerId;				// SSF_PRIVATE_SOUND plays only for this listener, unspatialized
	const idSoundShader *	shader;
	float					diversity;				// 0..1, picks among the shader's samples
	bool					waitfortrigger;			// don't start it at spawn time
	soundShaderParmsQ4_t	parms;					// overrides of the shader's
} refSoundQ4_t;

// a sound emitter, as the game reaches it through a handle
class idSoundEmitterQ4 {
public:
	virtual					~idSoundEmitterQ4( void ) {}

	virtual void			UpdateEmitter( const idVec3 &origin, const idVec3 &velocity, int listenerId, const soundShaderParmsQ4_t *parms ) = 0;
							// the length of the sound started, in milliseconds
	virtual int				StartSound( const idSoundShader *shader, const s_channelType channel, float diversity = 0.0f, int shaderFlags = 0 ) = 0;
	virtual void			ModifySound( const s_channelType channel, const soundShaderParmsQ4_t *parms ) = 0;
	virtual void			StopSound( const s_channelType channel ) = 0;
	virtual void			FadeSound( const s_channelType channel, float to, float over ) = 0;
	virtual bool			CurrentlyPlaying( const s_channelType channel = SCHANNEL_ANY ) const = 0;
	virtual float			CurrentAmplitude( int channelFlags = -1, bool factorDistance = false ) = 0;
	virtual bool			AttachedToWorld( int worldId ) const = 0;
	virtual int				Handle( void ) const = 0;
};

class idSoundSystemQ4 {
public:
	virtual					~idSoundSystemQ4( void ) {}

							// emitters of a sound world, by handle
	virtual int				AllocSoundEmitter( int worldId ) = 0;
	virtual void			FreeSoundEmitter( int worldId, int handle, bool immediate ) = 0;
	virtual idSoundEmitterQ4 *EmitterForIndex( int worldId, int handle ) = 0;

	virtual void			StopAllSounds( int worldId ) = 0;
	virtual void			SetMute( bool mute ) = 0;
							// the world the player hears: on = the game's, off = the menu's; the previous one
	virtual int				SetActiveSoundWorld( bool on ) = 0;

	virtual void			PlaceListener( const idVec3 &origin, const idMat3 &axis, const int listenerId, const int gameTime, const idStr &areaName ) = 0;
	virtual void			ResetListener( void ) = 0;
							// the screen shake the world's sounds cause at a position
	virtual float			CurrentShakeAmplitudeForPosition( int worldId, const int time, const idVec3 &listenerPosition ) = 0;
							// fade every sound of a class to a level (dB) over seconds
	virtual void			FadeSoundClasses( int worldId, const int soundClass, float to, const float over ) = 0;
							// a shader played at once on its own, outside any emitter (menus, announcer)
	virtual void			PlayShaderDirectly( int worldId, const char *name, int channel = -1 ) = 0;
	virtual void			EndCinematic( void ) = 0;		// a cinematic's sounds end with it

	virtual void			WriteToSaveGame( int worldId, idFile *savefile ) = 0;
	virtual void			ReadFromSaveGame( int worldId, idFile *savefile ) = 0;

							// multiplayer voice chat (docs/quake4-port.md Q9)
	virtual bool			EnableRecording( bool enable, bool test, float &micLevel ) = 0;
	virtual int				GetVoiceData( byte *buffer, int maxSize ) = 0;
	virtual void			PlayVoiceData( int clientNum, const byte *buffer, int bytes ) = 0;
	virtual int				GetCommClientNum( int channel ) const = 0;
	virtual int				GetNumVoiceChannels( void ) const = 0;
};

// Quake 4's game gives sound distances in map units (its shaders' and its entities'
// s_mindistance / s_maxdistance); the engine keeps metres (GameTitle soundUnitsToMeters)
const float SOUND_Q4_UNITS_TO_METERS	= DOOM_TO_METERS;

// idSoundShader's Quake 4 form of GetParms: the shader's own parameters in map units, with the
// Quake 4 ones at their neutral values (no pitch shift, full wet and dry levels)
ID_INLINE void idSoundShader::GetParms( soundShaderParmsQ4_t *out ) const {
	const soundShaderParms_t *p = GetParms();
	out->minDistance = p->minDistance * ( 1.0f / SOUND_Q4_UNITS_TO_METERS );
	out->maxDistance = p->maxDistance * ( 1.0f / SOUND_Q4_UNITS_TO_METERS );
	out->volume = p->volume;
	out->attenuatedVolume = 0.0f;
	out->shakes = p->shakes;
	out->soundShaderFlags = p->soundShaderFlags;
	out->soundClass = p->soundClass;
	out->frequencyShift = 1.0f;
	out->wetLevel = 1.0f;
	out->dryLevel = 1.0f;
}

#endif /* !__SOUNDQ4_H__ */
