/*
	Offline test for the engine's effect decl parser (neo/renderer/bse/DeclEffect.cpp, Quake 4's
	BSE effects).

	Compiles the parser's own source into this program with the real idLexer (libidlib.a). The
	decl and model managers it asks for tables, materials, sound shaders, effects and models are
	stand-ins that record each name and answer a pointer the parser only stores. Files arrive on
	stdin as "name\nlength\n" followed by the bytes; each is split into decls the way
	idDeclManagerLocal::ParseFile does (a type word, or the effects folder's default type, a
	name, a braced body) and every decl is parsed as idDeclLocal::ParseLocal would.

	Prints, from the parsed structures (not the text): the failures, then COUNT / SUM lines - how
	many of each segment, particle, keyword, parameter, domain and modifier were stored, how many
	numbers and their sum - which test_effect_parse.sh compares with an independent census of the
	text (q4_effect_census.py), then REF lines (every name looked up) and TRAIL lines (trails
	naming no trail segment). Before the retail files it runs control effects: broken ones that
	must fail and valid ones whose stored values are checked.

	Built and run by tools/quake4/test_effect_parse.sh. No game, no window, no GPU.
*/

#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sys/platform.h"
#include "idlib/Lexer.h"
#include "framework/Common.h"
#include "framework/DeclManager.h"
#include "framework/DeclTable.h"
#include "renderer/ModelManager.h"
#include "renderer/bse/DeclEffect.h"

#include "common_stub.h"
#include "decl_stub.h"
#include "effect_decls.h"

// every name the parser looks up, by kind
static std::map<std::string, std::set<std::string>> references;
alignas( 64 ) static unsigned char standIn[256];

static void Reference( const char *kind, const char *name ) {
	idStr canonical = name;
	canonical.ToLower();
	canonical.BackSlashesToSlashes();
	references[kind].insert( canonical.c_str() );
}

static struct {
	const idDecl *			FindType( declType_t type, const char *name, bool makeDefault = true ) {
								Reference( type == DECL_TABLE ? "table" : "decl", name );
								return reinterpret_cast<const idDecl *>( standIn );
							}
	const idMaterial *		FindMaterial( const char *name, bool makeDefault = true ) {
								Reference( "material", name );
								return reinterpret_cast<const idMaterial *>( standIn );
							}
	const idSoundShader *	FindSound( const char *name, bool makeDefault = true ) {
								Reference( "sound", name );
								return reinterpret_cast<const idSoundShader *>( standIn );
							}
	const idDeclEffect *	FindEffect( const char *name, bool makeDefault = true ) {
								Reference( "effect", name );
								return reinterpret_cast<const idDeclEffect *>( standIn );
							}
} declManagerStub;

static struct {
	idRenderModel *			FindModel( const char *name ) {
								Reference( "model", name );
								return reinterpret_cast<idRenderModel *>( standIn );
							}
} renderModelManagerStub;

// the parser under test, with the managers above
#define declManager			( &declManagerStub )
#define renderModelManager	( &renderModelManagerStub )
#include "renderer/bse/DeclEffect.cpp"
#undef declManager
#undef renderModelManager

/*
===============================================================================

	counts from the parsed structures

===============================================================================
*/

static std::map<std::string, long long> counts;
static std::map<std::string, double> sums;

static std::string Lower( const char *s ) {
	std::string out( s );
	for ( char &c : out ) {
		c = (char)tolower( (unsigned char)c );
	}
	return out;
}

static void Count( const std::string &key, long long n = 1 ) {
	if ( n != 0 ) {
		counts[Lower( key.c_str() )] += n;
	}
}

// numbers stored for a statement: how many and their sum
static void Numbers( const char *group, const float *values, int num ) {
	for ( int i = 0; i < num; i++ ) {
		counts[std::string( group ) + ".numbers"]++;
		sums[std::string( group ) + ".sum"] += values[i];
	}
}

static void RangeNumbers( const effectRange_t &range ) {
	const float values[2] = { range.min, range.max };
	Numbers( "values", values, 2 );
}

static void ScalarNumber( float value ) {
	Numbers( "values", &value, 1 );
}

