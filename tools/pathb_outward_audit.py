#!/usr/bin/env python3
"""Per-vertex audit of OUTWARD displacement, and which stage produced it.

Written to answer a specific hypothesis (2026-07-26): that the visible bumps are
vertices pushed AWAY from the body with no geometry underneath to justify it -
"magnetic repulsion". Global metrics cannot see this. A bump is ~15 vertices in a
20k mesh, so mean displacement, mean stretch and total penetration count all stay
flat while the shape is visibly wrong.

The metric that matters here is `unsupportedOutward`: a vertex that moved outward
along the body normal by more than a threshold while sitting too far from the body
for any contact to have pushed it. Contact can only push a vertex that is touching
something. An outward move with clearance to spare had no physical cause.

Feed it the stage dumps (DRAPE_STAGE_DUMP=1) plus the body OBJ, and it reports the
first stage that created each offender rather than only the end state.
"""

from __future__ import annotations

import argparse
import math
import os
import sys
from pathlib import Path

import numpy as np

# Pipeline order. Missing files are skipped, so this also works on a legacy run
# that only dumped the tail.
STAGES = [
    ("00_vanilla", "conform vanilla (entrada)"),
    ("00b_shell", "motor (shell publica)"),
    ("01_panel_flatten", "panel-flatten"),
    ("02_hem_smooth", "hem-smooth"),
    ("03_anticlip", "anti-clip"),
    ("04_stretch_cap", "stretch-cap"),
    ("05_body_clearance", "body-clearance"),
    ("06_selfcol_tuck", "selfcol-tuck"),
    ("07_post_prox_cap", "post-prox-cap"),
    ("08_body_evict", "body-evict"),
    ("09_final", "final"),
]


def read_obj_verts(path: Path) -> np.ndarray:
    v = []
    with open(path, "r") as f:
        for line in f:
            if line.startswith("v "):
                p = line.split()
                v.append((float(p[1]), float(p[2]), float(p[3])))
    return np.asarray(v, dtype=np.float64)


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


def vertex_normals(v: np.ndarray, tris: np.ndarray) -> np.ndarray:
    """Area-weighted vertex normals - the outward direction reference."""
    n = np.zeros_like(v)
    a, b, c = v[tris[:, 0]], v[tris[:, 1]], v[tris[:, 2]]
    fn = np.cross(b - a, c - a)
    for k in range(3):
        np.add.at(n, tris[:, k], fn)
    ln = np.linalg.norm(n, axis=1, keepdims=True)
    ln[ln < 1e-12] = 1.0
    return n / ln


