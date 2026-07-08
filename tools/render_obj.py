# Shaded OBJ comparison renders for eyeballing solver output.
# Usage: py -3 tools/render_obj.py out.png view label1=a.obj label2=b.obj ...
#   view = front|back|left|right|frontleft|frontright  [zlo zhi xlo xhi crop in world units]
import sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.collections import PolyCollection

def load_obj(path):
    vs, fs = [], []
    for line in open(path):
        if line.startswith("v "):
            vs.append([float(x) for x in line.split()[1:4]])
        elif line.startswith("f "):
            idx = [int(t.split("/")[0]) - 1 for t in line.split()[1:]]
            for k in range(1, len(idx) - 1):
                fs.append([idx[0], idx[k], idx[k + 1]])
    return np.array(vs), np.array(fs, dtype=int)

VIEWS = {
    # right/forward vectors per named view; Z is up in mesh space
    "front":      (np.array([1.0, 0, 0]),  np.array([0, -1.0, 0])),
    "back":       (np.array([-1.0, 0, 0]), np.array([0, 1.0, 0])),
    "left":       (np.array([0, -1.0, 0]), np.array([-1.0, 0, 0])),
    "right":      (np.array([0, 1.0, 0]),  np.array([1.0, 0, 0])),
    "frontleft":  (np.array([0.7071, -0.7071, 0]), np.array([-0.7071, -0.7071, 0])),
    "frontright": (np.array([0.7071, 0.7071, 0]),  np.array([0.7071, -0.7071, 0])),
}

def render(ax, vs, fs, view, crop=None):
    right, fwd = VIEWS[view]
    up = np.array([0, 0, 1.0])
    x2 = vs @ right
    y2 = vs @ up
    depth = vs @ fwd
    tri = vs[fs]
    n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    nl = np.linalg.norm(n, axis=1, keepdims=True); nl[nl == 0] = 1
    n = n / nl
    light = -(fwd + 0.3 * right + 0.5 * up)
    light = light / np.linalg.norm(light)
    shade = np.abs(n @ light) * 0.75 + 0.20
    zdepth = depth[fs].mean(axis=1)
    order = np.argsort(zdepth)  # far first
    polys = np.stack([x2[fs], y2[fs]], axis=-1)[order]
    cols = plt.cm.gray(shade[order])
    pc = PolyCollection(polys, facecolors=cols, edgecolors="black", linewidths=0.15)
    ax.add_collection(pc)
    if crop:
        ax.set_xlim(crop[2], crop[3]); ax.set_ylim(crop[0], crop[1])
    else:
        ax.set_xlim(x2.min() - 2, x2.max() + 2); ax.set_ylim(y2.min() - 2, y2.max() + 2)
    ax.set_aspect("equal"); ax.axis("off")

def main():
    out, view = sys.argv[1], sys.argv[2]
    args = sys.argv[3:]
    crop = None
    if args and "=" not in args[0]:
        crop = [float(a) for a in args[:4]]; args = args[4:]
    pairs = [a.split("=", 1) for a in args]
    fig, axes = plt.subplots(1, len(pairs), figsize=(7 * len(pairs), 9))
    if len(pairs) == 1: axes = [axes]
    for ax, (label, path) in zip(axes, pairs):
        vs, fs = load_obj(path)
        render(ax, vs, fs, view, crop)
        ax.set_title(label, fontsize=14)
    fig.tight_layout()
    fig.savefig(out, dpi=110, facecolor="white")
    print("wrote", out)

if __name__ == "__main__":
    main()
