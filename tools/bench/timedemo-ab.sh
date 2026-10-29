#!/usr/bin/env bash
# DUDE A/B timedemo bench (method: docs/rtx-sigma-shadows.md section 7).
#
# Runs `timeDemoQuit <demo>` once per variant of a SET, alternating the order every round
# (warm-up, then ABCD / DCBA / ABCD ...). Every run gets its OWN fresh sandbox (config + savepath),
# so archived cvars from your dude.cfg never leak in; only the display settings are copied from it.
# nvidia-smi logs the GPU clocks once a second next to each run (the 3080 Ti throttles).
# Afterwards tools/bench/timedemo_ab_report.py prints per-variant numbers: the benchmark report
# (fps, frame time, 1% low, GPU ms) and the r_vkGpuTime 3 per-pass scopes.
#
# It LAUNCHES THE GAME fullscreen, ~1.5 min per run. Close DUDE first.
#
# Usage: tools/bench/timedemo-ab.sh <set> [rounds]
#   set m1 : ReSTIR DI boost knob split at Path Tracing Nightmare (4 variants)
#   rounds : alternating rounds after the warm-up (default 3)
# Env: DUDE_BIN  binary (default <repo>/build/dude)
#      DUDE_CFG  your config, source of the display settings (default ~/.config/dude/base/dude.cfg)
#      DEMO      demo name (default demo1)
#      COOLDOWN  seconds idle between runs (default 10)
#      OUT       result directory (default ~/.local/share/dude/bench/<date>-<set>)
#      DRYRUN=1  print the command lines, launch nothing
set -euo pipefail

REPO="$( cd "$( dirname "$0" )/../.." && pwd )"
BIN="${DUDE_BIN:-$REPO/build/dude}"
DUDE_CFG="${DUDE_CFG:-$HOME/.config/dude/base/dude.cfg}"
DEMO="${DEMO:-demo1}"
COOLDOWN="${COOLDOWN:-10}"
SET="${1:-}"
ROUNDS="${2:-3}"

declare -a NAMES
declare -A DESC ARGS
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
	*)
		sed -n '2,23p' "$0"
		exit 1
		;;
esac

if [ "${DRYRUN:-0}" != 1 ]; then
	[ -x "$BIN" ] || { echo "no binary at $BIN (build first, or set DUDE_BIN)" >&2; exit 1; }
	if pgrep -x dude > /dev/null; then
		echo "DUDE is running - close it first (two instances would share the GPU)" >&2
		exit 1
	fi
fi
[ -r "$DUDE_CFG" ] || { echo "cannot read $DUDE_CFG (display settings source)" >&2; exit 1; }
SCHEMA="$( sed -n 's/^#define DUDE_CONFIG_SCHEMA[[:space:]]*\([0-9]*\).*/\1/p' "$REPO/neo/framework/Licensee.h" )"

OUT="${OUT:-${XDG_DATA_HOME:-$HOME/.local/share}/dude/bench/$( date +%Y%m%d-%H%M%S )-$SET}"
mkdir -p "$OUT"
{
	echo "set: $SET  rounds: $ROUNDS  demo: $DEMO  preset: $PRESET  dude_rtx: $RTX"
	echo "binary: $BIN ($( stat -c '%y' "$BIN" 2> /dev/null || echo '?' ))"
	echo "repo: $( git -C "$REPO" rev-parse --short HEAD 2> /dev/null || echo '?' ) on $( git -C "$REPO" branch --show-current 2> /dev/null || echo '?' )"
	echo "display settings from $DUDE_CFG:"
	grep -E '^seta (r_mode|r_fullscreen|r_fullscreenDesktop|r_fullscreenDisplay|r_fullscreenDisplayName|r_customWidth|r_customHeight|r_displayRefresh) ' "$DUDE_CFG" || true
	for v in "${NAMES[@]}"; do echo "variant $v: ${DESC[$v]}  [${ARGS[$v]}]"; done
} > "$OUT/meta.txt"
cat "$OUT/meta.txt"

# run order: warm-up (first variant, discarded), then alternating rounds
order=( "w:${NAMES[0]}" )
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
		echo "  $BIN ${args[*]}"
		continue
	fi
	nvidia-smi --query-gpu=timestamp,clocks.gr,clocks.mem,temperature.gpu,power.draw,clocks_event_reasons.active \
		--format=csv,noheader -lms 1000 > "$RUN/clocks.csv" 2> /dev/null &
	smi=$!
	set +e
	timeout 900 "$BIN" "${args[@]}" > "$RUN/stdout.log" 2>&1
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
