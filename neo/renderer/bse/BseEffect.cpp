/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: Quake 4's effects (Raven's BSE) - the runtime's simulation: segment timing, spawning,
// spawn domains, envelopes, particle evaluation, trails, and each particle kind's geometry. No
// renderer here (BseRender.cpp submits the geometry), so the offline test
// (tools/quake4/test_bse_sim.sh) runs this file as the engine does.
//
// Behaviour follows Quake 4's own effect system as docs/quake4-bse.md 3.1 and 3.2 set it out; its
// sources: id's FX editor pages for what the keywords mean, and for the arithmetic (spawn
// timing, distance attenuation, sampler shapes, envelope rates, particle motion, sprite corners,
// colour packing, draw order, loops, durations, bounds; lines, oriented quads, bolts, strips,
// models, trails) OpenQ4's src/bse, consulted under the decision of docs/quake4-port.md 9.2 and
// re-expressed here.

#include "sys/platform.h"
#include "idlib/math/Math.h"
#include "idlib/math/Angles.h"
#include "framework/Common.h"
#include "framework/DeclTable.h"
#include "renderer/bse/BseEffect.h"

idCVar *bse_enabled = NULL;
idCVar *bse_render = NULL;
idCVar *bse_scale = NULL;
idCVar *bse_maxParticles = NULL;
idCVar *bse_debug = NULL;

static int BSE_ParticleCap( void ) {
	return idMath::ClampInt( 0, BSE_MAX_PARTICLES, BSE_MaxParticles() );
}

static float BSE_RandomRange( float min, float max ) {
	return min + ( max - min ) * BSE_Random().RandomFloat();
}

/*
===============================================================================

	spawn domains

	A domain as the samplers see it: its shape, its lowest and highest values per component
	(a point's are equal) and a spiral's repeat length. The shapes of the random distributions
	are the retail ones, which the effects were authored against: directions are a random point
	of the [-1,1] cube pushed onto the sphere, and a volume sphere scales each axis by its own
	random radius, packing samples towards the centre and the axis planes.

===============================================================================
*/

typedef struct {
	effectDomainType_t	type;
	int					flags;
	int					components;
	idVec3				mins;
	idVec3				maxs;
	float				range;		// spiral: one turn per this much x
} bseDomain_t;

static const idVec3 bseCubeNormals[6] = {
	idVec3( -1.0f, 0.0f, 0.0f ), idVec3( 0.0f, -1.0f, 0.0f ), idVec3( 0.0f, 0.0f, -1.0f ),
	idVec3( 1.0f, 0.0f, 0.0f ), idVec3( 0.0f, 1.0f, 0.0f ), idVec3( 0.0f, 0.0f, 1.0f )
};

static bseDomain_t BSE_MakeDomain( const effectDomain_t &domain, int components, float defaultValue ) {
	bseDomain_t d;
	d.type = domain.type;
	d.flags = domain.flags;
	d.components = components;
	d.range = domain.type == EFFECT_DOMAIN_SPIRAL ? domain.values[6] : 0.0f;
	EffectDomainRange( domain, components, defaultValue, d.mins, d.maxs );
	return d;
}

static void BSE_NormalizeOrKeep( idVec3 &v ) {
	const float lengthSqr = v.LengthSqr();
	if ( lengthSqr != 0.0f ) {
		v *= idMath::InvSqrt( lengthSqr );
	}
}

// the normal of a volume sample points away from the centre (from the origin without one)
static void BSE_SampleNormal( idVec3 *normal, const idVec3 &sample, const idVec3 *centre ) {
	if ( normal != NULL ) {
		*normal = centre != NULL ? sample - *centre : sample;
		BSE_NormalizeOrKeep( *normal );
	}
}

static idVec3 BSE_RandomDirection3( void ) {
	idVec3 dir( BSE_RandomRange( -1.0f, 1.0f ), BSE_RandomRange( -1.0f, 1.0f ), BSE_RandomRange( -1.0f, 1.0f ) );
	BSE_NormalizeOrKeep( dir );
	return dir;
}

static idVec2 BSE_RandomDirection2( void ) {
	idVec2 dir( BSE_RandomRange( -1.0f, 1.0f ), BSE_RandomRange( -1.0f, 1.0f ) );
	const float lengthSqr = dir.LengthSqr();
	if ( lengthSqr != 0.0f ) {
		dir *= idMath::InvSqrt( lengthSqr );
	}
	return dir;
}

/*
================
BSE_SampleDomain

A value from a domain (out has the domain's components; the rest are left alone). seed is the
spawn fraction linearSpacing uses: i / count for a spawner's particles, the birth time itself
for an emitter's (retail's; such lines extrapolate far beyond their ends, as in the game).
normal and centre: a generated normal, for position domains.
================
*/
static void BSE_SampleDomain( const bseDomain_t &d, float seed, idVec3 &out, idVec3 *normal, const idVec3 *centre ) {
	const int n = d.components;
	switch ( d.type ) {
		case EFFECT_DOMAIN_NONE:
		case EFFECT_DOMAIN_POINT:
			for ( int i = 0; i < n; i++ ) {
				out[i] = d.mins[i];
			}
			break;

		case EFFECT_DOMAIN_LINE: {
			// one component: always at random
			const float t = ( n > 1 && ( d.flags & EFFECT_DOMAIN_LINEAR_SPACING ) ) ? seed : BSE_Random().RandomFloat();
			for ( int i = 0; i < n; i++ ) {
				out[i] = d.mins[i] + ( d.maxs[i] - d.mins[i] ) * t;
			}
			break;
		}

		case EFFECT_DOMAIN_BOX:
			if ( !( d.flags & EFFECT_DOMAIN_SURFACE ) ) {
				for ( int i = 0; i < n; i++ ) {
					out[i] = BSE_RandomRange( d.mins[i], d.maxs[i] );
				}
				break;
			}
			if ( n == 1 ) {
				// retail picks between the lowest x and the lowest y here
				out[0] = d.mins[ BSE_Random().RandomInt( 2 ) ];
			} else if ( n == 2 ) {
				// the rectangle's outline: an edge, then a point along it
				switch ( BSE_Random().RandomInt( 4 ) ) {
					case 0:		out[0] = d.mins[0]; out[1] = BSE_RandomRange( d.mins[1], d.maxs[1] ); break;
					case 1:		out[0] = BSE_RandomRange( d.mins[0], d.maxs[0] ); out[1] = d.mins[1]; break;
					case 2:		out[0] = d.maxs[0]; out[1] = BSE_RandomRange( d.mins[1], d.maxs[1] ); break;
					default:	out[0] = BSE_RandomRange( d.mins[0], d.maxs[0] ); out[1] = d.maxs[1]; break;
				}
			} else {
				// a face (-x -y -z +x +y +z), then a point on it
				const int face = BSE_Random().RandomInt( 6 );
				for ( int i = 0; i < 3; i++ ) {
					if ( i == face % 3 ) {
						out[i] = face < 3 ? d.mins[i] : d.maxs[i];
					} else {
						out[i] = BSE_RandomRange( d.mins[i], d.maxs[i] );
					}
				}
				if ( normal != NULL && centre != NULL ) {
					*normal = bseCubeNormals[face];
					return;
				}
				BSE_SampleNormal( normal, out, NULL );
				return;
			}
			break;

		case EFFECT_DOMAIN_SPHERE: {
			const idVec3 mid = ( d.mins + d.maxs ) * 0.5f;
			const idVec3 radius = ( d.maxs - d.mins ) * 0.5f;
			const idVec3 dir = BSE_RandomDirection3();
			for ( int i = 0; i < 3; i++ ) {
				const float r = ( d.flags & EFFECT_DOMAIN_SURFACE ) ? radius[i] : BSE_RandomRange( 0.0f, radius[i] );
				out[i] = mid[i] + r * dir[i];
			}
			break;
		}

		case EFFECT_DOMAIN_CYLINDER: {
			// along x; y and z give the elliptical cross-section; a cone narrows to a point at the
			// lowest x
			const idVec3 mid = ( d.mins + d.maxs ) * 0.5f;
			const idVec3 radius = ( d.maxs - d.mins ) * 0.5f;
			const idVec2 dir = BSE_RandomDirection2();
			const float length = d.maxs.x - d.mins.x;
			const float along = BSE_RandomRange( 0.0f, length );
			const float taper = ( length != 0.0f && ( d.flags & EFFECT_DOMAIN_CONE ) ) ? along / length : 1.0f;
			out.x = d.mins.x + along;
			if ( !( d.flags & EFFECT_DOMAIN_SURFACE ) ) {
				out.y = mid.y + BSE_RandomRange( 0.0f, radius.y * taper ) * dir.x;
				out.z = mid.z + BSE_RandomRange( 0.0f, radius.z * taper ) * dir.y;
				break;
			}
			const float edgeY = radius.y * dir.x;
			const float edgeZ = radius.z * dir.y;
			out.y = mid.y + edgeY * taper;
			out.z = mid.z + edgeZ * taper;
			if ( normal != NULL && centre != NULL ) {
				if ( taper == 1.0f ) {
					normal->Set( 0.0f, dir.x, dir.y );		// straight out from the axis
				} else {
					// a cone's side leans back towards its apex
					normal->Set( -( edgeY * edgeY + edgeZ * edgeZ ), edgeY * length, edgeZ * length );
					BSE_NormalizeOrKeep( *normal );
				}
				return;
			}
			break;
		}

		case EFFECT_DOMAIN_SPIRAL: {
			out.x = ( d.flags & EFFECT_DOMAIN_LINEAR_SPACING ) ? d.mins.x + ( d.maxs.x - d.mins.x ) * seed : BSE_RandomRange( d.mins.x, d.maxs.x );
			const float radiusY = BSE_RandomRange( d.mins.y, d.maxs.y );
			const float radiusZ = BSE_RandomRange( d.mins.z, d.maxs.z );
			if ( d.range != 0.0f ) {
				float s, c;
				idMath::SinCos( idMath::TWO_PI * out.x / d.range, s, c );
				out.y = c * radiusY - s * radiusZ;
				out.z = c * radiusZ + s * radiusY;
			} else {
				out.y = radiusY;
				out.z = radiusZ;
			}
			if ( normal != NULL ) {
				normal->Set( centre != NULL ? 0.0f : out.x, out.y, out.z );
				BSE_NormalizeOrKeep( *normal );
			}
			return;
		}

		default:
			break;
	}
	if ( n == 3 ) {
		BSE_SampleNormal( normal, out, centre );
	}
}

