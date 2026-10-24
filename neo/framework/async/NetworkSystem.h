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

#ifndef __NETWORKSYSTEM_H__
#define __NETWORKSYSTEM_H__

#include "idlib/BitMsg.h"
#include "sys/sys_public.h"

// Quake 4's server browser (idNetworkSystem::GetScannedServerInfo, AddSortFunction)
typedef struct {
	netadr_t				adr;
	idDict					serverInfo;
	int						ping;
	int						clients;
	int						OSMask;
	bool					favorite;				// marked by the player
	bool					dedicated;
	bool					performanceFiltered;	// too heavy for this machine
} scannedServer_t;

typedef enum {
	SC_NONE = -1,
	SC_FAVORITE,
	SC_LOCKED,
	SC_DEDICATED,
	SC_PB,
	SC_NAME,
	SC_PING,
	SC_REPEATER,
	SC_PLAYERS,
	SC_GAMETYPE,
	SC_MAP,
	SC_ALL,
	NUM_SC
} sortColumn_t;

typedef struct {
	sortColumn_t			column;
	idList<int>::cmp_t *	compareFn;
	idList<int>::filter_t *	filterFn;
	const char *			description;
} sortInfo_t;

/*
===============================================================================

  Network System.

===============================================================================
*/

class idNetworkSystem {
public:
	virtual					~idNetworkSystem( void ) {}

	virtual void			ServerSendReliableMessage( int clientNum, const idBitMsg &msg );
	virtual void			ServerSendReliableMessageExcluding( int clientNum, const idBitMsg &msg );
	virtual int				ServerGetClientPing( int clientNum );
	virtual int				ServerGetClientPrediction( int clientNum );
	virtual int				ServerGetClientTimeSinceLastPacket( int clientNum );
	virtual int				ServerGetClientTimeSinceLastInput( int clientNum );
	virtual int				ServerGetClientOutgoingRate( int clientNum );
	virtual int				ServerGetClientIncomingRate( int clientNum );
	virtual float			ServerGetClientIncomingPacketLoss( int clientNum );

	virtual void			ClientSendReliableMessage( const idBitMsg &msg );
	virtual int				ClientGetPrediction( void );
	virtual int				ClientGetTimeSinceLastPacket( void );
	virtual int				ClientGetOutgoingRate( void );
	virtual int				ClientGetIncomingRate( void );
	virtual float			ClientGetIncomingPacketLoss( void );

	// ---- Quake 4 game library (appended; a dhewm3 game library's vtable is unchanged) -----
	// Its multiplayer, server browser and multiplayer loading screen; multiplayer is a later
	// phase (docs/quake4-port.md Q9), so for now they report no servers, friends or clients.
	virtual int				ServerGetClientNum( int clientId );
	virtual int				ServerGetServerTime( void );
	virtual const char *	GetServerAddress( void );
	virtual void			AddFriend( int clientNum );
	virtual void			RemoveFriend( int clientNum );
	virtual const char *	GetClientGUID( int clientNum );
	virtual void			SetLoadingText( const char *loadingText );
	virtual void			AddLoadingIcon( const char *icon );
	virtual int				GetNumScannedServers( void );
	virtual const scannedServer_t *GetScannedServerInfo( int serverNum );
	virtual void			AddSortFunction( const sortInfo_t &sortInfo );
	virtual bool			RemoveSortFunction( const sortInfo_t &sortInfo );
	virtual void			UseSortFunction( const sortInfo_t &sortInfo, bool use = true );
};

extern idNetworkSystem *	networkSystem;

#endif /* !__NETWORKSYSTEM_H__ */