def nearest_body(query: np.ndarray, body: np.ndarray, bnrm: np.ndarray, chunk: int = 512):
    """Nearest body VERTEX per query point, plus its normal and signed gap.

    Vertex-nearest rather than triangle-nearest: this is a diagnostic, and on a
    body mesh this dense the difference does not change which vertices are
    flagged. Chunked so a 20k x 20k distance matrix is never materialised.
    """
    idx = np.empty(len(query), dtype=np.int64)
    dist = np.empty(len(query), dtype=np.float64)
    for s in range(0, len(query), chunk):
        e = min(s + chunk, len(query))
        d = np.linalg.norm(query[s:e, None, :] - body[None, :, :], axis=2)
        i = np.argmin(d, axis=1)
        idx[s:e] = i
        dist[s:e] = d[np.arange(e - s), i]
    nrm = bnrm[idx]
    sign = np.einsum("ij,ij->i", query - body[idx], nrm)
    gap = np.where(sign < 0.0, -dist, dist)
    return gap, nrm


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--shape", required=True, help="shape name as used in the dump filenames")
    ap.add_argument("--body", required=True, type=Path, help="body OBJ (--body-obj)")
    ap.add_argument("--dump-dir", type=Path,
                    default=Path(os.environ.get("TEMP", ".")))
    ap.add_argument("--outward", type=float, default=1.0,
                    help="outward move (units) above which a vertex is an offender")
    ap.add_argument("--contact-range", type=float, default=0.5,
                    help="body gap under which contact could plausibly have pushed it")
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--topology", type=Path, default=None,
                    help="OBJ com as FACES da peça (--result-obj) para agrupar ofensores")
    args = ap.parse_args()

    safe = "".join(ch if (ch.isalnum() or ch in "_-") else "_" for ch in args.shape)
    stages = []
    for key, label in STAGES:
        p = args.dump_dir / f"drape_stage_{key}__{safe}.obj"
        if p.exists():
            stages.append((key, label, read_obj_verts(p)))
    if len(stages) < 2:
        print(f"precisa de >=2 dumps para '{args.shape}' em {args.dump_dir}\n"
              f"rode com DRAPE_STAGE_DUMP=1", file=sys.stderr)
        return 2

    n = len(stages[0][2])
    if any(len(v) != n for _, _, v in stages):
        print("dumps com contagens de vértice diferentes - abortando", file=sys.stderr)
        return 2

    body, btris = read_obj(args.body)
    bnrm = vertex_normals(body, btris)

    print(f"peça '{args.shape}': {n} vértices, {len(stages)} estágios, corpo {len(body)} verts\n")

    # MEASURE THE GAP AT EVERY STAGE, not the displacement projected on a fixed
    # normal. Projection was the first thing tried here and it lies: on a curved
    # body a purely TANGENTIAL slide reads as a large outward move. It reported
    # Cloak vertex 400 as "3.21u outward" while that vertex's clearance went from
    # 0.87 to 1.14 - it slid along the hip, it did not fly off. Gap change is
    # invariant to that.
    gaps = []
    for key, label, v in stages:
        g, _ = nearest_body(v, body, bnrm)
        gaps.append(g)
    gap0, gapN = gaps[0], gaps[-1]

    print(f"{'estágio':<22}{'move médio':>12}{'move máx':>11}{'dgap médio':>13}"
          f"{'dgap máx':>11}{'verts +1u gap':>15}")
    print("-" * 84)
    per_stage_gap = []
    for i in range(1, len(stages)):
        d = np.linalg.norm(stages[i][2] - stages[i - 1][2], axis=1)
        dg = gaps[i] - gaps[i - 1]
        per_stage_gap.append(dg)
        print(f"{stages[i][1]:<22}{d.mean():>12.3f}{d.max():>11.3f}"
              f"{dg.mean():>13.3f}{dg.max():>11.3f}{int((dg > 1.0).sum()):>15}")

    total_mag = np.linalg.norm(stages[-1][2] - stages[0][2], axis=1)
    gapRise = gapN - gap0

    # THE METRIC. The vertex ended up materially FARTHER from the body than it
    # started, from a start pose already too clear for contact to have reached
    # it. Contact can only push what it touches; a gap that grows from open air
    # had no physical cause.
    unsupported = (gapRise > args.outward) & (gap0 > args.contact_range)
    print(f"\nunsupportedOutward: {int(unsupported.sum())} de {n} vértices "
          f"(gap subiu > {args.outward}u, partindo de gap > {args.contact_range}u)")
    print(f"deslocamento total: médio {total_mag.mean():.3f}  máx {total_mag.max():.3f}")
    print(f"gap: médio {gap0.mean():.2f} -> {gapN.mean():.2f}   "
          f"mín {gap0.min():.2f} -> {gapN.min():.2f}")

    if unsupported.any():
        order = [i for i in np.argsort(-gapRise) if unsupported[i]][: args.top]
        print(f"\npiores {len(order)} (estágio = onde o gap mais subiu):")
        print(f"{'vert':>7}{'dgap':>9}{'gap inicial':>13}{'gap final':>11}  estágio dominante")
        for vi in order:
            contribs = [(per_stage_gap[k][vi], stages[k + 1][1])
                        for k in range(len(per_stage_gap))]
            best = max(contribs, key=lambda t: t[0])
            print(f"{vi:>7}{gapRise[vi]:>9.2f}{gap0[vi]:>13.2f}{gapN[vi]:>11.2f}"
                  f"  {best[1]} (+{best[0]:.2f})")

        # THE DISCRIMINATOR. 15 offenders scattered over a 2300-vertex mesh is
        # numerical noise; 15 offenders sharing edges is a visible bump. Without
        # this the count alone cannot tell the two apart, and the whole question
        # ("are there bumps?") turns on which one it is.
        if args.topology is not None and args.topology.exists():
            _, gtris = read_obj(args.topology)
            adj = {}
            for t in gtris:
                for k in range(3):
                    a, b = int(t[k]), int(t[(k + 1) % 3])
                    adj.setdefault(a, set()).add(b)
                    adj.setdefault(b, set()).add(a)
            offs = set(int(i) for i in np.flatnonzero(unsupported))
            seen, comps = set(), []
            for start in offs:
                if start in seen:
                    continue
                stack, comp = [start], []
                seen.add(start)
                while stack:
                    cur = stack.pop()
                    comp.append(cur)
                    for nb in adj.get(cur, ()):
                        if nb in offs and nb not in seen:
                            seen.add(nb)
                            stack.append(nb)
                comps.append(sorted(comp))
            comps.sort(key=len, reverse=True)
            sizes = [len(c) for c in comps]
            print(f"\nagrupamento dos ofensores: {len(comps)} grupo(s), "
                  f"tamanhos {sizes[:8]}{' ...' if len(sizes) > 8 else ''}")
            print("  isolados (grupo de 1) = ruído numérico; "
                  "grupo grande e conexo = bolha visível")
            if comps and len(comps[0]) >= 3:
                print(f"  MAIOR GRUPO: {len(comps[0])} verts conexos -> {comps[0][:12]}")

        print("\nresponsabilidade por estágio (soma do dgap positivo dos ofensores):")
        blame = [(float(per_stage_gap[k][unsupported].clip(min=0).sum()), stages[k + 1][1])
                 for k in range(len(per_stage_gap))]
        for amount, label in sorted(blame, reverse=True):
            if amount > 1e-6:
                print(f"  {label:<24}{amount:>10.1f}u")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
