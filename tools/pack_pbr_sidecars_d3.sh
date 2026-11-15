#!/bin/bash
# Bakes and packs the Doom 3 family PBR sidecar packs (docs/pbr-texture-maps.md), the optional
# downloads published next to the baked AO pack, versioned on their OWN axis (v<N>): re-published
# when the baker or its settings change, not with every DUDE release.
#
#   base  Doom 3                       -> dist/d3-pbr-sidecars/z_pbr_sidecars.pk4      into base/
#   roe   Resurrection of Evil         -> dist/d3-pbr-sidecars/z_pbr_sidecars_roe.pk4  into d3xp/
#   lm    The Lost Mission (d3le)      -> dist/d3-pbr-sidecars/z_pbr_sidecars_lm.pk4   into d3le/
#
# Each pack covers every level of its game (maps/game/**.map in its pk4s, multiplayer included) and
# every md5 model, with the game's own material decls (RoE reads base + d3xp, LM base + d3xp + d3le),
# so each one works on its own. They go in the game's own folder, not all in base/ like the baked
# AO: RoE and LM redefine some of Doom 3's materials, and a sidecar is named after its diffuse, so a
# merged pack would give one game the other's roughness / metalness. d3xp/ and d3le/ load after
# base/, so their pack wins there.
#
# Channel adjust: pbr/pbr_sidecar_adjust.cfg (the PBR editor's All sidecars tab, "Save for next
# bake"), read from the first game folder; every pack records it in pbr/pbr_sidecar_baked.cfg.
# Bake all three with the same settings.
#
# Usage:  tools/pack_pbr_sidecars_d3.sh v<N> [base] [roe] [lm]
#   Bakes the listed games (default: all three) with tools/bake_pbr_sidecars.sh into
#   build/pbr-sidecars/<pack>/, then collects all three packs (baked now or earlier) into
#   dist/d3-pbr-sidecars/ with a BUILDINFO.txt inside each and a .sha256 next to it.
#   Nothing is installed: copy each pack into its folder with the game closed.
#   Env: D3XP=<dir> D3LE=<dir> (default: d3xp/ and d3le/ next to base/), OUT_DIR=<dir>
set -euo pipefail
DUDE="$(cd "$(dirname "$0")/.." && pwd)"
TAG="${1:-}"
[[ "$TAG" =~ ^v?[0-9]+$ ]] || { sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 2; }
TAG="v${TAG#v}"; shift
GAMES=("$@"); [ ${#GAMES[@]} -gt 0 ] || GAMES=(base roe lm)
BASE="$DUDE/base"; D3XP="${D3XP:-$DUDE/d3xp}"; D3LE="${D3LE:-$DUDE/d3le}"
OUT_DIR="${OUT_DIR:-$DUDE/dist/d3-pbr-sidecars}"

pack_of() { case "$1" in base) echo z_pbr_sidecars ;; roe) echo z_pbr_sidecars_roe ;; lm) echo z_pbr_sidecars_lm ;; *) return 1 ;; esac; }
dirs_of() { case "$1" in base) echo "$BASE" ;; roe) echo "$BASE $D3XP" ;; lm) echo "$BASE $D3XP $D3LE" ;; esac; }
folder_of() { case "$1" in base) echo base ;; roe) echo d3xp ;; lm) echo d3le ;; esac; }
title_of() { case "$1" in base) echo "Doom 3" ;; roe) echo "Resurrection of Evil" ;; lm) echo "The Lost Mission" ;; esac; }

for g in "${GAMES[@]}"; do
	pack_of "$g" >/dev/null || { echo "unknown game '$g' (base, roe, lm)" >&2; exit 2; }
	dirs=($(dirs_of "$g")); own="${dirs[-1]}"
	[ -d "$own" ] || { echo "$g: no game folder $own" >&2; exit 1; }
	# the game's own levels: its pk4s' maps/game/**.map
	maps=()
	for m in $(for p in "$own"/*.pk4; do unzip -Z1 "$p" 'maps/game/*.map' 2>/dev/null || true; done | sort -u); do
		m="${m#maps/}"; maps+=(-m "${m%.map}")
	done
	[ ${#maps[@]} -gt 0 ] || { echo "$g: no maps/game/*.map in $own/*.pk4" >&2; exit 1; }
	search=(); for d in "${dirs[@]}"; do search+=(-s "$d"); done
	echo "== $(title_of "$g"): $(( ${#maps[@]} / 2 )) levels + md5 models"
	"$DUDE/tools/bake_pbr_sidecars.sh" "${search[@]}" "${maps[@]}" --md5 'models/md5/*' -n "$(pack_of "$g")"
done

mkdir -p "$OUT_DIR"
commit="$(git -C "$DUDE" rev-parse --short HEAD 2>/dev/null || echo unknown)"
for g in base roe lm; do
	name="$(pack_of "$g")"; src="$DUDE/build/pbr-sidecars/$name/$name.pk4"
	[ -f "$src" ] || { echo "skipped $name: not baked yet ($src)"; continue; }
	stage="$(mktemp -d)"
	cp "$src" "$stage/$name.pk4"
	adj="$(unzip -p "$src" pbr/pbr_sidecar_baked.cfg | grep -E '^(ao|rough|metal) ' | tr '\n' ';' | sed 's/;$//; s/;/; /g')"
	cat > "$stage/BUILDINFO.txt" <<EOF
DUDE PBR sidecar pack ($(title_of "$g")): generated roughness / metalness / AO maps, see docs/pbr-texture-maps.md
pack_version: $TAG
packed: $(date -u '+%Y-%m-%d %H:%M UTC')
packed_on_dude_commit: $commit
sidecars: $(unzip -Z1 "$src" | grep -c '_orm\.tga$')
channel_adjust: $adj
place_in: $(folder_of "$g")/   (optional; remove it to go back to the PBR table's flat values)
bake: see bake-info.txt
EOF
	( cd "$stage" && zip -q -X "$name.pk4" BUILDINFO.txt )
	mv "$stage/$name.pk4" "$OUT_DIR/$name.pk4"
	rm -rf "$stage"
	( cd "$OUT_DIR" && sha256sum "$name.pk4" > "$name.pk4.sha256" )
	echo "$OUT_DIR/$name.pk4  ($(du -h "$OUT_DIR/$name.pk4" | cut -f1)) -> $(folder_of "$g")/"
done
echo "done ($TAG). Publish as release assets on the PBR sidecars vN axis (--latest=false), see docs/releasing.md"
