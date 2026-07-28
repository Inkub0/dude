#!/usr/bin/env bash
# bake_ao.sh — run the offline AO baker headless WITHOUT clobbering your video settings.
#
# Why this exists: the baker runs inside the full `dude` client, which under xvfb (no real
# display) falls back to a low windowed resolution. Because the video cvars are CVAR_ARCHIVE,
# a normal `+quit` writes that fallback into ~/.config/dhewm3/base/dude.cfg, overwriting your
# real resolution/fullscreen. This wrapper points fs_configpath at a throwaway dir, so the
# fallback is written there and thrown away — your real config is never opened for writing.
# Baked maps still land in the real savepath (generated/aomaps), exactly as before.
#
# Usage:
#   ./bake_ao.sh +bakeAOFolder models/mapobjects
#   ./bake_ao.sh +bakeAO models/md5/monsters/imp/imp.md5mesh
#   AO_RAYS=256 ./bake_ao.sh +bakeAOFolder models/mapobjects/cpu   # override tunables
#
# Everything after the script name is passed straight to the engine, so you can chain
# multiple +bakeAO* commands. A final +quit is appended automatically.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DUDE="$HERE/build/dude"

if [ ! -x "$DUDE" ]; then
	echo "bake_ao.sh: '$DUDE' not found — build first (./build.sh)" >&2
	exit 1
fi
if [ "$#" -eq 0 ]; then
	echo "usage: $0 +bakeAOFolder <path>   |   +bakeAO <model>   [more +commands...]" >&2
	exit 2
fi

# throwaway config dir so the xvfb fallback video settings never reach your real dude.cfg
CFG_THROWAWAY="$(mktemp -d "${TMPDIR:-/tmp}/dude-bake-cfg.XXXXXX")"
trap 'rm -rf "$CFG_THROWAWAY"' EXIT

# bake tunables (override via env); match the shipped defaults used for the batch bakes
AO_RAYS="${AO_RAYS:-128}"
AO_SIZE="${AO_SIZE:-0}"       # 0 = auto (match each surface's texture size)
AO_THREADS="${AO_THREADS:-0}" # 0 = auto (all hardware threads)

exec xvfb-run -a "$DUDE" \
	+set fs_configpath "$CFG_THROWAWAY" \
	+set r_occlusionMapBakeSize "$AO_SIZE" \
	+set r_occlusionMapBakeRays "$AO_RAYS" \
	+set r_occlusionMapBakeThreads "$AO_THREADS" \
	"$@" \
	+quit
