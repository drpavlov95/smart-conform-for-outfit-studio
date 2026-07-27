#!/usr/bin/env python3
"""Does free cloth HANG, or does it wrap the body? Radial profile vs the plumb line.

Written because penetration count cannot answer this and actively misleads. On
2026-07-26 admitting Fur 1 BaseArmor to the cloth path took its body
penetrations from 25 to 1 and the user looked at the result and said "o tecido
gruda demais no corpo, nao tem caimento nenhum". Both are true: a garment
shrink-wrapped onto the body has almost no penetration BY CONSTRUCTION. Low
penetration rewards hugging.

The physical law this measures instead:

    Cloth hanging under gravity cannot be NARROWER than the widest thing it has
    already draped over, above it.

Nothing pulls a free panel inward. Once the fabric clears the hip, the hem can
hang straight down or swing out, but it cannot tuck back in against the thigh
unless something holds it there. So take the cloth's horizontal radius per
height band, sweep DOWNWARD keeping a running maximum, and any band that sits
inside that running maximum is holding a shape gravity did not give it.

    tuckBelowPlumb   how far, in units, the cloth falls inside its own plumb
                     envelope. 0 = it hangs. Large = it wraps.
    bodyFollow       correlation between the cloth radius profile and the body
                     radius profile over the same bands. ~1 = the garment is an
                     offset copy of the body silhouette; that is the "skintight"
                     look, and it is what the user described at the belly, groin
                     and iliac fossa.

Both are computed over FREE cloth only (bands below the support), read no hand
sculpt, and know nothing about which garment this is.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np


def read_obj(path: Path):
    v, tris = [], []
    with open(path, "r") as f:
        for line in f:
            if line.startswith("v "):
                p = line.split()
                v.append((float(p[1]), float(p[2]), float(p[3])))
            elif line.startswith("f "):
                idx = [int(t.split("/")[0]) - 1 for t in line.split()[1:]]
                for k in range(1, len(idx) - 1):
                    tris.append((idx[0], idx[k], idx[k + 1]))
    return np.asarray(v, dtype=np.float64), np.asarray(tris, dtype=np.int64)


def radius_profile(verts: np.ndarray, axis_xy: np.ndarray, zlo: float, zhi: float,
                   bands: int, pct: float = 90.0):
    """Per-band horizontal radius from the vertical axis.

    A high percentile rather than the max: one stray spike must not define the
    silhouette, which is the whole point of separating this from the outward
    audit.
    """
    edges = np.linspace(zlo, zhi, bands + 1)
    r = np.hypot(verts[:, 0] - axis_xy[0], verts[:, 1] - axis_xy[1])
    out = np.full(bands, np.nan)
    for b in range(bands):
        m = (verts[:, 2] >= edges[b]) & (verts[:, 2] < edges[b + 1])
        if m.sum() >= 4:
            out[b] = np.percentile(r[m], pct)
    centres = 0.5 * (edges[:-1] + edges[1:])
    return centres, out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--garment", required=True, type=Path, nargs="+",
                    help="one or more result OBJs to compare, in order")
    ap.add_argument("--label", nargs="+", default=None)
    ap.add_argument("--body", required=True, type=Path)
    ap.add_argument("--bands", type=int, default=28)
    ap.add_argument("--profile", action="store_true", help="print the band table")
    args = ap.parse_args()

    body, _ = read_obj(args.body)
    axis = np.array([np.median(body[:, 0]), np.median(body[:, 1])])

    labels = args.label or [p.stem for p in args.garment]
    results = []
    for path, label in zip(args.garment, labels):
        g, _ = read_obj(path)
        zlo, zhi = float(g[:, 2].min()), float(g[:, 2].max())
        zc, gr = radius_profile(g, axis, zlo, zhi, args.bands)
        _, br = radius_profile(body, axis, zlo, zhi, args.bands)

        valid = ~np.isnan(gr)
        # Plumb envelope: running max sweeping DOWNWARD (high z -> low z).
        env = np.full_like(gr, np.nan)
        run = -np.inf
        for i in range(len(gr) - 1, -1, -1):
            if np.isnan(gr[i]):
                continue
            run = max(run, gr[i])
            env[i] = run
        tuck = np.where(valid, env - gr, np.nan)
        # Only the lower half can meaningfully "hang"; above the widest point
        # the garment is still being supported.
        widest = int(np.nanargmax(gr))
        below = np.zeros(len(gr), dtype=bool)
        below[:widest] = True
        below &= valid

        tuck_mean = float(np.nanmean(tuck[below])) if below.any() else 0.0
        tuck_max = float(np.nanmax(tuck[below])) if below.any() else 0.0
        both = valid & ~np.isnan(br)
        follow = (float(np.corrcoef(gr[both], br[both])[0, 1])
                  if both.sum() > 3 else float("nan"))
        results.append((label, tuck_mean, tuck_max, follow, zc, gr, br, env, widest))

    print(f"{'config':<26}{'tuck medio':>13}{'tuck max':>11}{'segue o corpo':>16}")
    print("-" * 68)
    for label, tm, tx, fo, *_ in results:
        print(f"{label:<26}{tm:>13.3f}{tx:>11.3f}{fo:>16.3f}")
    print("\n  tuck  = quanto o pano entra pra dentro do proprio prumo (0 = pendura)")
    print("  segue = correlacao com a silhueta do corpo (~1 = molde do corpo)")

    if args.profile:
        for label, tm, tx, fo, zc, gr, br, env, widest in results:
            print(f"\n--- {label} (banda mais larga: z={zc[widest]:.1f}) ---")
            print(f"{'z':>8}{'raio pano':>12}{'prumo':>9}{'tuck':>8}{'raio corpo':>12}")
            for i in range(len(zc) - 1, -1, -1):
                if np.isnan(gr[i]):
                    continue
                mark = " <=" if (i < widest and (env[i] - gr[i]) > 0.5) else ""
                print(f"{zc[i]:>8.1f}{gr[i]:>12.2f}{env[i]:>9.2f}"
                      f"{env[i]-gr[i]:>8.2f}{br[i]:>12.2f}{mark}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
