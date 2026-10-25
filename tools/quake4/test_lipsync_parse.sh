#!/bin/sh
# Offline test of Quake 4's lip-sync phrases against the real idLexer (build/libidlib.a, which the
# game library links): every viseme string of every language in the retail paks must parse the way
# the game walks it (tools/quake4/tests/lipsync_parse_test.cpp). The French, Italian and Spanish
# phrases hold Western European letters; a lexer that takes them for white space fails ~2000 of
# them, each a fatal error in the game. Needs a built build/. Q4BASE overrides the data path.
# Never starts the game.
set -e
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
Q4BASE="${Q4BASE:-/mnt/linux-data/SteamLibrary/steamapps/common/Quake 4/q4base}"
WORK="${TMPDIR:-/tmp}/dude-lipsync-test"
rm -rf "$WORK" && mkdir -p "$WORK"

c++ -std=gnu++17 -O1 -g -msse4.1 -fno-strict-aliasing -w \
	-DD3_ARCH=\"x86_64\" -DD3_HAS_SIMD_SSE41=1 -DD3_IS_BIG_ENDIAN=0 -DD3_OSTYPE=\"linux\" -DD3_SIZEOFPTR=8 \
	-I"$DIR/tools/quake4/tests" -I"$DIR/build" -I"$DIR/neo" \
	"$DIR/tools/quake4/tests/lipsync_parse_test.cpp" "$DIR/build/libidlib.a" -lpthread \
	-o "$WORK/lipsync_parse_test"

python3 - "$Q4BASE" <<'PY' | "$WORK/lipsync_parse_test"
import glob, re, sys, zipfile
files = {}
for pak in sorted(glob.glob(sys.argv[1] + "/pak*.pk4")):
    try:
        z = zipfile.ZipFile(pak)
    except zipfile.BadZipFile:
        continue
    for n in z.namelist():
        if n.lower().endswith(".lipsync"):
            files[n.lower()] = z.read(n)          # later paks override earlier ones
out = sys.stdout.buffer
for key in sorted(files):
    for decl in re.finditer(rb'lipSync\s+(\S+)\s*\{(.*?)\n\}', files[key], re.S):
        for v in re.finditer(rb'visemes\s+"(\w+)"\s+"([^"]*)"', decl.group(2)):
            name = decl.group(1) + b"/" + v.group(1)
            out.write(name + b"\n" + str(len(v.group(2))).encode() + b"\n" + v.group(2))
PY
