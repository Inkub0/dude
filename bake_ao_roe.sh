#!/usr/bin/env bash
# bake_ao_roe.sh — bake the AO maps for Resurrection of Evil (d3xp), headless, via bake_ao.sh.
#
# Bakes every model RoE adds (characters/monsters under models/md5 incl. its weapons and
# cinematics, props under models/mapobjects, static weapons under models/weapons) that base Doom 3
# does not already ship -- base's packs cover the shared ones. Maps land in
# <savepath>/d3xp/generated/aomaps (default ~/.local/share/dude/d3xp/generated/aomaps).
#
# Why not +bakeAOFolder: with fs_game d3xp, base is mounted too, so a folder bake would also
# re-bake all of base's props into the d3xp tree. So the RoE-only list is computed from the pk4s
# and fed as individual +bakeAO commands, in batches: the engine holds at most 32 '+' command
# lines (com_consoleLines in Common.cpp, unchecked -- more crashes it), and bake_ao.sh + this
# script already use 8 of them.
#
# Usage:
#   ./bake_ao_roe.sh            bake all of them (~13 launches of the game under xvfb)
#   ./bake_ao_roe.sh --list     print the model list and exit (no launch)
#   AO_GPU=0 ./bake_ao_roe.sh   any bake_ao.sh tunable (AO_RAYS, AO_SIZE, AO_GPU, ...) passes through

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BATCH=20

if [ ! -d "$HERE/d3xp" ]; then
	echo "bake_ao_roe.sh: no d3xp/ next to this script" >&2
	exit 1
fi

# RoE models that base does not have (case-insensitive, later paks override earlier ones)
mapfile -t MODELS < <(python3 - "$HERE" <<'PY'
import glob, sys, zipfile
root = sys.argv[1]
roots = ("models/md5/", "models/mapobjects/", "models/weapons/")
def models(pattern):
    found = {}
    for pak in sorted(glob.glob(pattern)):
        try:
            z = zipfile.ZipFile(pak)
        except zipfile.BadZipFile:
            continue
        for n in z.namelist():
            l = n.lower()
            if l.startswith(roots) and l.endswith((".lwo", ".ase", ".md5mesh")):
                found[l] = n
    return found
roe = models(f"{root}/d3xp/*.pk4")
base = models(f"{root}/base/*.pk4")
for key in sorted(roe):
    if key not in base:
        print(roe[key])
PY
)

if [ "${#MODELS[@]}" -eq 0 ]; then
	echo "bake_ao_roe.sh: found no RoE models in $HERE/d3xp/*.pk4" >&2
	exit 1
fi
if [ "${1:-}" = "--list" ]; then
	printf '%s\n' "${MODELS[@]}"
	echo "${#MODELS[@]} models" >&2
	exit 0
fi

total=${#MODELS[@]}
batches=$(( ( total + BATCH - 1 ) / BATCH ))
echo "bake_ao_roe.sh: $total RoE models in $batches batches of up to $BATCH"
failed=()
for (( b = 0; b < batches; b++ )); do
	args=()
	for m in "${MODELS[@]:b * BATCH:BATCH}"; do
		args+=( +bakeAO "$m" )
	done
	echo "== batch $(( b + 1 ))/$batches"
	if ! "$HERE/bake_ao.sh" +set fs_basepath "$HERE" +set fs_game d3xp "${args[@]}"; then
		failed+=( $(( b + 1 )) )
	fi
done

echo
if [ "${#failed[@]}" -gt 0 ]; then
	echo "bake_ao_roe.sh: batches FAILED: ${failed[*]} (re-run; the others are done)" >&2
	exit 1
fi
echo "bake_ao_roe.sh: all $batches batches done -> ~/.local/share/dude/d3xp/generated/aomaps (default savepath)"
