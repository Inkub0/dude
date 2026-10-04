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

# Single canonical binary: build/dude (see build.sh). No fallback dirs — a
# stale secondary build once silently shadowed fresh changes (removed
# 2026-07-30); override explicitly with DUDE_BIN=... if you mean another one.
BIN="${DUDE_BIN:-$DIR/build/dude}"
if [ ! -x "$BIN" ]; then
	echo "run.sh: no dude binary at build/dude — run ./build.sh first," >&2
	echo "        or set DUDE_BIN=/path/to/dude" >&2
	exit 1
fi

# NVIDIA: pace vblank sync to the DP-0 output (frame-pacing win on this GPU).
# Harmless on other drivers; override by exporting these before running run.sh.
export __GL_SYNC_DISPLAY_DEVICE="${__GL_SYNC_DISPLAY_DEVICE:-DP-0}"
export __GL_SYNC_TO_VBLANK="${__GL_SYNC_TO_VBLANK:-1}"

# NVIDIA on X11: permit VRR (G-SYNC / G-SYNC Compatible / Adaptive-Sync) so the
# in-engine "VRR" VSync mode can actually drive the panel's variable refresh.
# You still need "Allow G-SYNC/G-SYNC Compatible" enabled in nvidia-settings and
# Force Composition Pipeline off. Harmless on other drivers; override to 0 to opt out.
export __GL_GSYNC_ALLOWED="${__GL_GSYNC_ALLOWED:-1}"
export __GL_VRR_ALLOWED="${__GL_VRR_ALLOWED:-1}"

exec "$BIN" \
	+set fs_basepath "$DIR" \
	+set fs_savepath "$DIR" \
	"$@"
