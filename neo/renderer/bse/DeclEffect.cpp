/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: the effect decl's parser (Quake 4's BSE) - see DeclEffect.h and docs/quake4-bse.md.
//
// Strict by design: an unknown keyword, a value list of the wrong length for its parameter or a
// block that does not close fails the decl (it defaults to an effect without segments, and
// lintDecls lists it). Every retail effect follows these rules exactly
// (tools/quake4/test_effect_parse.sh), so a failure means a file this grammar does not know.

#include "sys/platform.h"
#include "idlib/Lexer.h"
#include "framework/Common.h"
#include "framework/DeclManager.h"
#include "framework/DeclTable.h"
#include "renderer/ModelManager.h"
#include "renderer/bse/DeclEffect.h"

/*
===============================================================================

	keywords

===============================================================================
*/

static const char *const effectSegmentNames[EFFECT_SEG_NUM] = {
	"emitter", "spawner", "trail", "decal", "light", "sound", "effect", "shake", "doubleVision", "tunnel", "delay"
};

static const char *const effectParticleNames[EFFECT_PARTICLE_NUM] = {
	"sprite", "line", "oriented", "electricity", "debris", "model", "linked", "decal", "light"
};

static const char *const effectParmNames[EFFECT_PARM_NUM] = {
	"position", "velocity", "acceleration", "friction", "size", "length", "tint", "fade", "rotate", "angle", "offset"
};

static const char *const effectDomainNames[EFFECT_DOMAIN_NUM] = {
	"", "point", "line", "box", "sphere", "cylinder", "spiral"
};

static const char *const effectDomainFlagNames[EFFECT_DOMAIN_NUM_FLAGS] = {
	"surface", "relative", "cone", "useEndOrigin", "linearSpacing", "attenuate", "inverseAttenuate"
};

static const char *const effectSegmentFlagNames[EFFECT_SEGMENT_NUM_FLAGS] = {
	"locked", "constant", "attenuateEmitter", "inverseAttenuateEmitter"
};

// particle flags given by a bare keyword (EFFECT_PARTICLE_BLEND_ADD is "blend add")
static const char *const effectParticleFlagNames[EFFECT_PARTICLE_NUM_FLAGS] = {
	NULL, "persist", "generatedNormal", "generatedOriginNormal", "flipNormal", "generatedLine", "shadows", "specular"
};

// the stage blocks of a particle
enum {
	STAGE_START,
	STAGE_MOTION,
	STAGE_END
};

static int FindEffectKeyword( const idToken &token, const char *const *names, int numNames ) {
	for ( int i = 0; i < numNames; i++ ) {
		if ( names[i] != NULL && names[i][0] != '\0' && !token.Icmp( names[i] ) ) {
			return i;
		}
	}
	return -1;
}

/*
================
EffectParmComponents

How many components a parameter has for a particle type. The retail files give every value list
with exactly this many numbers per point; the pairs no retail particle uses follow the same rules.
================
*/
static int EffectParmComponents( effectParticleType_t type, int parm ) {
	switch ( parm ) {
		case EFFECT_PARM_FADE:
			return 1;
		case EFFECT_PARM_SIZE:
			switch ( type ) {
				case EFFECT_PARTICLE_LINE:
				case EFFECT_PARTICLE_ELECTRICITY:
				case EFFECT_PARTICLE_LINKED:
					return 1;		// a width
				case EFFECT_PARTICLE_LIGHT:
				case EFFECT_PARTICLE_MODEL:
				case EFFECT_PARTICLE_DEBRIS:
					return 3;		// a radius or a scale per axis
				default:
					return 2;		// a quad's width and height
			}
		case EFFECT_PARM_ROTATE:
			switch ( type ) {
				case EFFECT_PARTICLE_ORIENTED:
				case EFFECT_PARTICLE_MODEL:
				case EFFECT_PARTICLE_DEBRIS:
					return 3;		// angles
				default:
					return 1;		// a roll in the quad's plane
			}
		default:
			return 3;
	}
}