/*
================
BSE_EvaluateEnvelope

A parameter at time t of a life: from its start to its end value by the envelope's table, read
at rate * t + offset per component. "count n" is n passes over the life (the rate fixed at birth
by oneOverLife), "rate n" n passes per second; without an envelope the value keeps its start.
================
*/
static void BSE_EvaluateEnvelope( const effectEnvelope_t &envelope, const float *start, const float *end, int components,
		float t, float oneOverLife, float *out ) {
	if ( envelope.table == NULL ) {
		for ( int i = 0; i < components; i++ ) {
			out[i] = start[i];
		}
		return;
	}
	const bool perSecond = ( envelope.flags & EFFECT_ENVELOPE_RATE ) != 0;
	const float *rates = perSecond ? envelope.rate : ( ( envelope.flags & EFFECT_ENVELOPE_COUNT ) ? envelope.count : NULL );
	const bool offsets = ( envelope.flags & EFFECT_ENVELOPE_OFFSET ) != 0;
	for ( int i = 0; i < components; i++ ) {
		float rate = rates != NULL ? rates[i] : 1.0f;
		if ( !perSecond ) {
			rate *= oneOverLife;
		}
		const float x = rate * t + ( offsets ? envelope.offset[i] : 0.0f );
		out[i] = start[i] + ( end[i] - start[i] ) * envelope.table->TableLookup( x );
	}
}

/*
===============================================================================

	idBseSegment

===============================================================================
*/

/*
================
idBseSegment::Init
================
*/
void idBseSegment::Init( idBseEffect *effect, int index, float time ) {
	this->index = index;
	tmpl = &effect->decl->GetSegment( index );
	expired = false;
	lastTime = time;
	segStartTime = segEndTime = time;
	count = tmpl->count;
	fastInterval = slowInterval = 0.0f;
	budget = budgetOnce = budgetLooping = 0;
	trailDuration = 0.0f;
	particles.Clear();
	particles.SetGranularity( 32 );

	if ( !tmpl->enabled ) {
		expired = true;
		return;
	}
	InitTime( effect, effect->startTime );

	// "density": the count follows the size of the spawn domain
	if ( tmpl->density.max != 0.0f ) {
		const idEffectParticle &particle = tmpl->particle;
		idVec3 mins, maxs;
		EffectDomainRange( particle.parms[EFFECT_PARM_POSITION].start, 3, 0.0f, mins, maxs );
		float xExtent = maxs.x - mins.x;
		if ( ( particle.parms[EFFECT_PARM_POSITION].start.flags & EFFECT_DOMAIN_USE_END_ORIGIN ) && effect->hasEndOrigin ) {
			xExtent = ( effect->originalEndOrigin - effect->originalOrigin ).Length() - mins.x;
		}
		const float volume = idMath::ClampFloat( BSE_TIME_EPSILON, 1000.0f, ( xExtent + ( maxs.y - mins.y ) + ( maxs.z - mins.z ) ) * 0.01f );
		count.min = tmpl->density.min * volume;
		count.max = tmpl->density.max * volume;
		if ( tmpl->particleCap > 0 ) {
			count.min = idMath::ClampFloat( 1.0f, tmpl->particleCap, count.min );
			count.max = idMath::ClampFloat( 1.0f, tmpl->particleCap, count.max );
		}
	}

	if ( tmpl->type == EFFECT_SEG_EMITTER || tmpl->type == EFFECT_SEG_TRAIL ) {
		// seconds per particle at the lowest and highest count
		slowInterval = count.min > 0.0f ? 1.0f / count.min : 0.0f;
		fastInterval = count.max > 0.0f ? 1.0f / count.max : 0.0f;
		fastInterval = idMath::ClampFloat( 0.002f, 300.0f, fastInterval );
		slowInterval = idMath::ClampFloat( fastInterval, 300.0f, slowInterval );
	}
}

/*
================
idBseSegment::InitTime

The segment's window for the loop starting at time: its start and duration picked from their
ranges. It makes the effect last at least that long, unless it is a sound (sounds do not keep a
looping effect from looping).
================
*/
void idBseSegment::InitTime( idBseEffect *effect, float time ) {
	expired = false;
	if ( tmpl->type == EFFECT_SEG_TRAIL ) {
		segStartTime = segEndTime = time;	// a trail follows the particles that leave it
	} else {
		segStartTime = BSE_RandomRange( tmpl->start.min, tmpl->start.max ) + time;
		segEndTime = BSE_RandomRange( tmpl->duration.min, tmpl->duration.max ) + segStartTime;
	}
	if ( !tmpl->ignoreDuration || !effect->looping ) {
		effect->SetDuration( segEndTime - time );
	}
}

/*
================
idBseSegment::CalcBudget

The most particles the segment holds at once, sized as Quake 4 sizes them: an emitter one
particle per spawn interval over the shorter of its window and a particle's life, a spawner its
count, more for a looping effect whose particles outlive a loop. A spawn beyond it is dropped.
A non-looping effect lasts until the segment's last particle can die. A trail segment holds what
the segments leaving it need (AddTrailBudget, once every budget is known).
================
*/
void idBseSegment::CalcBudget( idBseEffect *effect, float time ) {
	if ( tmpl->type == EFFECT_SEG_TRAIL ) {
		return;
	}
	budget = budgetOnce = budgetLooping = 0;
	trailDuration = 0.0f;
	if ( !tmpl->enabled ) {
		return;
	}
	const idEffectParticle &particle = tmpl->particle;
	if ( particle.type == EFFECT_PARTICLE_NONE || ( tmpl->detail > 0.0f && BSE_Scale() < tmpl->detail ) ) {
		return;
	}

	const float maxLife = particle.duration.max + BSE_FUTURE;
	const float minEffectDuration = effect->decl->GetMinDuration();
	int countOnce = 0, countLooping = 0;
	switch ( tmpl->type ) {
		case EFFECT_SEG_EMITTER:
			if ( particle.type == EFFECT_PARTICLE_DEBRIS ) {
				countOnce = countLooping = 1;
			} else {
				const float window = Min( maxLife, tmpl->duration.max ) + BSE_FUTURE;
				trailDuration = window;
				countOnce = countLooping = (int)idMath::Ceil( window / fastInterval ) + 1;
				if ( minEffectDuration > BSE_TIME_EPSILON && minEffectDuration < maxLife ) {
					countLooping = (int)idMath::Ceil( ( (float)countOnce / minEffectDuration ) * maxLife ) + 1;
				}
			}
			break;
		case EFFECT_SEG_SPAWNER:
			if ( particle.type == EFFECT_PARTICLE_DEBRIS ) {
				countOnce = countLooping = 1;
			} else {
				countOnce = countLooping = (int)idMath::Ceil( count.max );
				if ( !( tmpl->flags & EFFECT_SEGMENT_CONSTANT ) && minEffectDuration > BSE_TIME_EPSILON && minEffectDuration < maxLife ) {
					countLooping = countOnce * ( (int)idMath::Ceil( maxLife / minEffectDuration ) + 1 ) + 1;
				}
			}
			break;
		case EFFECT_SEG_DECAL:
		case EFFECT_SEG_LIGHT:
			countOnce = countLooping = 1;
			break;
		default:
			break;
	}
	budgetOnce = idMath::ClampInt( 0, BSE_ParticleCap(), countOnce );
	budgetLooping = idMath::ClampInt( 0, BSE_ParticleCap(), countLooping );
	budget = effect->looping ? budgetLooping : budgetOnce;

	if ( !effect->looping ) {
		effect->SetDuration( ( segEndTime - time ) + particle.duration.max );
	}
}

/*
================
idBseSegment::AddTrailBudget

A named trail holds, per particle leaving it, one of its own particles per spawn interval over
the longer of that particle's trail time and its own particles' life.
================
*/
void idBseSegment::AddTrailBudget( int once, int looping, float duration ) {
	duration = Max( duration, tmpl->particle.duration.max );
	const int perParticle = (int)idMath::Ceil( ( duration + BSE_FUTURE ) / Max( BSE_TIME_EPSILON, fastInterval ) ) + 1;
	budgetOnce = idMath::ClampInt( 0, BSE_ParticleCap(), budgetOnce + once * perParticle );
	budgetLooping = idMath::ClampInt( 0, BSE_ParticleCap(), budgetLooping + looping * perParticle );
}

/*
================
idBseSegment::Rewind

An ambient looping effect starts with two loops of history: a segment spanning the whole loop
starts two loops early.
================
*/
void idBseSegment::Rewind( idBseEffect *effect ) {
	if ( effect->duration == segEndTime - segStartTime ) {
		segStartTime -= 2.0f * effect->duration;
		lastTime = segStartTime;
	}
}

// emitter: seconds between spawns, by bse_scale and the distance attenuation
float idBseSegment::AttenuatedInterval( idBseEffect *effect ) const {
	const float scale = idMath::ClampFloat( 0.0f, 1.0f, BSE_Scale() );
	const float interval = idMath::ClampFloat( fastInterval, slowInterval, idMath::Lerp( slowInterval, fastInterval, scale ) );
	if ( !( tmpl->flags & ( EFFECT_SEGMENT_ATTENUATE_EMITTER | EFFECT_SEGMENT_INVERSE_ATTENUATE_EMITTER ) ) ) {
		return interval;
	}
	float attenuation = effect->SegmentAttenuation( *tmpl );
	if ( tmpl->flags & EFFECT_SEGMENT_INVERSE_ATTENUATE_EMITTER ) {
		attenuation = 1.0f - attenuation;
	}
	if ( attenuation < 0.002f ) {
		return 1.0f;
	}
	return Max( 1.1920929e-7f, interval / attenuation );
}

// spawner: how many at once, by bse_scale and the distance attenuation
float idBseSegment::AttenuatedCount( idBseEffect *effect ) const {
	const float scale = idMath::ClampFloat( 0.0f, 1.0f, BSE_Scale() );
	const float countMax = idMath::Lerp( count.min, count.max, scale );
	const float n = idMath::ClampFloat( count.min, count.max, BSE_RandomRange( count.min, countMax ) );
	if ( !( tmpl->flags & ( EFFECT_SEGMENT_ATTENUATE_EMITTER | EFFECT_SEGMENT_INVERSE_ATTENUATE_EMITTER ) ) ) {
		return n;
	}
	float attenuation = effect->SegmentAttenuation( *tmpl );
	if ( tmpl->flags & EFFECT_SEGMENT_INVERSE_ATTENUATE_EMITTER ) {
		attenuation = 1.0f - attenuation;
	}
	return attenuation * n;
}

