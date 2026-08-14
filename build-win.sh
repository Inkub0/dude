#!/usr/bin/env bash
# Cross-compile DUDE for 64-bit Windows (mingw-w64) from Linux, then stage a
# runnable folder (exe + all runtime DLLs) you can copy or mount into a Windows VM.
#
# One-time deps (Arch/CachyOS):
#   sudo pacman -S --needed mingw-w64-gcc
#   paru -S mingw-w64-sdl2 mingw-w64-openal
#
# Usage:
#   ./build-win.sh            incremental build into build-win/, stage to dist-win/
#   ./build-win.sh -c         wipe build-win/ and reconfigure from scratch
#   extra args pass through to cmake --build (e.g. ./build-win.sh --target dude)
#
# Vulkan is intentionally OFF here: the GL backend is enough to validate the
# filesystem-path work, and it avoids cross-compiling glslang / the Vulkan loader.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$ROOT/neo"
BUILD="$ROOT/build-win"
DIST="$ROOT/dist-win"
PREFIX="${MINGW_PREFIX:-x86_64-w64-mingw32}"

CLEAN=0
BUILD_ARGS=()
for arg in "$@"; do
	case "$arg" in
		-c|--clean) CLEAN=1 ;;
		*)          BUILD_ARGS+=("$arg") ;;
	esac
done

if ! command -v "${PREFIX}-gcc" >/dev/null 2>&1; then
	echo "build-win.sh: ${PREFIX}-gcc not found. Install the toolchain first:" >&2
	echo "    sudo pacman -S --needed mingw-w64-gcc" >&2
	echo "    paru -S mingw-w64-sdl2 mingw-w64-openal" >&2
	exit 1
fi

[ "$CLEAN" -eq 1 ] && { echo ">> clean: removing $BUILD"; rm -rf "$BUILD"; }

if [ ! -f "$BUILD/CMakeCache.txt" ]; then
	echo ">> configuring (mingw-w64, Vulkan OFF)"
	cmake -S "$SRC" -B "$BUILD" \
		-DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-mingw-w64.cmake" \
		-DDHEWM3_VULKAN=OFF
fi

echo ">> building"
cmake --build "$BUILD" -j"$(nproc)" "${BUILD_ARGS[@]}"

# ---- stage a self-contained folder for the VM -----------------------------
echo ">> staging $DIST"
mkdir -p "$DIST"
# the binary (dhewm3 names its client target 'dude' -> dude.exe)
cp -f "$BUILD"/*.exe "$DIST"/ 2>/dev/null || true
# game logic libs (base.dll, d3xp.dll) are dlopen'd at runtime via LoadLibrary,
# so they appear in NO import table and the transitive walk below can't find
# them. Copy them explicitly, and feed them to stage_deps so their own runtime
# deps get pulled in too.
cp -f "$BUILD"/*.dll "$DIST"/ 2>/dev/null || true

# Runtime DLLs, resolved TRANSITIVELY: walk each binary's import table with
# objdump, and for every imported DLL that exists in the mingw sysroot (i.e. is
# a bundled lib, not a Windows system DLL), copy it and recurse into ITS imports.
# This catches deps-of-deps like libssp-0.dll (pulled in by OpenAL32.dll) that a
# hardcoded list silently missed.
OBJDUMP="${PREFIX}-objdump"
# search dirs for bundled DLLs (sysroot bin + gcc runtime dir)
SEARCH_DIRS=("/usr/${PREFIX}/bin" "/usr/lib/gcc/${PREFIX}")

# staging walks lots of system DLLs that are deliberately "not found"; those
# non-zero returns must not trip `set -e`. Disable errexit for this section.
set +e

find_dll() { # $1 = dll name -> prints sysroot path if bundled, else nothing
	local d hit
	for d in "${SEARCH_DIRS[@]}"; do
		hit="$(find "$d" -maxdepth 2 -name "$1" 2>/dev/null | head -1)"
		[ -n "$hit" ] && { echo "$hit"; return 0; }
	done
	return 0
}

stage_deps() { # $1 = path to a PE file whose imports to resolve
	local names name src
	names="$("$OBJDUMP" -p "$1" 2>/dev/null | awk '/DLL Name:/ {print $3}')"
	for name in $names; do
		[ -f "$DIST/$name" ] && continue          # already staged
		src="$(find_dll "$name")"
		[ -z "$src" ] && continue                 # system DLL, leave to Windows
		cp -f "$src" "$DIST/$name"; echo "   + $name"
		stage_deps "$DIST/$name"                  # recurse into its imports
	done
}

for bin in "$DIST"/*.exe "$DIST"/*.dll; do
	stage_deps "$bin"
done
set -e

# Dev convenience: symlink the repo's game data next to the exe so a bare launch
# (no +set fs_basepath) finds it via the engine's <exe-dir>/base probe. A real
# distributable would ship actual pk4s here instead; these links are harmless and
# dist-win/ is git-ignored.
for gd in base d3xp; do
	[ -d "$ROOT/$gd" ] && ln -sfn "../$gd" "$DIST/$gd"
done

echo ">> done."
echo "   binary + DLLs staged in: $DIST"
echo "   Copy your Doom 3 'base/' (pk4s) next to the exe, or launch with"
echo "   +set fs_basepath to wherever the VM sees the game data."