// the numbers a domain takes for a parameter of that many components, -1 when it cannot
static int EffectDomainValues( int domain, int components ) {
	switch ( domain ) {
		case EFFECT_DOMAIN_POINT:
			return components;
		case EFFECT_DOMAIN_LINE:
		case EFFECT_DOMAIN_BOX:
			return components * 2;
		case EFFECT_DOMAIN_SPHERE:
		case EFFECT_DOMAIN_CYLINDER:
			return components == 3 ? 6 : -1;
		case EFFECT_DOMAIN_SPIRAL:
			return components == 3 ? 7 : -1;
		default:
			return -1;
	}
}

static bool EffectSegmentHasParticles( effectSegmentType_t type ) {
	switch ( type ) {
		case EFFECT_SEG_EMITTER:
		case EFFECT_SEG_SPAWNER:
		case EFFECT_SEG_TRAIL:
		case EFFECT_SEG_DECAL:
		case EFFECT_SEG_LIGHT:
			return true;
		default:
			return false;
	}
}

/*
===============================================================================

	values

===============================================================================
*/

// "a,b,c": up to maxValues comma separated numbers
static bool ParseEffectFloats( idLexer &src, float *values, int maxValues, int &numValues ) {
	numValues = 0;
	do {
		if ( numValues >= maxValues ) {
			src.Warning( "more than %d values", maxValues );
			return false;
		}
		bool error;
		values[numValues++] = src.ParseFloat( &error );
		if ( error || src.HadError() ) {
			return false;
		}
	} while ( src.CheckTokenString( "," ) );
	return true;
}

// exactly count numbers
static bool ParseEffectValues( idLexer &src, const char *keyword, float *values, int count ) {
	float read[EFFECT_MAX_DOMAIN_VALUES];
	int numRead;
	if ( !ParseEffectFloats( src, read, EFFECT_MAX_DOMAIN_VALUES, numRead ) ) {
		return false;
	}
	if ( numRead != count ) {
		src.Warning( "'%s' takes %d value%s, found %d", keyword, count, count == 1 ? "" : "s", numRead );
		return false;
	}
	for ( int i = 0; i < count; i++ ) {
		values[i] = read[i];
	}
	return true;
}

static bool ParseEffectRange( idLexer &src, const char *keyword, effectRange_t &range ) {
	float values[2];
	if ( !ParseEffectValues( src, keyword, values, 2 ) ) {
		return false;
	}
	range.min = values[0];
	range.max = values[1];
	return true;
}

static bool ParseEffectScalar( idLexer &src, const char *keyword, float &value ) {
	return ParseEffectValues( src, keyword, &value, 1 );
}

// a decl, material, model or sound name: a word or a quoted string
static bool ParseEffectName( idLexer &src, const char *keyword, idToken &name ) {
	if ( !src.ReadToken( &name ) || name == "{" || name == "}" ) {
		src.Warning( "'%s' needs a name", keyword );
		return false;
	}
	return true;
}

/*
===============================================================================

	particle parameters

===============================================================================
*/

/*
================
ParseEffectDomain

"{ box 0,0,0,8,8,8 relative }": a start or end value.
================
*/
static bool ParseEffectDomain( idLexer &src, const char *parmName, effectDomain_t &domain, int components ) {
	idToken token;

	if ( domain.type != EFFECT_DOMAIN_NONE ) {
		src.Warning( "'%s' is given twice", parmName );
		return false;
	}
	if ( !src.ExpectTokenString( "{" ) || !src.ReadToken( &token ) ) {
		return false;
	}
	const int type = FindEffectKeyword( token, effectDomainNames, EFFECT_DOMAIN_NUM );
	if ( type < 0 ) {
		src.Warning( "'%s' expects a point, line, box, sphere, cylinder or spiral, found '%s'", parmName, token.c_str() );
		return false;
	}
	const int expected = EffectDomainValues( type, components );
	if ( expected < 0 ) {
		src.Warning( "'%s' has %d component%s, a %s needs 3", parmName, components, components == 1 ? "" : "s", effectDomainNames[type] );
		return false;
	}
	domain.type = (effectDomainType_t)type;
	if ( !ParseEffectFloats( src, domain.values, EFFECT_MAX_DOMAIN_VALUES, domain.numValues ) ) {
		return false;
	}
	if ( domain.numValues != expected ) {
		src.Warning( "'%s' %s takes %d values, found %d", parmName, effectDomainNames[type], expected, domain.numValues );
		return false;
	}

	while ( src.ReadToken( &token ) ) {
		if ( token == "}" ) {
			return true;
		}
		const int flag = FindEffectKeyword( token, effectDomainFlagNames, EFFECT_DOMAIN_NUM_FLAGS );
		if ( flag < 0 ) {
			src.Warning( "'%s' has an unknown domain modifier '%s'", parmName, token.c_str() );
			return false;
		}
		domain.flags |= BIT( flag );
	}
	src.Warning( "'%s' is not closed", parmName );
	return false;
}

