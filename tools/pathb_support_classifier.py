#!/usr/bin/env python3
"""Pure, garment-agnostic support classifiers for Path-B offline research.

This module deliberately knows nothing about Outfit Studio names, ground truth,
or hand-picked vertex IDs.  The caller supplies material topology and the two
candidate target fields (vanilla/raw and surface-corot).
"""

from __future__ import annotations

from dataclasses import asdict, dataclass
from typing import Iterable, Sequence

import numpy as np


@dataclass(frozen=True)
class CoherentV1Thresholds:
    lever_over_body_edge_max: float = 2.0
    coherent_incident_fraction_min: float = 0.75
    support_jump_over_cloth_edge_max: float = 2.0
    displacement_gradient_max: float = 1.0
    transported_edge_ratio_min: float = 0.5
    transported_edge_ratio_max: float = 2.0
    hybrid_edge_ratio_max: float = 2.0
    hybrid_face_area_ratio_min: float = 0.1
    epsilon: float = 1.0e-9


THRESHOLDS = CoherentV1Thresholds()


def _validate(
    base: np.ndarray,
    tris: np.ndarray,
    edges: np.ndarray,
    fixed: np.ndarray,
    requested: np.ndarray,
    authored_distance: np.ndarray,
    body_scale: np.ndarray,
    support_points: np.ndarray,
    raw_targets: np.ndarray,
    corot_targets: np.ndarray,
) -> None:
    n = len(base)
    if base.shape != (n, 3):
        raise ValueError("base must be Nx3")
    if (
        raw_targets.ndim != 3
        or corot_targets.ndim != 3
        or raw_targets.shape != corot_targets.shape
        or raw_targets.shape[1:] != base.shape
        or raw_targets.shape[0] == 0
    ):
        raise ValueError("raw/corot target sequences must be matching SxNx3 arrays")
    if support_points.shape != base.shape:
        raise ValueError("support_points must be Nx3")
    for label, values in (("fixed", fixed), ("requested", requested)):
        if values.shape != (n,):
            raise ValueError(f"{label} must have one entry per vertex")
    for label, values in (("authored_distance", authored_distance), ("body_scale", body_scale)):
        if values.shape != (n,):
            raise ValueError(f"{label} must have one entry per vertex")
    if tris.ndim != 2 or tris.shape[1] != 3 or edges.ndim != 2 or edges.shape[1] != 2:
        raise ValueError("triangles/edges must be Mx3/Mx2")
    if len(tris) and (tris.min() < 0 or tris.max() >= n):
        raise ValueError("triangle index outside vertex array")
    if len(edges) and (edges.min() < 0 or edges.max() >= n):
        raise ValueError("edge index outside vertex array")
    if not np.all(fixed <= requested):
        raise ValueError("every fixed vertex must be in requested mask")
    if not np.isfinite(base).all() or not np.isfinite(raw_targets).all() or not np.isfinite(corot_targets).all():
        raise ValueError("non-finite target geometry")
    if not np.isfinite(authored_distance[requested]).all():
        raise ValueError("non-finite authored distance on requested mask")
    if not np.isfinite(body_scale[requested]).all() or np.any(body_scale[requested] <= 0.0):
        raise ValueError("body scale must be finite and positive on requested mask")
    if not np.isfinite(support_points[requested]).all():
        raise ValueError("non-finite support point on requested mask")


def _adjacency(n: int, edges: np.ndarray) -> list[list[int]]:
    out: list[list[int]] = [[] for _ in range(n)]
    for a, b in edges:
        ai, bi = int(a), int(b)
        out[ai].append(bi)
        out[bi].append(ai)
    return out


def _reachable_from_fixed(
    fixed: np.ndarray, requested: np.ndarray, adjacency: list[list[int]]
) -> np.ndarray:
    reached = np.zeros(len(requested), dtype=bool)
    stack = [int(v) for v in np.flatnonzero(fixed)]
    reached[fixed] = True
    while stack:
        vertex = stack.pop()
        for neighbour in adjacency[vertex]:
            if requested[neighbour] and not reached[neighbour]:
                reached[neighbour] = True
                stack.append(neighbour)
    return reached


