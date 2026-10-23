#!/bin/bash
# Quake 4 null-game map sweep (docs/quake4-port.md §7, item 3): loads every single-player map
# of the campaign (Appendix C) in the null game, one run per map, and keeps each run's console
# output. tools/quake4/smoke_summary.py then reads the logs: load result, warnings, the .cm
# header and .aas self-checks.
#
# Usage:  tools/quake4/smoke-maps.sh [vulkan|opengl3|legacy] [map ...]
#         (default: vulkan, all 31 maps; a map is its maps.def name, e.g. process1_first)
# Env:    WAIT=frames to stay on each map before quitting (default 120)
#         TIMEOUT=seconds before a run is killed as hung (default 300)
#         SHOTS=1 takes a screenshot of each map's start view (into DUDE's save folder,
#         q4base/screenshots) for the visual check
#
# Each run opens the game window. Settings come from a copy of ~/.config/dude/q4base taken at
# the start, so the sweep's own +set (renderer, crash logging) never reach your dude.cfg.
# Output: logs/q4-sweep-<renderer>-<date>/<map>.log, plus summary.txt.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
API="${1:-vulkan}"
shift || true
WAIT="${WAIT:-120}"
TIMEOUT="${TIMEOUT:-300}"
SHOTS="${SHOTS:-0}"

case "$API" in
	vulkan|opengl3|legacy) ;;
	*) echo "smoke-maps.sh: renderer must be vulkan, opengl3 or legacy (got '$API')" >&2; exit 2 ;;
esac

MAPS=( "$@" )
if [ ${#MAPS[@]} -eq 0 ]; then
	MAPS=( airdefense1 airdefense2 hangar1 hangar2 mcc_landing building_b convoy1
	       convoy2 convoy2b mcc_1 mcc_2 hub1 hub2 medlabs walker dispersal
	       recomp putra waste process1_first process1_second process2 network1
	       network2 storage1_first storage1_second storage2 tram1 tram1b core1
	       core2 )
fi

OUT="$ROOT/logs/q4-sweep-$API-$(date +%Y%m%d-%H%M%S)"
CONFIG="$OUT/config"
mkdir -p "$CONFIG/q4base"
if [ -d "$HOME/.config/dude/q4base" ]; then
	cp -a "$HOME/.config/dude/q4base/." "$CONFIG/q4base/"
fi

echo "smoke-maps.sh: $API, ${#MAPS[@]} maps, logs in $OUT"
for name in "${MAPS[@]}"; do
	# the two-level maps: one .map, the level picked by its entity filter (map <file> <filter>)
	file="$name"
	filter=""
	case "$name" in
		*_first|*_second) file="${name%_*}"; filter="${name##*_}" ;;
	esac
	shot=()
	if [ "$SHOTS" = "1" ]; then
		shot=( +screenshot +wait 5 )
	fi
	log="$OUT/$name.log"
	start=$(date +%s.%N)
	# shellcheck disable=SC2086
	timeout -k 10 "$TIMEOUT" "$ROOT/run-q4.sh" \
		+set fs_configpath "$CONFIG" \
		+set r_graphicsAPI "$API" \
		+set r_vkCrashLogging 1 \
		+map "game/$file" $filter \
		+wait "$WAIT" "${shot[@]}" +quit \
		> "$log" 2>&1
	rc=$?
	end=$(date +%s.%N)
	printf '\n=== smoke-maps: exit %d, %s s ===\n' "$rc" "$(awk "BEGIN { printf \"%.1f\", $end - $start }")" >> "$log"
	case $rc in
		0)   state=ok ;;
		124) state=TIMEOUT ;;
		*)   state="EXIT $rc" ;;
	esac
	printf '%-18s %s\n' "$name" "$state"
done

python3 "$ROOT/tools/quake4/smoke_summary.py" "$OUT" | tee "$OUT/summary.txt"