/*
================
ParseEffectEnvelope

"{ envelope linear count 2,2 offset 0,0 rate 1,1 }": how a value moves from its start to its
end. The envelope is a table decl; count, offset and rate give one number per component.
================
*/
static bool ParseEffectEnvelope( idLexer &src, const char *parmName, effectEnvelope_t &envelope, int components ) {
	idToken token;

	if ( envelope.table != NULL ) {
		src.Warning( "'%s' has two envelopes", parmName );
		return false;
	}
	if ( !src.ExpectTokenString( "{" ) || !src.ReadToken( &token ) ) {
		return false;
	}
	if ( token.Icmp( "envelope" ) ) {
		src.Warning( "'%s' in a motion block expects an envelope, found '%s'", parmName, token.c_str() );
		return false;
	}
	if ( !ParseEffectName( src, "envelope", token ) ) {
		return false;
	}
	envelope.table = static_cast<const idDeclTable *>( declManager->FindType( DECL_TABLE, token, true ) );

	while ( src.ReadToken( &token ) ) {
		if ( token == "}" ) {
			return true;
		}
		float *list;
		int flag;
		if ( !token.Icmp( "count" ) ) {
			list = envelope.count;
			flag = EFFECT_ENVELOPE_COUNT;
		} else if ( !token.Icmp( "offset" ) ) {
			list = envelope.offset;
			flag = EFFECT_ENVELOPE_OFFSET;
		} else if ( !token.Icmp( "rate" ) ) {
			list = envelope.rate;
			flag = EFFECT_ENVELOPE_RATE;
		} else {
			src.Warning( "'%s' envelope: unknown keyword '%s'", parmName, token.c_str() );
			return false;
		}
		if ( envelope.flags & flag ) {
			src.Warning( "'%s' envelope: '%s' is given twice", parmName, token.c_str() );
			return false;
		}
		if ( !ParseEffectValues( src, token, list, components ) ) {
			return false;
		}
		envelope.flags |= flag;
	}
	src.Warning( "'%s' is not closed", parmName );
	return false;
}

/*
================
ParseEffectStage

"start { position { ... } size { ... } }": the parameters' start values, their envelopes
(motion) or their end values.
================
*/
static bool ParseEffectStage( idLexer &src, idEffectParticle &particle, int stage ) {
	idToken token;

	if ( !src.ExpectTokenString( "{" ) ) {
		return false;
	}
	while ( src.ReadToken( &token ) ) {
		if ( token == "}" ) {
			return true;
		}
		const int parm = FindEffectKeyword( token, effectParmNames, EFFECT_PARM_NUM );
		if ( parm < 0 ) {
			src.Warning( "unknown particle parameter '%s'", token.c_str() );
			return false;
		}
		effectParm_t &value = particle.parms[parm];
		const int components = EffectParmComponents( particle.type, parm );
		bool ok;
		switch ( stage ) {
			case STAGE_START:
				ok = ParseEffectDomain( src, effectParmNames[parm], value.start, components );
				break;
			case STAGE_MOTION:
				ok = ParseEffectEnvelope( src, effectParmNames[parm], value.envelope, components );
				break;
			default:
				ok = ParseEffectDomain( src, effectParmNames[parm], value.end, components );
				break;
		}
		if ( !ok ) {
			return false;
		}
	}
	src.Warning( "particle stage block is not closed" );
	return false;
}

