#!/usr/bin/env python3
"""Build-time GLSL -> SPIR-V compilation for the Vulkan backend (Phase 4 M0).

Prepares each shader exactly the way the engine loader and validate.py do:
prepend prelude.vk.glsl, inject `invariant gl_Position;` for vertex stages
(multi-pass depth invariance, replacing ARB_position_invariant), textually
resolve #include "file", then compile to <out>/<name>.spv (name keeps the
.vert/.frag suffix, so generic.vert -> generic.vert.spv).

Only shaders whose .spv is missing or older than the source (or a prelude /
include / this script) are recompiled.

Usage: compile_spv.py --compiler /path/to/glslangValidator|glslc --out DIR
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

SHADER_DIR = Path(__file__).resolve().parent


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    outdir = Path(args.out)
    outdir.mkdir(parents=True, exist_ok=True)
    compiler = Path(args.compiler)
    is_glslang = "glslang" in compiler.name.lower()

    prelude = (SHADER_DIR / "prelude.vk.glsl").read_text()
    # anything that, when changed, must trigger a full recompile
    deps_mtime = max(
        (SHADER_DIR / "prelude.vk.glsl").stat().st_mtime,
        Path(__file__).stat().st_mtime,
        *((SHADER_DIR / g).stat().st_mtime for g in os.listdir(SHADER_DIR)
          if g.endswith(".glsl") and g != "prelude.gl.glsl"),
    )

    fails = 0
    compiled = 0
    skipped = 0
    for f in sorted(SHADER_DIR.iterdir()):
        if f.suffix not in (".vert", ".frag", ".tesc", ".tese"):
            continue
        spv = outdir / (f.name + ".spv")
        if spv.exists() and spv.stat().st_mtime >= max(f.stat().st_mtime, deps_mtime):
            skipped += 1
            continue

        src = expand_includes(f.read_text())
        # gl_Position must be invariant in whichever stage finalizes it so the
        # depth prepass and the interaction pass agree bit-for-bit (multi-pass
        # depth-EQUAL). That is the vertex stage normally, or the tessellation
        # evaluation stage when a draw is tessellated (docs/tessellation.md).
        invariant = "invariant gl_Position;\n" if f.suffix in (".vert", ".tese") else ""
        full = prelude + "\n" + invariant + src

        # the temp file must keep the stage suffix so both compilers detect it
        with tempfile.NamedTemporaryFile("w", suffix=f.suffix, delete=False) as tmp:
            tmp.write(full)
            tmpname = tmp.name
        if is_glslang:
            cmd = [str(compiler), "-V", "--target-env", "vulkan1.4",
                   "-o", str(spv), tmpname]
        else:  # glslc
            cmd = [str(compiler), "--target-env=vulkan1.4",
                   "-o", str(spv), tmpname]
        r = subprocess.run(cmd, capture_output=True, text=True)
        os.unlink(tmpname)
        if r.returncode != 0:
            fails += 1
            spv.unlink(missing_ok=True)
            out = (r.stdout + r.stderr).replace(tmpname, f.name)
            print(f"FAIL {f.name}:")
            print("  " + "\n  ".join(out.strip().splitlines()))
        else:
            compiled += 1

    print(f"SPIR-V: {compiled} compiled, {skipped} up to date, {fails} failures")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
