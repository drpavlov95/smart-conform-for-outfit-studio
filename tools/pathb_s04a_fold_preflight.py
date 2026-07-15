#!/usr/bin/env python3
"""S0.4a: kinematic preflight of the hinge-fold gravity re-pose candidate.

Measurement-only. No solver. This asks whether the IDEAL outcome of the
user's plate-fold pass (every hanging vertex settles PLUMB below its support
boundary point, preserving authored arc length, stopping at body contact) can
even reach the sculpt - before any implementation.

Ideal-fold model (upper bound of what the pass could do, ignoring lateral
coherence): for each movable vertex, find its nearest ANCHORED vertex by
geodesic distance over the authored edge graph (edge weights = authored rest
lengths - inextensibility). Its plumb target hangs straight down from that
anchor's CANDIDATE position (the fold runs late, on the corrected result) by
the geodesic distance. The path from the candidate position to the plumb
target is swept with bisection against the morphed-body collider so the fold
"stops at contact" (the pendulum settles at vertical from either side, so
tucked-in cloth swings OUT to plumb and flared cloth swings IN).

Necessity trio per garment/band:
  1. does the DEFECT violate plumb? (candidate far from its plumb target)
  2. does the SCULPT satisfy plumb? (sculpt near the plumb target)
  3. is there margin? (fold moves the candidate meaningfully toward the sculpt)

Usage:
  py -3 tools/pathb_s04a_fold_preflight.py --capture CAP --candidate PROD.obj \
      --sculpt GT.obj [--clearance 0.20] [--fold-out OUT.obj]
"""
from __future__ import annotations

import argparse
import heapq
from pathlib import Path

import numpy as np

from shell_spike import (
    CaptureFront,
    UniformBodyCollider,
    fitted_drape_signal,
    read_capture_front,
)


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


def movable_mask(cap: CaptureFront) -> tuple[np.ndarray, np.ndarray]:
    """The spike's continuous mobility: u>0 movable, u<=0 anchored."""
    drape = fitted_drape_signal(cap)
    u = np.clip((drape - 0.05) / 0.50, 0.0, 1.0)
    u = u * u * (3.0 - 2.0 * u)
    return u, u <= 1.0e-12


def geodesic_from_anchors(n, edges, rest, anchored):
    """Dijkstra from the anchored set; returns (distance, anchor_index)."""
    adj: list[list[tuple[int, float]]] = [[] for _ in range(n)]
    for (a, b), r in zip(edges, rest):
        w = max(float(r), 1.0e-9)
        adj[int(a)].append((int(b), w))
        adj[int(b)].append((int(a), w))
    dist = np.full(n, np.inf)
    anchor = np.full(n, -1, dtype=np.int64)
    heap = []
    for i in np.flatnonzero(anchored):
        dist[i] = 0.0
        anchor[i] = i
        heap.append((0.0, int(i)))
    heapq.heapify(heap)
    while heap:
        d, i = heapq.heappop(heap)
        if d > dist[i] + 1.0e-12:
            continue
        for j, w in adj[i]:
            nd = d + w
            if nd < dist[j] - 1.0e-12:
                dist[j] = nd
                anchor[j] = anchor[i]
                heapq.heappush(heap, (nd, j))
    return dist, anchor


def contact_stop(collider, start, target, clearance, steps=8):
    """Last point on start->target with gap >= clearance (bisection sweep)."""
    def clear(p):
        return float(collider.gaps(p[None, :])[0]) >= clearance
    if clear(target):
        return target
    lo, hi = 0.0, 1.0  # lo = clear fraction, hi = blocked
    if not clear(start):
        return start   # already in contact where it stands: the fold cannot move it
    for _ in range(steps):
        mid = 0.5 * (lo + hi)
        if clear(start + mid * (target - start)):
            lo = mid
        else:
            hi = mid
    return start + lo * (target - start)


