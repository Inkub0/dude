/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: the null game - a spectator-only game library (nullgame.so / nullgame.dll).
//
// It implements idGame with nothing but a free-flying camera: the session loads a map's
// render world as usual, and this library renders it, flies through it with the normal
// movement keys and places the sound listener. No entities are spawned, no scripts run,
// nothing collides; what the map's entities would show when they spawn (lights, models,
// MD5 models frozen on their start animation, GUIs) is added render-only by the map
// dressing (NullGameDressing.cpp, nullgame_dressing 0 for the world alone). That is exactly
// what is needed to bring up an engine-side feature
// (file loaders, decls, materials, GUIs, effects) for a game title whose own game code
// is not built yet, which is how Quake 4 support starts (docs/quake4-port.md section 5.7).
//
// Selected automatically when the active title's game library is missing and the title
// allows it (GameTitle nullGameFallback), or explicitly with  +set fs_game nullgame.
//
// Console commands: getviewpos, setviewpos x y z [yaw [pitch]].

#include "sys/platform.h"
#include "idlib/Lib.h"
#include "idlib/MapFile.h"
#include "idlib/math/Angles.h"
#include "framework/Common.h"
#include "framework/CmdSystem.h"
#include "framework/CVarSystem.h"
#include "framework/FileSystem.h"
#include "framework/DeclManager.h"
#include "framework/Game.h"
#include "renderer/RenderSystem.h"
#include "renderer/RenderWorld.h"
#include "sound/sound.h"
#include "nullgame/NullGameDressing.h"

// interface pointers handed over by the engine in GetGameAPI
idSys *						sys = NULL;
idCommon *					common = NULL;
idCmdSystem *				cmdSystem = NULL;
idCVarSystem *				cvarSystem = NULL;
idFileSystem *				fileSystem = NULL;
idNetworkSystem *			networkSystem = NULL;
idRenderSystem *			renderSystem = NULL;
idSoundSystem *				soundSystem = NULL;
idRenderModelManager *		renderModelManager = NULL;
idUserInterfaceManager *	uiManager = NULL;
idDeclManager *				declManager = NULL;
idAASFileManager *			AASFileManager = NULL;
idCollisionModelManager *	collisionModelManager = NULL;
idCVar *					idCVar::staticVars = NULL;

static const float	NULLGAME_FLY_SPEED		= 320.0f;	// units per second, doubled while running
static const float	NULLGAME_BASE_FOV		= 90.0f;	// horizontal at 4:3, widened for the screen aspect
static const int	NULLGAME_LISTENER_ID	= 1;

/*
===============================================================================

	idNullGame

===============================================================================
*/

class idNullGame : public idGame {
public:
						idNullGame( void );

