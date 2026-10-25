/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: the Quake 4 game's sound system (quake4/SoundQ4.h) over the engine's sound worlds. The
// game names a world by id and an emitter by its index in that world, which is the engine
// emitter's own index (idSoundEmitter::Index, 1 and up), so a handle survives save games the
// way Doom 3's indices do. Distances come in map units and go to the engine in metres.
//
// Volumes are a linear scale in the game and dB in the engine (S_SoundParmsQ4ToEngine).
//
// Not carried over yet (Quake 4 parameters the engine's mixer has no slot for): doppler from
// the emitter velocity, the pitch shift, the wet/dry mix and the attenuated volume - accepted
// and dropped. Voice chat is multiplayer (Q9): recording reports no microphone.

#include "sys/platform.h"
#include "framework/Session_local.h"
#include "sound/snd_local.h"

#include "quake4/EngineQ4.h"

class idSoundSystemQ4Local;

/*
===============================================================================

	idSoundEmitterQ4Local - a handle's emitter, looked up in its world on every call

===============================================================================
*/

class idSoundEmitterQ4Local : public idSoundEmitterQ4 {
public:
							idSoundEmitterQ4Local( int worldId, int handle ) : worldId( worldId ), handle( handle ) {}

	virtual void			UpdateEmitter( const idVec3 &origin, const idVec3 &velocity, int listenerId, const soundShaderParmsQ4_t *parms );
	virtual int				StartSound( const idSoundShader *shader, const s_channelType channel, float diversity, int shaderFlags );
	virtual void			ModifySound( const s_channelType channel, const soundShaderParmsQ4_t *parms );
	virtual void			StopSound( const s_channelType channel );
	virtual void			FadeSound( const s_channelType channel, float to, float over );
	virtual bool			CurrentlyPlaying( const s_channelType channel ) const;
	virtual float			CurrentAmplitude( int channelFlags, bool factorDistance );
	virtual bool			AttachedToWorld( int worldId ) const;
	virtual int				Handle( void ) const { return handle; }

private:
	idSoundEmitterLocal *	Emitter( void ) const;

	int						worldId;
	int						handle;
};

/*
===============================================================================

	idSoundSystemQ4Local

===============================================================================
*/

class idSoundSystemQ4Local : public idSoundSystemQ4 {
public:
	virtual					~idSoundSystemQ4Local( void );

	virtual int				AllocSoundEmitter( int worldId );
	virtual void			FreeSoundEmitter( int worldId, int handle, bool immediate );
	virtual idSoundEmitterQ4 *EmitterForIndex( int worldId, int handle );

	virtual void			StopAllSounds( int worldId );
	virtual void			SetMute( bool mute );
	virtual int				SetActiveSoundWorld( bool on );

	virtual void			PlaceListener( const idVec3 &origin, const idMat3 &axis, const int listenerId, const int gameTime, const idStr &areaName );
	virtual void			ResetListener( void );
	virtual float			CurrentShakeAmplitudeForPosition( int worldId, const int time, const idVec3 &listenerPosition );
	virtual void			FadeSoundClasses( int worldId, const int soundClass, float to, const float over );
	virtual void			PlayShaderDirectly( int worldId, const char *name, int channel );
	virtual void			EndCinematic( void );

	virtual void			WriteToSaveGame( int worldId, idFile *savefile );
	virtual void			ReadFromSaveGame( int worldId, idFile *savefile );

	virtual bool			EnableRecording( bool enable, bool test, float &micLevel );
	virtual int				GetVoiceData( byte *buffer, int maxSize );
	virtual void			PlayVoiceData( int clientNum, const byte *buffer, int bytes );
	virtual int				GetCommClientNum( int channel ) const;
	virtual int				GetNumVoiceChannels( void ) const;

							// the engine world behind an id, NULL when there is none
	static idSoundWorldLocal *World( int worldId );
							// the engine emitter behind a handle, NULL when the handle is not live
	static idSoundEmitterLocal *Emitter( int worldId, int handle );

private:
	idList<idSoundEmitterQ4Local *>	emitters[SOUNDWORLD_MAX];	// by handle, made on first use
};

/*
===============
S_SoundParmsQ4ToEngine

The game's play parameters as the engine's: distances to metres, the Quake 4-only ones dropped.
===============
*/
void S_SoundParmsQ4ToEngine( const soundShaderParmsQ4_t &src, soundShaderParms_t &dst ) {
	dst.minDistance = src.minDistance * SOUND_Q4_UNITS_TO_METERS;
	dst.maxDistance = src.maxDistance * SOUND_Q4_UNITS_TO_METERS;
	// the game's volume is a linear scale (Entity.cpp turns an entity's s_volume dB into one), the
	// engine's dB; 0 means "not set" on both sides. A scale of exactly 1 is 0 dB, which the engine
	// would read as "not set" (OverrideParms) and so keep the shader's own volume: nudge it.
	if ( src.volume > 0.0f ) {
		dst.volume = 20.0f * log10f( src.volume );
		if ( dst.volume == 0.0f ) {
			dst.volume = -0.0001f;
		}
	} else {
		dst.volume = 0.0f;
	}
	dst.shakes = src.shakes;
	dst.soundShaderFlags = src.soundShaderFlags;
	dst.soundClass = src.soundClass;
}

