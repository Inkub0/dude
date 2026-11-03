#!/usr/bin/env python3
"""Compile the ReSTIR / RT shaders of a shader tree as the build does and print RADV/ACO statistics per program.

  probe.py [--tree DIR] [--nort] [--peaks N] [program ...]

--tree   shader directory (default neo/shaders; e.g. a `git archive master neo/shaders` copy for a before/after)
--nort   stub every ray query (proceed = false, getters = uniforms): RADV traverses in software, which hides the SHADING code's
         register pressure - stubbed, the numbers are a better proxy for GPUs with hardware traversal (NVIDIA)
--peaks  also print the N basic blocks with the highest VGPR demand (ACO_DEBUG=liveinfo) and their dominant instructions,
         to find which inlined code sets the register count
Programs default to the ReSTIR set below. Statistics: VGPRs (allocated), waves (per SIMD), Instructions, VALU, VMEM, Latency
(ACO's static estimate), Scratch. AMD numbers, a proxy: NVIDIA's own count comes from `+set r_vkPipelineStats restir_di`.
"""
import argparse
import re
import subprocess
import tempfile
from collections import Counter
from pathlib import Path
from common import SHADERS, RADV_ENV, compile_engine_shader, stats, tool

PROGRAMS = {	# program -> vertex stage (None = compute)
    "restir_di": "restir_di", "restir_di_boost": "restir_di_boost", "restir_di_full": "restir_di_full",
    "restir_di_boost_full": "restir_di_boost_full", "restir_di_young": "restir_di_young", "restir_di_gradient": "restir_di_gradient",
    "restir_di_apply": "restir_di_apply", "rtgi_ray": "rtgi_ray", "light_grid_fill.comp": None,
}
KEYS = [("VGPRs", "VGPRs"), ("Subgroups per SIMD", "waves"), ("Instructions", "instr"), ("VALU", "VALU"), ("VMEM", "VMEM"),
        ("Latency", "latency"), ("Scratch size", "scratch")]
A = r"\(\s*\w+\s*,\s*(?:true|false)\s*\)"
STUBS = [(r"rayQueryProceedEXT\(\s*\w+\s*\)", "( u_rtParms.x < -1e30 )"),
         (r"rayQueryGetIntersectionTypeEXT" + A, "uint( u_rtParms.z )"),
         (r"rayQueryGetIntersectionTEXT" + A, "u_rtParms.w"),
         (r"rayQueryGetIntersectionInstanceCustomIndexEXT" + A, "int( u_localParam0.z )"),
         (r"rayQueryGetIntersectionGeometryIndexEXT" + A, "int( u_localParam0.w )"),
         (r"rayQueryGetIntersectionPrimitiveIndexEXT" + A, "int( u_localParam1.z )"),
         (r"rayQueryGetIntersectionBarycentricsEXT" + A, "u_localParam0.xy"),
         (r"rayQueryGetIntersectionObjectToWorldEXT" + A, "mat4x3( u_modelViewMatrix )")]


def stub_rt(src):
    for pat, rep in STUBS:
        src = re.sub(pat, rep, src)
    return src


def peaks(spvs, n):
    r = subprocess.run([str(tool("vkstats"))] + [str(s) for s in spvs], capture_output=True, text=True,
                       env=dict(RADV_ENV, ACO_DEBUG="liveinfo", RADV_DEBUG="shaders"), timeout=900)
    L = r.stderr.splitlines()
    res, cur = [], None
    for i, l in enumerate(L):
        m = re.match(r"^(BB\d+)", l)
        if m:
            cur = (m.group(1), i)
        m = re.search(r"demand: (\d+) vgpr", l)
        if m and cur:
            res.append((int(m.group(1)), cur[0], cur[1]))
    for d, bb, i in sorted(res, reverse=True)[:n]:
        ops = Counter(m.group(1) for l in L[i:i + 40] for m in [re.search(r"= ([a-z_0-9]+)", l)] if m)
        print(f"      {d:4d} vgpr demand  {bb:7s}  {', '.join(k for k, _ in ops.most_common(7))}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default=str(SHADERS))
    ap.add_argument("--nort", action="store_true")
    ap.add_argument("--peaks", type=int, default=0)
    ap.add_argument("programs", nargs="*")
    a = ap.parse_args()
    out = Path(tempfile.mkdtemp(prefix="shaderstats-"))
    for prog in (a.programs or list(PROGRAMS)):
        vert = PROGRAMS.get(prog, prog)
        tr = stub_rt if a.nort else None
        if vert is None:
            spvs = [compile_engine_shader(a.tree, prog, out, tr)]
        else:
            spvs = [compile_engine_shader(a.tree, vert + ".vert", out), compile_engine_shader(a.tree, prog + ".frag", out, tr)]
        st = stats(spvs)
        print(f"{prog:24s} " + "  ".join(f"{short}={st.get(k, '?')}" for k, short in KEYS))
        if a.peaks:
            peaks(spvs, a.peaks)


if __name__ == "__main__":
    main()
