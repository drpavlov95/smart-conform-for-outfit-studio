"""Offline true-2D posterior cleft bridge for Path-B live-scene diagnosis.

This is deliberately a geometric repair spike, not a cloth-solver claim.  It
uses a real convex hull in the cross-section XY plane; the superseded C++ WIP
interpolated radius in azimuth space, which is not a Euclidean chord.

The default inputs are the exact live-scene dumps written by the DLL.  The
published dump contains vertices only, so authored topology is reused after
an exact vertex-count check.
"""

from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pathb_metrics import exact_seam_groups, integrity, load_obj
from pathb_view import Orientation, contact_sheet


def cross2(a: np.ndarray, b: np.ndarray) -> float:
    return float(a[0] * b[1] - a[1] * b[0])


def convex_hull_2d(points: np.ndarray) -> np.ndarray:
    """Andrew monotone chain; returns a CCW polygon without a repeated end."""
    if len(points) < 3:
        return np.empty((0, 2), dtype=np.float64)
    pts = np.unique(np.round(np.asarray(points, dtype=np.float64), 7), axis=0)
    if len(pts) < 3:
        return np.empty((0, 2), dtype=np.float64)
    order = np.lexsort((pts[:, 1], pts[:, 0]))
    pts = pts[order]

    def half(seq):
        out: list[np.ndarray] = []
        for p in seq:
            while len(out) >= 2 and cross2(out[-1] - out[-2], p - out[-1]) <= 1.0e-9:
                out.pop()
            out.append(p)
        return out

    lower = half(pts)
    upper = half(pts[::-1])
    hull = lower[:-1] + upper[:-1]
    return np.asarray(hull, dtype=np.float64)


def ray_hull_radius(center: np.ndarray, direction: np.ndarray, hull: np.ndarray) -> float | None:
    """Farthest positive intersection of a ray with a convex polygon."""
    if len(hull) < 3:
        return None
    best = -math.inf
    for i, a in enumerate(hull):
        b = hull[(i + 1) % len(hull)]
        edge = b - a
        den = cross2(direction, edge)
        if abs(den) <= 1.0e-10:
            continue
        rel = a - center
        t = cross2(rel, edge) / den
        u = cross2(rel, direction) / den
        if t >= -1.0e-7 and -1.0e-7 <= u <= 1.0 + 1.0e-7:
            best = max(best, t)
    return best if math.isfinite(best) and best >= 0.0 else None


def selftest() -> None:
    cloud = np.asarray([
        [-2.0, -1.0], [2.0, -1.0], [2.0, 1.0], [-2.0, 1.0],
        [0.0, 0.0], [0.5, 0.25], [-2.0, -1.0],
    ])
    hull = convex_hull_2d(cloud)
    expected = {(-2.0, -1.0), (2.0, -1.0), (2.0, 1.0), (-2.0, 1.0)}
    got = {tuple(p) for p in hull}
    if got != expected or len(hull) != 4:
        raise SystemExit(f"selftest FAILED: rectangle hull={hull.tolist()}")
    center = np.zeros(2, dtype=np.float64)
    checks = [
        (np.asarray([1.0, 0.0]), 2.0),
        (np.asarray([-1.0, 0.0]), 2.0),
        (np.asarray([0.0, 1.0]), 1.0),
        (np.asarray([0.0, -1.0]), 1.0),
    ]
    for direction, expected_radius in checks:
        actual = ray_hull_radius(center, direction, hull)
        if actual is None or abs(actual - expected_radius) > 1.0e-9:
            raise SystemExit(
                f"selftest FAILED: ray {direction.tolist()}={actual}, "
                f"expected {expected_radius}")
    print("SELFTEST PASS: true 2D hull + ray intersection")


def material_neighbors(n: int, tris: np.ndarray) -> list[np.ndarray]:
    adj = [set() for _ in range(n)]
    for tri in tris:
        a, b, c = map(int, tri)
        adj[a].update((b, c))
        adj[b].update((a, c))
        adj[c].update((a, b))
    return [np.asarray(sorted(v), dtype=np.int64) for v in adj]