/*
===============
idSoundSystemQ4Local::World
===============
*/
idSoundWorldLocal *idSoundSystemQ4Local::World( int worldId ) {
	idSoundWorld *world = NULL;
	switch ( worldId ) {
		case SOUNDWORLD_GAME:	world = sessLocal.sw; break;
		case SOUNDWORLD_MENU:	world = sessLocal.menuSoundWorld; break;
		default:				break;		// the editor's world: the engine's editors have their own
	}
	// every sound world is the sound system's own (idSoundSystemLocal::AllocSoundWorld)
	return static_cast<idSoundWorldLocal *>( world );
}

/*
===============
idSoundSystemQ4Local::Emitter
===============
*/
idSoundEmitterLocal *idSoundSystemQ4Local::Emitter( int worldId, int handle ) {
	idSoundWorldLocal *world = World( worldId );
	if ( world == NULL || handle <= 0 || handle >= world->emitters.Num() ) {
		return NULL;
	}
	idSoundEmitterLocal *emitter = world->emitters[handle];
	if ( emitter == NULL || emitter->removeStatus != REMOVE_STATUS_ALIVE ) {
		return NULL;
	}
	return emitter;
}

idSoundSystemQ4Local::~idSoundSystemQ4Local( void ) {
	for ( int i = 0; i < SOUNDWORLD_MAX; i++ ) {
		emitters[i].DeleteContents( true );
	}
}

int idSoundSystemQ4Local::AllocSoundEmitter( int worldId ) {
	idSoundWorldLocal *world = World( worldId );
	if ( world == NULL ) {
		return 0;
	}
	return world->AllocSoundEmitter()->Index();
}

void idSoundSystemQ4Local::FreeSoundEmitter( int worldId, int handle, bool immediate ) {
	idSoundEmitterLocal *emitter = Emitter( worldId, handle );
	if ( emitter != NULL ) {
		emitter->Free( immediate );
	}
}

idSoundEmitterQ4 *idSoundSystemQ4Local::EmitterForIndex( int worldId, int handle ) {
	if ( worldId <= SOUNDWORLD_NONE || worldId >= SOUNDWORLD_MAX || Emitter( worldId, handle ) == NULL ) {
		return NULL;
	}
	idList<idSoundEmitterQ4Local *> &list = emitters[worldId];
	if ( handle >= list.Num() ) {
		const int oldNum = list.Num();
		list.SetNum( handle + 1 );
		for ( int i = oldNum; i < list.Num(); i++ ) {
			list[i] = NULL;
		}
	}
	if ( list[handle] == NULL ) {
		list[handle] = new idSoundEmitterQ4Local( worldId, handle );
	}
	return list[handle];
}

void idSoundSystemQ4Local::StopAllSounds( int worldId ) {
	for ( int i = SOUNDWORLD_GAME; i < SOUNDWORLD_MAX; i++ ) {
		if ( worldId == i || worldId == SOUNDWORLD_ANY ) {
			idSoundWorldLocal *world = World( i );
			if ( world != NULL ) {
				world->StopAllSounds();
			}
		}
	}
}

void idSoundSystemQ4Local::SetMute( bool mute ) {
	soundSystem->SetMute( mute );
}

int idSoundSystemQ4Local::SetActiveSoundWorld( bool on ) {
	const int wasOn = ( sessLocal.sw != NULL && soundSystem->GetPlayingSoundWorld() == sessLocal.sw );
	idSoundWorld *world = on ? sessLocal.sw : sessLocal.menuSoundWorld;
	if ( world != NULL ) {
		soundSystem->SetPlayingSoundWorld( world );
	}
	return wasOn;
}

void idSoundSystemQ4Local::PlaceListener( const idVec3 &origin, const idMat3 &axis, const int listenerId, const int gameTime, const idStr &areaName ) {
	idSoundWorldLocal *world = World( SOUNDWORLD_GAME );
	if ( world != NULL ) {
		world->PlaceListener( origin, axis, listenerId, gameTime, areaName );
	}
}

void idSoundSystemQ4Local::ResetListener( void ) {
	// the engine re-spatializes from the next PlaceListener; nothing is cached to reset
}

float idSoundSystemQ4Local::CurrentShakeAmplitudeForPosition( int worldId, const int time, const idVec3 &listenerPosition ) {
	idSoundWorldLocal *world = World( worldId );
	return world != NULL ? world->CurrentShakeAmplitudeForPosition( time, listenerPosition ) : 0.0f;
}

void idSoundSystemQ4Local::FadeSoundClasses( int worldId, const int soundClass, float to, const float over ) {
	idSoundWorldLocal *world = World( worldId );
	if ( world != NULL ) {
		world->FadeSoundClasses( soundClass, to, over );
	}
}

