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
#include "idlib/geometry/Winding.h"
#include "framework/CVarSystem.h"
#include "renderer/Model.h"
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
extern idCVar *	bse_physics;		// particles collide (impact blocks)
extern idCVar *	bse_debris;			// debris particles become game entities

ID_INLINE bool	BSE_Enabled( void ) { return bse_enabled == NULL || bse_enabled->GetBool(); }
ID_INLINE bool	BSE_Render( void ) { return bse_render == NULL || bse_render->GetBool(); }
ID_INLINE float	BSE_Scale( void ) { return bse_scale != NULL ? bse_scale->GetFloat() : 1.0f; }
ID_INLINE int	BSE_MaxParticles( void ) { return bse_maxParticles != NULL ? bse_maxParticles->GetInteger() : 2048; }
ID_INLINE int	BSE_Debug( void ) { return bse_debug != NULL ? bse_debug->GetInteger() : 0; }
ID_INLINE bool	BSE_Physics( void ) { return bse_physics == NULL || bse_physics->GetBool(); }
ID_INLINE bool	BSE_Debris( void ) { return bse_debris == NULL || bse_debris->GetBool(); }

const float BSE_FUTURE				= 0.016f;	// emitters spawn this far past the time they are serviced at
const float BSE_TIME_EPSILON		= 0.002f;
const float BSE_MAX_LIFE			= 300.0f;	// seconds
const int	BSE_MAX_PARTICLES		= 2048;		// per segment
const int	BSE_MAX_TRAIL_COUNT		= 2500;		// motion / burn trail steps
const int	BSE_MAX_FORKS			= 16;		// electricity
const int	BSE_MAX_BOLTS			= 200;		// electricity: segments of a branch
const int	BSE_MAX_BOLT_POINTS		= 255;		// electricity: points of a branch
const int	BSE_MAX_BOLT_VERTS		= 16384;	// electricity: a segment's vertices (retail's surface; bolts past it are cut)
const int	BSE_MAX_SURFACE_VERTS	= 10000;	// the other kinds, and motion trails: retail's surfaces; particles past them are not drawn
const int	BSE_MAX_SURFACE_INDEXES	= 30000;
const float BSE_PHYSICS_SAMPLE		= 0.1f;		// a physics trace covers the particle's last 0.1 s of motion
const float BSE_BOUNCE_REST_SPEED_SQR	= 2500.0f;	// a bounce slower than 50 units/s off a floor comes to rest

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
	float					motionStartTime;	// where the motion runs from: the birth, or the last bounce
	bool					stationary;			// came to rest on a floor
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
	idVec3					lengthStart, lengthEnd;		// lines and electricity
	float					textureScale;		// lines: repeats along the length; a strip: per unit of fraction
	// motion and burn trails
	float					trailTime;
	int						trailCount;
	float					lastTrailTime;		// a named trail: its next particle's birth time
	int						seed;				// electricity: the bolt's shape
} bseParticle_t;

// a viewer, in the effect's frame
typedef struct {
	idVec3					origin;
	idVec3					left;				// the view's axis 1 and 2
	idVec3					up;
} bseView_t;

// a segment's geometry as it is built: grows as needed, kept for reuse
class idBseGeometry {
public:
							idBseGeometry( void ) { verts.SetGranularity( 1024 ); indexes.SetGranularity( 1024 ); bounds.Clear(); }
	void					Clear( void ) { verts.SetNum( 0, false ); indexes.SetNum( 0, false ); bounds.Clear(); }
	int						NumVerts( void ) const { return verts.Num(); }
	int						NumIndexes( void ) const { return indexes.Num(); }
	idDrawVert &			AddVert( const idVec3 &xyz, float s, float t, const byte rgba[4] );
	void					AddTri( int a, int b, int c ) { indexes.Append( a ); indexes.Append( b ); indexes.Append( c ); }

	idList<idDrawVert>		verts;
	idList<glIndex_t>		indexes;
	idBounds				bounds;
};

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
	int						budgetOnce;			// ... when the effect plays once, and when it loops
	int						budgetLooping;
	float					trailDuration;		// how long a particle leaves a named trail, for its budget
	idList<bseParticle_t>	particles;			// draw order: soonest to die first (complex: newest first)
	// a sound segment's play: the volume and pitch it picked, and whether its looping sound is on
	float					soundVolume;
	float					soundFreqShift;
	bool					soundPlaying;
	// a light segment's render light (the engine side; -1: none)
	int						lightHandle;

							// a named trail's budget grows by what the segments leaving it hold
	void					AddTrailBudget( int once, int looping, float duration );
							// a particle of this (trail) segment, left at birthTime where offset and axis say
	void					SpawnTrailParticle( idBseEffect *effect, float birthTime, const idVec3 &offset, const idMat3 &axis );

