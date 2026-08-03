#!/usr/bin/env python3
"""Validate the shared GLSL shaders for both targets.

Mimics the engine loader: prepend the target prelude, textually resolve
#include "renderparms.glsl", then compile with glslangValidator —
plain GLSL semantics for the GL 3.3 path, -V --target-env vulkan1.4 for SPIR-V.

Usage: python3 neo/shaders/validate.py
"""
import subprocess, sys, tempfile, os
from pathlib import Path

SHADER_DIR = Path(__file__).resolve().parent
preludes = {
    "gl": (SHADER_DIR / "prelude.gl.glsl").read_text(),
    "vk": (SHADER_DIR / "prelude.vk.glsl").read_text(),
}


def expand_includes(text, depth=0):
    # mirror the engine loader: resolve #include "file" textually, recursively
    if depth > 8:
        raise RuntimeError("include depth > 8 (cycle?)")
    out = []
    for line in text.splitlines(keepends=True):
        stripped = line.strip()
        if stripped.startswith('#include "') and stripped.endswith('"'):
            inc = (SHADER_DIR / stripped[len('#include "'):-1]).read_text()
            out.append(expand_includes(inc, depth + 1))
        else:
            out.append(line)
    return "".join(out)


fails = 0
checked = 0
for f in sorted(SHADER_DIR.iterdir()):
    if f.suffix not in (".vert", ".frag"):
        continue
    src = expand_includes(f.read_text())
    for target, prelude in preludes.items():
        # the engine loader injects this for vertex stages (multi-pass depth
        # invariance, replacing ARB_position_invariant)
        invariant = "invariant gl_Position;\n" if f.suffix == ".vert" else ""
        full = prelude + "\n" + invariant + src
        with tempfile.NamedTemporaryFile("w", suffix=f.suffix, delete=False) as tmp:
            tmp.write(full)
            tmpname = tmp.name
        cmd = ["glslangValidator", tmpname]
        if target == "vk":
            cmd = ["glslangValidator", "-V", "--target-env", "vulkan1.4",
                   "-o", os.devnull, tmpname]
        r = subprocess.run(cmd, capture_output=True, text=True)
        checked += 1
        if r.returncode != 0:
            fails += 1
            print(f"FAIL [{target}] {f.name}:")
            out = (r.stdout + r.stderr).replace(tmpname, f.name)
            print("  " + "\n  ".join(out.strip().splitlines()))
        os.unlink(tmpname)

print(f"{checked} compiles, {fails} failures")
sys.exit(1 if fails else 0)
