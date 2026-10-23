#!/usr/bin/env python3
"""Replica of DUDE's .cm loader (neo/cm/CollisionModel_files.cpp) run over retail files.

Walks every collision model file token by token in the order LoadCollisionModelFile,
ParseCollisionModel, ParseVertices, ParseEdges, ParseNodes, ParsePolygons and ParseBrushes
read it, with the same version handling: Doom 3's CM "1.00"; Quake 4's CM "3" (a number
after the model name, two-count block headers, polygon texture coordinates plus primitive
number, a brush primitive number) and CM "2" (a few pak019 models: polygon texture
coordinates only). Also checks that a Quake 4 block header's counts match the rows (the
engine sizes its allocation block from them) and that every brush contents name is one the
engine knows.

Usage: q4_cm_check.py <pk4 glob>[,<pk4 glob>...]
"""
import collections
import glob
import re
import sys
import zipfile

TOKEN = re.compile(r'/\*.*?\*/|//[^\n]*|"[^"]*"|[(){},]|[^\s(){},"]+', re.S)
NUMBER = re.compile(r"-?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?")
CONTENTS = {"solid", "opaque", "water", "playerclip", "monsterclip", "moveableclip", "ikclip", "blood",
            "body", "corpse", "trigger", "aas_solid", "aas_obstacle", "flashlight_trigger",
            # Quake 4 (neo/cm/CollisionModel_debug.cpp cm_contentsNameByIndex)
            "sightclip", "largeshotclip", "notacticalfeatures", "vehicleclip", "flyclip", "itemclip",
            "projectileclip", "shotclip"}


class CmError(Exception):
    pass


class Lexer:
    def __init__(self, text):
        self.toks = [t for t in TOKEN.findall(text) if not t.startswith(("/*", "//"))]
        self.i = 0

    def peek(self, k=0):
        j = self.i + k
        return self.toks[j] if j < len(self.toks) else None

    def read(self):
        t = self.peek()
        if t is None:
            raise CmError("unexpected end of file")
        self.i += 1
        return t

    def expect(self, s):
        t = self.read()
        if t != s:
            raise CmError(f"expected '{s}' but found '{t}' (token {self.i})")

    def check(self, s):
        if self.peek() == s:
            self.i += 1
            return True
        return False

    def is_number(self, k=0):
        t = self.peek(k)
        return t is not None and NUMBER.fullmatch(t) is not None

    def number(self):
        t = self.read()
        if not NUMBER.fullmatch(t):
            raise CmError(f"expected a number but found '{t}' (token {self.i})")
        return float(t)

    def matrix(self, n):
        self.expect("(")
        v = [self.number() for _ in range(n)]
        self.expect(")")
        return v


def parse_nodes(lx, st):
    # iterative form of the recursive ParseNodes
    pending = 1
    while pending:
        lx.expect("(")
        plane_type = int(lx.number())
        lx.number()
        lx.expect(")")
        pending -= 1
        st["nodes"] += 1
        if plane_type != -1:
            pending += 2


def block_header(lx):
    nums = []
    while len(nums) < 2 and lx.is_number():
        nums.append(int(lx.number()))
    return nums


def parse_model(lx, revision, st):
    lx.read()                                   # name (a string)
    if lx.is_number():
        lx.number()                             # Quake 4: a number after the name
    lx.expect("{")
    while not lx.check("}"):
        section = lx.read()
        if section == "vertices":
            lx.expect("{")
            n = int(lx.number())
            for _ in range(n):
                lx.matrix(3)
            lx.expect("}")
            st["vertices"] += n
        elif section == "edges":
            lx.expect("{")
            n = int(lx.number())
            for _ in range(n):
                lx.matrix(2)
                lx.number()
                lx.number()
            lx.expect("}")
            st["edges"] += n
        elif section == "nodes":
            lx.expect("{")
            parse_nodes(lx, st)
            lx.expect("}")
        elif section == "polygons":
            header = block_header(lx)
            lx.expect("{")
            polys = edges = 0
            while not lx.check("}"):
                n = int(lx.number())
                lx.expect("(")
                for _ in range(n):
                    lx.number()
                lx.expect(")")
                lx.matrix(3)
                lx.number()
                lx.matrix(3)
                lx.matrix(3)
                mat = lx.read()
                if not mat.startswith('"'):
                    raise CmError(f"polygon material is not a string: {mat}")
                if revision >= 2:               # Quake 4 trailer
                    for _ in range(3):
                        lx.matrix(2)
                    st["polygon uv trailers"] += 1
                    if revision >= 3:
                        lx.number()
                polys += 1
                edges += n
            if len(header) == 2 and header != [polys, edges]:
                raise CmError(f"polygon header {header} vs rows {[polys, edges]}")
            st["polygons"] += polys
        elif section == "brushes":
            header = block_header(lx)
            lx.expect("{")
            brushes = planes = 0
            while not lx.check("}"):
                n = int(lx.number())
                lx.expect("{")
                for _ in range(n):
                    lx.matrix(3)
                    lx.number()
                lx.expect("}")
                lx.matrix(3)
                lx.matrix(3)
                contents = lx.read()
                if contents.startswith('"'):
                    for name in contents.strip('"').split(","):
                        if name and name.lower() not in CONTENTS:
                            st[f"unknown contents '{name}'"] += 1
                if revision >= 3:
                    lx.number()                 # Quake 4: primitive number
                    st["brush primitive numbers"] += 1
                brushes += 1
                planes += n
            if len(header) == 2 and header != [brushes, planes]:
                raise CmError(f"brush header {header} vs rows {[brushes, planes]}")
            st["brushes"] += brushes
        else:
            raise CmError(f"bad token '{section}'")
    st["models"] += 1


def check(text, st):
    lx = Lexer(text)
    lx.expect("CM")
    version = lx.read()
    revisions = {'"1.00"': 0, '"2"': 2, '"3"': 3}
    if version not in revisions:
        raise CmError(f"version {version}")
    st[f"version {version}"] += 1
    lx.number()                                 # map CRC
    while lx.peek() is not None:
        lx.expect("collisionModel")
        parse_model(lx, revisions[version], st)


def main():
    files = {}
    for pat in sys.argv[1].split(","):
        for p in sorted(glob.glob(pat)):
            try:
                z = zipfile.ZipFile(p)
            except zipfile.BadZipFile:
                continue
            for n in z.namelist():
                if n.lower().endswith(".cm"):
                    files[n.lower()] = (z, n)
    st = collections.Counter()
    failures = 0
    for key in sorted(files):
        z, n = files[key]
        try:
            check(z.read(n).decode("latin-1"), st)
        except CmError as e:
            failures += 1
            print(f"FAIL {n}: {e}", flush=True)
    print(f"{len(files)} .cm files, {failures} failed: {dict(st)}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
