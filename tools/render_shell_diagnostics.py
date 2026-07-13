#!/usr/bin/env python3
"""Render profile diagnostics for static-shell spike OBJ candidates."""

from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

from shell_spike import load_obj


def profile_y_by_z(
    v: np.ndarray, mask: np.ndarray, outward_sign: float, z_step: float = 2.0
):
    rows = []
    if not np.any(mask):
        return np.empty(0), np.empty(0)
    lo = np.floor(v[mask, 2].min() / z_step) * z_step
    hi = np.ceil(v[mask, 2].max() / z_step) * z_step
    for z0 in np.arange(lo, hi + 0.5 * z_step, z_step):
        m = mask & (v[:, 2] >= z0) & (v[:, 2] < z0 + z_step)
        if np.any(m):
            extreme = np.max(outward_sign * v[m, 1]) * outward_sign
            rows.append((float(np.median(v[m, 2])), float(extreme)))
    if not rows:
        return np.empty(0), np.empty(0)
    z, y = np.asarray(rows).T
    return y, z


def top_surface_xy(
    v: np.ndarray, zlo: float, zhi: float, outward_sign: float, x_step: float = 1.5
):
    mask = (
        (v[:, 2] >= zlo)
        & (v[:, 2] < zhi)
        & (outward_sign * v[:, 1] > -2.0)
    )
    rows = []
    for x0 in np.arange(-18.0, 18.0, x_step):
        m = mask & (v[:, 0] >= x0) & (v[:, 0] < x0 + x_step)
        if np.any(m):
            extreme = np.max(outward_sign * v[m, 1]) * outward_sign
            rows.append((float(np.median(v[m, 0])), float(extreme)))
    if not rows:
        return np.empty(0), np.empty(0)
    return np.asarray(rows).T


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("output", type=Path)
    ap.add_argument("--body", type=Path, required=True)
    ap.add_argument("--body-scale", type=float, default=1.0)
    ap.add_argument("--side", choices=("front", "rear"), default="front")
    ap.add_argument("--mesh-scale", type=float, default=1.0)
    ap.add_argument("meshes", nargs="+", help="LABEL=path.obj")
    args = ap.parse_args()

    outward_sign = 1.0 if args.side == "front" else -1.0
    body = load_obj(args.body).verts * args.body_scale
    entries = []
    for spec in args.meshes:
        label, path = spec.split("=", 1)
        scale = args.mesh_scale
        if "@" in path:
            scale_text, path = path.split("@", 1)
            scale = float(scale_text)
        entries.append((label, load_obj(Path(path)).verts * scale))

    colors = plt.cm.tab10(np.linspace(0.0, 1.0, max(10, len(entries))))
    fig, axes = plt.subplots(2, 3, figsize=(18, 10))

    for ax, xlo, xhi, title in (
        (axes[0, 0], 3.0, 8.0, "perfil da nádega |x|=3..8"),
        (axes[0, 1], 0.0, 1.5, "perfil da costura |x|<1.5"),
    ):
        bm = (outward_sign * body[:, 1] > 0.0) & (np.abs(body[:, 0]) >= xlo) & (np.abs(body[:, 0]) < xhi)
        y, z = profile_y_by_z(body, bm, outward_sign)
        ax.plot(y, z, "k--", linewidth=1.0, label="corpo")
        for color, (label, v) in zip(colors, entries):
            m = (outward_sign * v[:, 1] > 0.0) & (np.abs(v[:, 0]) >= xlo) & (np.abs(v[:, 0]) < xhi)
            y, z = profile_y_by_z(v, m, outward_sign)
            ax.plot(y, z, marker="o", markersize=2, linewidth=1.5, color=color, label=label)
        ax.set_title(title)
        ax.set_xlabel("y (costas para fora →)")
        ax.set_ylabel("z")
        ax.set_ylim(25, 82)
        ax.grid(alpha=0.25)
        ax.legend(fontsize=8)

    axes[0, 2].axis("off")
    for ax, (zlo, zhi) in zip(axes[1], ((44, 50), (52, 58), (60, 66))):
        x, y = top_surface_xy(body, zlo, zhi, outward_sign)
        ax.plot(x, y, "k--", linewidth=1.0, label="corpo")
        for color, (label, v) in zip(colors, entries):
            x, y = top_surface_xy(v, zlo, zhi, outward_sign)
            ax.plot(x, y, marker="o", markersize=2, linewidth=1.3, color=color, label=label)
        ax.set_title(f"vista superior z={zlo}..{zhi}")
        ax.set_xlabel("x")
        ax.set_ylabel("y")
        ax.grid(alpha=0.25)
        ax.legend(fontsize=7)

    fig.tight_layout()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.output, dpi=120, facecolor="white")
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
