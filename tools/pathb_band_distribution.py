#!/usr/bin/env python3
"""S0.3e material-band section diagnostics.

This is a measurement tool, not a constraint.  It compares one or more
candidates with the same authored sculpt on material iso-contours cut from the
authored base mesh.  Every reported sample has the same triangle/fraction in
the candidate and sculpt, so the correspondence is material rather than a
nearest-point proxy.

Compared with the retired S0.3d endpoint-cloud metric this version:

* orients the material PCA axis deterministically toward world +Z and always
  reports world-space dX/dY/dZ plus dDown=-dZ;
* integrates each contour segment with two-point Gauss quadrature, weighted by
  authored segment length (shared crossings and triangulation density no
  longer bias an unweighted point mean);
* reports paired, material-correspondent shape residuals after removing center
  and scale, including within-band axial variation;
* retains an eight-sector summary only as a coarse diagnostic and reports its
  coverage instead of silently discarding empty sectors;
* accepts repeated --candidate LABEL=OBJ arguments so production and Path-B
  shells are characterized side by side.

Usage:
  py -3 tools/pathb_band_distribution.py --base BASE.obj --sculpt GT.obj \
      --candidate production=PROD.obj --candidate shell_base=SHELL.obj
  py -3 tools/pathb_band_distribution.py --selftest
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path

import numpy as np

from pathb_band_descriptor import components, load_obj, weld


WORLD_UP = np.array([0.0, 0.0, 1.0], dtype=np.float64)
GAUSS_T = np.array(
    [0.5 - 0.5 / math.sqrt(3.0), 0.5 + 0.5 / math.sqrt(3.0)],
    dtype=np.float64,
)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def oriented_principal_axis(points: np.ndarray) -> np.ndarray:
    """Principal material axis with a deterministic world-space sign.

    For an axis with a meaningful vertical component, +axis always points
    toward world +Z.  Near-horizontal components use the sign of their largest
    world component as a deterministic fallback.  The raw PCA sign is never
    interpreted as gravity.
    """
    axis, _values, _eigengap = principal_axis_diagnostics(points)
    return axis


def principal_axis_diagnostics(points: np.ndarray) -> tuple[np.ndarray, np.ndarray, float]:
    center = points.mean(axis=0)
    values, vectors = np.linalg.eigh(np.cov((points - center).T))
    axis = vectors[:, -1].astype(np.float64)
    if abs(float(axis[2])) > 1.0e-8:
        if axis[2] < 0.0:
            axis = -axis
    else:
        dominant = int(np.argmax(np.abs(axis)))
        if axis[dominant] < 0.0:
            axis = -axis
    axis /= max(float(np.linalg.norm(axis)), 1.0e-30)
    eigengap = float((values[-1] - values[-2]) / max(abs(float(values[-1])), 1.0e-30))
    return axis, values.astype(np.float64), eigengap


def perpendicular_basis(axis: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Deterministic world-aligned basis perpendicular to ``axis``."""
    seed = np.array([1.0, 0.0, 0.0], dtype=np.float64)
    e1 = seed - float(seed @ axis) * axis
    if np.linalg.norm(e1) < 1.0e-8:
        seed = np.array([0.0, 1.0, 0.0], dtype=np.float64)
        e1 = seed - float(seed @ axis) * axis
    e1 /= max(float(np.linalg.norm(e1)), 1.0e-30)
    e2 = np.cross(axis, e1)
    e2 /= max(float(np.linalg.norm(e2)), 1.0e-30)
    return e1, e2