def posterior_coordinates(points: np.ndarray, center: np.ndarray, back_sign: float):
    rel = points[:, :2] - center
    lateral = rel[:, 0]
    depth = back_sign * rel[:, 1]
    theta = np.arctan2(lateral, depth)
    radius = np.linalg.norm(rel, axis=1)
    return rel, lateral, depth, theta, radius


def apply_true_hull_bridge(
    cloth: np.ndarray,
    body: np.ndarray,
    tris: np.ndarray,
    seam_groups: list[list[int]],
    center: np.ndarray,
    back_sign: float,
    *,
    slab_half: float,
    slab_step: float,
    half_angle_deg: float,
    sample_angle_deg: float,
    concavity_min: float,
    cloth_dip_min: float,
    max_lift: float,
    smooth_iters: int,
    smooth_rings: int,
    body_z_fraction: float,
) -> tuple[np.ndarray, dict]:
    out = cloth.copy()
    half_angle = math.radians(half_angle_deg)
    sample_angle = math.radians(sample_angle_deg)
    tan_half = math.tan(half_angle)

    _, _, cdepth, ctheta, cradius = posterior_coordinates(out, center, back_sign)
    body_z_min = float(body[:, 2].min())
    body_z_max = float(body[:, 2].max())
    z_ceiling = body_z_min + body_z_fraction * (body_z_max - body_z_min)
    # The spine/scapula groove is another real posterior concavity.  The
    # gluteal cleft is mechanically confined to the lower body, so use the
    # same morphology-relative height guard as the production cleft detector.
    posterior = (
        (cdepth > 0.0)
        & (np.abs(ctheta) <= half_angle)
        & (out[:, 2] <= z_ceiling)
    )
    if not posterior.any():
        raise SystemExit("no cloth vertices in the verified posterior wedge")

    zmin = float(np.floor(out[posterior, 2].min()))
    zmax = float(np.ceil(out[posterior, 2].max()))
    target_sum = np.zeros(len(out), dtype=np.float64)
    target_count = np.zeros(len(out), dtype=np.int32)
    qualified_samples = 0
    max_body_concavity = 0.0

    zc = zmin
    while zc <= zmax + 1.0e-6:
        bm = np.abs(body[:, 2] - zc) <= slab_half
        cm = posterior & (np.abs(out[:, 2] - zc) <= slab_half)
        if bm.sum() < 8 or cm.sum() < 5:
            zc += slab_step
            continue

        bpts = body[bm]
        _, blat, bdepth, btheta, bradius = posterior_coordinates(bpts, center, back_sign)
        bwedge = (bdepth > 0.0) & (np.abs(btheta) <= half_angle)
        bpts = bpts[bwedge]
        btheta = btheta[bwedge]
        bradius = bradius[bwedge]
        if len(bpts) < 6:
            zc += slab_step
            continue

        cidx = np.nonzero(cm)[0]
        cpts = out[cidx]
        # Adding the radial origin closes each posterior wedge and makes the
        # ray/hull intersection deterministic without admitting arms/front.
        body_hull = convex_hull_2d(np.vstack((center, bpts[:, :2])))
        cloth_hull = convex_hull_2d(np.vstack((center, cpts[:, :2])))
        if len(body_hull) < 3 or len(cloth_hull) < 3:
            zc += slab_step
            continue

        for i in cidx:
            rel = out[i, :2] - center
            radius = float(np.linalg.norm(rel))
            if radius <= 1.0e-6:
                continue
            direction = rel / radius
            body_hr = ray_hull_radius(center, direction, body_hull)
            cloth_hr = ray_hull_radius(center, direction, cloth_hull)
            if body_hr is None or cloth_hr is None:
                continue

            delta = np.abs(np.arctan2(np.sin(btheta - ctheta[i]), np.cos(btheta - ctheta[i])))
            near = delta <= sample_angle
            if not near.any():
                continue
            body_actual = float(bradius[near].max())
            body_concavity = body_hr - body_actual
            max_body_concavity = max(max_body_concavity, body_concavity)
            if body_concavity < concavity_min:
                continue
            if cloth_hr <= radius + cloth_dip_min:
                continue

            target_sum[i] += min(cloth_hr, radius + max_lift)
            target_count[i] += 1
            qualified_samples += 1
        zc += slab_step

    lifted = target_count > 0
    seed_lift = np.zeros(len(out), dtype=np.float64)
    for i in np.nonzero(lifted)[0]:
        rel = out[i, :2] - center
        radius = float(np.linalg.norm(rel))
        target = target_sum[i] / target_count[i]
        if radius > 1.0e-6 and target > radius:
            seed_lift[i] = target - radius

    # Exact authored seams are duplicated vertex IDs.  They are one material
    # point and must receive one displacement, even when only one duplicate
    # happened to land in a sampled slab/bin.
    for group in seam_groups:
        lift = float(seed_lift[group].max(initial=0.0))
        if lift > 0.0:
            seed_lift[group] = lift
    lifted = seed_lift > 0.0

    # Smooth the OUTWARD DISPLACEMENT FIELD, not vertex positions.  Position
    # Laplacian on a sparse lifted set shrinks authored panels by several
    # units even when the actual hull correction is only a few tenths.
    neighbors = material_neighbors(len(out), tris)
    band = lifted.copy()
    frontier = lifted.copy()
    for _ in range(smooth_rings):
        nxt = np.zeros(len(out), dtype=bool)
        for i in np.nonzero(frontier)[0]:
            nxt[neighbors[i]] = True
        nxt &= ~band
        band |= nxt
        frontier = nxt
    smooth_lift = seed_lift.copy()
    for _ in range(smooth_iters):
        upd = smooth_lift.copy()
        for i in np.nonzero(band & ~lifted)[0]:
            nb = neighbors[i]
            if len(nb):
                upd[i] = 0.4 * smooth_lift[i] + 0.6 * smooth_lift[nb].mean()
        for group in seam_groups:
            # Tying after every Jacobi step also prevents unequal diffusion
            # across topology-split but geometrically welded material points.
            upd[group] = float(upd[group].mean())
        smooth_lift = upd

    affected = smooth_lift > 1.0e-6
    for i in np.nonzero(affected)[0]:
        rel = cloth[i, :2] - center
        radius = float(np.linalg.norm(rel))
        if radius > 1.0e-6:
            out[i, :2] = center + rel * ((radius + smooth_lift[i]) / radius)
    pre_smooth = cloth.copy()
    for i in np.nonzero(lifted)[0]:
        rel = cloth[i, :2] - center
        radius = float(np.linalg.norm(rel))
        if radius > 1.0e-6:
            pre_smooth[i, :2] = center + rel * ((radius + seed_lift[i]) / radius)

    displacement = np.linalg.norm(out - cloth, axis=1)
    radial_lift = np.linalg.norm(pre_smooth[:, :2] - center, axis=1) - cradius
    stats = {
        "lifted_vertices": int(lifted.sum()),
        "affected_vertices": int(affected.sum()),
        "qualified_samples": int(qualified_samples),
        "mean_radial_lift_before_smooth": float(radial_lift[lifted].mean()) if lifted.any() else 0.0,
        "max_radial_lift_before_smooth": float(radial_lift.max(initial=0.0)),
        "mean_final_displacement": float(displacement[affected].mean()) if affected.any() else 0.0,
        "max_final_displacement": float(displacement.max(initial=0.0)),
        "max_body_concavity": float(max_body_concavity),
        "body_z_ceiling": float(z_ceiling),
        "front_vertices_moved": int(((back_sign * (cloth[:, 1] - center[1]) <= 0.0) & (displacement > 1.0e-5)).sum()),
        "back_vertices_moved": int(((back_sign * (cloth[:, 1] - center[1]) > 0.0) & (displacement > 1.0e-5)).sum()),
        "z_moved_min": float(cloth[displacement > 1.0e-5, 2].min()) if (displacement > 1.0e-5).any() else None,
        "z_moved_max": float(cloth[displacement > 1.0e-5, 2].max()) if (displacement > 1.0e-5).any() else None,
    }
    return out, stats