static void CountDomain( const char *stage, int parm, const effectDomain_t &domain ) {
	if ( domain.type == EFFECT_DOMAIN_NONE ) {
		return;
	}
	Count( std::string( "stage." ) + stage + "." + effectParmNames[parm] );
	Count( std::string( "domain." ) + effectDomainNames[domain.type] );
	for ( int f = 0; f < EFFECT_DOMAIN_NUM_FLAGS; f++ ) {
		if ( domain.flags & BIT( f ) ) {
			Count( std::string( "domain.flag." ) + effectDomainFlagNames[f] );
		}
	}
	Numbers( "domain", domain.values, domain.numValues );
}

static void CountEnvelope( int parm, int components, const effectEnvelope_t &envelope ) {
	if ( envelope.table == NULL ) {
		return;
	}
	Count( std::string( "stage.motion." ) + effectParmNames[parm] );
	if ( envelope.flags & EFFECT_ENVELOPE_COUNT ) {
		Count( "envelope.count" );
		Numbers( "envelope", envelope.count, components );
	}
	if ( envelope.flags & EFFECT_ENVELOPE_OFFSET ) {
		Count( "envelope.offset" );
		Numbers( "envelope", envelope.offset, components );
	}
	if ( envelope.flags & EFFECT_ENVELOPE_RATE ) {
		Count( "envelope.rate" );
		Numbers( "envelope", envelope.rate, components );
	}
}

static const struct { int key; const char *name; } segmentKeys[] = {
	{ EFFECT_SKEY_START, "start" }, { EFFECT_SKEY_DURATION, "duration" }, { EFFECT_SKEY_COUNT, "count" },
	{ EFFECT_SKEY_DENSITY, "density" }, { EFFECT_SKEY_ATTENUATION, "attenuation" }, { EFFECT_SKEY_VOLUME, "volume" },
	{ EFFECT_SKEY_FREQSHIFT, "freqShift" }, { EFFECT_SKEY_DETAIL, "detail" }, { EFFECT_SKEY_SCALE, "scale" },
	{ EFFECT_SKEY_PARTICLE_CAP, "particleCap" }, { EFFECT_SKEY_SOUND_SHADER, "soundShader" }, { EFFECT_SKEY_EFFECT, "effect" }
};

static const struct { int key; const char *name; } particleKeys[] = {
	{ EFFECT_PKEY_DURATION, "duration" }, { EFFECT_PKEY_MATERIAL, "material" }, { EFFECT_PKEY_GRAVITY, "gravity" },
	{ EFFECT_PKEY_ENTITYDEF, "entityDef" }, { EFFECT_PKEY_MODEL, "model" }, { EFFECT_PKEY_FORK, "fork" },
	{ EFFECT_PKEY_JITTER_RATE, "jitterRate" }, { EFFECT_PKEY_JITTER_SIZE, "jitterSize" }, { EFFECT_PKEY_JITTER_TABLE, "jitterTable" },
	{ EFFECT_PKEY_TRAIL_TYPE, "trailType" }, { EFFECT_PKEY_TRAIL_TIME, "trailTime" }, { EFFECT_PKEY_TRAIL_COUNT, "trailCount" },
	{ EFFECT_PKEY_TRAIL_MATERIAL, "trailMaterial" }, { EFFECT_PKEY_TILING, "tiling" }, { EFFECT_PKEY_IMPACT, "impact" },
	{ EFFECT_PKEY_TIMEOUT, "timeout" }, { EFFECT_PKEY_BOUNCE, "bounce" }, { EFFECT_PKEY_REMOVE, "remove" }
};

