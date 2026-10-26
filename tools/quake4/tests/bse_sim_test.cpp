/*
	Offline test of the engine's effect runtime (neo/renderer/bse/BseEffect.cpp, Quake 4's BSE):
	the effect decl parser (DeclEffect.cpp), the table decl parser (framework/DeclTable.cpp) and
	the runtime compiled into this program with the real idLexer (libidlib.a). The envelope
	tables are the retail ones, parsed from the paks' materials; materials, sounds, effects and
	models are stand-ins the parser only stores.

	argv[1]: the retail table decls, as "name\nlength\n" + text per table. stdin: effect files
	the same way. Every effect plays once, unlocked, at the origin (and its looping variant for
	three loops), serviced at 60 Hz; every live particle is evaluated each frame. Reported:
	  - numbers that are not finite (gate: none);
	  - non-looping effects that do not end by 120 s (gate: none);
	  - the decl's radius by the retail bounds formula against the file's "size": the effect
	    editor wrote "size" with it (under whatever g_gravity it ran with: 0 without a game);
	  - particles beyond twice that radius: the formula's radius is half the furthest reach, so
	    particles past twice it move further than retail's own bounds allow - a sign of a wrong
	    reading of the motion (velocity, gravity, envelopes);
	  - the most particles alive per segment against its budget.

	Geometry (S3): ten times a second every segment's particles and motion trails are built for
	a viewer as the renderer builds them - sprites, oriented quads, lines, bolts, strips, models
	(a stand-in cube for every model) and burn trails. Gate: every vertex finite, every index
	inside its geometry, whole triangles. Reported per particle kind: the segments drawn and the
	most vertices one segment took; and how many named trail segments received particles.

	Lights, sounds, decals (S4): every light segment's light is evaluated ten times a second (gate:
	finite, radius at least 1); every decal's projection is checked (gate: four finite corners);
	sounds are counted as they start (a stand-in shader 1.5 s long, never looping).

	Physics, impacts, debris, nested effects (S5): every effect plays inside a closed room (a box
	the stand-in trace collides with, as the game's Translation would) and the effects it asks the
	game to play - impact, timeout and nested effects - are played back the way the game does
	(once, where asked; children of children up to three deep), each in the same room. The child
	effects are the real retail decls (a name the data lacks is a defaulted decl and plays nothing).
	Gate: no request that is not finite (origin, axis, velocity, spin) and no debris without an
	entity def; no particle that bounces or goes on impact crosses a wall while a trace covers it
	(retail's untraced first moments after a birth or a bounce are reported apart); every child
	chain ends; the live effects stay under the world's limit. Reported: trace hits, bounces, particles come to rest,
	the requests by kind (most in one game frame against the engine's per-frame limits), the
	debris entity defs asked for. The looping run ends with a stop: the effect (persisting
	particles included) must then end within 65 s.

	View effects (S6): every shake / double vision / tunnel request is checked (an end time not
	before the effect's start and finite, a scale finite and not negative) and counted by kind; the
	distance ramp is checked on a retail shake (rocket impact: no scale, so 1; attenuation 200-600:
	full at 100 units, half at 400, none at 700).

	Built and run by tools/quake4/test_bse_sim.sh. No game, no window, no GPU.
*/

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "sys/platform.h"
#include "idlib/Lexer.h"
#include "framework/Common.h"
#include "framework/DeclManager.h"
#include "framework/DeclTable.h"
#include "renderer/ModelManager.h"
#include "renderer/bse/DeclEffect.h"
#include "renderer/bse/BseEffect.h"

#include "common_stub.h"
#include "decl_stub.h"
#include "effect_decls.h"

// the retail tables and effects, by name
static std::map<std::string, idDeclTable *> tables;
static std::map<std::string, idDeclEffect *> effectsByName;
static idDeclEffect *FindEffectByName( const char *name );
static idDeclTable *defaultTable = NULL;
alignas( 64 ) static unsigned char standIn[256];

static std::string Lower( const char *s ) {
	std::string out( s );
	for ( char &c : out ) {
		c = (char)tolower( (unsigned char)c );
	}
	return out;
}

static struct {
	const idDecl *			FindType( declType_t type, const char *name, bool makeDefault = true ) {
								if ( type == DECL_TABLE ) {
									auto it = tables.find( Lower( name ) );
									// a missing table is the engine's default one, "{ { 0 } }"
									return it != tables.end() ? it->second : defaultTable;
								}
								return reinterpret_cast<const idDecl *>( standIn );
							}
	const idMaterial *		FindMaterial( const char *name, bool makeDefault = true ) { return reinterpret_cast<const idMaterial *>( standIn ); }
	const idSoundShader *	FindSound( const char *name, bool makeDefault = true ) { return reinterpret_cast<const idSoundShader *>( standIn ); }
	const idDeclEffect *	FindEffect( const char *name, bool makeDefault = true ) { return FindEffectByName( name ); }
} declManagerStub;

