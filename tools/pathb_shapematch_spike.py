# Region shape-matching spike (plan Phase 0/2, fingerprint-directed).
#
# C2 measured: the sculpt preserves AUTHORED local curvature (med 2.7-3.5 deg
# drift) while sitting 5-8u outside the authored position - a quasi-isometric
# transport of the authored shell. Region shape-matching (Muller) IS that law:
# overlapping clusters projected toward the best-fit rigid transform of their
# AUTHORED rest shape, blended; contact + band support position the shell.
#
# Support law (plan Phase 1): band (authoredGap<=0.3) binds ONCE in the
# authored pose (nearest ref-body triangle, barycentric + normal offset) and
# transports with the morphed triangle frame. No vanilla-conform anywhere.
#
# Usage:
#   py -3 pathb_shapematch_spike.py [--alpha 0.6] [--rings 2] [--iters 200]
#       [--steps 6] [--gravity 0.01] [--out ../tmp/pathb/sm_bs.obj]
import argparse
import sys
from pathlib import Path

import numpy as np
from scipy.spatial import cKDTree

sys.path.insert(0, str(Path(__file__).parent))
from pathb_metrics import load_obj, _pt_tri_dist2
from shell_spike import UniformBodyCollider, read_capture_front

CAP = Path("../corpus/fur_blacksmith_rm_heavy/drape_capture__Rley_3BA_Vanilla_-_Body_-_Blacksmith__Physics___BlackSmithTorso__RM_Heavy.drcap")


def exact_seam_groups(base_v, tol=0.003):
    key = np.round(base_v / tol).astype(np.int64)
    seen = {}
    for i, k in enumerate(map(tuple, key)):
        seen.setdefault(k, []).append(i)
    return [np.array(g) for g in seen.values() if len(g) > 1]


def bind_band(base, band_idx, bverts, btris):
    """Bind each band vert to its nearest REF body triangle (authored pose)."""
    tri = bverts[btris]
    cen = tri.mean(1)
    kd = cKDTree(cen)
    binds = []
    for i in band_idx:
        p = base[i]
        _, cand = kd.query(p, k=24)
        cand = np.atleast_1d(cand)
        a, b, c = (bverts[btris[cand, 0]], bverts[btris[cand, 1]],
                   bverts[btris[cand, 2]])
        d2 = _pt_tri_dist2(np.repeat(p[None], len(cand), 0), a, b, c)
        tbest = int(cand[np.argmin(d2)])
        A, B, C = bverts[btris[tbest]]
        # barycentric of the closest point (project p to the triangle plane)
        n = np.cross(B - A, C - A)
        nn = n / max(np.linalg.norm(n), 1e-12)
        q = p - nn * np.dot(p - A, nn)
        v0, v1, v2 = B - A, C - A, q - A
        d00, d01, d11 = v0 @ v0, v0 @ v1, v1 @ v1
        d20, d21 = v2 @ v0, v2 @ v1
        den = max(d00 * d11 - d01 * d01, 1e-12)
        w1 = (d11 * d20 - d01 * d21) / den
        w2 = (d00 * d21 - d01 * d20) / den
        w0 = 1.0 - w1 - w2
        off = float(np.dot(p - A, nn))     # authored normal offset (signed)
        binds.append((tbest, w0, w1, w2, off))
    return binds


def band_targets(binds, bverts, btris):
    out = np.empty((len(binds), 3))
    for k, (t, w0, w1, w2, off) in enumerate(binds):
        A, B, C = bverts[btris[t]]
        n = np.cross(B - A, C - A)
        nn = n / max(np.linalg.norm(n), 1e-12)
        out[k] = w0 * A + w1 * B + w2 * C + nn * off
    return out


