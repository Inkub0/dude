/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: the Quake 4 game library as the engine runs it (docs/quake4-port.md 5.6, Q5.4). The
// session, the renderer and the tools talk to "the game" through Doom 3's idGame and idGameEdit;
// for Quake 4 those are adapters onto the library's idGameQ4 and idGameEditQ4, converting what
// the two games describe differently (user commands, render worlds, render lights / entities,
// sounds) on the way.
//
// Multiplayer entries (snapshots, the server's client admission) are Q9: the single-player
// session never calls them; they forward what converts and answer "no" otherwise.

#include "sys/platform.h"
#include "framework/Common.h"
#include "framework/Game.h"
#include "framework/Session.h"
#include "framework/async/AsyncNetwork.h"
#include "framework/FileSystem.h"
#include "framework/async/NetworkSystem.h"
#include "renderer/RenderSystem.h"
#include "renderer/ModelManager.h"
#include "ui/UserInterface.h"
#include "tools/compilers/aas/AASFileManager.h"
#include "renderer/bse/BseEffect.h"

#include "quake4/EngineQ4.h"
#include "quake4/GameLoaderQ4.h"

#ifdef __DOOM_DLL__

static idGameQ4 *		gameQ4 = NULL;
static idGameEditQ4 *	gameEditQ4 = NULL;
static bool				gameQ4MapActive = false;	// from the start of a map's load to its shutdown

/*
===============
UsercmdsToQ4

The session's commands for every client, as the game's: Doom 3's buttons are Quake 4's low
byte, and the real time is the command's game time (the engine stamps no other).
===============
*/
static void UsercmdToQ4( const usercmd_t &src, usercmdQ4_t &dst ) {
	dst.gameFrame = src.gameFrame;
	dst.gameTime = src.gameTime;
	dst.realTime = src.gameTime;
	dst.duplicateCount = src.duplicateCount;
	dst.buttons = src.buttons;
	dst.forwardmove = src.forwardmove;
	dst.rightmove = src.rightmove;
	dst.upmove = src.upmove;
	dst.angles[0] = src.angles[0];
	dst.angles[1] = src.angles[1];
	dst.angles[2] = src.angles[2];
	dst.mx = src.mx;
	dst.my = src.my;
	dst.impulse = src.impulse;
	dst.flags = src.flags;
	dst.sequence = src.sequence;
}

/*
===============
UsercmdsToQ4

The single-player session hands the game one command (its own, Session.cpp RunGameTic); the
network server and client hand one per client slot. The game keeps the pointer it was given
(gameLocal.usercmds) past the frame, so the converted commands live here, not on the stack.
===============
*/
static usercmdQ4_t			usercmdsQ4[MAX_ASYNC_CLIENTS];

static const usercmdQ4_t *UsercmdsToQ4( const usercmd_t *src ) {
	const int num = idAsyncNetwork::IsActive() ? MAX_ASYNC_CLIENTS : 1;
	memset( usercmdsQ4, 0, sizeof( usercmdsQ4 ) );
	for ( int i = 0; i < num; i++ ) {
		UsercmdToQ4( src[i], usercmdsQ4[i] );
	}
	return usercmdsQ4;
}

/*
===============================================================================

	idGameAdapterQ4 - idGameQ4 as the session's idGame

===============================================================================
*/

class idGameAdapterQ4 : public idGame {
public:
	virtual void				Init( void ) { gameQ4->Init(); }
	virtual void				Shutdown( void ) { gameQ4->Shutdown(); }

	virtual void				SetLocalClient( int clientNum ) { gameQ4->SetLocalClient( clientNum ); }
	virtual const idDict *		SetUserInfo( int clientNum, const idDict &userInfo, bool isClient, bool canModify ) { return gameQ4->SetUserInfo( clientNum, userInfo, isClient ); }
	virtual const idDict *		GetUserInfo( int clientNum ) { return gameQ4->GetUserInfo( clientNum ); }
	virtual void				ThrottleUserInfo( void ) { gameQ4->ThrottleUserInfo(); }
	virtual void				SetServerInfo( const idDict &serverInfo ) { gameQ4->SetServerInfo( serverInfo ); }
	virtual const idDict &		GetPersistentPlayerInfo( int clientNum ) { return gameQ4->GetPersistentPlayerInfo( clientNum ); }
	virtual void				SetPersistentPlayerInfo( int clientNum, const idDict &playerInfo ) { gameQ4->SetPersistentPlayerInfo( clientNum, playerInfo ); }

