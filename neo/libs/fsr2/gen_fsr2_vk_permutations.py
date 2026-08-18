#!/usr/bin/env python3
# Generate the FidelityFX FSR2 Vulkan SPIR-V permutation headers on Linux, replacing the
# Windows-only FidelityFX_SC.exe for the glslang/Vulkan path. Uses the system glslang
# (glslangValidator) + spirv-dis (SPIRV-Tools) only. The emitted *_permutations.h match the
# format that FSR2's ffx_fsr2_shaders_vk.cpp #includes (per-pass PermutationKey union,
# g_<name>_IndirectionTable[], g_<name>_PermutationInfo[]). One-time offline step: the output
# headers are vendored into neo/libs/fsr2/vk/shaders/. See docs/fsr-temporal-pipeline.md (C0).
#
# Usage: gen_fsr2_vk_permutations.py <fsr2-api-src-dir> <output-dir>
#   <fsr2-api-src-dir> = the upstream src/ffx-fsr2-api directory (shaders/ + vk/shaders/)
#   <output-dir>       = where to write ffx_fsr2_*_pass_permutations.h

import subprocess, sys, os, itertools, hashlib, re

if len(sys.argv) != 3:
    sys.exit("usage: gen_fsr2_vk_permutations.py <fsr2-api-src-dir> <output-dir>")
API = os.path.abspath(sys.argv[1])
OUT = os.path.abspath(sys.argv[2])
SH   = os.path.join(API, "shaders")
VKSH = os.path.join(API, "vk", "shaders")
os.makedirs(OUT, exist_ok=True)

GLSLANG = "glslangValidator"
SPIRVDIS = "spirv-dis"

# Fixed base defines (FFX_SC_BASE_ARGS + the VK base args from vk/CMakeLists.txt).
BASE_DEFINES = [
    "FFX_GPU=1", "FFX_GLSL=1",
    "FFX_FSR2_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
    "FFX_FSR2_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
    "FFX_FSR2_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
    "FFX_FSR2_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
    "FFX_FSR2_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
]

# The permuted options, in PermutationKey bitfield order (bit0..bitN). Each is (keyField,
# defineName). This order MUST match POPULATE_PERMUTATION_KEY in ffx_fsr2_shaders_vk.cpp so
# the emitted union's little-endian .index packing lines up with the runtime option bits.
OPTIONS = [
    ("FFX_FSR2_OPTION_REPROJECT_USE_LANCZOS_TYPE",    "FFX_FSR2_OPTION_REPROJECT_USE_LANCZOS_TYPE"),
    ("FFX_FSR2_OPTION_HDR_COLOR_INPUT",               "FFX_FSR2_OPTION_HDR_COLOR_INPUT"),
    ("FFX_FSR2_OPTION_LOW_RESOLUTION_MOTION_VECTORS", "FFX_FSR2_OPTION_LOW_RESOLUTION_MOTION_VECTORS"),
    ("FFX_FSR2_OPTION_JITTERED_MOTION_VECTORS",       "FFX_FSR2_OPTION_JITTERED_MOTION_VECTORS"),
    ("FFX_FSR2_OPTION_INVERTED_DEPTH",                "FFX_FSR2_OPTION_INVERTED_DEPTH"),
    ("FFX_FSR2_OPTION_APPLY_SHARPENING",              "FFX_FSR2_OPTION_APPLY_SHARPENING"),
    ("FFX_HALF",                                      "FFX_HALF"),
]

# (glsl base name, permutes FFX_HALF?). Luminance pyramid fixes FFX_HALF=0 (vk/CMakeLists.txt),
# and its PermutationKey omits the FFX_HALF field (ffx_fsr2_shaders_vk.cpp).
PASSES = [
    ("ffx_fsr2_tcr_autogen_pass",                 True),
    ("ffx_fsr2_autogen_reactive_pass",            True),
    ("ffx_fsr2_accumulate_pass",                  True),
    ("ffx_fsr2_compute_luminance_pyramid_pass",   False),
    ("ffx_fsr2_depth_clip_pass",                  True),
    ("ffx_fsr2_lock_pass",                        True),
    ("ffx_fsr2_reconstruct_previous_depth_pass",  True),
    ("ffx_fsr2_rcas_pass",                        True),
]

def compile_spv(passfile, defines):
    args = [GLSLANG, "--target-env", "vulkan1.1", "-S", "comp", "-e", "main", "-Os"]
    for d in BASE_DEFINES + defines:
        args += ["-D" + d]
    args += ["-I" + SH, "-I" + VKSH, "-o", "/dev/stdout", passfile]
    # glslang writes the binary to the -o path; use a temp file (it won't stream to stdout).
    tmp = os.path.join(OUT, "_tmp.spv")
    args[args.index("/dev/stdout")] = tmp
    r = subprocess.run(args, capture_output=True)
    if r.returncode != 0 or not os.path.exists(tmp):
        sys.exit("glslang failed for %s\n%s\n%s" % (passfile, r.stdout.decode(errors='replace'), r.stderr.decode(errors='replace')))
    with open(tmp, "rb") as f:
        data = f.read()
    os.remove(tmp)
    return data

