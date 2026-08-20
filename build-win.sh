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
#   ./build-win.sh --no-vulkan  GL-only build (skip the Vulkan backend)
#   extra args pass through to cmake --build (e.g. ./build-win.sh --target dude)
#
# Vulkan: there is no official mingw-w64 Vulkan package, so we self-provision a
# local prefix (deps-mingw/vulkan/): the host's Vulkan headers are platform-
# agnostic, and the import lib is generated from winevulkan's vulkan-1.dll
# export table (on real Windows vulkan-1.dll ships with the GPU driver, so it
# is never bundled). SPIR-V is compiled offline by the HOST glslangValidator.
# shaderc (runtime custom-ARB compile for mod shaders) is OFF: pkg-config would
# leak the host's Linux .so into the Windows link; mod-ARB stages degrade to
# skip on VK until a mingw shaderc is provided. Falls back to GL-only when a
# piece is missing.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$ROOT/neo"
BUILD="$ROOT/build-win"
DIST="$ROOT/dist-win"
PREFIX="${MINGW_PREFIX:-x86_64-w64-mingw32}"

VKDEPS="$ROOT/deps-mingw/vulkan"

CLEAN=0
VULKAN=1
BUILD_ARGS=()
for arg in "$@"; do
	case "$arg" in
		-c|--clean)  CLEAN=1 ;;
		--no-vulkan) VULKAN=0 ;;
		*)           BUILD_ARGS+=("$arg") ;;
	esac
done

if ! command -v "${PREFIX}-gcc" >/dev/null 2>&1; then
	echo "build-win.sh: ${PREFIX}-gcc not found. Install the toolchain first:" >&2
	echo "    sudo pacman -S --needed mingw-w64-gcc" >&2
	echo "    paru -S mingw-w64-sdl2 mingw-w64-openal" >&2
	exit 1
fi

[ "$CLEAN" -eq 1 ] && { echo ">> clean: removing $BUILD"; rm -rf "$BUILD"; }

# Self-provision the mingw Vulkan prefix: host headers + an import lib built
# from winevulkan's export table (a faithful model of the real loader's export
# surface). Returns non-zero -> the configure step falls back to GL-only.
ensure_vulkan_deps() {
	[ -d "$VKDEPS/include/vulkan" ] && [ -f "$VKDEPS/lib/libvulkan-1.dll.a" ] && return 0
	local winedll=/usr/lib/wine/x86_64-windows/vulkan-1.dll
	if [ ! -d /usr/include/vulkan ]; then
		echo ">> Vulkan deps: host headers missing (pacman -S vulkan-headers)"; return 1; fi
	if [ ! -f "$winedll" ]; then
		echo ">> Vulkan deps: $winedll missing (pacman -S wine)"; return 1; fi
	if ! command -v "${PREFIX}-dlltool" >/dev/null 2>&1; then
		echo ">> Vulkan deps: ${PREFIX}-dlltool missing"; return 1; fi
	if ! command -v glslangValidator >/dev/null 2>&1 && ! command -v glslc >/dev/null 2>&1; then
		echo ">> Vulkan deps: no host glslangValidator/glslc (pacman -S glslang)"; return 1; fi
	echo ">> Vulkan deps: generating $VKDEPS"
	mkdir -p "$VKDEPS/include" "$VKDEPS/lib"
	cp -r /usr/include/vulkan "$VKDEPS/include/"
	[ -d /usr/include/vk_video ] && cp -r /usr/include/vk_video "$VKDEPS/include/"
	"${PREFIX}-objdump" -p "$winedll" \
		| awk '/Forwarder RVA -- winevulkan\./ {n=$NF; sub(/^winevulkan\./,"",n); print n}' \
		> "$VKDEPS/vulkan-1.exports"
	if [ ! -s "$VKDEPS/vulkan-1.exports" ]; then
		echo ">> Vulkan deps: no exports parsed from $winedll"; rm -rf "$VKDEPS"; return 1; fi
	{ echo "LIBRARY vulkan-1.dll"; echo "EXPORTS"; cat "$VKDEPS/vulkan-1.exports"; } > "$VKDEPS/vulkan-1.def"
	"${PREFIX}-dlltool" -d "$VKDEPS/vulkan-1.def" -l "$VKDEPS/lib/libvulkan-1.dll.a" -D vulkan-1.dll
}

if [ ! -f "$BUILD/CMakeCache.txt" ]; then
	VK_FLAGS=(-DDHEWM3_VULKAN=OFF)
	if [ "$VULKAN" -eq 1 ] && ensure_vulkan_deps; then
		VK_FLAGS=(-DDHEWM3_VULKAN=ON
		          -DVulkan_INCLUDE_DIR="$VKDEPS/include"
		          -DVulkan_LIBRARY="$VKDEPS/lib/libvulkan-1.dll.a"
		          -DDUDE_RUNTIME_ARB_COMPILER=OFF)
		echo ">> configuring (mingw-w64, Vulkan ON via $VKDEPS)"
	else
		echo ">> configuring (mingw-w64, Vulkan OFF)"
	fi
	cmake -S "$SRC" -B "$BUILD" \
		-DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-mingw-w64.cmake" \
		"${VK_FLAGS[@]}"
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