static void CountParticle( const idEffectParticle &p ) {
	Count( std::string( "particle." ) + effectParticleNames[p.type] );
	for ( const auto &k : particleKeys ) {
		if ( p.keys & k.key ) {
			Count( std::string( "particle.key." ) + k.name );
		}
	}
	if ( p.flags & EFFECT_PARTICLE_BLEND_ADD ) {
		Count( "particle.flag.blend" );
	}
	for ( int f = 1; f < EFFECT_PARTICLE_NUM_FLAGS; f++ ) {
		if ( p.flags & BIT( f ) ) {
			Count( std::string( "particle.flag." ) + effectParticleFlagNames[f] );
		}
	}
	static const char *const trailNames[] = { "", "motion", "burn", "segment" };
	if ( p.trailType != EFFECT_TRAIL_NONE ) {
		Count( std::string( "particle.trail." ) + trailNames[p.trailType] );
	}
	Count( "particle.impact.effect", p.impactEffects.Num() );
	Count( "particle.timeout.effect", p.timeoutEffects.Num() );

	// numbers, in the order the keys come in particleKeys (the sum does not depend on it)
	if ( p.keys & EFFECT_PKEY_DURATION ) RangeNumbers( p.duration );
	if ( p.keys & EFFECT_PKEY_GRAVITY ) RangeNumbers( p.gravity );
	if ( p.keys & EFFECT_PKEY_FORK ) ScalarNumber( (float)p.fork );
	if ( p.keys & EFFECT_PKEY_JITTER_RATE ) ScalarNumber( p.jitterRate );
	if ( p.keys & EFFECT_PKEY_JITTER_SIZE ) Numbers( "values", p.jitterSize.ToFloatPtr(), 3 );
	if ( p.keys & EFFECT_PKEY_TRAIL_TIME ) RangeNumbers( p.trailTime );
	if ( p.keys & EFFECT_PKEY_TRAIL_COUNT ) RangeNumbers( p.trailCount );
	if ( p.keys & EFFECT_PKEY_TILING ) ScalarNumber( p.tiling );
	if ( p.keys & EFFECT_PKEY_BOUNCE ) ScalarNumber( p.bounce );
	if ( p.keys & EFFECT_PKEY_REMOVE ) ScalarNumber( p.remove );
	if ( p.keys & EFFECT_PKEY_ENTITYDEF ) Reference( "entitydef", p.entityDef );

	for ( int parm = 0; parm < EFFECT_PARM_NUM; parm++ ) {
		CountDomain( "start", parm, p.parms[parm].start );
		CountEnvelope( parm, EffectParmComponents( p.type, parm ), p.parms[parm].envelope );
		CountDomain( "end", parm, p.parms[parm].end );
	}
}

static std::vector<std::string> danglingTrails;

static void CountEffect( const idDeclEffect &effect ) {
	Count( "decls" );
	if ( effect.GetKeys() & EFFECT_KEY_SIZE ) {
		Count( "effect.size" );
		ScalarNumber( effect.GetSize() );
	}
	if ( effect.GetKeys() & EFFECT_KEY_CUTOFF_DISTANCE ) {
		Count( "effect.cutOffDistance" );
		ScalarNumber( effect.GetCutOffDistance() );
	}
	for ( int i = 0; i < effect.GetNumSegments(); i++ ) {
		const idEffectSegment &s = effect.GetSegment( i );
		Count( std::string( "segment." ) + effectSegmentNames[s.type] );
		for ( const auto &k : segmentKeys ) {
			if ( s.keys & k.key ) {
				Count( std::string( "segment.key." ) + k.name );
			}
		}
		for ( int f = 0; f < EFFECT_SEGMENT_NUM_FLAGS; f++ ) {
			if ( s.flags & BIT( f ) ) {
				Count( std::string( "segment.flag." ) + effectSegmentFlagNames[f] );
			}
		}
		if ( s.keys & EFFECT_SKEY_START ) RangeNumbers( s.start );
		if ( s.keys & EFFECT_SKEY_DURATION ) RangeNumbers( s.duration );
		if ( s.keys & EFFECT_SKEY_COUNT ) RangeNumbers( s.count );
		if ( s.keys & EFFECT_SKEY_DENSITY ) RangeNumbers( s.density );
		if ( s.keys & EFFECT_SKEY_ATTENUATION ) RangeNumbers( s.attenuation );
		if ( s.keys & EFFECT_SKEY_VOLUME ) RangeNumbers( s.volume );
		if ( s.keys & EFFECT_SKEY_FREQSHIFT ) RangeNumbers( s.freqShift );
		if ( s.keys & EFFECT_SKEY_DETAIL ) ScalarNumber( s.detail );
		if ( s.keys & EFFECT_SKEY_SCALE ) ScalarNumber( s.scale );
		if ( s.keys & EFFECT_SKEY_PARTICLE_CAP ) ScalarNumber( (float)s.particleCap );

		if ( s.particle.type != EFFECT_PARTICLE_NONE ) {
			CountParticle( s.particle );
			if ( s.particle.trailType == EFFECT_TRAIL_SEGMENT && s.particle.trailSegment < 0 ) {
				danglingTrails.push_back( std::string( effect.GetName() ) + " " + s.name.c_str() + " " + s.particle.trailName.c_str() );
			}
		}
	}
}

