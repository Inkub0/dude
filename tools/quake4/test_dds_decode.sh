#!/bin/sh
# Offline test of the engine's DDS source decoder (LoadDDS in neo/renderer/Image_files.cpp, the
# last resort of R_LoadImage when a texture exists only as a .dds): compiles the decoder's own
# lines, verbatim, with stub file I/O (tests/dds_decode_test.cpp) and compares what it decodes
# with Pillow's DDS decoder over Quake 4 files of every format the paks hold (DXT1, DXT3, DXT5,
# RXGB, uncompressed 32-bit) - RXGB as DXT5 with red read back from alpha. Needs a configured
# build/ and Pillow. Q4BASE overrides the data path. Never starts the game.
set -e
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
Q4BASE="${Q4BASE:-/mnt/linux-data/SteamLibrary/steamapps/common/Quake 4/q4base}"
WORK="${TMPDIR:-/tmp}/dude-dds-test"
rm -rf "$WORK" && mkdir -p "$WORK"

python3 - "$DIR" "$WORK/dds_extract.inc" <<'PY'
import sys
root, out = sys.argv[1], sys.argv[2]
image_h = open(f"{root}/neo/renderer/Image.h", encoding="latin-1").read()
start = image_h.index("// surface description flags") if "// surface description flags" in image_h else image_h.index("const unsigned int DDSF_")
end = image_h.index("} ddsFileHeader_t;") + len("} ddsFileHeader_t;")
files = open(f"{root}/neo/renderer/Image_files.cpp", encoding="latin-1").read()
dstart = files.index("static void DDS_Color565")
dend = files.index("//====", files.index("static void LoadDDS( const char *name, byte **pic, int *width, int *height, ID_TIME_T *timestamp ) {"))
open(out, "w").write(image_h[start:end] + "\n\n" + files[dstart:dend])
PY

c++ -std=gnu++17 -O1 -g -msse4.1 -fno-strict-aliasing -w \
	-DD3_ARCH=\"x86_64\" -DD3_HAS_SIMD_SSE41=1 -DD3_IS_BIG_ENDIAN=0 -DD3_OSTYPE=\"linux\" -DD3_SIZEOFPTR=8 \
	-I"$DIR/build" -I"$DIR/neo" -I"$WORK" -I"$DIR/tools/quake4/tests" \
	"$DIR/tools/quake4/tests/dds_decode_test.cpp" "$DIR/build/libidlib.a" -lpthread \
	-o "$WORK/dds_decode_test"

python3 - "$Q4BASE" "$WORK/dds_decode_test" <<'PY'
import glob, io, struct, subprocess, sys, zipfile
from PIL import Image

base, exe = sys.argv[1], sys.argv[2]
per_format = 6
samples, seen = [], {}
for pak in sorted(glob.glob(f"{base}/pak0*.pk4")):
    z = zipfile.ZipFile(pak)
    for n in z.namelist():
        if not n.lower().endswith(".dds"):
            continue
        with z.open(n) as f:
            h = f.read(128)
        if h[:4] != b"DDS ":
            continue
        flags, fourcc, bits = struct.unpack_from("<I", h, 80)[0], h[84:88], struct.unpack_from("<I", h, 88)[0]
        caps2 = struct.unpack_from("<I", h, 112)[0]
        if caps2 & 0x200:
            continue                                     # cube maps are not a source-image case
        fmt = fourcc.decode("latin-1") if flags & 4 else f"rgb{bits}"
        if seen.get(fmt, 0) < per_format or "shell_loader_s" in n:
            seen[fmt] = seen.get(fmt, 0) + 1
            samples.append((fmt, n, z.read(n)))

feed = b"".join(f"{n}\n{len(d)}\n".encode() + d for _, n, d in samples)
out = subprocess.run([exe], input=feed, capture_output=True, check=True).stdout
pos, failures = 0, 0
for fmt, n, data in samples:
    eol = out.index(b"\n", pos)
    name, w, h = out[pos:eol].decode().rsplit(" ", 2)
    w, h = int(w), int(h)
    pos = eol + 1
    got = out[pos:pos + w * h * 4]
    pos += w * h * 4
    ref_data = data
    if fmt == "RXGB":                                    # Pillow knows it as DXT5
        ref_data = data[:84] + b"DXT5" + data[88:]
    ref = Image.open(io.BytesIO(ref_data)).convert("RGBA")
    if (w, h) != ref.size:
        print(f"FAIL {fmt} {n}: decoded {w}x{h}, Pillow {ref.size[0]}x{ref.size[1]}")
        failures += 1
        continue
    exp = bytearray(ref.tobytes())
    if fmt == "RXGB":                                    # red comes back from alpha, alpha opaque
        for i in range(0, len(exp), 4):
            exp[i], exp[i + 3] = exp[i + 3], 255
    worst = max(abs(a - b) for a, b in zip(got, exp))
    off = sum(1 for a, b in zip(got, exp) if abs(a - b) > 2) / len(exp)
    state = "ok" if worst <= 2 else "FAIL"
    failures += state != "ok"
    print(f"{state:4} {fmt:5} {w:5}x{h:<5} max channel diff {worst:3}, >2 off {off:.4%}  {n}")
print(f"{len(samples)} files, {failures} failed")
sys.exit(1 if failures else 0)
PY
