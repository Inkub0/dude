/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: Quake 4's effects (Raven's BSE) - the console tools. bseLog and bseStats are retail's
// commands (their help texts and output lines are the retail executable's strings; what they
// count is DUDE's reading of them). bseSweep is DUDE's: it plays every effect in front of the view,
// a batch at a time, and reports what did not play, what did not parse and which batches logged
// warnings - the Quake 4 phase gate's sweep (docs/quake4-bse.md 4, S7).

#include "sys/platform.h"
#include "framework/Common.h"
#include "framework/CmdSystem.h"
#include "framework/DeclManager.h"
#include "framework/Session.h"
#include "idlib/containers/StrList.h"
#include "renderer/RenderWorld.h"
#include "renderer/bse/BseEffect.h"

/*
===============================================================================

	bseLog: how often each effect played and looped since the game started

===============================================================================
*/

typedef struct {
	int		plays;
	int		loops;			// of effects freed since; the live ones' are added when listed
} bsePlayLog_t;

static idList<bsePlayLog_t>	bsePlayLog;		// by decl index

static bsePlayLog_t &BSE_PlayLogFor( const idDeclEffect *decl ) {
	const int index = decl->Index();
	if ( index >= bsePlayLog.Num() ) {
		const int old = bsePlayLog.Num();
		bsePlayLog.SetNum( index + 1 );
		for ( int i = old; i < bsePlayLog.Num(); i++ ) {
			bsePlayLog[i].plays = bsePlayLog[i].loops = 0;
		}
	}
	return bsePlayLog[index];
}

void BSE_ToolsEffectPlayed( const idDeclEffect *decl ) {
	BSE_PlayLogFor( decl ).plays++;
}

void BSE_ToolsEffectFreed( const idBseEffect *effect ) {
	BSE_PlayLogFor( effect->decl ).loops += effect->loops;
}

static void BSE_Log_f( const idCmdArgs &args ) {
	// the live effects' loops so far, on top of the freed ones'
	idList<int> liveLoops;
	liveLoops.SetNum( bsePlayLog.Num() );
	for ( int i = 0; i < liveLoops.Num(); i++ ) {
		liveLoops[i] = 0;
	}
	const idList<idBseEffect *> &playing = BSE_PlayingEffects();
	for ( int i = 0; i < playing.Num(); i++ ) {
		const int index = playing[i]->decl->Index();
		if ( index < liveLoops.Num() ) {
			liveLoops[index] += playing[i]->loops;
		}
	}
	const int numDecls = declManager->GetNumDecls( DECL_EFFECT );
	int played = 0;
	for ( int i = 0; i < bsePlayLog.Num() && i < numDecls; i++ ) {
		const bsePlayLog_t &log = bsePlayLog[i];
		const int loops = log.loops + liveLoops[i];
		if ( log.plays == 0 && loops == 0 ) {
			continue;
		}
		const idDecl *decl = declManager->DeclByIndex( DECL_EFFECT, i, false );
		common->Printf( "%d plays (%d loops): '%s'\n", log.plays, loops, decl != NULL ? decl->GetName() : "?" );
		played++;
	}
	common->Printf( "%d effects played or looped out of %d\n", played, numDecls );
}

/*
===============================================================================

	bseStats [all]: what the loaded effect decls hold

===============================================================================
*/

static void BSE_Stats_f( const idCmdArgs &args ) {
	const int numDecls = declManager->GetNumDecls( DECL_EFFECT );
	if ( args.Argc() > 1 && !idStr::Icmp( args.Argv( 1 ), "all" ) ) {
		common->Printf( "Processing %d effect decls....\n", numDecls );
		for ( int i = 0; i < numDecls; i++ ) {
			declManager->DeclByIndex( DECL_EFFECT, i, true );
		}
	}
	common->Printf( "... processing %d registered effects\n", numDecls );

	int loaded = 0, neverReferenced = 0, segments = 0, withParticles = 0;
	float particles = 0.0f;
	for ( int i = 0; i < numDecls; i++ ) {
		const idDeclEffect *decl = static_cast<const idDeclEffect *>( declManager->DeclByIndex( DECL_EFFECT, i, false ) );
		if ( decl == NULL || decl->GetState() == DS_UNPARSED ) {
			continue;
		}
		loaded++;
		if ( !decl->EverReferenced() ) {
			neverReferenced++;
		}
		for ( int s = 0; s < decl->GetNumSegments(); s++ ) {
			const idEffectSegment &segment = decl->GetSegment( s );
			segments++;
			if ( !segment.hasParticles || !segment.enabled ) {
				continue;
			}
			withParticles++;
			// the most a play of it has alive at once: a spawner's count; an emitter's (or a trail's)
			// rate over the shorter of its window and a particle's life
			const float count = segment.count.max;
			if ( segment.type == EFFECT_SEG_SPAWNER ) {
				particles += count;
			} else {
				particles += count * Min( segment.duration.max, segment.particle.duration.max );
			}
		}
	}
	common->Printf( "%d segments in %d loaded effects (%d never referenced)\n", segments, loaded, neverReferenced );
	common->Printf( "%.2f segments per effect\n", loaded > 0 ? (float)segments / loaded : 0.0f );
	common->Printf( "%.2f of segments have particles\n", segments > 0 ? (float)withParticles / segments : 0.0f );
	common->Printf( "%.2f particles per segment with particles\n", withParticles > 0 ? particles / withParticles : 0.0f );
}