/*
================
idBseSegment::Check

Spawns what is due by time. An emitter spawns at its interval, each particle at its own birth
time, up to BSE_FUTURE ahead and to the end of its window; a spawner spawns its count at its
start, the count truncated (a range a,b gives a to b-1 particles). Lines spanning to the end
origin follow it. Other segment kinds act once per loop (phases S4-S6); a trail segment's
particles come from the particles leaving them (EmitTrail).
================
*/
bool idBseSegment::Check( idBseEffect *effect, float time ) {
	// the spawn clock moves on with every service, even before the segment starts
	const float previous = lastTime;
	lastTime = time;

	// lines spanning to the end origin follow it (and re-roll their lengths while the effect moves)
	if ( time >= segStartTime && effect->endOriginChanged && ( tmpl->type == EFFECT_SEG_EMITTER || tmpl->type == EFFECT_SEG_SPAWNER ) ) {
		RefreshLengths( effect );
	}

	if ( expired || effect->stopped ) {
		return true;
	}
	if ( time < segStartTime ) {
		return false;
	}
	if ( !tmpl->enabled || ( tmpl->detail > 0.0f && BSE_Scale() < tmpl->detail ) ) {
		return true;
	}

	switch ( tmpl->type ) {
		case EFFECT_SEG_EMITTER: {
			if ( tmpl->particle.type == EFFECT_PARTICLE_NONE ) {
				return true;
			}
			if ( !effect->SpawnAllowed() ) {
				return expired;
			}
			float spawnEnd = time + BSE_FUTURE;
			if ( segEndTime - BSE_TIME_EPSILON <= spawnEnd ) {
				spawnEnd = segEndTime;
			}
			const float interval = AttenuatedInterval( effect );
			float spawnTime = previous;
			if ( spawnTime < segStartTime && interval > 0.0f ) {
				spawnTime += idMath::Ceil( ( segStartTime - spawnTime ) / interval ) * interval;
			}
			const int cap = BSE_ParticleCap();
			for ( int spawned = 0; spawnTime < spawnEnd && spawned < cap; spawned++ ) {
				// the birth time doubles as the linearSpacing fraction, as in retail
				Spawn( effect, spawnTime, spawnTime );
				spawnTime += interval;
			}
			if ( segEndTime - BSE_TIME_EPSILON <= spawnEnd ) {
				expired = true;
			}
			lastTime = spawnTime;
			return expired;
		}
		case EFFECT_SEG_SPAWNER: {
			if ( tmpl->particle.type != EFFECT_PARTICLE_NONE ) {
				const int n = idMath::ClampInt( 0, BSE_ParticleCap(), (int)AttenuatedCount( effect ) );
				for ( int i = 0; i < n; i++ ) {
					Spawn( effect, segStartTime, (float)i / (float)n );
				}
			}
			expired = true;
			return true;
		}
		default:
			// trails follow their particles (S3); delays only time the loop; sounds, decals, lights,
			// nested effects and view effects come with S4-S6
			expired = true;
			return true;
	}
}

/*
================
idBseSegment::Spawn
================
*/
void idBseSegment::Spawn( idBseEffect *effect, float birthTime, float fraction, const idVec3 *trailOffset, const idMat3 *trailAxis ) {
	if ( particles.Num() >= budget ) {
		return;		// as in retail: no free particle, no spawn
	}
	bseParticle_t p;
	SpawnParticle( effect, p, birthTime, fraction, trailOffset, trailAxis );
	if ( p.endTime - BSE_TIME_EPSILON <= effect->currentTime && !( tmpl->flags & EFFECT_SEGMENT_CONSTANT ) ) {
		return;		// already over (an ambient effect's history): takes no slot
	}
	Insert( p );
}

// a named trail's particle: its birth time doubles as the linearSpacing fraction, as an emitter's
void idBseSegment::SpawnTrailParticle( idBseEffect *effect, float birthTime, const idVec3 &offset, const idMat3 &axis ) {
	if ( tmpl->enabled && tmpl->particle.type != EFFECT_PARTICLE_NONE && !( tmpl->detail > 0.0f && BSE_Scale() < tmpl->detail ) ) {
		Spawn( effect, birthTime, birthTime, &offset, &axis );
	}
}

// draw order: soonest to die first; a complex segment's newest first
void idBseSegment::Insert( const bseParticle_t &p ) {
	if ( tmpl->complex && tmpl->particle.type != EFFECT_PARTICLE_LINKED ) {
		particles.Insert( p, 0 );
		return;
	}
	int i = particles.Num();
	while ( i > 0 && particles[i - 1].endTime >= p.endTime ) {
		i--;
	}
	// equal end times: the new one goes first, as retail's list insert does
	particles.Insert( p, i );
}

// the vector a normal turns +x into, for parameters authored along +x
static idMat3 BSE_NormalAxis( const idVec3 &normal ) {
	return normal.ToMat3();
}

/*
================
BSE_NormalAngles

A generated normal as the pitch, yaw and roll (radians) an oriented particle turns by: Quake 4's
own conversion, pitch negated and wrapped to 0-2pi, and a normal straight down pitched by 3/4 pi
rather than 3/2 pi - a quirk the authored effects show.
================
*/
static idVec3 BSE_NormalAngles( const idVec3 &normal ) {
	float pitch, yaw;
	if ( normal.x == 0.0f && normal.y == 0.0f ) {
		yaw = 0.0f;
		pitch = normal.z > 0.0f ? idMath::HALF_PI : 3.0f * idMath::PI * 0.25f;
	} else {
		yaw = idMath::ATan( normal.y, normal.x );
		if ( yaw < 0.0f ) {
			yaw += idMath::TWO_PI;
		}
		pitch = idMath::ATan( normal.z, idMath::Sqrt( normal.x * normal.x + normal.y * normal.y ) );
		if ( pitch < 0.0f ) {
			pitch += idMath::TWO_PI;
		}
	}
	return idVec3( -pitch, yaw, 0.0f );
}

/*
================
idBseSegment::SampleLength

A line's length from a domain; one whose start length has useEndOrigin reaches out to the end
origin: x moves out by the distance from the effect to it (an unset domain stays nothing).
================
*/
void idBseSegment::SampleLength( idBseEffect *effect, const effectDomain_t &domain, idVec3 &length ) const {
	bseDomain_t d = BSE_MakeDomain( domain, 3, 0.0f );
	if ( effect->hasEndOrigin && ( tmpl->particle.parms[EFFECT_PARM_LENGTH].start.flags & EFFECT_DOMAIN_USE_END_ORIGIN ) &&
			d.type != EFFECT_DOMAIN_NONE ) {
		const float distance = ( effect->currentEndOrigin - effect->currentOrigin ).Length();
		d.flags = 0;
		d.mins.x += distance;
		d.maxs.x += distance;
	}
	length.Zero();
	BSE_SampleDomain( d, 0.0f, length, NULL, NULL );
}

/*
================
idBseSegment::RefreshLengths

The end origin moved: the live lines of a segment spanning to it take new lengths - sampled
again, without the turn by a generated normal, a flip or attenuation, as Quake 4 does.
================
*/
void idBseSegment::RefreshLengths( idBseEffect *effect ) {
	const idEffectParticle &tmplP = tmpl->particle;
	if ( !tmplP.usesEndOrigin || ( tmplP.type != EFFECT_PARTICLE_LINE && tmplP.type != EFFECT_PARTICLE_ELECTRICITY ) ) {
		return;
	}
	for ( int i = 0; i < particles.Num(); i++ ) {
		bseParticle_t &p = particles[i];
		SampleLength( effect, tmplP.parms[EFFECT_PARM_LENGTH].start, p.lengthStart );
		SampleLength( effect, tmplP.parms[EFFECT_PARM_LENGTH].end, p.lengthEnd );
		if ( tmplP.parms[EFFECT_PARM_LENGTH].end.flags & EFFECT_DOMAIN_RELATIVE ) {
			p.lengthEnd += p.lengthStart;
		}
		if ( tmplP.keys & EFFECT_PKEY_TILING ) {
			p.textureScale = p.lengthStart.Length() / tmplP.tiling;
		}
	}
}