/*
===============================================================================

	decls

===============================================================================
*/

// parses one decl's text through its stub manager state; true when it parsed without defaulting
static bool ParseEffect( idDeclBaseStub &base, const char *text, int length, int &warnings ) {
	const int warningsBefore = stubWarnings;
	const bool ok = base.ParseText( text, length );
	warnings = stubWarnings - warningsBefore;
	return ok;
}

/*
===============================================================================

	controls

===============================================================================
*/

typedef struct {
	const char *	label;
	bool			parses;
	const char *	text;
} control_t;

static const control_t controls[] = {
	{ "unknown effect keyword", false, "effect c { size 1 colour 1 }" },
	{ "unknown segment keyword", false, "effect c { size 1 emitter \"e\" { counts 1,1 } }" },
	{ "unknown particle keyword", false, "effect c { emitter \"e\" { sprite { lifetime 1,1 } } }" },
	{ "unknown parameter", false, "effect c { emitter \"e\" { sprite { start { colour { point 1,1,1 } } } } }" },
	{ "unknown domain", false, "effect c { emitter \"e\" { sprite { start { size { disc 1,1 } } } } }" },
	{ "unknown domain modifier", false, "effect c { emitter \"e\" { sprite { start { size { point 1,1 outward } } } } }" },
	{ "sprite size with 3 values", false, "effect c { emitter \"e\" { sprite { start { size { point 1,1,1 } } } } }" },
	{ "line size box with 4 values", false, "effect c { emitter \"e\" { line { start { size { box 1,1,2,2 } } } } }" },
	{ "sphere for a 2-component size", false, "effect c { emitter \"e\" { sprite { start { size { sphere 1,1,1,2,2,2 } } } } }" },
	{ "segment range with 1 value", false, "effect c { emitter \"e\" { duration 1 } }" },
	{ "segment range with 3 values", false, "effect c { emitter \"e\" { count 1,2,3 } }" },
	{ "envelope in a start block", false, "effect c { emitter \"e\" { sprite { start { size { envelope linear } } } } }" },
	{ "domain in a motion block", false, "effect c { emitter \"e\" { sprite { motion { size { point 1,1 } } } } }" },
	{ "envelope count of the wrong length", false, "effect c { emitter \"e\" { sprite { motion { size { envelope linear count 1 } } } } }" },
	{ "unknown envelope keyword", false, "effect c { emitter \"e\" { sprite { motion { fade { envelope linear speed 1 } } } } }" },
	{ "a parameter twice in one block", false, "effect c { emitter \"e\" { sprite { start { fade { point 1 } fade { point 0 } } } } }" },
	{ "two particle blocks", false, "effect c { emitter \"e\" { sprite { } line { } } }" },
	{ "particles in a sound segment", false, "effect c { sound \"s\" { sprite { } } }" },
	{ "a keyword twice in a segment", false, "effect c { emitter \"e\" { count 1,1 count 2,2 } }" },
	{ "unknown blend", false, "effect c { emitter \"e\" { sprite { blend alpha } } }" },
	{ "unknown impact keyword", false, "effect c { emitter \"e\" { sprite { impact { stick 1 } } } }" },
	{ "bounce twice in an impact", false, "effect c { emitter \"e\" { sprite { impact { bounce 1 bounce 0 } } } }" },
	{ "a number for a keyword", false, "effect c { emitter \"e\" { 5 } }" },
	{ "a word for a number", false, "effect c { emitter \"e\" { count one,two } }" },
	{ "a minus without a number", false, "effect c { emitter \"e\" { count -,1 } }" },
	{ "segment not closed", false, "effect c { emitter \"e\" { count 1,1 " },
	{ "effect not closed", false, "effect c { size 4" },
	{ "domain not closed", false, "effect c { emitter \"e\" { sprite { start { fade { point 1 " },
	{ "a valid effect", true, NULL },		// checked value by value below
};