def band_table(z, mask, *fields, lo=None, hi=None, step=6):
    zlo = int(np.floor((lo if lo is not None else z[mask].min())))
    zhi = int(np.ceil((hi if hi is not None else z[mask].max())))
    rows = []
    for b in range(zlo, zhi, step):
        m = mask & (z >= b) & (z < b + step)
        if m.sum() < 5:
            continue
        rows.append((b, int(m.sum()), *[float(np.mean(f[m])) for f in fields]))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", required=True)
    ap.add_argument("--candidate", required=True)
    ap.add_argument("--sculpt", required=True)
    ap.add_argument("--clearance", type=float, default=0.20)
    ap.add_argument("--contact-anchor", type=float, default=0.60,
                    help="candidate verts within this gap of the morphed body are "
                         "SUPPORT (the fold hangs from the last contact line, e.g. "
                         "the butt apex), not just preset anchors. <=0 disables.")
    ap.add_argument("--fold-out")
    a = ap.parse_args()

    cap = read_capture_front(Path(a.capture))
    cand = load_obj(a.candidate)
    sculpt = load_obj(a.sculpt)
    n = len(cap.base_verts)
    assert len(cand) == n and len(sculpt) == n, "vert count mismatch"

    u, anchored = movable_mask(cap)
    collider = UniformBodyCollider(cap.body_verts, cap.body_tris)
    n_preset = int(anchored.sum())
    if a.contact_anchor > 0.0:
        # the user's fold anchors at the last CONTACT line (Codex's coherent
        # support boundary), not only at preset anchors: cloth resting on the
        # body (butt apex, shoulders, legs) is support for everything below it.
        gaps = collider.gaps(cand)
        anchored = anchored | (gaps <= a.contact_anchor)
    dist, anchor = geodesic_from_anchors(n, cap.edges, cap.edge_rest, anchored)
    fold_zone = (u >= 0.55) & ~anchored & np.isfinite(dist) & (anchor >= 0)
    down = np.array([0.0, 0.0, -1.0])

    folded = cand.copy()
    idx = np.flatnonzero(fold_zone)
    for i in idx:
        base_anchor = cand[anchor[i]]
        plumb = base_anchor + down * dist[i]
        folded[i] = contact_stop(collider, cand[i], plumb, a.clearance)

    # necessity trio, measured on the fold zone
    plumb_res_cand = np.linalg.norm(folded - cand, axis=1)          # how far the fold MOVES the defect
    d_cand = np.linalg.norm(cand - sculpt, axis=1)
    d_fold = np.linalg.norm(folded - sculpt, axis=1)

    name = Path(a.capture).name.split("___")[-1].replace(".drcap", "")
    print(f"# S0.4a fold preflight  {name}  foldZone={len(idx)}/{n} "
          f"anchored={int(anchored.sum())} (preset={n_preset}) clearance={a.clearance} "
          f"contactAnchor={a.contact_anchor}")
    fz = fold_zone
    print(f"defeito viola plumb?   fold move on zone: mean={plumb_res_cand[fz].mean():.3f} "
          f"p90={np.percentile(plumb_res_cand[fz], 90):.3f} max={plumb_res_cand[fz].max():.3f}")
    print(f"indexado vs sculpt na zona:  DEFEITO={d_cand[fz].mean():.4f}  "
          f"FOLD-IDEAL={d_fold[fz].mean():.4f}  delta={(d_fold[fz]-d_cand[fz]).mean():+.4f}")
    closer = int((d_fold[fz] < d_cand[fz] - 0.05).sum())
    farther = int((d_fold[fz] > d_cand[fz] + 0.05).sum())
    print(f"verts que APROXIMAM do sculpt: {closer}   que AFASTAM: {farther}   "
          f"neutros: {int(fz.sum()) - closer - farther}")
    print("\nbandas z (n, moveu, defeito->sculpt, fold->sculpt):")
    for b, cnt, mv, dc, df in band_table(cap.base_verts[:, 2], fz,
                                         plumb_res_cand, d_cand, d_fold):
        flag = "  <-- fold ajuda" if df < dc - 0.1 else ("  <-- fold PIORA" if df > dc + 0.1 else "")
        print(f"  z{b:3d}: n={cnt:4d} moveu={mv:6.3f} defeito={dc:6.3f} fold={df:6.3f}{flag}")

    if a.fold_out:
        write_obj(a.fold_out, folded, cap.tris)
        print(f"\nwrote {a.fold_out}")


if __name__ == "__main__":
    main()
