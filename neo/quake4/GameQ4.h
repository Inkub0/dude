/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: the Quake 4 game library's interface (docs/quake4-port.md 5.6). When the title is
// Quake 4 the engine loads its game library with this import block - the engine's own systems
// where Quake 4 asks the same of them as Doom 3 (with the methods Quake 4 adds, appended), the
// Quake 4 facades where it asks differently (sound, collision, effects) - and drives the game
// through idGameQ4, which an engine-side adapter presents to the session as an idGame. Doom 3's
// game interface (framework/Game.h) and its ABI stay as they are.
//
// The Quake 4 game code is compiled with its own names for these types (quake4/GameNamesQ4.h
// maps them); here, and in the engine, they have names of their own.

#ifndef __GAMEQ4_H__
#define __GAMEQ4_H__

#include "idlib/Dict.h"
#include "idlib/BitMsg.h"
#include "idlib/containers/StrList.h"
#include "framework/GameReturn.h"
#include "quake4/UsercmdQ4.h"
#include "quake4/RenderWorldQ4.h"
#include "quake4/SoundQ4.h"
#include "quake4/CollisionModelQ4.h"
#include "quake4/EffectsQ4.h"

class idSys;
class idCommon;
class idCmdSystem;
class idCVarSystem;
class idFileSystem;
class idNetworkSystem;
class idRenderSystem;
class idRenderModelManager;
class idUserInterface;
class idUserInterfaceManager;
class idDeclManager;
class idAASFileManager;
class idAASFile;
class idDecl;
class idEntity;
class idMD5Anim;
class idThread;
class idProgram;
class idInterpreter;
class idCmdArgs;
class rvClientEffect;		// the game's client-side effect entity
class rvGameLog;			// the game's statistics log
class MemInfo;				// the game's memory report

// the game's editors (activeEditors, toolFlag_t): those both games have keep Doom 3's bits, these
// are Quake 4's own
const int EDITOR_FX					= BIT(10);
const int EDITOR_REVERB				= BIT(11);
const int EDITOR_PLAYBACKS			= BIT(12);
const int EDITOR_MODVIEW			= BIT(13);
const int EDITOR_LOGVIEW			= BIT(14);
const int EDITOR_ENTVIEW			= BIT(15);
const int EDITOR_RENDERBUMP			= BIT(18);
const int EDITOR_SPAWN_GUI			= BIT(19);
const int EDITOR_DECL_VALIDATING	= BIT(20);

typedef enum {
	ST_REGULAR,
	ST_QUICK,
	ST_AUTO,
	ST_CHECKPOINT
} saveType_t;

enum demoState_t {
	DEMO_NONE,
	DEMO_RECORDING,
	DEMO_PLAYING
};

// a client's prediction frame
struct ClientStats_t {
	bool					isLastPredictFrame;
	bool					isLagged;
	bool					isNewFrame;
};

// a client's view origin, for a repeater
typedef struct userOrigin_s {
	idVec3					origin;
	int						followClient;
} userOrigin_t;

// GetClassDebugInfo's callback: one variable of an entity
typedef void ( *debugInfoProc_t )( const char *classname, const char *name, const char *value, void *userdata );

/*
===============================================================================

	idGameQ4 - the game, as the engine runs it

===============================================================================
*/

class idGameQ4 {
public:
	virtual						~idGameQ4( void ) {}

	virtual void				Init( void ) = 0;
	virtual void				Shutdown( void ) = 0;

	// clients and their info
	virtual void				SetLocalClient( int clientNum ) = 0;
	virtual const idDict *		SetUserInfo( int clientNum, const idDict &userInfo, bool isClient ) = 0;
	virtual const idDict *		GetUserInfo( int clientNum ) = 0;
	virtual const idDict *		RepeaterSetUserInfo( int clientNum, const idDict &userInfo ) = 0;
	virtual bool				IsClientActive( int clientNum ) = 0;
	virtual void				ThrottleUserInfo( void ) = 0;
	virtual void				SetServerInfo( const idDict &serverInfo ) = 0;
	virtual const idDict &		GetPersistentPlayerInfo( int clientNum ) = 0;
	virtual void				SetPersistentPlayerInfo( int clientNum, const idDict &playerInfo ) = 0;

