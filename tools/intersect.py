#!/usr/bin/env python3
"""Which surface goes through which - answered exactly, not inferred.

The viewer's `pen` mode asks "is this vertex behind some surface", by nearest
face and the sign of the offset. That is fine for a colour and useless for the
question that decides the fix: is the skirt passing through the CLOAK, through
the PANTS, or through ITSELF? Contact between two garments and self-collision of
one garment are different problems with very different costs, and a sign test
cannot tell them apart. It cannot see self-intersection at all - the nearest
face to a vertex is its own neighbour, so the sign is always ~0.

So this does the real test: triangle against triangle. A pair intersects when an
edge of one crosses the other (Möller-Trumbore per edge), which covers every
case here except exact coplanar overlap - degenerate, and not what tears a
garment. Adjacent triangles are excluded by shared vertices, so a fold touching
itself does not count until it actually crosses.

Output is a matrix of who crosses whom, with SELF on the diagonal.
"""
from __future__ import annotations
import argparse
from pathlib import Path

import numpy as np
from scipy.spatial import cKDTree


def load_obj(path):
    vs, fs = [], []
    for line in open(path):
        if line.startswith("v "):
            vs.append([float(x) for x in line.split()[1:4]])
        elif line.startswith("f "):
            i = [int(t.split("/")[0]) - 1 for t in line.split()[1:]]
            for k in range(1, len(i) - 1):
                fs.append([i[0], i[k], i[k + 1]])
    return np.array(vs, np.float64), np.array(fs, np.int32)


def edges_hit_tris(o, d, tv0, tv1, tv2, eps=1e-9):
    """Möller-Trumbore for many (origin, direction) segments against many tris.

    Returns a boolean: the segment (o -> o+d) crosses the triangle strictly
    inside its span, so touching endpoints do not register.
    """
    e1, e2 = tv1 - tv0, tv2 - tv0
    p = np.cross(d, e2)
    det = np.einsum("ij,ij->i", e1, p)
    ok = np.abs(det) > eps
    inv = np.zeros_like(det)
    inv[ok] = 1.0 / det[ok]
    t0 = o - tv0
    u = np.einsum("ij,ij->i", t0, p) * inv
    q = np.cross(t0, e1)
    v = np.einsum("ij,ij->i", d, q) * inv
    t = np.einsum("ij,ij->i", e2, q) * inv
    return ok & (u >= 0) & (u <= 1) & (v >= 0) & (u + v <= 1) & (t > 1e-6) & (t < 1 - 1e-6)


def pair_intersections(vA, fA, vB, fB, same=False, chunk=200000):
    """Indices (iA, iB) of intersecting triangle pairs."""
    cA = vA[fA].mean(axis=1)
    cB = vB[fB].mean(axis=1)
    rA = np.linalg.norm(vA[fA] - cA[:, None, :], axis=2).max(axis=1)
    rB = np.linalg.norm(vB[fB] - cB[:, None, :], axis=2).max(axis=1)
    reach = float(rA.max() + rB.max())
    pairs = cKDTree(cA).query_ball_tree(cKDTree(cB), r=reach)
    ia, ib = [], []
    for i, js in enumerate(pairs):
        for j in js:
            if same and j <= i:
                continue
            ia.append(i); ib.append(j)
    if not ia:
        return np.empty(0, int), np.empty(0, int)
    ia = np.array(ia); ib = np.array(ib)
    # cheap reject: bounding spheres must actually reach each other
    keep = np.linalg.norm(cA[ia] - cB[ib], axis=1) <= (rA[ia] + rB[ib])
    ia, ib = ia[keep], ib[keep]
    if same and len(ia):
        # adjacency: triangles sharing any vertex are neighbours, not crossings
        sh = np.zeros(len(ia), bool)
        for k in range(3):
            for m in range(3):
                sh |= (fA[ia][:, k] == fB[ib][:, m])
        ia, ib = ia[~sh], ib[~sh]

    hit_a, hit_b = [], []
    for s in range(0, len(ia), chunk):
        qa, qb = ia[s:s + chunk], ib[s:s + chunk]
        A = vA[fA[qa]]
        B = vB[fB[qb]]
        hit = np.zeros(len(qa), bool)
        for k in range(3):                      # edges of A against B
            o = A[:, k]; d = A[:, (k + 1) % 3] - o
            hit |= edges_hit_tris(o, d, B[:, 0], B[:, 1], B[:, 2])
        for k in range(3):                      # edges of B against A
            o = B[:, k]; d = B[:, (k + 1) % 3] - o
            hit |= edges_hit_tris(o, d, A[:, 0], A[:, 1], A[:, 2])
        hit_a.append(qa[hit]); hit_b.append(qb[hit])
    if not hit_a:                      # no candidate pairs survived the reject
        return np.empty(0, int), np.empty(0, int)
    return np.concatenate(hit_a), np.concatenate(hit_b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--obj", action="append", required=True,
                    help="rotulo=arquivo.obj (repita; inclua o corpo se quiser)")
    a = ap.parse_args()

    meshes = []
    for spec in a.obj:
        label, _, path = spec.partition("=")
        v, f = load_obj(path)
        meshes.append((label, v, f))
        print(f"{label:>14s}: {len(v):6d} verts, {len(f):6d} tris")

    print(f"\n{'':>14s} " + " ".join(f"{m[0][:11]:>11s}" for m in meshes))
    total_self = total_cross = 0
    for i, (la, va, fa) in enumerate(meshes):
        row = []
        for j, (lb, vb, fb) in enumerate(meshes):
            if j < i:
                row.append(f"{'':>11s}")
                continue
            ha, _hb = pair_intersections(va, fa, vb, fb, same=(i == j))
            n = len(ha)
            if i == j:
                total_self += n
                row.append(f"{('SELF ' + str(n)) if n else '.':>11s}")
            else:
                total_cross += n
                row.append(f"{n if n else '.':>11}")
        print(f"{la[:14]:>14s} " + " ".join(row))
    print(f"\ntriangulos cruzando: entre pecas {total_cross}, consigo mesmo {total_self}")


if __name__ == "__main__":
    main()