def _face_flip_mask(vertices: np.ndarray, base: np.ndarray, tris: np.ndarray) -> np.ndarray:
    test_tri = vertices[tris]
    base_tri = base[tris]
    test_n = np.cross(test_tri[:, 1] - test_tri[:, 0], test_tri[:, 2] - test_tri[:, 0])
    base_n = np.cross(base_tri[:, 1] - base_tri[:, 0], base_tri[:, 2] - base_tri[:, 0])
    test_area = np.linalg.norm(test_n, axis=1)
    base_area = np.linalg.norm(base_n, axis=1)
    return (
        (test_area > 1.0e-9)
        & (base_area > 1.0e-9)
        & (np.einsum("ij,ij->i", test_n, base_n) < 0.0)
    )


def _edge_ratios(vertices: np.ndarray, base: np.ndarray, edges: np.ndarray) -> np.ndarray:
    rest = np.linalg.norm(base[edges[:, 0]] - base[edges[:, 1]], axis=1)
    current = np.linalg.norm(vertices[edges[:, 0]] - vertices[edges[:, 1]], axis=1)
    return current / np.maximum(rest, 1.0e-12)


def _face_area_ratios(
    vertices: np.ndarray, base: np.ndarray, tris: np.ndarray
) -> np.ndarray:
    base_tri = base[tris]
    current_tri = vertices[tris]
    base_area2 = np.linalg.norm(
        np.cross(base_tri[:, 1] - base_tri[:, 0], base_tri[:, 2] - base_tri[:, 0]),
        axis=1,
    )
    current_area2 = np.linalg.norm(
        np.cross(
            current_tri[:, 1] - current_tri[:, 0],
            current_tri[:, 2] - current_tri[:, 0],
        ),
        axis=1,
    )
    return np.divide(
        current_area2,
        base_area2,
        out=np.ones(len(tris), dtype=np.float64),
        where=base_area2 > 1.0e-12,
    )


def _invalid_support_components(
    mask: np.ndarray,
    fixed: np.ndarray,
    adjacency: list[list[int]],
    tris: np.ndarray,
) -> set[int]:
    """Reject post-prune islands without both fixed support and a face patch."""
    triangle_core = np.zeros(len(mask), dtype=bool)
    selected_triangles = tris[np.all(mask[tris], axis=1)]
    if len(selected_triangles):
        triangle_core[selected_triangles.ravel()] = True
    visited = np.zeros(len(mask), dtype=bool)
    removals: set[int] = set()
    for start in np.flatnonzero(np.logical_and(mask, ~visited)):
        component = []
        stack = [int(start)]
        visited[start] = True
        while stack:
            vertex = stack.pop()
            component.append(vertex)
            for neighbour in adjacency[vertex]:
                if mask[neighbour] and not visited[neighbour]:
                    visited[neighbour] = True
                    stack.append(neighbour)
        idx = np.asarray(component, dtype=np.int32)
        if not fixed[idx].any() or not triangle_core[idx].any():
            removals.update(component)
    return removals


def _force_seam_removals(mask: np.ndarray, removals: set[int], seam_groups: Sequence[Sequence[int]]) -> None:
    """Keep coherent-v1 all-or-none across exact material seam duplicates."""
    for group in seam_groups:
        selected = [int(v) for v in group if mask[int(v)]]
        if selected and (len(selected) != len(group) or any(int(v) in removals for v in group)):
            removals.update(selected)