	// maps and savegames
	virtual void				InitFromNewMap( const char *mapName, idRenderWorldQ4 *renderWorld, bool isServer, bool isClient, int randseed ) = 0;
	virtual bool				InitFromSaveGame( const char *mapName, idRenderWorldQ4 *renderWorld, idFile *saveGameFile ) = 0;
	virtual void				SaveGame( idFile *saveGameFile, saveType_t saveType = ST_REGULAR ) = 0;
	virtual void				MapShutdown( void ) = 0;
	virtual void				CacheDictionaryMedia( const idDict *dict ) = 0;
	virtual void				SpawnPlayer( int clientNum ) = 0;

	// frames
	virtual gameReturn_t		RunFrame( const usercmdQ4_t *clientCmds, int activeEditors, bool lastCatchupFrame, int serverGameFrame ) = 0;
	virtual void				MenuFrame( void ) = 0;
	virtual void				RepeaterFrame( const userOrigin_t *clientOrigins, bool lastCatchupFrame, int serverGameFrame ) = 0;
	virtual bool				Draw( int clientNum ) = 0;

	// menus
	virtual escReply_t			HandleESC( idUserInterface **gui ) = 0;
	virtual idUserInterface *	StartMenu( void ) = 0;
	virtual const char *		HandleGuiCommands( const char *menuCommand ) = 0;
	virtual void				HandleMainMenuCommands( const char *menuCommand, idUserInterface *gui ) = 0;

	// server
	virtual allowReply_t		ServerAllowClient( int clientId, int numClients, const char *IP, const char *guid, const char *password, const char *privatePassword, char reason[MAX_STRING_CHARS] ) = 0;
	virtual void				ServerClientConnect( int clientNum, const char *guid ) = 0;
	virtual void				ServerClientBegin( int clientNum ) = 0;
	virtual void				ServerClientDisconnect( int clientNum ) = 0;
	virtual void				ServerWriteInitialReliableMessages( int clientNum ) = 0;
	virtual void				ServerWriteSnapshot( int clientNum, int sequence, idBitMsg &msg, dword *clientInPVS, int numPVSClients, int lastSnapshotFrame ) = 0;
	virtual bool				ServerApplySnapshot( int clientNum, int sequence ) = 0;
	virtual void				ServerProcessReliableMessage( int clientNum, const idBitMsg &msg ) = 0;

	// repeater
	virtual allowReply_t		RepeaterAllowClient( int clientId, int numClients, const char *IP, const char *guid, bool repeater, const char *password, const char *privatePassword, char reason[MAX_STRING_CHARS] ) = 0;
	virtual void				RepeaterClientConnect( int clientNum ) = 0;
	virtual void				RepeaterClientBegin( int clientNum ) = 0;
	virtual void				RepeaterClientDisconnect( int clientNum ) = 0;
	virtual void				RepeaterWriteInitialReliableMessages( int clientNum ) = 0;
	virtual bool				RepeaterApplySnapshot( int clientNum, int sequence ) = 0;
	virtual void				RepeaterProcessReliableMessage( int clientNum, const idBitMsg &msg ) = 0;
	virtual void				RepeaterWriteSnapshot( int clientNum, int sequence, idBitMsg &msg, dword *clientInPVS, int numPVSClients, const userOrigin_t &pvs_origin, int lastSnapshotFrame ) = 0;
	virtual void				RepeaterEndSnapshots( void ) = 0;

	// client
	virtual void				ClientReadSnapshot( int clientNum, int snapshotSequence, const int gameFrame, const int gameTime, const int dupeUsercmds, const int aheadOfServer, const idBitMsg &msg ) = 0;
	virtual bool				ClientApplySnapshot( int clientNum, int sequence ) = 0;
	virtual void				ClientProcessReliableMessage( int clientNum, const idBitMsg &msg ) = 0;
	virtual gameReturn_t		ClientPrediction( int clientNum, const usercmdQ4_t *clientCmds, bool lastPredictFrame = true, ClientStats_t *cs = NULL ) = 0;
	virtual void				ClientRun( void ) = 0;
	virtual void				ClientEndFrame( void ) = 0;
	virtual void				ClientReadServerDemoSnapshot( int sequence, const int gameFrame, const int gameTime, const idBitMsg &msg ) = 0;
	virtual void				ClientReadRepeaterSnapshot( int sequence, const int gameFrame, const int gameTime, const int aheadOfServer, const idBitMsg &msg ) = 0;
	virtual void				ProcessRconReturn( bool success ) = 0;