# Parse spirv-dis text to classify each bound resource as storage image / sampled image /
# uniform buffer, with its name and binding. Returns (storage, sampled, uniform) lists of
# (name, binding), each sorted by binding.
def reflect(spv):
    dis = subprocess.run([SPIRVDIS, "--no-color", "-"], input=spv, capture_output=True)
    if dis.returncode != 0:
        sys.exit("spirv-dis failed:\n" + dis.stderr.decode(errors='replace'))
    txt = dis.stdout.decode(errors='replace')
    names, bindings, ptr_pointee, ptr_sc = {}, {}, {}, {}
    image_sampled, sampledimage, sampler_types, block_structs = {}, set(), set(), set()
    var_type, var_sc = {}, {}
    for line in txt.splitlines():
        line = line.strip()
        m = re.match(r'%(\S+)\s*=\s*OpTypePointer\s+(\w+)\s+%(\S+)', line)
        if m: ptr_sc[m.group(1)] = m.group(2); ptr_pointee[m.group(1)] = m.group(3); continue
        m = re.match(r'%(\S+)\s*=\s*OpTypeImage\s+%\S+\s+\w+\s+\d+\s+\d+\s+\d+\s+(\d+)', line)
        if m: image_sampled[m.group(1)] = int(m.group(2)); continue
        m = re.match(r'%(\S+)\s*=\s*OpTypeSampledImage', line)
        if m: sampledimage.add(m.group(1)); continue
        m = re.match(r'%(\S+)\s*=\s*OpTypeSampler\b', line)
        if m: sampler_types.add(m.group(1)); continue
        m = re.match(r'%(\S+)\s*=\s*OpVariable\s+%(\S+)\s+(\w+)', line)
        if m: var_type[m.group(1)] = m.group(2); var_sc[m.group(1)] = m.group(3); continue
        m = re.match(r'OpName\s+%(\S+)\s+"([^"]*)"', line)
        if m: names[m.group(1)] = m.group(2); continue
        m = re.match(r'OpDecorate\s+%(\S+)\s+Binding\s+(\d+)', line)
        if m: bindings[m.group(1)] = int(m.group(2)); continue
        m = re.match(r'OpDecorate\s+%(\S+)\s+Block', line)
        if m: block_structs.add(m.group(1)); continue
    storage, sampled, uniform = [], [], []
    for var, binding in bindings.items():
        if var not in var_type:
            continue
        name = names.get(var, var)
        pt = ptr_pointee.get(var_type[var])
        # Immutable samplers (s_PointClamp / s_LinearClamp) live in descriptor set 0 and are
        # created by the VK backend itself (pImmutableSamplers) -- they are NOT part of the
        # per-shader blob reflection (Fsr2ShaderBlobVK has no sampler category). Exclude them.
        if pt in sampler_types or name.startswith('s_'):
            continue
        cat = None
        if pt in image_sampled:
            cat = 'storage' if image_sampled[pt] == 2 else 'sampled'
        elif pt in sampledimage:
            cat = 'sampled'
        else:
            cat = 'uniform'  # Uniform/StorageBuffer block
        # cross-check against FSR2's naming convention (rw_ / r_ / cb) to catch parser drift
        exp = 'storage' if name.startswith('rw_') else ('uniform' if name.startswith('cb') else 'sampled')
        if exp != cat:
            sys.stderr.write("WARN %s: type-class %s != name-class %s\n" % (name, cat, exp))
        {'storage': storage, 'sampled': sampled, 'uniform': uniform}[cat].append((name, binding))
    for lst in (storage, sampled, uniform):
        lst.sort(key=lambda t: t[1])
    return storage, sampled, uniform

def carr_u8(name, data):
    body = ",".join(str(b) for b in data)
    return "static const uint8_t %s[] = {%s};\n" % (name, body)

def carr_names(name, items):
    if not items: return "static const char* %s[] = { 0 };\n" % name
    return "static const char* %s[] = {%s};\n" % (name, ",".join('"%s"' % n for n, _ in items))

def carr_binds(name, items):
    if not items: return "static const uint32_t %s[] = { 0 };\n" % name
    return "static const uint32_t %s[] = {%s};\n" % (name, ",".join(str(b) for _, b in items))