static const char *validControl =
	"effect controls/valid\n"
	"{\n"
	"	size	12.5\n"
	"	cutOffDistance	2048\n"
	"	spawner {\n"							// no name
	"		count		3,4\n"
	"		locked\n"
	"		inverseAttenuateEmitter\n"
	"		sprite {\n"
	"			duration	0.5,1\n"
	"			blend	add\n"
	"			trailType	\"sparks\"\n"
	"			start {\n"
	"				position { spiral -1,-2,-3,1,2,3,0.5 useEndOrigin linearSpacing }\n"
	"				size { point 2,3 }\n"
	"				tint { line 0,0,0,1,1,1 inverseAttenuate }\n"
	"			}\n"
	"			motion {\n"
	"				size { envelope exp_1minusx/x2 count 2,2 offset 0.5,0.25 rate 1,-1 }\n"
	"			}\n"
	"			end {\n"
	"				size { box 4,5,6,7 }\n"
	"			}\n"
	"			impact { bounce 0.5 remove 1 effect \"effects/a\" effect effects/b }\n"
	"			timeout { effect \"effects/c\" }\n"
	"		}\n"
	"	}\n"
	"	trail \"sparks\" { count 1,1 sprite { } }\n"
	"	tunnel \"t\" { duration 1,2 scale 0.5 }\n"
	"}\n";

static bool CheckValidControl( const idDeclEffect &e ) {
	bool ok = true;
	auto check = [&ok]( bool condition, const char *what ) {
		if ( !condition ) {
			printf( "  control 'a valid effect': %s\n", what );
			ok = false;
		}
	};
	check( e.GetSize() == 12.5f && e.GetCutOffDistance() == 2048.0f, "size / cutOffDistance" );
	check( e.GetNumSegments() == 3, "3 segments" );
	if ( e.GetNumSegments() != 3 ) {
		return false;
	}
	const idEffectSegment &s = e.GetSegment( 0 );
	check( s.type == EFFECT_SEG_SPAWNER && s.name.Length() == 0, "unnamed spawner" );
	check( s.count.min == 3.0f && s.count.max == 4.0f && s.keys == EFFECT_SKEY_COUNT, "count 3,4" );
	check( s.flags == ( EFFECT_SEGMENT_LOCKED | EFFECT_SEGMENT_INVERSE_ATTENUATE_EMITTER ), "segment flags" );
	const idEffectParticle &p = s.particle;
	check( p.type == EFFECT_PARTICLE_SPRITE && p.flags == EFFECT_PARTICLE_BLEND_ADD, "additive sprite" );
	check( p.duration.min == 0.5f && p.duration.max == 1.0f, "duration" );
	check( p.trailType == EFFECT_TRAIL_SEGMENT && p.trailSegment == 1, "trail resolved to segment 1" );
	const effectDomain_t &position = p.parms[EFFECT_PARM_POSITION].start;
	check( position.type == EFFECT_DOMAIN_SPIRAL && position.numValues == 7 && position.values[0] == -1.0f &&
		position.values[2] == -3.0f && position.values[6] == 0.5f, "spiral values" );
	check( position.flags == ( EFFECT_DOMAIN_USE_END_ORIGIN | EFFECT_DOMAIN_LINEAR_SPACING ), "spiral modifiers" );
	check( p.parms[EFFECT_PARM_TINT].start.flags == EFFECT_DOMAIN_INVERSE_ATTENUATE, "inverseAttenuate" );
	const effectEnvelope_t &envelope = p.parms[EFFECT_PARM_SIZE].envelope;
	check( envelope.table != NULL && envelope.flags == ( EFFECT_ENVELOPE_COUNT | EFFECT_ENVELOPE_OFFSET | EFFECT_ENVELOPE_RATE ), "envelope lists" );
	check( envelope.offset[1] == 0.25f && envelope.rate[1] == -1.0f, "envelope values" );
	check( p.parms[EFFECT_PARM_SIZE].end.type == EFFECT_DOMAIN_BOX && p.parms[EFFECT_PARM_SIZE].end.values[3] == 7.0f, "end box" );
	check( p.bounce == 0.5f && p.remove == 1.0f && p.impactEffects.Num() == 2 && p.timeoutEffects.Num() == 1, "impact / timeout" );
	check( p.parms[EFFECT_PARM_FADE].start.type == EFFECT_DOMAIN_NONE && p.parms[EFFECT_PARM_FADE].envelope.table == NULL, "fade not given" );
	check( e.GetSegment( 1 ).type == EFFECT_SEG_TRAIL && e.GetSegment( 1 ).particle.type == EFFECT_PARTICLE_SPRITE, "trail segment" );
	check( e.GetSegment( 2 ).type == EFFECT_SEG_TUNNEL && e.GetSegment( 2 ).scale == 0.5f, "tunnel segment" );
	return ok;
}

