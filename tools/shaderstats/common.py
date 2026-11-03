"""Shared helpers for the DUDE shaderstats tools (tools/shaderstats/README.md)."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
SHADERS = REPO / "neo" / "shaders"
BIN = HERE / "bin"
# Mesa RADV only: the Ryzen's integrated GPU. The NVIDIA card (and the game) are never touched.
RADV_ENV = dict(os.environ, VK_ICD_FILENAMES="/usr/share/vulkan/icd.d/radeon_icd.json")


def tool(name):
    p = BIN / name
    if not p.exists():
        sys.exit(f"{p} missing - run tools/shaderstats/build.sh first")
    return p


def expand(d, text, extra=()):
    """Resolve #include "file" textually (as neo/shaders/compile_spv.py does), from shader dir d, then the extra dirs."""
    out = []
    for line in text.splitlines(keepends=True):
        s = line.strip()
        if s.startswith('#include "') and s.endswith('"'):
            name = s[len('#include "'):-1]
            for base in (Path(d), *map(Path, extra)):
                if (base / name).exists():
                    out.append(expand(d, (base / name).read_text(), extra))
                    break
            else:
                raise FileNotFoundError(name)
        else:
            out.append(line)
    return "".join(out)


def compile_glsl(source, stage_suffix, out_spv, env="vulkan1.4"):
    """Compile GLSL source text to SPIR-V; exits with the compiler output on failure."""
    with tempfile.NamedTemporaryFile("w", suffix=stage_suffix, delete=False) as t:
        t.write(source)
        tn = t.name
    r = subprocess.run(["glslangValidator", "-V", "--target-env", env, "-o", str(out_spv), tn], capture_output=True, text=True)
    os.unlink(tn)
    if r.returncode != 0:
        sys.exit("shader compile failed:\n" + (r.stdout + r.stderr))
    return out_spv


def compile_engine_shader(d, name, out_dir, transform=None):
    """An engine shader exactly as the build prepares it: prelude.vk.glsl + `invariant gl_Position;` for vertex stages +
    resolved includes. transform(src) may rewrite the expanded source (e.g. stub ray queries)."""
    d = Path(d)
    f = d / name
    src = expand(d, f.read_text())
    if transform:
        src = transform(src)
    inv = "invariant gl_Position;\n" if f.suffix in (".vert", ".tese") else ""
    full = (d / "prelude.vk.glsl").read_text() + "\n" + inv + src
    out = Path(out_dir) / (name + ".spv")
    return compile_glsl(full, f.suffix, out)


def stats(spvs, isa_out=None):
    """Run vkstats on (vert, frag) or (comp,) SPIR-V; returns {statistic name: value string}."""
    args = [str(tool("vkstats"))] + [str(s) for s in spvs] + ([str(isa_out)] if isa_out else [])
    r = subprocess.run(args, capture_output=True, text=True, env=RADV_ENV, timeout=900)
    st = {}
    for line in r.stdout.splitlines():
        if line.startswith("  "):
            parts = line.split()
            if len(parts) >= 2:
                st[" ".join(parts[:-1])] = parts[-1]
    if not st:
        sys.exit("vkstats failed:\n" + r.stdout + r.stderr)
    return st