def material_contour_samples(
    wbase: np.ndarray,
    meshes: dict[str, np.ndarray],
    triangles: np.ndarray,
    s_norm: np.ndarray,
    level: float,
) -> tuple[np.ndarray, dict[str, np.ndarray]]:
    """Return fixed-material quadrature weights and paired contour samples.

    Every straddling authored triangle contributes one iso-segment.  Two-point
    Gauss samples exactly integrate linear position and quadratic moments along
    that segment.  Weights are measured in the authored base, so deformation
    cannot change the sampled material population.
    """
    weights: list[float] = []
    samples: dict[str, list[np.ndarray]] = {name: [] for name in meshes}
    for tri in triangles:
        s = s_norm[tri]
        above = s > level
        count = int(above.sum())
        if count == 0 or count == 3:
            continue
        lone = int(np.flatnonzero(above == (count == 1))[0])
        others = [local for local in range(3) if local != lone]
        material_edges: list[tuple[int, int, float]] = []
        for other in others:
            a, b = int(tri[lone]), int(tri[other])
            denom = float(s_norm[b] - s_norm[a])
            fraction = 0.5 if abs(denom) < 1.0e-12 else (level - s_norm[a]) / denom
            material_edges.append((a, b, float(np.clip(fraction, 0.0, 1.0))))

        base_endpoints = [
            wbase[a] + fraction * (wbase[b] - wbase[a])
            for a, b, fraction in material_edges
        ]
        authored_length = float(np.linalg.norm(base_endpoints[1] - base_endpoints[0]))
        if authored_length <= 1.0e-12:
            continue
        for gauss_t in GAUSS_T:
            weights.append(0.5 * authored_length)
            for name, verts in meshes.items():
                endpoints = [
                    verts[a] + fraction * (verts[b] - verts[a])
                    for a, b, fraction in material_edges
                ]
                samples[name].append(
                    endpoints[0] + gauss_t * (endpoints[1] - endpoints[0])
                )
    if not weights:
        return np.empty(0, dtype=np.float64), {
            name: np.empty((0, 3), dtype=np.float64) for name in meshes
        }
    return np.asarray(weights, dtype=np.float64), {
        name: np.asarray(points, dtype=np.float64) for name, points in samples.items()
    }


def weighted_mean(values: np.ndarray, weights: np.ndarray) -> np.ndarray:
    return np.sum(values * weights.reshape((-1,) + (1,) * (values.ndim - 1)), axis=0) / max(
        float(weights.sum()), 1.0e-30
    )


def weighted_median(values: np.ndarray, weights: np.ndarray) -> float:
    order = np.argsort(values, kind="stable")
    ordered_v = values[order]
    ordered_w = weights[order]
    cutoff = 0.5 * float(ordered_w.sum())
    return float(ordered_v[min(int(np.searchsorted(np.cumsum(ordered_w), cutoff)), len(order) - 1)])


def section_stats(
    points: np.ndarray,
    weights: np.ndarray,
    axis: np.ndarray,
    e1: np.ndarray,
    e2: np.ndarray,
) -> dict[str, object] | None:
    if len(points) < 2 or float(weights.sum()) <= 1.0e-12:
        return None
    center = weighted_mean(points, weights)
    relative = points - center
    axial = relative @ axis
    perpendicular = relative - axial[:, None] * axis
    radius = np.linalg.norm(perpendicular, axis=1)
    width_rms = math.sqrt(float(np.sum(weights * radius * radius) / weights.sum()))
    x = relative @ e1
    y = relative @ e2
    angles = np.arctan2(y, x)
    sectors: list[float] = []
    sector_weights: list[float] = []
    for sector in range(8):
        lo = -math.pi + sector * math.pi / 4.0
        mask = (angles >= lo) & (angles < lo + math.pi / 4.0)
        sector_weights.append(float(weights[mask].sum()))
        sectors.append(weighted_median(radius[mask], weights[mask]) if mask.any() else float("nan"))
    return {
        "center": center,
        "relative": relative,
        "axial": axial,
        "perpendicular": perpendicular,
        "width_rms": width_rms,
        "sectors": np.asarray(sectors, dtype=np.float64),
        "sector_weights": np.asarray(sector_weights, dtype=np.float64),
    }


