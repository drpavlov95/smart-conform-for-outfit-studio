#!/usr/bin/env python3
"""Candidate-independent free-cloth plumb/gap metric for Path-B.

The frozen population uses only authored cloth, raw conform, the paired bodies,
and the garment drape signal.  It never reads a hand sculpt, garment name, or
candidate-specific mask, so candidates cannot improve the score by changing
which faces are judged.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

try:
    from tools import shell_spike as ss
except ModuleNotFoundError:  # direct invocation from tools/
    import shell_spike as ss  # type: ignore[no-redef]


DEGENERATE_AREA_EPS = 1.0e-12
COLLAPSE_AREA_RATIO = 0.10
PENETRATION_EPS = 1.0e-4
FACE_SAMPLES = np.asarray(
    [
        [1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0],
        [0.5, 0.5, 0.0],
        [0.5, 0.0, 0.5],
        [0.0, 0.5, 0.5],
    ],
    dtype=np.float64,
)


def face_geometry(vertices: np.ndarray, triangles: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    tri = vertices[triangles]
    cross = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    area2 = np.linalg.norm(cross, axis=1)
    normals = cross / np.maximum(area2[:, None], 1.0e-30)
    # A vertical panel has a horizontal normal and therefore zero tilt.
    tilt = np.degrees(np.arcsin(np.clip(np.abs(normals[:, 2]), 0.0, 1.0)))
    tilt[area2 <= 2.0 * DEGENERATE_AREA_EPS] = np.nan
    return 0.5 * area2, tilt


def weighted_percentile(values: np.ndarray, weights: np.ndarray, percentile: float) -> float:
    values = np.asarray(values, dtype=np.float64)
    weights = np.asarray(weights, dtype=np.float64)
    if values.shape != weights.shape or values.size == 0:
        raise ValueError("weighted percentile requires equally sized non-empty arrays")
    if (
        not np.all(np.isfinite(values))
        or not np.all(np.isfinite(weights))
        or np.any(weights <= 0.0)
    ):
        raise ValueError(
            "weighted percentile received non-finite values or non-positive weights"
        )
    if not np.isfinite(percentile) or not 0.0 <= percentile <= 100.0:
        raise ValueError("weighted percentile is outside [0,100]")
    order = np.argsort(values, kind="stable")
    values = values[order]
    weights = weights[order]
    target = np.clip(percentile, 0.0, 100.0) * weights.sum() / 100.0
    index = min(int(np.searchsorted(np.cumsum(weights), target, side="left")), len(values) - 1)
    return float(values[index])


def material_representatives(vertex_count: int, seam_groups: list[list[int]]) -> np.ndarray:
    parent = np.arange(vertex_count, dtype=np.int32)

    def find(value: int) -> int:
        while parent[value] != value:
            parent[value] = parent[parent[value]]
            value = int(parent[value])
        return value

    def union(a: int, b: int) -> None:
        ra, rb = find(a), find(b)
        if ra == rb:
            return
        if ra > rb:
            ra, rb = rb, ra
        parent[rb] = ra

    for group in seam_groups:
        if not group:
            continue
        for vertex in group[1:]:
            union(int(group[0]), int(vertex))
    return np.asarray([find(i) for i in range(vertex_count)], dtype=np.int32)


def largest_face_island(
    triangles: np.ndarray,
    selected_face_ids: np.ndarray,
    active: np.ndarray,
    representatives: np.ndarray,
    area: np.ndarray,
) -> tuple[int, float]:
    active_ids = selected_face_ids[active]
    if not len(active_ids):
        return 0, 0.0
    edge_faces: dict[tuple[int, int], list[int]] = {}
    for face_id in active_ids:
        nodes = representatives[triangles[int(face_id)]]
        for ia, ib in ((0, 1), (1, 2), (2, 0)):
            a, b = int(nodes[ia]), int(nodes[ib])
            if a == b:
                continue
            edge = (a, b) if a < b else (b, a)
            edge_faces.setdefault(edge, []).append(int(face_id))
    adjacency: dict[int, set[int]] = {int(face): set() for face in active_ids}
    for faces in edge_faces.values():
        faces = sorted(set(faces))
        for face in faces:
            adjacency[face].update(other for other in faces if other != face)

    remaining = set(adjacency)
    best_count = 0
    best_area = 0.0
    while remaining:
        seed = min(remaining)
        remaining.remove(seed)
        stack = [seed]
        component = []
        while stack:
            face = stack.pop()
            component.append(face)
            unseen = adjacency[face] & remaining
            remaining.difference_update(unseen)
            stack.extend(unseen)
        component_area = float(area[np.asarray(component, dtype=np.int32)].sum())
        if (component_area, len(component)) > (best_area, best_count):
            best_count = len(component)
            best_area = component_area
    return best_count, best_area


def validate_candidate_integrity(
    name: str,
    vertices: np.ndarray,
    triangles: np.ndarray,
    base_cross: np.ndarray,
    base_area: np.ndarray,
    selected_face_ids: np.ndarray,
    body: ss.UniformBodyCollider,
) -> dict[str, float | int]:
    nonfinite = int((~np.isfinite(vertices)).sum())
    if nonfinite:
        raise SystemExit(
            f"{name}: integrity FAIL: non-finite coordinates={nonfinite}"
        )
    tri = vertices[triangles]
    cross = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    area = 0.5 * np.linalg.norm(cross, axis=1)
    if not np.all(np.isfinite(cross)) or not np.all(np.isfinite(area)):
        raise SystemExit(f"{name}: integrity FAIL: non-finite face geometry")
    valid = base_area > DEGENERATE_AREA_EPS
    degenerate = valid & (area <= DEGENERATE_AREA_EPS)
    dot = np.einsum("ij,ij->i", base_cross, cross)
    if not np.all(np.isfinite(dot)):
        raise SystemExit(f"{name}: integrity FAIL: non-finite orientation dot")
    flipped = valid & (dot < 0.0)
    collapsed = valid & (area < COLLAPSE_AREA_RATIO * base_area)
    samples = np.einsum(
        "sc,fcd->fsd", FACE_SAMPLES, tri[selected_face_ids]
    ).reshape(-1, 3)
    face_gaps = body.gaps(samples)
    if not np.all(np.isfinite(face_gaps)):
        raise SystemExit(
            f"{name}: integrity FAIL: face gap query returned NaN/Inf"
        )
    penetrating = face_gaps < -PENETRATION_EPS
    failures = []
    for label, count in (
        ("degenerate", int(degenerate.sum())),
        ("flipped", int(flipped.sum())),
        ("collapsed", int(collapsed.sum())),
        ("judged-face penetration samples", int(penetrating.sum())),
    ):
        if count:
            failures.append(f"{label}={count}")
    if failures:
        raise SystemExit(f"{name}: integrity FAIL: {', '.join(failures)}")
    return {
        "nonfinite_coordinates": 0,
        "degenerate_faces": 0,
        "flipped_faces": 0,
        "collapsed_faces": 0,
        "judged_face_sample_count": int(face_gaps.size),
        "judged_face_penetration_samples": 0,
        "judged_face_worst_gap": float(face_gaps.min()),
    }


def summarize_candidate(
    vertices: np.ndarray,
    triangles: np.ndarray,
    selected_face_ids: np.ndarray,
    selected_vertex_ids: np.ndarray,
    base_area: np.ndarray,
    representatives: np.ndarray,
    body: ss.UniformBodyCollider,
    island_tilt: float,
) -> dict[str, float | int]:
    selected_area = base_area[selected_face_ids]
    _area, tilt_all = face_geometry(vertices, triangles)
    tilt = tilt_all[selected_face_ids]
    total_area = float(selected_area.sum())
    over = tilt > island_tilt
    island_count, island_area = largest_face_island(
        triangles, selected_face_ids, over, representatives, base_area
    )
    gaps = body.gaps(vertices[selected_vertex_ids])
    if not np.all(np.isfinite(gaps)):
        raise ValueError("candidate gap query returned NaN/Inf")
    return {
        "tilt_area_weighted_mean_deg": float(np.average(tilt, weights=selected_area)),
        "tilt_p90_deg": float(np.percentile(tilt, 90.0)),
        "tilt_area_weighted_p90_deg": weighted_percentile(tilt, selected_area, 90.0),
        "tilt_area_fraction_over_threshold": float(selected_area[over].sum() / total_area),
        "largest_island_face_count": island_count,
        "largest_island_area": island_area,
        "largest_island_area_fraction": island_area / total_area,
        "gap_mean": float(np.mean(gaps)),
        "gap_p10": float(np.percentile(gaps, 10.0)),
        "gap_count_below_1": int(np.sum(gaps < 1.0)),
        "gap_count_below_2_5": int(np.sum(gaps < 2.5)),
    }


def parse_candidate(value: str) -> tuple[str, Path]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("candidate must be NAME=OBJ")
    name, raw_path = (part.strip() for part in value.split("=", 1))
    if not name or not raw_path:
        raise argparse.ArgumentTypeError("candidate must be NAME=OBJ")
    return name, Path(raw_path)


def selftest() -> None:
    vertical = np.asarray(
        [[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 0.0, 1.0]],
        dtype=np.float64,
    )
    area, tilt = face_geometry(vertical, np.asarray([[0, 1, 2]], dtype=np.int32))
    if abs(float(tilt[0])) > 1.0e-12 or abs(float(area[0]) - 0.5) > 1.0e-12:
        raise RuntimeError(f"vertical face metric failed: area={area} tilt={tilt}")
    slope = np.tan(np.radians(20.0))
    tilted = vertical.copy()
    tilted[:, 1] = slope * tilted[:, 2]
    _area, tilt = face_geometry(tilted, np.asarray([[0, 1, 2]], dtype=np.int32))
    if abs(float(tilt[0]) - 20.0) > 1.0e-10:
        raise RuntimeError(f"tilted face metric failed: {tilt}")
    q = weighted_percentile(np.asarray([1.0, 9.0]), np.asarray([9.0, 1.0]), 90.0)
    if q != 1.0:
        raise RuntimeError(f"weighted percentile failed: {q}")
    try:
        weighted_percentile(np.asarray([np.nan]), np.asarray([1.0]), 90.0)
    except ValueError:
        pass
    else:
        raise RuntimeError("weighted percentile accepted NaN")
    triangles = np.asarray(
        [[0, 1, 2], [0, 3, 4], [5, 6, 7]], dtype=np.int32
    )
    representatives = np.asarray([0, 1, 2, 3, 4, 0, 1, 7], dtype=np.int32)
    count, joined_area = largest_face_island(
        triangles,
        np.arange(3, dtype=np.int32),
        np.ones(3, dtype=bool),
        representatives,
        np.ones(3, dtype=np.float64),
    )
    if count != 2 or joined_area != 2.0:
        raise RuntimeError("material-edge island connectivity failed")
    print("pathb plumb gate selftest: PASS")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--capture", type=Path)
    parser.add_argument("--candidate", action="append", type=parse_candidate, default=[])
    parser.add_argument("--gap", type=float, default=2.5)
    parser.add_argument("--drape", type=float, default=0.55)
    parser.add_argument("--base-tilt", type=float, default=10.0)
    parser.add_argument("--island-tilt", type=float, default=8.0)
    for axis in "xyz":
        parser.add_argument(f"--{axis}-min", type=float)
        parser.add_argument(f"--{axis}-max", type=float)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        selftest()
    if args.capture is None:
        if args.selftest:
            return 0
        parser.error("--capture is required unless only --selftest is used")
    if not args.candidate:
        parser.error("at least one --candidate NAME=OBJ is required")
    names = [name for name, _path in args.candidate]
    duplicates = sorted({name for name in names if names.count(name) > 1})
    if duplicates:
        parser.error("duplicate candidate name(s): " + ", ".join(duplicates))
    for label, value, lower, upper in (
        ("--gap", args.gap, 0.0, None),
        ("--drape", args.drape, 0.0, 1.0),
        ("--base-tilt", args.base_tilt, 0.0, 90.0),
        ("--island-tilt", args.island_tilt, 0.0, 90.0),
    ):
        if (
            not np.isfinite(value)
            or value < lower
            or (upper is not None and value > upper)
        ):
            upper_text = upper if upper is not None else "inf"
            parser.error(f"{label} must be finite and in [{lower},{upper_text}]")
    for axis in "xyz":
        lower = getattr(args, f"{axis}_min")
        upper = getattr(args, f"{axis}_max")
        if lower is not None and not np.isfinite(lower):
            parser.error(f"--{axis}-min must be finite")
        if upper is not None and not np.isfinite(upper):
            parser.error(f"--{axis}-max must be finite")
        if lower is not None and upper is not None and lower > upper:
            parser.error(f"--{axis}-min must be <= --{axis}-max")

    capture = ss.read_capture_front(args.capture.resolve())
    for label, array in (
        ("cloth base", capture.base_verts),
        ("cloth raw diff", capture.raw_diff),
        ("body base", capture.ref_body_verts),
        ("body final", capture.body_verts),
    ):
        if not np.all(np.isfinite(array)):
            raise SystemExit(f"capture {label} contains NaN/Inf")
    topology = ss.topology_stats(capture.base_verts, capture.tris)
    representatives = material_representatives(
        len(capture.base_verts), topology["seam_groups"]
    )
    authored_body = ss.UniformBodyCollider(capture.ref_body_verts, capture.ref_body_tris)
    final_body = ss.UniformBodyCollider(capture.body_verts, capture.body_tris)
    raw_vertices = capture.base_verts + capture.raw_diff
    authored_gap = authored_body.gaps(capture.base_verts)
    raw_gap = final_body.gaps(raw_vertices)
    drape = ss.fitted_drape_signal(capture)
    frozen_vertices = (
        (authored_gap >= args.gap)
        & (raw_gap >= args.gap)
        & (drape >= args.drape)
    )
    bounds: dict[str, float] = {}
    for axis_index, axis in enumerate("xyz"):
        lower = getattr(args, f"{axis}_min")
        upper = getattr(args, f"{axis}_max")
        if lower is not None:
            frozen_vertices &= capture.base_verts[:, axis_index] >= lower
            bounds[f"{axis}_min"] = lower
        if upper is not None:
            frozen_vertices &= capture.base_verts[:, axis_index] <= upper
            bounds[f"{axis}_max"] = upper
    base_area, base_tilt = face_geometry(capture.base_verts, capture.tris)
    selected = (
        np.all(frozen_vertices[capture.tris], axis=1)
        & (base_tilt <= args.base_tilt)
        & (base_area > 1.0e-12)
    )
    selected_face_ids = np.flatnonzero(selected).astype(np.int32)
    if not len(selected_face_ids):
        raise SystemExit("frozen plumb mask contains no faces")
    selected_vertex_ids = np.unique(
        capture.tris[selected_face_ids].reshape(-1)
    ).astype(np.int32)
    base_tri = capture.base_verts[capture.tris]
    base_cross = np.cross(
        base_tri[:, 1] - base_tri[:, 0], base_tri[:, 2] - base_tri[:, 0]
    )
    if not np.all(np.isfinite(base_cross)) or np.any(
        base_area <= DEGENERATE_AREA_EPS
    ):
        raise SystemExit(
            "capture authored mesh contains non-finite/degenerate faces"
        )

    report: dict[str, object] = {
        "capture": str(args.capture.resolve()),
        "capture_sha256": hashlib.sha256(args.capture.read_bytes()).hexdigest(),
        "frozen": {
            "vertex_count": int(len(selected_vertex_ids)),
            "eligible_vertex_count": int(np.sum(frozen_vertices)),
            "face_count": int(len(selected_face_ids)),
            "gap_threshold": args.gap,
            "drape_threshold": args.drape,
            "base_tilt_threshold_deg": args.base_tilt,
            "island_tilt_threshold_deg": args.island_tilt,
            "authored_vertex_bounds": bounds,
            "face_ids_sha256": hashlib.sha256(selected_face_ids.astype("<i4").tobytes()).hexdigest(),
            "vertex_ids_sha256": hashlib.sha256(
                selected_vertex_ids.astype("<i4").tobytes()
            ).hexdigest(),
        },
        "integrity_thresholds": {
            "degenerate_area_epsilon": DEGENERATE_AREA_EPS,
            "collapse_area_ratio": COLLAPSE_AREA_RATIO,
            "penetration_epsilon": PENETRATION_EPS,
            "face_samples_barycentric": FACE_SAMPLES.tolist(),
        },
        "gap_report_thresholds": [1.0, 2.5],
        "candidates": {},
    }
    for name, path in args.candidate:
        resolved = path.resolve()
        mesh = ss.load_obj(resolved)
        if len(mesh.verts) != len(capture.base_verts) or not np.array_equal(mesh.tris, capture.tris):
            raise SystemExit(f"{name}: candidate topology differs from capture")
        integrity = validate_candidate_integrity(
            name,
            mesh.verts,
            capture.tris,
            base_cross,
            base_area,
            selected_face_ids,
            final_body,
        )
        row = summarize_candidate(
            mesh.verts,
            capture.tris,
            selected_face_ids,
            selected_vertex_ids,
            base_area,
            representatives,
            final_body,
            args.island_tilt,
        )
        row["integrity"] = integrity
        row["path"] = str(resolved)
        row["sha256"] = hashlib.sha256(resolved.read_bytes()).hexdigest()
        report["candidates"][name] = row  # type: ignore[index]
        print(
            f"{name}: faces={len(selected_face_ids)} "
            f"tiltMean/P90/areaP90={row['tilt_area_weighted_mean_deg']:.3f}/"
            f"{row['tilt_p90_deg']:.3f}/"
            f"{row['tilt_area_weighted_p90_deg']:.3f}deg "
            f"over={100.0*row['tilt_area_fraction_over_threshold']:.1f}% "
            f"island={100.0*row['largest_island_area_fraction']:.1f}% "
            f"gapMean/P10={row['gap_mean']:.3f}/{row['gap_p10']:.3f} "
            f"gap<1/<2.5={row['gap_count_below_1']}/{row['gap_count_below_2_5']}"
        )
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(
            json.dumps(report, indent=2, sort_keys=True, allow_nan=False) + "\n",
            encoding="utf-8",
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