/*
================
ParseEffectEvent

"impact { bounce 0.3 remove 0 effect "effects/..." }" and "timeout { effect "..." }": the
effects to play there (any number), and for an impact how the particle reacts.
================
*/
static bool ParseEffectEvent( idLexer &src, idEffectParticle &particle, bool impact ) {
	idToken token, name;

	if ( !src.ExpectTokenString( "{" ) ) {
		return false;
	}
	while ( src.ReadToken( &token ) ) {
		if ( token == "}" ) {
			return true;
		}
		if ( !token.Icmp( "effect" ) ) {
			if ( !ParseEffectName( src, "effect", name ) ) {
				return false;
			}
			const idDeclEffect *effect = declManager->FindEffect( name );
			if ( impact ) {
				particle.impactEffects.Append( effect );
			} else {
				particle.timeoutEffects.Append( effect );
			}
		} else if ( impact && ( !token.Icmp( "bounce" ) || !token.Icmp( "remove" ) ) ) {
			const bool bounce = !token.Icmp( "bounce" );
			const int key = bounce ? EFFECT_PKEY_BOUNCE : EFFECT_PKEY_REMOVE;
			if ( particle.keys & key ) {
				src.Warning( "'%s' is given twice in impact", token.c_str() );
				return false;
			}
			if ( !ParseEffectScalar( src, token, bounce ? particle.bounce : particle.remove ) ) {
				return false;
			}
			particle.keys |= key;
		} else {
			src.Warning( "unknown keyword '%s' in %s", token.c_str(), impact ? "impact" : "timeout" );
			return false;
		}
	}
	src.Warning( "%s block is not closed", impact ? "impact" : "timeout" );
	return false;
}

/*
===============================================================================

	idEffectParticle

===============================================================================
*/

void idEffectParticle::Clear( void ) {
	type = EFFECT_PARTICLE_NONE;
	flags = 0;
	keys = 0;
	duration.min = duration.max = 0.0f;
	gravity.min = gravity.max = 0.0f;
	material = NULL;
	entityDef.Clear();
	model = NULL;
	fork = 0;
	jitterRate = 0.0f;
	jitterSize.Zero();
	jitterTable = NULL;
	trailType = EFFECT_TRAIL_NONE;
	trailName.Clear();
	trailSegment = -1;
	trailTime.min = trailTime.max = 0.0f;
	trailCount.min = trailCount.max = 0.0f;
	trailMaterial = NULL;
	tiling = 0.0f;
	bounce = 0.0f;
	remove = 0.0f;
	impactEffects.Clear();
	timeoutEffects.Clear();
	memset( parms, 0, sizeof( parms ) );		// EFFECT_DOMAIN_NONE, no envelope
}

