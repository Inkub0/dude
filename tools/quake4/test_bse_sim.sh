#!/bin/sh
# Offline test of the engine's Quake 4 effect runtime (neo/renderer/bse/BseEffect.cpp): compiles the
# runtime, the effect decl parser and the table decl parser with the real idLexer
# (build/libidlib.a), loads the retail envelope tables, and plays every retail effect once and
# looping at 60 Hz, evaluating every live particle each frame (tests/bse_sim_test.cpp). Gate: no
# number that is not finite, every non-looping effect ends, no segment over its particle budget.
# Physics (S5): every effect plays in a closed room; bounces, removals and rests are checked, the
# child effects it asks the game for are played back (real decls, three deep) and the requests
# counted and checked (finite, bounded, every chain ends, every stopped looping effect ends).
# Also reports how the particles' reach compares with each effect's radius by the retail bounds
# formula, and that radius against the file's own "size". Needs a configured build/. Q4BASE
# overrides the data path. Never starts the game.
set -e
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
Q4BASE="${Q4BASE:-/mnt/linux-data/SteamLibrary/steamapps/common/Quake 4/q4base}"
WORK="${TMPDIR:-/tmp}/dude-bse-sim-test"
rm -rf "$WORK" && mkdir -p "$WORK"

# the effects and the table decls (later paks override earlier ones, as the file system does)
python3 - "$Q4BASE" "$WORK" <<'PY'
import glob, os, re, sys, zipfile
base, work = sys.argv[1], sys.argv[2]
effects, materials = {}, {}
for pak in sorted(glob.glob(base + "/*.pk4")):
    try:
        z = zipfile.ZipFile(pak)
    except zipfile.BadZipFile:
        continue
    for n in z.namelist():
        low = n.lower()
        if low.startswith("effects/") and low.endswith(".fx"):
            effects[low] = (pak, n)
        elif low.endswith(".mtr"):
            materials[low] = (pak, n)
with open(os.path.join(work, "effects.bin"), "wb") as out:
    for key in sorted(effects):
        pak, n = effects[key]
        data = zipfile.ZipFile(pak).read(n)
        out.write(f"{n}\n{len(data)}\n".encode() + data)
tables = {}
for key in sorted(materials):
    pak, n = materials[key]
    text = zipfile.ZipFile(pak).read(n).decode("latin-1")
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    for m in re.finditer(r"(?:^|\s)table\s+(\S+)\s*\{", text):
        depth, i = 1, m.end()
        while depth and i < len(text):
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
        tables[m.group(1).lower()] = "table " + m.group(1) + " " + text[m.end() - 1:i]
with open(os.path.join(work, "tables.bin"), "wb") as out:
    for name in sorted(tables):
        data = tables[name].encode("latin-1")
        out.write(f"{name}\n{len(data)}\n".encode() + data)
print(f"{len(effects)} effect files, {len(tables)} tables")
PY

c++ -std=gnu++17 -O2 -g -msse4.1 -fno-strict-aliasing -w \
	-DD3_ARCH=\"x86_64\" -DD3_HAS_SIMD_SSE41=1 -DD3_IS_BIG_ENDIAN=0 -DD3_OSTYPE=\"linux\" -DD3_SIZEOFPTR=8 \
	-I"$DIR/tools/quake4/tests" -I"$DIR/build" -I"$DIR/neo" \
	"$DIR/tools/quake4/tests/bse_sim_test.cpp" "$DIR/build/libidlib.a" -lpthread \
	-o "$WORK/bse_sim_test"

"$WORK/bse_sim_test" "$WORK/tables.bin" < "$WORK/effects.bin" 2> "$WORK/warnings.txt"