	// multiplayer
	virtual bool				ValidateServerSettings( const char *map, const char *gameType ) = 0;
	virtual void				GetClientStats( int clientNum, char *data, const int len ) = 0;
	virtual void				SwitchTeam( int clientNum, int team ) = 0;
	virtual bool				DownloadRequest( const char *IP, const char *guid, const char *paks, char urls[ MAX_STRING_CHARS ] ) = 0;
	virtual bool				HTTPRequest( const char *IP, const char *file, bool isGamePak ) = 0;
	virtual bool				PlayerChatDisabled( int clientNum ) = 0;
	virtual void				GetPlayerName( int clientNum, char *name ) = 0;
	virtual void				GetPlayerClan( int clientNum, char *clan ) = 0;
	virtual void				SetFriend( int clientNum, bool isFriend ) = 0;
	virtual const char *		GetLongGametypeName( const char *gametype ) = 0;
	virtual void				ReceiveRemoteConsoleOutput( const char *output ) = 0;
	virtual void				RegisterClientGuid( int clientNum, const char *guid ) = 0;
	virtual bool				IsMultiplayer( void ) = 0;
	virtual void				PopulateBanList( idUserInterface *hud ) = 0;
	virtual void				RemoveGuidFromBanList( const char *guid ) = 0;
	virtual void				AddGuidToBanList( const char *guid ) = 0;
	virtual const char *		GetGuidByClientNum( int clientNum ) = 0;
	virtual void				UpdatePlayerPostMainMenu( void ) = 0;
	virtual void				ResetRconGuiStatus( void ) = 0;
	virtual void				GetBotInput( int clientNum, usercmdQ4_t &userCmd ) = 0;

	// demos
	virtual void				SetDemoState( demoState_t state, bool serverDemo, bool timeDemo ) = 0;
	virtual void				SetRepeaterState( bool isRepeater, bool serverIsRepeater ) = 0;
	virtual void				WriteNetworkInfo( idFile *file, int clientNum ) = 0;
	virtual void				ReadNetworkInfo( int gameTime, idFile *file, int clientNum ) = 0;
	virtual bool				ValidateDemoProtocol( int minor_ref, int minor ) = 0;
	virtual void				ServerWriteServerDemoSnapshot( int sequence, idBitMsg &msg, int lastSnapshotFrame ) = 0;
	virtual int					GetDemoFollowClient( void ) = 0;

	// the view, effects and the world
	virtual void				StartViewEffect( int type, float time, float scale ) = 0;
	virtual rvClientEffect *	PlayEffect( const idDecl *effect, const idVec3 &origin, const idMat3 &axis, bool loop = false, const idVec3 &endOrigin = vec3_origin,
										bool broadcast = false, bool predictBit = false, effectCategory_t category = EC_IGNORE, const idVec4 &effectTint = idVec4( 1.0f, 1.0f, 1.0f, 1.0f ) ) = 0;
	virtual void				GetPlayerView( idVec3 &origin, idMat3 &axis ) = 0;
	virtual const idVec3		GetCurrentGravity( const idVec3 &origin, const idMat3 &axis ) const = 0;
	virtual void				Translation( traceQ4_t &trace, idVec3 &source, idVec3 &dest, idTraceModel *trm, int clipMask ) = 0;
	virtual void				SpawnClientMoveable( const char *name, int lifetime, const idVec3 &origin, const idMat3 &axis, const idVec3 &velocity, const idVec3 &angular_velocity ) = 0;
	virtual bool				GetPlayerInfo( idVec3 &origin, idMat3 &axis, int PlayerNum = -1, idAngles *deltaViewAngles = NULL, int reqClientNum = -1 ) = 0;
	virtual void				SetPlayerInfo( idVec3 &origin, idMat3 &axis, int PlayerNum = -1 ) = 0;
	virtual bool				InCinematic( void ) = 0;
	virtual void				SetViewComments( const char *text = 0 ) = 0;
	virtual void				ListEntityStats( const idCmdArgs &args ) = 0;