	virtual void				Init( void );
	virtual void				Shutdown( void );
	virtual void				SetLocalClient( int clientNum );
	virtual const idDict *		SetUserInfo( int clientNum, const idDict &userInfo, bool isClient, bool canModify );
	virtual const idDict *		GetUserInfo( int clientNum );
	virtual void				ThrottleUserInfo( void ) {}
	virtual void				SetServerInfo( const idDict &serverInfo );
	virtual const idDict &		GetPersistentPlayerInfo( int clientNum );
	virtual void				SetPersistentPlayerInfo( int clientNum, const idDict &playerInfo );
	virtual void				InitFromNewMap( const char *mapName, idRenderWorld *renderWorld, idSoundWorld *soundWorld, bool isServer, bool isClient, int randseed );
	virtual bool				InitFromSaveGame( const char *mapName, idRenderWorld *renderWorld, idSoundWorld *soundWorld, idFile *saveGameFile );
	virtual void				SaveGame( idFile *saveGameFile );
	virtual void				MapShutdown( void );
	virtual void				CacheDictionaryMedia( const idDict *dict ) {}
	virtual void				SpawnPlayer( int clientNum ) {}
	virtual gameReturn_t		RunFrame( const usercmd_t *clientCmds );
	virtual bool				Draw( int clientNum );
	virtual escReply_t			HandleESC( idUserInterface **gui ) { return ESC_MAIN; }
	virtual idUserInterface *	StartMenu( void ) { return NULL; }
	virtual const char *		HandleGuiCommands( const char *menuCommand ) { return NULL; }
	virtual void				HandleMainMenuCommands( const char *menuCommand, idUserInterface *gui ) {}
	virtual allowReply_t		ServerAllowClient( int numClients, const char *IP, const char *guid, const char *password, char reason[MAX_STRING_CHARS] );
	virtual void				ServerClientConnect( int clientNum, const char *guid ) {}
	virtual void				ServerClientBegin( int clientNum ) {}
	virtual void				ServerClientDisconnect( int clientNum ) {}
	virtual void				ServerWriteInitialReliableMessages( int clientNum ) {}
	virtual void				ServerWriteSnapshot( int clientNum, int sequence, idBitMsg &msg, byte *clientInPVS, int numPVSClients ) {}
	virtual bool				ServerApplySnapshot( int clientNum, int sequence ) { return false; }
	virtual void				ServerProcessReliableMessage( int clientNum, const idBitMsg &msg ) {}
	virtual void				ClientReadSnapshot( int clientNum, int sequence, const int gameFrame, const int gameTime, const int dupeUsercmds, const int aheadOfServer, const idBitMsg &msg ) {}
	virtual bool				ClientApplySnapshot( int clientNum, int sequence ) { return false; }
	virtual void				ClientProcessReliableMessage( int clientNum, const idBitMsg &msg ) {}
	virtual gameReturn_t		ClientPrediction( int clientNum, const usercmd_t *clientCmds, bool lastPredictFrame );
	virtual void				SelectTimeGroup( int timeGroup ) {}
	virtual int					GetTimeGroupTime( int timeGroup ) { return gameTime; }
	virtual void				GetBestGameType( const char* map, const char* gametype, char buf[ MAX_STRING_CHARS ] );
	virtual void				GetClientStats( int clientNum, char *data, const int len );
	virtual void				SwitchTeam( int clientNum, int team ) {}
	virtual bool				DownloadRequest( const char *IP, const char *guid, const char *paks, char urls[ MAX_STRING_CHARS ] ) { return false; }
	virtual void				GetMapLoadingGUI( char gui[ MAX_STRING_CHARS ] ) {}	// keep the session's choice

	void						GetViewPos( idVec3 &origin, idAngles &angles ) const;
	void						SetViewPos( const idVec3 &origin, const idAngles &angles );

private:
	gameReturn_t				EmptyReturn( void ) const;
	void						FindPlayerStart( const idMapFile *mapFile, const char *mapName );
	idMat3						ViewAxis( void ) const;

	idRenderWorld *				renderWorld;
	idSoundWorld *				soundWorld;
	idDict						userInfo[MAX_ASYNC_CLIENTS];
	idDict						persistentPlayerInfo[MAX_ASYNC_CLIENTS];
	idDict						serverInfo;
	int							localClient;
	int							gameTime;
	idVec3						viewOrigin;
	idAngles					viewAngles;			// what is drawn
	idAngles					deltaViewAngles;	// view = usercmd angles + delta, so the start yaw sticks
	short						lastCmdAngles[3];
};

static idNullGame		nullGame;
static gameExport_t		gameExport;

/*
================
idNullGame::idNullGame
================
*/
idNullGame::idNullGame( void ) {
	renderWorld = NULL;
	soundWorld = NULL;
	localClient = 0;
	gameTime = 0;
	viewOrigin.Zero();
	viewAngles.Zero();
	deltaViewAngles.Zero();
	lastCmdAngles[0] = lastCmdAngles[1] = lastCmdAngles[2] = 0;
}

/*
================
NullGame_GetViewPos_f / NullGame_SetViewPos_f
================
*/
static void NullGame_GetViewPos_f( const idCmdArgs &args ) {
	idVec3 origin;
	idAngles angles;
	nullGame.GetViewPos( origin, angles );
	common->Printf( "(%s) %.1f %.1f\n", origin.ToString( 1 ), angles.yaw, angles.pitch );
}