def _evaluate_target(
    mask: np.ndarray,
    base: np.ndarray,
    tris: np.ndarray,
    edges: np.ndarray,
    raw_targets: np.ndarray,
    corot_targets: np.ndarray,
    seam_groups: Sequence[Sequence[int]],
    thresholds: CoherentV1Thresholds,
) -> dict:
    per_step = []
    raw_flip_max = 0
    hybrid_flip_max = 0
    new_flip_total = 0
    expansion_total = 0
    contraction_total = 0
    area_collapse_total = 0
    worsened_seam_total = 0
    raw_seam_worst = 0.0
    hybrid_seam_worst = 0.0
    raw_p95_max = 0.0
    hybrid_p95_max = 0.0
    raw_ratio_max = 0.0
    hybrid_ratio_max = 0.0
    raw_ratio_min = float("inf")
    hybrid_ratio_min = float("inf")
    raw_area_ratio_min = float("inf")
    hybrid_area_ratio_min = float("inf")
    for step_index, (raw_target, corot_target) in enumerate(
        zip(raw_targets, corot_targets), 1
    ):
        hybrid = raw_target.copy()
        hybrid[mask] = corot_target[mask]
        raw_flips = _face_flip_mask(raw_target, base, tris)
        hybrid_flips = _face_flip_mask(hybrid, base, tris)
        raw_ratio = _edge_ratios(raw_target, base, edges)
        hybrid_ratio = _edge_ratios(hybrid, base, edges)
        raw_area_ratio = _face_area_ratios(raw_target, base, tris)
        hybrid_area_ratio = _face_area_ratios(hybrid, base, tris)
        new_flips = int((hybrid_flips & ~raw_flips).sum())
        worsened_expansion = int(
            (
                hybrid_ratio
                > np.maximum(
                    thresholds.hybrid_edge_ratio_max,
                    raw_ratio + thresholds.epsilon,
                )
            ).sum()
        )
        worsened_contraction = int(
            (
                hybrid_ratio
                < np.minimum(
                    thresholds.transported_edge_ratio_min,
                    raw_ratio - thresholds.epsilon,
                )
            ).sum()
        )
        worsened_area_collapse = int(
            (
                hybrid_area_ratio
                < np.minimum(
                    thresholds.hybrid_face_area_ratio_min,
                    raw_area_ratio - thresholds.epsilon,
                )
            ).sum()
        )
        step_raw_seam_worst = 0.0
        step_hybrid_seam_worst = 0.0
        step_worsened_seams = 0
        for group in seam_groups:
            idx = np.asarray(group, dtype=np.int32)
            if len(idx) < 2:
                continue
            raw_split = float(
                np.linalg.norm(raw_target[idx] - raw_target[idx[0]], axis=1).max()
            )
            hybrid_split = float(
                np.linalg.norm(hybrid[idx] - hybrid[idx[0]], axis=1).max()
            )
            step_raw_seam_worst = max(step_raw_seam_worst, raw_split)
            step_hybrid_seam_worst = max(step_hybrid_seam_worst, hybrid_split)
            if hybrid_split > raw_split + thresholds.epsilon:
                step_worsened_seams += 1
        raw_p95 = float(np.percentile(raw_ratio, 95)) if len(raw_ratio) else 0.0
        hybrid_p95 = float(np.percentile(hybrid_ratio, 95)) if len(hybrid_ratio) else 0.0
        step_raw_max = float(raw_ratio.max()) if len(raw_ratio) else 0.0
        step_hybrid_max = float(hybrid_ratio.max()) if len(hybrid_ratio) else 0.0
        step_raw_min = float(raw_ratio.min()) if len(raw_ratio) else 0.0
        step_hybrid_min = float(hybrid_ratio.min()) if len(hybrid_ratio) else 0.0
        per_step.append(
            {
                "step": step_index,
                "raw_flips": int(raw_flips.sum()),
                "hybrid_flips": int(hybrid_flips.sum()),
                "new_flips": new_flips,
                "worsened_expansions_over_limit": worsened_expansion,
                "worsened_contractions_under_limit": worsened_contraction,
                "worsened_faces_under_area_limit": worsened_area_collapse,
                "worsened_seam_groups": step_worsened_seams,
                "raw_edge_ratio_p95": raw_p95,
                "hybrid_edge_ratio_p95": hybrid_p95,
                "raw_edge_ratio_min": step_raw_min,
                "hybrid_edge_ratio_min": step_hybrid_min,
                "raw_edge_ratio_max": step_raw_max,
                "hybrid_edge_ratio_max": step_hybrid_max,
                "raw_face_area_ratio_min": float(raw_area_ratio.min()),
                "hybrid_face_area_ratio_min": float(hybrid_area_ratio.min()),
                "raw_worst_seam_split": step_raw_seam_worst,
                "hybrid_worst_seam_split": step_hybrid_seam_worst,
            }
        )
        raw_flip_max = max(raw_flip_max, int(raw_flips.sum()))
        hybrid_flip_max = max(hybrid_flip_max, int(hybrid_flips.sum()))
        new_flip_total += new_flips
        expansion_total += worsened_expansion
        contraction_total += worsened_contraction
        area_collapse_total += worsened_area_collapse
        worsened_seam_total += step_worsened_seams
        raw_seam_worst = max(raw_seam_worst, step_raw_seam_worst)
        hybrid_seam_worst = max(hybrid_seam_worst, step_hybrid_seam_worst)
        raw_p95_max = max(raw_p95_max, raw_p95)
        hybrid_p95_max = max(hybrid_p95_max, hybrid_p95)
        raw_ratio_max = max(raw_ratio_max, step_raw_max)
        hybrid_ratio_max = max(hybrid_ratio_max, step_hybrid_max)
        raw_ratio_min = min(raw_ratio_min, step_raw_min)
        hybrid_ratio_min = min(hybrid_ratio_min, step_hybrid_min)
        raw_area_ratio_min = min(raw_area_ratio_min, float(raw_area_ratio.min()))
        hybrid_area_ratio_min = min(
            hybrid_area_ratio_min, float(hybrid_area_ratio.min())
        )
    return {
        "selected": int(mask.sum()),
        "morph_steps": len(raw_targets),
        "raw_flips_max_per_step": raw_flip_max,
        "hybrid_flips_max_per_step": hybrid_flip_max,
        "new_flips": new_flip_total,
        "raw_edge_ratio_p95_max": raw_p95_max,
        "hybrid_edge_ratio_p95_max": hybrid_p95_max,
        "raw_edge_ratio_min": raw_ratio_min,
        "hybrid_edge_ratio_min": hybrid_ratio_min,
        "raw_edge_ratio_max": raw_ratio_max,
        "hybrid_edge_ratio_max": hybrid_ratio_max,
        "worsened_edges_over_limit": expansion_total,
        "worsened_contractions_under_limit": contraction_total,
        "worsened_faces_under_area_limit": area_collapse_total,
        "raw_face_area_ratio_min": raw_area_ratio_min,
        "hybrid_face_area_ratio_min": hybrid_area_ratio_min,
        "raw_worst_seam_split": raw_seam_worst,
        "hybrid_worst_seam_split": hybrid_seam_worst,
        "worsened_seam_groups": worsened_seam_total,
        "per_step": per_step,
    }