static struct {
	idRenderModel *			FindModel( const char *name ) { return reinterpret_cast<idRenderModel *>( standIn ); }
} renderModelManagerStub;

#define declManager			( &declManagerStub )
#define renderModelManager	( &renderModelManagerStub )
#include "renderer/bse/DeclEffect.cpp"
#undef declManager
#undef renderModelManager

#include "framework/DeclTable.cpp"
#include "renderer/bse/BseEffect.cpp"

static bool Finite( const idVec3 &v ) {
	return std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z );
}

// every effect decl of the files, made before any is parsed so references find them; a name the
// files lack is a defaulted decl (the default effect: plays nothing), as the decl manager makes
static std::map<std::string, idDeclBaseStub *> effectBases;
static int missingEffects = 0;
static idDeclEffect *FindEffectByName( const char *name ) {
	const std::string key = Lower( name );
	auto it = effectsByName.find( key );
	if ( it != effectsByName.end() ) {
		return it->second;
	}
	idDeclEffect *decl = new idDeclEffect;
	idDeclBaseStub *base = new idDeclBaseStub( decl, DECL_EFFECT, name, "_missing", 0 );
	base->MakeDefault();
	effectsByName[key] = decl;
	effectBases[key] = base;
	missingEffects++;
	return decl;
}

// the runtime's engine hooks
static const float GRAVITY = 1066.0f;
static idRandom simRandom;
float BSE_GameGravity( void ) { return GRAVITY; }
bool BSE_ViewOrigin( const idRenderWorld *world, idVec3 &origin ) { return false; }	// no player: full detail
idRandom &BSE_Random( void ) { return simRandom; }

// sounds and decals: counted and checked, not played
static long soundsStarted = 0, soundUpdates = 0, soundsStopped = 0, decalsProjected = 0, badDecals = 0;
static long lightsEvaluated = 0, badLights = 0;
float BSE_SoundLength( const idSoundShader *shader ) { return 1.5f; }
bool BSE_SoundLooping( const idSoundShader *shader ) { return false; }
void BSE_StartSound( idBseEffect *effect, int channel, const idSoundShader *shader, float diversity, bool looping ) { soundsStarted++; }
void BSE_UpdateSound( idBseEffect *effect, float volume, float freqShift ) {
	soundUpdates++;
	if ( !std::isfinite( volume ) || !std::isfinite( freqShift ) ) {
		badDecals++;		// counted with the bad decals: any bad S4 output fails the gate
	}
}
void BSE_StopSound( idBseEffect *effect, int channel ) { soundsStopped++; }
void BSE_ProjectDecal( idBseEffect *effect, const idFixedWinding &winding, const idVec3 &projectionOrigin,
		float depth, const idMaterial *material, int startTimeMs ) {
	decalsProjected++;
	bool ok = winding.GetNumPoints() == 4 && std::isfinite( projectionOrigin.x ) && std::isfinite( projectionOrigin.y ) && std::isfinite( projectionOrigin.z );
	for ( int i = 0; i < winding.GetNumPoints(); i++ ) {
		const idVec5 &v = winding[i];
		ok = ok && std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z );
	}
	if ( !ok ) {
		badDecals++;
	}
}

// every model particle's model: a cube 8 units across, its faces' own vertices
static idDrawVert cubeVerts[24];
static glIndex_t cubeIndexes[36];
static srfTriangles_t cubeTri;
const srfTriangles_t *BSE_ModelGeometry( const idRenderModel *model ) {
	if ( cubeTri.numVerts == 0 ) {
		for ( int face = 0; face < 6; face++ ) {
			const int axis = face % 3;
			const float sign = face < 3 ? -1.0f : 1.0f;
			idVec3 normal( vec3_origin );
			normal[axis] = sign;
			const idVec3 u = axis == 0 ? idVec3( 0, 1, 0 ) : idVec3( 1, 0, 0 );
			const idVec3 v = normal.Cross( u );
			for ( int k = 0; k < 4; k++ ) {
				idDrawVert &dv = cubeVerts[face * 4 + k];
				dv.Clear();
				const float a = ( k == 1 || k == 2 ) ? 1.0f : -1.0f;
				const float b = ( k >= 2 ) ? 1.0f : -1.0f;
				dv.xyz = ( normal + u * a + v * b ) * 4.0f;
				dv.st.Set( a * 0.5f + 0.5f, b * 0.5f + 0.5f );
				dv.normal = normal;
				dv.tangents[0] = u;
				dv.tangents[1] = v;
			}
			const int base = face * 4;
			const int tri[6] = { 0, 1, 2, 0, 2, 3 };
			for ( int k = 0; k < 6; k++ ) {
				cubeIndexes[face * 6 + k] = base + tri[k];
			}
		}
		cubeTri.verts = cubeVerts;
		cubeTri.indexes = cubeIndexes;
		cubeTri.numVerts = 24;
		cubeTri.numIndexes = 36;
		cubeTri.bounds = idBounds( idVec3( -4, -4, -4 ), idVec3( 4, 4, 4 ) );
	}
	return model != NULL ? &cubeTri : NULL;
}