def paired_row(
    candidate: dict[str, object],
    sculpt: dict[str, object],
    weights: np.ndarray,
    axis: np.ndarray,
) -> dict[str, object]:
    candidate_center = np.asarray(candidate["center"])
    sculpt_center = np.asarray(sculpt["center"])
    delta = sculpt_center - candidate_center
    delta_axis = float(delta @ axis)
    delta_perp = float(np.linalg.norm(delta - delta_axis * axis))
    candidate_width = max(float(candidate["width_rms"]), 1.0e-12)
    sculpt_width = max(float(sculpt["width_rms"]), 1.0e-12)

    candidate_perp = np.asarray(candidate["perpendicular"]) / candidate_width
    sculpt_perp = np.asarray(sculpt["perpendicular"]) / sculpt_width
    perp_difference = sculpt_perp - candidate_perp
    shape_perp_rms = math.sqrt(
        float(np.sum(weights * np.einsum("ij,ij->i", perp_difference, perp_difference)) / weights.sum())
    )

    mean_width = 0.5 * (candidate_width + sculpt_width)
    axial_difference = np.asarray(sculpt["axial"]) - np.asarray(candidate["axial"])
    shape_axial_rms = math.sqrt(
        float(np.sum(weights * axial_difference * axial_difference) / weights.sum())
    ) / max(mean_width, 1.0e-12)

    centered_difference = np.asarray(sculpt["relative"]) - np.asarray(candidate["relative"])
    correspondence_centered_rms = math.sqrt(
        float(
            np.sum(weights * np.einsum("ij,ij->i", centered_difference, centered_difference))
            / weights.sum()
        )
    )

    candidate_sectors = np.asarray(candidate["sectors"])
    sculpt_sectors = np.asarray(sculpt["sectors"])
    common = np.isfinite(candidate_sectors) & np.isfinite(sculpt_sectors)
    candidate_coverage = int(np.isfinite(candidate_sectors).sum())
    sculpt_coverage = int(np.isfinite(sculpt_sectors).sum())
    # Eight-sector shape is only comparable when the complete projected
    # section is represented in both meshes.  Partial coverage is reported but
    # never silently summarized into a reassuring scalar.
    if int(common.sum()) == 8:
        normalized_candidate = candidate_sectors[common] / candidate_width
        normalized_sculpt = sculpt_sectors[common] / sculpt_width
        sector_shape_rms = float(
            np.sqrt(np.mean((normalized_sculpt - normalized_candidate) ** 2))
        )
    else:
        sector_shape_rms = None

    return {
        "delta_center": [float(value) for value in delta],
        "delta_center_norm": float(np.linalg.norm(delta)),
        "delta_xy": float(np.linalg.norm(delta[:2])),
        "delta_z": float(delta[2]),
        "delta_down": float(-delta[2]),
        "delta_axis": delta_axis,
        "delta_perp": delta_perp,
        "width_candidate": candidate_width,
        "width_sculpt": sculpt_width,
        "width_ratio": sculpt_width / candidate_width,
        "correspondence_centered_rms": correspondence_centered_rms,
        "shape_perp_rms": shape_perp_rms,
        "shape_axial_rms": shape_axial_rms,
        "sector_shape_rms": sector_shape_rms,
        "sector_coverage_candidate": candidate_coverage,
        "sector_coverage_sculpt": sculpt_coverage,
        "sector_coverage_common": int(common.sum()),
    }


