#!/bin/bash
# Bakes and packs the Quake 4 PBR sidecar pack (docs/pbr-texture-maps.md), the optional download
# published on Inkub0/dude-q4 next to the Quake 4 baked AO pack, versioned on its OWN axis (v<N>):
# re-published when the baker or its settings change, not with every DUDE release. Like the AO pack
# it is derived from Raven's textures, so it never goes on a DUDE release (docs/releasing.md).
#
#   Quake 4  -> dist/q4-pbr-sidecars/z_pbr_sidecars.pk4   into q4base/
#
# Covers every single-player level (maps/game/*.map in the retail pk4s) and every md5 model
# (Quake 4 keeps them under models/<kind>/, not models/md5/). --mp adds the multiplayer maps.
#
# Sources, in load order (later wins):
#   1. the retail q4base (found like run-q4.sh: Q4DIR, Steam libraries, common folders)
#   2. dist/q4-baked-ao/ - the published baked AO pack, reused as the sidecar AO of md5 models
#   3. the dude folder's q4base (Q4SAVE, default ~/.local/share/dude/q4base) - only for
#      pbr/pbr_sidecar_adjust.cfg, the PBR editor's "Save for next bake" (All sidecars tab)
#   4. this repo's q4base/ - the tracked PBR table, so it wins over the dude folder's copy
# Unbaked editor tuning in the dude folder's pbr/pbr_overrides.cfg is refused: fold it into the
# table first (tools/pbr_bake_inplace.py, docs/pbr-materials.md section 6), so the pack and the
# shipped table agree.
#
# Usage:  tools/pack_pbr_sidecars_q4.sh v<N> [--mp] [-- extra pbr_extract_maps.py options]
#   Bakes with tools/bake_pbr_sidecars.sh into build/pbr-sidecars/z_pbr_sidecars_q4/, then copies
#   the pack into dist/q4-pbr-sidecars/z_pbr_sidecars.pk4 with a BUILDINFO.txt inside and a
#   .sha256 next to it. Nothing is installed: copy it into q4base/ with the game closed.
#   Env: Q4DIR, Q4SAVE, OUT_DIR, PACK_ONLY=1 (bake nothing: re-collect and re-stamp the last bake)
set -euo pipefail
DUDE="$(cd "$(dirname "$0")/.." && pwd)"
TAG="${1:-}"
usage() { sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 2; }
[[ "$TAG" =~ ^v?[0-9]+$ ]] || usage
TAG="v${TAG#v}"; shift
MP=0 EXTRA=()
while [ $# -gt 0 ]; do
	case "$1" in
		--mp) MP=1; shift ;;
		--) shift; EXTRA=("$@"); break ;;
		*) usage ;;
	esac
done
Q4SAVE="${Q4SAVE:-$HOME/.local/share/dude/q4base}"
OUT_DIR="${OUT_DIR:-$DUDE/dist/q4-pbr-sidecars}"
AO_DIR="$DUDE/dist/q4-baked-ao"
NAME=z_pbr_sidecars_q4		# work name in build/pbr-sidecars/ (z_pbr_sidecars is Doom 3's)
SHIP=z_pbr_sidecars			# the shipped name: it lives in q4base/, no clash with Doom 3's