class QuickClosest:
    # exact point-triangle over k=64 KD candidates (spike-grade contact;
    # sidesteps a pre-existing sentinel bug in UniformBodyCollider's
    # exact-resolution loop when querying far-from-body points)
    def __init__(self, verts, tris):
        self.tris = tris
        tri = verts[tris]
        self.a, self.b, self.c = tri[:, 0], tri[:, 1], tri[:, 2]
        n = np.cross(self.b - self.a, self.c - self.a)
        self.n = n / np.maximum(np.linalg.norm(n, axis=1), 1e-12)[:, None]
        self.kd = cKDTree(tri.mean(1))

    def query(self, pts, k=64):
        k = min(k, len(self.tris))
        _, cand = self.kd.query(pts, k=k)
        cand = np.asarray(cand).reshape(len(pts), k)
        flat = cand.reshape(-1)
        rep = np.repeat(pts, k, axis=0)
        d2 = _pt_tri_dist2(rep, self.a[flat], self.b[flat],
                           self.c[flat]).reshape(len(pts), k)
        loc = np.argmin(d2, axis=1)
        row = np.arange(len(pts))
        tbest = cand[row, loc]
        dist = np.sqrt(d2[row, loc])
        nrm = self.n[tbest]
        sgn = np.sign(np.einsum('ij,ij->i', pts - self.a[tbest], nrm))
        sgn = np.where(sgn == 0, 1.0, sgn)
        return nrm, dist * sgn