private:
	float					AttenuatedInterval( idBseEffect *effect ) const;
	float					AttenuatedCount( idBseEffect *effect ) const;
	void					Spawn( idBseEffect *effect, float birthTime, float fraction, const idVec3 *trailOffset = NULL, const idMat3 *trailAxis = NULL );
	void					SpawnParticle( idBseEffect *effect, bseParticle_t &p, float birthTime, float fraction, const idVec3 *trailOffset, const idMat3 *trailAxis );
	void					SampleLength( idBseEffect *effect, const effectDomain_t &domain, idVec3 &length ) const;
	void					RefreshLengths( idBseEffect *effect );
	void					EmitTrail( idBseEffect *effect, bseParticle_t &p, float time );
	void					Insert( const bseParticle_t &p );
							// physics (an impact block): the trace over the last moments of motion; true: remove
	bool					RunPhysics( idBseEffect *effect, bseParticle_t &p, float time );
	void					Bounce( idBseEffect *effect, bseParticle_t &p, const idVec3 &endPos, const idVec3 &normal, float time );
	void					PlayTimeoutEffect( idBseEffect *effect, const bseParticle_t &p, float time );
	void					SpawnDebris( idBseEffect *effect, float birthTime, float fraction );
	void					StartSound( idBseEffect *effect );
	void					HandleSound( idBseEffect *effect, float time );
	void					ProjectDecal( idBseEffect *effect );
};

// a light segment's render light at a time: from its live particle
typedef struct {
	idVec3					origin;				// world space
	idMat3					axis;
	idVec3					radius;
	idVec3					color;				// the particle's colour as it is: fade and the owner's colour do not apply
	const idMaterial *		material;			// NULL: the engine's default light
	bool					shadows;
	bool					specular;
} bseLight_t;

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
								// a view effect's strength: its scale, by the player's distance to the origin
	float					OriginAttenuation( const idEffectSegment &segment ) const;
							// a particle's position at time, in the effect's current frame
	idVec3					EvaluatePosition( const bseParticle_t &p, const idEffectParticle &tmpl, float time ) const;
							// a point of the effect's current frame in the world
	idVec3					ToWorld( const idVec3 &v ) const { return currentOrigin + currentAxis * v; }
							// its velocity at time, in the frame it was born in
	idVec3					EvaluateVelocity( const bseParticle_t &p, float time ) const;
							// a particle's state at time, in the effect's current frame; false when not alive then
	bool					EvaluateParticle( const bseParticle_t &p, const idEffectParticle &tmpl, float time,
								idVec3 &position, idVec4 &tint, idVec3 &size, idVec3 &rotate ) const;
							// a segment's particles alive at time as geometry for a viewer (effect frame),
							// appended to geo; returns the particles drawn
	int						BuildSegment( const idBseSegment &segment, float time, const bseView_t &view, idBseGeometry &geo ) const;
							// the ribbons of a segment's "trailType motion" particles
	int						BuildMotionTrails( const idBseSegment &segment, float time, const bseView_t &view, idBseGeometry &geo ) const;
	int						CountParticles( void ) const;
							// a light segment's light at time; false when it has none then
	bool					EvaluateLight( const idBseSegment &segment, float time, bseLight_t &light ) const;
							// the live particles' bounds, if a service changed them since (BSE_SyncWorld: once
							// before a scene renders, not every game tic)
	void					UpdateBounds( void );

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
	idMat3					originalAxis;
	idVec3					originalEndOrigin;
	idVec3					currentEndOrigin;
	bool					hasEndOrigin;
	idVec3					gravity;
	float					boundsRadius;		// the decl's, under the game's gravity
	idBounds				worldBounds;		// grows as the effect moves
	idBounds				localBounds;		// worldBounds in the current frame
	idBounds				particleBounds;		// the live particles at the last service, current frame
	bool					boundsDirty;		// serviced since particleBounds was taken
	float					ownerAttenuation;
	float					shortestDistance;	// player to localBounds
	float					originDistance;		// player to the origin
	idVec4					tint;				// the owner's colour (shader parms 0-3)
	float					brightness;			// shader parm 6
	idList<idBseSegment>	segments;

	bool					endOriginChanged;	// lines spanning to the end origin take their new lengths

	// the engine side
	idRenderWorld *			world;
	int						entityHandle;
	int						litHandle;			// the lit entity drawing lit model particles, or -1
	bool					entityDirty;		// serviced since its entities were last updated
	int						serviceTime;		// ms, the last game time serviced
	bool					finished;

