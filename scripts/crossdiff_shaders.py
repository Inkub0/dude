#!/usr/bin/env python3
"""Semantic cross-check: hand-translated GLSL (neo/shaders) vs transpiled ARB
(arbtool glsl output) — two independent derivations of the same ARB source.

Both shader bodies are transformed into Python vec-ops and executed with
identical seeded inputs; outputs must agree numerically. Uniform identity
comes from a per-pair map (RenderParams name -> ARB env/local slot), varyings
match by VARY(n) location, samplers by SAMPLER_BINDING unit; textures are
deterministic stubs. Usage: python3 scripts/crossdiff_shaders.py
"""
import math, random, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HAND = ROOT / "neo" / "shaders"
TRANS = ROOT / "testdata" / "transpiled"

# ---------------------------------------------------------------- vec runtime
class Vec:
    __slots__ = ("v",)
    def __init__(self, *a):
        out = []
        for x in a:
            if isinstance(x, Vec): out += x.v
            elif isinstance(x, bool): out.append(1.0 if x else 0.0)
            else: out.append(float(x))
        if len(out) == 1 and len(a) == 1: out = out * 4  # splat handled by ctor helpers
        self.v = out
    def _bin(self, o, f):
        if isinstance(o, (int, float)): o = Vec(*([float(o)] * len(self.v)))
        if len(o.v) == 1: o = Vec(*(o.v * len(self.v)))
        if len(self.v) == 1: return Vec(*[f(self.v[0], y) for y in o.v])
        assert len(self.v) == len(o.v), (self.v, o.v)
        return Vec(*[f(x, y) for x, y in zip(self.v, o.v)])
    def __add__(self, o): return self._bin(o, lambda a, b: a + b)
    def __radd__(self, o): return self._bin(o, lambda a, b: b + a)
    def __sub__(self, o): return self._bin(o, lambda a, b: a - b)
    def __rsub__(self, o): return self._bin(o, lambda a, b: b - a)
    def __mul__(self, o):
        if isinstance(o, Mat4): raise TypeError
        return self._bin(o, lambda a, b: a * b)
    def __rmul__(self, o): return self.__mul__(o)
    def __truediv__(self, o): return self._bin(o, lambda a, b: a / b)
    def __rtruediv__(self, o): return self._bin(o, lambda a, b: b / a)
    def __neg__(self): return Vec(*[-x for x in self.v])
    def __getattr__(self, sw):
        idx = ["xyzw".index(c) if c in "xyzw" else "rgba".index(c) for c in sw]
        if len(idx) == 1: return self.v[idx[0]]
        return Vec(*[self.v[i] for i in idx])
    def __getitem__(self, i): return self.v[i]
    def __float__(self):
        assert len(self.v) == 1
        return self.v[0]

def _c(n):
    def ctor(*a):
        out = []
        for x in a:
            if isinstance(x, Vec): out += x.v
            else: out.append(float(x))
        if len(out) == 1: out = out * n
        assert len(out) == n, (a, n)
        return Vec(*out)
    return ctor
vec2, vec3, vec4 = _c(2), _c(3), _c(4)

class Mat4:  # column-major, m[c][r]
    def __init__(self, cols): self.cols = cols
    def __getitem__(self, c): return self.cols[c]
    def __mul__(self, v):
        assert isinstance(v, Vec) and len(v.v) == 4
        out = [sum(self.cols[c][r] * v.v[c] for c in range(4)) for r in range(4)]
        return Vec(*out)

def _s(x): return x.v[0] if isinstance(x, Vec) and len(x.v) == 1 else float(x)
def dot(a, b): return sum(x * y for x, y in zip(a.v, b.v))
def normalize(a):
    l = math.sqrt(dot(a, a)); return Vec(*[x / l for x in a.v])
def cross(a, b):
    return Vec(a.v[1]*b.v[2]-a.v[2]*b.v[1], a.v[2]*b.v[0]-a.v[0]*b.v[2], a.v[0]*b.v[1]-a.v[1]*b.v[0])