static void NullGame_SetViewPos_f( const idCmdArgs &args ) {
	if ( args.Argc() != 4 && args.Argc() != 5 && args.Argc() != 6 ) {
		common->Printf( "usage: setviewpos <x> <y> <z> [yaw [pitch]]\n" );
		return;
	}
	idVec3 origin;
	idAngles angles;
	nullGame.GetViewPos( origin, angles );
	for ( int i = 0; i < 3; i++ ) {
		origin[i] = atof( args.Argv( i + 1 ) );
	}
	if ( args.Argc() >= 5 ) {
		angles.yaw = atof( args.Argv( 4 ) );
	}
	if ( args.Argc() == 6 ) {
		angles.pitch = atof( args.Argv( 5 ) );
	}
	nullGame.SetViewPos( origin, angles );
}

/*
================
idNullGame::Init
================
*/
void idNullGame::Init( void ) {
	idLib::Init();
	idCVar::RegisterStaticVars();

	common->Printf( "----- Initializing Null Game -----\n" );
	// a game library cannot call engine free functions (GameTitle::Get would leave an
	// undefined symbol and fail dlopen) - the resolved title is in the com_title cvar
	common->Printf( "spectator only: maps render with their lights and models, nothing moves or simulates (com_title %s)\n",
		cvarSystem->GetCVarString( "com_title" ) );

	// The decl types and folders a game library registers, so the engine's decl tools
	// (lintDecls, listEntityDefs ...) cover the title's game decls too. Model, export and
	// camera decls are kept as text: the map dressing reads the model defs it needs itself.
	const bool quake4 = idStr::Icmp( cvarSystem->GetCVarString( "com_title" ), "quake4" ) == 0;
	declManager->RegisterDeclType( "model",		DECL_MODELDEF,		idDeclAllocator<idDecl> );
	declManager->RegisterDeclType( "export",	DECL_MODELEXPORT,	idDeclAllocator<idDecl> );
	if ( quake4 ) {
		declManager->RegisterDeclType( "camera",	DECL_CAMERADEF,		idDeclAllocator<idDecl> );
	}
	declManager->RegisterDeclFolder( "def",		".def",		DECL_ENTITYDEF );
	declManager->RegisterDeclFolder( "af",		".af",		DECL_AF );
	if ( !quake4 ) {		// Doom 3's game-side effect decls (Quake 4's are the engine's BSE effects)
		declManager->RegisterDeclFolder( "fx",			".fx",		DECL_FX );
		declManager->RegisterDeclFolder( "particles",	".prt",		DECL_PARTICLE );
		declManager->RegisterDeclFolder( "newpdas",		".pda",		DECL_PDA );
	}

	cmdSystem->AddCommand( "getviewpos", NullGame_GetViewPos_f, CMD_FL_GAME, "prints the null game camera position and angles" );
	cmdSystem->AddCommand( "setviewpos", NullGame_SetViewPos_f, CMD_FL_GAME, "moves the null game camera: setviewpos x y z [yaw [pitch]]" );
}

/*
================
idNullGame::Shutdown
================
*/
void idNullGame::Shutdown( void ) {
	MapShutdown();
	cmdSystem->RemoveFlaggedCommands( CMD_FL_GAME );
	idLib::ShutDown();
}

/*
================
idNullGame::SetLocalClient
================
*/
void idNullGame::SetLocalClient( int clientNum ) {
	localClient = clientNum;
}

/*
================
idNullGame::SetUserInfo / GetUserInfo / SetServerInfo / persistent info
================
*/
const idDict *idNullGame::SetUserInfo( int clientNum, const idDict &info, bool isClient, bool canModify ) {
	if ( clientNum < 0 || clientNum >= MAX_ASYNC_CLIENTS ) {
		return NULL;
	}
	userInfo[clientNum] = info;
	return NULL;	// NULL: the game did not modify the info
}

const idDict *idNullGame::GetUserInfo( int clientNum ) {
	if ( clientNum < 0 || clientNum >= MAX_ASYNC_CLIENTS ) {
		return NULL;
	}
	return &userInfo[clientNum];
}

void idNullGame::SetServerInfo( const idDict &info ) {
	serverInfo = info;
}

const idDict &idNullGame::GetPersistentPlayerInfo( int clientNum ) {
	static idDict empty;
	if ( clientNum < 0 || clientNum >= MAX_ASYNC_CLIENTS ) {
		return empty;
	}
	// the session refuses to save a "dead" player; the spectator is always alive
	persistentPlayerInfo[clientNum].SetInt( "health", 100 );
	return persistentPlayerInfo[clientNum];
}

