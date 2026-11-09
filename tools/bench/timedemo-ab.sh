#!/usr/bin/env bash
# DUDE A/B timedemo bench (method: docs/rtx-sigma-shadows.md section 7).
#
# Runs `timeDemoQuit <demo>` once per variant of a SET, alternating the order every round
# (warm-ups, then ABCD / DCBA / ABCD ...). Every run gets its OWN fresh sandbox (config + savepath),
# so archived cvars from your dude.cfg never leak in; only the display settings are copied from it.
# nvidia-smi logs the GPU clocks once a second next to each run (the 3080 Ti throttles).
# Afterwards tools/bench/timedemo_ab_report.py prints per-variant numbers: the benchmark report
# (fps, frame time, 1% low, GPU ms) and the r_vkGpuTime 3 per-pass scopes.
#
# It LAUNCHES THE GAME fullscreen, ~1.5 min per run. Close DUDE first.
#
# Usage: tools/bench/timedemo-ab.sh <set> [rounds]
#   set m1     : ReSTIR DI boost knob split at Path Tracing Nightmare (4 variants)
#   set serial : the light-serial fix, two BINARIES at Path Tracing Nightmare - A = build/dude-noserialfix, B = build/dude
#   set load   : the timeDemo's map load - A = like a level load (com_timeDemoLevelLoad 1, the default), B = the old
#                first-sight streaming (0); Path Tracing Nightmare, emissive mode on
#   set shadermath : perf/restir-shader-math, two binaries - A / C = master (build/dude-ref-master), B / D = the branch
#                (build/dude); A / B emissive mode, C / D placed lights (emissive off). VARIANTS="A B" = the short run
#   set boostpicks : A = master (build/dude-ref-master), B = boost picks in the gather + grid skip (build/dude-items13),
#                C = B + candidate read-ahead (build/dude); emissive mode; NVIDIA register readout in each run's stdout.log
#   set animsig : A = master (build/dude-ref-master), B = the animated BLAS signature from the base model + surface ids/counts
#                (build/dude); emissive mode - compare rt.animblas and the "pre-scene per frame" builds
#   set v097   : the 0.9.8 release notes - A = the published 0.9.7 binary (dist/release-0.9.7), B = current master as a
#                RELEASE build (./build-linux-release.sh -> build-linux-release/dude); Path Tracing, Emissive Mode off
#   set v097em : the same with Emissive Mode on (each binary's own emissive defaults; the authored setups seeded)
#   set v098   : 0.9.8 vs now - A / C = the 0.9.8 release binary (dist/release-0.9.8), B / D = current master as a RELEASE
#                build (./build-linux-release.sh -> build-linux-release/dude). A / B Path Tracing (Emissive Mode off, the
#                default in both), C / D raster Medium on Vulkan, the CPU-bound probe where per-frame CPU work shows (the
#                buffer churn fixed after 0.9.8). VARIANTS="A B" or "C D" = the short runs
#   set memcheck : one run of build/dude, emissive mode - its stdout.log lists GPU-read buffers that landed in system RAM
#                (WARMUPS=0 ... memcheck 1)
#   set emitteraxis : A = master (build/dude-ref-master), B = emitter rows with a plain cone axis (build/dude); emissive mode
#   set registers : A = master (build/dude-ref-master), B = the gather with fewer live registers (build/dude); emissive
#                mode; NVIDIA register readout in each run's stdout.log
#   set bounceprof : the GI bounce's cost split, 8 variants of build/dude-bounceprof (r_rtgiBounceProfile bits; the image is
#                wrong - timing only); run it with 2 rounds
#   (gather / boostgi / bounceem are HISTORY - 2026-11-02: their cvars only existed in those branches' builds)
#   set gather : the ReSTIR DI gather's cost split in EMISSIVE MODE (needs the diag/di-gather-profile build: r_rtDiGatherProfile
#                bits each remove one piece - timing only, the image is wrong) - 7 variants; your authored emissive setups are
#                copied into every sandbox (demo1 = game/mc_underground)
#   set boostgi : the ReSTIR DI boost reusing the plain gather's GI term (perf/restir-di-boost-reuse-gi, r_rtReStirDIBoostReuseGI),
#                emissive mode as in 'gather' - A = the old re-trace, B = the reuse
#   set bounceem : the GI bounce hit's emitters (perf/rtgi-bounce-emitter-sampling, r_rtgiBounceEmitterPick), emissive mode as
#                in 'gather' - A = every emitter area-sampled (old), B = one picked by the oriented target, C = by stored importance
#   set cpusplit : where the RENDERER's CPU time goes (frontend vs backend), for the multi-core decision (docs: SMP /
#                parallel frontend, 2026-11-06). The RELEASE build (build-linux-release/dude: build/dude is -O0, its CPU side
#                means nothing). A = Path Tracing as played (emissive on), B = the same at FSR2 0.5 render scale (towards
#                CPU-bound), C = raster Medium on Vulkan, D = raster Medium on GL3 (vsync off, no fps cap: C / D are the
#                CPU-bound probes - there the backend figure is real CPU work, not a GPU wait). 2 rounds are enough.
#                E-H = the Potato preset (the vanilla floor: stencil shadows built on the CPU), for a GPU held at a low
#                power limit: E / F native resolution (Vulkan / GL3), G / H 1280x720 windowed (Vulkan / GL3) - the GPU
#                nearly idle, so the CPU is the limit: VARIANTS="E F G H"
#   rounds : alternating rounds after the warm-up (default 3)
# Env: DUDE_BIN  binary (default <repo>/build/dude)
#      DUDE_CFG  your config, source of the display settings (default ~/.config/dude/base/dude.cfg)
#      DEMO      demo name (default demo1)
#      COOLDOWN  seconds idle between runs (default 30)
#      WARMUPS   discarded warm-up runs before the rounds (default 2: the 3080 Ti still runs cooler and faster on
#                the run right after a single warm-up - 2026-10-31, 1340 -> 1221 -> ~1120 MHz)
#      OUT       result directory (default ~/.local/share/dude/bench/<date>-<set>)
#      VARIANTS  run only these variants of the set, in this order (e.g. VARIANTS="A D")
#      EMISSIVE_DIR  authored emissive setups copied into every sandbox for sets that need them (default
#                ~/.local/share/dude/base/emissive: the plain ./build/dude save path; run.sh saves under the repo's base/)
#      TWICE     0 (default) = one timed pass after the engine's level-load emulation (com_timeDemoLevelLoad 1: the map,
#                its entities and the player precached like a level load - play-like streaming); 1 = `timeDemoQuit <demo>
#                twice`, an un-timed pass first (everything the demo shows warm)
#      DRYRUN=1  print the command lines, launch nothing
set -euo pipefail