/*
===============================================================================

	bseSweep [<filter>] [quit] | bseSweep stop

	Plays every effect (or those whose name contains the filter) once, in batches laid out in
	front of the view, as testEffect would, each for its play up to a few seconds. Between
	batches it notes what the batch did: effects that did not play, the warnings the engine gave
	while they did (the log has their text just above), the most particles one effect had alive.
	A summary ends it; "quit" then quits (for +bseSweep from the command line).

===============================================================================
*/

const int		BSE_SWEEP_BATCH = 12;			// effects at once: 4 x 3 in front of the view
const int		BSE_SWEEP_SHOW_MSEC = 2500;		// each is shown this long at most (then dies out)
const int		BSE_SWEEP_GRACE_MSEC = 2000;	// then the batch's last particles get this long

typedef struct {
	bool								active;
	bool								quit;
	const idRenderWorld *				world;
	idList<const idDeclEffect *>		effects;
	int									next;
	int									batches;
	int									batchEndMs;
	int									warningsAtBatch;
	idList<const idDeclEffect *>		batch;
	int									startMs;
	int									notPlayed;
	int									notParsed;
	int									warningBatches;
	int									mostParticles;
	idStr								mostParticlesName;
	idStrList							notPlayedNames;
} bseSweep_t;

static bseSweep_t	bseSweep;

static int BSE_SweepCompare( const idDeclEffect * const *a, const idDeclEffect * const *b ) {
	return idStr::Icmp( ( *a )->GetName(), ( *b )->GetName() );
}

static void BSE_Sweep_f( const idCmdArgs &args ) {
	if ( args.Argc() > 1 && !idStr::Icmp( args.Argv( 1 ), "stop" ) ) {
		if ( bseSweep.active ) {
			BSE_ClearTestEffects();
			bseSweep.active = false;
			common->Printf( "bseSweep: stopped after %d of %d effects\n", bseSweep.next, bseSweep.effects.Num() );
		}
		return;
	}
	idRenderWorld *world = session != NULL ? session->rw : NULL;
	if ( world == NULL || BSE_ShownView( world ) == NULL ) {
		common->Printf( "bseSweep: no map is shown - start one first\n" );
		return;
	}
	idStr filter;
	bool quit = false;
	for ( int i = 1; i < args.Argc(); i++ ) {
		if ( !idStr::Icmp( args.Argv( i ), "quit" ) ) {
			quit = true;
		} else {
			filter = args.Argv( i );
		}
	}

	BSE_ClearTestEffects();
	bseSweep.active = true;
	bseSweep.quit = quit;
	bseSweep.world = world;
	bseSweep.effects.Clear();
	bseSweep.batch.Clear();
	bseSweep.next = bseSweep.batches = 0;
	bseSweep.batchEndMs = -1;
	bseSweep.notPlayed = bseSweep.notParsed = bseSweep.warningBatches = bseSweep.mostParticles = 0;
	bseSweep.mostParticlesName.Clear();
	bseSweep.notPlayedNames.Clear();
	bseSweep.startMs = Sys_Milliseconds();

	// every effect, parsed now (a decl that fails becomes the default effect: counted, not played)
	const int warningsBefore = Com_NumWarnings();
	const int numDecls = declManager->GetNumDecls( DECL_EFFECT );
	for ( int i = 0; i < numDecls; i++ ) {
		const idDeclEffect *decl = static_cast<const idDeclEffect *>( declManager->DeclByIndex( DECL_EFFECT, i, true ) );
		if ( decl == NULL || ( filter.Length() && idStr::FindText( decl->GetName(), filter, false ) < 0 ) ) {
			continue;
		}
		if ( decl->GetState() == DS_DEFAULTED ) {
			bseSweep.notParsed++;
			common->Printf( "bseSweep: '%s' did not parse\n", decl->GetName() );
			continue;
		}
		bseSweep.effects.Append( decl );
	}
	bseSweep.effects.Sort( BSE_SweepCompare );
	common->Printf( "bseSweep: %d effects to play, %d at a time, %g s each at most%s (bseSweep stop ends it)\n",
		bseSweep.effects.Num(), BSE_SWEEP_BATCH, MS2SEC( BSE_SWEEP_SHOW_MSEC ), quit ? ", then quit" : "" );
	if ( Com_NumWarnings() > warningsBefore ) {
		common->Printf( "bseSweep: parsing them gave %d warnings (above)\n", Com_NumWarnings() - warningsBefore );
	}
}

