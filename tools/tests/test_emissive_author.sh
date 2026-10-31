#!/bin/sh
# Offline test of the emissive authoring parsers (neo/renderer/RtEmissiveAuthor.cpp): extracts the
# EMAUTH-TEST-BEGIN/END block verbatim, compiles it with the real idLexer (build/libidlib.a, the stub idCommon of
# tools/quake4/tests), scans every .map of Doom 3 + RoE + Lost Mission for light entities and compares each count
# with an independent Python count; runs the per-map file tokenizer cases. Needs a configured build/. Never starts
# the game. D3PAKS (comma-separated pk4 globs) overrides the data paths.
set -e
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
D3PAKS="${D3PAKS:-$DIR/base/pak*.pk4,$DIR/d3xp/pak*.pk4,$DIR/d3le/*.pk4}"
WORK="${TMPDIR:-/tmp}/dude-emauth-test"
rm -rf "$WORK" && mkdir -p "$WORK"

sed -n '/EMAUTH-TEST-BEGIN/,/EMAUTH-TEST-END/p' "$DIR/neo/renderer/RtEmissiveAuthor.cpp" > "$WORK/emauth_gen.inc"
[ -s "$WORK/emauth_gen.inc" ] || { echo "no EMAUTH-TEST block in RtEmissiveAuthor.cpp" >&2; exit 1; }
c++ -std=gnu++17 -O1 -g -msse4.1 -fno-strict-aliasing -w \
	-DD3_ARCH=\"x86_64\" -DD3_HAS_SIMD_SSE41=1 -DD3_IS_BIG_ENDIAN=0 -DD3_OSTYPE=\"linux\" -DD3_SIZEOFPTR=8 \
	-I"$WORK" -I"$DIR/build" -I"$DIR/neo" -I"$DIR/tools/quake4/tests" \
	"$DIR/tools/tests/emissive_author_test.cpp" "$DIR/build/libidlib.a" -lpthread \
	-o "$WORK/emauth_test"

python3 - "$D3PAKS" "$WORK/emauth_test" <<'PY'
import glob, re, subprocess, sys
files = {}
for pattern in sys.argv[1].split(","):
    for pak in sorted(glob.glob(pattern)):
        import zipfile
        try:
            z = zipfile.ZipFile(pak)
        except zipfile.BadZipFile:
            continue
        for n in z.namelist():
            if n.lower().startswith("maps/") and n.lower().endswith(".map"):
                files[n.lower()] = (z, n)          # later paks override earlier ones
stream = b""
expect = {}
ent_re = re.compile(r'\n// entity \d+\n')
for key in sorted(files):
    z, n = files[key]
    data = z.read(n)
    stream += f"{n}\n{len(data)}\n".encode() + data
    # independent count: entity blocks whose header (before the first primitive) has classname light + name + origin
    text = data.decode("latin-1")
    count = 0
    for block in ent_re.split(text)[1:]:
        head = block.split("// primitive")[0]
        kv = dict(re.findall(r'^"([^"]+)" "([^"]*)"', head, re.M))
        if kv.get("classname", "").lower() == "light" and kv.get("name") and kv.get("origin"):
            count += 1
    expect[n] = count
out = subprocess.run([sys.argv[2]], input=stream, capture_output=True).stdout.decode()
bad = 0
total = 0
for line in out.splitlines():
    if line.startswith("tokenizer:") or line.startswith("FAIL"):
        print(line)
        if line.startswith("FAIL") or "FAIL" in line.split("(")[0]:
            bad += 1
        continue
    parts = line.split()
    name, got = parts[0], int(parts[1])
    total += got
    if got != expect.get(name, -1):
        bad += 1
        print(f"MISMATCH {name}: scan {got}, expected {expect.get(name)}")
print(f"{len(expect)} maps, {total} named lights found; {'PASS' if bad == 0 else f'{bad} FAILURES'}")
for line in out.splitlines():
    if "mars_city1.map" in line:
        print("  e.g.", line)
sys.exit(1 if bad else 0)
PY
