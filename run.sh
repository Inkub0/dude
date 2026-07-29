#!/bin/sh
# Portable DUDE launcher.
#
# Runs DUDE with game data AND savegames kept under this directory
# (fs_basepath == fs_savepath == the folder this script lives in), so the whole
# tree is a self-contained install you can move or copy and it stays complete.
#
# Config (dude.cfg) still lives in your XDG config dir (~/.config/dhewm3) unless
# you also pass +set fs_configpath "$DIR". Extra args are forwarded, e.g.:
#   ./run.sh +game d3xp +set r_graphicsAPI opengl3
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"

# Prefer build/dude, fall back to build-native/dude; override with DUDE_BIN=...
BIN="${DUDE_BIN:-$DIR/build/dude}"
[ -x "$BIN" ] || BIN="$DIR/build-native/dude"
if [ ! -x "$BIN" ]; then
	echo "run.sh: no dude binary found (looked for build/dude, build-native/dude)." >&2
	echo "        build it first, or set DUDE_BIN=/path/to/dude" >&2
	exit 1
fi

exec "$BIN" \
	+set fs_basepath "$DIR" \
	+set fs_savepath "$DIR" \
	"$@"