def _map2(a, b, f):
    if isinstance(a, Vec):
        if not isinstance(b, Vec): b = Vec(*([float(b)] * len(a.v)))
        return Vec(*[f(x, y) for x, y in zip(a.v, b.v)])
    if isinstance(b, Vec): return _map2(Vec(*([float(a)]*len(b.v))), b, f)
    return f(float(a), float(b))
def gmin(a, b): return _map2(a, b, min)
def gmax(a, b): return _map2(a, b, max)
def clamp(x, lo, hi):
    if isinstance(x, Vec):
        lo = lo.v if isinstance(lo, Vec) else [float(lo)]*len(x.v)
        hi = hi.v if isinstance(hi, Vec) else [float(hi)]*len(x.v)
        return Vec(*[max(l, min(h, y)) for y, l, h in zip(x.v, lo, hi)])
    return max(float(lo), min(float(hi), float(x)))
def mix(a, b, t):
    if not isinstance(t, Vec):
        t = Vec(*([float(t)] * (len(a.v) if isinstance(a, Vec) else 1)))
    return a + (b - a) * t if isinstance(a, Vec) else float(a) + (float(b)-float(a))*t.v[0]
def gfloor(x): return Vec(*[math.floor(y) for y in x.v]) if isinstance(x, Vec) else math.floor(x)
def fract(x): return Vec(*[y - math.floor(y) for y in x.v]) if isinstance(x, Vec) else x - math.floor(x)
def gabs(x): return Vec(*[abs(y) for y in x.v]) if isinstance(x, Vec) else abs(x)
def gpow(a, b): return math.pow(_s(a), _s(b))
def inversesqrt(x): return 1.0 / math.sqrt(_s(x))
def exp2(x): return math.pow(2.0, _s(x))
def log2(x): return math.log2(_s(x))
def lessThan(a, b): return [x < y for x, y in zip(a.v, b.v)]
def greaterThanEqual(a, b): return [x >= y for x, y in zip(a.v, b.v)]
def gany(bl): return any(bl)

class Discard(Exception): pass
def _discard(): raise Discard()

def texfn(unit, coords, comps):
    # deterministic smooth stub in (0,1); identical for both derivations
    out = []
    for i in range(4):
        h = 0.0
        for j, c in enumerate(coords[:comps]):
            h += math.sin(c * (1.3 + 0.7 * j) + unit * 2.1 + i * 0.9)
        out.append(0.5 + 0.5 * math.sin(h + unit + i))
    return Vec(*out)

# ------------------------------------------------------------- glsl -> python
def load_shader(path):
    src = path.read_text()
    src = re.sub(r"//[^\n]*", "", src)
    samplers, varyings = {}, {}
    for m in re.finditer(r"SAMPLER_BINDING\((\d+)\)\s+uniform\s+(\w+)\s+(\w+)\s*;", src):
        comps = 3 if ("Cube" in m.group(2) or "3D" in m.group(2)) else 2
        samplers[m.group(3)] = (int(m.group(1)), comps)
    for m in re.finditer(r"VARY\((\d+)\)\s+(?:in|out)\s+(vec[234])\s+(\w+)\s*;", src):
        varyings[m.group(3)] = (int(m.group(1)), int(m.group(2)[-1]))
    body = src[src.index("void main()"):]
    body = body[body.index("{") + 1:body.rindex("}")]
    # join lines, transform if-discards, strip block braces
    body = " ".join(body.split())
    body = re.sub(r"if\s*\((.*?)\)\s*\{\s*discard;\s*\}", lambda m: f"@IF@{m.group(1)}@THEN@", body)
    body = body.replace("{", " ").replace("}", " ")
    stmts = []
    def clean(s):
        s = re.sub(r"^(const\s+)?(vec[234]|float)\s+(?=\w)", "", s.strip())
        return s.replace("||", " or ").replace("&&", " and ")
    for raw in body.split(";"):
        s = raw.strip()
        if not s: continue
        m = re.match(r"@IF@(.*)@THEN@\s*(.*)$", s)
        if m:
            stmts.append(("if", clean(m.group(1))))
            if m.group(2).strip(): stmts.append(("stmt", clean(m.group(2))))
            continue
        stmts.append(("stmt", clean(s)))
    return samplers, varyings, stmts

