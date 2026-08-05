#!/usr/bin/env python3
"""See a whole outfit the way he sees it, and be able to trust the picture.

Three things kept me reporting wins he could not find:

  1. I rendered ONE garment, isolated, with no body. He opens the set - cloak
     over skirt over pants over belt - on the body. A piece can measure fine
     alone and wreck the assembly.
  2. I sorted faces by depth, which invents shards wherever two surfaces touch,
     so the picture could not distinguish a real defect from its own artifact.
     This uses a z-buffer (tools/raster.py).
  3. I only ever compared against vanilla. When he says "it got worse" he means
     worse than the build he had, and that axis did not exist. Any number of
     columns can be given now.

Modes:
  solid   each piece its own tint, body dull underneath
  pen     paint the CLOTH-IN-CLOTH and CLOTH-IN-BODY vertices red. The defect
          he reports as "horroroso" is one garment coming through another, and
          it should be visible as itself rather than inferred from a scalar.
  diff    heat the vertices that moved between the first column and this one
"""
from __future__ import annotations
import argparse
import os
import subprocess
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

import sys
sys.path.insert(0, str(Path(__file__).parent))
from raster import Frame, draw, frame_bounds, VIEWS   # noqa: E402

TINTS = [(0.74, 0.76, 0.82), (0.82, 0.73, 0.68), (0.71, 0.80, 0.73),
         (0.84, 0.80, 0.66), (0.76, 0.71, 0.83), (0.80, 0.78, 0.74)]
BODY = (0.50, 0.48, 0.47)
HOT = (0.92, 0.15, 0.12)


def load_obj(path):
    vs, fs = [], []
    for line in open(path):
        if line.startswith("v "):
            vs.append([float(x) for x in line.split()[1:4]])
        elif line.startswith("f "):
            i = [int(t.split("/")[0]) - 1 for t in line.split()[1:]]
            for k in range(1, len(i) - 1):
                fs.append([i[0], i[k], i[k + 1]])
    return np.array(vs, np.float64), np.array(fs, np.int32)


def inside_mask(pts, verts, faces, tol=0.12):
    """Which of `pts` sit behind the surface (verts, faces).

    Nearest face centroid, then the sign against that face's normal. Approximate
    on purpose - this decides a COLOUR, not a score; the harness scores
    penetration exactly and is the thing to quote. `tol` keeps ordinary contact
    from lighting up the whole garment.

    Vectorised through a KD-tree because a slow viewer is a viewer I stop
    running, and looking once was itself one of the failures here.
    """
    from scipy.spatial import cKDTree
    tri = verts[faces]
    cen = tri.mean(axis=1)
    fn = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    ln = np.linalg.norm(fn, axis=1, keepdims=True)
    ln[ln == 0] = 1
    fn = fn / ln
    _d, j = cKDTree(cen).query(pts)
    return np.einsum("ij,ij->i", pts - cen[j], fn[j]) < -tol


