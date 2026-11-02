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
#   set gather : the ReSTIR DI gather's cost split in EMISSIVE MODE (needs the diag/di-gather-profile build: r_rtDiGatherProfile
#                bits each remove one piece - timing only, the image is wrong) - 7 variants; your authored emissive setups are
#                copied into every sandbox (demo1 = game/mc_underground)
#   set boostgi : the ReSTIR DI boost reusing the plain gather's GI term (perf/restir-di-boost-reuse-gi, r_rtReStirDIBoostReuseGI),
#                emissive mode as in 'gather' - A = the old re-trace, B = the reuse
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
#      DRYRUN=1  print the command lines, launch nothing
set -euo pipefail

REPO="$( cd "$( dirname "$0" )/../.." && pwd )"
BIN="${DUDE_BIN:-$REPO/build/dude}"
DUDE_CFG="${DUDE_CFG:-$HOME/.config/dude/base/dude.cfg}"
DEMO="${DEMO:-demo1}"
COOLDOWN="${COOLDOWN:-30}"
WARMUPS="${WARMUPS:-2}"
SET="${1:-}"
ROUNDS="${2:-3}"

declare -a NAMES
declare -A DESC ARGS BINV		# BINV: a variant's own binary (default: \$BIN) - for A/B of two builds
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
	*)
		sed -n '2,32p' "$0"
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
mkdir -p "$OUT"
{
	echo "set: $SET  rounds: $ROUNDS  warm-ups: $WARMUPS  cooldown: ${COOLDOWN}s  demo: $DEMO  preset: $PRESET  dude_rtx: $RTX"
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
		echo "seta dude_cfgSchema \"$SCHEMA\""
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
		+timeDemoQuit "$DEMO" )
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