void idNullGame::SetPersistentPlayerInfo( int clientNum, const idDict &info ) {
	if ( clientNum >= 0 && clientNum < MAX_ASYNC_CLIENTS ) {
		persistentPlayerInfo[clientNum] = info;
	}
}

/*
================
idNullGame::FindPlayerStart

Where to put the camera: the first info_player_start (or, failing that, any player
start) of the parsed .map, at the eye height of a standing player. mapFile is NULL when
the .map did not parse.
================
*/
void idNullGame::FindPlayerStart( const idMapFile *mapFile, const char *mapName ) {
	viewOrigin.Zero();
	idAngles start( 0.0f, 0.0f, 0.0f );

	if ( mapFile == NULL ) {
		common->Warning( "null game: couldn't parse %s - camera starts at the origin", mapName );
	} else {
		const idMapEntity *best = NULL;
		for ( int i = 0; i < mapFile->GetNumEntities() && best == NULL; i++ ) {
			const idMapEntity *ent = mapFile->GetEntity( i );
			if ( idStr::Icmp( ent->epairs.GetString( "classname" ), "info_player_start" ) == 0 ) {
				best = ent;
			}
		}
		for ( int i = 0; i < mapFile->GetNumEntities() && best == NULL; i++ ) {
			const idMapEntity *ent = mapFile->GetEntity( i );
			if ( idStr::Icmpn( ent->epairs.GetString( "classname" ), "info_player_", 12 ) == 0 ) {
				best = ent;
			}
		}
		if ( best != NULL ) {
			viewOrigin = best->epairs.GetVector( "origin" ) + idVec3( 0.0f, 0.0f, 68.0f );
			start.yaw = best->epairs.GetFloat( "angle" );
			common->Printf( "null game: camera at %s (%s)\n", viewOrigin.ToString( 0 ), best->epairs.GetString( "classname" ) );
		} else {
			common->Printf( "null game: %s has no player start - camera starts at the origin\n", mapName );
		}
	}

	viewAngles = start;
	// the usercmd angles were reset for the new map; keep the start yaw on top of them
	deltaViewAngles = start;
	lastCmdAngles[0] = lastCmdAngles[1] = lastCmdAngles[2] = 0;
}

/*
================
idNullGame::InitFromNewMap
================
*/
void idNullGame::InitFromNewMap( const char *mapName, idRenderWorld *rw, idSoundWorld *sw, bool isServer, bool isClient, int randseed ) {
	// the session has already loaded the new map into rw, which dropped anything added to
	// it before: forget the old dressing's handles without freeing them
	NullGameDressing::Clear( NULL );
	renderWorld = rw;
	soundWorld = sw;
	gameTime = 0;

	idMapFile mapFile;
	const bool parsed = mapFile.Parse( mapName );
	FindPlayerStart( parsed ? &mapFile : NULL, mapName );
	if ( parsed ) {
		NullGameDressing::Spawn( mapFile, renderWorld, gameTime );
	}
}

/*
================
idNullGame::InitFromSaveGame / SaveGame
================
*/
bool idNullGame::InitFromSaveGame( const char *mapName, idRenderWorld *rw, idSoundWorld *sw, idFile *saveGameFile ) {
	common->Warning( "the null game has no save games" );
	return false;
}

void idNullGame::SaveGame( idFile *saveGameFile ) {
	common->Warning( "the null game has no save games; nothing was saved" );
}

/*
================
idNullGame::MapShutdown
================
*/
void idNullGame::MapShutdown( void ) {
	NullGameDressing::Clear( renderWorld );		// the render world still holds the dressing here
	renderWorld = NULL;
	soundWorld = NULL;
}

/*
================
idNullGame::EmptyReturn
================
*/
gameReturn_t idNullGame::EmptyReturn( void ) const {
	gameReturn_t ret;
	memset( &ret, 0, sizeof( ret ) );
	ret.health = 100;
	return ret;
}

/*
================
idNullGame::ViewAxis
================
*/
idMat3 idNullGame::ViewAxis( void ) const {
	return viewAngles.ToMat3();
}

