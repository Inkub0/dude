/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: Quake 4's effects (Raven's BSE, docs/quake4-bse.md) - the effect decl. Written from the
// retail files' grammar (all 1172 effect decls of the 1.4.3 paks parse), the SDK's interface
// header and public documentation; the names are DUDE's own.

#ifndef __DECLEFFECT_H__
#define __DECLEFFECT_H__

#include "idlib/containers/List.h"
#include "idlib/math/Vector.h"
#include "idlib/Str.h"
#include "framework/DeclManager.h"

class idDeclTable;
class idMaterial;
class idSoundShader;
class idRenderModel;
class idDeclEffect;

/*
===============================================================================

	idDeclEffect - effects/<name>.fx

	effect effects/ambient/acid_react_waste {
		size	449
		emitter "bubbles2" {
			duration	1,1
			count		50,50
			oriented {
				duration	1,1
				blend		add
				material	"gfx/effects/fluids_drips/bubble_additive"
				start {
					position	{ sphere 1,-80,-80,1,80,80 }
					size		{ point 5,5 }
					tint		{ line 0.25098,0,0,0.501961,0.501961,0 }
				}
				motion {
					size		{ envelope "linear" }
					fade		{ envelope "linear" }
				}
				end {
					size		{ box 10,10,30,30 }
				}
			}
		}
	}

	An effect is a list of segments that play side by side, each from its start time: particle
	segments (emitter, spawner, trail, decal, light) hold one particle description, the others
	play a sound, another effect or a view effect. A particle's parameters (position, size,
	tint, ...) take a value at birth from their "start" domain - a point, or a random place in a
	line, box, sphere, cylinder or spiral - and, when "motion" gives them an envelope (a table
	decl), move towards their "end" value over the particle's life. Each parameter has a fixed
	number of components per particle type (a sprite's size is 2, a line's 1, a light's 3), and
	the file gives every value list as exactly that many numbers per point of the domain.

===============================================================================
*/

// what the segments of an effect do
typedef enum {
	EFFECT_SEG_EMITTER,				// emits its count of particles per second, over its duration
	EFFECT_SEG_SPAWNER,				// spawns its count of particles at once
	EFFECT_SEG_TRAIL,				// particles left behind by another segment's (its trailType names this one)
	EFFECT_SEG_DECAL,				// decals on the surfaces around the effect
	EFFECT_SEG_LIGHT,				// a light
	EFFECT_SEG_SOUND,				// a sound shader, on the effect's sound emitter
	EFFECT_SEG_EFFECT,				// another effect
	EFFECT_SEG_SHAKE,				// view effects for the player near the effect
	EFFECT_SEG_DOUBLEVISION,
	EFFECT_SEG_TUNNEL,
	EFFECT_SEG_DELAY,				// shows nothing: its time window shapes the effect's loop
	EFFECT_SEG_NUM
} effectSegmentType_t;

// the particles of a particle segment
typedef enum {
	EFFECT_PARTICLE_NONE = -1,
	EFFECT_PARTICLE_SPRITE,			// a quad facing the viewer
	EFFECT_PARTICLE_LINE,			// a quad along its length vector, turned to the viewer
	EFFECT_PARTICLE_ORIENTED,		// a quad turned by its own angles
	EFFECT_PARTICLE_ELECTRICITY,	// a jittered bolt along its length, with forks
	EFFECT_PARTICLE_DEBRIS,			// handed to the game as an entity (entityDef)
	EFFECT_PARTICLE_MODEL,			// a render model
	EFFECT_PARTICLE_LINKED,			// the segment's particles joined into one strip
	EFFECT_PARTICLE_DECAL,			// a decal projected onto the surfaces around it
	EFFECT_PARTICLE_LIGHT,			// a light
	EFFECT_PARTICLE_NUM
} effectParticleType_t;

// a particle's parameters
typedef enum {
	EFFECT_PARM_POSITION,			// where it is born, about the effect's origin
	EFFECT_PARM_VELOCITY,
	EFFECT_PARM_ACCELERATION,
	EFFECT_PARM_FRICTION,
	EFFECT_PARM_SIZE,
	EFFECT_PARM_LENGTH,				// a line's or a bolt's extent
	EFFECT_PARM_TINT,				// colour
	EFFECT_PARM_FADE,				// opacity
	EFFECT_PARM_ROTATE,				// the particle's own rotation
	EFFECT_PARM_ANGLE,				// the angles that orbit the offset about the particle's position
	EFFECT_PARM_OFFSET,				// a displacement from the particle's position
	EFFECT_PARM_NUM
} effectParmId_t;

// where a parameter's value comes from
typedef enum {
	EFFECT_DOMAIN_NONE,				// not given
	EFFECT_DOMAIN_POINT,			// one value
	EFFECT_DOMAIN_LINE,				// between two values
	EFFECT_DOMAIN_BOX,				// within the box two values span
	EFFECT_DOMAIN_SPHERE,			// within the sphere a box spans (3 components)
	EFFECT_DOMAIN_CYLINDER,			// within the cylinder a box spans (3 components)
	EFFECT_DOMAIN_SPIRAL,			// along a spiral in a box, with a 7th value (3 components)
	EFFECT_DOMAIN_NUM
} effectDomainType_t;