/*
================
idBseSegment::SpawnParticle

A particle's birth. A line's lengths come first, then velocity, acceleration and friction; then
its position, with a generated normal when the particle asks for one: the normal turns the
velocity and a line's length (authored along +x) to point away from the domain's centre (or the
origin), and an oriented particle turns to face along it. Acceleration is authored relative to
the direction of travel and friction relative to the acceleration's. An unlocked particle keeps
the effect's frame at its birth time, so the effect can move away from it. A named trail's
particle (trailOffset) starts where the particle leaving it is, its velocity turned to that
particle's direction of travel (trailAxis).
================
*/
void idBseSegment::SpawnParticle( idBseEffect *effect, bseParticle_t &p, float birthTime, float fraction,
		const idVec3 *trailOffset, const idMat3 *trailAxis ) {
	const idEffectParticle &tmplP = tmpl->particle;
	const effectParm_t *parms = tmplP.parms;
	const effectParticleType_t type = tmplP.type;
	const bool hasLength = type == EFFECT_PARTICLE_LINE || type == EFFECT_PARTICLE_ELECTRICITY;

	memset( &p, 0, sizeof( p ) );
	p.startTime = birthTime;
	p.lastTrailTime = birthTime;
	p.fraction = fraction;
	p.locked = ( tmpl->flags & EFFECT_SEGMENT_LOCKED ) != 0;
	p.textureScale = 1.0f;

	if ( hasLength ) {
		SampleLength( effect, parms[EFFECT_PARM_LENGTH].start, p.lengthStart );
		SampleLength( effect, parms[EFFECT_PARM_LENGTH].end, p.lengthEnd );
	}

	BSE_SampleDomain( BSE_MakeDomain( parms[EFFECT_PARM_VELOCITY].start, 3, 0.0f ), fraction, p.velocity, NULL, NULL );
	BSE_SampleDomain( BSE_MakeDomain( parms[EFFECT_PARM_ACCELERATION].start, 3, 0.0f ), fraction, p.acceleration, NULL, NULL );
	BSE_SampleDomain( BSE_MakeDomain( parms[EFFECT_PARM_FRICTION].start, 3, 0.0f ), fraction, p.friction, NULL, NULL );

	// the position, and the normal it generates
	const bool originNormal = ( tmplP.flags & EFFECT_PARTICLE_GENERATED_ORIGIN_NORMAL ) != 0;
	const bool centreNormal = ( tmplP.flags & EFFECT_PARTICLE_GENERATED_NORMAL ) != 0;
	idVec3 normal( 1.0f, 0.0f, 0.0f );
	idVec3 *wantNormal = ( originNormal || centreNormal ) ? &normal : NULL;
	const idVec3 *centre = ( !originNormal && centreNormal ) ? &tmplP.spawnCentre : NULL;
	bseDomain_t position = BSE_MakeDomain( parms[EFFECT_PARM_POSITION].start, 3, 0.0f );
	if ( effect->hasEndOrigin && ( position.flags & EFFECT_DOMAIN_USE_END_ORIGIN ) ) {
		// sampled once, then again with x running from that sample to the end origin's distance
		if ( position.type == EFFECT_DOMAIN_NONE || position.type == EFFECT_DOMAIN_POINT ) {
			position.type = EFFECT_DOMAIN_LINE;
		}
		idVec3 first( fraction, 0.0f, 0.0f );
		BSE_SampleDomain( position, fraction, first, NULL, NULL );
		position.mins.x = first.x;
		position.maxs.x = ( effect->currentEndOrigin - effect->originalOrigin ).Length();
	}
	p.initPos.Set( fraction, 0.0f, 0.0f );
	BSE_SampleDomain( position, fraction, p.initPos, wantNormal, centre );

	// a named trail's particle moves in the frame of the particle that left it
	if ( trailAxis != NULL ) {
		p.velocity = *trailAxis * p.velocity;
	}

	const idVec3 orientation = BSE_NormalAngles( normal );
	if ( wantNormal != NULL ) {
		normal.Normalize();
		const idMat3 normalAxis = BSE_NormalAxis( normal );
		p.velocity = normalAxis * p.velocity;
		if ( hasLength ) {
			p.lengthStart = normalAxis * p.lengthStart;
			p.lengthEnd = normalAxis * p.lengthEnd;
		}
	}
	if ( tmplP.flags & EFFECT_PARTICLE_FLIP_NORMAL ) {
		p.velocity = -p.velocity;
		p.lengthStart = -p.lengthStart;
		p.lengthEnd = -p.lengthEnd;
	}
	if ( p.velocity.LengthSqr() != 0.0f ) {
		normal = p.velocity;
		normal.Normalize();
	}
	p.acceleration = BSE_NormalAxis( normal ) * p.acceleration;
	if ( p.acceleration.LengthSqr() != 0.0f ) {
		normal = p.acceleration;
		normal.Normalize();
	}
	p.friction = BSE_NormalAxis( normal ) * p.friction;

	if ( p.locked ) {
		p.initAxis = trailAxis != NULL ? *trailAxis : mat3_identity;
	} else {
		// born where the effect was at the birth time within the frame
		p.initOrigin = effect->currentOrigin;
		p.initAxis = effect->currentAxis;
		const float dt = effect->currentTime - effect->lastTime;
		if ( dt >= 0.002f ) {
			const float stillToMove = 1.0f - ( birthTime - effect->lastTime ) / dt;
			p.initPos -= effect->currentAxis.Transpose() * ( ( effect->currentOrigin - effect->lastOrigin ) * stillToMove );
		}
	}
	if ( trailOffset != NULL ) {
		p.initPos += *trailOffset;
	}

	p.hasOffset = parms[EFFECT_PARM_OFFSET].start.type != EFFECT_DOMAIN_NONE || parms[EFFECT_PARM_OFFSET].end.type != EFFECT_DOMAIN_NONE;

	const float life = idMath::ClampFloat( BSE_TIME_EPSILON, BSE_MAX_LIFE, BSE_RandomRange( tmplP.duration.min, tmplP.duration.max ) );
	p.endTime = p.startTime + life;
	p.oneOverLife = 1.0f / life;

	// start and end values; an end marked relative is added to the start. Defaults: colour,
	// opacity and size 1, the rest 0; end values 0 but size 1
	struct {
		int		parm;
		idVec3 *start;
		idVec3 *end;
		float	startDefault;
		float	endDefault;
	} vectors[] = {
		{ EFFECT_PARM_TINT, &p.tintStart, &p.tintEnd, 1.0f, 0.0f },
		{ EFFECT_PARM_SIZE, &p.sizeStart, &p.sizeEnd, 1.0f, 1.0f },
		{ EFFECT_PARM_ROTATE, &p.rotateStart, &p.rotateEnd, 0.0f, 0.0f },
		{ EFFECT_PARM_ANGLE, &p.angleStart, &p.angleEnd, 0.0f, 0.0f },
		{ EFFECT_PARM_OFFSET, &p.offsetStart, &p.offsetEnd, 0.0f, 0.0f },
	};
	for ( int v = 0; v < (int)( sizeof( vectors ) / sizeof( vectors[0] ) ); v++ ) {
		const effectParm_t &parm = parms[vectors[v].parm];
		const int components = EffectParmComponents( type, vectors[v].parm );
		BSE_SampleDomain( BSE_MakeDomain( parm.start, components, vectors[v].startDefault ), fraction, *vectors[v].start, NULL, NULL );
		BSE_SampleDomain( BSE_MakeDomain( parm.end, components, vectors[v].endDefault ), fraction, *vectors[v].end, NULL, NULL );
		if ( parm.end.flags & EFFECT_DOMAIN_RELATIVE ) {
			for ( int i = 0; i < components; i++ ) {
				( *vectors[v].end )[i] += ( *vectors[v].start )[i];
			}
		}
	}
	idVec3 fade;
	BSE_SampleDomain( BSE_MakeDomain( parms[EFFECT_PARM_FADE].start, 1, 1.0f ), fraction, fade, NULL, NULL );
	p.fadeStart = fade.x;
	BSE_SampleDomain( BSE_MakeDomain( parms[EFFECT_PARM_FADE].end, 1, 0.0f ), fraction, fade, NULL, NULL );
	p.fadeEnd = fade.x;
	if ( parms[EFFECT_PARM_FADE].end.flags & EFFECT_DOMAIN_RELATIVE ) {
		p.fadeEnd += p.fadeStart;
	}
	if ( hasLength && ( parms[EFFECT_PARM_LENGTH].end.flags & EFFECT_DOMAIN_RELATIVE ) ) {
		p.lengthEnd += p.lengthStart;
	}

	// angles are turns in the file; an oriented particle then turns to its generated normal
	p.rotateStart *= idMath::TWO_PI;
	p.rotateEnd *= idMath::TWO_PI;
	p.angleStart *= idMath::TWO_PI;
	p.angleEnd *= idMath::TWO_PI;
	if ( type == EFFECT_PARTICLE_ORIENTED ) {
		p.rotateStart += orientation;
		p.rotateEnd += orientation;
	}

	// trails, a bolt's shape
	if ( tmplP.trailType == EFFECT_TRAIL_MOTION || tmplP.trailType == EFFECT_TRAIL_BURN ) {
		p.trailTime = BSE_RandomRange( tmplP.trailTime.min, tmplP.trailTime.max );
		p.trailCount = idMath::ClampInt( 0, BSE_MAX_TRAIL_COUNT, (int)BSE_RandomRange( tmplP.trailCount.min, tmplP.trailCount.max ) );
	}
	if ( type == EFFECT_PARTICLE_ELECTRICITY ) {
		p.seed = BSE_Random().RandomInt();
	}

	// distance attenuation thins opacity and size where the start domain asks for it
	float attenuation = effect->SegmentAttenuation( *tmpl );
	if ( parms[EFFECT_PARM_FADE].start.flags & EFFECT_DOMAIN_ATTENUATE ) {
		const float a = ( parms[EFFECT_PARM_FADE].start.flags & EFFECT_DOMAIN_INVERSE_ATTENUATE ) ? 1.0f - attenuation : attenuation;
		p.fadeStart *= a;
		p.fadeEnd *= a;
	}
	if ( parms[EFFECT_PARM_SIZE].start.flags & EFFECT_DOMAIN_ATTENUATE ) {
		const float a = ( parms[EFFECT_PARM_SIZE].start.flags & EFFECT_DOMAIN_INVERSE_ATTENUATE ) ? 1.0f - attenuation : attenuation;
		p.sizeStart *= a;
		p.sizeEnd *= a;
	}

	// gravity: the world's, scaled by the particle's, in the effect's frame
	const float gravityScale = BSE_RandomRange( tmplP.gravity.min, tmplP.gravity.max );
	if ( gravityScale != 0.0f ) {
		p.acceleration += effect->currentAxis.Transpose() * ( effect->gravity * gravityScale );
	}

	// a line's texture repeats once per "tiling" units of its start length; a strip's runs
	// "tiling" times over the strip
	if ( tmplP.keys & EFFECT_PKEY_TILING ) {
		if ( hasLength ) {
			p.textureScale = p.lengthStart.Length() / tmplP.tiling;
		} else if ( type == EFFECT_PARTICLE_LINKED ) {
			p.textureScale = tmplP.tiling;
		}
	}
	if ( hasLength && ( parms[EFFECT_PARM_LENGTH].start.flags & EFFECT_DOMAIN_ATTENUATE ) ) {
		const float a = ( parms[EFFECT_PARM_LENGTH].start.flags & EFFECT_DOMAIN_INVERSE_ATTENUATE ) ? 1.0f - attenuation : attenuation;
		p.lengthStart *= a;
		p.lengthEnd *= a;
	}
}

/*
================
idBseSegment::EmitTrail

A particle with a named trail leaves that segment's particles behind it, one per the trail's
spawn interval from its birth on, each born where the particle was then and moving along its
direction of travel.
================
*/
void idBseSegment::EmitTrail( idBseEffect *effect, bseParticle_t &p, float time ) {
	idBseSegment &trail = effect->segments[tmpl->particle.trailSegment];
	const float until = time + BSE_FUTURE;
	while ( p.lastTrailTime < until ) {
		if ( p.lastTrailTime >= p.startTime && p.lastTrailTime < p.endTime ) {
			const idVec3 position = effect->EvaluatePosition( p, tmpl->particle, p.lastTrailTime );
			idVec3 direction = effect->EvaluateVelocity( p, p.lastTrailTime );
			if ( direction.LengthSqr() > 1e-8f ) {
				direction.Normalize();
			} else {
				direction.Set( 1.0f, 0.0f, 0.0f );
			}
			trail.SpawnTrailParticle( effect, p.lastTrailTime, position, direction.ToMat3() );
		}
		const float interval = trail.AttenuatedInterval( effect );
		if ( interval <= BSE_TIME_EPSILON ) {
			break;
		}
		p.lastTrailTime += interval;
	}
}