/*
================
idNullGame::RunFrame

One 60 Hz tic of free flight. Forward follows the full view direction (pitch included),
strafe stays horizontal, up/down is world up.
================
*/
gameReturn_t idNullGame::RunFrame( const usercmd_t *clientCmds ) {
	gameTime += USERCMD_MSEC;

	if ( clientCmds != NULL && localClient >= 0 && localClient < MAX_ASYNC_CLIENTS ) {
		const usercmd_t &cmd = clientCmds[localClient];

		for ( int i = 0; i < 3; i++ ) {
			lastCmdAngles[i] = cmd.angles[i];
		}
		viewAngles.pitch	= SHORT2ANGLE( cmd.angles[PITCH] ) + deltaViewAngles.pitch;
		viewAngles.yaw		= SHORT2ANGLE( cmd.angles[YAW] ) + deltaViewAngles.yaw;
		viewAngles.roll		= 0.0f;
		viewAngles.pitch	= idMath::ClampFloat( -89.0f, 89.0f, viewAngles.pitch );
		viewAngles.Normalize360();

		idVec3 forward, right;
		viewAngles.ToVectors( &forward, &right, NULL );
		idVec3 flatRight = right;
		flatRight.z = 0.0f;
		flatRight.Normalize();

		float speed = NULLGAME_FLY_SPEED * ( ( cmd.buttons & BUTTON_RUN ) ? 2.0f : 1.0f );
		float dt = USERCMD_MSEC * 0.001f;
		idVec3 move = forward * ( cmd.forwardmove / 127.0f )
					+ flatRight * ( cmd.rightmove / 127.0f )
					+ idVec3( 0.0f, 0.0f, 1.0f ) * ( cmd.upmove / 127.0f );
		viewOrigin += move * speed * dt;
	}

	if ( soundWorld != NULL ) {
		soundWorld->PlaceListener( viewOrigin, ViewAxis(), NULLGAME_LISTENER_ID, gameTime, "" );
	}

	return EmptyReturn();
}

/*
================
idNullGame::ClientPrediction
================
*/
gameReturn_t idNullGame::ClientPrediction( int clientNum, const usercmd_t *clientCmds, bool lastPredictFrame ) {
	return EmptyReturn();
}

/*
================
idNullGame::Draw
================
*/
bool idNullGame::Draw( int clientNum ) {
	if ( renderWorld == NULL ) {
		return false;
	}

	renderView_t view;
	memset( &view, 0, sizeof( view ) );
	view.viewID = clientNum + 1;	// non-zero: a player view
	view.x = 0;
	view.y = 0;
	view.width = SCREEN_WIDTH;
	view.height = SCREEN_HEIGHT;

	// the base game's hor+ rule: the vertical fov of a 90 degree 4:3 view, widened to the screen
	float fovY = RAD2DEG( 2.0f * idMath::ATan( idMath::Tan( DEG2RAD( NULLGAME_BASE_FOV * 0.5f ) ) * ( 3.0f / 4.0f ) ) );
	float aspect = 4.0f / 3.0f;
	if ( renderSystem->GetScreenWidth() > 0 && renderSystem->GetScreenHeight() > 0 ) {
		aspect = (float)renderSystem->GetScreenWidth() / (float)renderSystem->GetScreenHeight();
	}
	view.fov_y = fovY;
	view.fov_x = RAD2DEG( 2.0f * idMath::ATan( idMath::Tan( DEG2RAD( fovY * 0.5f ) ) * aspect ) );

	view.vieworg = viewOrigin;
	view.viewaxis = ViewAxis();
	view.time = gameTime;
	view.globalMaterial = NULL;

	renderWorld->RenderScene( &view );
	return true;
}

/*
================
idNullGame::ServerAllowClient / GetBestGameType / GetClientStats
================
*/
allowReply_t idNullGame::ServerAllowClient( int numClients, const char *IP, const char *guid, const char *password, char reason[MAX_STRING_CHARS] ) {
	reason[0] = '\0';
	return ALLOW_YES;
}

void idNullGame::GetBestGameType( const char* map, const char* gametype, char buf[ MAX_STRING_CHARS ] ) {
	idStr::Copynz( buf, gametype, MAX_STRING_CHARS );
}