def build_columns(scene, work, harness, env_extra, variants):
    caps = sorted(Path(scene).glob("*.drcap"))
    if not caps:
        raise SystemExit(f"nenhuma captura em {scene}")
    work = Path(work); work.mkdir(parents=True, exist_ok=True)
    base = dict(os.environ)
    base.update(dict(DRAPE_SHELL_SOLVE="1", DRAPE_SHELL_TRANSACTIONS="0",
                     DRAPE_SHELL_BODY_EVICT="1", DRAPE_ANTICLIP_SAFE_FLOOR="1",
                     DRAPE_POSTERIOR_CHORD="1"))
    for kv in env_extra.split():
        if "=" in kv:
            k, v = kv.split("=", 1)
            base[k] = v

    cols, body = [], None
    for label, envs in variants:
        env = dict(base)
        for kv in envs.split():
            if "=" in kv:
                k, v = kv.split("=", 1)
                env[k] = v
        pieces = []
        for i, cap in enumerate(caps):
            shape = cap.name.split("___")[-1].split("__RM")[0]
            tag = label.replace(" ", "_").replace("/", "_")
            out = work / f"{shape}__{tag}.obj"
            bod = work / f"{shape}__body.obj"
            args = [harness, "--replay", str(cap), "--body-obj", str(bod)]
            args += (["--input-obj", str(out)] if envs == "VANILLA"
                     else ["--result-obj", str(out)])
            subprocess.run(args, env=env, capture_output=True)
            if out.exists():
                v, f = load_obj(out)
                pieces.append((v, f, TINTS[i % len(TINTS)], shape))
            if body is None and bod.exists():
                body = load_obj(bod)
        cols.append((label, pieces))
        print(f"  coluna '{label}': {len(pieces)} peca(s)")
    return cols, body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--harness", default="./build/algo/Release/drape-harness.exe")
    ap.add_argument("--work", default="tmp/sceneview")
    ap.add_argument("--views", default="costas,costas34,perfil")
    ap.add_argument("--crop", default="")
    ap.add_argument("--mode", default="solid", choices=["solid", "pen", "diff"])
    ap.add_argument("--env", default="")
    ap.add_argument("--px", type=int, default=760)
    ap.add_argument("--cull", action="store_true", help="descarta faces de costas")
    ap.add_argument("--two-sided", action="store_true", help="ilumina os dois lados")
    ap.add_argument("--col", action="append", default=[],
                    help="rotulo=ENV=VAL ...  (use ENV literal VANILLA para o conform do host)")
    a = ap.parse_args()

    variants = []
    for c in a.col:
        label, _, envs = c.partition("=")
        variants.append((label, envs))
    if not variants:
        variants = [("VANILLA (sem Drape)", "VANILLA"), ("NOSSO", "")]

    cols, body = build_columns(a.scene, a.work, a.harness, a.env, variants)
    views = [v.strip() for v in a.views.split(",") if v.strip() in VIEWS]
    crop = tuple(float(x) for x in a.crop.split(":")) if a.crop else None

    ref = cols[0][1]
    fig, axes = plt.subplots(len(views), len(cols),
                             figsize=(6.4 * len(cols), 6.4 * len(views)),
                             squeeze=False)
    for ci, (label, pieces) in enumerate(cols):
        meshes = [(v, f, t) for v, f, t, _ in pieces]
        if body is not None:
            meshes = meshes + [(body[0], body[1], BODY)]
        for ri, view in enumerate(views):
            center, half = frame_bounds(meshes, view, crop)
            fr = Frame(a.px, int(a.px * 1.25))
            if body is not None:
                draw(fr, body[0], body[1], BODY, view, center, half, cull=a.cull, two_sided=a.two_sided)
            for pi, (v, f, tint, shape) in enumerate(pieces):
                col = np.array(tint, np.float32)
                if a.mode == "pen":
                    hot = np.zeros(len(v), bool)
                    if body is not None:
                        hot |= inside_mask(v, body[0], body[1])
                    for qi, (ov, of, _t, _s) in enumerate(pieces):
                        if qi != pi:
                            hot |= inside_mask(v, ov, of)
                    col = np.tile(np.array(tint, np.float32), (len(v), 1))
                    col[hot] = HOT
                elif a.mode == "diff" and ci > 0 and pi < len(ref):
                    rv = ref[pi][0]
                    if len(rv) == len(v):
                        d = np.linalg.norm(v - rv, axis=1)
                        t = np.clip(d / max(d.max(), 1e-6), 0, 1)[:, None]
                        col = (np.array(tint, np.float32)[None, :] * (1 - t) +
                               np.array(HOT, np.float32)[None, :] * t)
                draw(fr, v, f, col, view, center, half, cull=a.cull, two_sided=a.two_sided)
            axes[ri][ci].imshow(fr.image())
            axes[ri][ci].set_title(f"{label} — {view}", fontsize=13)
            axes[ri][ci].axis("off")
    fig.tight_layout()
    fig.savefig(a.out, dpi=100, facecolor="white")
    print("wrote", a.out)


if __name__ == "__main__":
    main()
