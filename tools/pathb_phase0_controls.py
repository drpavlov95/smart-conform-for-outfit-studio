# Phase 0 controls (plan review 2026-07-17): decide the stiffness question
# BEFORE any solver sweep, with pure geometry/measurement.
#
# C1 RIGID LIMIT: what can a rigid rotation of the back panel about the belt
#   line reach? Three constructions:
#   (a) minimum-clearance tent: smallest rotation that clears the morphed
#       body at every panel vert; profile of the rotated panel.
#   (b) rotation that matches the sculpt's apex extent; where does the hem go?
#   (c) the sculpt itself, for reference.
#   If (a) sits far below the sculpt and (b) blows the hem out, NO uniform
#   stiffness reproduces the sculpt; the A-line needs rest-shape flare or is
#   a coarse rail (the user said the sculpt is approximate).
#
# C2 DIHEDRAL FINGERPRINT: on back-panel hinges, is the sculpt closer to the
#   AUTHORED dihedrals (rigid shell) or to FLAT (straightened fall = drape)?
#   Split by hinge edge orientation: horizontal edges carry the vertical
#   (fall-line) curvature; vertical edges carry circumferential curvature.
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from pathb_metrics import load_obj
from shell_spike import UniformBodyCollider, read_capture_front

CAP = Path("../corpus/fur_blacksmith_rm_heavy/drape_capture__Rley_3BA_Vanilla_-_Body_-_Blacksmith__Physics___BlackSmithTorso__RM_Heavy.drcap")
GT = "../fixtures/pathb/blacksmith__GROUND_TRUTH.obj"


def profile(v, mask=None, zr=range(38, 82, 4)):
    out = {}
    for z in zr:
        m = (v[:, 2] >= z) & (v[:, 2] < z + 4) & (v[:, 1] < 0)
        if mask is not None:
            m &= mask
        out[z] = float(-v[m][:, 1].min()) if m.sum() else None
    return out


def fmt(p):
    return " ".join(f"z{z}:{p[z]:.1f}" if p[z] is not None else f"z{z}:-"
                    for z in p)