/*
================
ParseEffectParticle

"sprite { duration 1,1 material "..." start { ... } motion { ... } end { ... } }"
================
*/
static bool ParseEffectParticle( idLexer &src, idEffectParticle &particle ) {
	idToken token, name;

	if ( !src.ExpectTokenString( "{" ) ) {
		return false;
	}
	while ( src.ReadToken( &token ) ) {
		if ( token == "}" ) {
			return true;
		}

		bool ok = true;
		int key = 0;
		if ( !token.Icmp( "start" ) ) {
			ok = ParseEffectStage( src, particle, STAGE_START );
		} else if ( !token.Icmp( "motion" ) ) {
			ok = ParseEffectStage( src, particle, STAGE_MOTION );
		} else if ( !token.Icmp( "end" ) ) {
			ok = ParseEffectStage( src, particle, STAGE_END );
		} else if ( !token.Icmp( "duration" ) ) {
			ok = ParseEffectRange( src, token, particle.duration );
			key = EFFECT_PKEY_DURATION;
		} else if ( !token.Icmp( "material" ) ) {
			ok = ParseEffectName( src, "material", name );
			if ( ok ) {
				particle.material = declManager->FindMaterial( name );
			}
			key = EFFECT_PKEY_MATERIAL;
		} else if ( !token.Icmp( "blend" ) ) {
			ok = ParseEffectName( src, "blend", name );
			if ( ok && name.Icmp( "add" ) ) {
				src.Warning( "unknown blend '%s'", name.c_str() );
				ok = false;
			}
			particle.flags |= EFFECT_PARTICLE_BLEND_ADD;
		} else if ( !token.Icmp( "gravity" ) ) {
			ok = ParseEffectRange( src, token, particle.gravity );
			key = EFFECT_PKEY_GRAVITY;
		} else if ( !token.Icmp( "entityDef" ) ) {
			ok = ParseEffectName( src, "entityDef", name );
			particle.entityDef = name;
			key = EFFECT_PKEY_ENTITYDEF;
		} else if ( !token.Icmp( "model" ) ) {
			ok = ParseEffectName( src, "model", name );
			if ( ok ) {
				particle.model = renderModelManager->FindModel( name );
			}
			key = EFFECT_PKEY_MODEL;
		} else if ( !token.Icmp( "fork" ) ) {
			float value = 0.0f;
			ok = ParseEffectScalar( src, token, value );
			particle.fork = (int)value;
			key = EFFECT_PKEY_FORK;
		} else if ( !token.Icmp( "jitterRate" ) ) {
			ok = ParseEffectScalar( src, token, particle.jitterRate );
			key = EFFECT_PKEY_JITTER_RATE;
		} else if ( !token.Icmp( "jitterSize" ) ) {
			ok = ParseEffectValues( src, token, particle.jitterSize.ToFloatPtr(), 3 );
			key = EFFECT_PKEY_JITTER_SIZE;
		} else if ( !token.Icmp( "jitterTable" ) ) {
			ok = ParseEffectName( src, "jitterTable", name );
			if ( ok ) {
				particle.jitterTable = static_cast<const idDeclTable *>( declManager->FindType( DECL_TABLE, name, true ) );
			}
			key = EFFECT_PKEY_JITTER_TABLE;
		} else if ( !token.Icmp( "trailType" ) ) {
			ok = ParseEffectName( src, "trailType", name );
			if ( !name.Icmp( "motion" ) ) {
				particle.trailType = EFFECT_TRAIL_MOTION;
			} else if ( !name.Icmp( "burn" ) ) {
				particle.trailType = EFFECT_TRAIL_BURN;
			} else {
				particle.trailType = EFFECT_TRAIL_SEGMENT;		// resolved by idDeclEffect::Finish
				particle.trailName = name;
			}
			key = EFFECT_PKEY_TRAIL_TYPE;
		} else if ( !token.Icmp( "trailTime" ) ) {
			ok = ParseEffectRange( src, token, particle.trailTime );
			key = EFFECT_PKEY_TRAIL_TIME;
		} else if ( !token.Icmp( "trailCount" ) ) {
			ok = ParseEffectRange( src, token, particle.trailCount );
			key = EFFECT_PKEY_TRAIL_COUNT;
		} else if ( !token.Icmp( "trailMaterial" ) ) {
			ok = ParseEffectName( src, "trailMaterial", name );
			if ( ok ) {
				particle.trailMaterial = declManager->FindMaterial( name );
			}
			key = EFFECT_PKEY_TRAIL_MATERIAL;
		} else if ( !token.Icmp( "tiling" ) ) {
			ok = ParseEffectScalar( src, token, particle.tiling );
			key = EFFECT_PKEY_TILING;
		} else if ( !token.Icmp( "impact" ) ) {
			ok = ParseEffectEvent( src, particle, true );
			key = EFFECT_PKEY_IMPACT;
		} else if ( !token.Icmp( "timeout" ) ) {
			ok = ParseEffectEvent( src, particle, false );
			key = EFFECT_PKEY_TIMEOUT;
		} else {
			const int flag = FindEffectKeyword( token, effectParticleFlagNames, EFFECT_PARTICLE_NUM_FLAGS );
			if ( flag < 0 ) {
				src.Warning( "unknown keyword '%s' in a %s particle", token.c_str(), effectParticleNames[particle.type] );
				return false;
			}
			particle.flags |= BIT( flag );
		}
		if ( !ok ) {
			return false;
		}
		if ( particle.keys & key ) {
			src.Warning( "'%s' is given twice in a %s particle", token.c_str(), effectParticleNames[particle.type] );
			return false;
		}
		particle.keys |= key;
	}
	src.Warning( "%s particle block is not closed", effectParticleNames[particle.type] );
	return false;
}

/*
===============================================================================

	idEffectSegment

===============================================================================
*/

void idEffectSegment::Clear( void ) {
	type = EFFECT_SEG_EMITTER;
	name.Clear();
	flags = 0;
	keys = 0;
	start.min = start.max = 0.0f;
	duration.min = duration.max = 0.0f;
	count.min = count.max = 0.0f;
	density.min = density.max = 0.0f;
	attenuation.min = attenuation.max = 0.0f;
	volume.min = volume.max = 0.0f;
	freqShift.min = freqShift.max = 0.0f;
	detail = 0.0f;
	scale = 0.0f;
	particleCap = 0;
	soundShader = NULL;
	effect = NULL;
	particle.Clear();
}

