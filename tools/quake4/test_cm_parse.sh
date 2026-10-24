#!/bin/sh
# Offline test of the engine's .cm loader (neo/cm/CollisionModel_files.cpp) on both titles:
# copies the loader's functions out of neo/cm/ (tests/cm_extract.py), compiles them with the
# real idLexer (build/libidlib.a, stub idCommon) and loads every collision file of the retail
# Quake 4 paks and of Doom 3 + RoE + Lost Mission. Needs a configured build/. Q4BASE and D3PAKS
# (comma-separated pk4 globs) override the data paths. Never starts the game.
set -e
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
Q4BASE="${Q4BASE:-/mnt/linux-data/SteamLibrary/steamapps/common/Quake 4/q4base}"
D3PAKS="${D3PAKS:-$DIR/base/pak*.pk4,$DIR/d3xp/pak*.pk4,$DIR/d3le/*.pk4}"
WORK="${TMPDIR:-/tmp}/dude-cm-test"
rm -rf "$WORK" && mkdir -p "$WORK"

python3 "$DIR/tools/quake4/tests/cm_extract.py" "$DIR" "$WORK/cm_parse_gen.inc"
c++ -std=gnu++17 -O1 -g -msse4.1 -fno-strict-aliasing -w \
	-DD3_ARCH=\"x86_64\" -DD3_HAS_SIMD_SSE41=1 -DD3_IS_BIG_ENDIAN=0 -DD3_OSTYPE=\"linux\" -DD3_SIZEOFPTR=8 \
	-I"$WORK" -I"$DIR/build" -I"$DIR/neo" \
	"$DIR/tools/quake4/tests/cm_parse_test.cpp" "$DIR/build/libidlib.a" -lpthread \
	-o "$WORK/cm_parse_test"

feed() {
	python3 - "$1" <<'PY' | "$WORK/cm_parse_test"
import glob, sys, zipfile
files = {}
for pattern in sys.argv[1].split(","):
    for pak in sorted(glob.glob(pattern)):
        try:
            z = zipfile.ZipFile(pak)
        except zipfile.BadZipFile:
            continue
        for n in z.namelist():
            if n.lower().endswith(".cm"):
                files[n.lower()] = (z, n)          # later paks override earlier ones
out = sys.stdout.buffer
for key in sorted(files):
    z, n = files[key]
    data = z.read(n)
    out.write(f"{n}\n{len(data)}\n".encode() + data)
PY
}

status=0
echo "== Quake 4"
feed "$Q4BASE/pak*.pk4" || status=1
echo "== Doom 3 + RoE + Lost Mission"
feed "$D3PAKS" || status=1
exit $status