def run_shader(samplers, stmts, ns):
    class TexProxy:
        def __init__(self, unit, comps): self.unit, self.comps = unit, comps
    for name, (unit, comps) in samplers.items():
        ns[name] = TexProxy(unit, comps)
    def texture(sm, coords, bias=0.0):
        c = coords.v if isinstance(coords, Vec) else [float(coords)]
        return texfn(sm.unit, c, sm.comps)
    def textureProj(sm, coords):
        c = [x / coords.v[3] for x in coords.v[:2]]
        return texfn(sm.unit, c, 2)
    g = dict(ns, vec2=vec2, vec3=vec3, vec4=vec4, dot=dot, normalize=normalize,
             cross=cross, min=gmin, max=gmax, clamp=clamp, mix=mix, floor=gfloor,
             fract=fract, abs=gabs, pow=gpow, inversesqrt=inversesqrt, exp2=exp2,
             log2=log2, sin=lambda x: math.sin(_s(x)), cos=lambda x: math.cos(_s(x)),
             lessThan=lessThan, greaterThanEqual=greaterThanEqual, any=gany,
             texture=texture, textureProj=textureProj, _discard=_discard)
    # execute; swizzle assignment via regex rewrite: a.xy = expr -> _swz(...)
    def _swz_assign(name, sw, val):
        sw = "".join("xyzw"["rgba".index(c)] if c in "rgba" else c for c in sw)
        cur = list(g[name].v)
        idx = ["xyzw".index(c) for c in sw]
        vv = val.v if isinstance(val, Vec) else [float(val)] * len(idx)
        if isinstance(val, Vec) and len(vv) == 4 and len(idx) < 4:
            vv = [val.v["xyzw".index(c)] for c in sw]   # res_.xy pattern: take same comps
        for k, i in enumerate(idx): cur[i] = vv[k] if k < len(vv) else vv[-1]
        g[name] = Vec(*cur)
    g["_swz_assign"] = _swz_assign
    discarded = False
    skip = False
    for kind, s in stmts:
        if kind == "if":
            try:
                if eval(s, g): raise Discard()
            except Discard:
                discarded = True
                break
            continue
        # expand compound swizzle assigns: a.xy *= e  ->  a.xy = (a.xy) * (e)
        mc = re.match(r"^(\w+)\.([xyzwrgba]{1,4})\s*([*+/-])=\s*(.*)$", s)
        if mc:
            s = f"{mc.group(1)}.{mc.group(2)} = ({mc.group(1)}.{mc.group(2)}) {mc.group(3)} ({mc.group(4)})"
        m = re.match(r"^(\w+)\.([xyzwrgba]{1,4})\s*=\s*(.*)$", s)
        try:
            if m:
                val = eval(m.group(3), g)
                _swz_assign(m.group(1), m.group(2), val if isinstance(val, Vec) else Vec(*([float(val)]*len(m.group(2)))))
            else:
                try:
                    exec(s, g)
                except Discard:
                    raise
                except Exception as e:
                    raise RuntimeError(f"statement failed: {s!r}: {e}")
        except Discard:
            discarded = True
            break
    return g, discarded

