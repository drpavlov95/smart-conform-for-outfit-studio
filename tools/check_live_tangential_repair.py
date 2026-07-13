#!/usr/bin/env python3
"""Score the experimental C++ tangential repair against the live hand edit."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

from shell_spike import ObjMesh, UniformBodyCollider, load_obj, surface_distance_stats


ROOT = Path(__file__).resolve().parent.parent


def unique_edges(tris: np.ndarray) -> np.ndarray:
    return np.unique(
        np.sort(
            np.vstack((tris[:, [0, 1]], tris[:, [1, 2]], tris[:, [2, 0]])),
            axis=1,
        ),
        axis=0,
    )


def triangle_metrics(verts: np.ndarray, tris: np.ndarray):
    e0 = verts[tris[:, 1]] - verts[tris[:, 0]]
    e1 = verts[tris[:, 2]] - verts[tris[:, 0]]
    e2 = verts[tris[:, 2]] - verts[tris[:, 1]]
    lengths = np.stack(
        (np.linalg.norm(e0, axis=1), np.linalg.norm(e1, axis=1), np.linalg.norm(e2, axis=1)),
        axis=1,
    )
    cosines = np.stack(
        (
            np.einsum("ij,ij->i", e0, e1) / np.maximum(lengths[:, 0] * lengths[:, 1], 1e-12),
            np.einsum("ij,ij->i", -e0, e2) / np.maximum(lengths[:, 0] * lengths[:, 2], 1e-12),
            np.einsum("ij,ij->i", -e1, -e2) / np.maximum(lengths[:, 1] * lengths[:, 2], 1e-12),
        ),
        axis=1,
    )
    min_angle = np.degrees(np.arccos(np.clip(cosines, -1.0, 1.0))).min(axis=1)
    aspect = lengths.max(axis=1) / np.maximum(lengths.min(axis=1), 1e-12)
    normals = np.cross(e0, e1)
    return min_angle, aspect, normals


def mirror_pairs(base: np.ndarray, tolerance: float = 0.4):
    mirrored = base.copy()
    mirrored[:, 0] *= -1.0
    nearest = np.empty(len(base), dtype=np.int32)
    distance = np.empty(len(base), dtype=np.float64)
    for start in range(0, len(base), 256):
        d2 = np.sum((mirrored[start : start + 256, None] - base[None]) ** 2, axis=2)
        nearest[start : start + 256] = np.argmin(d2, axis=1)
        distance[start : start + 256] = np.sqrt(np.min(d2, axis=1))
    return [
        (i, int(j))
        for i, j in enumerate(nearest)
        if i < j and nearest[j] == i and max(distance[i], distance[j]) <= tolerance
    ]


def symmetry_rms(verts: np.ndarray, base: np.ndarray, pairs, mask: np.ndarray) -> float:
    displacement = verts - base
    values = []
    for i, j in pairs:
        if not (mask[i] and mask[j]):
            continue
        mirrored = np.array((-displacement[j, 0], displacement[j, 1], displacement[j, 2]))
        values.append(np.linalg.norm(displacement[i] - mirrored))
    return float(np.sqrt(np.mean(np.square(values)))) if values else float("nan")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", type=Path, default=ROOT / "tmp/capture_20260710_blacksmith/base.obj")
    parser.add_argument("--off", type=Path, default=ROOT / "tmp/cpp_tangential_v2_off.obj")
    parser.add_argument("--on", type=Path, default=ROOT / "tmp/cpp_tangential_v2_on.obj")
    parser.add_argument(
        "--target",
        type=Path,
        default=ROOT / "tmp/BlackSmithTorso_hand-edit of the smart-conformed RM_Heavy slider.obj",
    )
    parser.add_argument("--target-scale", type=float, default=10.0)
    parser.add_argument("--body", type=Path, default=ROOT / "tmp/capture_20260710_blacksmith/body.obj")
    parser.add_argument("--body-scale", type=float, default=1.0)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--no-gate", action="store_true")
    args = parser.parse_args()

    base_mesh = load_obj(args.base)
    off_mesh = load_obj(args.off)
    on_mesh = load_obj(args.on)
    target_mesh = load_obj(args.target)
    body_mesh = load_obj(args.body)
    target = target_mesh.verts * args.target_scale
    body = body_mesh.verts * args.body_scale
    tris = base_mesh.tris
    for name, mesh in (("off", off_mesh), ("on", on_mesh), ("target", target_mesh)):
        if len(mesh.verts) != len(base_mesh.verts) or not np.array_equal(mesh.tris, tris):
            raise ValueError(f"{name} topology/order does not match base")

    edges = unique_edges(tris)
    authored_length = np.linalg.norm(
        base_mesh.verts[edges[:, 1]] - base_mesh.verts[edges[:, 0]], axis=1
    )
    tri_center = off_mesh.verts[tris].mean(axis=1)
    edge_mid = off_mesh.verts[edges].mean(axis=1)
    central_faces = (
        (tri_center[:, 1] > 0)
        & (tri_center[:, 2] >= 38)
        & (tri_center[:, 2] <= 68)
        & (np.abs(tri_center[:, 0]) <= 3)
    )
    central_edges = (
        (edge_mid[:, 1] > 0)
        & (edge_mid[:, 2] >= 38)
        & (edge_mid[:, 2] <= 68)
        & (np.abs(edge_mid[:, 0]) <= 4)
    )
    roi = (
        (off_mesh.verts[:, 1] > 0)
        & (off_mesh.verts[:, 2] >= 35)
        & (off_mesh.verts[:, 2] <= 72)
        & (np.abs(off_mesh.verts[:, 0]) <= 22)
    )
    symmetry_core = (
        (off_mesh.verts[:, 1] > 4)
        & (off_mesh.verts[:, 2] >= 40)
        & (off_mesh.verts[:, 2] <= 67)
        & (np.abs(off_mesh.verts[:, 0]) <= 17)
    )
    pairs = mirror_pairs(base_mesh.verts)

    def score(verts: np.ndarray):
        min_angle, aspect, normals = triangle_metrics(verts, tris)
        edge_ratio = np.linalg.norm(verts[edges[:, 1]] - verts[edges[:, 0]], axis=1) / np.maximum(
            authored_length, 1e-12
        )
        indexed = np.linalg.norm(verts - target, axis=1)
        surface = surface_distance_stats(verts[roi], ObjMesh(target, tris))
        return {
            "target_index_roi_mean": float(indexed[roi].mean()),
            "target_index_roi_p95": float(np.percentile(indexed[roi], 95)),
            "target_surface_roi_mean": float(surface[0]),
            "target_surface_roi_p95": float(surface[1]),
            "central_min_angle_p05": float(np.percentile(min_angle[central_faces], 5)),
            "central_min_angle_min": float(min_angle[central_faces].min()),
            "central_aspect_p95": float(np.percentile(aspect[central_faces], 95)),
            "central_aspect_max": float(aspect[central_faces].max()),
            "central_edge_ratio_min": float(edge_ratio[central_edges].min()),
            "central_edges_below_0_5": int(np.sum(central_edges & (edge_ratio < 0.5))),
            "symmetry_core_rms": symmetry_rms(verts, base_mesh.verts, pairs, symmetry_core),
            "face_flips": int(
                np.sum(
                    np.einsum(
                        "ij,ij->i",
                        triangle_metrics(off_mesh.verts, tris)[2],
                        normals,
                    )
                    <= 0
                )
            ),
        }

    report = {"off": score(off_mesh.verts), "on": score(on_mesh.verts)}
    movement = np.linalg.norm(on_mesh.verts - off_mesh.verts, axis=1)
    moved = movement > 1e-5
    collider = UniformBodyCollider(body, body_mesh.tris, 4.0)
    gaps = collider.gaps(on_mesh.verts, np.flatnonzero(moved))
    report["movement"] = {
        "count": int(moved.sum()),
        "mean": float(movement[moved].mean()) if moved.any() else 0.0,
        "p95": float(np.percentile(movement[moved], 95)) if moved.any() else 0.0,
        "max": float(movement.max()),
        "outside_roi_max": float(movement[~roi].max()),
        "moved_body_gap_min": float(np.nanmin(gaps)) if len(gaps) else float("nan"),
        "moved_body_penetrations": int(np.sum(gaps < 0)),
    }

    failures = []
    off, on = report["off"], report["on"]
    checks = (
        (on["target_index_roi_mean"] < 0.70 * off["target_index_roi_mean"], "ROI target error did not improve enough"),
        (on["central_min_angle_p05"] >= 25.0, "central triangle angle remains too acute"),
        (on["central_aspect_p95"] <= 2.5, "central aspect ratio remains too high"),
        (on["central_edge_ratio_min"] >= 0.74, "authored compression floor was not restored"),
        (on["central_edges_below_0_5"] == 0, "collapsed central edges remain"),
        (on["symmetry_core_rms"] <= 0.01, "core displacement field is still asymmetric"),
        (on["face_flips"] == 0, "face orientation flip detected"),
        (report["movement"]["outside_roi_max"] <= 0.02, "repair leaked outside ROI"),
        (report["movement"]["moved_body_penetrations"] == 0, "repair introduced body penetration"),
    )
    failures.extend(message for ok, message in checks if not ok)
    report["failures"] = failures

    print(json.dumps(report, indent=2, sort_keys=True))
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0 if args.no_gate or not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
