#!/bin/sh
# Offline test of the engine's AAS loader (neo/tools/compilers/aas/AASFile.cpp) on both titles:
# compiles the loader's own source with the real idLexer (build/libidlib.a, stub idCommon) and
# loads every navigation file of the retail Quake 4 paks (DewmAAS 1.08: five sizes per map, dummy
# files, tactical features) and of Doom 3 + RoE + Lost Mission (1.07). Needs a configured build/.
# Q4BASE and D3PAKS (comma-separated pk4 globs) override the data paths. Never starts the game.
set -e
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
Q4BASE="${Q4BASE:-/mnt/linux-data/SteamLibrary/steamapps/common/Quake 4/q4base}"
D3PAKS="${D3PAKS:-$DIR/base/pak*.pk4,$DIR/d3xp/pak*.pk4,$DIR/d3le/*.pk4}"
WORK="${TMPDIR:-/tmp}/dude-aas-test"
rm -rf "$WORK" && mkdir -p "$WORK"

c++ -std=gnu++17 -O1 -g -msse4.1 -fno-strict-aliasing -w \
	-DD3_ARCH=\"x86_64\" -DD3_HAS_SIMD_SSE41=1 -DD3_IS_BIG_ENDIAN=0 -DD3_OSTYPE=\"linux\" -DD3_SIZEOFPTR=8 \
	-I"$DIR/build" -I"$DIR/neo" \
	"$DIR/tools/quake4/tests/aas_parse_test.cpp" "$DIR/build/libidlib.a" -lpthread \
	-o "$WORK/aas_parse_test"

feed() {
	python3 - "$1" <<'PY' | "$WORK/aas_parse_test"
import glob, re, sys, zipfile
files = {}
for pattern in sys.argv[1].split(","):
    for pak in sorted(glob.glob(pattern)):
        try:
            z = zipfile.ZipFile(pak)
        except zipfile.BadZipFile:
            continue
        for n in z.namelist():
            if re.search(r"\.aas\w*$", n.lower()):
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