/*
	physics: the room every effect plays in - a closed box about the origin (the effect's origin is
	128 units above the floor, 192 from the walls), solid outside. A point that crosses one of its
	planes from inside stops there, a little short along its motion, as the collision model's own
	traces do (CM_CLIP_EPSILON); the contact normal points into the room.
*/
static const idBounds simRoom( idVec3( -192.0f, -192.0f, -128.0f ), idVec3( 192.0f, 192.0f, 256.0f ) );
static long traces = 0, traceHits = 0;
bool BSE_Trace( idBseEffect *effect, const idVec3 &start, const idVec3 &end, idVec3 &endPos, idVec3 &normal ) {
	traces++;
	const idVec3 delta = end - start;
	float best = 2.0f;
	int bestAxis = -1, bestSide = 0;
	for ( int axis = 0; axis < 3; axis++ ) {
		for ( int side = 0; side < 2; side++ ) {
			const float plane = simRoom[side][axis];
			// distances inside the room are positive
			const float d0 = side ? plane - start[axis] : start[axis] - plane;
			const float d1 = side ? plane - end[axis] : end[axis] - plane;
			if ( d0 >= 0.0f && d1 < 0.0f ) {
				const float f = d0 / ( d0 - d1 );
				if ( f < best ) {
					best = f;
					bestAxis = axis;
					bestSide = side;
				}
			}
		}
	}
	if ( bestAxis < 0 ) {
		return false;
	}
	const float length = delta.Length();
	const float back = length > 0.0f ? Min( best, 0.25f / length ) : 0.0f;
	endPos = start + delta * ( best - back );
	normal.Zero();
	normal[bestAxis] = bestSide ? -1.0f : 1.0f;
	traceHits++;
	return true;
}
bool BSE_CanPlayImpactEffect( void ) { return true; }		// the rate limit comes with S7

// the requests to the game: checked, counted, and the effects played back by the simulation
typedef struct {
	const idDeclEffect *	child;
	idVec3					origin;
	idMat3					axis;
	int						depth;		// the asking effect's depth + 1
} simRequest_t;
static std::vector<simRequest_t> pendingRequests;
static int requestDepth = 0;			// the depth of the effect being serviced
static long badRequests = 0, impactRequests = 0, timeoutRequests = 0, nestedRequests = 0, debrisRequests = 0;
static long frameEffectRequests = 0, frameDebrisRequests = 0;
static long maxFrameEffectRequests = 0, maxFrameDebrisRequests = 0;
static std::map<std::string, long> debrisDefs;
static std::string maxRequestEffect;

static bool FiniteAxis( const idMat3 &axis ) {
	for ( int i = 0; i < 3; i++ ) {
		if ( !std::isfinite( axis[i].x ) || !std::isfinite( axis[i].y ) || !std::isfinite( axis[i].z ) || idMath::Fabs( axis[i].Length() - 1.0f ) > 1e-3f ) {
			return false;
		}
	}
	return true;
}

void BSE_RequestEffect( idBseEffect *effect, const idDeclEffect *child, const idVec3 &origin, const idMat3 &axis ) {
	if ( !std::isfinite( origin.x ) || !std::isfinite( origin.y ) || !std::isfinite( origin.z ) || !FiniteAxis( axis ) ) {
		badRequests++;
		return;
	}
	// which kind: a nested segment's effect, an impact effect, or else a timeout effect
	bool nested = false, impact = false;
	for ( int i = 0; i < effect->decl->GetNumSegments(); i++ ) {
		const idEffectSegment &seg = effect->decl->GetSegment( i );
		nested = nested || ( seg.type == EFFECT_SEG_EFFECT && seg.effect == child );
		impact = impact || seg.particle.impactEffects.FindIndex( child ) >= 0;
	}
	( nested ? nestedRequests : impact ? impactRequests : timeoutRequests )++;
	frameEffectRequests++;
	if ( child->GetNumSegments() == 0 ) {
		return;		// a missing child: the engine never sends it
	}
	simRequest_t r = { child, origin, axis, requestDepth + 1 };
	pendingRequests.push_back( r );
}

void BSE_RequestDebris( idBseEffect *effect, const char *entityDef, const idVec3 &origin, const idMat3 &axis,
		const idVec3 &velocity, const idVec3 &angularVelocity ) {
	if ( entityDef == NULL || entityDef[0] == '\0' || !Finite( origin ) || !Finite( velocity ) || !Finite( angularVelocity ) || !FiniteAxis( axis ) ) {
		badRequests++;
		return;
	}
	debrisRequests++;
	frameDebrisRequests++;
	debrisDefs[Lower( entityDef )]++;
}