def classify_support(
    *,
    base: np.ndarray,
    tris: np.ndarray,
    edges: np.ndarray,
    fixed: np.ndarray,
    requested: np.ndarray,
    authored_distance: np.ndarray,
    body_scale: np.ndarray,
    support_points: np.ndarray,
    raw_targets: np.ndarray,
    corot_targets: np.ndarray,
    seam_groups: Sequence[Sequence[int]] = (),
    thresholds: CoherentV1Thresholds = THRESHOLDS,
) -> dict:
    """Return frozen broad/coherent-v1 masks plus auditable diagnostics."""
    base = np.asarray(base, dtype=np.float64)
    tris = np.asarray(tris, dtype=np.int32)
    edges = np.asarray(edges, dtype=np.int32)
    fixed = np.asarray(fixed, dtype=bool)
    requested = np.asarray(requested, dtype=bool)
    authored_distance = np.asarray(authored_distance, dtype=np.float64)
    body_scale = np.asarray(body_scale, dtype=np.float64)
    support_points = np.asarray(support_points, dtype=np.float64)
    raw_targets = np.asarray(raw_targets, dtype=np.float64)
    corot_targets = np.asarray(corot_targets, dtype=np.float64)
    if raw_targets.ndim == 2:
        raw_targets = raw_targets[None, :, :]
    if corot_targets.ndim == 2:
        corot_targets = corot_targets[None, :, :]
    _validate(
        base, tris, edges, fixed, requested, authored_distance, body_scale,
        support_points, raw_targets, corot_targets,
    )

    adjacency = _adjacency(len(base), edges)
    # Treat exact duplicated material seams as welded graph adjacency for
    # support reachability/components.  Target membership remains all-or-none.
    for group in seam_groups:
        if len(group) < 2:
            continue
        root = int(group[0])
        for raw_vertex in group[1:]:
            vertex = int(raw_vertex)
            adjacency[root].append(vertex)
            adjacency[vertex].append(root)
    reachable = _reachable_from_fixed(fixed, requested, adjacency)
    lever_ratio = np.full(len(base), np.inf, dtype=np.float64)
    lever_ratio[requested] = authored_distance[requested] / body_scale[requested]

    coherent_edges = np.zeros(len(edges), dtype=bool)
    degree = np.zeros(len(base), dtype=np.int32)
    good_degree = np.zeros(len(base), dtype=np.int32)
    corot_displacement = corot_targets - base[None, :, :]
    for edge_index, (a_raw, b_raw) in enumerate(edges):
        a, b = int(a_raw), int(b_raw)
        if not (requested[a] and requested[b]):
            continue
        length = max(float(np.linalg.norm(base[a] - base[b])), 1.0e-12)
        support_jump = float(np.linalg.norm(support_points[a] - support_points[b]) / length)
        displacement_gradient = float(
            np.linalg.norm(
                corot_displacement[:, a] - corot_displacement[:, b], axis=1
            ).max()
            / length
        )
        transported_ratios = np.linalg.norm(
            corot_targets[:, a] - corot_targets[:, b], axis=1
        ) / length
        good = (
            support_jump <= thresholds.support_jump_over_cloth_edge_max
            and displacement_gradient <= thresholds.displacement_gradient_max
            and float(transported_ratios.min()) >= thresholds.transported_edge_ratio_min
            and float(transported_ratios.max()) <= thresholds.transported_edge_ratio_max
        )
        coherent_edges[edge_index] = good
        degree[a] += 1
        degree[b] += 1
        if good:
            good_degree[a] += 1
            good_degree[b] += 1
    coherent_fraction = np.divide(
        good_degree, degree, out=np.zeros(len(base), dtype=np.float64), where=degree > 0
    )
    coherent_vertex = (degree > 0) & (coherent_fraction + thresholds.epsilon >= thresholds.coherent_incident_fraction_min)
    provisional = (
        requested
        & reachable
        & (lever_ratio <= thresholds.lever_over_body_edge_max + thresholds.epsilon)
        & coherent_vertex
    )

    # Mixed seam membership would prescribe two targets to coincident material.
    removals: set[int] = set()
    _force_seam_removals(provisional, removals, seam_groups)
    if removals:
        provisional[np.fromiter(sorted(removals), dtype=np.int32)] = False

    coherent = provisional.copy()
    prune_iterations = 0
    pruned_vertices = 0
    component_pruned_vertices = 0
    while True:
        removals: set[int] = set()
        for raw_target, corot_target in zip(raw_targets, corot_targets):
            hybrid = raw_target.copy()
            hybrid[coherent] = corot_target[coherent]
            raw_flips = _face_flip_mask(raw_target, base, tris)
            hybrid_flips = _face_flip_mask(hybrid, base, tris)
            raw_edge_ratio = _edge_ratios(raw_target, base, edges)
            hybrid_edge_ratio = _edge_ratios(hybrid, base, edges)
            raw_area_ratio = _face_area_ratios(raw_target, base, tris)
            hybrid_area_ratio = _face_area_ratios(hybrid, base, tris)
            removals.update(
                map(int, tris[hybrid_flips & ~raw_flips].ravel())
            )
            bad_expansion = edges[
                hybrid_edge_ratio
                > np.maximum(
                    thresholds.hybrid_edge_ratio_max,
                    raw_edge_ratio + thresholds.epsilon,
                )
            ]
            bad_contraction = edges[
                hybrid_edge_ratio
                < np.minimum(
                    thresholds.transported_edge_ratio_min,
                    raw_edge_ratio - thresholds.epsilon,
                )
            ]
            removals.update(map(int, bad_expansion.ravel()))
            removals.update(map(int, bad_contraction.ravel()))
            bad_area = tris[
                hybrid_area_ratio
                < np.minimum(
                    thresholds.hybrid_face_area_ratio_min,
                    raw_area_ratio - thresholds.epsilon,
                )
            ]
            removals.update(map(int, bad_area.ravel()))
            for group in seam_groups:
                idx = np.asarray(group, dtype=np.int32)
                if len(idx) < 2:
                    continue
                raw_split = float(
                    np.linalg.norm(
                        raw_target[idx] - raw_target[idx[0]], axis=1
                    ).max()
                )
                hybrid_split = float(
                    np.linalg.norm(hybrid[idx] - hybrid[idx[0]], axis=1).max()
                )
                if hybrid_split > raw_split + thresholds.epsilon:
                    removals.update(map(int, idx))
        component_removals = _invalid_support_components(
            coherent, fixed, adjacency, tris
        )
        component_pruned_vertices += sum(
            1 for vertex in component_removals if coherent[vertex]
        )
        removals.update(component_removals)
        _force_seam_removals(coherent, removals, seam_groups)
        remove = np.asarray(sorted(v for v in removals if coherent[v]), dtype=np.int32)
        if not len(remove):
            break
        coherent[remove] = False
        pruned_vertices += len(remove)
        prune_iterations += 1
        if prune_iterations > len(base):
            raise RuntimeError("coherent-v1 integrity closure did not converge")

    broad_eval = _evaluate_target(
        requested, base, tris, edges, raw_targets, corot_targets,
        seam_groups, thresholds,
    )
    coherent_eval = _evaluate_target(
        coherent, base, tris, edges, raw_targets, corot_targets,
        seam_groups, thresholds,
    )
    selected_edge_count = int(
        np.logical_and(coherent[edges[:, 0]], coherent[edges[:, 1]]).sum()
    )
    selected_triangle_count = int(np.all(coherent[tris], axis=1).sum())
    selected_fixed_count = int(np.logical_and(coherent, fixed).sum())
    selected_lever_max = (
        float(lever_ratio[coherent].max()) if coherent.any() else float("inf")
    )
    selected_coherence_min = (
        float(coherent_fraction[coherent].min()) if coherent.any() else 0.0
    )
    final_invalid_components = _invalid_support_components(
        coherent, fixed, adjacency, tris
    )
    nonvacuous_patch = (
        int(coherent.sum()) >= 3
        and selected_edge_count >= 2
        and selected_triangle_count >= 1
        and selected_fixed_count >= 1
    )
    coherent_gate = (
        coherent_eval["new_flips"] == 0
        and coherent_eval["worsened_edges_over_limit"] == 0
        and coherent_eval["worsened_contractions_under_limit"] == 0
        and coherent_eval["worsened_faces_under_area_limit"] == 0
        and coherent_eval["worsened_seam_groups"] == 0
        and coherent_eval["hybrid_worst_seam_split"] <= coherent_eval["raw_worst_seam_split"] + thresholds.epsilon
        and selected_lever_max <= thresholds.lever_over_body_edge_max + thresholds.epsilon
        and nonvacuous_patch
        and not final_invalid_components
    )
    if not coherent_gate:
        raise RuntimeError("coherent-v1 failed its construction-time integrity gate")

    return {
        "thresholds": asdict(thresholds),
        "masks": {"broad": requested, "coherent-v1": coherent},
        "diagnostics": {
            "fixed": int(fixed.sum()),
            "requested": int(requested.sum()),
            "requested_reachable_from_fixed": int((requested & reachable).sum()),
            "requested_orphan": int((requested & ~reachable).sum()),
            "provisional_coherent": int(provisional.sum()),
            "integrity_prune_iterations": prune_iterations,
            "integrity_pruned_vertices": pruned_vertices,
            "component_pruned_vertices": component_pruned_vertices,
            "final_invalid_component_vertices": len(final_invalid_components),
            "selected_fixed": selected_fixed_count,
            "selected_material_edges": selected_edge_count,
            "selected_material_triangles": selected_triangle_count,
            "nonvacuous_patch_pass": nonvacuous_patch,
            "lever_ratio_selected_max": selected_lever_max,
            "coherent_fraction_selected_min": selected_coherence_min,
            "broad_target": broad_eval,
            "coherent_v1_target": coherent_eval,
            "coherent_v1_gate_pass": coherent_gate,
        },
    }