	// the game's sounds go to the session's game sound world by id (SoundSystemQ4)
	virtual void				InitFromNewMap( const char *mapName, idRenderWorld *renderWorld, idSoundWorld *soundWorld, bool isServer, bool isClient, int randseed ) {
									gameQ4MapActive = true;
									BSE_ResetRateTimes();		// as retail's effect manager at a level load
									gameQ4->InitFromNewMap( mapName, RenderWorldQ4( renderWorld ), isServer, isClient, randseed );
								}
	virtual bool				InitFromSaveGame( const char *mapName, idRenderWorld *renderWorld, idSoundWorld *soundWorld, idFile *saveGameFile ) {
									gameQ4MapActive = true;
									BSE_ResetRateTimes();
									return gameQ4->InitFromSaveGame( mapName, RenderWorldQ4( renderWorld ), saveGameFile );
								}
	virtual void				SaveGame( idFile *saveGameFile ) { gameQ4->SaveGame( saveGameFile, ST_REGULAR ); }
	virtual void				MapShutdown( void ) { gameQ4->MapShutdown(); gameQ4MapActive = false; }
	virtual void				CacheDictionaryMedia( const idDict *dict ) { gameQ4->CacheDictionaryMedia( dict ); }
	virtual void				SpawnPlayer( int clientNum ) { gameQ4->SpawnPlayer( clientNum ); }

	virtual gameReturn_t		RunFrame( const usercmd_t *clientCmds ) {
									return gameQ4->RunFrame( UsercmdsToQ4( clientCmds ), com_editors, true, 0 );
								}
	virtual bool				Draw( int clientNum ) { return gameQ4->Draw( clientNum ); }

	virtual escReply_t			HandleESC( idUserInterface **gui ) { return gameQ4->HandleESC( gui ); }
	virtual idUserInterface *	StartMenu( void ) { return gameQ4->StartMenu(); }
	virtual const char *		HandleGuiCommands( const char *menuCommand ) { return gameQ4->HandleGuiCommands( menuCommand ); }
	virtual void				HandleMainMenuCommands( const char *menuCommand, idUserInterface *gui ) { gameQ4->HandleMainMenuCommands( menuCommand, gui ); }

	virtual allowReply_t		ServerAllowClient( int numClients, const char *IP, const char *guid, const char *password, char reason[MAX_STRING_CHARS] ) {
									return gameQ4->ServerAllowClient( -1, numClients, IP, guid, password, "", reason );
								}
	virtual void				ServerClientConnect( int clientNum, const char *guid ) { gameQ4->ServerClientConnect( clientNum, guid ); }
	virtual void				ServerClientBegin( int clientNum ) { gameQ4->ServerClientBegin( clientNum ); }
	virtual void				ServerClientDisconnect( int clientNum ) { gameQ4->ServerClientDisconnect( clientNum ); }
	virtual void				ServerWriteInitialReliableMessages( int clientNum ) { gameQ4->ServerWriteInitialReliableMessages( clientNum ); }
	virtual void				ServerWriteSnapshot( int clientNum, int sequence, idBitMsg &msg, byte *clientInPVS, int numPVSClients ) {
									// Quake 4's snapshots carry its PVS as dwords and a delta frame (Q9)
								}
	virtual bool				ServerApplySnapshot( int clientNum, int sequence ) { return gameQ4->ServerApplySnapshot( clientNum, sequence ); }
	virtual void				ServerProcessReliableMessage( int clientNum, const idBitMsg &msg ) { gameQ4->ServerProcessReliableMessage( clientNum, msg ); }

	virtual void				ClientReadSnapshot( int clientNum, int sequence, const int gameFrame, const int gameTime, const int dupeUsercmds, const int aheadOfServer, const idBitMsg &msg ) {
									gameQ4->ClientReadSnapshot( clientNum, sequence, gameFrame, gameTime, dupeUsercmds, aheadOfServer, msg );
								}
	virtual bool				ClientApplySnapshot( int clientNum, int sequence ) { return gameQ4->ClientApplySnapshot( clientNum, sequence ); }
	virtual void				ClientProcessReliableMessage( int clientNum, const idBitMsg &msg ) { gameQ4->ClientProcessReliableMessage( clientNum, msg ); }
	virtual gameReturn_t		ClientPrediction( int clientNum, const usercmd_t *clientCmds, bool lastPredictFrame ) {
									return gameQ4->ClientPrediction( clientNum, UsercmdsToQ4( clientCmds ), lastPredictFrame, NULL );
								}