def analyze_arrays(
    base_verts: np.ndarray,
    triangles: np.ndarray,
    sculpt_verts: np.ndarray,
    candidates: dict[str, np.ndarray],
    levels: list[float],
    component: int | None = None,
) -> dict[str, object]:
    expected = len(base_verts)
    if len(sculpt_verts) != expected or any(len(verts) != expected for verts in candidates.values()):
        raise ValueError("base, sculpt and every candidate must have identical vertex counts")

    remap, canonical = weld(base_verts, triangles)
    welded_triangles = remap[triangles]
    welded_base = base_verts[canonical]
    welded_sculpt = sculpt_verts[canonical]
    welded_candidates = {name: verts[canonical] for name, verts in candidates.items()}
    component_list = components(len(canonical), welded_triangles)
    component_of = np.full(len(canonical), -1, dtype=np.int64)
    for component_index, component_vertices in enumerate(component_list):
        component_of[np.asarray(component_vertices, dtype=np.int64)] = component_index
    component_sizes = [len(values) for values in component_list]
    if component is None:
        component_index = int(np.argmax(component_sizes))
        component_selection = "largest"
    else:
        if component < 0 or component >= len(component_list):
            raise ValueError(
                f"component index {component} outside [0,{len(component_list) - 1}]"
            )
        component_index = int(component)
        component_selection = "explicit"
    component_vertices = np.asarray(component_list[component_index], dtype=np.int64)
    component_triangles = welded_triangles[
        component_of[welded_triangles[:, 0]] == component_index
    ]
    authored_points = welded_base[component_vertices]
    authored_center = authored_points.mean(axis=0)
    axis, eigenvalues, eigengap = principal_axis_diagnostics(authored_points)
    e1, e2 = perpendicular_basis(axis)
    material_coordinate = (welded_base - authored_center) @ axis
    material_min = float(material_coordinate[component_vertices].min())
    material_max = float(material_coordinate[component_vertices].max())
    material_s = (material_coordinate - material_min) / max(material_max - material_min, 1.0e-12)

    result: dict[str, object] = {
        "component": component_index,
        "component_selection": component_selection,
        "component_sizes": component_sizes,
        "component_vertices": int(len(component_vertices)),
        "axis": [float(value) for value in axis],
        "axis_orientation": "world_+Z",
        "axis_up_cos": float(axis @ WORLD_UP),
        "axis_eigenvalues": [float(value) for value in eigenvalues],
        "axis_relative_eigengap": eigengap,
        "weld_tolerance": 0.003,
        "levels": {},
    }
    meshes = {"sculpt": welded_sculpt, **welded_candidates}
    for level in levels:
        weights, samples = material_contour_samples(
            welded_base, meshes, component_triangles, material_s, level
        )
        sculpt_stats = section_stats(samples["sculpt"], weights, axis, e1, e2)
        if sculpt_stats is None:
            continue
        level_result: dict[str, object] = {
            "level": float(level),
            "authored_arc_weight": float(weights.sum()),
            "quadrature_samples": int(len(weights)),
            "candidates": {},
        }
        for name in candidates:
            candidate_stats = section_stats(samples[name], weights, axis, e1, e2)
            if candidate_stats is None:
                continue
            level_result["candidates"][name] = paired_row(
                candidate_stats, sculpt_stats, weights, axis
            )
        result["levels"][f"{level:.6f}"] = level_result
    return result


def print_result(result: dict[str, object]) -> None:
    axis = np.asarray(result["axis"])
    print(
        f"# S0.3e material-band diagnostics component={result['component']} "
        f"({result['component_vertices']} verts) axis={np.round(axis, 6)} "
        f"orientation={result['axis_orientation']} upCos={result['axis_up_cos']:.4f} "
        f"eigengap={result['axis_relative_eigengap']:.4f}"
    )
    print(
        f"{'candidate':>14} {'s':>5} {'dX':>8} {'dY':>8} {'dZ':>8} "
        f"{'dDown':>8} {'dAxis':>8} {'dPerp':>8} {'wRatio':>8} "
        f"{'corrRMS':>8} {'shapeP':>8} {'shapeA':>8} {'sectRMS':>8} {'cover':>7}"
    )
    levels = result["levels"]
    for level_text, level_result in levels.items():
        for name, row in level_result["candidates"].items():
            dx, dy, dz = row["delta_center"]
            sector_rms = row["sector_shape_rms"]
            sector_text = "n/a" if sector_rms is None else f"{sector_rms:.3f}"
            print(
                f"{name:>14} {float(level_text):5.2f} {dx:8.3f} {dy:8.3f} {dz:8.3f} "
                f"{row['delta_down']:8.3f} {row['delta_axis']:8.3f} {row['delta_perp']:8.3f} "
                f"{row['width_ratio']:8.3f} {row['correspondence_centered_rms']:8.3f} "
                f"{row['shape_perp_rms']:8.3f} {row['shape_axial_rms']:8.3f} "
                f"{sector_text:>8} {row['sector_coverage_common']:>3}/8"
            )
    print(
        "\nDelta is sculpt - candidate in world space. dDown=-dZ: positive means the "
        "sculpt is lower. dAxis uses the printed +Z-oriented material axis and is "
        "never treated as gravity. shapeP/shapeA use paired material samples after "
        "removing center (and perpendicular scale for shapeP)."
    )


