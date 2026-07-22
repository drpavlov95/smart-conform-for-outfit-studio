# MANDATORY PRE-DELIVERY RENDER SHEET (2026-07-22). The hard lesson of this
# project, learned the expensive way over and over: NO scalar metric matches the
# user's eye. Surface distance misses creases (cloth in the right place but
# folded). Per-vertex distance over-flags invisible tangential slide. Dihedral
# catches sharp hem creases but dilutes long shallow thigh folds. The user sees
# all of them instantly in OutfitStudio; my numbers said "fine" every time.
#
# The one thing that DID match his eye was RENDERING it and LOOKING - the thigh
# creases and the butt groove were obvious in a smooth-shaded render from his
# angles, next to the hand-sculpt. So the validation is not a number, it is this
# sheet: result / production / hand-sculpt, side by side, in the views the user
# actually inspects, smooth-shaded like OutfitStudio. Generate it and LOOK,
# region by region, against the sculpt column, before anything reaches him.
#
# Usage: py -3 pathb_render_sheet.py RESULT.obj --capture CAP [--prod PROD.obj]
#        [--sculpt sculpt.npy] [--out sheet.png]
import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from shell_spike import read_capture_front
from pathb_view import Orientation, _cull


def load(f):
    return np.array([[float(x) for x in l.split()[1:4]]
                     for l in open(f) if l.startswith("v ")])


def vnormals(v, t):
    fn = np.cross(v[t[:, 1]] - v[t[:, 0]], v[t[:, 2]] - v[t[:, 0]])
    fn /= np.maximum(np.linalg.norm(fn, axis=1), 1e-9)[:, None]
    out = np.zeros_like(v)
    for k in range(3):
        np.add.at(out, t[:, k], fn)
    return out / np.maximum(np.linalg.norm(out, axis=1), 1e-9)[:, None]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("result")
    ap.add_argument("--capture", required=True)
    ap.add_argument("--prod")
    ap.add_argument("--sculpt", help=".npy of the hand-sculpt mesh")
    ap.add_argument("--out", default="tmp/live0721/render_sheet.png")
    a = ap.parse_args()

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection

    cap = read_capture_front(Path(a.capture))
    tris = np.asarray(cap.tris)
    body_v = np.asarray(cap.body_verts)
    o = Orientation.detect(body_v)
    front = -90 if o.front_sign < 0 else 90
    back = -front

    # (name, azim, elev, zlim) - the views the user inspects. Smooth-shaded,
    # perspective ~FOV 65 like OutfitStudio; zoomed crops on the trouble spots.
    views = [
        ("frente", front, 4, (18, 116)),
        ("costas", back, 4, (18, 116)),
        ("perfil-dir", 0, 3, (18, 116)),
        ("bumbum", back, -12, (40, 76)),
        ("coxa-perfil", 0, 3, (24, 60)),
    ]
    cols = [("RESULT", load(a.result))]
    if a.prod:
        cols.append(("PRODUCAO", load(a.prod)))
    if a.sculpt:
        cols.append(("SCULPT", np.load(a.sculpt)))

    light = np.array([0.4, -0.3, 0.55])
    light = light / np.linalg.norm(light)
    nrow, ncol = len(views), len(cols)
    fig = plt.figure(figsize=(4.6 * ncol, 4.0 * nrow))
    for ri, (vname, az, el, (z0, z1)) in enumerate(views):
        for ci, (label, gv) in enumerate(cols):
            ax = fig.add_subplot(nrow, ncol, ri * ncol + ci + 1,
                                 projection="3d")
            nn = vnormals(gv, tris)
            keep = _cull(gv, tris, az, elev=el)
            tv = gv[tris[keep]]
            fnorm = nn[tris[keep]].mean(1)
            fnorm /= np.maximum(np.linalg.norm(fnorm, axis=1), 1e-9)[:, None]
            sh = 0.35 + 0.6 * np.abs(fnorm @ light)
            ax.add_collection3d(Poly3DCollection(
                tv, facecolors=np.column_stack(
                    [sh * 0.5, sh * 0.5, sh * 0.53, np.ones(len(tv))]),
                edgecolors="none"))
            cen = gv.mean(0)
            ax.set_xlim(cen[0] - 24, cen[0] + 24)
            ax.set_ylim(cen[1] - 24, cen[1] + 24)
            ax.set_zlim(z0, z1)
            ax.view_init(elev=el, azim=az)
            ax.set_axis_off()
            ax.set_box_aspect((1, 1, 1))
            ax.set_proj_type("persp", focal_length=0.45)
            ax.set_title(f"{label} - {vname}", fontsize=9)
    fig.suptitle(f"axes {o.label()} - COMPARE each RESULT view to SCULPT",
                 fontsize=11)
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(a.out, dpi=120, bbox_inches="tight")
    print(a.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
