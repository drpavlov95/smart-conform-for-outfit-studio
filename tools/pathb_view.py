# The SEEING pipeline (user mandate 2026-07-17): orientation is VERIFIED from
# anatomy at load time — never assumed from convention. History: the analysis
# ran MIRRORED (y<0 treated as back; anatomy proves -y is the FRONT: hanging
# belly apex ~z64, waist crease ~z82, breasts ~z94; +y is the BACK: broad flat
# torso z70-94, receding nape at the top). Codex caught it first; every render
# label and profile table before this module carried the flip risk.
#
# Provides:
#   Orientation.detect(body_verts)  -> verified axes or raises loudly
#   render_views(...)               -> lit, labeled, body+garment composites
#   contact_sheet(...)              -> candidate | production | sculpt strip
#
# Rule: every visual claim about a result goes through this module.
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))


class Orientation:
    """front_sign: sign of y on the character's FRONT side."""

    def __init__(self, front_sign):
        self.front_sign = front_sign

    @classmethod
    def detect(cls, body_verts):
        b = np.asarray(body_verts)
        core = np.abs(b[:, 0]) < 8
        def ext(zlo, zhi, sign):
            m = core & (b[:, 2] >= zlo) & (b[:, 2] < zhi)
            if not m.sum():
                return 0.0
            return float((sign * b[m][:, 1]).max())
        # LESSON (2026-07-17, caught by the user): torso landmarks are
        # AMBIGUOUS on a heavy body - butt/lumbar/scapula mimics the
        # belly/crease/breast valley, and the first version of this detector
        # published a WRONG verdict stamped 'VERIFIED'. The LEGS are not
        # ambiguous: the gastrocnemius (calf, lower leg) bulges BACKWARD and
        # the patella/quads (knee band) bulge FORWARD - mechanically fixed
        # anatomy, immune to weight morphs. Both voters must agree.
        legs = (np.abs(b[:, 0]) > 1.5) & (np.abs(b[:, 0]) < 10)
        def leg_side(zlo, zhi):
            # mean per-leg asymmetry: + => bulge toward +y
            vals = []
            for sx in (+1.0, -1.0):
                m = legs & (np.sign(b[:, 0]) == sx) &                     (b[:, 2] >= zlo) & (b[:, 2] < zhi)
                if not m.sum():
                    continue
                yc = b[m][:, 1].mean()
                vals.append((b[m][:, 1].max() - yc) - (yc - b[m][:, 1].min()))
            return float(np.mean(vals)) if vals else 0.0
        zmin = float(b[:, 2].min())
        calf = leg_side(zmin + 2, zmin + 11)    # pure calf band: bulges BACK
        knee = leg_side(zmin + 23, zmin + 31)   # knee/quads band: bulges FRONT
        vote_calf = -np.sign(calf)   # front is the OPPOSITE of the calf bulge
        vote_knee = np.sign(knee)
        if vote_calf == 0 or vote_knee == 0 or vote_calf != vote_knee:
            raise SystemExit(
                f"Orientation.detect: leg voters disagree (calf={calf:+.2f} "
                f"knee={knee:+.2f}) - REFUSE to guess. Inspect manually.")
        return cls(front_sign=float(vote_calf))

    def label(self):
        return (f"FRONT={'-y' if self.front_sign < 0 else '+y'} "
                f"BACK={'+y' if self.front_sign < 0 else '-y'}")

    def profile(self, verts, side, zr=range(36, 84, 4), xband=None):
        """Max extent toward `side` ('front'|'back') per z band."""
        v = np.asarray(verts)
        s = self.front_sign if side == "front" else -self.front_sign
        out = {}
        for z in zr:
            m = (v[:, 2] >= z) & (v[:, 2] < z + 4) & (s * v[:, 1] > 0)
            if xband is not None:
                m &= (np.abs(v[:, 0]) >= xband[0]) & (np.abs(v[:, 0]) < xband[1])
            out[z] = float((s * v[m][:, 1]).max()) if m.sum() else None
        return out


def _shade(tris_xyz, light=(0.3, -0.5, 0.8)):
    n = np.cross(tris_xyz[:, 1] - tris_xyz[:, 0], tris_xyz[:, 2] - tris_xyz[:, 0])
    n /= np.maximum(np.linalg.norm(n, axis=1), 1e-12)[:, None]
    l = np.asarray(light) / np.linalg.norm(light)
    lam = np.abs(n @ l)
    return 0.35 + 0.6 * lam


