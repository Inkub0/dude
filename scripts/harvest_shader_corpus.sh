#!/usr/bin/env bash
# Harvest ARB shader programs (glprogs/*) from locally installed pk4 archives
# into testdata/glprogs/<source>/ as the transpiler regression corpus.
#
# The extracted shaders are copyrighted game/mod content and are NOT committed
# (testdata/ is gitignored) — only this script is. Run it after installing the
# game data; arbtool then tests against whatever was found.
#
# Usage: scripts/harvest_shader_corpus.sh [phobos-install-dir]

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/testdata/glprogs"

harvest() { # harvest <name> <pk4...>
	local name="$1"; shift
	local dest="$OUT/$name"
	mkdir -p "$dest"
	for pk4 in "$@"; do
		[ -f "$pk4" ] || continue
		# forward-slash paths (unix-built pk4s); exit codes ignored — unzip
		# returns 1 for mere warnings and 11 for "nothing matched"
		unzip -o -j -q "$pk4" 'glprogs/*' -d "$dest" 2>/dev/null || true
		# backslash paths (Windows-built pk4s, e.g. Phobos); \\ escapes the
		# backslash for unzip's wildcard matcher
		unzip -o -j -q "$pk4" 'glprogs\\*' -d "$dest" 2>/dev/null || true
	done
	# drop non-ARB formats (Cg sources)
	rm -f "$dest"/*.cg
	local count
	count=$(ls -1 "$dest" 2>/dev/null | wc -l)
	if [ "$count" -gt 0 ]; then
		echo "$name: $count files"
	else
		rmdir "$dest" 2>/dev/null || true
		echo "$name: nothing extracted, skipped"
	fi
}

harvest base  "$ROOT"/base/pak000.pk4
harvest d3xp  "$ROOT"/d3xp/pak000.pk4 "$ROOT"/d3xp/pak001.pk4

PHOBOS="${1:-/home/peppe/Games/gog/doom-3-phobos/drive_c/GOG Games/DOOM 3 Phobos}"
if [ -d "$PHOBOS/tfphobos" ]; then
	harvest phobos "$PHOBOS"/tfphobos/pak*.pk4
else
	echo "phobos: not found at $PHOBOS, skipped"
fi

echo "corpus in $OUT"
