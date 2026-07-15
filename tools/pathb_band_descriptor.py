# Path B S0.3a: material-consistent band-circumference descriptor (H3b).
#
# The old descriptor summed "roughly horizontal edges" including diagonals,
# per topological component before seam union, grouped by world-Z. That is not
# a circumference and depends on triangulation. This one is correct by
# construction:
#
#   - weld exact-coincident base verts (seam union) and remap triangles;
#   - split into connected components on the welded topology;
#   - per component, axis u = PCA principal direction of the AUTHORED base
#     verts (works for skirts, cloaks, sleeves; not tied to world Z);
#   - material coordinate s(v) = normalized projection of base[v] on u in [0,1];
#   - for an iso-level s_k, every triangle straddling s_k is cut by the material
#     iso-line into ONE segment between two edge-crossing points; the crossing
#     point on edge (a,b) sits at the SAME material fraction in base and in the
#     measured mesh, so the segment length in each mesh is comparable;
#   - arc(s_k) = sum of those segment lengths (the true cross-section perimeter),
#     invariant to diagonal orientation and subdivision.
#
# Reports arc_mesh(s_k)/arc_base(s_k) per component per band.
#
# Usage:
#   pathb_band_descriptor.py --base BASE.obj --mesh M.obj [--levels 9]
#   pathb_band_descriptor.py --selftest        # invariance on synthetic meshes
import argparse
import sys
import numpy as np


def load_obj(path):
    vs, fs = [], []
    for line in open(path):
        if line.startswith("v "):
            vs.append([float(x) for x in line.split()[1:4]])
        elif line.startswith("f "):
            idx = [int(t.split("/")[0]) - 1 for t in line.split()[1:]]
            for k in range(1, len(idx) - 1):
                fs.append([idx[0], idx[k], idx[k + 1]])
    return np.asarray(vs, dtype=np.float64), np.asarray(fs, dtype=np.int64)


def weld(base_v, tris, tol=0.003):
    key = np.round(base_v / tol).astype(np.int64)
    rep = {}
    remap = np.empty(len(base_v), dtype=np.int64)
    canon = []
    for i, k in enumerate(map(tuple, key)):
        if k not in rep:
            rep[k] = len(canon)
            canon.append(i)
        remap[i] = rep[k]
    return remap, np.asarray(canon, dtype=np.int64)


def components(n, tris):
    parent = list(range(n))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for t in tris:
        for a, b in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
            ra, rb = find(a), find(b)
            if ra != rb:
                parent[ra] = rb
    groups = {}
    for i in range(n):
        groups.setdefault(find(i), []).append(i)
    return list(groups.values())


def band_arcs(base_v, mesh_v, tris, levels):
    """Return {component_index: [(s_k, arc_base, arc_mesh, ratio), ...]}."""
    remap, canon = weld(base_v, tris)
    wtris = remap[tris]
    wbase = base_v[canon]
    wmesh = mesh_v[canon]
    nw = len(canon)
    # which welded triangles per component
    comp_of = np.full(nw, -1, dtype=np.int64)
    comps = components(nw, wtris)
    for ci, verts in enumerate(comps):
        for v in verts:
            comp_of[v] = ci
    out = {}
    for ci, comp_verts in enumerate(comps):
        cv = np.asarray(comp_verts)
        if len(cv) < 8:
            continue
        pts = wbase[cv]
        centroid = pts.mean(0)
        # PCA principal axis
        cov = np.cov((pts - centroid).T)
        w, vec = np.linalg.eigh(cov)
        axis = vec[:, np.argmax(w)]
        s_all = (wbase - centroid) @ axis
        smin = s_all[cv].min()
        smax = s_all[cv].max()
        span = max(smax - smin, 1e-9)
        s_norm = (s_all - smin) / span  # material coord in [0,1] on this comp
        ctris = wtris[comp_of[wtris[:, 0]] == ci]
        rows = []
        for lvl in levels:
            arc_b = 0.0
            arc_m = 0.0
            for t in ctris:
                s = s_norm[t]
                above = s > lvl
                if above.sum() == 0 or above.sum() == 3:
                    continue  # triangle does not straddle this level
                # the two edges that cross: from the lone vertex to the other two
                lone = int(np.where(above == (above.sum() == 1))[0][0])
                others = [j for j in range(3) if j != lone]
                cp_b = []
                cp_m = []
                for o in others:
                    a, b = t[lone], t[o]
                    denom = s_norm[b] - s_norm[a]
                    f = 0.5 if abs(denom) < 1e-12 else (lvl - s_norm[a]) / denom
                    f = min(max(f, 0.0), 1.0)
                    cp_b.append(wbase[a] + f * (wbase[b] - wbase[a]))
                    cp_m.append(wmesh[a] + f * (wmesh[b] - wmesh[a]))
                arc_b += float(np.linalg.norm(cp_b[0] - cp_b[1]))
                arc_m += float(np.linalg.norm(cp_m[0] - cp_m[1]))
            ratio = arc_m / arc_b if arc_b > 1e-9 else float("nan")
            rows.append((round(lvl, 2), round(arc_b, 3), round(arc_m, 3),
                         round(ratio, 3)))
        out[ci] = (len(cv), rows)
    return out