REPO="$( cd "$( dirname "$0" )/../.." && pwd )"
BIN="${DUDE_BIN:-$REPO/build/dude}"
DUDE_CFG="${DUDE_CFG:-$HOME/.config/dude/base/dude.cfg}"
DEMO="${DEMO:-demo1}"
COOLDOWN="${COOLDOWN:-30}"
TWICE="${TWICE:-0}"
TWICE_ARG=""
if [ "$TWICE" = 1 ]; then TWICE_ARG="twice"; fi
WARMUPS="${WARMUPS:-2}"
SET="${1:-}"
ROUNDS="${2:-3}"

declare -a NAMES
declare -A DESC ARGS BINV		# BINV: a variant's own binary (default: \$BIN) - for A/B of two builds
declare -A SCHEMAV				# SCHEMAV: a variant's own config schema (default: this tree's) - for a binary of an older release
SEED_EMISSIVE=0					# 1 = copy the authored emissive/<map>.cfg files into every sandbox
EMISSIVE_DIR="${EMISSIVE_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/dude/base/emissive}"
case "$SET" in
	m1)
		# Path Tracing Nightmare (dude_rtx 2, preset 5); 0.9.6 had BoostHistory 2 / BoostConfidence 0
		PRESET=5; RTX=2
		NAMES=( A B C D )
		DESC[A]="0.9.7 defaults: BoostHistory 4, BoostConfidence 0.3"
		ARGS[A]="+set r_rtReStirDIBoostHistory 4 +set r_rtReStirDIBoostConfidence 0.3"
		DESC[B]="BoostHistory 2, BoostConfidence 0.3"
		ARGS[B]="+set r_rtReStirDIBoostHistory 2 +set r_rtReStirDIBoostConfidence 0.3"
		DESC[C]="BoostHistory 4, BoostConfidence 0"
		ARGS[C]="+set r_rtReStirDIBoostHistory 4 +set r_rtReStirDIBoostConfidence 0"
		DESC[D]="0.9.6 values: BoostHistory 2, BoostConfidence 0"
		ARGS[D]="+set r_rtReStirDIBoostHistory 2 +set r_rtReStirDIBoostConfidence 0"
		;;
	serial)
		# 2026-10-29: dudeSerial was reassigned on EVERY UpdateLightDef - a light moved / re-parmed each frame reset the
		# ReSTIR DI gradient's history on its footprint. Copy the build without the fix to build/dude-noserialfix first.
		PRESET=5; RTX=2
		NAMES=( A B )
		DESC[A]="without the light serial fix"
		BINV[A]="$REPO/build/dude-noserialfix"
		ARGS[A]=""
		DESC[B]="with the light serial fix"
		BINV[B]="$BIN"
		ARGS[B]=""
		;;
	gather)
		# 2026-11-02: where the ~6.7 ms ReSTIR DI gather goes in emissive mode (docs/rtx-experimental-lighting.md). Each bit of
		# r_rtDiGatherProfile REMOVES one piece; the drop vs A is its cost. Emissive mode as the user plays it (physical, 1 sample).
		PRESET=5; RTX=2; SEED_EMISSIVE=1
		EM="+set r_rtEmissiveLighting 1 +set r_rtEmissivePhysical 1 +set r_rtEmissiveSamples 1"
		NAMES=( A B C D E F G )
		DESC[A]="reference (nothing removed)";                ARGS[A]="$EM +set r_rtDiGatherProfile 0"
		DESC[B]="no GI bounce block (trace, hit shade, cache update)"; ARGS[B]="$EM +set r_rtDiGatherProfile 1"
		DESC[C]="no emitter rows at the bounce hit";          ARGS[C]="$EM +set r_rtDiGatherProfile 2"
		DESC[D]="no cache tail serve (every hit shaded live)"; ARGS[D]="$EM +set r_rtDiGatherProfile 4"
		DESC[E]="no DI shadow ray";                           ARGS[E]="$EM +set r_rtDiGatherProfile 8"
		DESC[F]="DI: first candidate only";                   ARGS[F]="$EM +set r_rtDiGatherProfile 16"
		DESC[G]="no cache update";                            ARGS[G]="$EM +set r_rtDiGatherProfile 32"
		;;
	boostgi)
		# 2026-11-02: the boost pass re-traced the GI bounce for every young pixel (2.4 ms of 3.8 in the 'gather' profile)
		PRESET=5; RTX=2; SEED_EMISSIVE=1
		EM="+set r_rtEmissiveLighting 1 +set r_rtEmissivePhysical 1 +set r_rtEmissiveSamples 1"
		NAMES=( A B )
		DESC[A]="boost re-traces the GI bounce (old)";   ARGS[A]="$EM +set r_rtReStirDIBoostReuseGI 0"
		DESC[B]="boost reuses the gather's GI term";     ARGS[B]="$EM +set r_rtReStirDIBoostReuseGI 1"
		;;
	bounceem)
		# 2026-11-02: an area sample for every emitter of the bounce hit's cell was ~3 ms of the gather ('gather' profile)
		PRESET=5; RTX=2; SEED_EMISSIVE=1
		EM="+set r_rtEmissiveLighting 1 +set r_rtEmissivePhysical 1 +set r_rtEmissiveSamples 1"
		NAMES=( A B C )
		DESC[A]="every emitter of the cell area-sampled (old)";  ARGS[A]="$EM +set r_rtgiBounceEmitterPick 0"
		DESC[B]="one emitter by the oriented target";           ARGS[B]="$EM +set r_rtgiBounceEmitterPick 1"
		DESC[C]="one emitter by the cell's stored importance";  ARGS[C]="$EM +set r_rtgiBounceEmitterPick 2"
		;;
	load)
		# 2026-11-02: com_timeDemoLevelLoad - the demo's map, entities and player precached like a level load vs first sight
		PRESET=5; RTX=2; SEED_EMISSIVE=1
		EM="+set r_rtEmissiveLighting 1 +set r_rtEmissivePhysical 1 +set r_rtEmissiveSamples 1"
		NAMES=( A B )
		DESC[A]="level load (com_timeDemoLevelLoad 1)";   ARGS[A]="$EM +set com_timeDemoLevelLoad 1"
		DESC[B]="first sight (com_timeDemoLevelLoad 0)";  ARGS[B]="$EM +set com_timeDemoLevelLoad 0"
		;;
	shadermath)
		# 2026-11-02 (perf/restir-shader-math): the same ReSTIR math, cheaper shaders - two BINARIES, each with its own
		# SPIR-V: A / C = master (build/dude-ref-master, its shaders embedded), B / D = this branch (build/dude). A / B in
		# emissive mode (your demo1 setup), C / D with emissive mode off (placed lights: the cookie-size path). Short run:
		# VARIANTS="A B"
		PRESET=5; RTX=2; SEED_EMISSIVE=1
		EM="+set r_rtEmissiveLighting 1 +set r_rtEmissivePhysical 1 +set r_rtEmissiveSamples 1"
		NAMES=( A B C D )
		DESC[A]="master shaders, emissive mode";        ARGS[A]="$EM";                          BINV[A]="$REPO/build/dude-ref-master"
		DESC[B]="refactored shaders, emissive mode";    ARGS[B]="$EM";                          BINV[B]="$BIN"
		DESC[C]="master shaders, placed lights";        ARGS[C]="+set r_rtEmissiveLighting 0";  BINV[C]="$REPO/build/dude-ref-master"
		DESC[D]="refactored shaders, placed lights";    ARGS[D]="+set r_rtEmissiveLighting 0";  BINV[D]="$BIN"
		;;
	boostpicks)
		# 2026-11-03: three BINARIES, emissive mode (demo1 setup), the drivers' register readout in every run's stdout.log
		# (r_vkPipelineStats restir_di: all variants alike, so the capture never favours one). A = master
		# (build/dude-ref-master), B = the boost's picks drawn by the plain gather + the light-grid fill skipping covered cells
		# (build/dude-items13), C = B + the candidate loops reading the next grid slot ahead (build/dude)
		PRESET=5; RTX=2; SEED_EMISSIVE=1
		EM="+set r_rtEmissiveLighting 1 +set r_rtEmissivePhysical 1 +set r_rtEmissiveSamples 1 +set r_vkPipelineStats restir_di"
		NAMES=( A B C )
		DESC[A]="master";                                   ARGS[A]="$EM";   BINV[A]="$REPO/build/dude-ref-master"
		DESC[B]="boost picks in the gather + grid skip";    ARGS[B]="$EM";   BINV[B]="$REPO/build/dude-items13"
		DESC[C]="B + candidate read-ahead";                 ARGS[C]="$EM";   BINV[C]="$BIN"
		;;
	animsig)
		# 2026-11-04 (perf/animblas-stable-signature): two BINARIES, emissive mode. A = master (build/dude-ref-master), B = the
		# animated BLAS topology signature without the snapshot model pointer / gpuSkinVB handles (build/dude): fewer rebuilds
		PRESET=5; RTX=2; SEED_EMISSIVE=1
		EM="+set r_rtEmissiveLighting 1 +set r_rtEmissivePhysical 1 +set r_rtEmissiveSamples 1"
		NAMES=( A B )
		DESC[A]="master";                                ARGS[A]="$EM";   BINV[A]="$REPO/build/dude-ref-master"
		DESC[B]="animated BLAS: stable signature";       ARGS[B]="$EM";   BINV[B]="$BIN"
		;;
	v097|v097em)
		# 2026-11-04: 0.9.7 vs now for the 0.9.8 release notes. Same assets for both (the repo's base, fs_basepath), so only the
		# engine differs; each binary keeps its own defaults (fresh config, 0.9.7's at its schema 3 - no one-time reset).
		# Master's timedemo streams on first sight like 0.9.7's (com_timeDemoLevelLoad 0: 0.9.7 has no level-load
		# emulation), so frame times and 1% lows compare the renderers, not the demo loader. B must be a release build:
		# build/dude is -O0 (the CPU side would not compare).
		PRESET=5; RTX=2
		EMV=""
		if [ "$SET" = v097em ]; then SEED_EMISSIVE=1; EMV="+set r_rtEmissiveLighting 1"; fi
		NAMES=( A B )
		DESC[A]="0.9.7 release";                ARGS[A]="$EMV";                                 BINV[A]="$REPO/dist/release-0.9.7/dude-0.9.7-linux-x86_64/dude"; SCHEMAV[A]=3
		DESC[B]="master, release build";        ARGS[B]="$EMV +set com_timeDemoLevelLoad 0";    BINV[B]="$REPO/build-linux-release/dude"
		;;
	v098)
		# 2026-11-09: 0.9.8 vs current master. Same assets for both (the repo's base, fs_basepath), so only the engine differs;
		# each binary keeps its own defaults (fresh config, 0.9.8's at its schema 4 - no one-time reset). Both have the timeDemo's
		# level-load emulation (com_timeDemoLevelLoad, ab7c91c6 is in 0.9.8), so it stays at its default. B / D must be a release
		# build: build/dude is -O0 (the CPU side would not compare).
		PRESET=5; RTX=2
		R098="$REPO/dist/release-0.9.8/dude-0.9.8-linux-x86_64/dude"
		RB="$REPO/build-linux-release/dude"
		RASTER="+set dude_rtx 0 +dudePreset 2"
		NAMES=( A B C D )
		DESC[A]="0.9.8 release, Path Tracing";                    ARGS[A]="";          BINV[A]="$R098"; SCHEMAV[A]=4
		DESC[B]="master (release build), Path Tracing";           ARGS[B]="";          BINV[B]="$RB"
		DESC[C]="0.9.8 release, raster Medium (CPU-bound probe)"; ARGS[C]="$RASTER";   BINV[C]="$R098"; SCHEMAV[C]=4
		DESC[D]="master (release build), raster Medium";          ARGS[D]="$RASTER";   BINV[D]="$RB"
		;;
	memcheck)
		# 2026-11-04: ONE run of build/dude in emissive mode for the Vulkan memory-placement warnings (a GPU-read buffer that
		# landed in system RAM - Resizable BAR off), read from the run's stdout.log: WARMUPS=0 tools/bench/timedemo-ab.sh memcheck 1
		PRESET=5; RTX=2; SEED_EMISSIVE=1
		NAMES=( A )
		DESC[A]="build/dude, emissive mode";  ARGS[A]="+set r_rtEmissiveLighting 1 +set r_rtEmissivePhysical 1 +set r_rtEmissiveSamples 1"
		;;
	emitteraxis)
		# 2026-11-03 (perf/rtgi-bounce-emitter-micro): two BINARIES, emissive mode. A = master (build/dude-ref-master, its shaders
		# embedded), B = emitter rows with a plain cone axis (build/dude)
		PRESET=5; RTX=2; SEED_EMISSIVE=1
		EM="+set r_rtEmissiveLighting 1 +set r_rtEmissivePhysical 1 +set r_rtEmissiveSamples 1"
		NAMES=( A B )
		DESC[A]="master";                         ARGS[A]="$EM";   BINV[A]="$REPO/build/dude-ref-master"
		DESC[B]="emitter cone axis stored plain"; ARGS[B]="$EM";   BINV[B]="$BIN"
		;;
	registers)
		# 2026-11-03 (perf/restir-gather-registers): two BINARIES, emissive mode, the drivers' register readout in every run's
		# stdout.log. A = master (build/dude-ref-master, its shaders embedded), B = the gather with fewer live registers (build/dude)
		PRESET=5; RTX=2; SEED_EMISSIVE=1
		EM="+set r_rtEmissiveLighting 1 +set r_rtEmissivePhysical 1 +set r_rtEmissiveSamples 1 +set r_vkPipelineStats restir_di"
		NAMES=( A B )
		DESC[A]="master";                    ARGS[A]="$EM";   BINV[A]="$REPO/build/dude-ref-master"
		DESC[B]="fewer live registers";      ARGS[B]="$EM";   BINV[B]="$BIN"
		;;
	bounceprof)
		# 2026-11-03: the GI bounce's cost split (diag/rtgi-bounce-profile, build/dude-bounceprof: r_rtgiBounceProfile bits each
		# remove one piece - timing only, the image is WRONG). Emissive mode as in boostpicks. 2 rounds are enough.
		PRESET=5; RTX=2; SEED_EMISSIVE=1
		EM="+set r_rtEmissiveLighting 1 +set r_rtEmissivePhysical 1 +set r_rtEmissiveSamples 1"
		PB="$REPO/build/dude-bounceprof"
		NAMES=( A B C D E F G H )
		DESC[A]="everything (profile 0)";              ARGS[A]="$EM +set r_rtgiBounceProfile 0";   BINV[A]="$PB"
		DESC[B]="no GI bounce at all";                 ARGS[B]="$EM +set r_rtgiBounceProfile 1";   BINV[B]="$PB"
		DESC[C]="bounce trace only, no hit shading";   ARGS[C]="$EM +set r_rtgiBounceProfile 2";   BINV[C]="$PB"
		DESC[D]="no placed lights at the hit";         ARGS[D]="$EM +set r_rtgiBounceProfile 4";   BINV[D]="$PB"
		DESC[E]="no emitters at the hit";              ARGS[E]="$EM +set r_rtgiBounceProfile 8";   BINV[E]="$PB"
		DESC[F]="no dominant shadow ray at the hit";   ARGS[F]="$EM +set r_rtgiBounceProfile 16";  BINV[F]="$PB"
		DESC[G]="no cache at the hit";                 ARGS[G]="$EM +set r_rtgiBounceProfile 32";  BINV[G]="$PB"
		DESC[H]="no cache update at the pixel";        ARGS[H]="$EM +set r_rtgiBounceProfile 64";  BINV[H]="$PB"
		;;
	cpusplit)
		# 2026-11-06: the renderer's CPU split for the multi-core decision. The backend timer includes the Vulkan frame-fence
		# wait (RB_RHI_ExecuteBackEndCommands times r->BeginFrame): in a GPU-bound run (A) "backend" is mostly waiting, in the
		# CPU-bound probes (C, D) it is real work. Release build for every variant.
		PRESET=5; RTX=2; SEED_EMISSIVE=1
		RB="$REPO/build-linux-release/dude"
		EM="+set r_rtEmissiveLighting 1 +set r_rtEmissivePhysical 1"
		NAMES=( A B C D )
		DESC[A]="Path Tracing Nightmare, emissive mode (as played)";        ARGS[A]="$EM";                                              BINV[A]="$RB"
		DESC[B]="the same at FSR2 0.5 render scale (towards CPU-bound)";    ARGS[B]="$EM +set r_fsr 1 +set r_fsrScale 0.5";             BINV[B]="$RB"
		DESC[C]="raster Medium, Vulkan (CPU-bound probe)";                  ARGS[C]="+set dude_rtx 0 +dudePreset 2";                     BINV[C]="$RB"
		DESC[D]="raster Medium, GL3 (CPU-bound probe)";                     ARGS[D]="+set r_graphicsAPI opengl3 +set dude_rtx 0 +dudePreset 2"; BINV[D]="$RB"
		# 2026-11-06: the GPU stays at a 160 W limit (it needs repasting): Potato, and a small window, keep it out of the way
		LOWRES="+set r_fullscreen 0 +set r_mode -1 +set r_customWidth 1280 +set r_customHeight 720"
		NAMES=( A B C D E F G H )
		DESC[E]="Potato, Vulkan, native resolution";                        ARGS[E]="+set dude_rtx 0 +dudePreset 0";                     BINV[E]="$RB"
		DESC[F]="Potato, GL3, native resolution";                           ARGS[F]="+set r_graphicsAPI opengl3 +set dude_rtx 0 +dudePreset 0"; BINV[F]="$RB"
		DESC[G]="Potato, Vulkan, 1280x720 window (CPU-bound)";              ARGS[G]="$LOWRES +set dude_rtx 0 +dudePreset 0";             BINV[G]="$RB"
		DESC[H]="Potato, GL3, 1280x720 window (CPU-bound)";                 ARGS[H]="$LOWRES +set r_graphicsAPI opengl3 +set dude_rtx 0 +dudePreset 0"; BINV[H]="$RB"
		;;
	*)
		sed -n '2,40p' "$0"
		exit 1
		;;
