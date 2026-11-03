#!/usr/bin/env python3
"""Equivalence test of rtEmissiveTargetRow (the emitter's RIS target: reach verdict, colour, direction, distance) between two
shader trees, on the same random emitter rows and receivers, run on the iGPU (RADV).

  em_test.py OLD_DIR NEW_DIR        (needs numpy)

Each tree's rows are packed in ITS OWN emitter row layout (RhiWorld.cpp RB_RHI_GatherSceneLights), detected from its
rt_emissive.glsl: the old one keeps the centroid in pos and an octahedral axis in projF.x (rtEmissiveOctDecode present), the
current one keeps the unit axis in pos and sin(half-angle) in projF.x (centroid = cull.xyz). So a tree before and after a row
layout change can be compared as well as two trees with the same layout.
The receivers mix near-field points (0.5-4 bounding radii, where the solid-angle maths matters), far points and back-facing
normals. Report p99.9 and the maximum, not only the median (tools/shaderstats/README.md).
"""
import struct
import subprocess
import sys
from pathlib import Path
import numpy as np
from common import HERE, RADV_ENV, compile_glsl, expand, tool

f32 = np.float32
HARNESS = """#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
layout( local_size_x = 64 ) in;
#include "rt_light_eval.glsl"
#include "rt_emissive.glsl"
layout( push_constant ) uniform PC { uvec2 base; uint n; uint mode; } pc;
layout( buffer_reference, std430, buffer_reference_align = 16 ) buffer Hdr { uint lightsOff; uint queryOff; uint outOff; uint pad; };
layout( buffer_reference, std430, buffer_reference_align = 16 ) buffer Vec4s { vec4 v[]; };
uvec2 addOff( uvec2 a, uint off ) { uint c; uint lo = uaddCarry( a.x, off, c ); return uvec2( lo, a.y + c ); }
void main() {
	uint i = gl_GlobalInvocationID.x;
	if ( i >= pc.n ) { return; }
	Hdr h = Hdr( pc.base );
	RtLights lights = RtLights( addOff( pc.base, h.lightsOff ) );
	Vec4s Q = Vec4s( addOff( pc.base, h.queryOff ) ), O = Vec4s( addOff( pc.base, h.outOff ) );
	vec4 a = Q.v[ 2u * i ], b = Q.v[ 2u * i + 1u ];
	vec3 c, ld; float dl, ndl;
	bool ok = rtEmissiveTargetRow( lights, floatBitsToUint( a.w ), a.xyz, b.xyz, c, ld, dl, ndl );
	O.v[ 2u * i ] = vec4( c, ok ? 1.0 : 0.0 );
	O.v[ 2u * i + 1u ] = vec4( ld, dl );
}
"""
NR, NQ = 400, 200000


def bits(u):
    return struct.unpack("<f", struct.pack("<I", u))[0]


def unit(v):
    return v / np.linalg.norm(v)


def oct_cpu(a):
    """The old layout's octahedral axis (RhiWorld.cpp before the unit-axis rows), float32 like the C++."""
    a = a.astype(f32)
    l1 = f32(abs(a[0]) + abs(a[1]) + abs(a[2]))
    ox = f32(a[0] / l1) if l1 > 0 else f32(0)
    oy = f32(a[1] / l1) if l1 > 0 else f32(0)
    if a[2] < 0:
        ox, oy = f32((f32(1) - abs(oy)) * (1 if ox >= 0 else -1)), f32((f32(1) - abs(ox)) * (1 if oy >= 0 else -1))
    sx = int(np.clip(np.floor(f32(ox * f32(32767)) + f32(0.5)), -32767, 32767)) & 0xFFFF
    sy = int(np.clip(np.floor(f32(oy * f32(32767)) + f32(0.5)), -32767, 32767)) & 0xFFFF
    return bits(sx | (sy << 16))


def make_rows(rng):
    rows = []
    for _ in range(NR):
        R = f32(rng.uniform(40, 900))
        rows.append(dict(cen=rng.uniform(-800, 800, 3).astype(f32), br=f32(rng.uniform(1, 40)), R=R,
                         axis=unit(rng.normal(size=3)).astype(f32),
                         co=f32(-1.0) if rng.random() < 0.3 else f32(rng.uniform(-0.5, 1.0)), both=rng.random() < 0.2,
                         prof=float(rng.integers(0, 3)), two=1.0 if rng.random() < 0.2 else 0.0,
                         knee=f32(0.25 * R) if rng.random() < 0.5 else f32(0), nofade=1.0 if rng.random() < 0.5 else 0.0,
                         color=rng.uniform(0.05, 3.0, 3).astype(f32)))
    return rows


