#!/bin/bash
# Builds the Quake 4 game library (q4base.so) from the separate game repository
# (docs/quake4-port.md 5.6), against this DUDE checkout: its headers, its idlib (build/libidlib.a)
# and the compile flags DUDE builds its own game libraries with (read from build/, so q4base.so is
# compiled exactly like base.so). The result is copied next to build/dude, where the engine looks
# for it when it runs Quake 4.
#
# Windows: DUDE_BUILD=build-win builds q4base.dll with MinGW against ./build-win.sh's build (its
# toolchain file is read from that build's CMake cache) and copies it next to build-win's dude.exe.
#
# Usage:  tools/quake4/build-q4game.sh [--inventory]
#         --inventory  compile every game source syntax-only and report the errors grouped
#                      (tools/quake4/q4game_inventory.py) instead of building
# Env:    Q4GAME_DIR   the game repository (default ~/dude-quake4-game)
#         DUDE_BUILD   the DUDE build directory to build against and install into, relative to
#                      the checkout (default build; e.g. build-asan for an ASAN build)
#
# The game repository is under the Quake 4 SDK licence and stays outside this tree; nothing
# from it is copied here except the built library. Needs a configured, built DUDE build/.
set -euo pipefail
DUDE="$(cd "$(dirname "$0")/../.." && pwd)"
Q4GAME_DIR="${Q4GAME_DIR:-$HOME/dude-quake4-game}"
BUILD="$DUDE/${DUDE_BUILD:-build}"
FLAGS="$BUILD/CMakeFiles/base.dir/flags.make"
# the game's own build tree, one per DUDE build so their objects never mix
OUT="$Q4GAME_DIR/$(basename "$BUILD")"

[ -f "$Q4GAME_DIR/CMakeLists.txt" ] || { echo "build-q4game.sh: no game repository at $Q4GAME_DIR (set Q4GAME_DIR)" >&2; exit 1; }
[ -f "$FLAGS" ] && [ -f "$BUILD/libidlib.a" ] || { echo "build-q4game.sh: build DUDE first (no $BUILD/libidlib.a or base target flags)" >&2; exit 1; }

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

# a cross build (build-win): the game library is built with the same toolchain as the engine -
# the two exchange C++ objects, so they must agree on the compiler and its ABI
TOOLCHAIN="$(sed -n 's/^CMAKE_TOOLCHAIN_FILE:[A-Z]*=//p' "$BUILD/CMakeCache.txt")"
TOOLCHAIN_ARGS=()
LIB=q4base.so
if [ -n "$TOOLCHAIN" ]; then
	TOOLCHAIN_ARGS=(-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN")
	case "$TOOLCHAIN" in *mingw*) LIB=q4base.dll ;; esac
fi

# how DUDE links its own game libraries' C++ runtime (static in the release and Windows builds,
# so a game library needs no libstdc++ / libgcc of its own next to it)
LINKFLAGS="$(tr ' ' '\n' < "$BUILD/CMakeFiles/base.dir/link.txt" | { grep -E '^-static-lib(gcc|stdc\+\+)$' || true; } | sort -u | tr '\n' ' ')"

cmake -S "$Q4GAME_DIR" -B "$OUT" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "${TOOLCHAIN_ARGS[@]}" \
	-DDUDE_GAME_LINK_FLAGS="$LINKFLAGS" \
	-DDUDE_DIR="$DUDE" -DDUDE_BUILD_DIR="$BUILD" \
	-DDUDE_GAME_DEFINES="$DEFINES" -DDUDE_GAME_FLAGS="$CXXFLAGS" -DDUDE_GAME_INCLUDES="$INCLUDES" >/dev/null

if [ "${1:-}" = "--inventory" ]; then
	exec python3 "$DUDE/tools/quake4/q4game_inventory.py" "$OUT/compile_commands.json" "$Q4GAME_DIR/source"
fi

cmake --build "$OUT" -j"$(nproc)"
# a new file renamed into place, never written over the old one: a running game keeps executing
# its mapped copy (cp truncates it in place -> SIGBUS in the running game)
cp "$OUT/$LIB" "$BUILD/$LIB.new"
mv -f "$BUILD/$LIB.new" "$BUILD/$LIB"
# what it was built from, for the release package's BUILDINFO (tools/quake4/package-q4.sh): the
# GPL source of the idlib inside it is DUDE at this commit
stamp() {
	local rev; rev="$(git -C "$1" rev-parse HEAD)"
	if [ -n "$(git -C "$1" status --porcelain --untracked-files=no -- $2)" ]; then rev="$rev (uncommitted changes)"; fi
	echo "$rev"
}
{
	echo "DUDE commit:     $(stamp "$DUDE" neo)"
	echo "dude-q4 commit:  $(stamp "$Q4GAME_DIR" "source CMakeLists.txt")"
	echo "built:           $(date -u '+%Y-%m-%d %H:%M UTC') in $(basename "$BUILD")"
	# the compiler the sources were compiled with (a cross build's toolchain file sets it outside the cache)
	echo "compiler:        $("$(python3 -c 'import json,sys,shlex; print(shlex.split(json.load(open(sys.argv[1]))[0]["command"])[0])' "$OUT/compile_commands.json")" --version | head -1)"
} > "$BUILD/q4base.buildinfo"
echo "build-q4game.sh: $BUILD/$LIB"
