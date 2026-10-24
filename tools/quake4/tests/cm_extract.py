#!/usr/bin/env python3
"""Copies the engine's .cm loading code out of neo/cm/ for tests/cm_parse_test.cpp.

The parse functions cannot be compiled with their whole module (it needs the file system,
decl manager and renderer interfaces), so this lifts them verbatim - every function the
loader runs from LoadCollisionModelFile down, the contents-name tables and the block
allocators - with one substitution: the idLexer that opens the file becomes the test's
in-memory lexer. Fails loudly when a function is missing, so a rename breaks the test
instead of silently testing stale code.

Usage: cm_extract.py <repo root> <output .inc>
"""
import re
import sys


def function(lines, pattern):
    r = re.compile(pattern)
    for i, line in enumerate(lines):
        if r.search(line) and not line.rstrip().endswith(";"):
            out = []
            for body in lines[i:]:
                out.append(body)
                if body in ("}", "};"):
                    return "\n".join(out)
    sys.exit(f"cm_extract.py: no definition matches {pattern}")


def main():
    root, output = sys.argv[1], sys.argv[2]
    read = lambda name: open(f"{root}/neo/cm/{name}").read().split("\n")
    files, load, debug = read("CollisionModel_files.cpp"), read("CollisionModel_load.cpp"), read("CollisionModel_debug.cpp")
    parts = [line for line in files if line.startswith("#define CM_")]
    parts.append(function(debug, r"^const char \*cm_contentsNameByIndex\[\]"))
    parts.append(function(debug, r"^int cm_contentsFlagByIndex\[\]"))
    parts.append(function(debug, r"idCollisionModelManagerLocal::ContentsFromString\("))
    for name in ("AllocModel", "AllocNode", "AllocPolygon", "AllocBrush"):
        parts.append(function(load, rf"idCollisionModelManagerLocal::{name}\("))
    for name in ("ParseVertices", "ParseEdges", "ParseNodes"):
        parts.append(function(files, rf"idCollisionModelManagerLocal::{name}\("))
    parts.append(function(files, r"^static int ParseBlockSize\("))
    parts.append(function(files, r"^static void CheckBlockCounts\("))
    for name in ("ParsePolygons", "ParseBrushes", "ParseCollisionModel", "LoadCollisionModelFile"):
        parts.append(function(files, rf"idCollisionModelManagerLocal::{name}\("))
    text = "\n\n".join(parts) + "\n"
    if text.count("new idLexer( fileName )") != 1:
        sys.exit("cm_extract.py: LoadCollisionModelFile no longer opens its file with new idLexer( fileName )")
    open(output, "w").write(text.replace("new idLexer( fileName )", "new MemoryLexer( fileName )"))


if __name__ == "__main__":
    main()