	// Quake 4 has no time groups (RoE's slow motion): every group runs on the game's clock
	virtual void				SelectTimeGroup( int timeGroup ) {}
	virtual int					GetTimeGroupTime( int timeGroup ) { return gameEditQ4->GetGameTime( NULL ); }

	virtual void				GetBestGameType( const char *map, const char *gametype, char buf[ MAX_STRING_CHARS ] ) { idStr::Copynz( buf, gametype, MAX_STRING_CHARS ); }
	virtual void				GetClientStats( int clientNum, char *data, const int len ) { gameQ4->GetClientStats( clientNum, data, len ); }
	virtual void				SwitchTeam( int clientNum, int team ) { gameQ4->SwitchTeam( clientNum, team ); }
	virtual bool				DownloadRequest( const char *IP, const char *guid, const char *paks, char urls[ MAX_STRING_CHARS ] ) { return gameQ4->DownloadRequest( IP, guid, paks, urls ); }

	// the session picks Quake 4's loading screen and fills it from the mapDef itself
	virtual void				GetMapLoadingGUI( char gui[ MAX_STRING_CHARS ] ) {}
};

static idGameAdapterQ4		gameAdapterQ4;
static idGameEdit			gameEditAdapterQ4;

/*
===============================================================================

	idGameEdit - for Quake 4, onto idGameEditQ4

	The engine itself defines Doom 3's idGameEdit only here: a Doom 3 game library brings its
	own (and a hard-linked Doom 3 game is never a Quake 4 one, hence __DOOM_DLL__).

===============================================================================
*/

void idGameEdit::ParseSpawnArgsToRenderLight( const idDict *args, renderLight_t *renderLight ) {
	renderLightQ4_t light;
	memset( &light, 0, sizeof( light ) );
	gameEditQ4->ParseSpawnArgsToRenderLight( args, &light );
	R_RenderLightQ4ToEngine( light, *renderLight );
}

void idGameEdit::ParseSpawnArgsToRenderEntity( const idDict *args, renderEntity_t *renderEntity ) {
	renderEntityQ4_t entity;
	memset( &entity, 0, sizeof( entity ) );
	gameEditQ4->ParseSpawnArgsToRenderEntity( args, &entity );
	R_RenderEntityQ4ToEngine( entity, *renderEntity );
}

void idGameEdit::ParseSpawnArgsToRefSound( const idDict *args, refSound_t *refSound ) {
	refSoundQ4_t sound;
	memset( &sound, 0, sizeof( sound ) );
	gameEditQ4->ParseSpawnArgsToRefSound( args, &sound );
	memset( refSound, 0, sizeof( *refSound ) );
	refSound->origin = sound.origin;
	refSound->listenerId = sound.listenerId;
	refSound->shader = sound.shader;
	refSound->diversity = sound.diversity;
	refSound->waitfortrigger = sound.waitfortrigger;
	S_SoundParmsQ4ToEngine( sound.parms, refSound->parms );
}

idRenderModel *	idGameEdit::ANIM_GetModelFromEntityDef( const char *classname ) { return gameEditQ4->ANIM_GetModelFromEntityDef( classname ); }
const idVec3 &	idGameEdit::ANIM_GetModelOffsetFromEntityDef( const char *classname ) { return gameEditQ4->ANIM_GetModelOffsetFromEntityDef( classname ); }
idRenderModel *	idGameEdit::ANIM_GetModelFromEntityDef( const idDict *args ) { return gameEditQ4->ANIM_GetModelFromEntityDef( args ); }
idRenderModel *	idGameEdit::ANIM_GetModelFromName( const char *modelName ) { return gameEditQ4->ANIM_GetModelFromName( modelName ); }
const idMD5Anim *idGameEdit::ANIM_GetAnimFromEntityDef( const char *classname, const char *animname ) { return gameEditQ4->ANIM_GetAnimFromEntityDef( classname, animname ); }
int				idGameEdit::ANIM_GetNumAnimsFromEntityDef( const idDict *args ) { return gameEditQ4->ANIM_GetNumAnimsFromEntityDef( args ); }
const char *	idGameEdit::ANIM_GetAnimNameFromEntityDef( const idDict *args, int animNum ) { return gameEditQ4->ANIM_GetAnimNameFromEntityDef( args, animNum ); }
const idMD5Anim *idGameEdit::ANIM_GetAnim( const char *fileName ) { return gameEditQ4->ANIM_GetAnim( fileName ); }
int				idGameEdit::ANIM_GetLength( const idMD5Anim *anim ) { return gameEditQ4->ANIM_GetLength( anim ); }
int				idGameEdit::ANIM_GetNumFrames( const idMD5Anim *anim ) { return gameEditQ4->ANIM_GetNumFrames( anim ); }
void			idGameEdit::ANIM_CreateAnimFrame( const idRenderModel *model, const idMD5Anim *anim, int numJoints, idJointMat *frame, int time, const idVec3 &offset, bool remove_origin_offset ) {
					gameEditQ4->ANIM_CreateAnimFrame( model, anim, numJoints, frame, time, offset, remove_origin_offset );
				}
