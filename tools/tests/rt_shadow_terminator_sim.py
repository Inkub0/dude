#!/usr/bin/env python3
# Offline check behind r_rtShadowNearStart (neo/shaders/rtshadow_ray.frag, docs/rtx-shadow-blur.md "Near ray start").
# Needs numpy. Run: python3 tools/tests/rt_shadow_terminator_sim.py
#
# A smooth-shaded low-poly CONVEX solid (an icosphere) drawn as its flat triangles - what is drawn is what the rays
# hit - lit from far away, with the softening ray's rules: start pushed along the flat normal, hits on the inside of
# a mesh ignored within 3 units (r_rtShadowInsideSkip).
#   1. Alone: every point the shading calls lit (smooth N.L > 0) that the ray shadows is an artifact - the facets of
#      the "shadow terminator", where a triangle already faces away from the light but its shading normal does not.
#   2. Behind a wall that covers it: every lit point the ray does NOT shadow is a light leak.
# Four ways to start the ray:
#   legacy 1.0         r_rtSunShadowOffset. No facets, no leak - and blind to occluders closer than 1 unit.
#   near 0.05          facets: 26 to 77% of the facing-away band goes dark, as whole triangles.
#   near + own-plane   ignore hits past the triangle's own plane: no facets, but that band leaks through the wall.
#   near where facing  0.05 where the triangle faces the light, back to 1.0 as it turns edge-on (cosine 0.15 to 0):
#                      no facets, no leak. This is what the shader does.
import numpy as np
rng = np.random.default_rng(7)

def icosphere(sub, R):
    t = (1 + 5 ** 0.5) / 2
    v = np.array([[-1,t,0],[1,t,0],[-1,-t,0],[1,-t,0],[0,-1,t],[0,1,t],[0,-1,-t],[0,1,-t],[t,0,-1],[t,0,1],[-t,0,-1],[-t,0,1]], float)
    f = [[0,11,5],[0,5,1],[0,1,7],[0,7,10],[0,10,11],[1,5,9],[5,11,4],[11,10,2],[10,7,6],[7,1,8],
         [3,9,4],[3,4,2],[3,2,6],[3,6,8],[3,8,9],[4,9,5],[2,4,11],[6,2,10],[8,6,7],[9,8,1]]
    v /= np.linalg.norm(v, axis=1)[:, None]
    for _ in range(sub):
        nf = []; vl = list(map(tuple, v)); cache = {}
        def mid(a, b):
            k = (min(a, b), max(a, b))
            if k not in cache:
                m = (np.array(vl[a]) + np.array(vl[b])) / 2; m /= np.linalg.norm(m)
                vl.append(tuple(m)); cache[k] = len(vl) - 1
            return cache[k]
        for a, b, c in f:
            ab, bc, ca = mid(a, b), mid(b, c), mid(c, a)
            nf += [[a, ab, ca], [b, bc, ab], [c, ca, bc], [ab, bc, ca]]
        f = nf; v = np.array(vl)
    return v * R, np.array(f)

def trace(O, D, V, F, FN, skip):
    # closest COUNTED hit per ray: outside hits always, inside hits only beyond `skip` (r_rtShadowInsideSkip)
    a = V[F[:, 0]]; e1 = V[F[:, 1]] - a; e2 = V[F[:, 2]] - a
    best = np.full(len(O), np.inf)
    for i in range(len(F)):
        p = np.cross(D, e2[i]); det = p @ e1[i]
        ok = np.abs(det) > 1e-12
        inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
        s = O - a[i]; u = (s * p).sum(1) * inv
        q = np.cross(s, e1[i]); w = (D * q).sum(1) * inv
        t = (q @ e2[i]) * inv
        hit = ok & (u >= 0) & (w >= 0) & (u + w <= 1) & (t > 1e-6)
        inside = (D @ FN[i]) > 0            # leaving through this face = seen from inside
        hit &= (~inside) | (t > skip)
        best = np.where(hit & (t < best), t, best)
    return best