def parse_candidate(value: str) -> tuple[str, Path]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("candidate must be LABEL=OBJ")
    label, raw_path = value.split("=", 1)
    label = label.strip()
    if not label:
        raise argparse.ArgumentTypeError("candidate label must not be empty")
    return label, Path(raw_path)


def synthetic_tube(nz: int = 6, nr: int = 24, flip: bool = False):
    z_values = np.linspace(0.0, 10.0, nz)
    angles = np.linspace(0.0, 2.0 * math.pi, nr, endpoint=False)
    base = np.asarray(
        [[2.0 * math.cos(angle), 2.0 * math.sin(angle), z] for z in z_values for angle in angles],
        dtype=np.float64,
    )
    triangles: list[list[int]] = []
    for zi in range(nz - 1):
        for ai in range(nr):
            aj = (ai + 1) % nr
            a = zi * nr + ai
            b = zi * nr + aj
            c = (zi + 1) * nr + ai
            d = (zi + 1) * nr + aj
            triangles.extend([[a, b, d], [a, d, c]] if flip else [[a, b, c], [b, d, c]])
    candidate = base.copy()
    candidate[:, 0] = 1.10 * base[:, 0] + 0.05 * base[:, 2]
    candidate[:, 1] = 0.90 * base[:, 1] - 0.02 * base[:, 2]
    sculpt = base.copy()
    sculpt[:, 0] = 0.95 * base[:, 0] + 0.05 * base[:, 2] + 0.40
    sculpt[:, 1] = 1.05 * base[:, 1] - 0.02 * base[:, 2] - 0.20
    sculpt[:, 2] = base[:, 2] + 0.30
    return base, np.asarray(triangles, dtype=np.int64), candidate, sculpt


