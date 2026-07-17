# Path B leak map: WHERE does the body leak through the candidate cloth?
#
# Loads a capture (morphed body) + candidate OBJ, computes:
#   - per-vertex signed gap (penetrations listed by region, fixed/movable)
#   - per-face-sample penetrations (centroid + edge mids), clustered by z-band
#     and by azimuth (front/back/left/right/crotch)
#   - composite renders (body dark, garment light) from the user's angles:
#     leaks show as dark patches inside the garment silhouette.
#
# Usage:
#   py -3 tools/pathb_leakmap.py --capture CAP --candidate C.obj \
#       [--attach 0.3] [--out-prefix tmp/pathb/leak]
import argparse
from pathlib import Path

import numpy as np

from shell_spike import UniformBodyCollider, read_capture_front


def load_obj(path):
    vs, fs = [], []
    for line in open(path):
        if line.startswith("v "):
            vs.append([float(x) for x in line.split()[1:4]])
        elif line.startswith("f "):
            idx = [int(t.split("/")[0]) - 1 for t in line.split()[1:]]
            for k in range(1, len(idx) - 1):
                fs.append([idx[0], idx[k], idx[k + 1]])
    return np.asarray(vs, float), np.asarray(fs, np.int64)


def region(p, center):
    # ORIENTATION VERIFIED 2026-07-17 (pathb_view.Orientation): the character
    # FACES -y in this pipeline. Labels before that date were MIRRORED.
    dx, dy = p[0] - center[0], p[1] - center[1]
    if abs(dx) < 4.0 and abs(dy) < 4.0:
        return "crotch/central"
    if abs(dy) >= abs(dx):
        return "front" if dy < 0 else "back"
    return "x+" if dx > 0 else "x-"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", required=True)
    ap.add_argument("--candidate", required=True)
    ap.add_argument("--attach", type=float, default=0.3)
    ap.add_argument("--out-prefix", default="tmp/pathb/leak")
    a = ap.parse_args()

    cap = read_capture_front(Path(a.capture))
    cand, ctris = load_obj(a.candidate)
    n = min(len(cand), len(cap.base_verts))
    cand = cand[:n]
    col = UniformBodyCollider(cap.body_verts, cap.body_tris)
    base_col = UniformBodyCollider(cap.base_body_verts, cap.body_tris) \
        if hasattr(cap, "base_body_verts") else None
    tris = cap.tris

    authored_gap = (base_col.gaps(cap.base_verts) if base_col is not None
                    else col.gaps(cap.base_verts))
    fixed = authored_gap <= a.attach
    center = cap.body_verts.mean(0)

    vg = col.gaps(cand)
    pen = vg < 0.0
    print(f"# vertex pens: {int(pen.sum())} worst={vg.min():.3f} "
          f"(fixed {int((pen & fixed).sum())} / movable {int((pen & ~fixed).sum())})")
    order = np.argsort(vg)
    print("worst 12 vertex pens:")
    for i in order[:12]:
        if vg[i] >= 0:
            break
        p = cand[i]
        print(f"  v{i} gap={vg[i]:+.3f} z={p[2]:.1f} {region(p, center)} "
              f"{'FIXED' if fixed[i] else 'movable'} authored={authored_gap[i]:.2f}")

    # face samples: centroid + edge midpoints
    tri = cand[tris]
    samples = np.concatenate([
        tri.mean(1),
        0.5 * (tri[:, 0] + tri[:, 1]),
        0.5 * (tri[:, 1] + tri[:, 2]),
        0.5 * (tri[:, 2] + tri[:, 0]),
    ])
    sg = col.gaps(samples)
    spen = sg < 0.0
    tid = np.tile(np.arange(len(tris)), 4)
    print(f"\n# face-sample pens: {int(spen.sum())} worst={sg.min():.3f}")
    bands = {}
    for k in np.nonzero(spen)[0]:
        p = samples[k]
        t = tris[tid[k]]
        cls = "FIXED" if fixed[t].all() else ("mixed" if fixed[t].any() else "movable")
        key = (int(p[2] // 5) * 5, region(p, center), cls)
        d = bands.setdefault(key, [0, 0.0])
        d[0] += 1
        d[1] = min(d[1], sg[k])
    print("face pens by (z-band, region, class):")
    for key in sorted(bands, key=lambda k: bands[k][1]):
        c, w = bands[key]
        print(f"  z={key[0]:>3} {key[1]:<14} {key[2]:<8} n={c:<4} worst={w:+.3f}")

    # composite renders: painter's algorithm, body dark / garment light,
    # penetrating garment faces flagged red-ish (dark gray in grayscale).
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from mpl_toolkits.mplot3d.art3d import Poly3DCollection
    except Exception as e:
        print(f"(render skipped: {e})")
        return

    tri_pen = np.zeros(len(tris), bool)
    for k in np.nonzero(spen)[0]:
        tri_pen[tid[k]] = True

    views = [("back", -90), ("back_left", -135), ("left", 180),
             ("front", 90), ("right", 0)]
    for name, azim in views:
        fig = plt.figure(figsize=(7, 10))
        ax = fig.add_subplot(111, projection="3d")
        bt = cap.body_verts[cap.body_tris]
        pc = Poly3DCollection(bt, facecolors=(0.25, 0.2, 0.2),
                              edgecolors="none")
        ax.add_collection3d(pc)
        gt_ = cand[tris]
        colors = np.tile((0.85, 0.82, 0.75, 1.0), (len(tris), 1))
        colors[tri_pen] = (0.9, 0.2, 0.2, 1.0)
        gc = Poly3DCollection(gt_, facecolors=colors, edgecolors="none")
        ax.add_collection3d(gc)
        allv = np.concatenate([cap.body_verts, cand])
        c = allv.mean(0)
        r = (allv.max(0) - allv.min(0)).max() * 0.42
        ax.set_xlim(c[0] - r, c[0] + r)
        ax.set_ylim(c[1] - r, c[1] + r)
        ax.set_zlim(c[2] - r, c[2] + r)
        ax.view_init(elev=5, azim=azim)
        ax.set_axis_off()
        ax.set_box_aspect((1, 1, 1))
        out = f"{a.out_prefix}_{name}.png"
        fig.savefig(out, dpi=110, bbox_inches="tight")
        plt.close(fig)
        print(f"wrote {out}")


if __name__ == "__main__":
    main()
