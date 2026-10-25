/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: Quake 4's effects (Raven's BSE) - the runtime's simulation: segment timing, spawning,
// spawn domains, envelopes and particle evaluation. No renderer here (BseRender.cpp draws), so the
// offline test (tools/quake4/test_bse_sim.sh) runs this file as the engine does.
//
// Behaviour follows Quake 4's own effect system as docs/quake4-bse.md 3.1 sets it out; its
// sources: id's FX editor pages for what the keywords mean, and for the arithmetic (spawn
// timing, distance attenuation, sampler shapes, envelope rates, particle motion, sprite corners,
// colour packing, draw order, loops, durations, bounds) OpenQ4's src/bse, consulted under the
// decision of docs/quake4-port.md 9.2 and re-expressed here.

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
	budget = 0;
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
A non-looping effect lasts until the segment's last particle can die.
================
*/
void idBseSegment::CalcBudget( idBseEffect *effect, float time ) {
	budget = 0;
	if ( tmpl->type == EFFECT_SEG_TRAIL || !tmpl->enabled ) {
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
	budget = idMath::ClampInt( 0, BSE_ParticleCap(), effect->looping ? countLooping : countOnce );

	if ( !effect->looping ) {
		effect->SetDuration( ( segEndTime - time ) + particle.duration.max );
	}
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
start, the count truncated (a range a,b gives a to b-1 particles). Other segment kinds act once
per loop (phases S4-S6).
================
*/
bool idBseSegment::Check( idBseEffect *effect, float time ) {
	// the spawn clock moves on with every service, even before the segment starts
	const float previous = lastTime;
	lastTime = time;

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
void idBseSegment::Spawn( idBseEffect *effect, float birthTime, float fraction ) {
	if ( particles.Num() >= budget ) {
		return;		// as in retail: no free particle, no spawn
	}
	bseParticle_t p;
	SpawnParticle( effect, p, birthTime, fraction );
	if ( p.endTime - BSE_TIME_EPSILON <= effect->currentTime && !( tmpl->flags & EFFECT_SEGMENT_CONSTANT ) ) {
		return;		// already over (an ambient effect's history): takes no slot
	}
	Insert( p );
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

/*
================
idBseSegment::SpawnParticle

A particle's birth. Velocity, acceleration and friction come first; then its position, with a
generated normal when the particle asks for one: the normal turns the velocity (authored along
+x) to point away from the domain's centre (or the origin). Acceleration is authored relative to
the direction of travel and friction relative to the acceleration's. An unlocked particle keeps
the effect's frame at its birth time, so the effect can move away from it.
================
*/
void idBseSegment::SpawnParticle( idBseEffect *effect, bseParticle_t &p, float birthTime, float fraction ) {
	const idEffectParticle &tmplP = tmpl->particle;
	const effectParm_t *parms = tmplP.parms;
	const effectParticleType_t type = tmplP.type;

	memset( &p, 0, sizeof( p ) );
	p.startTime = birthTime;
	p.fraction = fraction;
	p.locked = ( tmpl->flags & EFFECT_SEGMENT_LOCKED ) != 0;

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

	if ( wantNormal != NULL ) {
		normal.Normalize();
		p.velocity = normal.ToMat3() * p.velocity;
	}
	if ( tmplP.flags & EFFECT_PARTICLE_FLIP_NORMAL ) {
		p.velocity = -p.velocity;
	}
	if ( p.velocity.LengthSqr() != 0.0f ) {
		normal = p.velocity;
		normal.Normalize();
	}
	p.acceleration = normal.ToMat3() * p.acceleration;
	if ( p.acceleration.LengthSqr() != 0.0f ) {
		normal = p.acceleration;
		normal.Normalize();
	}
	p.friction = normal.ToMat3() * p.friction;

	if ( p.locked ) {
		p.initAxis.Identity();
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
		{ EFFECT_PARM_LENGTH, &p.lengthStart, &p.lengthEnd, 0.0f, 0.0f },
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

	// angles are turns in the file
	p.rotateStart *= idMath::TWO_PI;
	p.rotateEnd *= idMath::TWO_PI;
	p.angleStart *= idMath::TWO_PI;
	p.angleEnd *= idMath::TWO_PI;

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
}

/*
================
idBseSegment::UpdateParticles

Drops the particles that are over by time - all of a stopped effect's but those that persist. A
constant segment's particles live as long as the effect.
================
*/
bool idBseSegment::UpdateParticles( idBseEffect *effect, float time ) {
	const bool infinite = ( tmpl->flags & EFFECT_SEGMENT_CONSTANT ) != 0;
	const bool persist = ( tmpl->particle.flags & EFFECT_PARTICLE_PERSIST ) != 0;
	int kept = 0;
	for ( int i = 0; i < particles.Num(); i++ ) {
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
	UpdateFromOwner( parms, time );
	lastTime = currentTime = time;

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
	currentEndOrigin = parms.endOrigin;
	if ( hasEndOrigin && decl->UsesEndOrigin() ) {
		worldBounds.AddPoint( currentEndOrigin + half );
		worldBounds.AddPoint( currentEndOrigin - half );
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

Where the live particles are, in the current frame, each grown by its largest size: with the
radius about the origin, what the effect's entity is culled by. Quake 4 bounds a played effect
by the geometry it drew; the retail radius alone is half a particle's furthest reach.
================
*/
void idBseEffect::UpdateParticleBounds( float time ) {
	particleBounds.Clear();
	for ( int i = 0; i < segments.Num(); i++ ) {
		const idBseSegment &segment = segments[i];
		const idEffectParticle &tmplP = segment.tmpl->particle;
		const idDeclTable *sizeTable = tmplP.parms[EFFECT_PARM_SIZE].envelope.table;
		const float tableMax = sizeTable != NULL ? Max( 1.0f, sizeTable->GetMaxValue() ) : 1.0f;
		for ( int j = 0; j < segment.particles.Num(); j++ ) {
			const bseParticle_t &p = segment.particles[j];
			const idVec3 pos = EvaluatePosition( p, tmplP, time );
			float extent = 0.0f;
			for ( int k = 0; k < 3; k++ ) {
				extent = Max( extent, Max( idMath::Fabs( p.sizeStart[k] ), idMath::Fabs( p.sizeEnd[k] ) ) );
				extent = Max( extent, Max( idMath::Fabs( p.lengthStart[k] ), idMath::Fabs( p.lengthEnd[k] ) ) );
			}
			extent *= tableMax;
			particleBounds.AddPoint( pos - idVec3( extent, extent, extent ) );
			particleBounds.AddPoint( pos + idVec3( extent, extent, extent ) );
		}
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
	UpdateParticleBounds( time );

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

// a colour channel: rounded, and wrapping past 0-1 as retail's bytes do (a fade of 100 wraps)
static byte BSE_ColorByte( float x ) {
	const float v = idMath::ClampFloat( -1.0e9f, 1.0e9f, x * 255.0f );
	return (byte)( (int)lrintf( v ) & 0xFF );
}

/*
================
idBseEffect::BuildSprites

The particles alive at time as sprites: four corners at the position minus and plus the
viewer's left and up axes (turned by the rotation and scaled by the size), the texture's corners
on them - a sprite is its image turned 45 degrees, size being the centre to corner distance.
The vertex colour carries the owner's colour, the particle's and its opacity: an additive
particle fades through its colour and stays opaque, the others fade through alpha. A fully faded
particle still draws.
================
*/
int idBseEffect::BuildSprites( const idBseSegment &segment, float time, const idVec3 &viewLeft, const idVec3 &viewUp,
		idDrawVert *verts, int maxQuads, idBounds &bounds ) const {
	const idEffectParticle &tmplP = segment.tmpl->particle;
	const bool additive = ( tmplP.flags & EFFECT_PARTICLE_BLEND_ADD ) != 0;
	const idVec3 normal = viewUp.Cross( viewLeft );
	const bool infinite = ( segment.tmpl->flags & EFFECT_SEGMENT_CONSTANT ) != 0;
	int quads = 0;
	for ( int i = 0; i < segment.particles.Num() && quads < maxQuads; i++ ) {
		bseParticle_t p = segment.particles[i];
		if ( infinite && time >= p.endTime - BSE_TIME_EPSILON ) {
			p.endTime = time + 1.0f;	// a constant segment's particles live on
		}
		idVec3 pos, size, rotate;
		idVec4 color;
		if ( !EvaluateParticle( p, tmplP, time, pos, color, size, rotate ) ) {
			continue;
		}

		float s, c;
		idMath::SinCos( rotate.x, s, c );
		const idVec3 right = ( viewLeft * c - viewUp * s ) * size.x;
		const idVec3 up = ( viewLeft * s + viewUp * c ) * size.y;

		const float rgbScale = additive ? color.w * brightness : brightness;
		const float alpha = additive ? 1.0f : tint.w * color.w;
		byte rgba[4];
		rgba[0] = BSE_ColorByte( tint.x * color.x * rgbScale );
		rgba[1] = BSE_ColorByte( tint.y * color.y * rgbScale );
		rgba[2] = BSE_ColorByte( tint.z * color.z * rgbScale );
		rgba[3] = BSE_ColorByte( alpha );

		static const float st[4][2] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
		const idVec3 corners[4] = { pos - right, pos - up, pos + right, pos + up };
		idDrawVert *v = verts + quads * 4;
		for ( int k = 0; k < 4; k++ ) {
			v[k].Clear();
			v[k].xyz = corners[k];
			v[k].st.Set( st[k][0], st[k][1] );
			v[k].normal = normal;
			v[k].tangents[0] = -viewLeft;
			v[k].tangents[1] = -viewUp;
			v[k].color[0] = rgba[0];
			v[k].color[1] = rgba[1];
			v[k].color[2] = rgba[2];
			v[k].color[3] = rgba[3];
			bounds.AddPoint( corners[k] );
		}
		quads++;
	}
	return quads;
}

int idBseEffect::CountParticles( void ) const {
	int n = 0;
	for ( int i = 0; i < segments.Num(); i++ ) {
		n += segments[i].particles.Num();
	}
	return n;
}