def selftest() -> int:
    levels = [0.25, 0.50, 0.75]
    variants = []
    for nz, flip in ((6, False), (6, True), (11, False)):
        base, triangles, candidate, sculpt = synthetic_tube(nz=nz, flip=flip)
        variants.append(analyze_arrays(base, triangles, sculpt, {"candidate": candidate}, levels))
    reference = variants[0]
    failures: list[str] = []
    if reference["axis"][2] < 0.99:
        failures.append(f"axis is not +Z oriented: {reference['axis']}")
    for level, data in reference["levels"].items():
        row = data["candidates"]["candidate"]
        if abs(row["delta_z"] - 0.30) > 1.0e-9 or abs(row["delta_down"] + 0.30) > 1.0e-9:
            failures.append(f"world-Z sign mismatch at {level}: {row['delta_z']}/{row['delta_down']}")
        if row["sector_coverage_common"] != 8:
            failures.append(f"unexpected sector coverage at {level}: {row['sector_coverage_common']}")
        if row["shape_perp_rms"] <= 0.01:
            failures.append(f"material deformation was not detected at {level}")

    base, triangles, _candidate, _sculpt = synthetic_tube()
    translation = np.array([0.4, -0.2, 0.3])
    translated = analyze_arrays(
        base, triangles, base + translation, {"base": base}, [0.5]
    )["levels"]["0.500000"]["candidates"]["base"]
    if translated["shape_perp_rms"] > 1.0e-10 or translated["shape_axial_rms"] > 1.0e-10:
        failures.append("pure translation changed paired shape")
    scaled = base.copy()
    scaled[:, :2] *= 1.25
    scale_row = analyze_arrays(
        base, triangles, scaled, {"base": base}, [0.5]
    )["levels"]["0.500000"]["candidates"]["base"]
    if abs(scale_row["width_ratio"] - 1.25) > 1.0e-10 or scale_row["shape_perp_rms"] > 1.0e-10:
        failures.append(
            f"pure radial scale mismatch ratio={scale_row['width_ratio']} "
            f"shape={scale_row['shape_perp_rms']}"
        )
    invariant_keys = (
        "delta_center_norm", "delta_xy", "delta_z", "delta_axis", "delta_perp",
        "width_ratio", "correspondence_centered_rms", "shape_perp_rms",
        "shape_axial_rms",
    )
    for variant_index, variant in enumerate(variants[1:], start=1):
        for level in reference["levels"]:
            expected = reference["levels"][level]["candidates"]["candidate"]
            actual = variant["levels"][level]["candidates"]["candidate"]
            for key in invariant_keys:
                if abs(float(expected[key]) - float(actual[key])) > 1.0e-8:
                    failures.append(
                        f"invariance variant={variant_index} level={level} key={key}: "
                        f"{expected[key]} vs {actual[key]}"
                    )
            # The sector statistic is intentionally secondary: bins have hard
            # angular boundaries, so a quadrature sample can swap bins when a
            # quad diagonal changes.  It must remain close, while the paired
            # material metrics above are the invariance-critical measurements.
            if abs(float(expected["sector_shape_rms"]) - float(actual["sector_shape_rms"])) > 5.0e-3:
                failures.append(
                    f"sector summary drift variant={variant_index} level={level}: "
                    f"{expected['sector_shape_rms']} vs {actual['sector_shape_rms']}"
                )
    if failures:
        print("selftest: FAIL")
        for failure in failures:
            print(f"  {failure}")
        return 1
    print("selftest: PASS (axis sign, world-Z sign, diagonal and vertical-subdivision invariance)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", type=Path)
    parser.add_argument("--sculpt", type=Path)
    parser.add_argument("--candidate", action="append", type=parse_candidate, default=[])
    parser.add_argument(
        "--defect", type=Path,
        help="backward-compatible alias for --candidate defect=OBJ",
    )
    parser.add_argument("--levels", type=int, default=7)
    parser.add_argument(
        "--component", type=int,
        help="welded material component index (default: largest; sizes are printed)",
    )
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    if args.base is None or args.sculpt is None:
        parser.error("--base and --sculpt are required unless --selftest is used")
    labels = [label for label, _path in args.candidate]
    duplicates = sorted({label for label in labels if labels.count(label) > 1})
    if duplicates:
        parser.error("duplicate candidate label(s): " + ", ".join(duplicates))
    candidate_paths = dict(args.candidate)
    if args.defect is not None:
        if "defect" in candidate_paths:
            parser.error("candidate label 'defect' supplied twice")
        candidate_paths["defect"] = args.defect
    if not candidate_paths:
        parser.error("provide at least one --candidate LABEL=OBJ (or --defect OBJ)")
    missing = [str(path) for path in [args.base, args.sculpt, *candidate_paths.values()] if not path.is_file()]
    if missing:
        parser.error("missing input(s): " + ", ".join(missing))

    base_verts, triangles = load_obj(str(args.base))
    sculpt_verts, sculpt_triangles = load_obj(str(args.sculpt))
    if not np.isfinite(base_verts).all() or not np.isfinite(sculpt_verts).all():
        raise ValueError("base/sculpt contains NaN or infinity")
    if len(sculpt_triangles) and not np.array_equal(triangles, sculpt_triangles):
        raise ValueError("base and sculpt triangle topology differ")
    candidate_verts: dict[str, np.ndarray] = {}
    for label, path in candidate_paths.items():
        verts, candidate_triangles = load_obj(str(path))
        if len(candidate_triangles) and not np.array_equal(triangles, candidate_triangles):
            raise ValueError(f"base and candidate {label!r} triangle topology differ")
        if not np.isfinite(verts).all():
            raise ValueError(f"candidate {label!r} contains NaN or infinity")
        candidate_verts[label] = verts
    levels = [float(value) for value in np.linspace(0.15, 0.85, max(1, args.levels))]
    result = analyze_arrays(
        base_verts, triangles, sculpt_verts, candidate_verts, levels,
        component=args.component,
    )
    result["inputs"] = {
        "base": {"path": str(args.base), "sha256": sha256(args.base)},
        "sculpt": {"path": str(args.sculpt), "sha256": sha256(args.sculpt)},
        "candidates": {
            label: {"path": str(path), "sha256": sha256(path)}
            for label, path in candidate_paths.items()
        },
    }
    print_result(result)
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(
            json.dumps(result, indent=2, sort_keys=True, allow_nan=False) + "\n",
            encoding="utf-8",
        )
        print(f"wrote {args.json_out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