	// the developer HUD
	virtual void				DebugSetString( const char *name, const char *value ) = 0;
	virtual void				DebugSetFloat( const char *name, float value ) = 0;
	virtual void				DebugSetInt( const char *name, int value ) = 0;
	virtual const char *		DebugGetStatString( const char *name ) = 0;
	virtual int					DebugGetStatInt( const char *name ) = 0;
	virtual float				DebugGetStatFloat( const char *name ) = 0;
	virtual bool				IsDebugHudActive( void ) const = 0;

	// the loading screen of a map (its mapDef)
	virtual const char *		GetLoadingGui( const char *mapDeclName ) = 0;
	virtual void				SetupLoadingGui( idUserInterface *gui ) = 0;
};

/*
===============================================================================

	idGameEditQ4 - the game's side of the editors and debugging tools

	Implemented by the game library; the engine calls it for its tools.

===============================================================================
*/

class idGameEditQ4 {
public:
	virtual						~idGameEditQ4( void ) {}

	// the dictionary to render light / entity / sound parsing both the game and the tools use
	virtual bool				ParseSpawnArgsToRenderLight( const idDict *args, renderLightQ4_t *renderLight );
	virtual void				ParseSpawnArgsToRenderEntity( const idDict *args, renderEntityQ4_t *renderEntity );
	virtual void				ParseSpawnArgsToRefSound( const idDict *args, refSoundQ4_t *refSound );

	// skeletal animation outside the game
	virtual idRenderModel *		ANIM_GetModelFromEntityDef( const char *classname );
	virtual const idVec3 &		ANIM_GetModelOffsetFromEntityDef( const char *classname );
	virtual idRenderModel *		ANIM_GetModelFromEntityDef( const idDict *args );
	virtual idRenderModel *		ANIM_GetModelFromName( const char *modelName );
	virtual const idMD5Anim *	ANIM_GetAnimFromEntityDef( const char *classname, const char *animname );
	virtual const idMD5Anim *	ANIM_GetAnimFromEntity( const idEntity *ent, int animNum );
	virtual float				ANIM_GetAnimPlaybackRateFromEntity( idEntity *ent, int animNum );
	virtual const char *		ANIM_GetAnimNameFromEntity( const idEntity *ent, int animNum );
	virtual int					ANIM_GetNumAnimsFromEntityDef( const idDict *args );
	virtual const char *		ANIM_GetAnimNameFromEntityDef( const idDict *args, int animNum );
	virtual const idMD5Anim *	ANIM_GetAnim( const char *fileName );
	virtual int					ANIM_GetLength( const idMD5Anim *anim );
	virtual int					ANIM_GetNumFrames( const idMD5Anim *anim );
	virtual const char *		ANIM_GetFilename( const idMD5Anim *anim );
	virtual int					ANIM_ConvertFrameToTime( const idMD5Anim *anim, int frame );
	virtual int					ANIM_ConvertTimeToFrame( const idMD5Anim *anim, int time );
	virtual void				ANIM_CreateAnimFrame( const idRenderModel *model, const idMD5Anim *anim, int numJoints, idJointMat *frame, int time, const idVec3 &offset, bool remove_origin_offset );
	virtual idRenderModel *		ANIM_CreateMeshForAnim( idRenderModel *model, const char *classname, const char *animname, int frame, bool remove_origin_offset );
	virtual void				FlushUnusedAnims( void );

	// articulated figures
	virtual bool				AF_SpawnEntity( const char *fileName );
	virtual void				AF_UpdateEntities( const char *fileName );
	virtual void				AF_UndoChanges( void );
	virtual idRenderModel *		AF_CreateMesh( const idDict &args, idVec3 &meshOrigin, idMat3 &meshAxis, bool &poseIsSet );

	// selection, entity defs and spawning
	virtual void				ClearEntitySelection( void );
	virtual int					GetSelectedEntities( idEntity *list[], int max );
	virtual void				AddSelectedEntity( idEntity *ent );
	virtual void				TriggerSelected( void );
	virtual const idDict *		FindEntityDefDict( const char *name, bool makeDefault = true ) const;
	virtual void				SpawnEntityDef( const idDict &args, idEntity **ent );
	virtual idEntity *			FindEntity( const char *name ) const;
	virtual const char *		GetUniqueEntityName( const char *classname ) const;

