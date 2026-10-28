#!/bin/sh
# Offline test of the AO baker's GPU tracer (neo/tools/compilers/aobake/aobake_gpu.cpp and
# neo/shaders/aobake.comp) against the engine's CPU tracer: copies the CPU tracer out of aobake.cpp
# verbatim, compiles it with the GPU tracer, the build's embedded SPIR-V (shaders_embedded.cpp) and
# libidlib.a, and traces synthetic models both ways (aobake_gpu_test.cpp). Needs a configured,
# built Vulkan build/ and a GPU with ray query. Runs under the Khronos validation layer when it is
# installed (AOBAKE_VALIDATE=0 to skip). Never starts the game.
set -e
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="${TMPDIR:-/tmp}/dude-aobake-gpu-test"
rm -rf "$WORK" && mkdir -p "$WORK"

python3 - "$DIR/neo/tools/compilers/aobake/aobake.cpp" "$WORK/aobake_cpu_extract.inc" <<'PY'
import re, sys
lines = open(sys.argv[1]).read().split("\n")
def block(pattern):
    r = re.compile(pattern)
    for i, line in enumerate(lines):
        if r.search(line) and not line.rstrip().endswith(";"):
            out = []
            for body in lines[i:]:
                out.append(body)
                if body in ("}", "};"):
                    return "\n".join(out)
    sys.exit(f"aobake extract: nothing matches {pattern}")
parts = [block(p) for p in (
    r"^struct aoTri_t", r"^struct aoGrid_t", r"^static void AO_ClampCell\(", r"^static void AO_InsertTri\(",
    r"^static float AO_RayTri\(", r"^struct aoTraceCtx_t", r"^static float AO_TraceNearest\(",
    r"^static float AO_RadicalInverse\(", r"^static idVec3 AO_CosineHemisphere\(", r"^static void AO_Basis\(",
    r"^static float AO_SampleVisibility\(")]
open(sys.argv[2], "w").write("\n\n".join(parts) + "\n")
PY

c++ -std=gnu++17 -O2 -g -msse4.1 -fno-strict-aliasing -w \
	-DD3_ARCH=\"x86_64\" -DD3_HAS_SIMD_SSE41=1 -DD3_IS_BIG_ENDIAN=0 -DD3_OSTYPE=\"linux\" -DD3_SIZEOFPTR=8 \
	-I"$WORK" -I"$DIR/build" -I"$DIR/neo" -I"$DIR/tools/quake4/tests" \
	"$DIR/tools/aobake/aobake_gpu_test.cpp" "$DIR/neo/tools/compilers/aobake/aobake_gpu.cpp" \
	"$DIR/build/shaders_embedded.cpp" "$DIR/build/libidlib.a" -lvulkan -lpthread \
	-o "$WORK/aobake_gpu_test"

if [ "${AOBAKE_VALIDATE:-1}" = 1 ] && [ -e /usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json ]; then
	export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
fi
"$WORK/aobake_gpu_test" 2>&1 | tee "$WORK/out.txt"
status=$?
if grep -q "Validation Error" "$WORK/out.txt"; then
	echo "FAIL validation errors"
	exit 1
fi
exit $status