def selftest() -> None:
    base = np.array([[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [1.0, 1.0, 0.0], [0.0, 1.0, 0.0]])
    tris = np.array([[0, 1, 2], [0, 2, 3]], dtype=np.int32)
    edges = np.array([[0, 1], [0, 2], [0, 3], [1, 2], [2, 3]], dtype=np.int32)
    fixed = np.ones(4, dtype=bool)
    requested = np.ones(4, dtype=bool)
    raw = base + np.array([0.0, 0.0, 1.0])
    corot = raw.copy()
    result = classify_support(
        base=base, tris=tris, edges=edges, fixed=fixed, requested=requested,
        authored_distance=np.ones(4), body_scale=np.ones(4),
        support_points=base.copy(), raw_targets=raw, corot_targets=corot,
    )
    if result["masks"]["coherent-v1"].sum() != 4:
        raise RuntimeError("coherent-v1 selftest rejected a rigid coherent patch")
    far = np.ones(4)
    far[3] = 3.0
    result = classify_support(
        base=base, tris=tris, edges=edges, fixed=fixed, requested=requested,
        authored_distance=far, body_scale=np.ones(4),
        support_points=base.copy(), raw_targets=raw, corot_targets=corot,
    )
    if result["masks"]["coherent-v1"][3]:
        raise RuntimeError("coherent-v1 selftest accepted an overlong lever")
    print("pathb_support_classifier selftest PASS")


if __name__ == "__main__":
    selftest()