	// entities
	virtual void				EntityGetOrigin( idEntity *ent, idVec3 &org ) const;
	virtual void				EntityGetAxis( idEntity *ent, idMat3 &axis ) const;
	virtual void				EntitySetOrigin( idEntity *ent, const idVec3 &org );
	virtual void				EntitySetAxis( idEntity *ent, const idMat3 &axis );
	virtual void				EntityTranslate( idEntity *ent, const idVec3 &org );
	virtual const idDict *		EntityGetSpawnArgs( const idEntity *ent ) const;
	virtual void				EntityUpdateChangeableSpawnArgs( idEntity *ent, const idDict *dict );
	virtual void				EntityChangeSpawnArgs( idEntity *ent, const idDict *newArgs );
	virtual void				EntityUpdateVisuals( idEntity *ent );
	virtual void				EntitySetModel( idEntity *ent, const char *val );
	virtual void				EntityStopSound( idEntity *ent );
	virtual void				EntityDelete( idEntity *ent );
	virtual void				EntitySetColor( idEntity *ent, const idVec3 color );
	virtual const char *		EntityGetName( idEntity *ent ) const;
	virtual int					EntityToSafeId( idEntity *ent ) const;
	virtual idEntity *			EntityFromSafeId( int safeID ) const;
	virtual void				EntitySetSkin( idEntity *ent, const char *temp ) const;
	virtual void				EntityClearSkin( idEntity *ent ) const;
	virtual void				EntityShow( idEntity *ent ) const;
	virtual void				EntityHide( idEntity *ent ) const;
	virtual void				EntityGetBounds( idEntity *ent, idBounds &bounds ) const;
	virtual int					EntityPlayAnim( idEntity *ent, int animNum, int time, int blendtime );
	virtual void				EntitySetFrame( idEntity *ent, int animNum, int frame, int time, int blendtime );
	virtual void				EntityStopAllEffects( idEntity *ent );
	virtual void				EntityGetDelta( idEntity *ent, int fromTime, int toTime, idVec3 &delta );
	virtual void				EntityRemoveOriginOffset( idEntity *ent, bool remove );
	virtual const char *		EntityGetClassname( idEntity *ent ) const;
	virtual bool				EntityIsDerivedFrom( idEntity *ent, const char *classname ) const;
	virtual renderEntityQ4_t *	EntityGetRenderEntity( idEntity *ent );
	virtual idEntity *			EntityGetNextTeamEntity( idEntity *pEnt ) const;
	virtual void				EntitySetName( idEntity *pEnt, const char *psName );

	// the player
	virtual void				GetPlayerInfo( idVec3 &v3Origin, idMat3 &mat3Axis, int PlayerNum = -1, idAngles *deltaViewAngles = NULL ) const;
	virtual void				SetPlayerInfo( idVec3 &v3Origin, idMat3 &mat3Axis, int PlayerNum = -1 ) const;
	virtual bool				PlayerIsValid( void ) const;
	virtual void				PlayerGetOrigin( idVec3 &org ) const;
	virtual void				PlayerGetAxis( idMat3 &axis ) const;
	virtual void				PlayerGetViewAngles( idAngles &angles ) const;
	virtual void				PlayerGetEyePosition( idVec3 &org ) const;
	virtual bool				PlayerTraceFromEye( traceQ4_t &results, float length, int contentMask );

	// effects, lights, time and the camera
	virtual void				EffectRefreshTemplate( const idDecl *effect ) const;
	virtual void				LightSetParms( idEntity *ent, int maxLevel, int currentLevel, float radius );
	virtual int					GetGameTime( int *previous = NULL ) const;
	virtual void				SetGameTime( int time ) const;
	virtual bool				TracePoint( traceQ4_t &results, const idVec3 &start, const idVec3 &end, int contentMask ) const;
	virtual void				CacheDictionaryMedia( const idDict *dict ) const;
	virtual void				SetCamera( idEntity *camera ) const;
	virtual int					GetGameEntityRegisterTime( void ) const;
	virtual idEntity *			GetFirstSpawnedEntity( void ) const;
	virtual idEntity *			GetNextSpawnedEntity( idEntity *from ) const;

	// recorded playbacks
	virtual void				DrawPlaybackDebugInfo( void );
	virtual void				RecordPlayback( const usercmdQ4_t &cmd, idEntity *source );
	virtual bool				PlayPlayback( void );
	virtual void				ShutdownPlaybacks( void );

