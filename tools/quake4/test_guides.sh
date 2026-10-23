#!/bin/sh
# Offline test of the engine's Quake 4 material-guide expansion (neo/framework/DeclGuides.cpp):
# extract the retail .mtr/.guide files, expand them with the engine's C++ (linked against
# build/libidlib.a, stub idCommon), and diff the result against an independent Python
# expansion. Needs a configured build/ (for libidlib.a and config.h). Q4BASE overrides the
# Quake 4 data path. Never starts the game.
set -e
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
Q4BASE="${Q4BASE:-/mnt/linux-data/SteamLibrary/steamapps/common/Quake 4/q4base}"
WORK="${TMPDIR:-/tmp}/dude-guide-test"
rm -rf "$WORK" && mkdir -p "$WORK/src" "$WORK/out"

python3 - "$Q4BASE" "$WORK/src" <<'PY'
import os, sys, zipfile
base, out = sys.argv[1], sys.argv[2]
seen = set()
for pak in sorted((p for p in os.listdir(base) if p.endswith(".pk4")), reverse=True):
    z = zipfile.ZipFile(os.path.join(base, pak))
    for n in z.namelist():
        if n.endswith((".mtr", ".guide")) and n not in seen:
            seen.add(n)
            dst = os.path.join(out, n)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, "wb").write(z.read(n))
print(f"extracted {len(seen)} files")
PY

c++ -std=gnu++17 -O1 -g -msse4.1 -fno-strict-aliasing -w \
	-DD3_ARCH=\"x86_64\" -DD3_HAS_SIMD_SSE41=1 -DD3_IS_BIG_ENDIAN=0 -DD3_OSTYPE=\"linux\" -DD3_SIZEOFPTR=8 \
	-I"$DIR/build" -I"$DIR/neo" \
	"$DIR/tools/quake4/tests/guide_expand_test.cpp" "$DIR/build/libidlib.a" -lpthread \
	-o "$WORK/guide_expand_test"

"$WORK/guide_expand_test" "$WORK/src" "$WORK/out"
python3 "$DIR/tools/quake4/check_guides.py" "$WORK/src" "$WORK/out"