def main():
    cap = read_capture_front(CAP)
    base = cap.base_verts
    gt, _ = load_obj(GT)
    col = UniformBodyCollider(cap.body_verts, cap.body_tris)
    col0 = UniformBodyCollider(cap.ref_body_verts, cap.ref_body_tris)
    g0 = col0.gaps(base)

    movable = (g0 > 0.3)
    back = movable & (base[:, 1] < 0) & (base[:, 2] < 86)
    # belt pivot: bottom of the fixed band on the back
    fixed_back = (~movable) & (base[:, 1] < 0) & (base[:, 2] > 70)
    zp = float(base[fixed_back][:, 2].min()) if fixed_back.sum() else 84.0
    yp = float(base[fixed_back][:, 1].mean()) if fixed_back.sum() else -10.0
    print(f"# C1 rigid limit — pivot z={zp:.1f} (belt-band bottom), back panel n={int(back.sum())}")

    idx = np.nonzero(back)[0]
    P = base[idx]

    def rotate(phi):
        # rotate about the horizontal x-axis line through (y=yp, z=zp):
        # outward = -y for the back
        out = P.copy()
        dy = P[:, 1] - yp
        dz = P[:, 2] - zp
        c, s = np.cos(phi), np.sin(phi)
        out[:, 1] = yp + dy * c - dz * s * np.sign(-1.0)  # -y outward as z drops
        out[:, 2] = zp + dz * c + dy * s * 0.0            # keep z (small-angle shear form)
        # small-angle tent: displacement purely outward proportional to depth
        out = P.copy()
        out[:, 1] = P[:, 1] - np.maximum(zp - P[:, 2], 0.0) * np.tan(phi)
        return out

    # (a) minimum clearance rotation
    lo, hi = 0.0, 0.6
    for _ in range(40):
        mid = 0.5 * (lo + hi)
        if (col.gaps(rotate(mid)) < 0.15).sum() == 0:
            hi = mid
        else:
            lo = mid
    phiA = hi
    Ra = rotate(phiA)
    va = base.copy(); va[idx] = Ra
    pa = profile(va, mask=back | (~movable))
    print(f"(a) tenda de folga mínima: phi={np.degrees(phiA):.1f}°")
    print("    perfil:", fmt(pa))

    # (b) rotation matching sculpt apex extent (z62-70 max ~20.0-20.2)
    tgt = 20.0
    m_apex = (P[:, 2] >= 62) & (P[:, 2] < 70)
    if m_apex.sum():
        cur = -P[m_apex][:, 1].max()
        need = None
        lo, hi = 0.0, 1.0
        for _ in range(40):
            mid = 0.5 * (lo + hi)
            r = rotate(mid)
            if -r[m_apex][:, 1].min() >= tgt:
                hi = mid
            else:
                lo = mid
        phiB = hi
        Rb = rotate(phiB)
        vb = base.copy(); vb[idx] = Rb
        pb = profile(vb, mask=back | (~movable))
        print(f"(b) rotação até ápice=20: phi={np.degrees(phiB):.1f}°")
        print("    perfil:", fmt(pb))
    print("(c) SCULPT:", fmt(profile(gt)))
    print("    AUTORAL:", fmt(profile(base)))
    print("    CORPO  :", fmt(profile(cap.body_verts)))

    # ---- C2 dihedral fingerprint ----
    tris = cap.tris
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
    hinges = np.array(hinges)

    def dihedrals(v):
        a, b, c, d = (v[hinges[:, 0]], v[hinges[:, 1]],
                      v[hinges[:, 2]], v[hinges[:, 3]])
        e = b - a
        n1 = np.cross(b - a, c - a)
        n2 = np.cross(d - a, b - a)
        n1 /= np.maximum(np.linalg.norm(n1, axis=1), 1e-12)[:, None]
        n2 /= np.maximum(np.linalg.norm(n2, axis=1), 1e-12)[:, None]
        return np.arccos(np.clip((n1 * n2).sum(1), -1, 1))

    da = dihedrals(base)
    dg = dihedrals(gt)
    hz = base[hinges[:, 0], 2] * 0.5 + base[hinges[:, 1], 2] * 0.5
    hy = base[hinges[:, 0], 1] * 0.5 + base[hinges[:, 1], 1] * 0.5
    hmov = movable[hinges[:, 0]] & movable[hinges[:, 1]]
    sel = hmov & (hy < 0) & (hz > 40) & (hz < 80)
    edir = base[hinges[:, 1]] - base[hinges[:, 0]]
    ez = np.abs(edir[:, 2]) / np.maximum(np.linalg.norm(edir, axis=1), 1e-9)
    horiz = sel & (ez < 0.4)     # horizontal edges = fall-line curvature
    vert = sel & (ez > 0.7)
    print(f"\n# C2 fingerprint — hinges back-panel: {int(sel.sum())} (horiz {int(horiz.sum())}, vert {int(vert.sum())})")
    for name, m in [("fall-line (horiz edges)", horiz), ("circunf (vert edges)", vert), ("todos", sel)]:
        if m.sum() == 0:
            continue
        toward_auth = np.abs(dg[m] - da[m])
        toward_flat = np.abs(dg[m])          # 0 = flat (angle between normals)
        auth_flat = np.abs(da[m])
        print(f"  {name:24} |sculpt-autoral| med={np.degrees(np.median(toward_auth)):5.1f}°  "
              f"|sculpt-flat| med={np.degrees(np.median(toward_flat)):5.1f}°  "
              f"|autoral-flat| med={np.degrees(np.median(auth_flat)):5.1f}°")
    print("  leitura: sculpt≈autoral => casca rígida; sculpt<<autoral (esticado) => drape/prumo")


if __name__ == "__main__":
    main()
