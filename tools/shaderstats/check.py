#!/usr/bin/env python3
"""Compile every engine shader of a tree as the build does (prelude + resolved includes), without building the engine:
a quick syntax / type check after a shader edit.   check.py [DIR]   (default neo/shaders)"""
import sys
import tempfile
from pathlib import Path
from common import SHADERS, compile_engine_shader

STAGES = (".vert", ".frag", ".comp", ".tesc", ".tese")


def main():
    d = Path(sys.argv[1]) if len(sys.argv) > 1 else SHADERS
    out = Path(tempfile.mkdtemp(prefix="shaderstats-check-"))
    files = sorted(f for f in d.iterdir() if f.suffix in STAGES)
    fails = 0
    for f in files:
        try:
            compile_engine_shader(d, f.name, out)
        except SystemExit as e:
            fails += 1
            print("FAIL", f.name, str(e)[:600])
    print(f"{len(files)} shaders, {fails} failures")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