void idSoundSystemQ4Local::PlayShaderDirectly( int worldId, const char *name, int channel ) {
	idSoundWorldLocal *world = World( worldId );
	if ( world != NULL ) {
		world->PlayShaderDirectly( name, channel );
	}
}

void idSoundSystemQ4Local::EndCinematic( void ) {
	// the engine's cinematic sounds are ordinary emitters the game stops itself
}

void idSoundSystemQ4Local::WriteToSaveGame( int worldId, idFile *savefile ) {
	idSoundWorldLocal *world = World( worldId );
	if ( world != NULL ) {
		world->WriteToSaveGame( savefile );
	}
}

void idSoundSystemQ4Local::ReadFromSaveGame( int worldId, idFile *savefile ) {
	idSoundWorldLocal *world = World( worldId );
	if ( world != NULL ) {
		world->ReadFromSaveGame( savefile );
	}
}

bool idSoundSystemQ4Local::EnableRecording( bool enable, bool test, float &micLevel ) {
	micLevel = 0.0f;
	return false;
}

int idSoundSystemQ4Local::GetVoiceData( byte *buffer, int maxSize ) {
	return 0;
}

void idSoundSystemQ4Local::PlayVoiceData( int clientNum, const byte *buffer, int bytes ) {
}

int idSoundSystemQ4Local::GetCommClientNum( int channel ) const {
	return -1;
}

int idSoundSystemQ4Local::GetNumVoiceChannels( void ) const {
	return 0;
}

/*
===============================================================================

	idSoundEmitterQ4Local

===============================================================================
*/

idSoundEmitterLocal *idSoundEmitterQ4Local::Emitter( void ) const {
	return idSoundSystemQ4Local::Emitter( worldId, handle );
}

void idSoundEmitterQ4Local::UpdateEmitter( const idVec3 &origin, const idVec3 &velocity, int listenerId, const soundShaderParmsQ4_t *parms ) {
	idSoundEmitterLocal *emitter = Emitter();
	if ( emitter == NULL ) {
		return;
	}
	if ( parms != NULL ) {
		soundShaderParms_t engineParms;
		S_SoundParmsQ4ToEngine( *parms, engineParms );
		emitter->UpdateEmitter( origin, listenerId, &engineParms );
	} else {
		emitter->UpdateEmitter( origin, listenerId, NULL );
	}
}

int idSoundEmitterQ4Local::StartSound( const idSoundShader *shader, const s_channelType channel, float diversity, int shaderFlags ) {
	idSoundEmitterLocal *emitter = Emitter();
	return emitter != NULL ? emitter->StartSound( shader, channel, diversity, shaderFlags ) : 0;
}

void idSoundEmitterQ4Local::ModifySound( const s_channelType channel, const soundShaderParmsQ4_t *parms ) {
	idSoundEmitterLocal *emitter = Emitter();
	if ( emitter != NULL && parms != NULL ) {
		soundShaderParms_t engineParms;
		S_SoundParmsQ4ToEngine( *parms, engineParms );
		emitter->ModifySound( channel, &engineParms );
	}
}

void idSoundEmitterQ4Local::StopSound( const s_channelType channel ) {
	idSoundEmitterLocal *emitter = Emitter();
	if ( emitter != NULL ) {
		emitter->StopSound( channel );
	}
}

void idSoundEmitterQ4Local::FadeSound( const s_channelType channel, float to, float over ) {
	idSoundEmitterLocal *emitter = Emitter();
	if ( emitter != NULL ) {
		emitter->FadeSound( channel, to, over );
	}
}

bool idSoundEmitterQ4Local::CurrentlyPlaying( const s_channelType channel ) const {
	const idSoundEmitterLocal *emitter = Emitter();
	if ( emitter == NULL || !emitter->playing ) {
		return false;
	}
	if ( channel == SCHANNEL_ANY ) {
		return true;
	}
	for ( int i = 0; i < SOUND_MAX_CHANNELS; i++ ) {
		const idSoundChannel &chan = emitter->channels[i];
		if ( chan.triggerState && chan.triggerChannel == channel ) {
			return true;
		}
	}
	return false;
}

float idSoundEmitterQ4Local::CurrentAmplitude( int channelFlags, bool factorDistance ) {
	// the engine measures every channel together, at the source
	idSoundEmitterLocal *emitter = Emitter();
	return emitter != NULL ? emitter->CurrentAmplitude() : 0.0f;
}

bool idSoundEmitterQ4Local::AttachedToWorld( int worldId ) const {
	return worldId == SOUNDWORLD_ANY || worldId == this->worldId;
}

/*
===============================================================================

	access

===============================================================================
*/

idSoundSystemQ4 *SoundSystemQ4( void ) {
	static idSoundSystemQ4Local soundSystemQ4Local;
	return &soundSystemQ4Local;
}

idSoundEmitter *SoundEmitterForHandleQ4( int handle ) {
	return idSoundSystemQ4Local::Emitter( SOUNDWORLD_GAME, handle );
}
