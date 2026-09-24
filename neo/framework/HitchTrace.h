// TEMPORARY diagnostic (benchmark hitch hunt, 2026-09-24; docs/benchmark.md): r_hitchTrace N
// prints any instrumented step that takes >= N ms, and the benchmark marks frames >= N ms with a
// per-phase split, so a stall in demo1 can be attributed. Remove once the culprit is known.
#ifndef __HITCHTRACE_H__
#define __HITCHTRACE_H__

#include "sys/platform.h"
#include "sys/sys_public.h"
#include "framework/CVarSystem.h"

extern idCVar r_hitchTrace;
void R_HitchTrace( const char *step, const char *what, double ms );
// per-frame sums of every step (regardless of the print threshold), drained by the benchmark's
// long-frame marker so bursts of sub-threshold work (many small pipelines / uploads) show up
void R_HitchTraceFrameSummary( idStr &out );

// scoped timer: reports the enclosing block on exit
class idHitchScope {
public:
	idHitchScope( const char *step, const char *what ) : step( step ), what( what ), t0( Sys_MillisecondsPrecise() ) {}
	~idHitchScope() { R_HitchTrace( step, what, Sys_MillisecondsPrecise() - t0 ); }
private:
	const char *step;
	const char *what;
	double t0;
};

#endif
