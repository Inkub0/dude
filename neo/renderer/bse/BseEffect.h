/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: Quake 4's effects (Raven's BSE, docs/quake4-bse.md) - the runtime. A played effect
// (idBseEffect) runs its decl's segments from their start times, spawns their particles and
// evaluates them at any time asked; the renderer draws them as frame-temporary geometry of an
// engine render entity (BseRender.cpp). Written from the behaviour docs/quake4-bse.md 3.1 lists.
//
// Engine-only: the game library reaches effects through the Quake 4 render world facade.

#ifndef __BSEEFFECT_H__
#define __BSEEFFECT_H__

#include "idlib/containers/List.h"
#include "idlib/math/Vector.h"
#include "idlib/math/Matrix.h"
#include "idlib/math/Random.h"
#include "idlib/bv/Bounds.h"
#include "idlib/geometry/DrawVert.h"
#include "framework/CVarSystem.h"
#include "renderer/RenderWorld.h"
#include "renderer/bse/DeclEffect.h"

class idRenderWorld;
struct viewEntity_s;

// Quake 4's effect cvars, registered for that title only (BSE_Init); without them the accessors
// below give the retail defaults
extern idCVar *	bse_enabled;		// play effects
extern idCVar *	bse_render;			// draw them
extern idCVar *	bse_scale;			// detail 0-1: emitter counts, segment detail levels
extern idCVar *	bse_maxParticles;	// the most particles a segment holds
extern idCVar *	bse_debug;

ID_INLINE bool	BSE_Enabled( void ) { return bse_enabled == NULL || bse_enabled->GetBool(); }
ID_INLINE bool	BSE_Render( void ) { return bse_render == NULL || bse_render->GetBool(); }
ID_INLINE float	BSE_Scale( void ) { return bse_scale != NULL ? bse_scale->GetFloat() : 1.0f; }
ID_INLINE int	BSE_MaxParticles( void ) { return bse_maxParticles != NULL ? bse_maxParticles->GetInteger() : 2048; }
ID_INLINE int	BSE_Debug( void ) { return bse_debug != NULL ? bse_debug->GetInteger() : 0; }

const float BSE_FUTURE				= 0.016f;	// emitters spawn this far past the time they are serviced at
const float BSE_TIME_EPSILON		= 0.002f;
const float BSE_MAX_LIFE			= 300.0f;	// seconds
const int	BSE_MAX_PARTICLES		= 2048;		// per segment

// what plays an effect: the Quake 4 game's renderEffect_t, or testEffect
typedef struct {
	const idDeclEffect *	effect;
	float					startTime;			// seconds
	idVec3					origin;
	idMat3					axis;				// x is the direction the effect plays along
	idVec3					gravity;			// world gravity at the effect
	idVec3					endOrigin;			// a beam's end, when hasEndOrigin
	bool					hasEndOrigin;
	float					attenuation;		// the owner's own scale for distance attenuation, 1 = none
	bool					loop;
	bool					ambient;			// placed in the map rather than played by an event
	bool					inConnectedArea;	// the owner is in an area connected to the player's
	int						suppressSurfaceInViewID;
	int						allowSurfaceInViewID;
	bool					weaponDepthHack;
	float					modelDepthHack;
	int						referenceSoundHandle;
	float					shaderParms[MAX_ENTITY_SHADER_PARMS];
} bseEffectParms_t;

/*
===============================================================================

	runtime state

===============================================================================
*/

// a live particle; times are seconds of game time, positions are in the effect's frame
typedef struct {
	float					startTime;
	float					endTime;
	float					oneOverLife;		// fixed at birth: envelopes run at the rate set then
	float					fraction;			// the spawn fraction linearSpacing domains use
	bool					locked;				// moves with the effect; otherwise stays where it was born
	idVec3					initPos;			// birth position, in the effect's frame at birth (unlocked) or now (locked)
	idVec3					velocity;
	idVec3					acceleration;
	idVec3					friction;
	idVec3					initOrigin;			// unlocked: the effect's origin and axis at birth
	idMat3					initAxis;
	bool					hasOffset;
	// the parameters' start and end values (end includes start for "relative")
	idVec3					tintStart, tintEnd;
	float					fadeStart, fadeEnd;
	idVec3					sizeStart, sizeEnd;
	idVec3					rotateStart, rotateEnd;		// radians
	idVec3					angleStart, angleEnd;		// radians
	idVec3					offsetStart, offsetEnd;
	idVec3					lengthStart, lengthEnd;
} bseParticle_t;

class idBseEffect;

// a segment as it plays
class idBseSegment {
public:
	void					Init( idBseEffect *effect, int index, float time );
	void					InitTime( idBseEffect *effect, float time );	// a new window from time on
	void					CalcBudget( idBseEffect *effect, float time );
							// spawns what is due by time; true once the segment has done its part
	bool					Check( idBseEffect *effect, float time );
							// drops the particles that are over at time; true while any remain
	bool					UpdateParticles( idBseEffect *effect, float time );
	void					Rewind( idBseEffect *effect );		// for an ambient effect's history

