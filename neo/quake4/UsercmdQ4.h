/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: the Quake 4 game library's user commands (docs/quake4-port.md 5.6). Quake 4 widened the
// button bits to 16, added buttons and impulses past Doom 3's, and stamps each command with the
// real time as well as the game time - so its command is its own type, and the constants both
// games share come from Doom 3's header. The values are Quake 4's: its scripts and bindings
// name them.

#ifndef __USERCMDQ4_H__
#define __USERCMDQ4_H__

#include "framework/UsercmdGen.h"

// usercmdQ4_t::buttons past Doom 3's five
const int BUTTON_INGAMESTATS	= BIT(5);		// multiplayer statistics
const int BUTTON_VOICECHAT		= BIT(6);
const int BUTTON_TOURNEY		= BIT(7);		// tourney display
const int BUTTON_STRAFE			= BIT(8);

// usercmdQ4_t::impulse values past Doom 3's
const int IMPULSE_50			= 50;			// flashlight
const int IMPULSE_51			= 51;			// last weapon
const int IMPULSE_52			= 52;			// multiplayer statistics
const int IMPULSE_100			= 100;			// 100-127: the multiplayer buy menu
const int IMPULSE_101			= 101;
const int IMPULSE_102			= 102;
const int IMPULSE_103			= 103;
const int IMPULSE_104			= 104;
const int IMPULSE_105			= 105;
const int IMPULSE_106			= 106;
const int IMPULSE_107			= 107;
const int IMPULSE_108			= 108;
const int IMPULSE_109			= 109;
const int IMPULSE_110			= 110;
const int IMPULSE_111			= 111;
const int IMPULSE_112			= 112;
const int IMPULSE_113			= 113;
const int IMPULSE_114			= 114;
const int IMPULSE_115			= 115;
const int IMPULSE_116			= 116;
const int IMPULSE_117			= 117;
const int IMPULSE_118			= 118;
const int IMPULSE_119			= 119;
const int IMPULSE_120			= 120;
const int IMPULSE_121			= 121;
const int IMPULSE_122			= 122;
const int IMPULSE_123			= 123;
const int IMPULSE_124			= 124;
const int IMPULSE_125			= 125;
const int IMPULSE_126			= 126;
const int IMPULSE_127			= 127;

class usercmdQ4_t {
public:
	int			gameFrame;						// frame number
	int			gameTime;						// game time
	int			realTime;						// real time
	int			duplicateCount;					// duplication count for networking
	short		buttons;						// BUTTON_*
	signed char	forwardmove;					// forward/backward movement
	signed char	rightmove;						// left/right movement
	signed char	upmove;							// up/down movement
	short		angles[3];						// view angles
	short		mx;								// mouse delta x
	short		my;								// mouse delta y
	signed char	impulse;						// IMPULSE_*
	byte		flags;							// UCF_*
	int			sequence;						// for debugging

	void		ByteSwap( void ) {
					buttons = LittleShort( buttons );
					angles[0] = LittleShort( angles[0] );
					angles[1] = LittleShort( angles[1] );
					angles[2] = LittleShort( angles[2] );
					mx = LittleShort( mx );
					my = LittleShort( my );
					sequence = LittleInt( sequence );
				}
	bool		operator==( const usercmdQ4_t &rhs ) const {
					return buttons == rhs.buttons && forwardmove == rhs.forwardmove && rightmove == rhs.rightmove &&
						upmove == rhs.upmove && angles[0] == rhs.angles[0] && angles[1] == rhs.angles[1] &&
						angles[2] == rhs.angles[2] && impulse == rhs.impulse && flags == rhs.flags &&
						mx == rhs.mx && my == rhs.my;
				}
};

#endif /* !__USERCMDQ4_H__ */
