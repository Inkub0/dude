#!/usr/bin/env python3
"""What the three "( s t )" pairs of a Quake 4 .cm polygon are, measured against the .map.

A revision-3 polygon row ends with "( s t ) ( s t ) ( s t ) <primitive>". For world polygons
cut from a brush, the primitive is the brush's index in worldspawn, and the brush side in the
polygon's plane gives the exact texture coordinates at any point (idMapBrushSide::
GetTextureVectors). For each such polygon this inverts the three pairs to points on the plane
and reports where they lie: on polygon vertices (v), inside the polygon's bounds (b) or outside
(o) - plus, for the vertex case, which vertices in the order the polygon's edges give. Then it
counts the polygons whose material has a hit map (materialImage), the only ones a per-texel
material type would need the pairs for.

Usage: q4_cm_texcoords.py <q4base> [map ...]     (default: airdefense1 mcc_1)
"""
import collections
import glob
import math
import os
import re
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from q4_cm_check import Lexer  # noqa: E402

NUM = r'[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?'


def axis_base(n):
    # idlib/MapFile.cpp ComputeAxisBase
    n = [0.0 if abs(c) < 1e-6 else c for c in n]
    ry = -math.atan2(n[2], math.sqrt(n[1] * n[1] + n[0] * n[0]))
    rz = math.atan2(n[1], n[0])
    return ((-math.sin(rz), math.cos(rz), 0.0),
            (-math.sin(ry) * math.cos(rz), -math.sin(ry) * math.sin(rz), -math.cos(ry)))


def texture_vectors(plane, texmat):
    # idMapBrushSide::GetTextureVectors (worldspawn: origin 0)
    tx, ty = axis_base(plane[:3])
    return [[tx[k] * texmat[i][0] + ty[k] * texmat[i][1] for k in range(3)] + [texmat[i][2]] for i in range(2)]


def worldspawn_primitives(text):
    start = text.find('{')
    depth, j = 0, start
    while True:
        c = text[j]
        if c == '"':
            j = text.find('"', j + 1)
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                break
        j += 1
    ent = text[start:j]
    side = re.compile(r'\(\s*(' + NUM + r')\s+(' + NUM + r')\s+(' + NUM + r')\s+(' + NUM + r')\s*\)\s*\(\s*\(\s*(' + NUM + r')\s+('
                      + NUM + r')\s+(' + NUM + r')\s*\)\s*\(\s*(' + NUM + r')\s+(' + NUM + r')\s+(' + NUM + r')\s*\)\s*\)\s*"([^"]+)"')
    prims = []
    for m in re.finditer(r'(brushDef3|patchDef2|patchDef3)\s*\{', ent):
        if m.group(1) != 'brushDef3':
            prims.append(None)
            continue
        end = ent.find('\n }', m.end())
        sides = []
        for s in side.finditer(ent[m.end():end]):
            v = list(map(float, s.groups()[:10]))
            sides.append((v[:4], [v[4:7], v[7:10]], s.group(11).lower()))
        prims.append(sides)
    return prims


def world_polygons(text):
    lx = Lexer(text)
    lx.expect('CM')
    lx.read()
    lx.number()
    lx.expect('collisionModel')
    lx.read()
    if lx.is_number():
        lx.number()
    lx.expect('{')
    verts, edges, polys = [], [], []
    while not lx.check('}'):
        section = lx.read()
        if section == 'vertices':
            lx.expect('{')
            verts = [lx.matrix(3) for _ in range(int(lx.number()))]
            lx.expect('}')
        elif section == 'edges':
            lx.expect('{')
            for _ in range(int(lx.number())):
                edges.append([int(x) for x in lx.matrix(2)])
                lx.number()
                lx.number()
            lx.expect('}')
        elif section == 'nodes':
            lx.expect('{')
            pending = 1
            while pending:
                lx.expect('(')
                plane_type = int(lx.number())
                lx.number()
                lx.expect(')')
                pending += 1 if plane_type != -1 else -1
            lx.expect('}')
        elif section == 'polygons':
            while lx.is_number():
                lx.number()
            lx.expect('{')
            while not lx.check('}'):
                n = int(lx.number())
                lx.expect('(')
                es = [int(lx.number()) for _ in range(n)]
                lx.expect(')')
                normal = lx.matrix(3)
                dist = lx.number()
                b0, b1 = lx.matrix(3), lx.matrix(3)
                material = lx.read().strip('"').lower()
                st = [lx.matrix(2) for _ in range(3)]
                primitive = int(lx.number())
                # the polygon's points as CM_GetPolygonVertices-style code walks them
                vs = [verts[edges[abs(e)][1 if e < 0 else 0]] for e in es]
                polys.append((vs, normal, dist, b0, b1, material, st, primitive))
        else:
            break
    return polys