def write_obj(path: Path, verts: np.ndarray, tris: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="ascii", newline="\n") as stream:
        for v in verts:
            stream.write(f"v {v[0]:.6f} {v[1]:.6f} {v[2]:.6f}\n")
        for tri in tris:
            stream.write(f"f {tri[0] + 1} {tri[1] + 1} {tri[2] + 1}\n")


def plane_segments(vertices: np.ndarray, tris: np.ndarray, z: float) -> np.ndarray:
    """Return exact line segments where a triangle surface crosses z."""
    segments = []
    for tri in vertices[tris]:
        dz = tri[:, 2] - z
        points = []
        for a, b in ((0, 1), (1, 2), (2, 0)):
            da, db = float(dz[a]), float(dz[b])
            if abs(da) <= 1.0e-10:
                points.append(tri[a, :2])
            if da * db < -1.0e-12:
                t = da / (da - db)
                points.append(tri[a, :2] + t * (tri[b, :2] - tri[a, :2]))
        if len(points) < 2:
            continue
        unique = np.unique(np.round(np.asarray(points), 7), axis=0)
        if len(unique) >= 2:
            # A non-coplanar triangle intersects a plane in one segment.  If
            # z passes exactly through a vertex, choose the farthest pair.
            best = None
            best_d2 = -1.0
            for i in range(len(unique)):
                for j in range(i + 1, len(unique)):
                    d2 = float(np.sum((unique[i] - unique[j]) ** 2))
                    if d2 > best_d2:
                        best_d2 = d2
                        best = (unique[i], unique[j])
            if best is not None and best_d2 > 1.0e-14:
                segments.append(best)
    return np.asarray(segments, dtype=np.float64)


