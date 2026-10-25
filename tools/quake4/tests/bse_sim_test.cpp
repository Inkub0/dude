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

// the retail tables, by name
static std::map<std::string, idDeclTable *> tables;
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
	const idDeclEffect *	FindEffect( const char *name, bool makeDefault = true ) { return reinterpret_cast<const idDeclEffect *>( standIn ); }
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

// the runtime's engine hooks
static const float GRAVITY = 1066.0f;
static idRandom simRandom;
float BSE_GameGravity( void ) { return GRAVITY; }
bool BSE_ViewOrigin( const idRenderWorld *world, idVec3 &origin ) { return false; }	// no player: full detail
idRandom &BSE_Random( void ) { return simRandom; }

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

static bool ReadEntry( std::istream &in, std::string &name, std::string &data ) {
	std::string lengthLine;
	if ( !std::getline( in, name ) || !std::getline( in, lengthLine ) ) {
		return false;
	}
	data.assign( std::stoul( lengthLine ), '\0' );
	in.read( &data[0], data.size() );
	return true;
}

static bool Finite( const idVec3 &v ) {
	return std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z );
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
} simResult_t;

static simResult_t Simulate( const idDeclEffect *decl, bool loop, float seconds ) {
	simResult_t r;
	memset( &r, 0, sizeof( r ) );
	bseEffectParms_t parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.effect = decl;
	parms.axis.Identity();
	parms.gravity.Set( 0.0f, 0.0f, -GRAVITY );
	parms.attenuation = 1.0f;
	parms.loop = loop;
	parms.inConnectedArea = true;
	parms.shaderParms[0] = parms.shaderParms[1] = parms.shaderParms[2] = parms.shaderParms[3] = parms.shaderParms[6] = 1.0f;

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
	for ( int frame = 1; frame <= (int)( seconds * 60.0f ); frame++ ) {
		const float t = frame / 60.0f;
		effect.UpdateFromOwner( parms, t );
		const bool over = effect.Service( t );
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
			if ( segment.tmpl->hasParticles && segment.particles.Num() && frame % 6 == 0 ) {
				geo.Clear();
				effect.BuildSegment( segment, t, view, geo );
				CheckGeometry( geo, segment.tmpl->particle.type, false );
				geo.Clear();
				effect.BuildMotionTrails( segment, t, view, geo );
				CheckGeometry( geo, segment.tmpl->particle.type, true );
			}
		}
		if ( over && !loop ) {
			r.ended = true;
			r.endTime = t;
			break;
		}
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

	int effects = 0, failed = 0, notFinite = 0, notEnded = 0, overBudget = 0;
	int radiusMatch = 0, radiusWithin10 = 0, radiusOff = 0, withParticles = 0, leaving = 0;
	std::vector<std::string> notes;
	std::string name, data;
	while ( ReadEntry( std::cin, name, data ) ) {
		std::vector<declText_t> texts;
		SplitDecls( name.c_str(), data, texts );
		for ( const declText_t &text : texts ) {
			idDeclEffect *decl = new idDeclEffect;
			idDeclBaseStub *base = new idDeclBaseStub( decl, DECL_EFFECT, text.name, name.c_str(), text.line );
			if ( !base->ParseText( data.data() + text.offset, text.length ) ) {
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

	for ( const std::string &n : notes ) {
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
	return ( failed > 0 || notFinite > 0 || notEnded > 0 || overBudget > 0 || badGeometry > 0 ) ? 1 : 0;
}