/*
================
idBseSegment::UpdateParticles

Drops the particles that are over by time - all of a stopped effect's but those that persist. A
constant segment's particles live as long as the effect. Particles with a named trail leave its
particles up to time first.
================
*/
bool idBseSegment::UpdateParticles( idBseEffect *effect, float time ) {
	const bool infinite = ( tmpl->flags & EFFECT_SEGMENT_CONSTANT ) != 0;
	const bool persist = ( tmpl->particle.flags & EFFECT_PARTICLE_PERSIST ) != 0;
	const int trail = tmpl->particle.trailType == EFFECT_TRAIL_SEGMENT ? tmpl->particle.trailSegment : -1;
	const bool smoker = trail >= 0 && trail != index && trail < effect->segments.Num();
	int kept = 0;
	for ( int i = 0; i < particles.Num(); i++ ) {
		if ( smoker ) {
			EmitTrail( effect, particles[i], time );
		}
		bool remove = infinite ? effect->stopped : ( particles[i].endTime - BSE_TIME_EPSILON <= time );
		if ( effect->stopped && !persist ) {
			remove = true;
		}
		if ( !remove ) {
			if ( kept != i ) {
				particles[kept] = particles[i];
			}
			kept++;
		}
	}
	particles.SetNum( kept, false );
	return kept > 0;
}

/*
===============================================================================

	idBseEffect

===============================================================================
*/

/*
================
idBseEffect::Init
================
*/
void idBseEffect::Init( const bseEffectParms_t &parms, float time ) {
	owner = parms;
	decl = parms.effect;
	startTime = parms.startTime;
	duration = 0.0f;
	lastTime = currentTime = time;
	looping = parms.loop;
	ambient = parms.ambient;
	stopped = false;
	finished = false;
	inConnectedArea = parms.inConnectedArea;
	boundsRadius = decl->GetBoundsRadius( BSE_GameGravity() );
	ownerAttenuation = 1.0f;
	shortestDistance = originDistance = 0.0f;

	originalOrigin = currentOrigin = lastOrigin = parms.origin;
	currentAxis = parms.axis;
	currentVelocity.Zero();
	hasEndOrigin = parms.hasEndOrigin;
	originalEndOrigin = currentEndOrigin = parms.hasEndOrigin ? parms.endOrigin : vec3_origin;
	worldBounds.Clear();
	particleBounds.Clear();
	boundsDirty = false;
	UpdateFromOwner( parms, time );
	lastTime = currentTime = time;
	endOriginChanged = hasEndOrigin && decl->UsesEndOrigin();

	StartSegments( time );
}

/*
================
idBseEffect::StartSegments
================
*/
void idBseEffect::StartSegments( float time ) {
	segments.SetNum( decl->GetNumSegments() );
	for ( int i = 0; i < segments.Num(); i++ ) {
		segments[i].Init( this, i, time );
	}
	for ( int i = 0; i < segments.Num(); i++ ) {
		segments[i].CalcBudget( this, time );
	}
	// named trails: sized by the segments that leave them, whichever order they come in
	for ( int i = 0; i < segments.Num(); i++ ) {
		const idBseSegment &segment = segments[i];
		const idEffectParticle &particle = segment.tmpl->particle;
		const int trail = particle.trailType == EFFECT_TRAIL_SEGMENT ? particle.trailSegment : -1;
		if ( trail >= 0 && trail != i && segment.tmpl->hasParticles &&
				( segment.tmpl->type == EFFECT_SEG_EMITTER || segment.tmpl->type == EFFECT_SEG_SPAWNER ) ) {
			segments[trail].AddTrailBudget( segment.budgetOnce, segment.budgetLooping, segment.trailDuration );
		}
	}
	for ( int i = 0; i < segments.Num(); i++ ) {
		if ( segments[i].tmpl->type == EFFECT_SEG_TRAIL ) {
			segments[i].budget = looping ? segments[i].budgetLooping : segments[i].budgetOnce;
		}
	}

	// an ambient looping effect (placed in the map) is already in full flow when first seen
	if ( looping && ambient && duration != 0.0f ) {
		while ( time - 2.0f * duration > startTime ) {
			startTime += duration;
			for ( int i = 0; i < segments.Num(); i++ ) {
				segments[i].segStartTime += duration;
				segments[i].lastTime = segments[i].segStartTime;
			}
		}
		currentTime -= 2.0f * duration;
		startTime -= 2.0f * duration;
		for ( int i = 0; i < segments.Num(); i++ ) {
			segments[i].Rewind( this );
		}
	}
}

/*
================
idBseEffect::UpdateFromOwner

The owner's new state. The bounds only grow: particles left behind stay inside them.
================
*/
void idBseEffect::UpdateFromOwner( const bseEffectParms_t &parms, float time ) {
	owner = parms;
	lastTime = currentTime;
	lastOrigin = currentOrigin;
	currentTime = time;
	currentOrigin = parms.origin;
	currentAxis = parms.axis;
	const float dt = currentTime - lastTime;
	if ( dt > 0.002f ) {
		currentVelocity = ( currentOrigin - lastOrigin ) * ( 1.0f / dt );
	}
	gravity = parms.gravity;
	inConnectedArea = parms.inConnectedArea;

	const idVec3 half( boundsRadius, boundsRadius, boundsRadius );
	worldBounds.AddPoint( currentOrigin + half );
	worldBounds.AddPoint( currentOrigin - half );
	endOriginChanged = false;
	if ( hasEndOrigin && decl->UsesEndOrigin() ) {
		endOriginChanged = parms.endOrigin != currentEndOrigin || currentOrigin != lastOrigin;
		currentEndOrigin = parms.endOrigin;
		worldBounds.AddPoint( currentEndOrigin + half );
		worldBounds.AddPoint( currentEndOrigin - half );
	} else {
		currentEndOrigin = parms.endOrigin;
	}
	localBounds.Clear();
	for ( int i = 0; i < 8; i++ ) {
		const idVec3 corner( worldBounds[i & 1].x, worldBounds[( i >> 1 ) & 1].y, worldBounds[( i >> 2 ) & 1].z );
		localBounds.AddPoint( currentAxis.Transpose() * ( corner - currentOrigin ) );
	}

	tint.Set( parms.shaderParms[0], parms.shaderParms[1], parms.shaderParms[2], parms.shaderParms[3] );
	brightness = parms.shaderParms[6];
	ownerAttenuation = parms.attenuation;
}

/*
================
idBseEffect::UpdateAttenuation

Distances from the player's view: to the origin, and the shortest to the effect's bounds.
================
*/
void idBseEffect::UpdateAttenuation( void ) {
	if ( !decl->Attenuates() ) {
		return;
	}
	idVec3 view;
	if ( !BSE_ViewOrigin( world, view ) ) {
		shortestDistance = originDistance = 0.0f;
		return;
	}
	originDistance = idMath::ClampFloat( 1.0f, 131072.0f, ( currentOrigin - view ).LengthFast() );
	const idVec3 localView = currentAxis.Transpose() * ( view - currentOrigin );
	shortestDistance = idMath::ClampFloat( 1.0f, 131072.0f, localBounds.ShortestDistance( localView ) );
}

/*
================
idBseEffect::SegmentAttenuation

Full within the near distance (+1), fading linearly to nothing at the far one (-1); the owner's
attenuation scales it. A segment without distances takes the owner's.
================
*/
float idBseEffect::SegmentAttenuation( const idEffectSegment &segment ) const {
	const effectRange_t &range = segment.attenuation;
	if ( range.min <= 0.0f && range.max <= 0.0f ) {
		return ownerAttenuation;
	}
	if ( range.min + 1.0f > shortestDistance ) {
		return ownerAttenuation;
	}
	if ( range.max - 1.0f >= shortestDistance ) {
		return ( 1.0f - ( shortestDistance - range.min ) / ( range.max - range.min ) ) * ownerAttenuation;
	}
	return 0.0f;
}

/*
================
idBseEffect::UpdateParticleBounds

Where the live particles are, in the current frame, each grown by its reach: its largest size
(a model's times the model's own reach), its length, a bolt's forks and jitter, and where its
trail was. With the radius about the origin, what the effect's entity is culled by. Quake 4
bounds a played effect by the geometry it drew; the retail radius alone is half a particle's
furthest reach.
================
*/
void idBseEffect::UpdateParticleBounds( float time ) {
	particleBounds.Clear();
	for ( int i = 0; i < segments.Num(); i++ ) {
		const idBseSegment &segment = segments[i];
		const idEffectParticle &tmplP = segment.tmpl->particle;
		const idDeclTable *sizeTable = tmplP.parms[EFFECT_PARM_SIZE].envelope.table;
		const idDeclTable *lengthTable = tmplP.parms[EFFECT_PARM_LENGTH].envelope.table;
		const float sizeTableMax = sizeTable != NULL ? Max( 1.0f, sizeTable->GetMaxValue() ) : 1.0f;
		const float lengthTableMax = lengthTable != NULL ? Max( 1.0f, lengthTable->GetMaxValue() ) : 1.0f;
		float modelReach = 1.0f;
		if ( tmplP.type == EFFECT_PARTICLE_MODEL ) {
			const srfTriangles_t *mesh = BSE_ModelGeometry( tmplP.model );
			modelReach = mesh != NULL ? Max( mesh->bounds[0].Length(), mesh->bounds[1].Length() ) : 0.0f;
		}
		const bool trails = tmplP.trailType == EFFECT_TRAIL_MOTION || tmplP.trailType == EFFECT_TRAIL_BURN;
		for ( int j = 0; j < segment.particles.Num(); j++ ) {
			const bseParticle_t &p = segment.particles[j];
			const idVec3 pos = EvaluatePosition( p, tmplP, time );
			float extent = 0.0f;
			for ( int k = 0; k < 3; k++ ) {
				extent = Max( extent, Max( idMath::Fabs( p.sizeStart[k] ), idMath::Fabs( p.sizeEnd[k] ) ) );
			}
			extent *= sizeTableMax * modelReach;
			const float length = Max( p.lengthStart.Length(), p.lengthEnd.Length() ) * lengthTableMax;
			extent += length;
			if ( tmplP.type == EFFECT_PARTICLE_ELECTRICITY ) {
				// forks end within 20 units of the middle of their way to the end; the jitter
				// wanders with the square root of the bolts
				const float bolts = idMath::ClampFloat( 3.0f, (float)BSE_MAX_BOLTS, idMath::Ceil( length * 0.0625f ) );
				extent += 35.0f + tmplP.jitterSize.Length() * 2.0f * idMath::Sqrt( bolts );
			}
			particleBounds.AddPoint( pos - idVec3( extent, extent, extent ) );
			particleBounds.AddPoint( pos + idVec3( extent, extent, extent ) );
			if ( trails && p.trailTime > 0.0f ) {
				const idVec3 back = EvaluatePosition( p, tmplP, Max( p.startTime, time - p.trailTime ) );
				particleBounds.AddPoint( back - idVec3( extent, extent, extent ) );
				particleBounds.AddPoint( back + idVec3( extent, extent, extent ) );
			}
		}
	}
}