esac
if [ -n "${VARIANTS:-}" ]; then
	# shellcheck disable=SC2206
	pick=( $VARIANTS )
	for v in "${pick[@]}"; do
		[ -n "${ARGS[$v]+x}" ] || { echo "set $SET has no variant '$v' (has: ${NAMES[*]})" >&2; exit 1; }
	done
	NAMES=( "${pick[@]}" )
fi

if [ "${DRYRUN:-0}" != 1 ]; then
	for v in "${NAMES[@]}"; do
		b="${BINV[$v]:-$BIN}"
		[ -x "$b" ] || { echo "no binary at $b for variant $v (build first, or set DUDE_BIN)" >&2; exit 1; }
	done
	if pgrep -x dude > /dev/null; then
		echo "DUDE is running - close it first (two instances would share the GPU)" >&2
		exit 1
	fi
fi
[ -r "$DUDE_CFG" ] || { echo "cannot read $DUDE_CFG (display settings source)" >&2; exit 1; }
if [ "$SEED_EMISSIVE" = 1 ]; then
	ls "$EMISSIVE_DIR"/game/*.cfg > /dev/null 2>&1 || { echo "no authored emissive setups in $EMISSIVE_DIR/game (set EMISSIVE_DIR)" >&2; exit 1; }
fi
SCHEMA="$( sed -n 's/^#define DUDE_CONFIG_SCHEMA[[:space:]]*\([0-9]*\).*/\1/p' "$REPO/neo/framework/Licensee.h" )"