is_q4_install() { [ -f "$1/q4base/pak001.pk4" ]; }
find_q4() {
	if [ -n "${Q4DIR:-}" ]; then
		is_q4_install "$Q4DIR" && { echo "$Q4DIR"; return 0; }
		echo "Q4DIR='$Q4DIR' has no q4base/pak001.pk4" >&2; return 1
	fi
	for vdf in "$HOME/.local/share/Steam/steamapps/libraryfolders.vdf" "$HOME/.steam/steam/steamapps/libraryfolders.vdf"; do
		[ -f "$vdf" ] || continue
		while IFS= read -r lib; do
			is_q4_install "$lib/steamapps/common/Quake 4" && { echo "$lib/steamapps/common/Quake 4"; return 0; }
		done < <(sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$vdf")
	done
	for guess in "$HOME/GOG Games/Quake 4" "$HOME/Games/quake4" "$HOME/Games/Quake 4" /usr/local/games/quake4 /opt/quake4; do
		is_q4_install "$guess" && { echo "$guess"; return 0; }
	done
	echo "couldn't find a Quake 4 install (a folder with q4base/pak001.pk4): set Q4DIR" >&2; return 1
}

if [ "${PACK_ONLY:-0}" != 1 ]; then
	RETAIL="$(find_q4)/q4base"
	# unbaked editor tuning would make the pack disagree with the shipped table
	live="$Q4SAVE/pbr/pbr_overrides.cfg"
	if [ -f "$live" ] && grep -qvE '^[[:space:]]*(#|$)' "$live"; then
		echo "$live holds unbaked tuning - fold it in first:" >&2
		echo "  python3 tools/pbr_bake_inplace.py --table q4base/pbr/pbr_materials.cfg --overrides '$live' --reset" >&2
		exit 1
	fi
	[ -f "$AO_DIR/z_baked_ao.pk4" ] || echo "WARNING: no $AO_DIR/z_baked_ao.pk4 - md5 models get no baked AO" >&2
	maps=()
	pats=('maps/game/*.map'); [ "$MP" = 1 ] && pats+=('maps/mp/*.map')
	for m in $(for p in "$RETAIL"/pak*.pk4; do unzip -Z1 "$p" "${pats[@]}" 2>/dev/null || true; done | sort -u); do
		m="${m#maps/}"; maps+=(-m "${m%.map}")
	done
	[ ${#maps[@]} -gt 0 ] || { echo "no maps in $RETAIL/pak*.pk4" >&2; exit 1; }
	search=(-s "$RETAIL")
	[ -d "$AO_DIR" ] && search+=(-s "$AO_DIR")
	[ -d "$Q4SAVE" ] && search+=(-s "$Q4SAVE")
	search+=(-s "$DUDE/q4base")
	echo "== Quake 4: $(( ${#maps[@]} / 2 )) levels + md5 models, from $RETAIL"
	"$DUDE/tools/bake_pbr_sidecars.sh" "${search[@]}" "${maps[@]}" --md5 'models/*' -n "$NAME" -- "${EXTRA[@]}"
fi

src="$DUDE/build/pbr-sidecars/$NAME/$NAME.pk4"
[ -f "$src" ] || { echo "not baked yet: $src" >&2; exit 1; }
mkdir -p "$OUT_DIR"
commit="$(git -C "$DUDE" rev-parse --short HEAD 2>/dev/null || echo unknown)"
stage="$(mktemp -d)"
cp "$src" "$stage/$SHIP.pk4"
adj="$(unzip -p "$src" pbr/pbr_sidecar_baked.cfg | grep -E '^(ao|rough|metal) ' | tr '\n' ';' | sed 's/;$//; s/;/; /g')"
cat > "$stage/BUILDINFO.txt" <<EOF
DUDE PBR sidecar pack (Quake 4): generated roughness / metalness / AO maps + parallax height (alpha), see docs/pbr-texture-maps.md
pack_version: $TAG
packed: $(date -u '+%Y-%m-%d %H:%M UTC')
packed_on_dude_commit: $commit
sidecars: $(unzip -Z1 "$src" | grep -c '_orm\.tga$')
channel_adjust: $adj
place_in: q4base/   (optional; remove it to go back to the PBR table's flat values)
needs: DUDE 0.9.10 or newer, and your own retail Quake 4
licence: derived from Raven Software's Quake 4 textures, Quake 4 modding content - free of charge only
bake: see bake-info.txt
EOF
( cd "$stage" && zip -q -X "$SHIP.pk4" BUILDINFO.txt )
mv "$stage/$SHIP.pk4" "$OUT_DIR/$SHIP.pk4"
rm -rf "$stage"
( cd "$OUT_DIR" && sha256sum "$SHIP.pk4" > "$SHIP.pk4.sha256" )
echo "$OUT_DIR/$SHIP.pk4  ($(du -h "$OUT_DIR/$SHIP.pk4" | cut -f1)) -> q4base/"
echo "done ($TAG). Publish on Inkub0/dude-q4 only (--latest=false), never on a DUDE release - see docs/releasing.md"