private:
	void					UpdateAttenuation( void );
	void					UpdateParticleBounds( float time );
	void					SetDuration( float time );
	void					LoopSegments( float time );
	void					StartSegments( float time );
	bool					SpawnAllowed( void ) const { return currentTime - lastTime > BSE_TIME_EPSILON; }
	idVec3					ToCurrentFrame( const bseParticle_t &p, const idVec3 &v ) const;
	idVec3					BirthOffset( float birthTime ) const;
	byte					ColorByte( float x ) const;
	void					ParticleColor( const idEffectParticle &tmpl, const idVec4 &color, float override, byte rgba[4] ) const;
	bool					BuildParticle( const idBseSegment &segment, const bseParticle_t &p, float time, float override,
								const bseView_t &view, idBseGeometry &geo ) const;
	void					BuildElectricity( const idEffectParticle &tmpl, const bseParticle_t &p, float time, const idVec3 &position,
								const idVec3 &length, float width, const byte rgba[4], const bseView_t &view, idBseGeometry &geo ) const;
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
				// a model particle's mesh: its model's first surface, as Quake 4 draws it (NULL: none)
const srfTriangles_t *	BSE_ModelGeometry( const idRenderModel *model );
				// sounds, on the effect's emitter (the owner's referenceSoundHandle; none: silent)
float			BSE_SoundLength( const idSoundShader *shader );		// seconds, 0 when not known
bool			BSE_SoundLooping( const idSoundShader *shader );
void			BSE_StartSound( idBseEffect *effect, int channel, const idSoundShader *shader, float diversity, bool looping );
void			BSE_UpdateSound( idBseEffect *effect, float volume, float freqShift );	// the emitter follows the effect
void			BSE_StopSound( idBseEffect *effect, int channel );
				// a decal onto the world, as idRenderWorld::ProjectDecalOntoWorld takes it
void			BSE_ProjectDecal( idBseEffect *effect, const idFixedWinding &winding, const idVec3 &projectionOrigin,
					float depth, const idMaterial *material, int startTimeMs );
				// physics: a point moving from start to end through the game's world (the game's Translation,
				// solids and render-model clip); false when nothing is hit
bool			BSE_Trace( idBseEffect *effect, const idVec3 &start, const idVec3 &end, idVec3 &endPos, idVec3 &normal );
bool			BSE_CanPlayImpactEffect( void );		// the game's rate limit for particle impact effects
				// requests to the game, delivered at its next effect frame bracket (never mid-render): an effect
				// played once (impacts, timeouts, nested segments), and a debris entity
void			BSE_RequestEffect( idBseEffect *effect, const idDeclEffect *child, const idVec3 &origin, const idMat3 &axis );
void			BSE_RequestDebris( idBseEffect *effect, const char *entityDef, const idVec3 &origin, const idMat3 &axis,
					const idVec3 &velocity, const idVec3 &angularVelocity );
				// a view effect for the player (segment type shake, doubleVision or tunnel): until endTime (seconds
				// of game time), at that strength
void			BSE_RequestViewEffect( idBseEffect *effect, effectSegmentType_t type, float endTime, float scale );

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
				// before a scene of world renders at timeMs: the entities of its effects serviced since, with
				// their particles' bounds (a cinematic skip runs many game tics and renders none); also runs
				// testEffect's effects, which no game owner services
void			BSE_SyncWorld( idRenderWorld *world, int timeMs );
void			BSE_FreeEffect( idBseEffect *effect );
float			BSE_EffectDuration( const idBseEffect *effect );	// seconds
				// an effect decl's own length, when no effect plays it
float			BSE_DeclDuration( const idDeclEffect *decl );
				// the Quake 4 game's effect frame bracket (bse->StartFrame / EndFrame): queued child effects and
				// debris go to the game
void			BSE_DeliverRequests( void );

				// renderer: an entity drawn by an effect; R_AddModelSurfaces hands its effects here and
				// draws them once all entities are done, farthest first
bool			R_IsBseEntity( const struct renderEntity_s *parms );
void			R_AddBseEntity( struct viewEntity_s *vEntity );
void			R_AddBseSurfaces( void );

#endif /* !__BSEEFFECT_H__ */