void idBseEffect::UpdateBounds( void ) {
	if ( boundsDirty ) {
		UpdateParticleBounds( currentTime );
		boundsDirty = false;
	}
}

// the effect lasts at least this long (a negative time: the decl's shortest)
void idBseEffect::SetDuration( float time ) {
	if ( time < 0.0f ) {
		duration = decl->GetMinDuration();
	} else if ( duration < time ) {
		duration = time;
	}
}

// a new loop: every segment but the constant ones takes a new window
void idBseEffect::LoopSegments( float time ) {
	for ( int i = 0; i < segments.Num(); i++ ) {
		if ( !( segments[i].tmpl->flags & EFFECT_SEGMENT_CONSTANT ) && segments[i].tmpl->enabled ) {
			segments[i].InitTime( this, time );
		}
	}
	if ( BSE_Debug() ) {
		common->Printf( "BSE: '%s' loops (%g s)\n", decl->GetName(), duration );
	}
}

/*
================
idBseEffect::Service
================
*/
bool idBseEffect::Service( float time ) {
	UpdateAttenuation();
	for ( int i = 0; i < segments.Num(); i++ ) {
		segments[i].Check( this, time );
	}

	if ( !stopped && looping && duration != 0.0f && startTime + duration < time ) {
		startTime += duration;
		duration = 0.0f;
		LoopSegments( startTime );
	}

	bool alive = false;
	for ( int i = 0; i < segments.Num(); i++ ) {
		if ( segments[i].UpdateParticles( this, time ) ) {
			alive = true;
		}
	}
	boundsDirty = true;		// taken when a scene needs them (UpdateBounds)

	if ( stopped ) {
		return !alive;
	}
	if ( looping ) {
		if ( duration == 0.0f && startTime + duration < time ) {
			// nothing timed the loop: start over a little after the decl's longest play
			startTime += decl->GetMaxDuration() + 0.5f;
			LoopSegments( startTime );
		}
		return false;
	}
	return startTime + duration < time;
}

/*
================
idBseEffect::EvaluateParticle

A particle's position (in the effect's current frame), colour, opacity, size and rotation at
time. Position: birth position, velocity, acceleration and friction (an exponential drift that
pushes early and pulls back once half t squared passes the life), plus the offset turned by the
orbit angles. Colour and the rest follow their envelopes.
================
*/
idVec3 idBseEffect::EvaluatePosition( const bseParticle_t &p, const idEffectParticle &tmplP, float time ) const {
	const float life = p.endTime - p.startTime;
	const float t = Max( 0.0f, time - p.startTime );
	const effectParm_t *parms = tmplP.parms;

	idVec3 position = p.initPos + p.velocity * t;
	if ( p.hasOffset ) {
		idVec3 angle, offset;
		BSE_EvaluateEnvelope( parms[EFFECT_PARM_ANGLE].envelope, p.angleStart.ToFloatPtr(), p.angleEnd.ToFloatPtr(), 3, t, p.oneOverLife, angle.ToFloatPtr() );
		BSE_EvaluateEnvelope( parms[EFFECT_PARM_OFFSET].envelope, p.offsetStart.ToFloatPtr(), p.offsetEnd.ToFloatPtr(), 3, t, p.oneOverLife, offset.ToFloatPtr() );
		const idAngles orbit( RAD2DEG( angle.x ), RAD2DEG( angle.y ), RAD2DEG( angle.z ) );
		position += orbit.ToMat3() * offset;
	}
	const float halfT2 = 0.5f * t * t;
	position += p.acceleration * halfT2;
	if ( p.friction.LengthSqr() != 0.0f ) {
		const float lifeLength = Max( BSE_TIME_EPSILON, life );
		position += p.friction * ( ( idMath::Exp( ( lifeLength - halfT2 ) / lifeLength ) - 1.0f ) * halfT2 * halfT2 * ( 1.0f / 3.0f ) );
	}
	if ( !p.locked ) {
		// from the frame at birth to the current one
		position = currentAxis.Transpose() * ( p.initOrigin + p.initAxis * position - currentOrigin );
	}
	return position;
}

bool idBseEffect::EvaluateParticle( const bseParticle_t &p, const idEffectParticle &tmplP, float time,
		idVec3 &position, idVec4 &outTint, idVec3 &size, idVec3 &rotate ) const {
	if ( !( time > p.startTime - BSE_TIME_EPSILON && time < p.endTime ) ) {
		return false;
	}
	float evalTime = time - p.startTime;
	if ( time >= p.endTime - BSE_TIME_EPSILON ) {
		evalTime = ( p.endTime - p.startTime ) - BSE_TIME_EPSILON;
	}
	const effectParm_t *parms = tmplP.parms;
	const effectParticleType_t type = tmplP.type;
	position = EvaluatePosition( p, tmplP, time );

	idVec3 tintNow;
	float fade;
	BSE_EvaluateEnvelope( parms[EFFECT_PARM_TINT].envelope, p.tintStart.ToFloatPtr(), p.tintEnd.ToFloatPtr(), 3, evalTime, p.oneOverLife, tintNow.ToFloatPtr() );
	BSE_EvaluateEnvelope( parms[EFFECT_PARM_FADE].envelope, &p.fadeStart, &p.fadeEnd, 1, evalTime, p.oneOverLife, &fade );
	outTint.Set( tintNow.x, tintNow.y, tintNow.z, fade );
	size.Set( 1.0f, 1.0f, 1.0f );
	BSE_EvaluateEnvelope( parms[EFFECT_PARM_SIZE].envelope, p.sizeStart.ToFloatPtr(), p.sizeEnd.ToFloatPtr(),
		EffectParmComponents( type, EFFECT_PARM_SIZE ), evalTime, p.oneOverLife, size.ToFloatPtr() );
	rotate.Zero();
	BSE_EvaluateEnvelope( parms[EFFECT_PARM_ROTATE].envelope, p.rotateStart.ToFloatPtr(), p.rotateEnd.ToFloatPtr(),
		EffectParmComponents( type, EFFECT_PARM_ROTATE ), evalTime, p.oneOverLife, rotate.ToFloatPtr() );
	return true;
}

/*
===============================================================================

	geometry

	A segment's particles alive at a time, built for a viewer in the effect's frame - the way
	Quake 4 builds each kind (docs/quake4-bse.md 3.2). The vertex colour carries the owner's
	colour, the particle's and its opacity: an additive particle fades through its colour and
	stays opaque, the others fade through alpha. A fully faded particle still draws.

===============================================================================
*/

idDrawVert &idBseGeometry::AddVert( const idVec3 &xyz, float s, float t, const byte rgba[4] ) {
	idDrawVert &v = verts.Alloc();
	v.Clear();
	v.xyz = xyz;
	v.st.Set( s, t );
	v.color[0] = rgba[0];
	v.color[1] = rgba[1];
	v.color[2] = rgba[2];
	v.color[3] = rgba[3];
	bounds.AddPoint( xyz );
	return v;
}

// a colour channel: rounded, and wrapping past 0-1 as retail's bytes do (a fade of 100 wraps)
byte idBseEffect::ColorByte( float x ) const {
	const float v = idMath::ClampFloat( -1.0e9f, 1.0e9f, x * 255.0f );
	return (byte)( (int)lrintf( v ) & 0xFF );
}

// override: a burn trail's copies fade with their age
void idBseEffect::ParticleColor( const idEffectParticle &tmplP, const idVec4 &color, float override, byte rgba[4] ) const {
	const bool additive = ( tmplP.flags & EFFECT_PARTICLE_BLEND_ADD ) != 0;
	const float rgbScale = additive ? color.w * override * brightness : brightness;
	const float alpha = additive ? 1.0f : tint.w * color.w * override;
	rgba[0] = ColorByte( tint.x * color.x * rgbScale );
	rgba[1] = ColorByte( tint.y * color.y * rgbScale );
	rgba[2] = ColorByte( tint.z * color.z * rgbScale );
	rgba[3] = ColorByte( alpha );
}

// a vector of a particle's own frame in the effect's current frame (an unlocked particle keeps
// the frame it was born in)
idVec3 idBseEffect::ToCurrentFrame( const bseParticle_t &p, const idVec3 &v ) const {
	return p.locked ? v : currentAxis.Transpose() * ( p.initAxis * v );
}

/*
================
idBseEffect::EvaluateVelocity

The derivative Quake 4 takes of the motion, in the particle's own frame: a named trail's
particles and a "generatedLine" line point along it.
================
*/
idVec3 idBseEffect::EvaluateVelocity( const bseParticle_t &p, float time ) const {
	const float t = Max( 0.0f, time - p.startTime );
	idVec3 velocity = p.velocity + p.acceleration * t;
	if ( p.friction.LengthSqr() != 0.0f ) {
		const float life = Max( BSE_TIME_EPSILON, p.endTime - p.startTime );
		velocity += p.friction * ( 0.5f * t * t * ( 1.0f + idMath::Exp( ( life - t ) / life ) * ( 1.0f - t * ( 1.0f / 3.0f ) ) ) );
	}
	return velocity;
}

// a sprite or an oriented quad: the image's corners on the four points, turned 45 degrees
static void BSE_AddQuad( idBseGeometry &geo, const idVec3 corners[4], const byte rgba[4], const idVec3 &normal, const idVec3 &left, const idVec3 &up ) {
	static const float st[4][2] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
	const int base = geo.NumVerts();
	for ( int k = 0; k < 4; k++ ) {
		idDrawVert &v = geo.AddVert( corners[k], st[k][0], st[k][1], rgba );
		v.normal = normal;
		v.tangents[0] = -left;
		v.tangents[1] = -up;
	}
	geo.AddTri( base + 0, base + 1, base + 2 );
	geo.AddTri( base + 0, base + 2, base + 3 );
}

// a length that is not a number would spread through a whole bolt
static float BSE_SafeNormalize( idVec3 &v ) {
	const float lengthSqr = v.LengthSqr();
	if ( lengthSqr < 1e-12f ) {
		v.Zero();
		return 0.0f;
	}
	const float length = idMath::Sqrt( lengthSqr );
	v *= 1.0f / length;
	return length;
}

/*
===============================================================================

	electricity

	A bolt runs from the particle to the end of its length in steps of about 16 units (3-200):
	each step's point wanders off the line by a jitter that adds up along the bolt, scaled by the
	jitter table at that fraction of the way; each step is kinked twice over (nine short pieces
	that zigzag about it). Forks leave from random points of the bolt towards near the middle of
	the rest of its way. The ribbon is as wide as the size, turned square to the bolt and the
	viewer. The shape holds for the jitter rate (jitterRate 0: a new one every game tic, 60 a
	second, as retail draws it at most). A segment's bolts take at most retail's 16384 vertices.

===============================================================================
*/