def posterior_profile(
    vertices: np.ndarray,
    tris: np.ndarray,
    center: np.ndarray,
    back_sign: float,
    z_values: np.ndarray,
    lateral_lo: float,
    lateral_hi: float,
) -> np.ndarray:
    """Exact posterior envelope of triangle/plane intersections in an x band."""
    result = np.full(len(z_values), np.nan, dtype=np.float64)
    intervals = [(-lateral_hi, lateral_hi)] if lateral_lo <= 0.0 else [
        (-lateral_hi, -lateral_lo), (lateral_lo, lateral_hi)]
    for i, z in enumerate(z_values):
        best = -math.inf
        for segment in plane_segments(vertices, tris, float(z)):
            lateral = segment[:, 0] - center[0]
            depth = back_sign * (segment[:, 1] - center[1])
            dx = float(lateral[1] - lateral[0])
            dd = float(depth[1] - depth[0])
            for lo, hi in intervals:
                if abs(dx) <= 1.0e-12:
                    if lo <= lateral[0] <= hi:
                        best = max(best, float(depth.max()))
                    continue
                ta = (lo - lateral[0]) / dx
                tb = (hi - lateral[0]) / dx
                enter = max(0.0, min(ta, tb))
                leave = min(1.0, max(ta, tb))
                if enter <= leave:
                    best = max(best,
                               float(depth[0] + enter * dd),
                               float(depth[0] + leave * dd))
        if math.isfinite(best) and best > 0.0:
            result[i] = best
    return result