// view effects: checked and counted, by kind (shake, double vision, tunnel)
static long viewRequests[3], badViewRequests = 0;
static float maxViewScale = 0.0f, maxViewTime = 0.0f;
void BSE_RequestViewEffect( idBseEffect *effect, effectSegmentType_t type, float endTime, float scale ) {
	if ( !std::isfinite( endTime ) || !std::isfinite( scale ) || scale < 0.0f || endTime < effect->startTime - BSE_TIME_EPSILON ) {
		badViewRequests++;
		return;
	}
	viewRequests[type == EFFECT_SEG_SHAKE ? 0 : type == EFFECT_SEG_DOUBLEVISION ? 1 : 2]++;
	maxViewScale = Max( maxViewScale, scale );
	maxViewTime = Max( maxViewTime, endTime - effect->currentTime );
}

static bool ReadEntry( std::istream &in, std::string &name, std::string &data ) {
	std::string lengthLine;
	if ( !std::getline( in, name ) || !std::getline( in, lengthLine ) ) {
		return false;
	}
	data.assign( std::stoul( lengthLine ), '\0' );
	in.read( &data[0], data.size() );
	return true;
}

// per particle kind: the segments drawn, their vertices, the most one segment took
typedef struct {
	long	segments;
	long	verts;
	int		maxVerts;
	long	trailSegments;		// motion trail ribbons
	int		maxTrailVerts;
	std::string	maxEffect;		// the effect of the largest segment
} kindStats_t;
static kindStats_t kindStats[EFFECT_PARTICLE_LIGHT + 1];
static long badGeometry = 0;
static const char *currentEffect = "";		// a vertex not finite, an index outside, a broken triangle
static int namedTrails = 0, namedTrailsFed = 0;

static void CheckGeometry( const idBseGeometry &geo, int kind, bool trail ) {
	for ( int v = 0; v < geo.NumVerts(); v++ ) {
		const idDrawVert &dv = geo.verts[v];
		if ( !std::isfinite( dv.xyz.x ) || !std::isfinite( dv.xyz.y ) || !std::isfinite( dv.xyz.z ) ||
				!std::isfinite( dv.st.x ) || !std::isfinite( dv.st.y ) ) {
			badGeometry++;
		}
	}
	if ( geo.NumIndexes() % 3 != 0 ) {
		badGeometry++;
	}
	for ( int i = 0; i < geo.NumIndexes(); i++ ) {
		if ( geo.indexes[i] < 0 || geo.indexes[i] >= geo.NumVerts() ) {
			badGeometry++;
		}
	}
	if ( geo.NumVerts() == 0 || kind < 0 || kind > EFFECT_PARTICLE_LIGHT ) {
		return;
	}
	kindStats_t &k = kindStats[kind];
	if ( trail ) {
		k.trailSegments++;
		k.maxTrailVerts = Max( k.maxTrailVerts, geo.NumVerts() );
	} else {
		k.segments++;
		k.verts += geo.NumVerts();
		if ( geo.NumVerts() > k.maxVerts ) {
			k.maxVerts = geo.NumVerts();
			k.maxEffect = currentEffect;
		}
	}
}

typedef struct {
	int		notFinite;
	bool	ended;
	float	endTime;
	float	maxDistance;		// furthest particle from the origin
	long	samples;
	long	outside;			// particle samples beyond twice the decl's radius (+1): further than the retail bounds formula reaches
	int		overBudget;
	bool	endedAfterStop;		// the looping run: over within 65 s of its stop
} simResult_t;

// the effects one run plays: the effect tested and the children it had the game play
typedef struct {
	idBseEffect *		effect;
	bseEffectParms_t	parms;
	int					depth;		// 0: the effect tested
} simEffect_t;
static const int SIM_MAX_DEPTH = 3;			// children of children of children
static const int SIM_MAX_LIVE = 512;		// effects alive at once in one run
static long escapes = 0, gapEscapes = 0, bounces = 0, rests = 0, childrenPlayed = 0, childrenTooDeep = 0, worldFull = 0, childrenNotEnded = 0;
static int maxLive = 0;
static std::vector<std::string> escapeNotes;

static bseEffectParms_t SimParms( const idDeclEffect *decl, bool loop, const idVec3 &origin, const idMat3 &axis, float time ) {
	bseEffectParms_t parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.effect = decl;
	parms.startTime = time;
	parms.origin = origin;
	parms.axis = axis;
	parms.gravity.Set( 0.0f, 0.0f, -GRAVITY );
	parms.attenuation = 1.0f;
	parms.loop = loop;
	parms.inConnectedArea = true;
	parms.shaderParms[0] = parms.shaderParms[1] = parms.shaderParms[2] = parms.shaderParms[3] = parms.shaderParms[6] = 1.0f;
	return parms;
}

