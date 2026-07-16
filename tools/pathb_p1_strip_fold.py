#!/usr/bin/env python3
"""P1: 1D strip prototype of the user's hinge-fold, as a REFINEMENT layer.

Runs on the PRODUCTION result (the pipeline's "excellent state"), offline.
Each movable vertex belongs to a meridian chain (steepest-ascent parent in the
AUTHORED base mesh). From the support set (preset anchors + body contact) the
chain is re-posed by forward kinematics: every edge keeps its CURRENT length
and rotates its direction toward straight down by a residual fraction lambda,
stopping at body contact (bisection on the rotation). Exact seam twins are
re-welded afterwards.

This is the 1-column idealization: lateral edges are NOT constrained (that is
P2's problem) - their stretch is REPORTED as the P2 difficulty gauge.

Judges (per lambda):
  - Codex's S05 local imprint metric (did the body imprint drop?)
  - back-right-thigh profile (the user's visible bump)
  - integrity: flips vs base (compared to production's own count), lateral
    edge stretch, body penetrations, displacement stats
  - determinism (byte-identical rerun)

Usage:
  py -3 tools/pathb_p1_strip_fold.py --capture CAP --candidate PROD.obj \
      [--lambdas 0.25,0.5,0.75,1.0] [--contact-anchor 0.6] [--clearance 0.2] \
      [--out-prefix tmp/pathb/p1_bs]
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np

import pathb_local_metrics as plm
from shell_spike import (
    UniformBodyCollider,
    fitted_drape_signal,
    read_capture_front,
)

DOWN = np.array([0.0, 0.0, -1.0])


def load_obj(path):
    vs = []
    for line in open(path):
        if line.startswith("v "):
            vs.append([float(x) for x in line.split()[1:4]])
    return np.asarray(vs, dtype=np.float64)


def write_obj(path, verts, tris):
    with open(path, "w", newline="\n") as f:
        for v in verts:
            f.write(f"v {v[0]:.6f} {v[1]:.6f} {v[2]:.6f}\n")
        for t in tris:
            f.write(f"f {t[0]+1} {t[1]+1} {t[2]+1}\n")


def exact_seam_groups(base_v, tol=0.003):
    key = np.round(base_v / tol).astype(np.int64)
    seen = {}
    for i, k in enumerate(map(tuple, key)):
        seen.setdefault(k, []).append(i)
    return [g for g in seen.values() if len(g) > 1]


def build_parents(base, edges):
    """Steepest-ascent parent per vertex in the AUTHORED mesh (meridian forest)."""
    n = len(base)
    adj: list[list[int]] = [[] for _ in range(n)]
    for a, b in edges:
        adj[int(a)].append(int(b))
        adj[int(b)].append(int(a))
    parent = np.full(n, -1, dtype=np.int64)
    for i in range(n):
        best = -1.0
        for j in adj[i]:
            dz = base[j, 2] - base[i, 2]
            if dz <= 1.0e-9:
                continue
            L = float(np.linalg.norm(base[j] - base[i]))
            score = dz / max(L, 1.0e-9)   # verticality of the ascent edge
            if score > best:
                best = score
                parent[i] = j
    return parent, adj


def nlerp(d, t):
    v = (1.0 - t) * d + t * DOWN
    L = float(np.linalg.norm(v))
    return v / L if L > 1.0e-12 else DOWN


def fold(cand, base, parent, support, movable, collider, lam, clearance):
    n = len(cand)
    # roots: process children only after the parent is resolved
    resolved = support.copy()
    new = cand.copy()
    order = np.argsort(-base[:, 2])          # top -> down in authored space
    stopped = 0
    orphans = 0
    for i in order:
        if support[i] or not movable[i]:
            continue
        p = parent[i]
        if p < 0 or not resolved[p]:
            orphans += 1                      # no supported chain above: leave as-is
            resolved[i] = True                # children of an orphan follow production
            continue
        e = cand[i] - cand[p]
        L = float(np.linalg.norm(e))
        if L < 1.0e-9:
            new[i] = new[p]
            resolved[i] = True
            continue
        d = e / L

        def pos(t):
            return new[p] + nlerp(d, t) * L

        def clear(q):
            return float(collider.gaps(q[None, :])[0]) >= clearance

        target = pos(lam)
        if clear(target):
            new[i] = target
        elif not clear(pos(0.0)):
            new[i] = pos(0.0)                 # even unrotated placement is blocked:
            stopped += 1                      # follow the chain, count it
        else:
            lo, hi = 0.0, lam
            for _ in range(8):
                mid = 0.5 * (lo + hi)
                if clear(pos(mid)):
                    lo = mid
                else:
                    hi = mid
            new[i] = pos(lo)
            stopped += 1
        resolved[i] = True
    return new, stopped, orphans


def flips_vs_base(verts, base, tris):
    def normals(v):
        tri = v[tris]
        return np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    nb = normals(base)
    nv = normals(verts)
    ab = np.linalg.norm(nb, axis=1)
    av = np.linalg.norm(nv, axis=1)
    ok = (ab > 1e-9) & (av > 1e-9)
    return int(((nb * nv).sum(1)[ok] < 0).sum())


def back_thigh(v, xlo=7.0, xhi=14.0):
    rows = []
    for zlo in range(31, 61, 6):
        m = (v[:, 0] > xlo) & (v[:, 0] < xhi) & (v[:, 1] < 0) & \
            (v[:, 2] >= zlo) & (v[:, 2] < zlo + 6)
        if m.sum() >= 3:
            rows.append((zlo, float(-v[m][:, 1].min())))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", required=True)
    ap.add_argument("--candidate", required=True)
    ap.add_argument("--lambdas", default="0.25,0.5,0.75,1.0")
    ap.add_argument("--contact-anchor", type=float, default=0.60)
    ap.add_argument("--clearance", type=float, default=0.20)
    ap.add_argument("--out-prefix")
    a = ap.parse_args()

    cap = read_capture_front(Path(a.capture))
    cand = load_obj(a.candidate)
    n = len(cap.base_verts)
    assert len(cand) == n

    drape = fitted_drape_signal(cap)
    u = np.clip((drape - 0.05) / 0.50, 0.0, 1.0)
    u = u * u * (3.0 - 2.0 * u)
    movable = u > 0.0
    collider = UniformBodyCollider(cap.body_verts, cap.body_tris)
    support = (~movable) | (collider.gaps(cand) <= a.contact_anchor)
    parent, adj = build_parents(cap.base_verts, cap.edges)
    groups = exact_seam_groups(cap.base_verts)

    ctx = plm.prepare_context_from_capture(cap)
    imp0 = plm.measure_imprint(ctx, cand)
    pens0 = int((collider.gaps(cand) < 0.05).sum())
    flips0 = flips_vs_base(cand, cap.base_verts, cap.tris)

    # lateral edges = non-meridian (not the parent link in either direction)
    plink = set()
    for i in range(n):
        if parent[i] >= 0:
            plink.add((min(i, int(parent[i])), max(i, int(parent[i]))))
    lat = np.array([k for k, (x, y) in enumerate(map(tuple, cap.edges))
                    if (min(x, y), max(x, y)) not in plink])
    el0 = np.linalg.norm(cand[cap.edges[:, 0]] - cand[cap.edges[:, 1]], axis=1)

    name = Path(a.capture).name.split("___")[-1].replace(".drcap", "")
    print(f"# P1 strip fold  {name}  n={n} support={int(support.sum())} "
          f"movable={int(movable.sum())}")
    print(f"PRODUCAO: imprintE={imp0.positive_projection_energy:.5f} "
          f"corr={imp0.correlation:.4f} pens={pens0} flips(base)={flips0}")
    print("perfil back-thigh producao:", back_thigh(cand))

    for lam in [float(x) for x in a.lambdas.split(",")]:
        folded, stopped, orphans = fold(cand, cap.base_verts, parent, support,
                                        movable, collider, lam, a.clearance)
        for grp in groups:
            folded[grp] = folded[grp].mean(axis=0)
        imp = plm.measure_imprint(ctx, folded)
        pens = int((collider.gaps(folded) < 0.05).sum())
        fl = flips_vs_base(folded, cap.base_verts, cap.tris)
        el = np.linalg.norm(folded[cap.edges[:, 0]] - folded[cap.edges[:, 1]], axis=1)
        ratio = el / np.maximum(el0, 1e-9)
        lr = ratio[lat] if len(lat) else ratio
        disp = np.linalg.norm(folded - cand, axis=1)
        print(f"\nlambda={lam}: moved n={int((disp > 0.05).sum())} "
              f"mean={disp[disp > 0.05].mean() if (disp > 0.05).any() else 0:.3f} "
              f"max={disp.max():.3f} contactStops={stopped} orphans={orphans}")
        print(f"  imprintE={imp.positive_projection_energy:.5f} "
              f"({(imp.positive_projection_energy / max(imp0.positive_projection_energy, 1e-12) - 1) * 100:+.1f}%) "
              f"corr={imp.correlation:.4f}")
        print(f"  flips(base)={fl} (prod {flips0})  pens={pens} (prod {pens0})  "
              f"lateral stretch p95={np.percentile(lr, 95):.3f} max={lr.max():.3f}")
        print(f"  perfil back-thigh: {back_thigh(folded)}")
        if a.out_prefix:
            out = f"{a.out_prefix}_lam{lam}.obj"
            write_obj(out, folded, cap.tris)
            print(f"  wrote {out}")


if __name__ == "__main__":
    main()
