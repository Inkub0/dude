#!/usr/bin/env bash
# DUDE build helper. Configures the build dir if needed, then builds.
#
# Usage:
#   ./build.sh                 incremental build (default)
#   ./build.sh -c | --clean    wipe the build dir and reconfigure from scratch
#   ./build.sh -v | --vulkan   configure with the Vulkan backend (-DDHEWM3_VULKAN=ON)
#   extra args are passed through to cmake --build (e.g. ./build.sh --target dude)

set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/neo"
BUILD="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/build"

CLEAN=0
CONFIGURE_ARGS=()
BUILD_ARGS=()
for arg in "$@"; do
	case "$arg" in
		-c|--clean)   CLEAN=1 ;;
		-v|--vulkan)  CONFIGURE_ARGS+=("-DDHEWM3_VULKAN=ON") ;;
		*)            BUILD_ARGS+=("$arg") ;;
	esac
done

if [ "$CLEAN" -eq 1 ]; then
	echo ">> clean: removing $BUILD"
	rm -rf "$BUILD"
fi

# (re)configure if the build dir or its cache is missing, or clean was requested
if [ ! -f "$BUILD/CMakeCache.txt" ] || [ ${#CONFIGURE_ARGS[@]} -gt 0 ]; then
	echo ">> configuring"
	cmake -S "$SRC" -B "$BUILD" "${CONFIGURE_ARGS[@]}"
fi

echo ">> building"
cmake --build "$BUILD" -j"$(nproc)" "${BUILD_ARGS[@]}"

echo ">> done: $BUILD/dude"