// physics after a service at t: bounces and rests counted. A particle that bounces or goes on impact
// and crossed a wall since the last frame, while the trace covered that frame (it traces its last
// 0.1 s once 0.1 s have passed since it moved off), must have been caught. Retail's trace leaves a
// slice untraced - the first trace waits for 0.1 s of motion and then starts 0.1 s back from its
// own time, a frame after the motion began - so a fast particle can cross a wall there and go on
// through it (the collision model's point traces are one-sided); those are counted apart.
static void CheckPhysics( const idBseEffect &effect, float t ) {
	const idBounds room = simRoom.Expand( 0.5f );
	for ( int s = 0; s < effect.segments.Num(); s++ ) {
		const idBseSegment &segment = effect.segments[s];
		const idEffectParticle &tmplP = segment.tmpl->particle;
		if ( !tmplP.hasPhysics ) {
			continue;
		}
		const bool contained = tmplP.bounce != 0.0f || tmplP.remove != 0.0f;
		for ( int i = 0; i < segment.particles.Num(); i++ ) {
			const bseParticle_t &p = segment.particles[i];
			if ( p.motionStartTime == t && p.motionStartTime != p.startTime ) {
				( p.stationary ? rests : bounces )++;
			}
			if ( !contained || t - p.motionStartTime < BSE_PHYSICS_SAMPLE || t >= p.endTime - BSE_TIME_EPSILON ) {
				continue;
			}
			const idVec3 now = effect.ToWorld( effect.EvaluatePosition( p, tmplP, t ) );
			if ( room.ContainsPoint( now ) ) {
				continue;
			}
			const idVec3 from = effect.ToWorld( effect.EvaluatePosition( p, tmplP, p.motionStartTime ) );
			if ( !room.ContainsPoint( from ) ) {
				continue;		// born in the wall: the trace starts in solid
			}
			const idVec3 before = effect.ToWorld( effect.EvaluatePosition( p, tmplP, t - 1.0f / 60.0f ) );
			if ( !room.ContainsPoint( before ) ) {
				gapEscapes++;	// out before this frame: through retail's untraced slice
				continue;
			}
			escapes++;
			if ( escapeNotes.size() < 10 ) {
				escapeNotes.push_back( std::string( "  escaped the room: " ) + effect.decl->GetName() + " segment " + std::to_string( s ) );
			}
		}
	}
}

// the requests of this game frame: the effects go to the game (played once, where asked)
static void DeliverRequests( std::vector<simEffect_t> &world, float t ) {
	maxFrameEffectRequests = Max( maxFrameEffectRequests, frameEffectRequests );
	maxFrameDebrisRequests = Max( maxFrameDebrisRequests, frameDebrisRequests );
	if ( frameEffectRequests + frameDebrisRequests > 0 && frameEffectRequests == maxFrameEffectRequests ) {
		maxRequestEffect = currentEffect;
	}
	frameEffectRequests = frameDebrisRequests = 0;
	for ( const simRequest_t &r : pendingRequests ) {
		if ( r.depth > SIM_MAX_DEPTH ) {
			childrenTooDeep++;
			continue;
		}
		if ( (int)world.size() >= SIM_MAX_LIVE ) {
			worldFull++;
			continue;
		}
		simEffect_t child;
		child.parms = SimParms( r.child, false, r.origin, r.axis, t );
		child.depth = r.depth;
		child.effect = new idBseEffect;
		child.effect->world = NULL;
		child.effect->entityHandle = -1;
		child.effect->litHandle = -1;
		child.effect->Init( child.parms, t );
		world.push_back( child );
		childrenPlayed++;
	}
	pendingRequests.clear();
	maxLive = Max( maxLive, (int)world.size() );
}

