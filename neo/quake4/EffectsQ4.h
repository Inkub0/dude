/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: the Quake 4 game library's effect system interface - Raven's BSE as the game sees it
// (docs/quake4-port.md 5.5, 5.6). Effects are played as render world effect defs
// (quake4/RenderWorldQ4.h); the game asks the effect manager only about filtering, rate
// limits, durations and frame bracketing. Until the effect system exists (phase Q4) the
// engine's manager accepts every effect and draws none.

#ifndef __EFFECTSQ4_H__
#define __EFFECTSQ4_H__

#include "quake4/RenderWorldQ4.h"

#define BSE_EFFECT_EXTENSION		"fx"

// the full-screen view effects the game plays
enum {
	VIEWEFFECT_DOUBLEVISION = 0,
	VIEWEFFECT_SHAKE,
	VIEWEFFECT_TUNNEL
};

// what an effect is for, for filtering and rate limiting
typedef enum {
	EC_IGNORE = 0,
	EC_IMPACT,
	EC_IMPACT_PARTICLES,
	EC_MAX
} effectCategory_t;

class idEffectManagerQ4 {
public:
	virtual					~idEffectManagerQ4( void ) {}

							// true when effects of this name and category are filtered out (not played)
	virtual bool			Filtered( const char *name, effectCategory_t category ) = 0;
							// true when another effect of the category may play now
	virtual bool			CanPlayRateLimited( effectCategory_t category ) = 0;
							// an effect def's length in seconds
	virtual float			EffectDuration( const idRenderEffectQ4 *def ) = 0;
							// a game frame's effects bracket
	virtual void			StartFrame( void ) = 0;
	virtual void			EndFrame( void ) = 0;
};

#endif /* !__EFFECTSQ4_H__ */