/*
================
ParseEffectSegment

"emitter "name" { duration 1,1 count 50,50 locked sprite { ... } }" - the segment type is read.
================
*/
static bool ParseEffectSegment( idLexer &src, idEffectSegment &segment ) {
	idToken token, name;
	const char *segmentName = effectSegmentNames[segment.type];

	// an optional name, then the body
	if ( !src.ReadToken( &token ) ) {
		src.Warning( "%s segment without a body", segmentName );
		return false;
	}
	if ( token != "{" ) {
		segment.name = token;
		if ( !src.ExpectTokenString( "{" ) ) {
			return false;
		}
	}

	while ( src.ReadToken( &token ) ) {
		if ( token == "}" ) {
			return true;
		}

		bool ok = true;
		int key = 0;
		if ( !token.Icmp( "start" ) ) {
			ok = ParseEffectRange( src, token, segment.start );
			key = EFFECT_SKEY_START;
		} else if ( !token.Icmp( "duration" ) ) {
			ok = ParseEffectRange( src, token, segment.duration );
			key = EFFECT_SKEY_DURATION;
		} else if ( !token.Icmp( "count" ) ) {
			ok = ParseEffectRange( src, token, segment.count );
			key = EFFECT_SKEY_COUNT;
		} else if ( !token.Icmp( "density" ) ) {
			ok = ParseEffectRange( src, token, segment.density );
			key = EFFECT_SKEY_DENSITY;
		} else if ( !token.Icmp( "attenuation" ) ) {
			ok = ParseEffectRange( src, token, segment.attenuation );
			key = EFFECT_SKEY_ATTENUATION;
		} else if ( !token.Icmp( "volume" ) ) {
			ok = ParseEffectRange( src, token, segment.volume );
			key = EFFECT_SKEY_VOLUME;
		} else if ( !token.Icmp( "freqShift" ) ) {
			ok = ParseEffectRange( src, token, segment.freqShift );
			key = EFFECT_SKEY_FREQSHIFT;
		} else if ( !token.Icmp( "detail" ) ) {
			ok = ParseEffectScalar( src, token, segment.detail );
			key = EFFECT_SKEY_DETAIL;
		} else if ( !token.Icmp( "scale" ) ) {
			ok = ParseEffectScalar( src, token, segment.scale );
			key = EFFECT_SKEY_SCALE;
		} else if ( !token.Icmp( "particleCap" ) ) {
			float value = 0.0f;
			ok = ParseEffectScalar( src, token, value );
			segment.particleCap = (int)value;
			key = EFFECT_SKEY_PARTICLE_CAP;
		} else if ( !token.Icmp( "soundShader" ) ) {
			ok = ParseEffectName( src, "soundShader", name );
			if ( ok ) {
				segment.soundShader = declManager->FindSound( name );
			}
			key = EFFECT_SKEY_SOUND_SHADER;
		} else if ( !token.Icmp( "effect" ) ) {
			ok = ParseEffectName( src, "effect", name );
			if ( ok ) {
				segment.effect = declManager->FindEffect( name );
			}
			key = EFFECT_SKEY_EFFECT;
		} else {
			const int flag = FindEffectKeyword( token, effectSegmentFlagNames, EFFECT_SEGMENT_NUM_FLAGS );
			if ( flag >= 0 ) {
				segment.flags |= BIT( flag );
			} else {
				const int particleType = FindEffectKeyword( token, effectParticleNames, EFFECT_PARTICLE_NUM );
				if ( particleType < 0 ) {
					src.Warning( "unknown keyword '%s' in %s segment '%s'", token.c_str(), segmentName, segment.name.c_str() );
					return false;
				}
				if ( !EffectSegmentHasParticles( segment.type ) ) {
					src.Warning( "a %s segment has no particles ('%s')", segmentName, token.c_str() );
					return false;
				}
				if ( segment.particle.type != EFFECT_PARTICLE_NONE ) {
					src.Warning( "%s segment '%s' has a second particle block", segmentName, segment.name.c_str() );
					return false;
				}
				segment.particle.type = (effectParticleType_t)particleType;
				ok = ParseEffectParticle( src, segment.particle );
			}
		}
		if ( !ok ) {
			return false;
		}
		if ( segment.keys & key ) {
			src.Warning( "'%s' is given twice in %s segment '%s'", token.c_str(), segmentName, segment.name.c_str() );
			return false;
		}
		segment.keys |= key;
	}
	src.Warning( "%s segment '%s' is not closed", segmentName, segment.name.c_str() );
	return false;
}