// what a batch did, once it is over
static void BSE_SweepEndBatch( void ) {
	const int warnings = Com_NumWarnings() - bseSweep.warningsAtBatch;
	if ( warnings > 0 ) {
		bseSweep.warningBatches++;
		idStr names;
		for ( int i = 0; i < bseSweep.batch.Num(); i++ ) {
			names += i ? ", " : "";
			names += bseSweep.batch[i]->GetName();
		}
		common->Printf( "bseSweep: %d warnings (above) while playing: %s\n", warnings, names.c_str() );
	}
	BSE_ClearTestEffects();
	bseSweep.batch.Clear();
}

/*
================
BSE_SweepFrame

Before each scene of the swept world: the batch's particle counts, and the next batch once this
one is over (or past its time).
================
*/
void BSE_SweepFrame( idRenderWorld *world, int timeMs ) {
	if ( !bseSweep.active || world != bseSweep.world ) {
		return;
	}
	// the most particles one effect of the batch has alive
	const idList<idBseEffect *> &playing = BSE_PlayingEffects();
	for ( int i = 0; i < playing.Num(); i++ ) {
		if ( bseSweep.batch.FindIndex( playing[i]->decl ) < 0 ) {
			continue;
		}
		const int particles = playing[i]->CountParticles();
		if ( particles > bseSweep.mostParticles ) {
			bseSweep.mostParticles = particles;
			bseSweep.mostParticlesName = playing[i]->decl->GetName();
		}
	}
	if ( bseSweep.batchEndMs >= 0 ) {
		if ( BSE_TestEffectsPlaying() && timeMs < bseSweep.batchEndMs + BSE_SWEEP_GRACE_MSEC ) {
			return;		// still playing
		}
		BSE_SweepEndBatch();
	}

	if ( bseSweep.next >= bseSweep.effects.Num() ) {
		const float seconds = MS2SEC( Sys_Milliseconds() - bseSweep.startMs );
		common->Printf( "bseSweep: %d effects in %d batches, %.0f s: %d did not play, %d did not parse, %d batches gave warnings; most particles alive in one effect: %d (%s)\n",
			bseSweep.effects.Num(), bseSweep.batches, seconds, bseSweep.notPlayed, bseSweep.notParsed, bseSweep.warningBatches,
			bseSweep.mostParticles, bseSweep.mostParticlesName.Length() ? bseSweep.mostParticlesName.c_str() : "-" );
		for ( int i = 0; i < bseSweep.notPlayedNames.Num(); i++ ) {
			common->Printf( "bseSweep: did not play: %s\n", bseSweep.notPlayedNames[i].c_str() );
		}
		bseSweep.active = false;
		bseSweep.batchEndMs = -1;
		if ( bseSweep.quit ) {
			cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "quit\n" );
		}
		return;
	}

	const renderView_t *view = BSE_ShownView( world );
	if ( view == NULL ) {
		return;
	}
	// the next batch, 4 x 3 across the view, 120 units apart
	bseSweep.warningsAtBatch = Com_NumWarnings();
	for ( int k = 0; k < BSE_SWEEP_BATCH && bseSweep.next < bseSweep.effects.Num(); k++ ) {
		const idDeclEffect *decl = bseSweep.effects[bseSweep.next++];
		const idVec2 offset( ( ( k % 4 ) - 1.5f ) * 120.0f, ( ( k / 4 ) - 1.0f ) * 90.0f );
		if ( BSE_StartTestEffect( decl, *view, false, offset, BSE_SWEEP_SHOW_MSEC, false ) == NULL ) {
			bseSweep.notPlayed++;
			bseSweep.notPlayedNames.Append( decl->GetName() );
			continue;
		}
		bseSweep.batch.Append( decl );
	}
	bseSweep.batches++;
	bseSweep.batchEndMs = timeMs + BSE_SWEEP_SHOW_MSEC;
	if ( bseSweep.batches % 20 == 0 ) {
		common->Printf( "bseSweep: %d / %d effects\n", bseSweep.next, bseSweep.effects.Num() );
	}
}

/*
===============================================================================

	init / shutdown

===============================================================================
*/

void BSE_InitTools( void ) {
	cmdSystem->AddCommand( "bseLog", BSE_Log_f, CMD_FL_RENDERER, "Dumps the number of times an effect has been played since game start" );
	cmdSystem->AddCommand( "bseStats", BSE_Stats_f, CMD_FL_RENDERER, "Dumps the stats of every registered effect - use all to force parse every effect" );
	cmdSystem->AddCommand( "bseSweep", BSE_Sweep_f, CMD_FL_RENDERER | CMD_FL_CHEAT,
		"Quake 4 (DUDE): plays every effect in front of the view, a batch at a time, and reports what did not play or gave warnings - bseSweep [<name filter>] [quit] | bseSweep stop" );
}

void BSE_ShutdownTools( void ) {
	cmdSystem->RemoveCommand( "bseLog" );
	cmdSystem->RemoveCommand( "bseStats" );
	cmdSystem->RemoveCommand( "bseSweep" );
	bseSweep.active = false;
	bseSweep.effects.Clear();
	bseSweep.batch.Clear();
	bseSweep.notPlayedNames.Clear();
	bsePlayLog.Clear();
}