# ------------------------------------------------------------------ pairs
# uniform map: RenderParams name -> ("env", n) | ("local", n) | ("const", (..))
COMMON = {
    "u_localLightOrigin": ("env", 4), "u_localViewOrigin": ("env", 5),
    "u_lightProjectionS": ("env", 6), "u_lightProjectionT": ("env", 7),
    "u_lightProjectionQ": ("env", 8), "u_lightFalloffS": ("env", 9),
    "u_bumpMatrixS": ("env", 10), "u_bumpMatrixT": ("env", 11),
    "u_diffuseMatrixS": ("env", 12), "u_diffuseMatrixT": ("env", 13),
    "u_specularMatrixS": ("env", 14), "u_specularMatrixT": ("env", 15),
    "u_vertexColorModulate": ("env", 16), "u_vertexColorAdd": ("env", 17),
    "u_diffuseModifier": ("env", 0), "u_specularModifier": ("env", 1),
    "u_screenCorrection": ("env", 0), "u_windowCoord": ("env", 1),
    "u_localParam0": ("local", 0), "u_localParam1": ("local", 1),
}
PAIRS = [
    # (hand stem, corpus file, extra uniform map, fp compare comps, check vp,
    #  hand-varying-loc -> arb-texcoord-loc map for pairs where the hand
    #  translation renumbered varyings sequentially)
    ("interaction", "base/interaction.vfp", {}, 4, True, {7: 8}),
    ("ambientlight", "base/ambientLight.vfp",
        {"u_modelMatrixRow0": ("env", 20), "u_modelMatrixRow1": ("env", 21), "u_modelMatrixRow2": ("env", 22)}, 3, True, {7: 8}),
    ("environment", "base/environment.vfp", {}, 4, True, {2: 8}),
    ("bumpyenvironment", "base/bumpyEnvironment.vfp",
        {"u_modelMatrixRow0": ("env", 6), "u_modelMatrixRow1": ("env", 7), "u_modelMatrixRow2": ("env", 8)}, 3, True, {5: 8}),
    ("shadow", "base/shadow.vp", {}, 0, True, {}),
    ("heathaze", "base/heatHaze.vfp", {}, 3, True, {0: 1, 1: 2}),
    ("heathaze_mask", "base/heatHazeWithMask.vfp", {}, 3, True, {}),
    ("heathaze_maskvertex", "base/heatHazeWithMaskAndVertex.vfp", {}, 3, True, {3: 8}),
    ("colorprocess", "base/colorProcess.vfp", {}, 3, True, {}),
    ("portalsky", "d3xp/portalSky.vfp", {}, 3, True, {0: 1}),
    ("bloodorb", "d3xp/bloodOrb1.vfp", {"u_localParam1": ("const", (1, 1, 1, 1))}, 4, False, {}),
]

def seeded_ns(seed, umap):
    rnd = random.Random(seed)
    def rv(lo=-1.0, hi=1.0): return Vec(*[rnd.uniform(lo, hi) for _ in range(4)])
    env = [rv() for _ in range(32)]
    env[4] = Vec(env[4].x, env[4].y, env[4].z, 0.0)  # light origin: ARB assumes w=0 (shadow.vp)
    loc = [rv() for _ in range(8)]
    def rmat(): return Mat4([Vec(*[rnd.uniform(-1, 1) for _ in range(4)]) for _ in range(4)])
    # the harness feeds both stages identical env/local values, so the split
    # per-target arrays (u_venv/u_fenv, see arbparams.glsl) alias one set here
    ns = {"u_env": env, "u_local": loc,
          "u_venv": env, "u_fenv": env, "u_vlocal": loc, "u_flocal": loc,
          "u_mvpMatrix": rmat(), "u_modelViewMatrix": rmat(),
          "u_projectionMatrix": rmat(), "u_textureMatrix": rmat(),
          "attr_Position": Vec(rnd.uniform(-50, 50), rnd.uniform(-50, 50), rnd.uniform(-50, 50), 1.0),
          "attr_TexCoord": Vec(rnd.uniform(0, 1), rnd.uniform(0, 1)),
          "attr_Normal": normalize(Vec(rnd.uniform(-1, 1), rnd.uniform(-1, 1), rnd.uniform(-1, 1))),
          "attr_Tangent": normalize(Vec(rnd.uniform(-1, 1), rnd.uniform(-1, 1), rnd.uniform(-1, 1))),
          "attr_Bitangent": normalize(Vec(rnd.uniform(-1, 1), rnd.uniform(-1, 1), rnd.uniform(-1, 1))),
          "attr_Color": Vec(rnd.uniform(0, 1), rnd.uniform(0, 1), rnd.uniform(0, 1), rnd.uniform(0, 1)),
          "gl_FragCoord": Vec(rnd.uniform(1, 2000), rnd.uniform(1, 1400), rnd.uniform(0.1, 0.999), rnd.uniform(0.5, 2.0)),
          "gl_Position": Vec(0, 0, 0, 0), "fragColor": Vec(0, 0, 0, 0),
          "var_fog": Vec(0, 0, 0, 1)}
    for name, spec in umap.items():
        if spec[0] == "env": ns[name] = env[spec[1]]
        elif spec[0] == "local": ns[name] = loc[spec[1]]
        else: ns[name] = Vec(*spec[1])
    # shadow.vp position has meaningful w (0 or 1)
    if rnd.random() < 0.5:
        ns["attr_Position"] = Vec(ns["attr_Position"].x, ns["attr_Position"].y, ns["attr_Position"].z, 0.0)
    return ns

