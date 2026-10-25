#!/bin/sh
# Offline test of the engine's effect decl parser (neo/renderer/bse/DeclEffect.cpp, Quake 4's BSE):
# compiles the parser's own source with the real idLexer (build/libidlib.a; stub idCommon and
# decl/model managers that record names), parses every effect decl of the retail paks (later paks
# override earlier ones, as the file system does) plus control effects that must fail or must
# store given values, and compares everything the parser stored with an independent census of
# the text (q4_effect_census.py). Gate: 0 failures, 0 parse warnings, controls as expected, 0
# census mismatches. The reference report (names the retail data lacks) is informational.
# Needs a configured build/. Q4BASE overrides the data path. Never starts the game.
set -e
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
Q4BASE="${Q4BASE:-/mnt/linux-data/SteamLibrary/steamapps/common/Quake 4/q4base}"
WORK="${TMPDIR:-/tmp}/dude-effect-test"
rm -rf "$WORK" && mkdir -p "$WORK/fx"

python3 - "$Q4BASE" "$WORK/fx" <<'PY'
import glob, os, sys, zipfile
files = {}
for pak in sorted(glob.glob(sys.argv[1] + "/*.pk4")):
    try:
        z = zipfile.ZipFile(pak)
    except zipfile.BadZipFile:
        continue
    for n in z.namelist():
        if n.lower().startswith("effects/") and n.lower().endswith(".fx"):
            files[n.lower()] = (pak, n)                  # later paks override earlier ones
for key, (pak, n) in files.items():
    dst = os.path.join(sys.argv[2], n)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, "wb").write(zipfile.ZipFile(pak).read(n))
print(f"extracted {len(files)} effect files")
PY

c++ -std=gnu++17 -O1 -g -msse4.1 -fno-strict-aliasing -w \
	-DD3_ARCH=\"x86_64\" -DD3_HAS_SIMD_SSE41=1 -DD3_IS_BIG_ENDIAN=0 -DD3_OSTYPE=\"linux\" -DD3_SIZEOFPTR=8 \
	-I"$DIR/tools/quake4/tests" -I"$DIR/build" -I"$DIR/neo" \
	"$DIR/tools/quake4/tests/effect_parse_test.cpp" "$DIR/build/libidlib.a" -lpthread \
	-o "$WORK/effect_parse_test"

status=0
python3 - "$WORK/fx" <<'PY' | "$WORK/effect_parse_test" > "$WORK/parsed.txt" 2> "$WORK/warnings.txt" || status=1
import glob, os, sys
root = sys.argv[1]
out = sys.stdout.buffer
for path in sorted(glob.glob(os.path.join(root, "effects", "**", "*.fx"), recursive=True)):
    data = open(path, "rb").read()
    out.write(f"{os.path.relpath(path, root)}\n{len(data)}\n".encode() + data)
PY

python3 "$DIR/tools/quake4/q4_effect_census.py" census "$WORK/fx" > "$WORK/census.txt"
grep -E "^(== |FAIL |FILEWARN |  CONTROL|  control)" "$WORK/parsed.txt" || true
grep "^== " "$WORK/census.txt"
python3 "$DIR/tools/quake4/q4_effect_census.py" compare "$WORK/parsed.txt" "$WORK/census.txt" || status=1
python3 "$DIR/tools/quake4/q4_effect_census.py" refs "$Q4BASE" "$WORK/parsed.txt"
echo "(parser output: $WORK/parsed.txt, warnings: $WORK/warnings.txt)"
exit $status
