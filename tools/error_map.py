# Per-vert error vs a reference mesh: stats by z-band + heatmap render.
# Usage: error_map.py ref.obj test.obj out.png [view]
import sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.collections import PolyCollection

def load(p):
    vs, fs = [], []
    for l in open(p):
        if l.startswith("v "):
            vs.append([float(x) for x in l.split()[1:4]])
        elif l.startswith("f "):
            idx = [int(t.split("/")[0]) - 1 for t in l.split()[1:]]
            for k in range(1, len(idx) - 1):
                fs.append([idx[0], idx[k], idx[k + 1]])
    return np.array(vs), np.array(fs, dtype=int)

VIEWS = {
    "front": (np.array([1.0, 0, 0]), np.array([0, -1.0, 0])),
    "back":  (np.array([-1.0, 0, 0]), np.array([0, 1.0, 0])),
    "left":  (np.array([0, -1.0, 0]), np.array([-1.0, 0, 0])),
    "right": (np.array([0, 1.0, 0]), np.array([1.0, 0, 0])),
}

ref, fs = load(sys.argv[1])
test, _ = load(sys.argv[2])
err = np.linalg.norm(test - ref, axis=1)
print(f"error vs reference: mean={err.mean():.3f} p50={np.percentile(err,50):.3f} "
      f"p95={np.percentile(err,95):.3f} max={err.max():.3f}")
zmin, zmax = ref[:, 2].min(), ref[:, 2].max()
print(f"{'z-band':>12} {'n':>5} {'mean':>6} {'p95':>6} {'max':>6}")
for zlo in np.arange(np.floor(zmin), zmax, 10):
    m = (ref[:, 2] >= zlo) & (ref[:, 2] < zlo + 10)
    if m.sum() == 0: continue
    print(f"{zlo:6.0f}-{zlo+10:<5.0f} {m.sum():5d} {err[m].mean():6.2f} "
          f"{np.percentile(err[m],95):6.2f} {err[m].max():6.2f}")
top = np.argsort(-err)[:8]
for vi in top:
    print(f"  worst v{vi}: err={err[vi]:.2f} at ({ref[vi][0]:6.2f},{ref[vi][1]:6.2f},{ref[vi][2]:6.2f})")

if len(sys.argv) > 3:
    views = sys.argv[4:] or ["back", "front"]
    fig, axes = plt.subplots(1, len(views), figsize=(8 * len(views), 10))
    if len(views) == 1: axes = [axes]
    vmax = max(np.percentile(err, 99), 0.5)
    for ax, view in zip(axes, views):
        right, fwd = VIEWS[view]
        up = np.array([0, 0, 1.0])
        x2 = test @ right; y2 = test @ up; depth = test @ fwd
        order = np.argsort(depth[fs].mean(axis=1))
        polys = np.stack([x2[fs], y2[fs]], axis=-1)[order]
        cols = plt.cm.inferno(np.clip(err[fs].mean(axis=1)[order] / vmax, 0, 1))
        ax.add_collection(PolyCollection(polys, facecolors=cols, edgecolors="none"))
        ax.set_xlim(x2.min() - 2, x2.max() + 2); ax.set_ylim(y2.min() - 2, y2.max() + 2)
        ax.set_aspect("equal"); ax.axis("off"); ax.set_title(f"{view} (max color={vmax:.1f}u)")
    fig.tight_layout(); fig.savefig(sys.argv[3], dpi=100, facecolor="white")
    print("wrote", sys.argv[3])