static simResult_t Simulate( const idDeclEffect *decl, bool loop, float seconds ) {
	simResult_t r;
	memset( &r, 0, sizeof( r ) );
	const bseEffectParms_t parms = SimParms( decl, loop, vec3_origin, mat3_identity, 0.0f );

	idBseEffect effect;
	effect.world = NULL;
	effect.entityHandle = -1;
	effect.litHandle = -1;
	effect.Init( parms, 0.0f );
	// a viewer off to the side, looking back at the effect
	bseView_t view;
	view.origin.Set( -150.0f, 40.0f, 30.0f );
	view.left.Set( 0.0f, 1.0f, 0.0f );
	view.up.Set( 0.0f, 0.0f, 1.0f );
	idBseGeometry geo;
	std::vector<bool> fed( effect.segments.Num(), false );
	std::vector<simEffect_t> children;
	bool rootOver = false;
	const int lastFrame = (int)( seconds * 60.0f );
	const int stopFrame = loop ? lastFrame : -1;
	const int finalFrame = lastFrame + 65 * 60;		// the stopped run and the children have 65 s to end
	for ( int frame = 1; frame <= finalFrame; frame++ ) {
		const float t = frame / 60.0f;
		if ( frame == stopFrame ) {
			effect.Stop();
		}
		if ( !rootOver ) {
			requestDepth = 0;
			effect.UpdateFromOwner( parms, t );
			const bool over = effect.Service( t );
			CheckPhysics( effect, t );
			for ( int s = 0; s < effect.segments.Num(); s++ ) {
				const idBseSegment &segment = effect.segments[s];
				if ( segment.particles.Num() > segment.budget ) {
					r.overBudget++;
				}
				for ( int i = 0; i < segment.particles.Num(); i++ ) {
					idVec3 pos, size, rotate;
					idVec4 tint;
					if ( !effect.EvaluateParticle( segment.particles[i], segment.tmpl->particle, t, pos, tint, size, rotate ) ) {
						continue;
					}
					if ( !Finite( pos ) || !Finite( size ) || !Finite( rotate ) || !std::isfinite( tint.w ) ) {
						r.notFinite++;
						continue;
					}
					if ( frame > lastFrame ) {
						continue;		// after the stop: only what is left dying out
					}
					const float d = pos.Length();
					r.maxDistance = Max( r.maxDistance, d );
					r.samples++;
					if ( d > 2.0f * effect.boundsRadius + 1.0f ) {
						r.outside++;
					}
				}
				if ( segment.particles.Num() > 0 ) {
					fed[s] = true;
				}
				if ( segment.tmpl->type == EFFECT_SEG_LIGHT && frame % 6 == 0 ) {
					bseLight_t light;
					if ( effect.EvaluateLight( segment, t, light ) ) {
						lightsEvaluated++;
						if ( !Finite( light.origin ) || !Finite( light.color ) || !Finite( light.radius ) ||
								light.radius.x < 1.0f || light.radius.y < 1.0f || light.radius.z < 1.0f ) {
							badLights++;
						}
					}
				}
				if ( segment.tmpl->hasParticles && segment.particles.Num() && frame % 6 == 0 ) {
					geo.Clear();
					effect.BuildSegment( segment, t, view, geo );
					CheckGeometry( geo, segment.tmpl->particle.type, false );
					geo.Clear();
					effect.BuildMotionTrails( segment, t, view, geo );
					CheckGeometry( geo, segment.tmpl->particle.type, true );
				}
			}
			if ( over && ( !loop || frame >= stopFrame ) ) {
				rootOver = true;
				if ( !loop ) {
					r.ended = true;
					r.endTime = t;
				} else {
					r.endedAfterStop = true;
				}
			}
		}

		// the children the game plays: serviced like any effect, freed once over
		for ( size_t c = 0; c < children.size(); c++ ) {
			requestDepth = children[c].depth;
			idBseEffect *child = children[c].effect;
			child->UpdateFromOwner( children[c].parms, t );
			const bool over = child->Service( t );
			CheckPhysics( *child, t );
			for ( int s = 0; s < child->segments.Num(); s++ ) {
				const idBseSegment &segment = child->segments[s];
				for ( int i = 0; i < segment.particles.Num(); i++ ) {
					idVec3 pos, size, rotate;
					idVec4 tint;
					if ( child->EvaluateParticle( segment.particles[i], segment.tmpl->particle, t, pos, tint, size, rotate ) &&
							( !Finite( pos ) || !Finite( size ) || !Finite( rotate ) || !std::isfinite( tint.w ) ) ) {
						r.notFinite++;
					}
				}
			}
			if ( over ) {
				delete child;
				children.erase( children.begin() + c-- );
			}
		}
		DeliverRequests( children, t );

		// a one-shot run is over once it and its children are; a looping one runs to its stop first
		if ( rootOver && children.empty() && pendingRequests.empty() && ( !loop || frame >= stopFrame ) ) {
			break;
		}
		if ( !loop && !rootOver && frame >= lastFrame ) {
			break;		// does not end in time: reported
		}
	}
	childrenNotEnded += (long)children.size();
	for ( simEffect_t &c : children ) {
		delete c.effect;
	}
	if ( !loop ) {
		for ( int s = 0; s < effect.segments.Num(); s++ ) {
			if ( effect.segments[s].tmpl->type == EFFECT_SEG_TRAIL && effect.segments[s].tmpl->enabled ) {
				namedTrails++;
				namedTrailsFed += fed[s] ? 1 : 0;
			}
		}
	}
	return r;
}