def build_clusters(n, tris, rings):
    adj = [[] for _ in range(n)]
    for t in tris:
        for a in range(3):
            for b in range(3):
                if a != b:
                    adj[t[a]].append(t[b])
    adj = [np.unique(a) for a in adj]
    clusters = []
    for i in range(n):
        seen = {i}
        frontier = [i]
        for _ in range(rings):
            nxt = []
            for j in frontier:
                for k in adj[j]:
                    if k not in seen:
                        seen.add(k)
                        nxt.append(k)
            frontier = nxt
        clusters.append(np.fromiter(seen, dtype=np.int64))
    return clusters


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--alpha", type=float, default=0.6)
    ap.add_argument("--rings", type=int, default=2)
    ap.add_argument("--iters", type=int, default=200)
    ap.add_argument("--steps", type=int, default=6)
    ap.add_argument("--gravity", type=float, default=0.01)
    ap.add_argument("--strain", type=float, default=1.10)
    ap.add_argument("--clearance", type=float, default=0.15)
    ap.add_argument("--out", default="../tmp/pathb/sm_bs.obj")
    a = ap.parse_args()

    cap = read_capture_front(CAP)
    base = cap.base_verts.copy()
    n = len(base)
    col0 = UniformBodyCollider(cap.ref_body_verts, cap.ref_body_tris)
    g0 = col0.gaps(base)
    band = (g0 <= 0.3)
    inside = (g0 < -0.1)
    movable = ~band
    band_idx = np.nonzero(band)[0]
    print(f"n={n} band={band.sum()} movable={movable.sum()} authored-inside={inside.sum()}")

    binds = bind_band(base, band_idx, cap.ref_body_verts, cap.ref_body_tris)
    clusters = build_clusters(n, cap.tris, a.rings)
    rest_cm = [base[c].mean(0) for c in clusters]
    rest_q = [base[c] - cm for c, cm in zip(clusters, rest_cm)]
    groups = exact_seam_groups(base)
    edges = cap.edges
    e_rest = np.linalg.norm(base[edges[:, 0]] - base[edges[:, 1]], axis=1)

    x = base.copy()
    for step in range(1, a.steps + 1):
        t = step / a.steps
        bv = cap.ref_body_verts + (cap.body_verts - cap.ref_body_verts) * t
        col = QuickClosest(bv, cap.body_tris)
        tgt = band_targets(binds, bv, cap.body_tris)
        x[band_idx] = tgt
        it_used, mv = 0, 0.0
        for it in range(a.iters):
            def ck(tag):
                if not np.isfinite(x).all():
                    bad = np.nonzero(~np.isfinite(x).all(axis=1))[0]
                    raise SystemExit(
                        f"NaN após {tag} step={step} it={it} verts={bad[:8]}")
            xp = x.copy()
            # gravity on free verts
            x[movable, 2] -= a.gravity
            ck('gravity')
            # region shape matching toward AUTHORED local shape
            goal = np.zeros_like(x)
            wsum = np.zeros(n)
            for c, q0, cm0 in zip(clusters, rest_q, rest_cm):
                cur = x[c]
                cm = cur.mean(0)
                H = q0.T @ (cur - cm)
                U, S, Vt = np.linalg.svd(H)
                R = (U @ Vt).T
                if np.linalg.det(R) < 0:
                    U[:, -1] *= -1
                    R = (U @ Vt).T
                g = (R @ q0.T).T + cm
                goal[c] += g
                wsum[c] += 1.0
            goal /= np.maximum(wsum, 1.0)[:, None]
            x[movable] += a.alpha * (goal[movable] - x[movable])
            ck('shape-match')
            # tension strain limit - JACOBI-AVERAGED (unaveraged add.at let a
            # 6-ring vert take 6 full corrections = the step-2 resonance pump)
            for _ in range(2):
                d = x[edges[:, 1]] - x[edges[:, 0]]
                L = np.linalg.norm(d, axis=1)
                over = L > e_rest * a.strain
                if not over.any():
                    break
                corr = d[over] * ((L[over] - e_rest[over] * a.strain) /
                                  np.maximum(L[over], 1e-9))[:, None]
                m0 = movable[edges[over, 0]].astype(float)
                m1 = movable[edges[over, 1]].astype(float)
                wsum2 = np.maximum(m0 + m1, 1e-9)
                acc = np.zeros_like(x)
                cnt = np.zeros(len(x))
                np.add.at(acc, edges[over, 0], corr * (m0 / wsum2)[:, None])
                np.add.at(acc, edges[over, 1], -corr * (m1 / wsum2)[:, None])
                np.add.at(cnt, edges[over, 0], m0)
                np.add.at(cnt, edges[over, 1], m1)
                nz = cnt > 0
                x[nz] += acc[nz] / cnt[nz][:, None]
            ck('strain')
            # contact: thickness floor along the exact nearest normal,
            # CAPPED per iteration (an uncapped full-depth push fought the
            # shape matching and diverged over ~tens of iterations)
            nrm, sgap = col.query(x)
            pen = movable & ~inside & (sgap < a.clearance)
            if pen.any():
                push = np.minimum(a.clearance - sgap[pen], 0.25)
                x[pen] += nrm[pen] * push[:, None]
            # global damping: overdamped quasi-static (kills residual
            # phase-interaction resonance; equilibrium unchanged)
            x[movable] = xp[movable] + 0.85 * (x[movable] - xp[movable])
            # trust region: no vert moves more than 1u per iteration - the
            # equilibrium is unchanged (cap inactive when settled) and no
            # phase interaction can run away to inf (step-2 strain runaway)
            dlt = x - xp
            dn = np.linalg.norm(dlt, axis=1)
            far = dn > 1.0
            if far.any():
                x[far] = xp[far] + dlt[far] * (1.0 / dn[far])[:, None]
            # band + seams
            x[band_idx] = tgt
            for g in groups:
                x[g] = x[g].mean(0)
            x[band_idx] = tgt
            if not np.isfinite(x).all():
                raise SystemExit(f"DIVERGED at step {step} iter {it}")
            mv = float(np.linalg.norm(x - xp, axis=1)[movable].max())
            it_used = it + 1
            if mv < 0.01:
                break
        print(f"step {step}/{a.steps}: iters={it_used} maxMove={mv:.4f}")

    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    with open(a.out, "w", newline="\n") as f:
        for v in x:
            f.write(f"v {v[0]:.6f} {v[1]:.6f} {v[2]:.6f}\n")
        for tvi in cap.tris:
            f.write(f"f {tvi[0]+1} {tvi[1]+1} {tvi[2]+1}\n")
    print(f"wrote {a.out}")

    # gate: back profile vs sculpt
    gt, _ = load_obj("../fixtures/pathb/blacksmith__GROUND_TRUTH.obj")
    def sil(vv):
        out = []
        for z in range(38, 82, 4):
            m = (vv[:, 2] >= z) & (vv[:, 2] < z + 4) & (vv[:, 1] < 0)
            out.append(f"{-vv[m][:, 1].min():.1f}" if m.sum() else "-")
        return out
    print("SCULPT :", " ".join(sil(gt)))
    print("RESULT :", " ".join(sil(x)))


if __name__ == "__main__":
    main()