void idNullGame::GetClientStats( int clientNum, char *data, const int len ) {
	if ( len > 0 ) {
		data[0] = '\0';
	}
}

/*
================
idNullGame::GetViewPos / SetViewPos
================
*/
void idNullGame::GetViewPos( idVec3 &origin, idAngles &angles ) const {
	origin = viewOrigin;
	angles = viewAngles;
}

void idNullGame::SetViewPos( const idVec3 &origin, const idAngles &angles ) {
	viewOrigin = origin;
	// re-base the delta so the next usercmd keeps this orientation
	deltaViewAngles.pitch	= angles.pitch - SHORT2ANGLE( lastCmdAngles[PITCH] );
	deltaViewAngles.yaw		= angles.yaw - SHORT2ANGLE( lastCmdAngles[YAW] );
	deltaViewAngles.roll	= 0.0f;
	viewAngles = angles;
}

/*
===============================================================================

	idGameEdit

	The engine calls a few of these outside the editors (GUI renderDef windows, the
	worldspawn lookup at map load), so they must exist; with no entities and no
	animation system every query answers "nothing".

===============================================================================
*/

static idGameEdit		nullGameEdit;
idGameEdit *			gameEdit = &nullGameEdit;

void			idGameEdit::ParseSpawnArgsToRenderLight( const idDict *args, renderLight_t *renderLight ) { memset( renderLight, 0, sizeof( *renderLight ) ); }
void			idGameEdit::ParseSpawnArgsToRenderEntity( const idDict *args, renderEntity_t *renderEntity ) { memset( renderEntity, 0, sizeof( *renderEntity ) ); }
void			idGameEdit::ParseSpawnArgsToRefSound( const idDict *args, refSound_t *refSound ) { memset( refSound, 0, sizeof( *refSound ) ); }

idRenderModel *	idGameEdit::ANIM_GetModelFromEntityDef( const char *classname ) { return NULL; }
const idVec3 &	idGameEdit::ANIM_GetModelOffsetFromEntityDef( const char *classname ) { return vec3_origin; }
idRenderModel *	idGameEdit::ANIM_GetModelFromEntityDef( const idDict *args ) { return NULL; }
idRenderModel *	idGameEdit::ANIM_GetModelFromName( const char *modelName ) { return NULL; }
const idMD5Anim *idGameEdit::ANIM_GetAnimFromEntityDef( const char *classname, const char *animname ) { return NULL; }
int				idGameEdit::ANIM_GetNumAnimsFromEntityDef( const idDict *args ) { return 0; }
const char *	idGameEdit::ANIM_GetAnimNameFromEntityDef( const idDict *args, int animNum ) { return ""; }
const idMD5Anim *idGameEdit::ANIM_GetAnim( const char *fileName ) { return NULL; }
int				idGameEdit::ANIM_GetLength( const idMD5Anim *anim ) { return 0; }
int				idGameEdit::ANIM_GetNumFrames( const idMD5Anim *anim ) { return 0; }
void			idGameEdit::ANIM_CreateAnimFrame( const idRenderModel *model, const idMD5Anim *anim, int numJoints, idJointMat *frame, int time, const idVec3 &offset, bool remove_origin_offset ) {}
idRenderModel *	idGameEdit::ANIM_CreateMeshForAnim( idRenderModel *model, const char *classname, const char *animname, int frame, bool remove_origin_offset ) { return NULL; }

bool			idGameEdit::AF_SpawnEntity( const char *fileName ) { return false; }
void			idGameEdit::AF_UpdateEntities( const char *fileName ) {}
void			idGameEdit::AF_UndoChanges( void ) {}
idRenderModel *	idGameEdit::AF_CreateMesh( const idDict &args, idVec3 &meshOrigin, idMat3 &meshAxis, bool &poseIsSet ) { poseIsSet = false; return NULL; }

void			idGameEdit::ClearEntitySelection( void ) {}
int				idGameEdit::GetSelectedEntities( idEntity *list[], int max ) { return 0; }
void			idGameEdit::AddSelectedEntity( idEntity *ent ) {}
void			idGameEdit::TriggerSelected() {}