def focused_diagnostic(
    path: Path,
    orientation: Orientation,
    center: np.ndarray,
    body: np.ndarray,
    body_tris: np.ndarray,
    production: np.ndarray,
    current: np.ndarray,
    candidate: np.ndarray,
    garment_tris: np.ndarray,
    scene_label: str = "LIVE",
) -> None:
    """Magnify the pelvis and separate horizontal cleft from vertical tuck."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.lines import Line2D

    back_sign = -orientation.front_sign
    z_slices = (42.0, 48.0, 54.0, 60.0)
    series = [
        ("body", body, body_tris, "#777777", 1.15, "-"),
        ("production", production, garment_tris, "#2c7fb8", 0.9, ":"),
        ("current", current, garment_tris, "#d95f0e", 1.25, "-"),
        ("true hull", candidate, garment_tris, "#31a354", 1.25, "--"),
    ]
    fig, axes = plt.subplots(2, 4, figsize=(15.5, 8.5))
    for ax, z in zip(axes[0], z_slices):
        for _, vertices, tris, color, width, linestyle in series:
            segments = plane_segments(vertices, tris, z)
            for seg in segments:
                lateral = seg[:, 0] - center[0]
                depth = back_sign * (seg[:, 1] - center[1])
                keep = np.max(depth) >= -1.0
                if keep:
                    ax.plot(lateral, depth, color=color, lw=width,
                            linestyle=linestyle, alpha=0.9)
        ax.axvline(0.0, color="#bbbbbb", lw=0.6)
        ax.set_xlim(-16.0, 16.0)
        ax.set_ylim(-1.0, 21.0)
        ax.set_aspect("equal", adjustable="box")
        ax.set_title(f"XY section at z={z:.0f}")
        ax.set_xlabel("lateral x")
        ax.grid(alpha=0.18)
    axes[0, 0].set_ylabel("posterior depth (BACK is up)")

    z_values = np.arange(34.0, 76.01, 0.5)
    profile_bands = [(0.0, 3.0, "central cleft |x|<3"),
                     (5.0, 11.0, "buttock/flank 5<|x|<11")]
    for ax, (lo, hi, title) in zip(axes[1, :2], profile_bands):
        for label, vertices, _, color, width, linestyle in series:
            profile = posterior_profile(
                vertices, body_tris if label == "body" else garment_tris,
                center, back_sign, z_values, lo, hi)
            ax.plot(profile, z_values, label=label, color=color, lw=width,
                    linestyle=linestyle)
        ax.invert_xaxis()
        ax.set_title(title)
        ax.set_xlabel("posterior depth (farther left = farther back)")
        ax.set_ylabel("z")
        ax.grid(alpha=0.22)

    displacement = np.linalg.norm(candidate - current, axis=1)
    moved = displacement > 1.0e-6
    ax = axes[1, 2]
    if moved.any():
        rel = current[moved, :2] - center
        scatter = ax.scatter(
            rel[:, 0], current[moved, 2], c=displacement[moved],
            cmap="viridis", vmin=0.0, vmax=max(0.35, float(displacement.max())),
            s=26, edgecolors="black", linewidths=0.25)
        fig.colorbar(scatter, ax=ax, label="outward displacement")
    ax.set_title("vertices affected by hull bridge")
    ax.set_xlabel("lateral x")
    ax.set_ylabel("z")
    ax.grid(alpha=0.22)

    ax = axes[1, 3]
    ax.axis("off")
    ax.text(
        0.02, 0.96,
        "What this sheet separates\n\n"
        "Top: true horizontal XY sections.\n"
        "A central dip here is gluteal-cleft bridging.\n\n"
        "Bottom-left: vertical posterior envelopes.\n"
        "A cloth curve that tracks the body after contact\n"
        "is the under-butt tuck / residual magnetism.\n\n"
        "Green-current overlap means the true hull is\n"
        "geometrically correct but too small to be the\n"
        "dominant visible defect.",
        va="top", fontsize=10)
    handles = [Line2D([0], [0], color=s[3], lw=1.8, linestyle=s[5], label=s[0])
               for s in series]
    fig.legend(handles=handles, loc="upper center", ncol=4,
               bbox_to_anchor=(0.5, 1.01))
    fig.suptitle(f"{scene_label} pelvis diagnosis - {orientation.label()}", y=1.035)
    fig.tight_layout()
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)


def main() -> int:
    temp = Path(os.environ.get("TEMP", "."))
    parser = argparse.ArgumentParser()
    parser.add_argument("--body", type=Path, default=temp / "drape_live_body.obj")
    parser.add_argument("--source", type=Path, default=temp / "drape_live_published.obj")
    parser.add_argument("--topology", type=Path, default=temp / "drape_live_authored.obj")
    parser.add_argument("--production", type=Path, default=temp / "drape_live_production.obj")
    parser.add_argument("--output", type=Path, default=Path("tmp/pathb/live_true_hull.obj"))
    parser.add_argument("--sheet", type=Path, default=Path("tmp/pathb/LIVE_true_hull_sheet.png"))
    parser.add_argument("--focused-sheet", type=Path,
                        default=Path("tmp/pathb/LIVE_true_hull_focused.png"))
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--slab-half", type=float, default=1.5)
    parser.add_argument("--slab-step", type=float, default=1.0)
    parser.add_argument("--half-angle", type=float, default=52.0)
    parser.add_argument("--sample-angle", type=float, default=5.0)
    parser.add_argument("--concavity-min", type=float, default=0.45)
    parser.add_argument("--cloth-dip-min", type=float, default=0.05)
    parser.add_argument("--max-lift", type=float, default=2.5)
    parser.add_argument("--smooth-iters", type=int, default=4)
    parser.add_argument("--smooth-rings", type=int, default=2)
    parser.add_argument("--body-z-fraction", type=float, default=0.55)
    args = parser.parse_args()

    if args.selftest:
        selftest()
        return 0

    body, body_tris = load_obj(args.body)
    source, source_tris = load_obj(args.source)
    topo_verts, tris = load_obj(args.topology)
    production, production_tris = load_obj(args.production)
    if len(source) != len(topo_verts) or len(source) != len(production):
        raise SystemExit("live garment vertex counts do not match")
    if len(source_tris) and not np.array_equal(source_tris, tris):
        raise SystemExit("source topology differs from authored topology")
    if len(production_tris) and not np.array_equal(production_tris, tris):
        raise SystemExit("production topology differs from authored topology")

    orientation = Orientation.detect(body)
    back_sign = -orientation.front_sign
    center = 0.5 * (body[:, :2].min(axis=0) + body[:, :2].max(axis=0))
    print(f"orientation={orientation.label()} center=({center[0]:.4f},{center[1]:.4f})")
    seam_groups = exact_seam_groups(topo_verts)
    candidate, stats = apply_true_hull_bridge(
        source, body, tris, seam_groups, center, back_sign,
        slab_half=args.slab_half,
        slab_step=args.slab_step,
        half_angle_deg=args.half_angle,
        sample_angle_deg=args.sample_angle,
        concavity_min=args.concavity_min,
        cloth_dip_min=args.cloth_dip_min,
        max_lift=args.max_lift,
        smooth_iters=args.smooth_iters,
        smooth_rings=args.smooth_rings,
        body_z_fraction=args.body_z_fraction,
    )
    for key, value in stats.items():
        print(f"{key}={value}")
    if stats["front_vertices_moved"]:
        raise SystemExit("posterior-only contract violated: front vertices moved")

    write_obj(args.output, candidate, tris)
    source_integrity = integrity(source, topo_verts, tris, seam_groups)
    candidate_integrity = integrity(candidate, topo_verts, tris, seam_groups)
    print(f"source_integrity={source_integrity}")
    print(f"candidate_integrity={candidate_integrity}")
    if candidate_integrity["flipped_faces"] > source_integrity["flipped_faces"]:
        raise SystemExit("candidate introduced face flips")
    if candidate_integrity["degenerate_faces"] > source_integrity["degenerate_faces"]:
        raise SystemExit("candidate introduced degenerate faces")
    if candidate_integrity["worst_seam_split"] > source_integrity["worst_seam_split"] + 1.0e-4:
        raise SystemExit("candidate worsened an exact authored seam")
    args.sheet.parent.mkdir(parents=True, exist_ok=True)
    contact_sheet(
        str(args.sheet), orientation,
        [("production", production), ("current live", source), ("true 2D hull", candidate)],
        tris, body, body_tris,
        views=["front", "back", "left_profile", "back_3q"],
    )
    focused_diagnostic(
        args.focused_sheet, orientation, center,
        body, body_tris, production, source, candidate, tris)
    print(f"wrote={args.output}")
    print(f"sheet={args.sheet}")
    print(f"focused_sheet={args.focused_sheet}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