def gen_pass(base, uses_half):
    passfile = os.path.join(SH, base + ".glsl")
    opts = OPTIONS if uses_half else OPTIONS[:-1]   # drop FFX_HALF for luminance pyramid
    nbits = len(opts)
    uniq = {}          # spv-hash -> blob index
    blobs = []         # list of (spv, storage, sampled, uniform)
    table = [0] * (1 << nbits)
    for idx in range(1 << nbits):
        defines = []
        for bit, (keyfield, defname) in enumerate(opts):
            defines.append("%s=%d" % (defname, (idx >> bit) & 1))
        if not uses_half:
            defines.append("FFX_HALF=0")
        spv = compile_spv(passfile, defines)
        h = hashlib.sha1(spv).hexdigest()
        if h not in uniq:
            uniq[h] = len(blobs)
            blobs.append((spv, ) + reflect(spv))
        table[idx] = uniq[h]

    lines = []
    lines.append("// Generated by gen_fsr2_vk_permutations.py (native glslang, replaces FidelityFX_SC).\n")
    lines.append("// Pass: %s | %d permutations -> %d unique blobs\n" % (base, 1 << nbits, len(blobs)))
    lines.append("#include <stdint.h>\n\n")
    for i, (spv, st, sa, un) in enumerate(blobs):
        words = len(spv) // 4
        lines.append(carr_u8("g_%s_permutation_%d" % (base, i), spv))
        lines.append(carr_names("g_%s_permutation_%d_storageImageResourceNames" % (base, i), st))
        lines.append(carr_binds("g_%s_permutation_%d_storageImageResourceBindings" % (base, i), st))
        lines.append(carr_names("g_%s_permutation_%d_sampledImageResourceNames" % (base, i), sa))
        lines.append(carr_binds("g_%s_permutation_%d_sampledImageResourceBindings" % (base, i), sa))
        lines.append(carr_names("g_%s_permutation_%d_uniformBufferResourceNames" % (base, i), un))
        lines.append(carr_binds("g_%s_permutation_%d_uniformBufferResourceBindings" % (base, i), un))
        lines.append("\n")
    # PermutationKey union
    lines.append("typedef union %s_PermutationKey {\n\tstruct {\n" % base)
    for keyfield, _ in opts:
        lines.append("\t\tuint32_t %s : 1;\n" % keyfield)
    lines.append("\t};\n\tuint32_t index;\n} %s_PermutationKey;\n\n" % base)
    # PermutationInfo struct
    lines.append("typedef struct %s_PermutationInfo {\n" % base)
    lines.append("\tconst uint32_t\tblobSize;\n\tconst uint8_t*\tblobData;\n")
    lines.append("\tconst uint32_t\tnumStorageImageResources;\n\tconst uint32_t\tnumSampledImageResources;\n\tconst uint32_t\tnumUniformBufferResources;\n")
    lines.append("\tconst char**\tstorageImageResourceNames;\n\tconst uint32_t*\tstorageImageResourceBindings;\n")
    lines.append("\tconst char**\tsampledImageResourceNames;\n\tconst uint32_t*\tsampledImageResourceBindings;\n")
    lines.append("\tconst char**\tuniformBufferResourceNames;\n\tconst uint32_t*\tuniformBufferResourceBindings;\n")
    lines.append("} %s_PermutationInfo;\n\n" % base)
    # IndirectionTable
    lines.append("static const int32_t g_%s_IndirectionTable[] = {\n\t%s\n};\n\n" % (base, ",".join(str(t) for t in table)))
    # PermutationInfo array
    lines.append("static const %s_PermutationInfo g_%s_PermutationInfo[] = {\n" % (base, base))
    for i, (spv, st, sa, un) in enumerate(blobs):
        lines.append("\t{ %d, g_%s_permutation_%d, %d, %d, %d, "
                     "g_%s_permutation_%d_storageImageResourceNames, g_%s_permutation_%d_storageImageResourceBindings, "
                     "g_%s_permutation_%d_sampledImageResourceNames, g_%s_permutation_%d_sampledImageResourceBindings, "
                     "g_%s_permutation_%d_uniformBufferResourceNames, g_%s_permutation_%d_uniformBufferResourceBindings },\n"
                     % (len(spv), base, i, len(st), len(sa), len(un),
                        base, i, base, i, base, i, base, i, base, i, base, i))
    lines.append("};\n")
    path = os.path.join(OUT, base + "_permutations.h")
    with open(path, "w") as f:
        f.write("".join(lines))
    print("  %s: %d perms -> %d blobs -> %s" % (base, 1 << nbits, len(blobs), os.path.basename(path)))

def main():
    print("Generating FSR2 VK permutation headers (native glslang):")
    for base, uses_half in PASSES:
        gen_pass(base, uses_half)
    print("done ->", OUT)

if __name__ == "__main__":
    main()