const idDict *	idGameEdit::FindEntityDefDict( const char *name, bool makeDefault ) const { return NULL; }
void			idGameEdit::SpawnEntityDef( const idDict &args, idEntity **ent ) { if ( ent != NULL ) { *ent = NULL; } }
idEntity *		idGameEdit::FindEntity( const char *name ) const { return NULL; }
const char *	idGameEdit::GetUniqueEntityName( const char *classname ) const { return ""; }

void			idGameEdit::EntityGetOrigin( idEntity *ent, idVec3 &org ) const { org.Zero(); }
void			idGameEdit::EntityGetAxis( idEntity *ent, idMat3 &axis ) const { axis.Identity(); }
void			idGameEdit::EntitySetOrigin( idEntity *ent, const idVec3 &org ) {}
void			idGameEdit::EntitySetAxis( idEntity *ent, const idMat3 &axis ) {}
void			idGameEdit::EntityTranslate( idEntity *ent, const idVec3 &org ) {}
const idDict *	idGameEdit::EntityGetSpawnArgs( idEntity *ent ) const { return NULL; }
void			idGameEdit::EntityUpdateChangeableSpawnArgs( idEntity *ent, const idDict *dict ) {}
void			idGameEdit::EntityChangeSpawnArgs( idEntity *ent, const idDict *newArgs ) {}
void			idGameEdit::EntityUpdateVisuals( idEntity *ent ) {}
void			idGameEdit::EntitySetModel( idEntity *ent, const char *val ) {}
void			idGameEdit::EntityStopSound( idEntity *ent ) {}
void			idGameEdit::EntityDelete( idEntity *ent ) {}
void			idGameEdit::EntitySetColor( idEntity *ent, const idVec3 color ) {}

bool			idGameEdit::PlayerIsValid( ) const { return false; }
void			idGameEdit::PlayerGetOrigin( idVec3 &org ) const { idAngles a; nullGame.GetViewPos( org, a ); }
void			idGameEdit::PlayerGetAxis( idMat3 &axis ) const { idVec3 o; idAngles a; nullGame.GetViewPos( o, a ); axis = a.ToMat3(); }
void			idGameEdit::PlayerGetViewAngles( idAngles &angles ) const { idVec3 o; nullGame.GetViewPos( o, angles ); }
void			idGameEdit::PlayerGetEyePosition( idVec3 &org ) const { idAngles a; nullGame.GetViewPos( org, a ); }

const idDict *	idGameEdit::MapGetEntityDict( const char *name ) const { return NULL; }
void			idGameEdit::MapSave( const char *path ) const {}
void			idGameEdit::MapSetEntityKeyVal( const char *name, const char *key, const char *val ) const {}
void			idGameEdit::MapCopyDictToEntity( const char *name, const idDict *dict ) const {}
int				idGameEdit::MapGetUniqueMatchingKeyVals( const char *key, const char *list[], const int max ) const { return 0; }
void			idGameEdit::MapAddEntity( const idDict *dict ) const {}
int				idGameEdit::MapGetEntitiesMatchingClassWithString( const char *classname, const char *match, const char *list[], const int max ) const { return 0; }
void			idGameEdit::MapRemoveEntity( const char *name ) const {}
void			idGameEdit::MapEntityTranslate( const char *name, const idVec3 &v ) const {}

/*
===========
GetGameAPI
===========
*/
extern "C" ID_GAME_API gameExport_t *GetGameAPI( gameImport_t *import ) {
	if ( import->version == GAME_API_VERSION ) {
		sys							= import->sys;
		common						= import->common;
		cmdSystem					= import->cmdSystem;
		cvarSystem					= import->cvarSystem;
		fileSystem					= import->fileSystem;
		networkSystem				= import->networkSystem;
		renderSystem				= import->renderSystem;
		soundSystem					= import->soundSystem;
		renderModelManager			= import->renderModelManager;
		uiManager					= import->uiManager;
		declManager					= import->declManager;
		AASFileManager				= import->AASFileManager;
		collisionModelManager		= import->collisionModelManager;
	}

	idLib::sys					= sys;
	idLib::common				= common;
	idLib::cvarSystem			= cvarSystem;
	idLib::fileSystem			= fileSystem;

	gameExport.version			= GAME_API_VERSION;
	gameExport.game				= &nullGame;
	gameExport.gameEdit			= gameEdit;
	return &gameExport;
}
