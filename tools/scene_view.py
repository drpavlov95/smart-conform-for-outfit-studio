#!/usr/bin/env python3
"""Render a WHOLE live scene the way he actually looks at it.

Every "it measured better" that turned out to be wrong this project had the same
cause: I was looking at one garment, flat-shaded, in isolation, from one fixed
angle, with no body - and he opens the outfit assembled, on the body, shaded,
from whatever angle the defect is visible in. A skirt that is fine alone can be
wrong next to the cloak; a silhouette that reads straight from dead-behind can
balloon at three-quarters.

So this renders the SET: every shape of a scene together, the body underneath,
smooth-shaded, from several angles, vanilla against ours side by side. It is
slower and less quantitative than the scalars, and it is the only thing that has
consistently agreed with him.
"""
from __future__ import annotations
import argparse
import subprocess
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.collections import PolyCollection

# right, forward - Z is up in mesh space. Skyrim: front = +y, back = -y.
VIEWS = {
    "frente":      (np.array([1.0, 0, 0]),   np.array([0, -1.0, 0])),
    "costas":      (np.array([-1.0, 0, 0]),  np.array([0, 1.0, 0])),
    "perfil":      (np.array([0, -1.0, 0]),  np.array([-1.0, 0, 0])),
    "costas34":    (np.array([-0.7071, -0.7071, 0]), np.array([0.7071, -0.7071, 0])),
    "frente34":    (np.array([0.7071, -0.7071, 0]),  np.array([-0.7071, -0.7071, 0])),
}


def load_obj(path: Path):
    vs, fs = [], []
    for line in open(path):
        if line.startswith("v "):
            vs.append([float(x) for x in line.split()[1:4]])
        elif line.startswith("f "):
            idx = [int(t.split("/")[0]) - 1 for t in line.split()[1:]]
            for k in range(1, len(idx) - 1):
                fs.append([idx[0], idx[k], idx[k + 1]])
    return np.array(vs), np.array(fs, dtype=int)


def draw(ax, meshes, view, crop=None):
    """meshes: list of (verts, faces, base_rgb). Painter's algorithm, one pass."""
    right, fwd = VIEWS[view]
    up = np.array([0, 0, 1.0])
    polys, colors, depths = [], [], []
    for vs, fs, rgb in meshes:
        if len(fs) == 0:
            continue
        tri = vs[fs]
        n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
        nl = np.linalg.norm(n, axis=1, keepdims=True)
        nl[nl == 0] = 1
        n = n / nl
        # headlight plus a fill from above, which is roughly what the OS
        # viewport does and keeps folds readable instead of flat.
        lam = np.clip(-(n @ fwd), 0, 1) * 0.75 + np.clip(n @ up, 0, 1) * 0.25
        shade = 0.25 + 0.75 * lam
        x2 = tri @ right
        y2 = tri @ up
        polys.extend(np.stack([x2, y2], axis=-1))
        colors.extend(np.clip(np.array(rgb)[None, :] * shade[:, None], 0, 1))
        depths.extend((tri @ fwd).mean(axis=1))   # one depth per face, not per corner
    order = np.argsort(depths)
    pc = PolyCollection([polys[i] for i in order],
                        facecolors=[colors[i] for i in order],
                        edgecolors="none", antialiased=False)
    ax.add_collection(pc)
    allv = np.concatenate([m[0] for m in meshes])
    if crop:
        zlo, zhi = crop
        sel = (allv[:, 2] >= zlo) & (allv[:, 2] <= zhi)
        if sel.any():
            allv = allv[sel]
    xs, ys = allv @ right, allv @ up
    pad = 2.0
    cx, cy = (xs.min() + xs.max()) / 2, (ys.min() + ys.max()) / 2
    half = max(xs.max() - xs.min(), ys.max() - ys.min()) / 2 + pad
    ax.set_xlim(cx - half, cx + half)
    ax.set_ylim(cy - half, cy + half)
    ax.set_aspect("equal")
    ax.axis("off")


# Garments get distinct tints so a piece can be told from its neighbour; the
# body is deliberately dull so cloth reads against it.
TINTS = [(0.72, 0.74, 0.80), (0.80, 0.72, 0.68), (0.70, 0.78, 0.72),
         (0.82, 0.78, 0.66), (0.74, 0.70, 0.80), (0.78, 0.76, 0.74)]
BODY = (0.52, 0.50, 0.49)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene", required=True, help="dir with the scene's .drcap files")
    ap.add_argument("--out", required=True)
    ap.add_argument("--harness", default="./build/algo/Release/drape-harness.exe")
    ap.add_argument("--work", default="tmp/sceneview")
    ap.add_argument("--views", default="costas,costas34,perfil")
    ap.add_argument("--crop", default="", help="zlo:zhi to zoom a band")
    ap.add_argument("--env", default="")
    a = ap.parse_args()

    work = Path(a.work)
    work.mkdir(parents=True, exist_ok=True)
    caps = sorted(Path(a.scene).glob("*.drcap"))
    if not caps:
        print("nenhuma captura em", a.scene)
        return 1

    import os
    base_env = dict(os.environ)
    base_env.update(dict(DRAPE_SHELL_SOLVE="1", DRAPE_SHELL_TRANSACTIONS="0",
                         DRAPE_SHELL_BODY_EVICT="1", DRAPE_ANTICLIP_SAFE_FLOOR="1",
                         DRAPE_POSTERIOR_CHORD="1"))
    for kv in a.env.split():
        if "=" in kv:
            k, v = kv.split("=", 1)
            base_env[k] = v

    ours, vanilla, body = [], [], None
    for i, cap in enumerate(caps):
        shape = cap.name.split("___")[-1].split("__RM")[0]
        r = work / f"{shape}__ours.obj"
        v = work / f"{shape}__vanilla.obj"
        b = work / f"{shape}__body.obj"
        subprocess.run([a.harness, "--replay", str(cap), "--result-obj", str(r),
                        "--input-obj", str(v), "--body-obj", str(b)],
                       env=base_env, capture_output=True)
        if r.exists():
            ours.append((*load_obj(r), TINTS[i % len(TINTS)]))
        if v.exists():
            vanilla.append((*load_obj(v), TINTS[i % len(TINTS)]))
        if body is None and b.exists():
            body = (*load_obj(b), BODY)
        print(f"  {shape}")

    if body is not None:
        ours.append(body)
        vanilla.append(body)
    views = [v.strip() for v in a.views.split(",") if v.strip() in VIEWS]
    crop = tuple(float(x) for x in a.crop.split(":")) if a.crop else None

    fig, axes = plt.subplots(len(views), 2,
                             figsize=(9 * 2, 8 * len(views)), squeeze=False)
    for r, view in enumerate(views):
        for c, (label, meshes) in enumerate((("VANILLA (sem Drape)", vanilla),
                                             ("NOSSO", ours))):
            draw(axes[r][c], meshes, view, crop)
            axes[r][c].set_title(f"{label} — {view}", fontsize=15)
    fig.tight_layout()
    fig.savefig(a.out, dpi=95, facecolor="white")
    print("wrote", a.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
