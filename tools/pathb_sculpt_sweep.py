# MANDATORY PRE-DELIVERY SWEEP (2026-07-22). Born from a repeated failure:
# I kept validating ONLY the region the user complained about (arm, then butt),
# delivered, and he found a DIFFERENT region broken (the thigh) every time -
# because the aggregate sculpt score (mean/p95 over all verts) HID regional
# regressions (it read "shell better" while the front thigh was 0.5-1.4u worse).
# This compares a result mesh to the hand-sculpt (the ground truth) PER BODY
# REGION, so every region that diverges more than production is surfaced BEFORE
# anything reaches the user. Run this on every shell change, not just the spot
# the user pointed at.
#
# Usage: py -3 pathb_sculpt_sweep.py RESULT.obj --capture CAP [--prod PROD.obj]
import argparse
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from shell_spike import read_capture_front

# Hand-sculpt OSD sources (RM_Heavy slider set per garment shape).
SD = r"E:\Wabbajack\InstallPacks\mods\Rley Mods - Vanilla Refits\CalienteTools\BodySlide\ShapeData"
OSD_FOR = {
    "BlackSmithTorso": (SD + r"\CBBE 3BA Vanilla (Physics)\Body\Blacksmith.osd",
                        "BlackSmithTorsoRM_Heavy"),
}


def load(f):
    return np.array([[float(x) for x in l.split()[1:4]]
                     for l in open(f) if l.startswith("v ")])


def read_osd_set(path, setname):
    data = open(path, "rb").read()
    off = 0
    _, _, count = struct.unpack_from("<III", data, off)
    off += 12
    for _ in range(count):
        (nlen,) = struct.unpack_from("<B", data, off)
        off += 1
        name = data[off:off + nlen].decode("ascii", "replace")
        off += nlen
        (dc,) = struct.unpack_from("<H", data, off)
        off += 2
        d = {}
        for _ in range(dc):
            idx, x, y, z = struct.unpack_from("<Hfff", data, off)
            off += 14
            d[idx] = (x, y, z)
        if name == setname:
            return d
    raise KeyError(setname)


def region(p, front_sign):
    x, y, z = p
    f = y * front_sign
    if z > 96:
        return "ombro/gola"
    if z > 88 and abs(x) > 10:
        return "manga-cima"
    if abs(x) > 22:
        return "manga-baixo/punho"
    if 72 < z <= 88:
        return "torso-peito" if f > 0 else "torso-costas"
    if 56 < z <= 72:
        return "cintura-frente" if f > 0 else "cintura/quadril-tras"
    if 40 < z <= 56:
        return "COXA-frente" if f > 0 else "bumbum"
    if 24 < z <= 40:
        return "coxa-baixa-frente" if f > 0 else "coxa-baixa-tras"
    return "hem"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("result")
    ap.add_argument("--capture", required=True)
    ap.add_argument("--prod", help="production (shell OFF) obj for comparison")
    ap.add_argument("--shape", default="BlackSmithTorso")
    a = ap.parse_args()

    cap = read_capture_front(Path(a.capture))
    base = np.asarray(cap.base_verts)
    # front sign from calf/knee anatomy (reuse pathb_view if importable)
    try:
        from pathb_view import Orientation
        fs = Orientation.detect(np.asarray(cap.body_verts)).front_sign
    except Exception:
        fs = 1.0

    osd_path, setname = OSD_FOR[a.shape]
    diffs = read_osd_set(osd_path, setname)
    sculpt = base.copy()
    for idx, (x, y, z) in diffs.items():
        if idx < len(sculpt):
            sculpt[idx] = base[idx] + np.array([x, y, z])

    res = load(a.result)
    dh = np.linalg.norm(res - sculpt, axis=1)
    prod = load(a.prod) if a.prod else None
    dp = np.linalg.norm(prod - sculpt, axis=1) if prod is not None else None

    reg = {}
    for i in range(len(base)):
        r = region(base[i], fs)
        reg.setdefault(r, [[], []])
        reg[r][0].append(dh[i])
        if dp is not None:
            reg[r][1].append(dp[i])

    print(f"{'regiao':26} {'n':>4} {'RESULT p90':>11}", end="")
    print(f" {'PROD p90':>9}  veredito" if dp is not None else "")
    rows = []
    for r, (h, p) in reg.items():
        h = np.array(h)
        pp = np.percentile(p, 90) if p else None
        rows.append((r, len(h), np.percentile(h, 90), pp))
    key = (lambda x: -(x[2] - (x[3] or 0)))
    worse = 0
    for r, n, hp, pp in sorted(rows, key=key):
        if pp is None:
            print(f"{r:26} {n:4d} {hp:11.2f}")
            continue
        v = "PIOR" if hp > pp + 0.15 else ("melhor" if hp < pp - 0.15 else "empate")
        if v == "PIOR":
            worse += 1
        print(f"{r:26} {n:4d} {hp:11.2f} {pp:9.2f}  {v}")
    if dp is not None:
        print(f"\n{worse} regiao(oes) PIOR que a producao vs o sculpt", end="")
        print(" - investigar causa-raiz antes de entregar" if worse else "")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
