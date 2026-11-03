#!/usr/bin/env python3
"""Per-function ACO cost (RADV) of the rt_emissive.glsl entry points, one column per shader tree.

  micro.py [label=DIR ...]            (default: now=neo/shaders)

e.g. `micro.py old=/tmp/master-shaders new=neo/shaders` after `git archive master neo/shaders | tar -x -C /tmp/master-shaders`.
Each function is wrapped in a minimal compute kernel that reads its inputs from a buffer, so the numbers are the function's
own instructions (plus a fixed few for the loads/stores). Add a function to BODY to measure it.
"""
import sys
import tempfile
from pathlib import Path
from common import SHADERS, compile_glsl, expand, stats

HEAD = """#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
layout( local_size_x = 64 ) in;
#include "rt_light_eval.glsl"
#include "rt_emissive.glsl"
layout( push_constant ) uniform PC { uvec2 base; uint n; uint mode; } pc;
layout( buffer_reference, std430, buffer_reference_align = 16 ) buffer Vec4s { vec4 v[]; };
void main() {
	uint i = gl_GlobalInvocationID.x;
	Vec4s Q = Vec4s( pc.base );
	vec4 a = Q.v[ 2u * i ], b = Q.v[ 2u * i + 1u ];
	vec3 P = a.xyz, N = b.xyz; uint row = floatBitsToUint( a.w ), tri = floatBitsToUint( b.w );
	RtLights lights = RtLights( pc.base );
	RtEmissiveTable t = rtEmissiveOpen( pc.base );
"""
BODY = {
    "TriWeight": "	Q.v[ i ].x = rtEmissiveTriWeight( t, tri, P, N, pc.mode & 3u, ( pc.mode & 4u ) != 0u, a.w );\n",
    "TargetRow": "	vec3 c, ld; float dl, ndl; bool ok = rtEmissiveTargetRow( lights, row, P, N, c, ld, dl, ndl ); Q.v[ i ] = vec4( c, ok ? dl : ndl );\n",
    "ShadeRow(1spp)": "	vec3 c, ld; float dl, ndl; bool ok = rtEmissiveShadeRow( lights, row, P, N, tri, 1u, true, c, ld, dl, ndl ); Q.v[ i ] = vec4( c, ok ? g_rtEmVisDist : 0.0 );\n",
}


def main():
    trees = [a.split("=", 1) for a in sys.argv[1:]] or [["now", str(SHADERS)]]
    out = Path(tempfile.mkdtemp(prefix="shaderstats-micro-"))
    for name, body in BODY.items():
        cells = []
        for label, d in trees:
            spv = compile_glsl(expand(d, HEAD + body + "}\n"), ".comp", out / f"m_{label}.comp.spv", env="vulkan1.3")
            st = stats([spv])
            cells.append(f"{label}: VALU {st.get('VALU')} instr {st.get('Instructions')} VGPRs {st.get('VGPRs')}")
        print(f"{name:15s} " + " | ".join(cells))


if __name__ == "__main__":
    main()