// domain modifiers, after the values: "sphere 1,-80,-80,1,80,80 surface"
#define EFFECT_DOMAIN_SURFACE				BIT( 0 )	// on the shape's surface, not in it
#define EFFECT_DOMAIN_RELATIVE				BIT( 1 )
#define EFFECT_DOMAIN_CONE					BIT( 2 )
#define EFFECT_DOMAIN_USE_END_ORIGIN		BIT( 3 )	// spans the effect's origin to its end origin (beams)
#define EFFECT_DOMAIN_LINEAR_SPACING		BIT( 4 )	// spaced in spawn order rather than at random
#define EFFECT_DOMAIN_ATTENUATE				BIT( 5 )	// scaled by the segment's distance attenuation
#define EFFECT_DOMAIN_INVERSE_ATTENUATE		BIT( 6 )
#define EFFECT_DOMAIN_NUM_FLAGS				7

const int EFFECT_MAX_DOMAIN_VALUES		= 7;	// a spiral: 3 + 3 + 1

// a parameter's value at birth or death, as the file gives it
typedef struct {
	effectDomainType_t		type;
	int						flags;			// EFFECT_DOMAIN_SURFACE ...
	int						numValues;
	float					values[EFFECT_MAX_DOMAIN_VALUES];
} effectDomain_t;

// envelope lists, after the table: "envelope linear count 2,2 offset 0.5,0.5"
#define EFFECT_ENVELOPE_COUNT				BIT( 0 )
#define EFFECT_ENVELOPE_OFFSET				BIT( 1 )
#define EFFECT_ENVELOPE_RATE				BIT( 2 )

// how a value moves from its start to its end over the particle's life
typedef struct {
	const idDeclTable *		table;			// NULL: no envelope
	int						flags;			// the lists given: EFFECT_ENVELOPE_COUNT ...
	float					count[3];		// one per component
	float					offset[3];
	float					rate[3];
} effectEnvelope_t;

typedef struct {
	effectDomain_t			start;			// "start" block
	effectEnvelope_t		envelope;		// "motion" block
	effectDomain_t			end;			// "end" block
} effectParm_t;

// a range the file gives as "min,max"
typedef struct {
	float					min;
	float					max;
} effectRange_t;

// particle flags
#define EFFECT_PARTICLE_BLEND_ADD			BIT( 0 )	// "blend add"
#define EFFECT_PARTICLE_PERSIST				BIT( 1 )	// lives on when the effect stops
#define EFFECT_PARTICLE_GENERATED_NORMAL	BIT( 2 )
#define EFFECT_PARTICLE_GENERATED_ORIGIN_NORMAL	BIT( 3 )
#define EFFECT_PARTICLE_FLIP_NORMAL			BIT( 4 )
#define EFFECT_PARTICLE_GENERATED_LINE		BIT( 5 )
#define EFFECT_PARTICLE_SHADOWS				BIT( 6 )	// a light particle casts shadows
#define EFFECT_PARTICLE_SPECULAR			BIT( 7 )	// a light particle lights specular
#define EFFECT_PARTICLE_NUM_FLAGS			8

// the particle keywords a file gives (which values are the file's, not defaults)
#define EFFECT_PKEY_DURATION				BIT( 0 )
#define EFFECT_PKEY_MATERIAL				BIT( 1 )
#define EFFECT_PKEY_GRAVITY					BIT( 2 )
#define EFFECT_PKEY_ENTITYDEF				BIT( 3 )
#define EFFECT_PKEY_MODEL					BIT( 4 )
#define EFFECT_PKEY_FORK					BIT( 5 )
#define EFFECT_PKEY_JITTER_RATE				BIT( 6 )
#define EFFECT_PKEY_JITTER_SIZE				BIT( 7 )
#define EFFECT_PKEY_JITTER_TABLE			BIT( 8 )
#define EFFECT_PKEY_TRAIL_TYPE				BIT( 9 )
#define EFFECT_PKEY_TRAIL_TIME				BIT( 10 )
#define EFFECT_PKEY_TRAIL_COUNT				BIT( 11 )
#define EFFECT_PKEY_TRAIL_MATERIAL			BIT( 12 )
#define EFFECT_PKEY_TILING					BIT( 13 )
#define EFFECT_PKEY_IMPACT					BIT( 14 )	// an "impact" block
#define EFFECT_PKEY_TIMEOUT					BIT( 15 )	// a "timeout" block
#define EFFECT_PKEY_BOUNCE					BIT( 16 )	// in "impact"
#define EFFECT_PKEY_REMOVE					BIT( 17 )	// in "impact"
#define EFFECT_PKEY_NUM						18

// what a particle leaves behind
typedef enum {
	EFFECT_TRAIL_NONE,
	EFFECT_TRAIL_MOTION,			// "trailType motion"
	EFFECT_TRAIL_BURN,				// "trailType burn"
	EFFECT_TRAIL_SEGMENT			// "trailType <name>": the particles of the trail segment of that name
} effectTrailType_t;