OUT="${OUT:-${XDG_DATA_HOME:-$HOME/.local/share}/dude/bench/$( date +%Y%m%d-%H%M%S )-$SET}"
# the NVIDIA driver must work: after a driver update without a reboot (2026-11-04: CachyOS update, "Driver/library version
# mismatch") its Vulkan driver fails and the game silently runs on the CPU's integrated GPU at 1-3 fps
if command -v nvidia-smi > /dev/null && [ "${DRYRUN:-0}" != 1 ]; then
	if ! smi_out="$( nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>&1 )"; then
		echo "nvidia-smi fails: $smi_out" >&2
		echo "the NVIDIA driver is not usable (after a driver update: reboot) - the game would run on another GPU; not starting" >&2
		exit 1
	fi
	echo "GPU: $smi_out"
fi

mkdir -p "$OUT"
{
	echo "set: $SET  rounds: $ROUNDS  warm-ups: $WARMUPS  cooldown: ${COOLDOWN}s  demo: $DEMO  preset: $PRESET  dude_rtx: $RTX  un-timed pass (twice): $TWICE"
	echo "binary: $BIN ($( stat -c '%y' "$BIN" 2> /dev/null || echo '?' ))"
	echo "repo: $( git -C "$REPO" rev-parse --short HEAD 2> /dev/null || echo '?' ) on $( git -C "$REPO" branch --show-current 2> /dev/null || echo '?' )"
	echo "display settings from $DUDE_CFG:"
	grep -E '^seta (r_mode|r_fullscreen|r_fullscreenDesktop|r_fullscreenDisplay|r_fullscreenDisplayName|r_customWidth|r_customHeight|r_displayRefresh) ' "$DUDE_CFG" || true
	for v in "${NAMES[@]}"; do echo "variant $v: ${DESC[$v]}  [${ARGS[$v]}]"; done
	if [ "$SEED_EMISSIVE" = 1 ]; then echo "emissive setups from $EMISSIVE_DIR: $( cd "$EMISSIVE_DIR" && ls game/*.cfg | tr '\n' ' ' )"; fi
	for v in "${NAMES[@]}"; do
		if [ -n "${BINV[$v]:-}" ]; then echo "binary $v: ${BINV[$v]} ($( stat -c '%y' "${BINV[$v]}" 2> /dev/null || echo '?' ))"; fi
	done
} > "$OUT/meta.txt"
cat "$OUT/meta.txt"