static int RunControls( void ) {
	int bad = 0;
	for ( const control_t &c : controls ) {
		const char *text = c.text != NULL ? c.text : validControl;
		idDeclEffect effect;
		idDeclBaseStub base( &effect, DECL_EFFECT, "controls/c", "controls", 1 );
		int warnings = 0;
		const bool parsed = ParseEffect( base, text, (int)strlen( text ), warnings );
		bool ok = ( parsed == c.parses );
		if ( !c.parses ) {
			// a failed decl is the default effect: nothing to play
			ok = ok && warnings > 0 && effect.GetNumSegments() == 0 && effect.GetSize() == 0.0f;
		} else {
			ok = ok && warnings == 0 && CheckValidControl( effect );
		}
		if ( !ok ) {
			printf( "  CONTROL FAILED: '%s' (parsed %d, %d warnings)\n", c.label, parsed, warnings );
			bad++;
		}
	}
	printf( "== controls: %d of %d as expected\n", (int)( sizeof( controls ) / sizeof( controls[0] ) ) - bad, (int)( sizeof( controls ) / sizeof( controls[0] ) ) );
	return bad;
}

/*
===============================================================================

	main

===============================================================================
*/

int main( int argc, char **argv ) {
	idLib::common = common;
	idLib::Init();

	fprintf( stderr, "-- control warnings (expected) --\n" );
	const int badControls = RunControls();
	references.clear();
	danglingTrails.clear();
	fprintf( stderr, "-- retail warnings --\n" );

	int files = 0, decls = 0, failed = 0, parseWarnings = 0, fileWarnings = 0;
	std::vector<std::string> failures;
	std::string name, lengthLine;
	while ( std::getline( std::cin, name ) && std::getline( std::cin, lengthLine ) ) {
		const size_t length = std::stoul( lengthLine );
		std::string data( length, '\0' );
		std::cin.read( &data[0], length );
		files++;

		std::vector<declText_t> texts;
		const int warnings = SplitDecls( name.c_str(), data, texts );
		if ( warnings > 0 ) {
			printf( "FILEWARN %s %d\n", name.c_str(), warnings );
			fileWarnings += warnings;
		}
		for ( const declText_t &t : texts ) {
			idDeclEffect effect;
			idDeclBaseStub base( &effect, DECL_EFFECT, t.name, name.c_str(), t.line );
			int declWarnings = 0;
			decls++;
			if ( !ParseEffect( base, data.data() + t.offset, t.length, declWarnings ) ) {
				failed++;
				failures.push_back( name + ":" + std::to_string( t.line ) + " " + t.name.c_str() );
			} else {
				CountEffect( effect );
			}
			parseWarnings += declWarnings;
		}
	}

	for ( const std::string &f : failures ) {
		printf( "FAIL %s\n", f.c_str() );
	}
	for ( const auto &c : counts ) {
		printf( "COUNT %s %lld\n", c.first.c_str(), c.second );
	}
	for ( const auto &s : sums ) {
		printf( "SUM %s %.6f\n", s.first.c_str(), s.second );
	}
	for ( const auto &r : references ) {
		for ( const std::string &n : r.second ) {
			printf( "REF %s %s\n", r.first.c_str(), n.c_str() );
		}
	}
	for ( const std::string &t : danglingTrails ) {
		printf( "TRAIL %s\n", t.c_str() );
	}
	printf( "== retail: %d files, %d decls, %d failed, %d parse warnings, %d decl-manager warnings\n",
		files, decls, failed, parseWarnings, fileWarnings );

	return ( failed > 0 || parseWarnings > 0 || badControls > 0 ) ? 1 : 0;
}