idRenderModel *	idGameEdit::ANIM_CreateMeshForAnim( idRenderModel *model, const char *classname, const char *animname, int frame, bool remove_origin_offset ) {
					return gameEditQ4->ANIM_CreateMeshForAnim( model, classname, animname, frame, remove_origin_offset );
				}

bool			idGameEdit::AF_SpawnEntity( const char *fileName ) { return gameEditQ4->AF_SpawnEntity( fileName ); }
void			idGameEdit::AF_UpdateEntities( const char *fileName ) { gameEditQ4->AF_UpdateEntities( fileName ); }
void			idGameEdit::AF_UndoChanges( void ) { gameEditQ4->AF_UndoChanges(); }
idRenderModel *	idGameEdit::AF_CreateMesh( const idDict &args, idVec3 &meshOrigin, idMat3 &meshAxis, bool &poseIsSet ) { return gameEditQ4->AF_CreateMesh( args, meshOrigin, meshAxis, poseIsSet ); }

void			idGameEdit::ClearEntitySelection( void ) { gameEditQ4->ClearEntitySelection(); }
int				idGameEdit::GetSelectedEntities( idEntity *list[], int max ) { return gameEditQ4->GetSelectedEntities( list, max ); }
void			idGameEdit::AddSelectedEntity( idEntity *ent ) { gameEditQ4->AddSelectedEntity( ent ); }
void			idGameEdit::TriggerSelected( void ) { gameEditQ4->TriggerSelected(); }

const idDict *	idGameEdit::FindEntityDefDict( const char *name, bool makeDefault ) const { return gameEditQ4->FindEntityDefDict( name, makeDefault ); }
void			idGameEdit::SpawnEntityDef( const idDict &args, idEntity **ent ) { gameEditQ4->SpawnEntityDef( args, ent ); }
idEntity *		idGameEdit::FindEntity( const char *name ) const { return gameEditQ4->FindEntity( name ); }
const char *	idGameEdit::GetUniqueEntityName( const char *classname ) const { return gameEditQ4->GetUniqueEntityName( classname ); }

void			idGameEdit::EntityGetOrigin( idEntity *ent, idVec3 &org ) const { gameEditQ4->EntityGetOrigin( ent, org ); }
void			idGameEdit::EntityGetAxis( idEntity *ent, idMat3 &axis ) const { gameEditQ4->EntityGetAxis( ent, axis ); }
void			idGameEdit::EntitySetOrigin( idEntity *ent, const idVec3 &org ) { gameEditQ4->EntitySetOrigin( ent, org ); }
void			idGameEdit::EntitySetAxis( idEntity *ent, const idMat3 &axis ) { gameEditQ4->EntitySetAxis( ent, axis ); }
void			idGameEdit::EntityTranslate( idEntity *ent, const idVec3 &org ) { gameEditQ4->EntityTranslate( ent, org ); }
const idDict *	idGameEdit::EntityGetSpawnArgs( idEntity *ent ) const { return gameEditQ4->EntityGetSpawnArgs( ent ); }
void			idGameEdit::EntityUpdateChangeableSpawnArgs( idEntity *ent, const idDict *dict ) { gameEditQ4->EntityUpdateChangeableSpawnArgs( ent, dict ); }
void			idGameEdit::EntityChangeSpawnArgs( idEntity *ent, const idDict *newArgs ) { gameEditQ4->EntityChangeSpawnArgs( ent, newArgs ); }
void			idGameEdit::EntityUpdateVisuals( idEntity *ent ) { gameEditQ4->EntityUpdateVisuals( ent ); }
void			idGameEdit::EntitySetModel( idEntity *ent, const char *val ) { gameEditQ4->EntitySetModel( ent, val ); }
void			idGameEdit::EntityStopSound( idEntity *ent ) { gameEditQ4->EntityStopSound( ent ); }
void			idGameEdit::EntityDelete( idEntity *ent ) { gameEditQ4->EntityDelete( ent ); }
void			idGameEdit::EntitySetColor( idEntity *ent, const idVec3 color ) { gameEditQ4->EntitySetColor( ent, color ); }