def run(sub, R, n=40000, skip=3.0, wall=False):
    V, F = icosphere(sub, R)
    nSolid = len(F)
    FN = np.cross(V[F[:, 1]] - V[F[:, 0]], V[F[:, 2]] - V[F[:, 0]]); FN /= np.linalg.norm(FN, axis=1)[:, None]
    if (FN * V[F].mean(1)).sum(1).min() < 0: raise SystemExit("winding")
    fi = rng.integers(0, len(F), n); r1 = np.sqrt(rng.random(n)); r2 = rng.random(n)
    b = np.stack([1 - r1, r1 * (1 - r2), r1 * r2], 1)
    P = (V[F[fi]] * b[:, :, None]).sum(1)
    Ns = P / np.linalg.norm(P, axis=1)[:, None]          # the smooth normal a sphere's vertex normals interpolate to
    Ng = FN[fi]
    L = np.array([0.3, 0.2, 1.0]); L /= np.linalg.norm(L); lightPos = L * 400.0
    if wall:
        # a wall between the solid and the light, big enough to cover all of it: every point is really shadowed
        c = L * 150.0; ux = np.cross(L, [0, 0, 1.0]); ux /= np.linalg.norm(ux); uy = np.cross(L, ux); S = 300.0
        q = np.array([c - ux*S - uy*S, c + ux*S - uy*S, c + ux*S + uy*S, c - ux*S + uy*S])
        V = np.vstack([V, q]); k = len(V) - 4
        # both windings: a wall seen from outside whichever way it faces
        F = np.vstack([F, [[k, k+1, k+2], [k, k+2, k+3], [k, k+2, k+1], [k, k+3, k+2]]])
        FN = np.cross(V[F[:, 1]] - V[F[:, 0]], V[F[:, 2]] - V[F[:, 0]]); FN /= np.linalg.norm(FN, axis=1)[:, None]
    edge = np.linalg.norm(V[F[:nSolid, 1]] - V[F[:nSolid, 0]], axis=1).mean()
    out = []
    for name, push, rule, blend in (("legacy start 1.0", 1.0, False, False), ("near start 0.05", 0.05, False, False),
                                    ("near 0.05 + own-plane rule", 0.05, True, False), ("near 0.05 where the facet faces the light", 0.05, False, True)):
        if blend:
            g0 = (Ng * ((lightPos - P) / np.linalg.norm(lightPos - P, axis=1)[:, None])).sum(1)
            w = np.clip(g0 / 0.15, 0, 1); w = w * w * (3 - 2 * w)
            push = (1.0 * (1 - w) + 0.05 * w)[:, None]
        O = P + Ng * push
        toL = lightPos - O; dL = np.linalg.norm(toL, axis=1); D = toL / dL[:, None]
        sNL = (Ns * D).sum(1); gNL = (Ng * D).sum(1)
        lit = sNL > 0                                     # what the interaction would light
        t = trace(O, D, V, F, FN, skip)
        shadow = np.isfinite(t)
        if rule:
            sink = -gNL
            own = (sink > 1e-4) & (t > 0.85 * push / np.maximum(sink, 1e-9))
            shadow &= ~own
        lost = (sNL * (lit & shadow)).sum() / (sNL * lit).sum()
        band = (lit & (gNL < 0))                          # shading says lit, facet faces away
        leak = (sNL * (lit & ~shadow)).sum() / (sNL * lit).sum()
        out.append((name, 100 * lost, 100 * (lit & shadow).sum() / lit.sum(), 100 * (shadow & band).sum() / max(band.sum(), 1), 100 * leak, 100 * (lit & ~shadow).sum() / lit.sum()))
    print("%d triangles, radius %g, mean edge %.1f units%s" % (nSolid, R, edge, ", behind a wall (everything is really shadowed)" if wall else ""))
    for o in out:
        if wall: print("   %-42s light LEAKED %5.2f%%   lit points not shadowed %5.2f%%" % (o[0], o[4], o[5]))
        else:    print("   %-42s light lost %5.2f%%   lit points shadowed %5.2f%%   of the facing-away band %5.1f%%" % o[:4])

for sub, R in ((1, 10.0), (2, 10.0), (1, 40.0), (2, 40.0)):
    run(sub, R)
for sub, R in ((1, 10.0), (1, 40.0)):
    run(sub, R, wall=True)
