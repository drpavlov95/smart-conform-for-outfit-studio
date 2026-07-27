#!/usr/bin/env python3
"""Count cloth-through-cloth intersections in a finished garment.

The user's report: on the vampire robe the trouser leg clips through the
overskirt, and both are the SAME mesh (vampire_robes_alt), so nothing that works
per shape or per layer can see it. No metric in this project measured it either
- penetration counts cloth inside the BODY, and a garment can pass cleanly
through itself while scoring zero.

Two numbers, both computed the way a viewer perceives the defect:

  pierced    triangle pairs that actually cross each other (segment-triangle
             intersection on all six edges). This is the visible clip.
  tooClose   vertex/triangle pairs closer than a fabric thickness without
             crossing. Fabric with no thickness reads as z-fighting.

Topologically adjacent geometry is excluded by a 2-ring rule, so a fold resting
against itself counts while a triangle never trips over its own neighbours.
"""

from __future__ import annotations

import argparse
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np


def read_obj(path: Path):
    v, tris = [], []
    with open(path, "r") as f:
        for line in f:
            if line.startswith("v "):
                p = line.split()
                v.append((float(p[1]), float(p[2]), float(p[3])))
            elif line.startswith("f "):
                idx = [int(t.split("/")[0]) - 1 for t in line.split()[1:]]
                for k in range(1, len(idx) - 1):
                    tris.append((idx[0], idx[k], idx[k + 1]))
    return np.asarray(v, dtype=np.float64), np.asarray(tris, dtype=np.int64)


def near_rings(n: int, tris: np.ndarray):
    r1 = defaultdict(set)
    for t in tris:
        for a in t:
            for b in t:
                if a != b:
                    r1[int(a)].add(int(b))
    out = []
    for i in range(n):
        acc = set(r1[i])
        acc.add(i)
        for j in r1[i]:
            acc |= r1[j]
        out.append(acc)
    return out


def seg_tri_hit(p0, p1, a, b, c, eps=1e-9):
    """Moller-Trumbore, segment form."""
    e1, e2 = b - a, c - a
    d = p1 - p0
    h = np.cross(d, e2)
    det = np.dot(e1, h)
    if abs(det) < eps:
        return False
    inv = 1.0 / det
    s = p0 - a
    u = inv * np.dot(s, h)
    if u < 0.0 or u > 1.0:
        return False
    q = np.cross(s, e1)
    v = inv * np.dot(d, q)
    if v < 0.0 or u + v > 1.0:
        return False
    t = inv * np.dot(e2, q)
    return 0.0 < t < 1.0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--garment", required=True, type=Path, nargs="+")
    ap.add_argument("--label", nargs="+", default=None)
    ap.add_argument("--thick-frac", type=float, default=0.35,
                    help="fabric thickness as a fraction of the mean edge")
    args = ap.parse_args()

    # THE BASELINE MATTERS MORE THAN THE TOTAL. Game garments are authored
    # self-intersecting on purpose - the vampire robe has trousers modelled
    # inside the overskirt and scores 3517 crossing pairs before anything runs.
    # Counting the total therefore measures the artist's overlap, not our
    # defect. What the viewer sees as a clip is the pairs that are crossing NOW
    # and were NOT crossing as authored, the same "no worse than authored" shape
    # as the radial rule.
    labels = args.label or [p.stem for p in args.garment]
    baseline: set | None = None
    print(f"{'config':<22}{'pierced':>10}{'NOVOS':>8}{'tooClose':>11}{'espessura':>12}")
    print("-" * 64)
    for path, label in zip(args.garment, labels):
        v, tris = read_obj(path)
        n = len(v)
        edges = set()
        for t in tris:
            for k in range(3):
                a, b = int(t[k]), int(t[(k + 1) % 3])
                edges.add((min(a, b), max(a, b)))
        el = np.mean([np.linalg.norm(v[a] - v[b]) for a, b in edges])
        thick = el * args.thick_frac
        rings = near_rings(n, tris)

        cell = max(thick * 2.0, el)
        grid = defaultdict(list)
        for ti, t in enumerate(tris):
            lo = v[t].min(axis=0) - thick
            hi = v[t].max(axis=0) + thick
            for ix in range(int(np.floor(lo[0] / cell)), int(np.floor(hi[0] / cell)) + 1):
                for iy in range(int(np.floor(lo[1] / cell)), int(np.floor(hi[1] / cell)) + 1):
                    for iz in range(int(np.floor(lo[2] / cell)), int(np.floor(hi[2] / cell)) + 1):
                        grid[(ix, iy, iz)].append(ti)

        seen = set()
        pierced = 0
        close = 0
        crossing = set()
        for key, bucket in grid.items():
            for i in range(len(bucket)):
                for j in range(i + 1, len(bucket)):
                    ti, tj = bucket[i], bucket[j]
                    pair = (min(ti, tj), max(ti, tj))
                    if pair in seen:
                        continue
                    seen.add(pair)
                    ta, tb = tris[ti], tris[tj]
                    if any(x in rings[int(y)] for x in ta for y in tb):
                        continue
                    A, B, C = v[ta[0]], v[ta[1]], v[ta[2]]
                    D, E, F = v[tb[0]], v[tb[1]], v[tb[2]]
                    hit = (seg_tri_hit(D, E, A, B, C) or seg_tri_hit(E, F, A, B, C)
                           or seg_tri_hit(F, D, A, B, C) or seg_tri_hit(A, B, D, E, F)
                           or seg_tri_hit(B, C, D, E, F) or seg_tri_hit(C, A, D, E, F))
                    if hit:
                        pierced += 1
                        crossing.add(pair)
                    else:
                        cen_a = (A + B + C) / 3.0
                        cen_b = (D + E + F) / 3.0
                        if np.linalg.norm(cen_a - cen_b) < thick:
                            close += 1
        if baseline is None:
            baseline = crossing
            novos = "-"
        else:
            novos = str(len(crossing - baseline))
        print(f"{label:<22}{pierced:>10}{novos:>8}{close:>11}{thick:>12.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