class idEffectParticle {
public:
							idEffectParticle( void ) { Clear(); }

	void					Clear( void );

	effectParticleType_t	type;
	int						flags;			// EFFECT_PARTICLE_BLEND_ADD ...
	int						keys;			// EFFECT_PKEY_*
	effectRange_t			duration;		// life, seconds
	effectRange_t			gravity;
	const idMaterial *		material;
	idStr					entityDef;		// the game entity a debris particle becomes
	idRenderModel *			model;			// a model particle's model

	// electricity
	int						fork;
	float					jitterRate;
	idVec3					jitterSize;
	const idDeclTable *		jitterTable;

	// trails
	effectTrailType_t		trailType;
	idStr					trailName;		// EFFECT_TRAIL_SEGMENT: the segment's name
	int						trailSegment;	// its index in the effect, -1 when no trail segment has the name
	effectRange_t			trailTime;
	effectRange_t			trailCount;
	const idMaterial *		trailMaterial;

	float					tiling;			// texture repeats along a line or a strip

	// "impact": what hitting a surface does; "timeout": what the end of the life plays
	float					bounce;
	float					remove;
	idList<const idDeclEffect *>	impactEffects;
	idList<const idDeclEffect *>	timeoutEffects;

	effectParm_t			parms[EFFECT_PARM_NUM];
};

// segment flags
#define EFFECT_SEGMENT_LOCKED				BIT( 0 )	// moves with the effect (and the joint it is bound to)
#define EFFECT_SEGMENT_CONSTANT				BIT( 1 )
#define EFFECT_SEGMENT_ATTENUATE_EMITTER	BIT( 2 )	// thinned out with distance (attenuation)
#define EFFECT_SEGMENT_INVERSE_ATTENUATE_EMITTER	BIT( 3 )
#define EFFECT_SEGMENT_NUM_FLAGS			4

// the segment keywords a file gives
#define EFFECT_SKEY_START					BIT( 0 )
#define EFFECT_SKEY_DURATION				BIT( 1 )
#define EFFECT_SKEY_COUNT					BIT( 2 )
#define EFFECT_SKEY_DENSITY					BIT( 3 )
#define EFFECT_SKEY_ATTENUATION				BIT( 4 )
#define EFFECT_SKEY_VOLUME					BIT( 5 )
#define EFFECT_SKEY_FREQSHIFT				BIT( 6 )
#define EFFECT_SKEY_DETAIL					BIT( 7 )
#define EFFECT_SKEY_SCALE					BIT( 8 )
#define EFFECT_SKEY_PARTICLE_CAP			BIT( 9 )
#define EFFECT_SKEY_SOUND_SHADER			BIT( 10 )
#define EFFECT_SKEY_EFFECT					BIT( 11 )
#define EFFECT_SKEY_NUM						12

class idEffectSegment {
public:
							idEffectSegment( void ) { Clear(); }

	void					Clear( void );

	effectSegmentType_t		type;
	idStr					name;
	int						flags;			// EFFECT_SEGMENT_LOCKED ...
	int						keys;			// EFFECT_SKEY_*
	effectRange_t			start;			// seconds after the effect starts
	effectRange_t			duration;
	effectRange_t			count;			// particles (per second for an emitter)
	effectRange_t			density;		// particles per unit of the spawn domain's size
	effectRange_t			attenuation;	// the distances over which attenuation acts
	effectRange_t			volume;			// sound
	effectRange_t			freqShift;		// sound
	float					detail;			// 0-1: the lowest quality that shows the segment
	float					scale;			// view effects
	int						particleCap;
	const idSoundShader *	soundShader;
	const idDeclEffect *	effect;			// EFFECT_SEG_EFFECT
	idEffectParticle		particle;		// type EFFECT_PARTICLE_NONE for segments without particles
};

// effect keywords
#define EFFECT_KEY_SIZE						BIT( 0 )
#define EFFECT_KEY_CUTOFF_DISTANCE			BIT( 1 )

class idDeclEffect : public idDecl {
public:
	virtual size_t			Size( void ) const;
	virtual const char *	DefaultDefinition( void ) const;
	virtual bool			Parse( const char *text, const int textLength );
	virtual void			FreeData( void );

	float					GetSize( void ) const { return size; }			// bounding radius about the origin
	float					GetCutOffDistance( void ) const { return cutOffDistance; }	// 0: none
	int						GetKeys( void ) const { return keys; }			// EFFECT_KEY_*
	int						GetNumSegments( void ) const { return segments.Num(); }
	const idEffectSegment &	GetSegment( int index ) const { return segments[index]; }
	bool					HasSound( void ) const { return hasSound; }		// a sound segment with a shader

private:
	float					size;
	float					cutOffDistance;
	int						keys;
	bool					hasSound;
	idList<idEffectSegment>	segments;		// in file order

	void					Finish( void );
};

#endif /* !__DECLEFFECT_H__ */
