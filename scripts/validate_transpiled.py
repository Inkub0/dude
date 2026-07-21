#!/usr/bin/env python3
"""Transpile the ARB shader corpus and validate the GLSL for both targets.

Runs arbtool glsl over testdata/glprogs/*, then compiles every output with
glslangValidator as GLSL 330 (GL) and SPIR-V (Vulkan 1.0), resolving the
arbparams.glsl include and prepending the per-target prelude — mirroring
what the engine loader will do.

Usage: python3 scripts/validate_transpiled.py
"""
import subprocess, sys, tempfile, os
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SHADERS = ROOT / "neo" / "shaders"
CORPUS = ROOT / "testdata" / "glprogs"
OUT = ROOT / "testdata" / "transpiled"
ARBTOOL = ROOT / "build" / "arbtool"

arbparams = (SHADERS / "arbparams.glsl").read_text()
preludes = {
    "gl": (SHADERS / "prelude.gl.glsl").read_text(),
    "vk": (SHADERS / "prelude.vk.glsl").read_text(),
}

# 1. transpile
OUT.mkdir(parents=True, exist_ok=True)
sources = sorted(
    p for p in CORPUS.rglob("*")
    if p.suffix in (".vfp", ".vp") or (p.suffix == ".txt" and "arb" in p.name.lower())
)
if not sources:
    sys.exit("no corpus found — run scripts/harvest_shader_corpus.sh first")

transpile_fails = 0
for src in sources:
    sub = OUT / src.parent.name
    sub.mkdir(exist_ok=True)
    r = subprocess.run([str(ARBTOOL), "glsl", str(sub), str(src)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        transpile_fails += 1
        print(r.stderr.strip())

# 2. validate
checked = fails = 0
for f in sorted(OUT.rglob("*")):
    if f.suffix not in (".vert", ".frag"):
        continue
    src = f.read_text().replace('#include "arbparams.glsl"', arbparams)
    for target, prelude in preludes.items():
        full = prelude + "\n" + src
        with tempfile.NamedTemporaryFile("w", suffix=f.suffix, delete=False) as tmp:
            tmp.write(full)
            tmpname = tmp.name
        cmd = ["glslangValidator", tmpname]
        if target == "vk":
            cmd = ["glslangValidator", "-V", "--target-env", "vulkan1.0", "-o", os.devnull, tmpname]
        r = subprocess.run(cmd, capture_output=True, text=True)
        checked += 1
        if r.returncode != 0:
            fails += 1
            out = (r.stdout + r.stderr).replace(tmpname, str(f))
            print(f"FAIL [{target}] {f.parent.name}/{f.name}:")
            print("  " + "\n  ".join(out.strip().splitlines()[:8]))
        os.unlink(tmpname)

print(f"\ntranspile failures: {transpile_fails}; glsl compiles: {checked}, failures: {fails}")
sys.exit(1 if (fails or transpile_fails) else 0)
