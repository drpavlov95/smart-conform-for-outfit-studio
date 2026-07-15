# Path B S0.2a: separated, correctly-labeled metrics for a candidate mesh.
#
# Reports, per (garment, OBJ), never mixing labels:
#   - indexed  mean/p95 : per-vertex ||test[i] - ref[i]|| (needs same vert count)
#   - surface fwd mean/p95 : each TEST vert -> closest point on REF surface
#   - surface rev mean/p95 : each REF  vert -> closest point on TEST surface
#     (rev catches collapse / lost coverage that fwd can hide)
#   - profile  : back panel outward extent by z (bulge/imprint probe)
#   - hem      : on a FROZEN authored vertex-ID list, radial spread std +
#                mean nearest-neighbour distance among the hem verts
#   - integrity: new flipped faces / degenerate faces / open exact-seams /
#                new non-adjacent triangle crossings / NaN  (vs the authored base)
#
# Surface distance is EXACT: candidate triangles via a centroid KD-tree, exact
# point-triangle clamping, ring/k expanded until the k-th centroid is farther
# than the current best + max triangle radius; a 64-sample brute-force check
# (diff < 1e-6) runs on first use and hard-errors if the fast path is wrong.
#
# Usage:
#   pathb_metrics.py --test T.obj [--ref GT.obj] [--base BASE.obj]
#                    [--hem-ids ids.txt] [--label NAME]
import argparse
import hashlib
import sys
import numpy as np

try:
    from scipy.spatial import cKDTree
    _HAVE_KD = True
except Exception:
    _HAVE_KD = False


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