def close(a, b, tol=2e-3):
    return abs(a - b) <= tol * max(1.0, abs(a), abs(b))

def main():
    total = fails = 0
    for stem, corpus, extra, fpcomps, checkvp, vmap in PAIRS:
        umap = dict(COMMON, **extra)
        cstem = Path(corpus).stem
        cdir = Path(corpus).parent.name
        stages = []
        if checkvp: stages.append("vert")
        if fpcomps: stages.append("frag")
        for ext in stages:
            hpath = HAND / f"{stem}.{ext}"
            tpath = TRANS / cdir / f"{cstem}.{ext}"
            if not hpath.exists() or not tpath.exists():
                print(f"SKIP {stem}.{ext}: missing file")
                continue
            hs, hv, hst = load_shader(hpath)
            ts, tv, tst = load_shader(tpath)
            for seed in range(6):
                total += 1
                ns = seeded_ns(seed, umap)
                # fragment: synthesize varyings by location for both sides
                rnd = random.Random(1000 + seed)
                # w=1: the ARB varying contract (texcoord default); keeps
                # TXP-with-w==1 and plain TEX comparable, as in real use
                locvals = {n: Vec(rnd.uniform(-2, 2), rnd.uniform(-2, 2), rnd.uniform(-2, 2), 1.0) for n in range(10)}
                def bind_vary(vd, ns_, locmap):
                    for name, (loc_, sz) in vd.items():
                        ns_[name] = Vec(*locvals[locmap.get(loc_, loc_)].v[:sz])
                nh, nt = dict(ns), dict(ns)
                if ext == "frag":
                    bind_vary(hv, nh, vmap); bind_vary(tv, nt, {})
                gh, dh = run_shader(hs, hst, nh)
                gt, dt = run_shader(ts, tst, nt)
                bad = []
                if dh != dt:
                    bad.append(f"discard {dh} vs {dt}")
                elif not dh:
                    if ext == "frag":
                        a, b = gh["fragColor"], gt["fragColor"]
                        for i in range(fpcomps):
                            if not close(a.v[i], b.v[i]): bad.append(f"fragColor[{i}] {a.v[i]:.5f} vs {b.v[i]:.5f}")
                    else:
                        a, b = gh["gl_Position"], gt["gl_Position"]
                        for i in range(4):
                            if not close(a.v[i], b.v[i]): bad.append(f"gl_Position[{i}] {a.v[i]:.5f} vs {b.v[i]:.5f}")
                        for name, (loc_, sz) in hv.items():
                            arbloc = vmap.get(loc_, loc_)
                            tname = [n for n, (l2, _) in tv.items() if l2 == arbloc]
                            if not tname: continue
                            va, vb = gh[name], gt[tname[0]]
                            for i in range(min(sz, len(vb.v))):
                                if not close(va.v[i], vb.v[i]):
                                    bad.append(f"{name}[{i}] {va.v[i]:.5f} vs {vb.v[i]:.5f}")
                if bad:
                    fails += 1
                    print(f"MISMATCH {stem}.{ext} seed {seed}: " + "; ".join(bad[:4]))
    print(f"\n{total} comparisons, {fails} mismatches")
    sys.exit(1 if fails else 0)

main()