def pack(rows, old_layout):
    LO, QO = 256, 256 + 128 * NR
    OO = QO + 32 * NQ
    buf = bytearray(OO + 32 * NQ)
    struct.pack_into("<4I", buf, 0, LO, QO, OO, 0)
    for i, r in enumerate(rows):
        o = LO + 128 * i
        pos = r["cen"] if old_layout else r["axis"]
        fx = oct_cpu(r["axis"]) if old_layout else float(np.sqrt(max(f32(1) - r["co"] * r["co"], f32(0)), dtype=f32))
        # cull, pos + radius, colour + flags (EMISSIVE), projS (.w = intensity), projT (.z profile, .w two-sided),
        # projQ (first triangle, count, knee, no-fade), projF (axis / sin, gradient, haze, cos [+4 = both faces])
        struct.pack_into("<4f3ff3fI", buf, o, *r["cen"], r["br"] + r["R"], *pos, r["R"], *r["color"], 64)
        struct.pack_into("<4f", buf, o + 48, 1, 1, 1, 1)
        struct.pack_into("<4f", buf, o + 64, 0, 0, r["prof"], r["two"])
        struct.pack_into("<4f", buf, o + 80, bits(0), bits(8), r["knee"], r["nofade"])
        struct.pack_into("<4f", buf, o + 96, fx, 1.0, 1.0, float(r["co"]) + (4.0 if r["both"] else 0.0))
    qr = np.random.default_rng(7)
    for k in range(NQ):
        ri = int(qr.integers(0, NR))
        r = rows[ri]
        d = qr.choice([qr.uniform(0.5, 1.2) * r["br"], qr.uniform(1.2, 4) * r["br"], qr.uniform(40, 1000)])
        P = r["cen"] + unit(qr.normal(size=3)) * d
        N = unit(qr.normal(size=3))
        if qr.random() < 0.6 and np.dot(N, r["cen"] - P) < 0:
            N = -N
        struct.pack_into("<3ff3ff", buf, QO + 32 * k, *P, bits(ri), *N, 0.0)
    return buf, OO


def run(d, label, rows, out):
    d = Path(d)
    old_layout = "rtEmissiveOctDecode" in (d / "rt_emissive.glsl").read_text()
    spv = compile_glsl(expand(d, HARNESS), ".comp", out / f"em_{label}.comp.spv", env="vulkan1.3")
    buf, OO = pack(rows, old_layout)
    (out / f"em_in_{label}.bin").write_bytes(buf)
    subprocess.run([str(tool("vkrun")), str(spv), str(out / f"em_in_{label}.bin"), str(out / f"em_out_{label}.bin"), "0", str(NQ)],
                   check=True, env=RADV_ENV, stderr=subprocess.DEVNULL)
    print(f"{label}: {d} ({'old octahedral' if old_layout else 'unit-axis'} row layout)")
    return np.frombuffer((out / f"em_out_{label}.bin").read_bytes()[OO:], dtype=np.float32).reshape(NQ, 2, 4)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    out = HERE / "out"
    out.mkdir(exist_ok=True)
    rows = make_rows(np.random.default_rng(2026))
    old, new = run(sys.argv[1], "a", rows, out), run(sys.argv[2], "b", rows, out)
    okO, okN = old[:, 0, 3] > 0.5, new[:, 0, 3] > 0.5
    print(f"{NQ} receivers, {NR} emitter rows: reached A {okO.sum()} B {okN.sum()}, reach-verdict mismatches {np.sum(okO != okN)}")
    both = okO & okN
    lum = [0.2126, 0.7152, 0.0722]
    lo, ln = old[both, 0, :3] @ lum, new[both, 0, :3] @ lum
    rel = np.abs(lo - ln) / np.maximum(np.abs(lo), 1e-30)
    print(f"target luminance: identical {np.mean(rel == 0) * 100:.2f} %, rel diff p50 {np.median(rel):.1e} "
          f"p99.9 {np.quantile(rel, 0.999):.1e} max {rel.max():.1e}")
    print(f"ldir / dl identical: {np.mean(np.all(old[both, 1] == new[both, 1], axis=1)) * 100:.2f} %")
    for k in np.nonzero(okO != okN)[0][:5]:
        print("  mismatch", k, "A", old[k, 0], "B", new[k, 0])


if __name__ == "__main__":
    main()