/*
===============================================================================

	idDeclEffect

===============================================================================
*/

/*
================
idDeclEffect::Size
================
*/
size_t idDeclEffect::Size( void ) const {
	size_t total = sizeof( idDeclEffect ) + segments.Allocated();
	for ( int i = 0; i < segments.Num(); i++ ) {
		const idEffectSegment &segment = segments[i];
		total += segment.name.Allocated() + segment.particle.entityDef.Allocated() + segment.particle.trailName.Allocated() +
				segment.particle.impactEffects.Allocated() + segment.particle.timeoutEffects.Allocated();
	}
	return total;
}

/*
================
idDeclEffect::DefaultDefinition

An effect without segments: it plays nothing and is over at once.
================
*/
const char *idDeclEffect::DefaultDefinition( void ) const {
	return "{\n\tsize\t0\n}";
}

/*
================
idDeclEffect::FreeData
================
*/
void idDeclEffect::FreeData( void ) {
	size = 0.0f;
	cutOffDistance = 0.0f;
	keys = 0;
	hasSound = false;
	segments.Clear();
}

/*
================
idDeclEffect::Parse
================
*/
bool idDeclEffect::Parse( const char *text, const int textLength ) {
	idLexer src;
	idToken token;

	src.LoadMemory( text, textLength, GetFileName(), GetLineNum() );
	src.SetFlags( DECL_LEXER_FLAGS );
	src.SkipUntilString( "{" );

	FreeData();
	segments.SetGranularity( 4 );

	bool ok = true;
	while ( ok ) {
		if ( !src.ReadToken( &token ) ) {
			src.Warning( "effect '%s' is not closed", GetName() );
			ok = false;
			break;
		}
		if ( token == "}" ) {
			break;
		}
		if ( !token.Icmp( "size" ) ) {
			ok = ParseEffectScalar( src, token, size );
			keys |= EFFECT_KEY_SIZE;
		} else if ( !token.Icmp( "cutOffDistance" ) ) {
			ok = ParseEffectScalar( src, token, cutOffDistance );
			keys |= EFFECT_KEY_CUTOFF_DISTANCE;
		} else {
			const int type = FindEffectKeyword( token, effectSegmentNames, EFFECT_SEG_NUM );
			if ( type < 0 ) {
				src.Warning( "effect '%s': unknown keyword '%s'", GetName(), token.c_str() );
				ok = false;
				break;
			}
			idEffectSegment &segment = segments.Alloc();
			segment.Clear();
			segment.type = (effectSegmentType_t)type;
			ok = ParseEffectSegment( src, segment );
		}
	}

	if ( !ok || src.HadError() ) {
		MakeDefault();
		return false;
	}
	Finish();
	return true;
}

/*
================
idDeclEffect::Finish

Links the particles' named trails to their trail segments and notes what the effect uses.
================
*/
void idDeclEffect::Finish( void ) {
	segments.Condense();

	hasSound = false;
	for ( int i = 0; i < segments.Num(); i++ ) {
		idEffectSegment &segment = segments[i];
		if ( segment.type == EFFECT_SEG_SOUND && segment.soundShader != NULL ) {
			hasSound = true;
		}

		idEffectParticle &particle = segment.particle;
		if ( particle.trailType == EFFECT_TRAIL_SEGMENT ) {
			particle.trailSegment = -1;
			for ( int j = 0; j < segments.Num(); j++ ) {
				if ( segments[j].type == EFFECT_SEG_TRAIL && !segments[j].name.Icmp( particle.trailName ) ) {
					particle.trailSegment = j;
					break;
				}
			}
			if ( particle.trailSegment < 0 ) {
				// retail data has one: no trail, as the game shipped
				common->DPrintf( "effect '%s': segment '%s' trails '%s', which is not a trail segment of the effect\n",
					GetName(), segment.name.c_str(), particle.trailName.c_str() );
			}
		}
	}
}
