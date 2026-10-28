#!/usr/bin/env bash
# bake_ao_mod.sh — bake the AO maps for an expansion / mod, headless, via bake_ao.sh.
#
# Bakes every model the mod's pk4s add (characters/monsters/cinematics under models/md5, props
# under models/mapobjects, static weapons under models/weapons) that the games it builds on do not
# already ship -- their own baked packs cover the shared ones. Maps land in
# <savepath>/<game>/generated/aomaps (default ~/.local/share/dude/<game>/generated/aomaps).
#
#   ./bake_ao_mod.sh d3xp              Resurrection of Evil (on base)
#   ./bake_ao_mod.sh d3le d3xp         The Lost Mission (on base + RoE, as it is played)
#   ./bake_ao_mod.sh d3le d3xp --list  print the model list and exit (no launch)
#   AO_GPU=0 ./bake_ao_mod.sh ...      any bake_ao.sh tunable (AO_RAYS, AO_SIZE, AO_GPU, ...) passes through
#
# Why not +bakeAOFolder: with fs_game set, base (and fs_game_base) are mounted too, so a folder
# bake would also re-bake their models into the mod's tree. So the mod-only list is computed from
# the pk4s and fed as individual +bakeAO commands, in batches: the engine holds at most 32 '+'
# command lines (com_consoleLines in Common.cpp, unchecked -- more crashes it), and bake_ao.sh +
# this script use up to 9 of them.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BATCH=20

GAME="${1:-}"
BASEGAME=""
LIST=0
shift || true
for arg in "$@"; do
	case "$arg" in
		--list) LIST=1 ;;
		*)      BASEGAME="$arg" ;;
	esac
done
if [ -z "$GAME" ] || [ "$GAME" = "--list" ]; then
	echo "usage: $0 <game dir> [<base game dir>] [--list]   e.g. $0 d3xp  |  $0 d3le d3xp" >&2
	exit 2
fi
for dir in "$GAME" $BASEGAME; do
	if [ ! -d "$HERE/$dir" ]; then
		echo "bake_ao_mod.sh: no $dir/ next to this script" >&2
		exit 1
	fi
done

# the mod's models that base (and the base game) do not have (case-insensitive)
mapfile -t MODELS < <(python3 - "$HERE" "$GAME" $BASEGAME <<'PY'
import glob, sys, zipfile
root, game, parents = sys.argv[1], sys.argv[2], ["base"] + sys.argv[3:]
roots = ("models/md5/", "models/mapobjects/", "models/weapons/")
def models(dirname):
    found = {}
    for pak in sorted(glob.glob(f"{root}/{dirname}/*.pk4")):
        try:
            z = zipfile.ZipFile(pak)
        except zipfile.BadZipFile:
            continue
        for n in z.namelist():
            l = n.lower()
            if l.startswith(roots) and l.endswith((".lwo", ".ase", ".md5mesh")):
                found[l] = n
    return found
mod = models(game)
shipped = set()
for p in parents:
    shipped |= set(models(p))
for key in sorted(mod):
    if key not in shipped:
        print(mod[key])
PY
)

if [ "${#MODELS[@]}" -eq 0 ]; then
	echo "bake_ao_mod.sh: found no models in $HERE/$GAME/*.pk4 that ${BASEGAME:+$BASEGAME / }base lack" >&2
	exit 1
fi
if [ "$LIST" -eq 1 ]; then
	printf '%s\n' "${MODELS[@]}"
	echo "${#MODELS[@]} models" >&2
	exit 0
fi

mount=( +set fs_basepath "$HERE" +set fs_game "$GAME" )
if [ -n "$BASEGAME" ]; then
	mount+=( +set fs_game_base "$BASEGAME" )
fi

total=${#MODELS[@]}
batches=$(( ( total + BATCH - 1 ) / BATCH ))
echo "bake_ao_mod.sh: $total $GAME models in $batches batches of up to $BATCH"
failed=()
for (( b = 0; b < batches; b++ )); do
	args=()
	for m in "${MODELS[@]:b * BATCH:BATCH}"; do
		args+=( +bakeAO "$m" )
	done
	echo "== batch $(( b + 1 ))/$batches"
	if ! "$HERE/bake_ao.sh" "${mount[@]}" "${args[@]}"; then
		failed+=( $(( b + 1 )) )
	fi
done

echo
if [ "${#failed[@]}" -gt 0 ]; then
	echo "bake_ao_mod.sh: batches FAILED: ${failed[*]} (re-run; the others are done)" >&2
	exit 1
fi
echo "bake_ao_mod.sh: all $batches batches done -> ~/.local/share/dude/$GAME/generated/aomaps (default savepath)"