def _cylinder(nz=10, nr=16, flip=False, dup_seam=False, subdiv=False):
    zs = np.linspace(0, 10, nz)
    ang = np.linspace(0, 2 * np.pi, nr, endpoint=False)
    verts = []
    grid = {}
    for i, z in enumerate(zs):
        for j, a in enumerate(ang):
            grid[(i, j)] = len(verts)
            verts.append([np.cos(a), np.sin(a), z])
    tris = []
    for i in range(nz - 1):
        for j in range(nr):
            j2 = (j + 1) % nr
            v00, v01 = grid[(i, j)], grid[(i, j2)]
            v10, v11 = grid[(i + 1, j)], grid[(i + 1, j2)]
            if flip:
                tris += [[v00, v01, v11], [v00, v11, v10]]
            else:
                tris += [[v00, v01, v10], [v01, v11, v10]]
    verts = np.asarray(verts, float)
    tris = np.asarray(tris, int)
    if dup_seam:  # duplicate the seam column j=0 as new coincident verts
        seam = [grid[(i, 0)] for i in range(nz)]
        newidx = {old: len(verts) + k for k, old in enumerate(seam)}
        verts = np.vstack([verts, verts[seam]])
        for t in tris:
            for k in range(3):
                if t[k] in newidx and np.random.RandomState(t[k]).rand() > 0.5:
                    t[k] = newidx[t[k]]
    return verts, tris


def selftest():
    lv = [0.25, 0.5, 0.75]
    base, tris = _cylinder()
    ref = band_arcs(base, base, tris, lv)[0][1]
    ok = True
    for name, (v2, t2) in [("flip", _cylinder(flip=True)),
                           ("subdiv", _cylinder(nz=20, nr=32))]:
        got = band_arcs(v2, v2, t2, lv)[0][1]
        for (l1, ab1, _, _), (l2, ab2, _, _) in zip(ref, got):
            # circumference of unit cylinder ~ 2*pi*0.99 (chord approx); compare
            if abs(ab1 - ab2) / max(ab1, 1e-9) > 0.05:
                print(f"  INVARIANCE FAIL {name} @s={l1}: {ab1} vs {ab2}")
                ok = False
    print("selftest:", "PASS" if ok else "FAIL",
          f"(cylinder base arc @0.5 = {ref[1][1]}, expect ~{2*np.pi*0.99:.2f})")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base")
    ap.add_argument("--mesh")
    ap.add_argument("--levels", type=int, default=9)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        sys.exit(selftest())
    bv, tris = load_obj(a.base)
    mv, _ = load_obj(a.mesh)
    levels = list(np.linspace(0.1, 0.9, a.levels))
    res = band_arcs(bv, mv, tris, levels)
    print(f"# band descriptor  base={a.base.split('/')[-1]} mesh={a.mesh.split('/')[-1]}")
    for ci, (nverts, rows) in sorted(res.items()):
        print(f"component {ci} ({nverts} verts):")
        for s, ab, am, r in rows:
            flag = "  <-- collapsed" if r < 0.9 else ("  <-- expanded" if r > 1.1 else "")
            print(f"  s={s:.2f}  arc_base={ab:8.3f}  arc_mesh={am:8.3f}  ratio={r:.3f}{flag}")


if __name__ == "__main__":
    main()
