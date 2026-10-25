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
	effect.Init( parms, 0.0f );
	const idVec3 left( 0.0f, 1.0f, 0.0f ), up( 0.0f, 0.0f, 1.0f );
	std::vector<idDrawVert> verts;
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
			if ( segment.tmpl->hasParticles && segment.tmpl->particle.type == EFFECT_PARTICLE_SPRITE && segment.particles.Num() ) {
				verts.resize( segment.particles.Num() * 4 );
				idBounds b;
				b.Clear();
				const int quads = effect.BuildSprites( segment, t, left, up, verts.data(), segment.particles.Num(), b );
				for ( int v = 0; v < quads * 4; v++ ) {
					if ( !Finite( verts[v].xyz ) ) {
						r.notFinite++;
					}
				}
			}
		}
		if ( over && !loop ) {
			r.ended = true;
			r.endTime = t;
			break;
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
	return ( failed > 0 || notFinite > 0 || notEnded > 0 || overBudget > 0 ) ? 1 : 0;
}