def render_views(out_prefix, orient, garment_v, garment_t, body_v, body_t,
                 title="", views=None):
    """Lit, depth-sorted, LABELED composites. Views named in CHARACTER terms."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection

    fs = orient.front_sign
    # camera azimuths in matplotlib terms: azim is measured from +x;
    # azim=-90 puts the camera on -y (sees the -y side of the mesh).
    cam = {
        "front": -90 if fs < 0 else 90,
        "back": 90 if fs < 0 else -90,
        "left_profile": 180,
        "right_profile": 0,
        "back_3q": (90 if fs < 0 else -90) + 45,
    }
    views = views or ["front", "back", "left_profile", "back_3q"]
    paths = []
    for name in views:
        fig = plt.figure(figsize=(6, 9))
        ax = fig.add_subplot(111, projection="3d")
        bt = body_v[body_t]
        bs = _shade(bt)
        pc = Poly3DCollection(bt, facecolors=np.column_stack(
            [bs * 0.45, bs * 0.35, bs * 0.32, np.ones(len(bt))]),
            edgecolors="none")
        ax.add_collection3d(pc)
        gt_ = garment_v[garment_t]
        gs = _shade(gt_)
        gc = Poly3DCollection(gt_, facecolors=np.column_stack(
            [gs * 0.82, gs * 0.78, gs * 0.70, np.ones(len(gt_))]),
            edgecolors="none")
        ax.add_collection3d(gc)
        allv = np.concatenate([body_v, garment_v])
        c = allv.mean(0)
        r = (allv.max(0) - allv.min(0)).max() * 0.40
        ax.set_xlim(c[0] - r, c[0] + r)
        ax.set_ylim(c[1] - r, c[1] + r)
        ax.set_zlim(c[2] - r, c[2] + r)
        ax.view_init(elev=4, azim=cam[name])
        ax.set_axis_off()
        ax.set_box_aspect((1, 1, 1))
        ax.set_title(f"{title} — {name.upper()} ({orient.label()})",
                     fontsize=9)
        out = f"{out_prefix}_{name}.png"
        fig.savefig(out, dpi=110, bbox_inches="tight")
        plt.close(fig)
        paths.append(out)
    return paths


def contact_sheet(out_path, orient, columns, garment_t, body_v, body_t,
                  views=None):
    """columns: list of (label, garment_verts). One row per view, one column
    per candidate — same camera, same lighting, judged side by side."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection

    fs = orient.front_sign
    cam = {
        "front": -90 if fs < 0 else 90,
        "back": 90 if fs < 0 else -90,
        "left_profile": 180,
        "back_3q": (90 if fs < 0 else -90) + 45,
    }
    views = views or ["front", "back", "left_profile", "back_3q"]
    nrow, ncol = len(views), len(columns)
    fig = plt.figure(figsize=(3.2 * ncol, 4.6 * nrow))
    for ri, vname in enumerate(views):
        for ci, (label, gv) in enumerate(columns):
            ax = fig.add_subplot(nrow, ncol, ri * ncol + ci + 1,
                                 projection="3d")
            bt = body_v[body_t]
            bs = _shade(bt)
            ax.add_collection3d(Poly3DCollection(
                bt, facecolors=np.column_stack(
                    [bs * 0.45, bs * 0.35, bs * 0.32, np.ones(len(bt))]),
                edgecolors="none"))
            gt_ = gv[garment_t]
            gs = _shade(gt_)
            ax.add_collection3d(Poly3DCollection(
                gt_, facecolors=np.column_stack(
                    [gs * 0.82, gs * 0.78, gs * 0.70, np.ones(len(gt_))]),
                edgecolors="none"))
            allv = np.concatenate([body_v, gv])
            c = allv.mean(0)
            r = (allv.max(0) - allv.min(0)).max() * 0.40
            ax.set_xlim(c[0] - r, c[0] + r)
            ax.set_ylim(c[1] - r, c[1] + r)
            ax.set_zlim(c[2] - r, c[2] + r)
            ax.view_init(elev=4, azim=cam[vname])
            ax.set_axis_off()
            ax.set_box_aspect((1, 1, 1))
            ax.set_title(f"{label}\n{vname}", fontsize=8)
    fig.suptitle(f"axes VERIFIED: {orient.label()}", fontsize=10)
    fig.savefig(out_path, dpi=100, bbox_inches="tight")
    plt.close(fig)
    return out_path
