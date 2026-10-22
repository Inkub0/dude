#!/bin/sh
# DUDE launcher for Quake 4 (docs/quake4-port.md).
#
# Runs the same build/dude binary as run.sh with the Quake 4 game title selected and
# fs_basepath pointed at a retail Quake 4 install (the folder that contains q4base/).
# Saves and config stay in DUDE's per-user folders (~/.local/share/dude/q4base and
# ~/.config/dude/q4base), never in the Quake 4 install.
#
# The install is found, in order:
#   1. Q4DIR=/path/to/Quake\ 4 ./run-q4.sh
#   2. every Steam library listed in libraryfolders.vdf (steamapps/common/Quake 4)
#   3. a few common GOG / manual install locations
# Extra args are forwarded, e.g.:  ./run-q4.sh +set r_graphicsAPI opengl3
#
# Until the Quake 4 game library (q4base.so) is built, DUDE runs the null game: a
# free-flying spectator with no gameplay, enough to bring up the engine side.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"

BIN="${DUDE_BIN:-$DIR/build/dude}"
if [ ! -x "$BIN" ]; then
	echo "run-q4.sh: no dude binary at build/dude — run ./build.sh first," >&2
	echo "           or set DUDE_BIN=/path/to/dude" >&2
	exit 1
fi

is_q4_install() {
	[ -f "$1/q4base/pak001.pk4" ]
}

find_q4() {
	if [ -n "$Q4DIR" ]; then
		if is_q4_install "$Q4DIR"; then
			echo "$Q4DIR"
			return 0
		fi
		echo "run-q4.sh: Q4DIR='$Q4DIR' has no q4base/pak001.pk4" >&2
		return 1
	fi

	for vdf in "$HOME/.local/share/Steam/steamapps/libraryfolders.vdf" \
	           "$HOME/.steam/steam/steamapps/libraryfolders.vdf"; do
		[ -f "$vdf" ] || continue
		# lines look like:   "path"		"/mnt/linux-data/SteamLibrary"
		for lib in $(sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$vdf" | tr ' ' '\001'); do
			lib=$(printf '%s' "$lib" | tr '\001' ' ')
			if is_q4_install "$lib/steamapps/common/Quake 4"; then
				echo "$lib/steamapps/common/Quake 4"
				return 0
			fi
		done
	done

	for guess in "$HOME/GOG Games/Quake 4" "$HOME/Games/quake4" "$HOME/Games/Quake 4" \
	             "/usr/local/games/quake4" "/opt/quake4"; do
		if is_q4_install "$guess"; then
			echo "$guess"
			return 0
		fi
	done

	echo "run-q4.sh: couldn't find a Quake 4 install (a folder containing q4base/pak001.pk4)." >&2
	echo "           Set Q4DIR=/path/to/Quake 4 and try again." >&2
	return 1
}

Q4="$(find_q4)"
echo "run-q4.sh: Quake 4 data from: $Q4"

# Same driver environment as run.sh (NVIDIA frame pacing + VRR permission).
export __GL_SYNC_DISPLAY_DEVICE="${__GL_SYNC_DISPLAY_DEVICE:-DP-0}"
export __GL_SYNC_TO_VBLANK="${__GL_SYNC_TO_VBLANK:-1}"
export __GL_GSYNC_ALLOWED="${__GL_GSYNC_ALLOWED:-1}"
export __GL_VRR_ALLOWED="${__GL_VRR_ALLOWED:-1}"

exec "$BIN" \
	+set com_title quake4 \
	+set fs_basepath "$Q4" \
	"$@"