class idBseBolt {
public:
	idBseGeometry *			geo;
	idRandom				random;
	idVec3					coords[BSE_MAX_BOLT_POINTS + 1];
	int						coordCount;
	idVec3					length;			// the main bolt's: every ribbon is turned by it
	idVec3					forward;
	idVec3					eye;
	idVec3					jitter;
	const idDeclTable *		jitterTable;
	float					size;
	float					step;
	float					fraction;
	byte					rgba[4];

	float					Rand( float min, float max ) { return min + ( max - min ) * random.RandomFloat(); }
	void					Segment( const idVec3 &start, float startFraction );
	void					Shape( idVec3 start, idVec3 end, int count, float startFraction, float endFraction );
	void					Branch( const idVec3 &start, const idVec3 &end );
};

// the bolt's sideways axis: level and square to it, +x when it is vertical
static idVec3 BSE_BoltLeft( const idVec3 &forward ) {
	const float planar = forward.x * forward.x + forward.y * forward.y;
	if ( planar == 0.0f ) {
		return idVec3( 1.0f, 0.0f, 0.0f );
	}
	const float inv = idMath::InvSqrt( planar );
	return idVec3( -forward.y * inv, forward.x * inv, 0.0f );
}

static int BSE_BoltCount( float length ) {
	return idMath::ClampInt( 3, BSE_MAX_BOLTS, (int)idMath::Ceil( length * 0.0625f ) );
}

// a pair of ribbon vertices; texture s runs along the bolt. A segment's bolts stop where retail's
// surface is full.
void idBseBolt::Segment( const idVec3 &start, float startFraction ) {
	if ( geo->NumVerts() + 2 > BSE_MAX_BOLT_VERTS ) {
		return;
	}
	idVec3 offset = length.Cross( eye );
	BSE_SafeNormalize( offset );
	offset *= size;
	const float s = startFraction * step + fraction;
	geo->AddVert( start + offset, s, 0.0f, rgba );
	geo->AddVert( start - offset, s, 1.0f, rgba );
}

// kinks a piece about a third and two thirds of the way along, out and back, count times over
void idBseBolt::Shape( idVec3 start, idVec3 end, int count, float startFraction, float endFraction ) {
	while ( count >= 1 ) {
		const float down1 = Rand( 0.05f, 0.09f );
		const float left1 = Rand( 0.05f, 0.09f );
		const float weight1 = Rand( 0.56f, 0.76f );
		const float down2 = Rand( -down1 - 0.02f, 0.02f - down1 );
		const float left2 = Rand( -left1 - 0.02f, 0.02f - left1 );
		const float weight2 = Rand( 0.23f, 0.43f );

		idVec3 dir = end - start;
		const float reach = BSE_SafeNormalize( dir ) * 0.7f;
		const idVec3 left = BSE_BoltLeft( dir );
		const idVec3 down = left.Cross( dir );
		const idVec3 point1 = start * weight1 + end * ( 1.0f - weight1 ) + left * ( left1 * reach ) + down * ( down1 * reach );
		const idVec3 point2 = start * weight2 + end * ( 1.0f - weight2 ) + left * ( left2 * reach ) + down * ( down2 * reach );
		const float mid0 = startFraction * ( 2.0f / 3.0f ) + endFraction * ( 1.0f / 3.0f );
		const float mid1 = startFraction * ( 1.0f / 3.0f ) + endFraction * ( 2.0f / 3.0f );

		Shape( start, point1, count - 1, startFraction, mid0 );
		Shape( point1, point2, count - 1, mid0, mid1 );
		count--;
		start = point2;
		startFraction = mid1;
	}
	Segment( start, startFraction );
}

// one branch, start to end; its points are kept for the forks
void idBseBolt::Branch( const idVec3 &start, const idVec3 &end ) {
	BSE_SafeNormalize( forward );
	const idVec3 left = BSE_BoltLeft( forward );
	const idVec3 down = left.Cross( forward );

	const int firstVert = geo->NumVerts();
	float at = step;
	idVec3 previous = start;
	idVec3 current = start;
	idVec3 wander( vec3_origin );
	int points = 0;
	bool more = true;
	while ( points < BSE_MAX_BOLT_POINTS ) {
		coords[points++] = previous;
		if ( 1.0f - step * 0.5f <= at ) {
			at = 1.0f;
			more = false;
		}
		wander += forward * Rand( -jitter.x, jitter.x );
		wander += left * Rand( -jitter.y, jitter.y );
		wander += down * Rand( -jitter.z, jitter.z );
		const float noise = jitterTable != NULL ? jitterTable->TableLookup( at ) : 0.0f;
		current = start + ( end - start ) * at + wander * noise;

		fraction = at - step;
		Shape( previous, current, 2, 0.0f, 1.0f );
		previous = current;
		at += step;
		if ( !more ) {
			break;
		}
	}
	if ( firstVert == geo->NumVerts() ) {
		coordCount = 0;
		return;
	}
	coords[points] = current;
	coordCount = points + 1;
	Segment( current, 1.0f );

	// pairs of (+side, -side) vertices; each quad splits along base..base+3
	for ( int base = firstVert; base < geo->NumVerts() - 2; base += 2 ) {
		geo->AddTri( base, base + 1, base + 3 );
		geo->AddTri( base, base + 3, base + 2 );
	}
}

void idBseEffect::BuildElectricity( const idEffectParticle &tmplP, const bseParticle_t &p, float time, const idVec3 &position,
		const idVec3 &length, float width, const byte rgba[4], const bseView_t &view, idBseGeometry &geo ) const {
	const float mainLength = length.Length();

	// the shape of this jitter period
	const float period = tmplP.jitterRate > 0.0f ? tmplP.jitterRate : ( 1.0f / 60.0f );
	const int epoch = (int)idMath::Floor( Max( 0.0f, time - p.startTime ) / period );
	idBseBolt bolt;
	bolt.geo = &geo;
	bolt.random.SetSeed( (int)( ( (unsigned int)p.seed * 2654435761u ) ^ ( (unsigned int)epoch * 2246822519u ) ) & 0x7fffffff );
	bolt.coordCount = 0;
	bolt.length = length;
	bolt.forward = length;
	bolt.eye = view.origin;
	bolt.jitter = tmplP.jitterSize;
	bolt.jitterTable = tmplP.jitterTable;
	bolt.size = width;
	bolt.fraction = 0.0f;
	for ( int i = 0; i < 4; i++ ) {
		bolt.rgba[i] = rgba[i];
	}
	bolt.step = p.textureScale / (float)BSE_BoltCount( mainLength );
	if ( bolt.step <= BSE_TIME_EPSILON ) {
		bolt.step = 1.0f / (float)BSE_BoltCount( mainLength );
	}

	const idVec3 end = position + length;
	bolt.Branch( position, end );

	idVec3 bases[BSE_MAX_FORKS];
	const int forks = idMath::ClampInt( 0, BSE_MAX_FORKS, tmplP.fork );
	for ( int i = 0; i < forks; i++ ) {
		if ( bolt.coordCount > 2 ) {
			bases[i] = bolt.coords[1 + bolt.random.RandomInt( bolt.coordCount - 2 )];
		} else {
			bases[i] = position;
		}
	}
	for ( int i = 0; i < forks; i++ ) {
		const idVec3 mid = ( bases[i] + end ) * 0.5f;
		const idVec3 forkEnd( mid.x + bolt.Rand( -20.0f, 20.0f ), mid.y + bolt.Rand( -20.0f, 20.0f ), mid.z + bolt.Rand( -20.0f, 20.0f ) );
		const idVec3 dir = forkEnd - bases[i];
		const float forkLength = dir.Length();
		if ( forkLength <= 1.0f || forkLength >= mainLength ) {
			continue;
		}
		bolt.forward = dir;
		bolt.step = 1.0f / (float)BSE_BoltCount( forkLength );
		bolt.Branch( bases[i], forkEnd );
	}
}

