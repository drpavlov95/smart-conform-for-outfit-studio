# HARD DEPLOY GATE (2026-07-20). Born from a failure: I shipped a build whose
# imprint, hem height and sculpt distance had all IMPROVED while the sleeves
# had a gaping hole - 60 inverted faces in the armpit region that no aggregate
# metric noticed. Averages cannot see a hole. This checks the defect classes
# that are individually visible to the eye, per REGION, against production.
#
# Blocking rules (any failure => do not deploy):
#   flipped faces        <= production (arms/armpit checked separately)
#   collapsed faces      <= production
#   open exact seams     == 0
#   worst edge stretch   < 8.0 (the host validator's ceiling; above it the
#                         host silently discards the whole shape)
#
# Usage: py -3 pathb_defect_gate.py CANDIDATE.obj [--baseline PROD.obj]
import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from pathb_metrics import exact_seam_groups, face_normals, load_obj
from shell_spike import read_capture_front

CAP = Path("../corpus/fur_blacksmith_rm_heavy/drape_capture__Rley_3BA_Vanilla_-_Body_-_Blacksmith__Physics___BlackSmithTorso__RM_Heavy.drcap")
HOST_STRETCH_CEILING = 8.0


def defects(v, base, tris, edges, groups):
    bn, ba = face_normals(base, tris)
    tn, ta = face_normals(v, tris)
    ok = ba > 1e-9
    flip = ok & ((tn * bn).sum(1) < 0)
    coll = ok & (ta < 0.10 * ba)
    cen = base[tris].mean(1)
    arm = (cen[:, 2] > 90) | (np.abs(cen[:, 0]) > 18)
    rest = np.linalg.norm(base[edges[:, 0]] - base[edges[:, 1]], axis=1)
    L = np.linalg.norm(v[edges[:, 0]] - v[edges[:, 1]], axis=1)
    ratio = np.where(rest > 1e-6, L / np.maximum(rest, 1e-9), 0.0)
    seam = max((float(np.linalg.norm(v[g] - v[g][0], axis=1).max())
                for g in groups), default=0.0)
    return dict(flips=int(flip.sum()), flips_arm=int((flip & arm).sum()),
                collapsed=int(coll.sum()), seam_open=seam,
                stretch=float(ratio.max()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("candidate")
    ap.add_argument("--baseline", default="../tmp/pathb/prod_bs.obj")
    ap.add_argument("--capture", default=str(CAP))
    a = ap.parse_args()

    cap = read_capture_front(Path(a.capture))
    base, tris = cap.base_verts, np.asarray(cap.tris)
    edges = np.asarray(cap.edges)
    groups = exact_seam_groups(base)

    cand = defects(load_obj(a.candidate)[0], base, tris, edges, groups)
    ref = defects(load_obj(a.baseline)[0], base, tris, edges, groups)

    checks = [
        ("faces invertidas (total)", cand["flips"], ref["flips"], "<="),
        ("faces invertidas (BRACOS)", cand["flips_arm"], ref["flips_arm"], "<="),
        ("faces colapsadas", cand["collapsed"], ref["collapsed"], "<="),
        ("costura aberta (u)", cand["seam_open"], 0.5, "<"),
        ("esticamento max", cand["stretch"], HOST_STRETCH_CEILING, "<"),
    ]
    print(f"{'verificacao':28} {'candidato':>10} {'limite':>10}   veredito")
    failed = 0
    for name, got, lim, op in checks:
        ok = got <= lim if op == "<=" else got < lim
        if not ok:
            failed += 1
        print(f"{name:28} {got:10.2f} {lim:10.2f}   {'ok' if ok else 'FALHA'}")
    print()
    if failed:
        print(f"BLOQUEADO: {failed} verificacao(oes) falharam - NAO deployar")
        return 1
    print("LIBERADO para deploy (mas o olho do usuario ainda decide)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