def solve3(m, r):
    # 3x3 Cramer's rule (no numpy needed)
    def det(a):
        return (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
                + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    d = det(m)
    if abs(d) < 1e-9:
        return None
    out = []
    for c in range(3):
        mc = [row[:] for row in m]
        for r_ in range(3):
            mc[r_][c] = r[r_]
        out.append(det(mc) / d)
    return out


def hit_map_materials(files):
    hit = set()
    for key, (z, n) in files.items():
        if not key.endswith('.mtr'):
            continue
        text = re.sub(r'//[^\n]*', '', z.read(n).decode('latin-1'))
        for m in re.finditer(r'(?m)^\s*([\w/\-\.]+)\s*\n?\s*\{', text):
            s = text.find('{', m.start())
            depth, j = 0, s
            while j < len(text):
                if text[j] == '{':
                    depth += 1
                elif text[j] == '}':
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            if re.search(r'^\s*materialimage\s', text[s:j].lower(), re.M):
                hit.add(m.group(1).lower())
    return hit


def main():
    base = sys.argv[1]
    maps = sys.argv[2:] or ['airdefense1', 'mcc_1']
    files = {}
    for p in sorted(glob.glob(os.path.join(base, 'pak0*.pk4'))):
        z = zipfile.ZipFile(p)
        for n in z.namelist():
            files[n.lower()] = (z, n)
    read = lambda k: files[k][0].read(files[k][1]).decode('latin-1')
    hit = hit_map_materials(files)
    for name in maps:
        prims = worldspawn_primitives(read(f'maps/game/{name}.map'))
        polys = world_polygons(read(f'maps/game/{name}.cm'))
        where = collections.Counter()
        order = collections.Counter()
        tested = hits = 0
        for vs, normal, dist, b0, b1, material, st, primitive in polys:
            if material in hit:
                hits += 1
            if primitive >= len(prims) or prims[primitive] is None:
                continue
            side = next(((pl, tm) for pl, tm, m in prims[primitive]
                         if m == material and sum((pl[k] - normal[k]) ** 2 for k in range(3)) < 1e-4 and abs(pl[3] + dist) < 0.05), None)
            if side is None:
                continue
            tv = texture_vectors(*side)
            m = [tv[0][:3], tv[1][:3], list(normal)]
            pts = [solve3(m, [q[0] - tv[0][3], q[1] - tv[1][3], dist]) for q in st]
            if None in pts:
                continue
            tested += 1

            def vertex_of(p):
                for i, v in enumerate(vs):
                    if sum((v[k] - p[k]) ** 2 for k in range(3)) < 0.05 ** 2:
                        return i
                return None
            idx = [vertex_of(p) for p in pts]
            tag = ''.join('v' if i is not None else ('b' if all(b0[k] - 0.05 <= p[k] <= b1[k] + 0.05 for k in range(3)) else 'o')
                          for i, p in zip(idx, pts))
            where[tag] += 1
            if tag == 'vvv':
                order[(len(vs),) + tuple(idx)] += 1
        on_vertices = where['vvv']
        print(f'{name}: {len(polys)} world polygons, {tested} from a worldspawn brush side; '
              f'all three points on vertices {on_vertices} ({100.0 * on_vertices / max(tested, 1):.0f}%), '
              f'{hits} use a hit-map material')
        print('  where the points lie:', dict(where.most_common()))
        print('  vertex order (numVerts, i0, i1, i2), most common:', order.most_common(6))


if __name__ == '__main__':
    main()
