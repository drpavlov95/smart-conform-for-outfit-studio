# Stiff-dihedral cell (plan Phase 0, review-valid config):
#   colored Gauss-Seidel (marco 10 machinery) + substepped continuation +
#   BILATERAL in-plane + residual telemetry. No props: no radial floor, no
#   shadow, no hull, no vanilla-conform anywhere. Support = Phase 1 law
#   (band <=0.3 bound in the AUTHORED pose, transported by the morphed
#   triangle frame).
#
# Interpretation contract (review): only runs whose dihedral residual
# plateaus (<~5 deg) may be read as evidence about stiffness.
#
# Usage:
#   py -3 pathb_stiffbend_spike.py [--bend-c 1e-6] [--stretch-c 1e-6]
#     [--substeps 48] [--iters 12] [--gravity 0.01]
#     [--out ../tmp/pathb/sb_bs.obj]
import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from pathb_metrics import load_obj
from pathb_shapematch_spike import (QuickClosest, band_targets, bind_band,
                                    exact_seam_groups)
from shell_spike import (UniformBodyCollider, greedy_constraint_batches,
                         hinge_geometry_scale, read_capture_front,
                         signed_dihedrals_local, wrap_angle,
                         xpbd_bending_colored_gs, xpbd_stretch_colored_gs)

CAP = Path("../corpus/fur_blacksmith_rm_heavy/drape_capture__Rley_3BA_Vanilla_-_Body_-_Blacksmith__Physics___BlackSmithTorso__RM_Heavy.drcap")