# run order: warm-up (first variant, discarded), then alternating rounds
order=()
for (( w = 0; w < WARMUPS; w++ )); do order+=( "w:${NAMES[0]}" ); done
for (( r = 1; r <= ROUNDS; r++ )); do
	if (( r % 2 )); then
		for v in "${NAMES[@]}"; do order+=( "$r:$v" ); done
	else
		for (( i = ${#NAMES[@]} - 1; i >= 0; i-- )); do order+=( "$r:${NAMES[$i]}" ); done
	fi
done

n=0
for item in "${order[@]}"; do
	round="${item%%:*}"
	v="${item#*:}"
	n=$(( n + 1 ))
	RUN="$( printf '%s/run-%02d-%s' "$OUT" "$n" "$v" )"
	SB="$RUN/sandbox"
	mkdir -p "$SB/cfg/base" "$SB/save"
	echo "$v" > "$RUN/variant"
	echo "$round" > "$RUN/round"
	# fresh config: current schema (no one-time reset), Vulkan, your display settings - nothing else
	{
		echo "seta dude_cfgSchema \"${SCHEMAV[$v]:-$SCHEMA}\""
		echo 'seta r_graphicsAPI "vulkan"'
		grep -E '^seta (r_mode|r_fullscreen|r_fullscreenDesktop|r_fullscreenDisplay|r_fullscreenDisplayName|r_customWidth|r_customHeight|r_displayRefresh) ' "$DUDE_CFG" || true
	} > "$SB/cfg/base/dude.cfg"
	if [ "$SEED_EMISSIVE" = 1 ]; then
		mkdir -p "$SB/save/base/emissive/game"
		cp "$EMISSIVE_DIR"/game/*.cfg "$SB/save/base/emissive/game/"		# the setups only, never a .recovery copy
	fi
	# command-line order matters: every +command runs after init, in this order (preset, then the
	# variant's overrides, then the demo)
	# shellcheck disable=SC2206
	args=( +set fs_basepath "$REPO" +set fs_savepath "$SB/save" +set fs_configpath "$SB/cfg"
		+set r_swapInterval 0 +set r_vrr 0 +set com_maxFPS 0 +set com_timeDemoRealTime 1
		+set dude_rtx "$RTX" +dudePreset "$PRESET"
		+set r_rtEmissiveLighting 0 +set r_vkGpuTime 3
		${ARGS[$v]}
		+timeDemoQuit "$DEMO" ${TWICE_ARG:+"$TWICE_ARG"} )
	echo "[$n/${#order[@]}] round $round variant $v: ${DESC[$v]}"
	if [ "${DRYRUN:-0}" = 1 ]; then
		echo "  ${BINV[$v]:-$BIN} ${args[*]}"
		continue
	fi
	nvidia-smi --query-gpu=timestamp,clocks.gr,clocks.mem,temperature.gpu,power.draw,clocks_event_reasons.active \
		--format=csv,noheader -lms 1000 > "$RUN/clocks.csv" 2> /dev/null &
	smi=$!
	set +e
	timeout 900 "${BINV[$v]:-$BIN}" "${args[@]}" > "$RUN/stdout.log" 2>&1
	rc=$?
	set -e
	kill "$smi" 2> /dev/null || true
	wait "$smi" 2> /dev/null || true
	echo "$rc" > "$RUN/exitcode"
	[ "$rc" = 0 ] || echo "  WARNING: exit code $rc (see $RUN/stdout.log)"
	sleep "$COOLDOWN"
done

[ "${DRYRUN:-0}" = 1 ] && exit 0
python3 "$REPO/tools/bench/timedemo_ab_report.py" "$OUT" | tee "$OUT/report.txt"
echo "results: $OUT"