bool			idGameEdit::PlayerIsValid( void ) const { return gameEditQ4->PlayerIsValid(); }
void			idGameEdit::PlayerGetOrigin( idVec3 &org ) const { gameEditQ4->PlayerGetOrigin( org ); }
void			idGameEdit::PlayerGetAxis( idMat3 &axis ) const { gameEditQ4->PlayerGetAxis( axis ); }
void			idGameEdit::PlayerGetViewAngles( idAngles &angles ) const { gameEditQ4->PlayerGetViewAngles( angles ); }
void			idGameEdit::PlayerGetEyePosition( idVec3 &org ) const { gameEditQ4->PlayerGetEyePosition( org ); }

const idDict *	idGameEdit::MapGetEntityDict( const char *name ) const { return gameEditQ4->MapGetEntityDict( name ); }
void			idGameEdit::MapSave( const char *path ) const { gameEditQ4->MapSave( path ); }
void			idGameEdit::MapSetEntityKeyVal( const char *name, const char *key, const char *val ) const { gameEditQ4->MapSetEntityKeyVal( name, key, val ); }
void			idGameEdit::MapCopyDictToEntity( const char *name, const idDict *dict ) const { gameEditQ4->MapCopyDictToEntity( name, dict ); }
int				idGameEdit::MapGetUniqueMatchingKeyVals( const char *key, const char *list[], const int max ) const { return gameEditQ4->MapGetUniqueMatchingKeyVals( key, list, max ); }
void			idGameEdit::MapAddEntity( const idDict *dict ) const { gameEditQ4->MapAddEntity( dict ); }
int				idGameEdit::MapGetEntitiesMatchingClassWithString( const char *classname, const char *match, const char *list[], const int max ) const {
					return gameEditQ4->MapGetEntitiesMatchingClassWithString( classname, match, list, max );
				}
void			idGameEdit::MapRemoveEntity( const char *name ) const { gameEditQ4->MapRemoveEntity( name ); }
void			idGameEdit::MapEntityTranslate( const char *name, const idVec3 &v ) const { gameEditQ4->MapEntityTranslate( name, v ); }

/*
===============================================================================

	loading

===============================================================================
*/

bool GameQ4_Load( void *getGameAPI, idGame **game, idGameEdit **gameEdit ) {
	gameImportQ4_t import;
	memset( &import, 0, sizeof( import ) );
	import.version					= GAME_API_VERSION_Q4;
	import.sys						= ::sys;
	import.common					= ::common;
	import.cmdSystem				= ::cmdSystem;
	import.cvarSystem				= ::cvarSystem;
	import.fileSystem				= ::fileSystem;
	import.networkSystem			= ::networkSystem;
	import.renderSystem				= ::renderSystem;
	import.soundSystemQ4			= SoundSystemQ4();
	import.renderModelManager		= ::renderModelManager;
	import.uiManager				= ::uiManager;
	import.declManager				= ::declManager;
	import.AASFileManager			= ::AASFileManager;
	import.collisionModelManagerQ4	= CollisionModelManagerQ4();
	import.bse						= EffectManagerQ4();

	const gameExportQ4_t *exports = ( (GetGameAPIQ4_t)getGameAPI )( &import );
	if ( exports == NULL || exports->version != GAME_API_VERSION_Q4 || exports->game == NULL || exports->gameEdit == NULL ) {
		return false;
	}
	gameQ4 = exports->game;
	gameEditQ4 = exports->gameEdit;
	*game = &gameAdapterQ4;
	*gameEdit = &gameEditAdapterQ4;
	return true;
}

void GameQ4_Unload( void ) {
	FreeRenderWorldsQ4();
	gameQ4 = NULL;
	gameEditQ4 = NULL;
	gameQ4MapActive = false;
}

idGameQ4 *GameQ4( void ) {
	return gameQ4;
}

bool GameQ4_MapActive( void ) {
	return gameQ4 != NULL && gameQ4MapActive;
}

#else

idGameQ4 *GameQ4( void ) {
	return NULL;
}

bool GameQ4_MapActive( void ) {
	return false;
}

#endif /* __DOOM_DLL__ */