/*
================
idBseEffect::BuildParticle

One particle at time, by its kind:
  - sprite: four corners at the position minus and plus the viewer's left and up axes (turned by
    the rotation, scaled by the size);
  - oriented: the same quad on the plane its three rotation angles (and a generated normal) turn
    to;
  - line: a ribbon from the position along its length, as wide as the size, turned to the
    viewer; the texture repeats along it by the tiling; "generatedLine" lays it along the
    direction of travel;
  - electricity: a bolt along the length (above);
  - linked: a pair of points, the size either side of the position along the viewer's left
    axis, joined to the previous particle's pair: the segment's particles make one strip;
  - model: the model's first surface turned by the rotation, scaled per axis by the size, drawn
    with the particle's material.
A segment's geometry stops at retail's surface size: 10000 vertices and 30000 indexes (a bolt's
16384 vertices); the particles past it are not drawn.
================
*/
bool idBseEffect::BuildParticle( const idBseSegment &segment, const bseParticle_t &p, float time, float override,
		const bseView_t &view, idBseGeometry &geo ) const {
	const idEffectParticle &tmplP = segment.tmpl->particle;

	// a particle that does not fit retail's surface is not drawn (a bolt is cut where it is full)
	int needVerts = 4, needIndexes = 6;
	if ( tmplP.type == EFFECT_PARTICLE_LINKED ) {
		needVerts = 2;
	} else if ( tmplP.type == EFFECT_PARTICLE_MODEL ) {
		const srfTriangles_t *mesh = BSE_ModelGeometry( tmplP.model );
		needVerts = mesh != NULL ? mesh->numVerts : 0;
		needIndexes = mesh != NULL ? mesh->numIndexes : 0;
	}
	if ( tmplP.type != EFFECT_PARTICLE_ELECTRICITY &&
			( geo.NumVerts() + needVerts > BSE_MAX_SURFACE_VERTS || geo.NumIndexes() + needIndexes > BSE_MAX_SURFACE_INDEXES ) ) {
		return false;
	}

	idVec3 pos, size, rotate;
	idVec4 color;
	if ( !EvaluateParticle( p, tmplP, time, pos, color, size, rotate ) ) {
		return false;
	}
	byte rgba[4];
	ParticleColor( tmplP, color, override, rgba );
	const idVec3 facing = view.up.Cross( view.left );

	switch ( tmplP.type ) {
		case EFFECT_PARTICLE_SPRITE: {
			float s, c;
			idMath::SinCos( rotate.x, s, c );
			const idVec3 right = ( view.left * c - view.up * s ) * size.x;
			const idVec3 up = ( view.left * s + view.up * c ) * size.y;
			const idVec3 corners[4] = { pos - right, pos - up, pos + right, pos + up };
			BSE_AddQuad( geo, corners, rgba, facing, view.left, view.up );
			return true;
		}
		case EFFECT_PARTICLE_ORIENTED: {
			const idMat3 axis = idAngles( RAD2DEG( rotate.x ), RAD2DEG( rotate.y ), RAD2DEG( rotate.z ) ).ToMat3();
			const idVec3 right = axis[1] * -size.x;
			const idVec3 up = axis[2] * size.y;
			const idVec3 corners[4] = { pos - right, pos - up, pos + right, pos + up };
			BSE_AddQuad( geo, corners, rgba, axis[0], axis[1], axis[2] );
			return true;
		}
		case EFFECT_PARTICLE_LINE:
		case EFFECT_PARTICLE_ELECTRICITY: {
			float evalTime = time - p.startTime;
			if ( time >= p.endTime - BSE_TIME_EPSILON ) {
				evalTime = ( p.endTime - p.startTime ) - BSE_TIME_EPSILON;
			}
			idVec3 length;
			BSE_EvaluateEnvelope( tmplP.parms[EFFECT_PARM_LENGTH].envelope, p.lengthStart.ToFloatPtr(), p.lengthEnd.ToFloatPtr(), 3,
				evalTime, p.oneOverLife, length.ToFloatPtr() );
			length = ToCurrentFrame( p, length );
			if ( tmplP.flags & EFFECT_PARTICLE_GENERATED_LINE ) {
				// along the direction of travel (that of the particle's own frame, as retail); none
				// without motion
				idVec3 direction = EvaluateVelocity( p, time );
				const float magnitude = length.Length();
				length = BSE_SafeNormalize( direction ) > 0.0f ? direction * magnitude : vec3_origin;
			}
			if ( tmplP.type == EFFECT_PARTICLE_ELECTRICITY ) {
				if ( length.Length() < 0.1f ) {
					return false;
				}
				BuildElectricity( tmplP, p, time, pos, length, size.x, rgba, view, geo );
				return true;
			}
			const idVec3 end = pos + length;
			idVec3 side = length.Cross( view.origin - ( pos + length * 0.5f ) );
			if ( side.LengthSqr() > 1e-8f ) {
				side.Normalize();
			}
			side *= size.x;
			const int base = geo.NumVerts();
			geo.AddVert( pos + side, 0.0f, 0.0f, rgba ).normal = facing;
			geo.AddVert( pos - side, 0.0f, 1.0f, rgba ).normal = facing;
			geo.AddVert( end - side, p.textureScale, 1.0f, rgba ).normal = facing;
			geo.AddVert( end + side, p.textureScale, 0.0f, rgba ).normal = facing;
			geo.AddTri( base + 0, base + 1, base + 2 );
			geo.AddTri( base + 0, base + 2, base + 3 );
			return true;
		}
		case EFFECT_PARTICLE_LINKED: {
			const idVec3 up = view.left * size.x;
			const float s = p.fraction * p.textureScale;
			const int base = geo.NumVerts();
			geo.AddVert( pos + up, s, 0.0f, rgba ).normal = facing;
			geo.AddVert( pos - up, s, 1.0f, rgba ).normal = facing;
			if ( base > 0 ) {
				geo.AddTri( base - 2, base - 1, base );
				geo.AddTri( base - 1, base + 1, base );
			}
			return true;
		}
		case EFFECT_PARTICLE_MODEL: {
			const srfTriangles_t *mesh = BSE_ModelGeometry( tmplP.model );
			if ( mesh == NULL || mesh->numVerts <= 0 || mesh->verts == NULL || mesh->indexes == NULL ) {
				return false;
			}
			const idMat3 rotation = idAngles( RAD2DEG( rotate.x ), RAD2DEG( rotate.y ), RAD2DEG( rotate.z ) ).ToMat3();
			const int base = geo.NumVerts();
			for ( int i = 0; i < mesh->numVerts; i++ ) {
				const idDrawVert &src = mesh->verts[i];
				idVec3 v = ToCurrentFrame( p, rotation * src.xyz );
				v.x *= size.x;		// scaled along the effect's axes, after the turn, as retail
				v.y *= size.y;
				v.z *= size.z;
				idDrawVert &dst = geo.AddVert( pos + v, src.st.x, src.st.y, rgba );
				dst.normal = ToCurrentFrame( p, rotation * src.normal );
				dst.tangents[0] = ToCurrentFrame( p, rotation * src.tangents[0] );
				dst.tangents[1] = ToCurrentFrame( p, rotation * src.tangents[1] );
			}
			for ( int i = 0; i + 2 < mesh->numIndexes; i += 3 ) {
				geo.AddTri( base + mesh->indexes[i], base + mesh->indexes[i + 1], base + mesh->indexes[i + 2] );
			}
			return true;
		}
		default:
			// decals, lights and debris are not drawn here (S4, S5)
			return false;
	}
}

/*
================
idBseEffect::BuildSegment

A segment's particles in its draw order; a "trailType burn" particle also draws copies of itself
as it was over its trail time, fainter with age.
================
*/
int idBseEffect::BuildSegment( const idBseSegment &segment, float time, const bseView_t &view, idBseGeometry &geo ) const {
	const idEffectParticle &tmplP = segment.tmpl->particle;
	const bool infinite = ( segment.tmpl->flags & EFFECT_SEGMENT_CONSTANT ) != 0;
	const bool burn = tmplP.trailType == EFFECT_TRAIL_BURN;
	int drawn = 0;
	for ( int i = 0; i < segment.particles.Num(); i++ ) {
		bseParticle_t p = segment.particles[i];
		if ( infinite && time >= p.endTime - BSE_TIME_EPSILON ) {
			p.endTime = time + 1.0f;	// a constant segment's particles live on
		}
		if ( !BuildParticle( segment, p, time, 1.0f, view, geo ) ) {
			continue;
		}
		drawn++;
		if ( burn && p.trailCount > 0 && p.trailTime > 0.0f ) {
			const float delta = p.trailTime / (float)p.trailCount;
			for ( int k = 1; k <= p.trailCount; k++ ) {
				const float then = time - (float)k * delta;
				if ( then < p.startTime || then >= p.endTime ) {
					continue;
				}
				BuildParticle( segment, p, then, (float)( p.trailCount - k ) / (float)p.trailCount, view, geo );
			}
		}
	}
	return drawn;
}

/*
================
idBseEffect::BuildMotionTrails

A "trailType motion" particle's ribbon: its positions back over its trail time (or its age), as
wide as its size, turned square to its velocity and the viewer, fading out towards the oldest.
Quake 4's own arithmetic, quirks included: the turn uses the birth velocity and position, and
each step draws the previous step's position (the first two coincide).
================
*/
int idBseEffect::BuildMotionTrails( const idBseSegment &segment, float time, const bseView_t &view, idBseGeometry &geo ) const {
	const idEffectParticle &tmplP = segment.tmpl->particle;
	if ( tmplP.trailType != EFFECT_TRAIL_MOTION ) {
		return 0;
	}
	const effectParm_t *parms = tmplP.parms;
	const bool infinite = ( segment.tmpl->flags & EFFECT_SEGMENT_CONSTANT ) != 0;
	int drawn = 0;
	for ( int i = 0; i < segment.particles.Num(); i++ ) {
		const bseParticle_t &p = segment.particles[i];
		if ( p.trailCount <= 0 || time < p.startTime - BSE_TIME_EPSILON || ( !infinite && time >= p.endTime ) ) {
			continue;
		}
		const float from = Max( time - p.trailTime, p.startTime );
		const float delta = time - from;
		if ( delta <= BSE_TIME_EPSILON ) {
			continue;
		}
		if ( geo.NumVerts() + ( p.trailCount + 1 ) * 2 > BSE_MAX_SURFACE_VERTS || geo.NumIndexes() + p.trailCount * 6 > BSE_MAX_SURFACE_INDEXES ) {
			break;		// retail's trail surface is full
		}
		const float evalTime = Max( 0.0f, time - p.startTime );
		idVec3 tintNow, size( 1.0f, 1.0f, 1.0f );
		float fade;
		BSE_EvaluateEnvelope( parms[EFFECT_PARM_TINT].envelope, p.tintStart.ToFloatPtr(), p.tintEnd.ToFloatPtr(), 3, evalTime, p.oneOverLife, tintNow.ToFloatPtr() );
		BSE_EvaluateEnvelope( parms[EFFECT_PARM_FADE].envelope, &p.fadeStart, &p.fadeEnd, 1, evalTime, p.oneOverLife, &fade );
		BSE_EvaluateEnvelope( parms[EFFECT_PARM_SIZE].envelope, p.sizeStart.ToFloatPtr(), p.sizeEnd.ToFloatPtr(),
			EffectParmComponents( tmplP.type, EFFECT_PARM_SIZE ), evalTime, p.oneOverLife, size.ToFloatPtr() );

		const idVec3 toView = view.origin - p.initPos;
		idVec3 across = p.velocity.Cross( toView );
		if ( across.LengthSqr() <= 1e-6f ) {
			across = idVec3( 0.0f, 1.0f, 0.0f ).Cross( toView );
		}
		if ( across.LengthSqr() > 1e-6f ) {
			across.Normalize();
		}
		const idVec3 halfWidth = across * ( size.x * 0.5f );

		idVec3 position = EvaluatePosition( p, tmplP, time );
		for ( int k = 0; k <= p.trailCount; k++ ) {
			const float t = k < p.trailCount ? (float)k / (float)p.trailCount : 1.0f;
			const idVec4 color( tintNow.x, tintNow.y, tintNow.z, k < p.trailCount ? fade * ( 1.0f - t ) : 0.0f );
			byte rgba[4];
			ParticleColor( tmplP, color, 1.0f, rgba );
			const int base = geo.NumVerts();
			geo.AddVert( position + halfWidth, 0.0f, t, rgba );
			geo.AddVert( position - halfWidth, 1.0f, t, rgba );
			if ( k > 0 ) {
				geo.AddTri( base - 2, base - 1, base );
				geo.AddTri( base - 1, base, base + 1 );
			}
			if ( k < p.trailCount ) {
				position = EvaluatePosition( p, tmplP, time - t * delta );
			}
		}
		drawn++;
	}
	return drawn;
}

int idBseEffect::CountParticles( void ) const {
	int n = 0;
	for ( int i = 0; i < segments.Num(); i++ ) {
		n += segments[i].particles.Num();
	}
	return n;
}