	const idEffectSegment *	tmpl;
	int						index;
	float					segStartTime;		// this loop's window
	float					segEndTime;
	float					lastTime;			// the emitter's spawn clock
	bool					expired;
	effectRange_t			count;				// count or density times the domain size
	float					fastInterval;		// seconds per particle at the highest and lowest count
	float					slowInterval;
	int						budget;				// most particles alive at once
	idList<bseParticle_t>	particles;			// draw order: soonest to die first (complex: newest first)

private:
	float					AttenuatedInterval( idBseEffect *effect ) const;
	float					AttenuatedCount( idBseEffect *effect ) const;
	void					Spawn( idBseEffect *effect, float birthTime, float fraction );
	void					SpawnParticle( idBseEffect *effect, bseParticle_t &p, float birthTime, float fraction );
	void					Insert( const bseParticle_t &p );
};

// a played effect
class idBseEffect {
public:
	void					Init( const bseEffectParms_t &parms, float time );
	void					UpdateFromOwner( const bseEffectParms_t &parms, float time );
							// runs the effect to time: spawns, loops, retires particles; true when it is over
	bool					Service( float time );
	void					Stop( void ) { stopped = true; }
	float					GetDuration( void ) const { return duration; }

							// the attenuation of a segment for the player's distance (0 = gone)
	float					SegmentAttenuation( const idEffectSegment &segment ) const;
							// a particle's position at time, in the effect's current frame
	idVec3					EvaluatePosition( const bseParticle_t &p, const idEffectParticle &tmpl, float time ) const;
							// a particle's state at time, in the effect's current frame; false when not alive then
	bool					EvaluateParticle( const bseParticle_t &p, const idEffectParticle &tmpl, float time,
								idVec3 &position, idVec4 &tint, idVec3 &size, idVec3 &rotate ) const;
							// a segment's sprites for a viewer with these left and up axes (effect frame): the
							// particles alive at time, four vertices and six indexes each; returns the quads
	int						BuildSprites( const idBseSegment &segment, float time, const idVec3 &viewLeft, const idVec3 &viewUp,
								idDrawVert *verts, int maxQuads, idBounds &bounds ) const;
	int						CountParticles( void ) const;

	bseEffectParms_t		owner;				// the owner's last state
	const idDeclEffect *	decl;
	float					startTime;			// this loop's start
	float					duration;			// this loop's length (a non-looping effect: until its last particle)
	float					lastTime;
	float					currentTime;
	bool					looping;
	bool					ambient;
	bool					stopped;
	bool					inConnectedArea;
	idVec3					currentOrigin;
	idVec3					lastOrigin;
	idMat3					currentAxis;
	idVec3					currentVelocity;
	idVec3					originalOrigin;
	idVec3					originalEndOrigin;
	idVec3					currentEndOrigin;
	bool					hasEndOrigin;
	idVec3					gravity;
	float					boundsRadius;		// the decl's, under the game's gravity
	idBounds				worldBounds;		// grows as the effect moves
	idBounds				localBounds;		// worldBounds in the current frame
	idBounds				particleBounds;		// the live particles at the last service, current frame
	float					ownerAttenuation;
	float					shortestDistance;	// player to localBounds
	float					originDistance;		// player to the origin
	idVec4					tint;				// the owner's colour (shader parms 0-3)
	float					brightness;			// shader parm 6
	idList<idBseSegment>	segments;

	// the engine side
	idRenderWorld *			world;
	int						entityHandle;
	int						serviceTime;		// ms, the last game time serviced
	bool					finished;

private:
	void					UpdateAttenuation( void );
	void					UpdateParticleBounds( float time );
	void					SetDuration( float time );
	void					LoopSegments( float time );
	void					StartSegments( float time );
	bool					SpawnAllowed( void ) const { return currentTime - lastTime > BSE_TIME_EPSILON; }
	friend class			idBseSegment;
};

/*
===============================================================================

	hooks: what the runtime asks of the engine (BseRender.cpp; the offline test has its own)

===============================================================================
*/

float			BSE_GameGravity( void );				// g_gravity, for the decls' bounds
bool			BSE_ViewOrigin( const idRenderWorld *world, idVec3 &origin );	// the player's view of world, for distance attenuation
idRandom &		BSE_Random( void );

/*
===============================================================================

	engine API (BseRender.cpp)

===============================================================================
*/

void			BSE_Init( void );
void			BSE_Shutdown( void );
void			BSE_WorldFreed( idRenderWorld *world );		// its effects keep playing without entities
				// NULL when the effect does not play (no decl, effects disabled)
idBseEffect *	BSE_PlayEffect( idRenderWorld *world, const bseEffectParms_t &parms, int timeMs );
				// the owner's new state; true when the effect is over and can be freed
bool			BSE_UpdateEffect( idBseEffect *effect, const bseEffectParms_t &parms, int timeMs );
void			BSE_StopEffect( idBseEffect *effect );				// no more spawns; live particles die out
void			BSE_FreeEffect( idBseEffect *effect );
float			BSE_EffectDuration( const idBseEffect *effect );	// seconds
				// an effect decl's own length, when no effect plays it
float			BSE_DeclDuration( const idDeclEffect *decl );

				// renderer: an entity drawn by an effect; R_AddModelSurfaces hands its effects here and
				// draws them once all entities are done, farthest first
bool			R_IsBseEntity( const struct renderEntity_s *parms );
void			R_AddBseEntity( struct viewEntity_s *vEntity );
void			R_AddBseSurfaces( void );

#endif /* !__BSEEFFECT_H__ */