def build_hinges(tris):
    edge_map = {}
    for ti, t in enumerate(tris):
        for k in range(3):
            a, b = int(t[k]), int(t[(k + 1) % 3])
            edge_map.setdefault((min(a, b), max(a, b)), []).append(ti)
    hinges = []
    for (a, b), ts in edge_map.items():
        if len(ts) == 2:
            t0, t1 = tris[ts[0]], tris[ts[1]]
            c = [v for v in t0 if v not in (a, b)][0]
            d = [v for v in t1 if v not in (a, b)][0]
            hinges.append((a, b, int(c), int(d)))
    return np.array(hinges, dtype=np.int32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bend-c", type=float, default=1e-6)
    ap.add_argument("--stretch-c", type=float, default=1e-6)
    ap.add_argument("--substeps", type=int, default=48)
    ap.add_argument("--iters", type=int, default=12)
    ap.add_argument("--gravity", type=float, default=0.01)
    ap.add_argument("--strain", type=float, default=1.10)
    ap.add_argument("--clearance", type=float, default=0.15)
    ap.add_argument("--damping", type=float, default=0.9)
    ap.add_argument("--out", default="../tmp/pathb/sb_bs.obj")
    ap.add_argument("--init-flare", action="store_true",
                    help="start from the rigid-carry state: each free vert "
                         "displaced by its sector band anchor's displacement "
                         "(basin-of-attraction test, NOT a solve prop)")
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
    inv_mass = np.where(movable, 1.0, 0.0)   # rep-adjusted below
    print(f"n={n} band={int(band.sum())} movable={int(movable.sum())} "
          f"bendC={a.bend_c} stretchC={a.stretch_c} sub={a.substeps}x{a.iters}")

    binds = bind_band(base, band_idx, cap.ref_body_verts, cap.ref_body_tris)
    groups = exact_seam_groups(base)
    # WELDED TOPOLOGY: seam twins become ONE solver vertex (rep) - the
    # per-iteration averaging weld fought bending forever (identical resid
    # p95 ~32deg across ALL configs = structural, not physical). Constraints
    # are built on reps; twins are broadcast back at the end.
    rep = np.arange(n)
    for g in groups:
        r = g.min()
        for v in g:
            rep[v] = r
    tris_w = rep[np.asarray(cap.tris)]
    edges = rep[np.asarray(cap.edges, dtype=np.int32)]
    keep = edges[:, 0] != edges[:, 1]
    edges = np.unique(np.sort(edges[keep], axis=1), axis=0).astype(np.int32)
    e_rest = np.linalg.norm(base[edges[:, 0]] - base[edges[:, 1]], axis=1)
    hinges = build_hinges(tris_w)
    bad = ((hinges[:, 0] == hinges[:, 1]) | (hinges[:, 2] == hinges[:, 3]) |
           (hinges[:, 0] == hinges[:, 2]) | (hinges[:, 0] == hinges[:, 3]) |
           (hinges[:, 1] == hinges[:, 2]) | (hinges[:, 1] == hinges[:, 3]))
    hinges = hinges[~bad]
    h_scale = hinge_geometry_scale(base, hinges)
    h_rest = signed_dihedrals_local(base[hinges])
    isrep = np.zeros(n, bool)
    isrep[np.unique(rep)] = True
    inv_mass = np.where(isrep, inv_mass, 0.0)
    e_batches = greedy_constraint_batches(edges, n)
    h_batches = greedy_constraint_batches(hinges, n)
    print(f"edges={len(edges)} ({len(e_batches)} cores)  "
          f"hinges={len(hinges)} ({len(h_batches)} cores)")

    x = base.copy()
    if a.init_flare:
        # nearest band vert in (azimuth, z)-weighted metric = this sector's
        # anchor; carry the free vert rigidly with that anchor's FULL-morph
        # displacement. Pure initial condition; every constraint still runs.
        from scipy.spatial import cKDTree as _KD
        tgt_full = band_targets(binds, cap.body_verts, cap.body_tris)
        disp = tgt_full - base[band_idx]
        cx, cy = cap.body_verts[:, 0].mean(), cap.body_verts[:, 1].mean()
        def feat(v):
            th = np.arctan2(v[:, 1] - cy, v[:, 0] - cx)
            return np.column_stack([np.cos(th) * 8, np.sin(th) * 8,
                                    v[:, 2] * 0.5])
        kd = _KD(feat(base[band_idx]))
        _, nb = kd.query(feat(base))
        x = base + disp[nb]
        x[band_idx] = tgt_full
        print("init-flare: carried by sector band anchors "
              f"(mean disp {np.linalg.norm(disp, axis=1).mean():.2f})")
    if a.init_flare:
        a.substeps = 1          # the init IS the continuation: relax at t=1
    for sub in range(1, a.substeps + 1):
        t = 1.0 if a.init_flare else sub / a.substeps
        bv = cap.ref_body_verts + (cap.body_verts - cap.ref_body_verts) * t
        col = QuickClosest(bv, cap.body_tris)
        tgt = band_targets(binds, bv, cap.body_tris)
        x[band_idx] = tgt
        lam_e = np.zeros(len(edges))
        lam_h = np.zeros(len(hinges))
        mv = 0.0
        for it in range(a.iters):
            xp = x.copy()
            x[movable, 2] -= a.gravity
            xpbd_stretch_colored_gs(x, edges, e_batches, e_rest, inv_mass,
                                    lam_e, a.stretch_c)
            xpbd_bending_colored_gs(x, hinges, h_batches, h_rest, h_scale,
                                    inv_mass, lam_h, a.bend_c)
            # averaged tension strain limit
            d = x[edges[:, 1]] - x[edges[:, 0]]
            L = np.linalg.norm(d, axis=1)
            over = L > e_rest * a.strain
            if over.any():
                corr = d[over] * ((L[over] - e_rest[over] * a.strain) /
                                  np.maximum(L[over], 1e-9))[:, None]
                m0 = movable[edges[over, 0]].astype(float)
                m1 = movable[edges[over, 1]].astype(float)
                w = np.maximum(m0 + m1, 1e-9)
                acc = np.zeros_like(x)
                cnt = np.zeros(n)
                np.add.at(acc, edges[over, 0], corr * (m0 / w)[:, None])
                np.add.at(acc, edges[over, 1], -corr * (m1 / w)[:, None])
                np.add.at(cnt, edges[over, 0], m0)
                np.add.at(cnt, edges[over, 1], m1)
                nz = cnt > 0
                x[nz] += acc[nz] / cnt[nz][:, None]
            # contact (thickness, capped)
            nrm, sgap = col.query(x)
            pen = movable & ~inside & (sgap < a.clearance)
            if pen.any():
                push = np.minimum(a.clearance - sgap[pen], 0.25)
                x[pen] += nrm[pen] * push[:, None]
            # damping + trust region
            x[movable] = xp[movable] + a.damping * (x[movable] - xp[movable])
            dlt = x - xp
            dn = np.linalg.norm(dlt, axis=1)
            far = dn > 1.0
            if far.any():
                x[far] = xp[far] + dlt[far] * (1.0 / dn[far])[:, None]
            x[band_idx] = tgt
            x = x[rep]              # broadcast reps to twins
            x[band_idx] = tgt
            if not np.isfinite(x).all():
                raise SystemExit(f"DIVERGED sub={sub} it={it}")
            mv = float(np.linalg.norm(x - xp, axis=1)[movable].max())
        if sub % 8 == 0 or sub == a.substeps:
            resid = np.degrees(np.abs(wrap_angle(
                signed_dihedrals_local(x[hinges]) - h_rest)))
            hmov = movable[hinges[:, 0]] & movable[hinges[:, 1]]
            sr = (np.linalg.norm(x[edges[:, 1]] - x[edges[:, 0]], axis=1) /
                  np.maximum(e_rest, 1e-9))
            print(f"sub {sub:3}/{a.substeps}: maxMove={mv:.4f} "
                  f"dihedralResid med={np.median(resid[hmov]):.1f} "
                  f"p95={np.percentile(resid[hmov], 95):.1f} "
                  f"max={resid[hmov].max():.1f} deg  "
                  f"stretch p95={np.percentile(sr, 95):.3f}")

    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    with open(a.out, "w", newline="\n") as f:
        for v in x:
            f.write(f"v {v[0]:.6f} {v[1]:.6f} {v[2]:.6f}\n")
        for tvi in cap.tris:
            f.write(f"f {tvi[0]+1} {tvi[1]+1} {tvi[2]+1}\n")
    print(f"wrote {a.out}")

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