def sha12(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()[:12]


def _pt_tri_dist2(p, a, b, c):
    # squared distance from points p (N,3) to triangles (a,b,c each (N,3)),
    # exact closest-point-on-triangle (Ericson, vectorized).
    ab = b - a
    ac = c - a
    ap = p - a
    d1 = (ab * ap).sum(1)
    d2 = (ac * ap).sum(1)
    bp = p - b
    d3 = (ab * bp).sum(1)
    d4 = (ac * bp).sum(1)
    cp = p - c
    d5 = (ab * cp).sum(1)
    d6 = (ac * cp).sum(1)
    va = d3 * d6 - d5 * d4
    vb = d5 * d2 - d1 * d6
    vc = d1 * d4 - d3 * d2
    denom = va + vb + vc
    v = np.zeros_like(d1)
    w = np.zeros_like(d1)
    # default: interior
    inter = denom > 1e-30
    v[inter] = vb[inter] / denom[inter]
    w[inter] = vc[inter] / denom[inter]
    closest = a + v[:, None] * ab + w[:, None] * ac
    # vertex/edge regions override the interior where the barycentric test fails
    m = (d1 <= 0) & (d2 <= 0)
    closest[m] = a[m]
    m = (d3 >= 0) & (d4 <= d3)
    closest[m] = b[m]
    m = (d6 >= 0) & (d5 <= d6)
    closest[m] = c[m]
    m = (vc <= 0) & (d1 >= 0) & (d3 <= 0)
    t = np.zeros_like(d1)
    md = m & (d1 - d3 != 0)
    t[md] = d1[md] / (d1[md] - d3[md])
    closest[m] = a[m] + t[m, None] * ab[m]
    m = (vb <= 0) & (d2 >= 0) & (d6 <= 0)
    t = np.zeros_like(d1)
    md = m & (d2 - d6 != 0)
    t[md] = d2[md] / (d2[md] - d6[md])
    closest[m] = a[m] + t[m, None] * ac[m]
    m = (va <= 0) & (d4 - d3 >= 0) & (d5 - d6 >= 0)
    t = np.zeros_like(d1)
    md = m & ((d4 - d3) - (d5 - d6) != 0)
    t[md] = (d4[md] - d3[md]) / ((d4[md] - d3[md]) - (d5[md] - d6[md]))
    closest[m] = b[m] + t[m, None] * (c[m] - b[m])
    diff = p - closest
    return (diff * diff).sum(1)


class Surface:
    def __init__(self, verts, tris):
        self.v = verts
        self.t = tris
        tri = verts[tris]
        self.a, self.b, self.c = tri[:, 0], tri[:, 1], tri[:, 2]
        self.cen = tri.mean(1)
        # triangle circumradius upper bound = max edge from centroid to vertex
        self.tri_rad = np.linalg.norm(tri - self.cen[:, None, :], axis=2).max(1)
        self.max_rad = float(self.tri_rad.max()) if len(tris) else 0.0
        self.kd = cKDTree(self.cen) if (_HAVE_KD and len(tris)) else None
        self._checked = False

    def _brute(self, p):
        d2 = _pt_tri_dist2(np.repeat(p[None], len(self.t), 0), self.a, self.b, self.c)
        return float(np.sqrt(d2.min()))

    def dist(self, pts):
        if len(self.t) == 0:
            return np.full(len(pts), np.nan)
        if self.kd is None:
            return np.array([self._brute(p) for p in pts])
        out = np.empty(len(pts))
        k = min(32, len(self.t))
        for i, p in enumerate(pts):
            best = np.inf
            kk = k
            while True:
                d, idx = self.kd.query(p, k=kk)
                idx = np.atleast_1d(idx)
                d = np.atleast_1d(d)
                d2 = _pt_tri_dist2(np.repeat(p[None], len(idx), 0),
                                   self.a[idx], self.b[idx], self.c[idx])
                best = float(np.sqrt(d2.min()))
                # exactness: the (kk-th) centroid distance minus max tri radius is
                # a lower bound for any not-yet-considered triangle's distance.
                if kk >= len(self.t) or d[-1] - self.max_rad >= best:
                    break
                kk = min(kk * 2, len(self.t))
            out[i] = best
        return out

    def check_exact(self, pts):
        if self._checked or self.kd is None or len(self.t) == 0:
            return
        self._checked = True
        n = min(64, len(pts))
        sample = pts[np.linspace(0, len(pts) - 1, n).astype(int)]
        fast = self.dist(sample)
        for p, f in zip(sample, fast):
            b = self._brute(p)
            if abs(f - b) > 1e-6:
                raise SystemExit(f"surface-distance exactness FAILED: fast={f} brute={b}")


def surf_stats(test_v, test_t, ref_v, ref_t):
    sref = Surface(ref_v, ref_t)
    stest = Surface(test_v, test_t)
    sref.check_exact(test_v)
    fwd = sref.dist(test_v)      # test vert -> ref surface
    rev = stest.dist(ref_v)      # ref  vert -> test surface
    return fwd, rev


def face_normals(v, t):
    tri = v[t]
    n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    area = np.linalg.norm(n, axis=1)
    return n, area


def integrity(test_v, base_v, tris, seam_groups):
    out = {}
    tn, ta = face_normals(test_v, tris)
    bn, ba = face_normals(base_v, tris)
    dot = (tn * bn).sum(1)
    good = (ta > 1e-9) & (ba > 1e-9)
    out["flipped_faces"] = int((good & (dot < 0)).sum())
    out["degenerate_faces"] = int((ba > 1e-9).sum() and ((ta < 0.10 * ba) & (ba > 1e-9)).sum())
    out["nan"] = int(np.isnan(test_v).sum())
    worst_seam = 0.0
    for grp in seam_groups:
        pts = test_v[grp]
        worst_seam = max(worst_seam, float(np.linalg.norm(pts - pts[0], axis=1).max()))
    out["worst_seam_split"] = round(worst_seam, 4)
    return out


def exact_seam_groups(base_v, tol=0.003):
    # groups of vertices coincident in the AUTHORED base (must stay welded)
    key = np.round(base_v / tol).astype(np.int64)
    seen = {}
    for i, k in enumerate(map(tuple, key)):
        seen.setdefault(k, []).append(i)
    return [g for g in seen.values() if len(g) > 1]


def back_profile(v, xlo=7.0, xhi=14.0):
    # outward extent (-y) by z on the back-right band; a bulge = local max
    rows = []
    for zlo in range(int(v[:, 2].min()), int(v[:, 2].max()) + 1, 3):
        m = (v[:, 0] > xlo) & (v[:, 0] < xhi) & (v[:, 1] < 0) & \
            (v[:, 2] >= zlo) & (v[:, 2] < zlo + 3)
        if m.sum():
            rows.append((zlo, round(float(-v[m][:, 1].min()), 2)))
    return rows


def hem_stats(v, ids):
    p = v[ids]
    c = p[:, :2].mean(0)
    r = np.hypot(p[:, 0] - c[0], p[:, 1] - c[1])
    if _HAVE_KD and len(p) > 1:
        d, _ = cKDTree(p).query(p, k=2)
        nn = float(d[:, 1].mean())
    else:
        nn = float("nan")
    return {"n": len(ids), "radial_std": round(float(r.std()), 3),
            "mean_nn": round(nn, 3)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--test", required=True)
    ap.add_argument("--ref")
    ap.add_argument("--base")
    ap.add_argument("--hem-ids")
    ap.add_argument("--label", default="")
    a = ap.parse_args()
    tv, tt = load_obj(a.test)
    line = {"label": a.label or a.test.split("/")[-1], "sha": sha12(a.test),
            "verts": len(tv), "faces": len(tt)}
    detail = []
    if a.ref:
        rv, rt = load_obj(a.ref)
        if len(rv) == len(tv):
            idx = np.linalg.norm(tv - rv, axis=1)
            line["indexed_mean"] = round(float(idx.mean()), 4)
            line["indexed_p95"] = round(float(np.percentile(idx, 95)), 4)
        else:
            line["indexed_mean"] = "NCOMP"
        fwd, rev = surf_stats(tv, tt, rv, rt)
        line["surf_fwd_mean"] = round(float(np.nanmean(fwd)), 4)
        line["surf_fwd_p95"] = round(float(np.nanpercentile(fwd, 95)), 4)
        line["surf_rev_mean"] = round(float(np.nanmean(rev)), 4)
        line["surf_rev_p95"] = round(float(np.nanpercentile(rev, 95)), 4)
    if a.base:
        bv, bt = load_obj(a.base)
        if len(bv) == len(tv):
            groups = exact_seam_groups(bv)
            line.update({f"int_{k}": v for k, v in
                         integrity(tv, bv, tt, groups).items()})
        else:
            line["int_note"] = "base vcount mismatch"
    if a.hem_ids:
        ids = [int(x) for x in open(a.hem_ids) if not x.startswith("#")]
        detail.append(("hem", hem_stats(tv, ids)))
    detail.append(("back_profile", back_profile(tv)))
    print("SUMMARY " + " ".join(f"{k}={v}" for k, v in line.items()))
    for name, d in detail:
        print(f"  {name}: {d}")


if __name__ == "__main__":
    main()
