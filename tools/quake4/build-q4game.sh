#!/bin/bash
# Builds the Quake 4 game library (q4base.so) from the separate game repository
# (docs/quake4-port.md 5.6), against this DUDE checkout: its headers, its idlib (build/libidlib.a)
# and the compile flags DUDE builds its own game libraries with (read from build/, so q4base.so is
# compiled exactly like base.so). The result is copied next to build/dude, where the engine looks
# for it when it runs Quake 4.
#
# Usage:  tools/quake4/build-q4game.sh [--inventory]
#         --inventory  compile every game source syntax-only and report the errors grouped
#                      (tools/quake4/q4game_inventory.py) instead of building
# Env:    Q4GAME_DIR   the game repository (default ~/dude-quake4-game)
#
# The game repository is under the Quake 4 SDK licence and stays outside this tree; nothing
# from it is copied here except the built library. Needs a configured, built DUDE build/.
set -euo pipefail
DUDE="$(cd "$(dirname "$0")/../.." && pwd)"
Q4GAME_DIR="${Q4GAME_DIR:-$HOME/dude-quake4-game}"
FLAGS="$DUDE/build/CMakeFiles/base.dir/flags.make"
OUT="$Q4GAME_DIR/build"

[ -f "$Q4GAME_DIR/CMakeLists.txt" ] || { echo "build-q4game.sh: no game repository at $Q4GAME_DIR (set Q4GAME_DIR)" >&2; exit 1; }
[ -f "$FLAGS" ] && [ -f "$DUDE/build/libidlib.a" ] || { echo "build-q4game.sh: build DUDE first (no build/libidlib.a or base target flags)" >&2; exit 1; }

value() { sed -n "s/^$1 = //p" "$FLAGS"; }
# DUDE's definitions for base.so, minus the target's own export macro
DEFINES="$(value CXX_DEFINES | sed 's/-Dbase_EXPORTS//')"
# DUDE's flags; the colour switch only matters to a terminal
CXXFLAGS="$(value CXX_FLAGS | sed 's/-fdiagnostics-color=always//')"
# only the system include paths (SDL, OpenAL): DUDE's own neo/ and build/ the game CMake adds itself,
# and neo/game - the Doom 3 game - must never be visible to the Quake 4 game
INCLUDES=""
for inc in $(value CXX_INCLUDES); do
	case "$inc" in
		-I"$DUDE"/*) ;;
		-I*) INCLUDES="$INCLUDES ${inc#-I}" ;;
	esac
done

cmake -S "$Q4GAME_DIR" -B "$OUT" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
	-DDUDE_DIR="$DUDE" -DDUDE_BUILD_DIR="$DUDE/build" \
	-DDUDE_GAME_DEFINES="$DEFINES" -DDUDE_GAME_FLAGS="$CXXFLAGS" -DDUDE_GAME_INCLUDES="$INCLUDES" >/dev/null

if [ "${1:-}" = "--inventory" ]; then
	exec python3 "$DUDE/tools/quake4/q4game_inventory.py" "$OUT/compile_commands.json" "$Q4GAME_DIR/source"
fi

cmake --build "$OUT" -j"$(nproc)"
# a new file renamed into place, never written over the old one: a running game keeps executing
# its mapped copy (cp truncates it in place -> SIGBUS in the running game)
cp "$OUT/q4base.so" "$DUDE/build/q4base.so.new"
mv -f "$DUDE/build/q4base.so.new" "$DUDE/build/q4base.so"
echo "build-q4game.sh: $DUDE/build/q4base.so"