	// the script debugger
	virtual int					ScriptGetStatementLineNumber( idProgram *program, int instructionPointer ) const;
	virtual const char *		ScriptGetStatementFileName( idProgram *program, int instructionPointer ) const;
	virtual int					ScriptGetStatementOperator( idProgram *program, int instructionPointer ) const;
	virtual void *				ScriptGetCurrentFunction( idInterpreter *interpreter ) const;
	virtual const char *		ScriptGetCurrentFunctionName( idInterpreter *interpreter ) const;
	virtual int					ScriptGetCallstackDepth( idInterpreter *interpreter ) const;
	virtual void *				ScriptGetCallstackFunction( idInterpreter *interpreter, int depth ) const;
	virtual const char *		ScriptGetCallstackFunctionName( idInterpreter *interpreter, int depth ) const;
	virtual int					ScriptGetCallstackStatement( idInterpreter *interpreter, int depth ) const;
	virtual bool				ScriptIsReturnOperator( int op ) const;
	virtual const char *		ScriptGetRegisterValue( idInterpreter *interpreter, const char *varname, int callstackDepth ) const;
	virtual idThread *			ScriptGetThread( idInterpreter *interpreter ) const;
	virtual int					ThreadGetCount( void );
	virtual idThread *			ThreadGetThread( int index );
	virtual const char *		ThreadGetName( idThread *thread );
	virtual int					ThreadGetNumber( idThread *thread );
	virtual const char *		ThreadGetState( idThread *thread );
	virtual void				GetClassDebugInfo( const idEntity *entity, debugInfoProc_t proc, void *userdata );

	// the map, in-game editing and reports
	virtual const idDict *		MapGetEntityDict( const char *name ) const;
	virtual void				MapSave( const char *path = NULL ) const;
	virtual bool				MapHasExportEntities( void ) const;
	virtual const char *		MapLoaded( void ) const;
	virtual void				MapSetEntityKeyVal( const char *name, const char *key, const char *val ) const;
	virtual void				MapCopyDictToEntity( const char *name, const idDict *dict ) const;
	virtual int					MapGetUniqueMatchingKeyVals( const char *key, const char *list[], const int max ) const;
	virtual void				MapAddEntity( const idDict *dict ) const;
	virtual int					MapGetEntitiesMatchingClassWithString( const char *classname, const char *match, const char *list[], const int max ) const;
	virtual void				MapRemoveEntity( const char *name ) const;
	virtual void				MapEntityTranslate( const char *name, const idVec3 &v ) const;
	virtual idAASFile *			GetAASFile( int i );
	virtual void				PrintMemInfo( MemInfo *mi );
	virtual size_t				ScriptSummary( const idCmdArgs &args ) const;
	virtual size_t				ClassSummary( const idCmdArgs &args ) const;
	virtual size_t				EntitySummary( const idCmdArgs &args ) const;
};

/*
===============================================================================

	The Quake 4 game API

===============================================================================
*/

const int GAME_API_VERSION_Q4		= 1;		// DUDE's own numbering: this interface is DUDE's

struct gameImportQ4_t {
	int							version;				// GAME_API_VERSION_Q4
	idSys *						sys;
	idCommon *					common;
	idCmdSystem *				cmdSystem;
	idCVarSystem *				cvarSystem;
	idFileSystem *				fileSystem;
	idNetworkSystem *			networkSystem;
	idRenderSystem *			renderSystem;
	idSoundSystemQ4 *			soundSystemQ4;			// the game's soundSystem (named so for GameNamesQ4.h)
	idRenderModelManager *		renderModelManager;
	idUserInterfaceManager *	uiManager;
	idDeclManager *				declManager;
	idAASFileManager *			AASFileManager;
	idCollisionModelManagerQ4 *	collisionModelManagerQ4;	// the game's collisionModelManager
	idEffectManagerQ4 *			bse;
};

struct gameExportQ4_t {
	int							version;				// GAME_API_VERSION_Q4
	idGameQ4 *					game;
	idGameEditQ4 *				gameEdit;
	rvGameLog *					gameLog;
};

extern "C" {
typedef gameExportQ4_t * ( *GetGameAPIQ4_t )( gameImportQ4_t *import );
}

#endif /* !__GAMEQ4_H__ */