int main( int argc, char **argv ) {
	idLib::common = common;
	idLib::Init();
	simRandom.SetSeed( 1234 );		// the cvars are not registered: the runtime uses the retail defaults

	// the tables
	{
		std::ifstream in( argv[1], std::ios::binary );
		std::string name, data;
		while ( ReadEntry( in, name, data ) ) {
			idDeclTable *table = new idDeclTable;
			idDeclBaseStub *base = new idDeclBaseStub( table, DECL_TABLE, name.c_str(), "materials", 1 );
			base->ParseText( data.data(), (int)data.size() );
			tables[Lower( name.c_str() )] = table;
		}
		defaultTable = new idDeclTable;
		idDeclBaseStub *base = new idDeclBaseStub( defaultTable, DECL_TABLE, "_missing", "materials", 1 );
		const char *text = defaultTable->DefaultDefinition();
		base->ParseText( text, (int)strlen( text ) );
	}
	printf( "== %d tables\n", (int)tables.size() );

	int effects = 0, failed = 0, notFinite = 0, notEnded = 0, overBudget = 0, notEndedAfterStop = 0;
	int radiusMatch = 0, radiusWithin10 = 0, radiusOff = 0, withParticles = 0, leaving = 0;
	std::vector<std::string> notes;

	// every decl first, so the effects they name (impact, timeout, nested) are the real ones
	typedef struct {
		std::string		file;
		declText_t		text;
		idDeclEffect *	decl;
		idDeclBaseStub *base;
	} simDecl_t;
	std::vector<std::string> files;
	std::vector<simDecl_t> decls;
	{
		std::string name, data;
		std::vector<std::string> datas;
		while ( ReadEntry( std::cin, name, data ) ) {
			files.push_back( name );
			datas.push_back( data );
		}
		for ( size_t f = 0; f < files.size(); f++ ) {
			std::vector<declText_t> texts;
			SplitDecls( files[f].c_str(), datas[f], texts );
			for ( const declText_t &text : texts ) {
				simDecl_t d;
				d.file = datas[f].substr( text.offset, text.length );
				d.text = text;
				d.decl = new idDeclEffect;
				d.base = new idDeclBaseStub( d.decl, DECL_EFFECT, text.name, files[f].c_str(), text.line );
				effectsByName[Lower( text.name.c_str() )] = d.decl;
				effectBases[Lower( text.name.c_str() )] = d.base;
				decls.push_back( d );
			}
		}
		for ( simDecl_t &d : decls ) {
			d.text.offset = 0;
			if ( !d.base->ParseText( d.file.data(), (int)d.file.size() ) ) {
				d.decl = NULL;
			}
		}
	}

	for ( const simDecl_t &d : decls ) {
		const declText_t &text = d.text;
		idDeclEffect *decl = d.decl;
		{
			if ( decl == NULL ) {
				failed++;
				continue;
			}
			effects++;

			// the retail bounds formula against the file's size
			const float radius = decl->GetBoundsRadius( GRAVITY );
			const float size = decl->GetSize();
			if ( idMath::Fabs( radius - size ) <= 1.0f ) {
				radiusMatch++;
			} else if ( idMath::Fabs( radius - size ) <= Max( 1.0f, size * 0.1f ) ) {
				radiusWithin10++;
			} else {
				radiusOff++;
				if ( radiusOff <= 15 ) {
					notes.push_back( "  radius " + std::to_string( (int)radius ) + " vs size " + std::to_string( (int)size ) + ": " + text.name.c_str() );
				}
			}

			currentEffect = decl->GetName();
			const simResult_t once = Simulate( decl, false, 120.0f );
			const simResult_t looped = Simulate( decl, true, Max( 1.0f, Min( 30.0f, decl->GetMaxDuration() * 3.0f ) ) );
			notFinite += once.notFinite + looped.notFinite;
			if ( !looped.endedAfterStop ) {
				notEndedAfterStop++;
				notes.push_back( std::string( "  does not end within 65 s of its stop: " ) + text.name.c_str() );
			}
			overBudget += once.overBudget + looped.overBudget;
			if ( !once.ended ) {
				notEnded++;
				notes.push_back( std::string( "  does not end in 120 s: " ) + text.name.c_str() );
			}
			if ( once.samples > 0 ) {
				withParticles++;
				if ( once.outside * 20 > once.samples ) {		// over 5 % of the samples
					leaving++;
					if ( leaving <= 15 ) {
						notes.push_back( "  " + std::to_string( (int)( 100 * once.outside / once.samples ) ) + "% beyond twice radius " +
							std::to_string( (int)radius ) + " (furthest " + std::to_string( (int)once.maxDistance ) + "): " + text.name.c_str() );
					}
				}
			}
		}
	}

	// the distance ramp of a retail shake: the rocket's impact (no "scale": 1; attenuation 200,600)
	int badRamp = 0;
	{
		auto it = effectsByName.find( "effects/weapons/rocketlauncher/impact" );
		const idDeclEffect *decl = it != effectsByName.end() ? it->second : NULL;
		const idEffectSegment *shake = NULL;
		for ( int i = 0; decl != NULL && i < decl->GetNumSegments(); i++ ) {
			if ( decl->GetSegment( i ).type == EFFECT_SEG_SHAKE ) {
				shake = &decl->GetSegment( i );
			}
		}
		if ( shake == NULL || shake->scale != 1.0f || shake->attenuation.min != 200.0f || shake->attenuation.max != 600.0f ) {
			badRamp++;
		} else {
			idBseEffect effect;
			effect.world = NULL;
			effect.entityHandle = effect.litHandle = -1;
			effect.Init( SimParms( decl, false, vec3_origin, mat3_identity, 0.0f ), 0.0f );
			const float distances[3] = { 100.0f, 400.0f, 700.0f }, expected[3] = { 1.0f, 0.5f, 0.0f };
			for ( int i = 0; i < 3; i++ ) {
				effect.originDistance = effect.shortestDistance = distances[i];
				if ( idMath::Fabs( effect.OriginAttenuation( *shake ) - expected[i] ) > 1e-4f ||
						idMath::Fabs( effect.SegmentAttenuation( *shake ) - expected[i] ) > 1e-4f ) {
					badRamp++;
				}
			}
		}
	}

	for ( const std::string &n : notes ) {
		printf( "%s\n", n.c_str() );
	}
	for ( const std::string &n : escapeNotes ) {
		printf( "%s\n", n.c_str() );
	}
	printf( "== %d effects simulated (%d failed to parse), %d with particles\n", effects, failed, withParticles );
	printf( "== not finite: %d; non-looping effects that do not end: %d; segments over budget: %d\n", notFinite, notEnded, overBudget );
	printf( "== computed radius vs the file's size: %d equal (within 1), %d within 10%%, %d further off\n", radiusMatch, radiusWithin10, radiusOff );
	printf( "== effects whose particles go beyond twice their radius - past the retail formula's reach (over 5%% of samples): %d of %d\n", leaving, withParticles );
	static const char *kindNames[] = { "sprite", "line", "oriented", "electricity", "debris", "model", "linked", "decal", "light" };
	printf( "== geometry, ten times a second (kind: segments drawn, vertices, most in one segment; motion trails):\n" );
	for ( int k = 0; k <= EFFECT_PARTICLE_LIGHT; k++ ) {
		const kindStats_t &ks = kindStats[k];
		if ( ks.segments || ks.trailSegments ) {
			printf( "   %-12s %8ld %10ld %6d   trails %6ld, most %d   (largest: %s)\n", kindNames[k], ks.segments, ks.verts, ks.maxVerts,
				ks.trailSegments, ks.maxTrailVerts, ks.maxEffect.c_str() );
		}
	}
	printf( "== named trail segments that received particles (once): %d of %d\n", namedTrailsFed, namedTrails );
	printf( "== bad geometry (vertex not finite, index outside, broken triangle): %ld\n", badGeometry );
	printf( "== lights evaluated ten times a second: %ld, bad (not finite, radius under 1): %ld\n", lightsEvaluated, badLights );
	printf( "== sounds started: %ld (emitter updates %ld, stops %ld); decals projected: %ld, bad (or bad sound parms): %ld\n",
		soundsStarted, soundUpdates, soundsStopped, decalsProjected, badDecals );
	printf( "== physics in the room: %ld traces, %ld hits, %ld bounces, %ld came to rest; crossed a wall under a trace: %ld (through the untraced slice, as retail: %ld samples)\n",
		traces, traceHits, bounces, rests, escapes, gapEscapes );
	printf( "== requests to the game: impact effects %ld, timeout effects %ld, nested effects %ld, debris %ld; bad (not finite, no entity def): %ld\n",
		impactRequests, timeoutRequests, nestedRequests, debrisRequests, badRequests );
	printf( "== most in one game frame: %ld effects (limit 256; %s), %ld debris (limit 128)\n",
		maxFrameEffectRequests, maxRequestEffect.c_str(), maxFrameDebrisRequests );
	printf( "== children played back: %ld (deeper than %d: %ld; world full at %d: %ld; most alive in one run: %d; not ended: %ld); names the data lacks: %d\n",
		childrenPlayed, SIM_MAX_DEPTH, childrenTooDeep, SIM_MAX_LIVE, worldFull, maxLive, childrenNotEnded, missingEffects );
	printf( "== looping effects that do not end within 65 s of their stop: %d\n", notEndedAfterStop );
	printf( "== debris entity defs asked for: %d", (int)debrisDefs.size() );
	int shown = 0;
	for ( const auto &d : debrisDefs ) {
		printf( "%s %s x%ld", shown++ ? "," : "", d.first.c_str(), d.second );
		if ( shown >= 8 ) {
			printf( ", ..." );
			break;
		}
	}
	printf( "\n" );
	printf( "== view effects: %ld shakes, %ld double visions, %ld tunnels (strongest %.2f, longest %.2f s); bad: %ld; distance ramp wrong: %d\n",
		viewRequests[0], viewRequests[1], viewRequests[2], maxViewScale, maxViewTime, badViewRequests, badRamp );
	const bool physicsBad = badViewRequests > 0 || badRamp > 0 || escapes > 0 || badRequests > 0 || childrenNotEnded > 0 || worldFull > 0 || notEndedAfterStop > 0;
	return ( failed > 0 || notFinite > 0 || notEnded > 0 || overBudget > 0 || badGeometry > 0 || badLights > 0 || badDecals > 0 || physicsBad ) ? 1 : 0;
}
