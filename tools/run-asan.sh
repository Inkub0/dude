#!/bin/bash
# Runs DUDE under AddressSanitizer (ASAN): a bad memory access stops the game at the line that
# made it, with a report, instead of crashing later somewhere unrelated.
#
# The ASAN build lives in build-asan/, configured like build/ (its options are copied from
# build/CMakeCache.txt) plus -DASAN=ON. It is rebuilt incrementally before every run, so it never
# lags behind the source - build/ stays the one build everything else uses.
#
# Usage:  tools/run-asan.sh [--d3 | --q4] [game arguments...]
#         --d3   Doom 3 (run.sh) - the default
#         --q4   Quake 4 (run-q4.sh, also rebuilds q4base.so against build-asan/)
#         e.g.   tools/run-asan.sh +map game/mars_city1
#                tools/run-asan.sh --q4 +map game/airdefense1
# Reports: ~/.local/share/dude/asan.<pid> (ASAN_OPTIONS overrides), failed asserts included (with
# their stack). Slower than build/ by ~2-3x.
set -euo pipefail
DUDE="$(cd "$(dirname "$0")/.." && pwd)"
ASAN_BUILD="$DUDE/build-asan"

QUAKE4=0
if [ "${1:-}" = "--q4" ]; then
	QUAKE4=1
	shift
elif [ "${1:-}" = "--d3" ]; then
	shift
fi
# the game takes +commands: anything else would reach the engine glued to run.sh's last
# "+set fs_savepath <dir>" and become part of the save path ("<dir> --d3" - saves the load menu
# never lists)
case "${1:-}" in
--*)
	echo "run-asan.sh: unknown option '$1' (--d3 or --q4 first; game arguments are +commands)" >&2
	exit 1
	;;
esac

if [ ! -f "$ASAN_BUILD/CMakeCache.txt" ]; then
	[ -f "$DUDE/build/CMakeCache.txt" ] || { echo "run-asan.sh: configure build/ first (./build.sh)" >&2; exit 1; }
	# build/'s own on/off options and build type, ASAN on
	OPTIONS=()
	while IFS= read -r line; do
		OPTIONS+=( "-D$line" )
	done < <( sed -n 's/^\([A-Z0-9_]*\):BOOL=\(.*\)$/\1=\2/p' "$DUDE/build/CMakeCache.txt" | grep -v '^CMAKE_\|^ASAN=' )
	BUILD_TYPE="$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$DUDE/build/CMakeCache.txt")"
	cmake -B "$ASAN_BUILD" -S "$DUDE/neo" -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Debug}" "${OPTIONS[@]}" -DASAN=ON >/dev/null
fi

make -C "$ASAN_BUILD" -j"$(nproc)" dude shaders_spv base d3xp d3le nullgame
if [ "$QUAKE4" = 1 ]; then
	[ -x "$DUDE/tools/quake4/build-q4game.sh" ] || { echo "run-asan.sh: --q4 needs the Quake 4 game library build (tools/quake4/build-q4game.sh, docs/quake4-port.md 5.6)" >&2; exit 1; }
	DUDE_BUILD=build-asan "$DUDE/tools/quake4/build-q4game.sh"
fi

# leak reports at exit are noise for a game (id's allocators keep memory until shutdown); a failed
# assert() raises SIGTRAP: handle_sigtrap=2 makes ASAN report it with a symbolized stack (and keeps
# the engine's own crash handler from taking it over)
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:handle_sigtrap=2:log_path=${XDG_DATA_HOME:-$HOME/.local/share}/dude/asan}"
export DUDE_BIN="$ASAN_BUILD/dude"
if [ "$QUAKE4" = 1 ]; then
	exec "$DUDE/run-q4.sh" "$@"
fi
exec "$DUDE/run.sh" "$@"
