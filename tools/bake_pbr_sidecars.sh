#!/bin/bash
# Bakes first-pass PBR sidecar textures (docs/pbr-texture-maps.md) for whole levels and packs them
# into one .pk4, reproducibly: same game data + same tools = same pack.
#
#   1. tools/pbr_extract_maps.py derives a <diffuse>_orm.tga for every material the levels draw,
#      with its current defaults (stock maps only, metal = specular grey capped at 0.2, roughness
#      anchored to the PBR table, ceramic x2.4 shinier). md5 models take their AO from the engine's
#      own AO baker (generated/aomaps/, bakeAO) where it exists; other characters get none.
#   2. Hand-made sidecars win: any loose <name>_orm.* / <name>_rmao.* already under a --search
#      folder's textures/ or models/ is left out of the pack, so the hand-made file is the one used.
#   3. Everything goes into <name>.pk4 with a bake-info.txt (the command, tool versions, git
#      revision) and is copied to every --install folder.
#
# Usage:  tools/bake_pbr_sidecars.sh [options] [-- extra pbr_extract_maps.py options]
#   -m MAP        level to bake (e.g. game/mars_city1); repeat for more
#   --md5 GLOB    bake every material the matching .md5mesh files use, skins included
#                 (e.g. 'models/md5/*' = all characters, monsters and weapons); repeatable
#   --match GLOB  bake materials matching GLOB (e.g. 'textures/mcity/*'); repeatable
#                 (at least one of -m, --md5, --match)
#   -s DIR        game folder to read, in load order (later wins); repeat. Default: base
#   -n NAME       pack name without .pk4. Default: z_pbr_<level> for one level, else z_pbr_sidecars
#   -o DIR        work folder (wiped first). Default: build/pbr-sidecars/<NAME>
#   -i DIR        copy the pack into DIR; repeat (e.g. -i base -i ~/.local/share/dude/base)
#   --keep-character-ao   derive AO from the normal maps for models/characters/* that have no
#                 baked AO (by default they get none: atlas borders shade badly)
#   -h            this help
#
# Examples:
#   tools/bake_pbr_sidecars.sh -m game/mars_city1 -n z_pbr_mars_city1_test -i base -i ~/.local/share/dude/base
#   tools/bake_pbr_sidecars.sh -m game/mars_city1 -m game/mc_underground -n z_pbr_marscity
#   tools/bake_pbr_sidecars.sh --md5 'models/md5/*' -n z_pbr_md5
#   tools/bake_pbr_sidecars.sh -s /path/to/quake4/q4base -m game/airdefense1 -- --layout rmao
#
# The work folder keeps _maps/ (editable greyscale maps + previews) and bake.log. Remove an
# installed pack to undo it; a running game needs a map reload (or reloadPbrTable) to see changes.
set -euo pipefail
DUDE="$(cd "$(dirname "$0")/.." && pwd)"
TOOL="$DUDE/tools/pbr_extract_maps.py"

usage() { sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

MAPS=() MD5=() MATCH=() SEARCH=() INSTALL=() NAME="" WORK="" CHAR_AO=0
while [ $# -gt 0 ]; do
	case "$1" in
		-m) MAPS+=("$2"); shift 2 ;;
		--md5) MD5+=("$2"); shift 2 ;;
		--match) MATCH+=("$2"); shift 2 ;;
		-s) SEARCH+=("$2"); shift 2 ;;
		-n) NAME="$2"; shift 2 ;;
		-o) WORK="$2"; shift 2 ;;
		-i) INSTALL+=("$2"); shift 2 ;;
		--keep-character-ao) CHAR_AO=1; shift ;;
		-h|--help) usage 0 ;;
		--) shift; break ;;
		*) echo "unknown option: $1 (-h for help)" >&2; exit 2 ;;
	esac
done
EXTRA=("$@")

