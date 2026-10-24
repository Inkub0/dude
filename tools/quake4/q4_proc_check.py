#!/usr/bin/env python3
"""Replica of DUDE's .proc loader (neo/renderer/RenderWorld_load.cpp) run over retail files.

Walks every .proc token by token in the exact order idRenderWorldLocal::InitFromMap,
ParseModel, ParseShadowModel, ParseInterAreaPortals and ParseNodes read them - every
Expect, every count - so a grammar difference shows up here before the engine meets it.
Accepts Doom 3's "mapProcFile003" and Quake 4's PROC "4" <crc>; for version 4 it allows
what the engine allows: an area model's sky number after numSurfaces, 12-number coloured
vertices, a portal's ( fadeImage near far ). Reports per-title counts and the first
mismatch per file.

Usage: q4_proc_check.py <pk4 glob>[,<pk4 glob>...]
   e.g. q4_proc_check.py "/path/Quake 4/q4base/pak*.pk4"
        q4_proc_check.py "base/pak*.pk4,d3xp/pak*.pk4"
"""
import collections
import glob
import re
import sys
import zipfile

TOKEN = re.compile(r'/\*.*?\*/|//[^\n]*|"[^"]*"|[(){}]|[^\s(){}"]+', re.S)


class ProcError(Exception):
    pass


class Lexer:
    def __init__(self, text):
        self.toks = [t for t in TOKEN.findall(text) if not t.startswith(("/*", "//"))]
        self.i = 0

    def peek(self):
        return self.toks[self.i] if self.i < len(self.toks) else None

    def read(self):
        t = self.peek()
        if t is None:
            raise ProcError("unexpected end of file")
        self.i += 1
        return t

    def expect(self, s):
        t = self.read()
        if t != s:
            raise ProcError(f"expected '{s}' but found '{t}' (token {self.i})")

    def check(self, s):
        if self.peek() == s:
            self.i += 1
            return True
        return False

    def number(self):
        t = self.read()
        if not re.fullmatch(r"-?\d+(\.\d*)?([eE][-+]?\d+)?|-?\.\d+", t):
            raise ProcError(f"expected a number but found '{t}' (token {self.i})")
        return float(t)

    def int(self):
        return int(self.number())

    def matrix(self, n):
        self.expect("(")
        v = [self.number() for _ in range(n)]
        self.expect(")")
        return v


def parse_model(lx, version, st):
    lx.expect("{")
    name = lx.read().strip('"')
    nsurf = lx.int()
    if version >= 4 and re.fullmatch(r"-?\d+", lx.peek() or ""):
        st["area sky"] += 1
        lx.int()
    for _ in range(nsurf):
        lx.expect("{")
        lx.read()                                   # material
        nv, ni = lx.int(), lx.int()
        for _ in range(nv):
            lx.expect("(")
            for _ in range(8):
                lx.number()
            if not lx.check(")"):
                for _ in range(4):
                    c = lx.number()
                    if not (0 <= c <= 255):
                        raise ProcError(f"vertex colour {c} outside 0-255")
                lx.expect(")")
                st["coloured verts"] += 1
            st["verts"] += 1
        for _ in range(ni):
            if lx.int() >= nv:
                raise ProcError("index out of range")
        lx.expect("}")
    lx.expect("}")
    st["models"] += 1


def parse_shadow(lx, st):
    lx.expect("{")
    lx.read()
    nv = lx.int()
    lx.int()
    lx.int()
    ni = lx.int()
    lx.int()
    for _ in range(nv):
        lx.matrix(3)
    for _ in range(ni):
        lx.int()
    lx.expect("}")
    st["shadow models"] += 1


def parse_portals(lx, st):
    lx.expect("{")
    nareas, nportals = lx.int(), lx.int()
    for _ in range(nportals):
        npts, a1, a2 = lx.int(), lx.int(), lx.int()
        if not (0 <= a1 < nareas and 0 <= a2 < nareas):
            raise ProcError("portal area out of range")
        for _ in range(npts):
            lx.matrix(3)
        if lx.check("("):                           # Quake 4 fade
            lx.read()
            lx.number()
            lx.number()
            lx.expect(")")
            st["fading portals"] += 1
        st["portals"] += 1
    lx.expect("}")


def parse_nodes(lx, st):
    lx.expect("{")
    n = lx.int()
    for _ in range(n):
        lx.matrix(4)
        lx.int()
        lx.int()
    lx.expect("}")
    st["nodes"] += n


def check(text, st):
    lx = Lexer(text)
    head = lx.read()
    if head == "PROC":
        if lx.read() != '"4"':
            raise ProcError("unsupported PROC version")
        lx.int()
        version = 4
    elif head == "mapProcFile003":
        version = 3
    else:
        raise ProcError(f"bad id '{head}'")
    st[f"version {version}"] += 1
    while lx.peek() is not None:
        t = lx.read()
        if t == "model":
            parse_model(lx, version, st)
        elif t == "shadowModel":
            parse_shadow(lx, st)
        elif t == "interAreaPortals":
            parse_portals(lx, st)
        elif t == "nodes":
            parse_nodes(lx, st)
        else:
            raise ProcError(f"bad token '{t}'")


def main():
    files = {}
    for pat in sys.argv[1].split(","):
        for p in sorted(glob.glob(pat)):
            try:
                z = zipfile.ZipFile(p)
            except zipfile.BadZipFile:
                continue
            for n in z.namelist():
                if n.lower().endswith(".proc"):
                    files[n.lower()] = (z, n)          # later paks override earlier ones
    st = collections.Counter()
    failures = 0
    for key in sorted(files):
        z, n = files[key]
        try:
            check(z.read(n).decode("latin-1"), st)
        except ProcError as e:
            failures += 1
            print(f"FAIL {n}: {e}")
    print(f"{len(files)} .proc files, {failures} failed: {dict(st)}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
