# Path B S0.3d: measurement-only band DISTRIBUTION (center / width / directional
# quantiles). No constraint, no tuning - this only characterizes what differs.
#
# Why: marco 7 showed the per-band ARC LENGTH of the sculpt ~= the defect, and
# S0.3c showed no causal factor (anchors / soft-anchors / contact) explains the
# free-region gap. So the missing information is HOW the cloth is distributed
# inside the band. This tool decomposes the sculpt-vs-defect difference into:
#
#   center shift  - the section sits somewhere else (translation; e.g. the
#                   sculpt hangs plumb while the defect follows the body)
#   width ratio   - the section is bigger/smaller (scale)
#   sector radii  - the section has a different shape (per-azimuth radius)
#
# It reuses the validated material-iso contour from pathb_band_descriptor: the
# crossing point on an edge sits at the SAME material fraction in every mesh, so
# contours are comparable across meshes.
#
# Usage:
#   pathb_band_distribution.py --base BASE.obj --defect D.obj --sculpt S.obj
#                              [--body BODY.obj] [--levels 7]
import argparse
import numpy as np
from pathb_band_descriptor import load_obj, weld, components


def contour_points(wbase, wmesh, ctris, s_norm, lvl):
    """Material-iso contour crossing points of `wmesh` at level `lvl`."""
    pts = []
    for t in ctris:
        s = s_norm[t]
        above = s > lvl
        na = int(above.sum())
        if na == 0 or na == 3:
            continue
        lone = int(np.where(above == (na == 1))[0][0])
        for o in [j for j in range(3) if j != lone]:
            a, b = t[lone], t[o]
            denom = s_norm[b] - s_norm[a]
            f = 0.5 if abs(denom) < 1e-12 else (lvl - s_norm[a]) / denom
            f = min(max(f, 0.0), 1.0)
            pts.append(wmesh[a] + f * (wmesh[b] - wmesh[a]))
    return np.asarray(pts) if pts else np.empty((0, 3))


def section_stats(pts, axis):
    """center, width and 8 directional radii of a contour, in the plane ⟂ axis."""
    if len(pts) < 3:
        return None
    center = pts.mean(0)
    # basis perpendicular to the component axis
    tmp = np.array([1.0, 0, 0]) if abs(axis[0]) < 0.9 else np.array([0, 1.0, 0])
    e1 = np.cross(axis, tmp)
    e1 /= np.linalg.norm(e1)
    e2 = np.cross(axis, e1)
    d = pts - center
    x = d @ e1
    y = d @ e2
    r = np.hypot(x, y)
    ang = np.arctan2(y, x)
    sect = []
    for k in range(8):
        lo = -np.pi + k * np.pi / 4
        m = (ang >= lo) & (ang < lo + np.pi / 4)
        sect.append(float(np.median(r[m])) if m.sum() else float("nan"))
    return dict(center=center, width=float(r.mean()), p90=float(np.percentile(r, 90)),
                sectors=np.asarray(sect), e1=e1, e2=e2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--defect", required=True)
    ap.add_argument("--sculpt", required=True)
    ap.add_argument("--body")
    ap.add_argument("--levels", type=int, default=7)
    a = ap.parse_args()
    bv, tris = load_obj(a.base)
    dv, _ = load_obj(a.defect)
    sv, _ = load_obj(a.sculpt)
    body = load_obj(a.body)[0] if a.body else None
    remap, canon = weld(bv, tris)
    wtris = remap[tris]
    wbase, wdef, wscu = bv[canon], dv[canon], sv[canon]
    comps = components(len(canon), wtris)
    comp_of = np.full(len(canon), -1, dtype=np.int64)
    for ci, vs in enumerate(comps):
        for v in vs:
            comp_of[v] = ci
    # biggest component only (the panel that matters)
    ci = int(np.argmax([len(c) for c in comps]))
    cv = np.asarray(comps[ci])
    pts = wbase[cv]
    centroid = pts.mean(0)
    w, vec = np.linalg.eigh(np.cov((pts - centroid).T))
    axis = vec[:, np.argmax(w)]
    s_all = (wbase - centroid) @ axis
    smin, smax = s_all[cv].min(), s_all[cv].max()
    s_norm = (s_all - smin) / max(smax - smin, 1e-9)
    ctris = wtris[comp_of[wtris[:, 0]] == ci]
    print(f"# band distribution  comp={ci} ({len(cv)} verts)  axis={np.round(axis,3)}")
    print(f"{'s':>5} {'dCenter':>8} {'dC_perp':>8} {'dC_axis':>8} "
          f"{'wid_def':>8} {'wid_scu':>8} {'w_ratio':>8} {'shape_rms':>9}")
    for lvl in np.linspace(0.15, 0.85, a.levels):
        pd = contour_points(wbase, wdef, ctris, s_norm, lvl)
        ps = contour_points(wbase, wscu, ctris, s_norm, lvl)
        sd, ss = section_stats(pd, axis), section_stats(ps, axis)
        if not sd or not ss:
            continue
        dc = ss["center"] - sd["center"]
        dc_axis = float(dc @ axis)
        dc_perp = float(np.linalg.norm(dc - dc_axis * axis))
        # shape difference AFTER removing center+scale: normalized sector radii
        nd = sd["sectors"] / max(sd["width"], 1e-9)
        ns = ss["sectors"] / max(ss["width"], 1e-9)
        ok = ~(np.isnan(nd) | np.isnan(ns))
        shape_rms = float(np.sqrt(((ns[ok] - nd[ok]) ** 2).mean())) if ok.any() else float("nan")
        print(f"{lvl:5.2f} {np.linalg.norm(dc):8.3f} {dc_perp:8.3f} {dc_axis:8.3f} "
              f"{sd['width']:8.3f} {ss['width']:8.3f} "
              f"{ss['width']/max(sd['width'],1e-9):8.3f} {shape_rms:9.3f}")
    print("\nleitura: dC_perp grande = a seção do sculpt fica em OUTRO LUGAR "
          "(translação horizontal: pendura a prumo vs segue o corpo);\n"
          "         w_ratio != 1 = escala; shape_rms grande = forma diferente "
          "(após remover centro e escala).")


if __name__ == "__main__":
    main()
