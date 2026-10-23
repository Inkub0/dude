#!/usr/bin/env python3
"""Quake 4 material keyword census against DUDE's material parser.

Reads guide-expanded .mtr files (the output directory of tools/quake4/test_guides.sh, so
guides count as the materials they define) and lists every keyword that starts a statement
in a material body (depth 1) or a stage (depth 2) and appears nowhere as a string literal in
neo/renderer/Material.cpp, and every "name(" image-program function Image_program.cpp does
not know. idMaterial::ParseMaterial / ParseStage default the WHOLE material
on an unknown keyword, so each one listed blanks every material that uses it.

Usage: q4_material_census.py <expanded materials dir> [Material.cpp]
"""
import collections
import os
import re
import sys

here = os.path.dirname(os.path.abspath(__file__))
mdir = sys.argv[1] if len(sys.argv) > 1 else "/tmp/dude-guide-test/out/materials"
src = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "..", "..", "neo", "renderer", "Material.cpp")

known = {s.lower() for s in re.findall(r'"([A-Za-z_][A-Za-z0-9_]*)"', open(src, encoding="latin-1").read())}

TOKEN = re.compile(r'"[^"\n]*"|[A-Za-z_][A-Za-z0-9_/.\\-]*|-?\d*\.?\d+|\S')


sys.path.insert(0, here)
from check_guides import strip_comments as strip   # "//" before "/*", like idLexer ("//* x" is a line comment)


unknown = collections.defaultdict(collections.Counter)   # keyword -> {depth: count}
materials_hit = collections.defaultdict(set)
total_materials = 0
for root, _, files in os.walk(mdir):
    for f in files:
        if not f.endswith(".mtr"):
            continue
        text = strip(open(os.path.join(root, f), encoding="latin-1").read())
        depth, parens, current = 0, 0, None
        prev_name = None
        for m in TOKEN.finditer(text):
            tok = m.group(0)
            if tok == "{":
                if depth == 0:
                    current = prev_name
                    total_materials += 1
                depth += 1
                continue
            if tok == "}":
                depth -= 1
                continue
            if tok == "(":
                parens += 1
                continue
            if tok == ")":
                parens = max(0, parens - 1)
                continue
            if depth == 0:
                prev_name = tok
                continue
            if parens:
                continue
            # first token of its line?
            ls = text.rfind("\n", 0, m.start())
            if text[ls + 1:m.start()].strip() not in ("", "{"):
                continue
            low = tok.lower()
            if not re.match(r"[a-z_]", low) or low in known:
                continue
            unknown[low][depth] += 1
            materials_hit[low].add(current)

# image-program functions: "map downsize( x.tga, 1 )" - the image-program parser
# (neo/renderer/Image_program.cpp) reads an unknown function as a file name and leaves the
# "(", which defaults the material
img_src = open(os.path.join(os.path.dirname(src), "Image_program.cpp"), encoding="latin-1").read()
img_known = {s.lower() for s in re.findall(r'"([A-Za-z_][A-Za-z0-9_]*)"', img_src)}
img_unknown = collections.Counter()
for root, _, files in os.walk(mdir):
    for f in files:
        if f.endswith(".mtr"):
            text = strip(open(os.path.join(root, f), encoding="latin-1").read())
            for fn in re.findall(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(", text):
                low = fn.lower()
                if low not in img_known and low not in known and low not in ("if",):
                    img_unknown[low] += 1

print(f"materials: {total_materials}")
print("unknown image-program functions:", dict(img_unknown) or "none")
print(f"{'keyword':28s} {'uses':>5s}  depth(1=material 2=stage)  materials")
for k, c in sorted(unknown.items(), key=lambda kv: -sum(kv[1].values())):
    print(f"{k:28s} {sum(c.values()):5d}  {dict(c)}  {len(materials_hit[k])}  e.g. {sorted(m for m in materials_hit[k] if m)[:2]}")