[ ${#MAPS[@]} -gt 0 ] || [ ${#MD5[@]} -gt 0 ] || [ ${#MATCH[@]} -gt 0 ] || { echo "nothing to bake: give -m LEVEL, --md5 GLOB or --match GLOB" >&2; usage 2; }
[ ${#SEARCH[@]} -gt 0 ] || SEARCH=("$DUDE/base")
if [ -z "$NAME" ]; then
	if [ ${#MAPS[@]} -eq 1 ] && [ ${#MATCH[@]} -eq 0 ] && [ ${#MD5[@]} -eq 0 ]; then NAME="z_pbr_$(basename "${MAPS[0]}")"; else NAME="z_pbr_sidecars"; fi
fi
NAME="${NAME%.pk4}"
[ -n "$WORK" ] || WORK="$DUDE/build/pbr-sidecars/$NAME"
for cmd in python3 zip; do command -v "$cmd" >/dev/null || { echo "needs $cmd" >&2; exit 1; }; done
python3 -c 'import numpy, PIL' 2>/dev/null || { echo "needs Python 3 with numpy and Pillow" >&2; exit 1; }

ARGS=()
for s in "${SEARCH[@]}"; do ARGS+=(--search "$s"); done
for m in "${MAPS[@]}"; do ARGS+=(--map "$m"); done
for g in "${MD5[@]}"; do ARGS+=(--md5 "$g"); done
for g in "${MATCH[@]}"; do ARGS+=(--match "$g"); done
[ "$CHAR_AO" = 1 ] || ARGS+=(--no-ao 'models/characters/*')
ARGS+=("${EXTRA[@]}")

rm -rf "$WORK"
mkdir -p "$WORK/out"
echo "baking into $WORK ..."
if ! python3 "$TOOL" "${ARGS[@]}" -o "$WORK/out" > "$WORK/bake.log" 2>&1; then
	tail -n 20 "$WORK/bake.log" >&2
	echo "bake failed, full log: $WORK/bake.log" >&2
	exit 1
fi
grep -E '^(skipped|wrote|HD map)' "$WORK/bake.log" || true

# hand-made sidecars already sitting loose in a game folder: keep them out of the pack
HAND="$WORK/hand-made.txt"
: > "$HAND"
for s in "${SEARCH[@]}"; do
	for top in textures models; do
		[ -d "$s/$top" ] || continue
		( cd "$s" && find "$top" -type f \( -iname '*_orm.tga' -o -iname '*_orm.jpg' -o -iname '*_orm.dds' \
			-o -iname '*_rmao.tga' -o -iname '*_rmao.jpg' -o -iname '*_rmao.dds' \) ) |
			sed -E 's/_(orm|rmao)\.[A-Za-z]+$//' | tr 'A-Z' 'a-z' >> "$HAND"
	done
done
sort -u -o "$HAND" "$HAND"

LIST="$WORK/pack-files.txt"
( cd "$WORK/out" && find . -name '*_orm.tga' -not -path './_maps/*' | sed 's|^\./||' | sort ) |
	while read -r f; do
		stem="$(echo "${f%_orm.tga}" | tr 'A-Z' 'a-z')"
		if grep -qxF "$stem" "$HAND"; then echo "  hand-made sidecar kept, generated one left out: $stem" >&2; else echo "$f"; fi
	done > "$LIST"

{
	echo "DUDE PBR sidecar pack $NAME.pk4 - generated, see docs/pbr-texture-maps.md"
	echo "baked:    $(date -u '+%Y-%m-%d %H:%M UTC')"
	dirty="$(git -C "$DUDE" status --porcelain -- tools/pbr_extract_maps.py 2>/dev/null)"
	echo "source:   $(git -C "$DUDE" rev-parse --short HEAD 2>/dev/null || echo unknown)${dirty:+ (pbr_extract_maps.py not committed)}"
	echo "tool:     tools/pbr_extract_maps.py sha256 $(sha256sum "$TOOL" | cut -c1-16)"
	# roughness is anchored to the PBR table, so the table is part of the recipe
	for s in "${SEARCH[@]}"; do
		for t in "$s"/pbr/pbr_materials.cfg "$s"/pbr/pbr_overrides.cfg; do
			[ -f "$t" ] && echo "table:    $t sha256 $(sha256sum "$t" | cut -c1-16)"
		done
	done
	printf 'command:  pbr_extract_maps.py'; printf ' %q' "${ARGS[@]}"; echo
	echo "files:    $(wc -l < "$LIST") sidecars ($(wc -l < "$HAND") hand-made stems excluded)"
} > "$WORK/out/bake-info.txt"
echo "bake-info.txt" >> "$LIST"

PK4="$WORK/$NAME.pk4"
rm -f "$PK4"
( cd "$WORK/out" && zip -q -9 -X "$PK4" -@ < "$LIST" )
echo "packed $(($(wc -l < "$LIST") - 1)) sidecars: $PK4 ($(du -h "$PK4" | cut -f1))"

for d in "${INSTALL[@]}"; do
	mkdir -p "$d"
	cp "$PK4" "$d/" && echo "installed: $d/$NAME.pk4"
done
