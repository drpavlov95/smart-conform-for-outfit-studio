#!/usr/bin/env python3
"""Garment-agnostic local body-imprint metrics for Path-B research.

This module deliberately has no access to garment names, fixture roles, hand
sculpts, vertex allowlists, or ground truth.  Its activation mask is frozen
from authored cloth, raw conform, fitted drape, and the authored/morphed body
pair before a candidate is inspected.

The metric measures the positive high-frequency projection of cloth motion on
the corresponding local body-morph motion.  Exact coincident material seams
are welded into graph nodes before the degree-normalized Laplacian is applied.
Error-island thresholds are frozen from the raw conform and island summaries
are weighted by authored surface area.

The public entry points are:

``prepare_context`` / ``prepare_context_from_capture``
    Build the welded graph, frozen mask, and body-material bindings.
``freeze_raw_error_threshold``
    Freeze an island threshold from raw conform only.
``measure_imprint``
    Measure one candidate on a frozen mask.
``compare_causal_delta``
    Compare entry and candidate on exactly the same mask and threshold.

Run ``python tools/pathb_local_metrics.py --selftest`` for dependency-free
synthetic checks (apart from NumPy and shell_spike's normal dependencies).
"""

from __future__ import annotations

import argparse
import hashlib
import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Sequence

import numpy as np

try:  # Importable as tools.pathb_local_metrics and directly as a script.
    from tools import shell_spike as ss
except ModuleNotFoundError:  # pragma: no cover - exercised by direct CLI use.
    import shell_spike as ss  # type: ignore[no-redef]


DEFAULT_DRAPE_THRESHOLD = 0.55
DEFAULT_GAP_THRESHOLD = 2.5
DEFAULT_MIN_COMPONENT_NODES = 32
DEFAULT_MIN_METRIC_NODES = 8
DEFAULT_BODY_SIGNAL_EPSILON = 1.0e-10
DEFAULT_MAD_MULTIPLIER = 3.0


@dataclass(frozen=True)
class MaterialGraph:
    """Triangle material graph after exact coincident seams are welded."""

    vertex_to_node: np.ndarray
    node_members: tuple[np.ndarray, ...]
    adjacency: tuple[np.ndarray, ...]
    node_edges: np.ndarray
    vertex_area: np.ndarray
    node_area: np.ndarray
    node_position: np.ndarray
    seam_group_count: int
    base_sha256: str

    @property
    def vertex_count(self) -> int:
        return int(len(self.vertex_to_node))

    @property
    def node_count(self) -> int:
        return int(len(self.node_members))


@dataclass(frozen=True)
class FrozenLocalMask:
    """Candidate-independent local metric population."""

    vertex_mask: np.ndarray
    node_mask: np.ndarray
    interior_node_mask: np.ndarray
    authored_gap: np.ndarray
    raw_gap: np.ndarray
    drape: np.ndarray
    drape_threshold: float
    gap_threshold: float
    min_component_nodes: int
    vertex_count: int
    node_count: int
    interior_node_count: int
    mask_sha256: str


@dataclass(frozen=True)
class BodyBinding:
    """Reference-body material correspondence and local outward direction."""

    valid: np.ndarray
    closest_point: np.ndarray
    triangle: np.ndarray
    barycentric: np.ndarray
    direction: np.ndarray
    body_motion: np.ndarray


@dataclass(frozen=True)
class ErrorThreshold:
    """Robust raw-only threshold used by every candidate error-island score."""

    valid: bool
    reason: str | None
    value: float | None
    median: float | None
    mad: float | None
    scaled_mad: float | None
    mad_multiplier: float
    minimum_value: float
    node_count: int
    mask_sha256: str


@dataclass(frozen=True)
class ErrorIslandMetrics:
    valid: bool
    reason: str | None
    threshold: float | None
    island_count: int
    error_node_count: int
    valid_area: float | None
    error_area: float | None
    error_area_fraction: float | None
    area_weighted_excess: float | None
    excess_integral: float | None
    aligned_score_p95: float | None
    largest_island_area: float | None
    largest_island_area_fraction: float | None
    largest_island_excess_integral: float | None
    largest_island_node_count: int


@dataclass(frozen=True)
class ImprintMetrics:
    valid: bool
    reason: str | None
    mask_sha256: str
    node_count: int
    area: float | None
    positive_projection_energy: float | None
    correlation: float | None
    alpha: float | None
    alpha_positive: float | None
    garment_laplacian_abs_p95: float | None
    aligned_score_p95: float | None
    body_laplacian_rms: float | None
    garment_laplacian_rms: float | None
    islands: ErrorIslandMetrics | None

    def as_dict(self) -> dict:
        return asdict(self)


@dataclass(frozen=True)
class WholePanelTransferMetrics:
    """Low-frequency transfer of a body morph into always-clear free cloth.

    Unlike :class:`ImprintMetrics`, this deliberately does not take a graph
    Laplacian.  It measures the whole free material component after removing
    one constant radial offset per measurement.  A constant offset can be a
    legitimate panel translation; a regression slope near one means that the
    *shape* of the distant cloth is still copying the corresponding body
    morph.
    """

    valid: bool
    reason: str | None
    mask_sha256: str
    node_count: int
    area: float | None
    body_motion_mean: float | None
    body_motion_std: float | None
    garment_motion_mean: float | None
    garment_motion_std: float | None
    alpha: float | None
    alpha_positive: float | None
    correlation: float | None
    intercept: float | None

    def as_dict(self) -> dict:
        return asdict(self)


@dataclass(frozen=True)
class WholePanelTransferGate:
    """One candidate's bounded-free-cloth transfer verdict."""

    valid: bool
    reason: str | None
    passed: bool
    raw_alpha_positive: float | None
    candidate_alpha_positive: float | None
    alpha_positive_ratio: float | None
    max_alpha_positive_ratio: float
    candidate_correlation: float | None
    max_correlation: float
    failures: tuple[str, ...]

    def as_dict(self) -> dict:
        return asdict(self)


@dataclass(frozen=True)
class CausalDeltaMetrics:
    """Candidate minus entry, with a shared frozen mask and raw threshold."""

    valid: bool
    reason: str | None
    mask_sha256: str
    threshold: ErrorThreshold
    entry: ImprintMetrics
    candidate: ImprintMetrics
    positive_projection_energy_delta: float | None
    positive_projection_energy_reduction_fraction: float | None
    correlation_delta: float | None
    alpha_delta: float | None
    garment_laplacian_abs_p95_delta: float | None
    aligned_score_p95_delta: float | None
    error_area_delta: float | None
    area_weighted_excess_delta: float | None
    excess_integral_delta: float | None
    largest_island_area_delta: float | None

    def as_dict(self) -> dict:
        return asdict(self)


@dataclass(frozen=True)
class LocalMetricContext:
    """All candidate-independent inputs needed by the local metric."""

    base_vertices: np.ndarray
    raw_vertices: np.ndarray
    graph: MaterialGraph
    frozen_mask: FrozenLocalMask
    body_binding: BodyBinding
    min_metric_nodes: int = DEFAULT_MIN_METRIC_NODES
    body_signal_epsilon: float = DEFAULT_BODY_SIGNAL_EPSILON


def _sha_mask(mask: np.ndarray) -> str:
    values = np.asarray(mask, dtype=np.uint8)
    payload = np.asarray([len(values)], dtype="<i8").tobytes() + values.tobytes()
    return hashlib.sha256(payload).hexdigest()


def _sha_vertices(verts: np.ndarray) -> str:
    values = np.ascontiguousarray(np.asarray(verts, dtype="<f8"))
    return hashlib.sha256(values.tobytes()).hexdigest()


def _validate_vertices(label: str, values: np.ndarray) -> np.ndarray:
    out = np.asarray(values, dtype=np.float64)
    if out.ndim != 2 or out.shape[1] != 3:
        raise ValueError(f"{label} must be Nx3")
    if not np.isfinite(out).all():
        raise ValueError(f"{label} contains non-finite values")
    return out


def _validate_tris(label: str, values: np.ndarray, vertex_count: int) -> np.ndarray:
    out = np.asarray(values, dtype=np.int32)
    if out.ndim != 2 or out.shape[1] != 3:
        raise ValueError(f"{label} must be Mx3")
    if len(out) and (int(out.min()) < 0 or int(out.max()) >= vertex_count):
        raise ValueError(f"{label} contains an out-of-range vertex")
    return out


def _unique_triangle_edges(tris: np.ndarray) -> np.ndarray:
    edges: set[tuple[int, int]] = set()
    for tri in tris:
        a, b, c = map(int, tri)
        for u, v in ((a, b), (b, c), (c, a)):
            edges.add((u, v) if u < v else (v, u))
    if not edges:
        return np.empty((0, 2), dtype=np.int32)
    return np.asarray(sorted(edges), dtype=np.int32)


def _vertex_areas(verts: np.ndarray, tris: np.ndarray) -> np.ndarray:
    areas = np.zeros(len(verts), dtype=np.float64)
    if not len(tris):
        return areas
    tri_area = 0.5 * np.linalg.norm(
        np.cross(
            verts[tris[:, 1]] - verts[tris[:, 0]],
            verts[tris[:, 2]] - verts[tris[:, 0]],
        ),
        axis=1,
    )
    share = tri_area / 3.0
    for corner in range(3):
        np.add.at(areas, tris[:, corner], share)
    return areas


def build_material_graph(
    base_vertices: np.ndarray,
    tris: np.ndarray,
    *,
    seam_groups: Sequence[Sequence[int]] | None = None,
) -> MaterialGraph:
    """Build a deterministic triangle graph with exact material seams welded.

    When ``seam_groups`` is omitted, vertices with bitwise-equal numeric
    positions (Python float tuple equality) are treated as seam twins.  This is
    the same conservative seam signal used by ``shell_spike.topology_stats``.
    """

    base = _validate_vertices("base_vertices", base_vertices)
    faces = _validate_tris("tris", tris, len(base))
    parent = np.arange(len(base), dtype=np.int32)

    def find(index: int) -> int:
        root = index
        while int(parent[root]) != root:
            root = int(parent[root])
        while int(parent[index]) != index:
            next_index = int(parent[index])
            parent[index] = root
            index = next_index
        return root

    def union(a: int, b: int) -> None:
        ra, rb = find(a), find(b)
        if ra == rb:
            return
        if ra < rb:
            parent[rb] = ra
        else:
            parent[ra] = rb

    if seam_groups is None:
        coincident: dict[tuple[float, float, float], list[int]] = {}
        for vi, point in enumerate(base):
            coincident.setdefault(tuple(map(float, point)), []).append(vi)
        groups = [members for members in coincident.values() if len(members) > 1]
    else:
        groups = [list(map(int, members)) for members in seam_groups if len(members) > 1]
    for members in groups:
        if min(members) < 0 or max(members) >= len(base):
            raise ValueError("seam_groups contains an out-of-range vertex")
        first = members[0]
        for other in members[1:]:
            union(first, other)

    root_members: dict[int, list[int]] = {}
    for vi in range(len(base)):
        root_members.setdefault(find(vi), []).append(vi)
    ordered_members = sorted(root_members.values(), key=lambda members: members[0])
    vertex_to_node = np.empty(len(base), dtype=np.int32)
    node_members: list[np.ndarray] = []
    for node, members in enumerate(ordered_members):
        ids = np.asarray(members, dtype=np.int32)
        vertex_to_node[ids] = node
        node_members.append(ids)

    material_edges: set[tuple[int, int]] = set()
    for a, b in _unique_triangle_edges(faces):
        na, nb = int(vertex_to_node[int(a)]), int(vertex_to_node[int(b)])
        if na != nb:
            material_edges.add((na, nb) if na < nb else (nb, na))
    node_edges = (
        np.asarray(sorted(material_edges), dtype=np.int32)
        if material_edges
        else np.empty((0, 2), dtype=np.int32)
    )
    adjacency_sets = [set() for _ in node_members]
    for a, b in node_edges:
        adjacency_sets[int(a)].add(int(b))
        adjacency_sets[int(b)].add(int(a))
    adjacency = tuple(
        np.asarray(sorted(neighbours), dtype=np.int32)
        for neighbours in adjacency_sets
    )

    vertex_area = _vertex_areas(base, faces)
    node_area = np.bincount(
        vertex_to_node, weights=vertex_area, minlength=len(node_members)
    ).astype(np.float64)
    node_position = np.zeros((len(node_members), 3), dtype=np.float64)
    for node, members in enumerate(node_members):
        weights = vertex_area[members]
        if float(weights.sum()) > 1.0e-20:
            node_position[node] = np.average(base[members], axis=0, weights=weights)
        else:
            node_position[node] = np.mean(base[members], axis=0)

    return MaterialGraph(
        vertex_to_node=vertex_to_node,
        node_members=tuple(node_members),
        adjacency=adjacency,
        node_edges=node_edges,
        vertex_area=vertex_area,
        node_area=node_area,
        node_position=node_position,
        seam_group_count=len(groups),
        base_sha256=_sha_vertices(base),
    )


def collapse_vertex_values(
    graph: MaterialGraph,
    values: np.ndarray,
    *,
    weights: np.ndarray | None = None,
) -> np.ndarray:
    """Area-weight vertex values onto welded material nodes."""

    data = np.asarray(values, dtype=np.float64)
    if data.shape[0] != graph.vertex_count:
        raise ValueError("values must have one row per graph vertex")
    if weights is None:
        vertex_weights = graph.vertex_area.copy()
    else:
        vertex_weights = np.asarray(weights, dtype=np.float64)
        if vertex_weights.shape != (graph.vertex_count,):
            raise ValueError("weights must have one entry per graph vertex")
        if np.any(vertex_weights < 0.0) or not np.isfinite(vertex_weights).all():
            raise ValueError("weights must be finite and non-negative")
    trailing = data.shape[1:]
    flat = data.reshape(graph.vertex_count, -1)
    out = np.zeros((graph.node_count, flat.shape[1]), dtype=np.float64)
    denominator = np.bincount(
        graph.vertex_to_node, weights=vertex_weights, minlength=graph.node_count
    ).astype(np.float64)
    for column in range(flat.shape[1]):
        np.add.at(
            out[:, column],
            graph.vertex_to_node,
            flat[:, column] * vertex_weights,
        )
    positive = denominator > 1.0e-20
    out[positive] /= denominator[positive, None]
    for node in np.flatnonzero(~positive):
        out[node] = np.mean(flat[graph.node_members[int(node)]], axis=0)
    return out.reshape((graph.node_count,) + trailing)


def degree_normalized_laplacian(
    graph: MaterialGraph, node_values: np.ndarray
) -> np.ndarray:
    """Return ``x_i - mean(x_j)`` over welded-node neighbours."""

    values = np.asarray(node_values, dtype=np.float64)
    if values.shape[0] != graph.node_count:
        raise ValueError("node_values must have one row per graph node")
    out = np.full_like(values, np.nan, dtype=np.float64)
    for node, neighbours in enumerate(graph.adjacency):
        if len(neighbours):
            out[node] = values[node] - np.mean(values[neighbours], axis=0)
    return out


def _prune_small_components(
    mask: np.ndarray, graph: MaterialGraph, minimum: int
) -> np.ndarray:
    selected = np.asarray(mask, dtype=bool).copy()
    seen = np.zeros(graph.node_count, dtype=bool)
    for seed in np.flatnonzero(selected):
        if seen[seed]:
            continue
        component: list[int] = []
        stack = [int(seed)]
        seen[seed] = True
        while stack:
            node = stack.pop()
            component.append(node)
            for neighbour in graph.adjacency[node]:
                ni = int(neighbour)
                if selected[ni] and not seen[ni]:
                    seen[ni] = True
                    stack.append(ni)
        if len(component) < minimum:
            selected[np.asarray(component, dtype=np.int32)] = False
    return selected


def freeze_local_mask(
    *,
    base_vertices: np.ndarray,
    raw_vertices: np.ndarray,
    drape: np.ndarray,
    graph: MaterialGraph,
    ref_body_vertices: np.ndarray,
    body_vertices: np.ndarray,
    body_tris: np.ndarray,
    drape_threshold: float = DEFAULT_DRAPE_THRESHOLD,
    gap_threshold: float = DEFAULT_GAP_THRESHOLD,
    min_component_nodes: int = DEFAULT_MIN_COMPONENT_NODES,
) -> FrozenLocalMask:
    """Freeze the S05 population from authored/raw inputs, never a candidate."""

    base = _validate_vertices("base_vertices", base_vertices)
    raw = _validate_vertices("raw_vertices", raw_vertices)
    if raw.shape != base.shape or graph.vertex_count != len(base):
        raise ValueError("base/raw/graph vertex counts differ")
    if graph.base_sha256 != _sha_vertices(base):
        raise ValueError("graph was built from a different authored mesh")
    drape_values = np.asarray(drape, dtype=np.float64)
    if drape_values.shape != (len(base),) or not np.isfinite(drape_values).all():
        raise ValueError("drape must be one finite value per cloth vertex")
    ref_body = _validate_vertices("ref_body_vertices", ref_body_vertices)
    body = _validate_vertices("body_vertices", body_vertices)
    if ref_body.shape != body.shape:
        raise ValueError("reference and morphed body vertex counts differ")
    faces = _validate_tris("body_tris", body_tris, len(ref_body))
    authored_gap = ss.UniformBodyCollider(ref_body, faces).gaps(base)
    raw_gap = ss.UniformBodyCollider(body, faces).gaps(raw)
    vertex_predicate = (
        (drape_values >= float(drape_threshold))
        & np.isfinite(authored_gap)
        & np.isfinite(raw_gap)
        & (authored_gap >= float(gap_threshold))
        & (raw_gap >= float(gap_threshold))
    )

    selected_per_node = np.bincount(
        graph.vertex_to_node,
        weights=vertex_predicate.astype(np.int32),
        minlength=graph.node_count,
    )
    member_count = np.asarray([len(members) for members in graph.node_members])
    node_mask = selected_per_node == member_count
    node_mask = _prune_small_components(
        node_mask, graph, max(1, int(min_component_nodes))
    )
    vertex_mask = node_mask[graph.vertex_to_node]
    interior = node_mask.copy()
    for node in np.flatnonzero(node_mask):
        neighbours = graph.adjacency[int(node)]
        if len(neighbours) < 2 or not np.all(node_mask[neighbours]):
            interior[node] = False
    mask_hash = _sha_mask(interior)
    return FrozenLocalMask(
        vertex_mask=vertex_mask,
        node_mask=node_mask,
        interior_node_mask=interior,
        authored_gap=authored_gap,
        raw_gap=raw_gap,
        drape=drape_values.copy(),
        drape_threshold=float(drape_threshold),
        gap_threshold=float(gap_threshold),
        min_component_nodes=max(1, int(min_component_nodes)),
        vertex_count=int(vertex_mask.sum()),
        node_count=int(node_mask.sum()),
        interior_node_count=int(interior.sum()),
        mask_sha256=mask_hash,
    )


def bind_local_body_motion(
    *,
    base_vertices: np.ndarray,
    ref_body_vertices: np.ndarray,
    body_vertices: np.ndarray,
    body_tris: np.ndarray,
    epsilon: float = 1.0e-12,
) -> BodyBinding:
    """Bind cloth to closest reference-body material and sample its morph."""

    base = _validate_vertices("base_vertices", base_vertices)
    ref_body = _validate_vertices("ref_body_vertices", ref_body_vertices)
    body = _validate_vertices("body_vertices", body_vertices)
    if body.shape != ref_body.shape:
        raise ValueError("reference and morphed body vertex counts differ")
    faces = _validate_tris("body_tris", body_tris, len(ref_body))
    collider = ss.UniformBodyCollider(ref_body, faces)
    valid = np.zeros(len(base), dtype=bool)
    closest = np.full_like(base, np.nan)
    triangle = np.full(len(base), -1, dtype=np.int32)
    barycentric = np.full((len(base), 3), np.nan, dtype=np.float64)
    direction = np.full_like(base, np.nan)
    body_motion = np.full_like(base, np.nan)
    for vi, point in enumerate(base):
        hit = collider.closest(point)
        if hit is None:
            continue
        cp, normal, _gap, ti = hit
        tri = faces[int(ti)]
        bary = ss.barycentric_coordinates(cp, ref_body[tri])
        if bary is None:
            continue
        radial = point - cp
        length = float(np.linalg.norm(radial))
        if length <= epsilon:
            radial = np.asarray(normal, dtype=np.float64)
            length = float(np.linalg.norm(radial))
        if length <= epsilon:
            continue
        closest[vi] = cp
        triangle[vi] = int(ti)
        barycentric[vi] = bary
        direction[vi] = radial / length
        body_motion[vi] = np.sum(
            (body[tri] - ref_body[tri]) * bary[:, None], axis=0
        )
        valid[vi] = True
    return BodyBinding(
        valid=valid,
        closest_point=closest,
        triangle=triangle,
        barycentric=barycentric,
        direction=direction,
        body_motion=body_motion,
    )


def prepare_context(
    *,
    base_vertices: np.ndarray,
    raw_vertices: np.ndarray,
    cloth_tris: np.ndarray,
    drape: np.ndarray,
    ref_body_vertices: np.ndarray,
    body_vertices: np.ndarray,
    body_tris: np.ndarray,
    seam_groups: Sequence[Sequence[int]] | None = None,
    drape_threshold: float = DEFAULT_DRAPE_THRESHOLD,
    gap_threshold: float = DEFAULT_GAP_THRESHOLD,
    min_component_nodes: int = DEFAULT_MIN_COMPONENT_NODES,
    min_metric_nodes: int = DEFAULT_MIN_METRIC_NODES,
    body_signal_epsilon: float = DEFAULT_BODY_SIGNAL_EPSILON,
) -> LocalMetricContext:
    """Prepare all candidate-independent S05 inputs."""

    base = _validate_vertices("base_vertices", base_vertices)
    raw = _validate_vertices("raw_vertices", raw_vertices)
    graph = build_material_graph(base, cloth_tris, seam_groups=seam_groups)
    frozen = freeze_local_mask(
        base_vertices=base,
        raw_vertices=raw,
        drape=drape,
        graph=graph,
        ref_body_vertices=ref_body_vertices,
        body_vertices=body_vertices,
        body_tris=body_tris,
        drape_threshold=drape_threshold,
        gap_threshold=gap_threshold,
        min_component_nodes=min_component_nodes,
    )
    binding = bind_local_body_motion(
        base_vertices=base,
        ref_body_vertices=ref_body_vertices,
        body_vertices=body_vertices,
        body_tris=body_tris,
    )
    return LocalMetricContext(
        base_vertices=base.copy(),
        raw_vertices=raw.copy(),
        graph=graph,
        frozen_mask=frozen,
        body_binding=binding,
        min_metric_nodes=max(1, int(min_metric_nodes)),
        body_signal_epsilon=float(body_signal_epsilon),
    )


def prepare_context_from_capture(
    capture: ss.CaptureFront,
    **kwargs,
) -> LocalMetricContext:
    """Prepare a context from a parsed capture without reading its metadata."""

    if not np.array_equal(capture.ref_body_tris, capture.body_tris):
        raise ValueError("reference and morphed body topology differs")
    return prepare_context(
        base_vertices=capture.base_verts,
        raw_vertices=capture.base_verts + capture.raw_diff,
        cloth_tris=capture.tris,
        drape=ss.fitted_drape_signal(capture),
        ref_body_vertices=capture.ref_body_verts,
        body_vertices=capture.body_verts,
        body_tris=capture.body_tris,
        **kwargs,
    )


def _resolve_node_mask(
    context: LocalMetricContext,
    measurement_mask: np.ndarray | None,
) -> tuple[np.ndarray, str]:
    frozen = context.frozen_mask.interior_node_mask
    if measurement_mask is None:
        selected = frozen.copy()
    else:
        supplied = np.asarray(measurement_mask, dtype=bool)
        if supplied.shape == (context.graph.node_count,):
            selected = supplied.copy()
        elif supplied.shape == (context.graph.vertex_count,):
            count = np.bincount(
                context.graph.vertex_to_node,
                weights=supplied.astype(np.int32),
                minlength=context.graph.node_count,
            )
            members = np.asarray(
                [len(ids) for ids in context.graph.node_members], dtype=np.int32
            )
            selected = count == members
        else:
            raise ValueError("measurement_mask must be per vertex or welded node")
        if np.any(selected & ~frozen):
            raise ValueError("measurement_mask must be a subset of the frozen interior mask")
    return selected, _sha_mask(selected)


def _weighted_quantile(
    values: np.ndarray, weights: np.ndarray, quantile: float
) -> float | None:
    values = np.asarray(values, dtype=np.float64)
    weights = np.asarray(weights, dtype=np.float64)
    valid = np.isfinite(values) & np.isfinite(weights) & (weights > 0.0)
    if not np.any(valid):
        return None
    values, weights = values[valid], weights[valid]
    order = np.argsort(values, kind="stable")
    values, weights = values[order], weights[order]
    cumulative = np.cumsum(weights)
    target = float(np.clip(quantile, 0.0, 1.0)) * float(cumulative[-1])
    index = min(int(np.searchsorted(cumulative, target, side="left")), len(values) - 1)
    return float(values[index])


def _weighted_correlation(
    first: np.ndarray,
    second: np.ndarray,
    weights: np.ndarray,
    *,
    epsilon: float,
    minimum_count: int,
) -> float | None:
    if len(first) < minimum_count:
        return None
    total = float(weights.sum())
    if total <= epsilon:
        return None
    w = weights / total
    first_centered = first - float(np.sum(w * first))
    second_centered = second - float(np.sum(w * second))
    first_var = float(np.sum(w * first_centered * first_centered))
    second_var = float(np.sum(w * second_centered * second_centered))
    if first_var <= epsilon * epsilon or second_var <= epsilon * epsilon:
        return None
    return float(
        np.sum(w * first_centered * second_centered)
        / np.sqrt(first_var * second_var)
    )


@dataclass(frozen=True)
class _Fields:
    mask: np.ndarray
    mask_sha256: str
    valid: np.ndarray
    body_laplacian: np.ndarray
    garment_laplacian: np.ndarray
    aligned_score: np.ndarray


def _metric_fields(
    context: LocalMetricContext,
    candidate_vertices: np.ndarray,
    measurement_mask: np.ndarray | None,
) -> _Fields:
    candidate = _validate_vertices("candidate_vertices", candidate_vertices)
    if candidate.shape != context.base_vertices.shape:
        raise ValueError("candidate vertex count differs from context")
    node_mask, mask_hash = _resolve_node_mask(context, measurement_mask)
    binding = context.body_binding
    garment_scalar_vertex = np.full(context.graph.vertex_count, np.nan)
    body_scalar_vertex = np.full(context.graph.vertex_count, np.nan)
    garment_scalar_vertex[binding.valid] = np.einsum(
        "ij,ij->i",
        (candidate - context.base_vertices)[binding.valid],
        binding.direction[binding.valid],
    )
    body_scalar_vertex[binding.valid] = np.einsum(
        "ij,ij->i",
        binding.body_motion[binding.valid],
        binding.direction[binding.valid],
    )
    safe_garment = np.where(np.isfinite(garment_scalar_vertex), garment_scalar_vertex, 0.0)
    safe_body = np.where(np.isfinite(body_scalar_vertex), body_scalar_vertex, 0.0)
    node_garment = collapse_vertex_values(context.graph, safe_garment)
    node_body = collapse_vertex_values(context.graph, safe_body)
    body_laplacian = degree_normalized_laplacian(context.graph, node_body)
    garment_laplacian = degree_normalized_laplacian(context.graph, node_garment)
    valid_binding_count = np.bincount(
        context.graph.vertex_to_node,
        weights=binding.valid.astype(np.int32),
        minlength=context.graph.node_count,
    )
    member_count = np.asarray([len(ids) for ids in context.graph.node_members])
    valid = (
        node_mask
        & (valid_binding_count == member_count)
        & np.isfinite(body_laplacian)
        & np.isfinite(garment_laplacian)
        & (context.graph.node_area > 0.0)
    )
    aligned = np.zeros(context.graph.node_count, dtype=np.float64)
    body_signal = np.abs(body_laplacian) > context.body_signal_epsilon
    aligned[body_signal] = np.maximum(
        0.0,
        garment_laplacian[body_signal] * np.sign(body_laplacian[body_signal]),
    )
    return _Fields(
        mask=node_mask,
        mask_sha256=mask_hash,
        valid=valid,
        body_laplacian=body_laplacian,
        garment_laplacian=garment_laplacian,
        aligned_score=aligned,
    )


def measure_whole_panel_transfer(
    context: LocalMetricContext,
    candidate_vertices: np.ndarray,
    *,
    measurement_mask: np.ndarray | None = None,
) -> WholePanelTransferMetrics:
    """Measure broad body-shape transfer into a frozen free-cloth population.

    Each material node receives two scalar signals along its authored
    cloth-to-reference-body direction:

    * ``b``: the morph of the corresponding reference-body material point;
    * ``g``: the candidate cloth displacement from authored cloth.

    The reported ``alpha`` is the weighted least-squares slope of ``g`` on
    ``b`` *with an intercept*.  Before that projection, each vector field has
    its best constant world-space translation removed.  Those two removals
    make a rigid fall/translation of a panel non-evidence; a value near one is
    instead the direct signature of a distant panel receiving the body's
    spatial morph field.

    The mask and body bindings are prepared before any candidate is inspected,
    so a candidate cannot improve its score by reducing coverage.
    """

    candidate = _validate_vertices("candidate_vertices", candidate_vertices)
    if candidate.shape != context.base_vertices.shape:
        raise ValueError("candidate vertex count differs from context")
    node_mask, mask_hash = _resolve_node_mask(context, measurement_mask)
    binding = context.body_binding

    garment_vector_vertex = np.full((context.graph.vertex_count, 3), np.nan)
    body_vector_vertex = np.full((context.graph.vertex_count, 3), np.nan)
    direction_vertex = np.full((context.graph.vertex_count, 3), np.nan)
    garment_vector_vertex[binding.valid] = (
        candidate - context.base_vertices
    )[binding.valid]
    body_vector_vertex[binding.valid] = binding.body_motion[binding.valid]
    direction_vertex[binding.valid] = binding.direction[binding.valid]
    node_garment_vector = collapse_vertex_values(
        context.graph, np.nan_to_num(garment_vector_vertex)
    )
    node_body_vector = collapse_vertex_values(
        context.graph, np.nan_to_num(body_vector_vertex)
    )
    node_direction = collapse_vertex_values(
        context.graph, np.nan_to_num(direction_vertex)
    )
    direction_length = np.linalg.norm(node_direction, axis=1)
    safe_direction = np.zeros_like(node_direction)
    direction_ok = direction_length > context.body_signal_epsilon
    safe_direction[direction_ok] = (
        node_direction[direction_ok] / direction_length[direction_ok, None]
    )
    valid_binding_count = np.bincount(
        context.graph.vertex_to_node,
        weights=binding.valid.astype(np.int32),
        minlength=context.graph.node_count,
    )
    member_count = np.asarray([len(ids) for ids in context.graph.node_members])
    valid = (
        node_mask
        & (valid_binding_count == member_count)
        & np.isfinite(node_garment_vector).all(axis=1)
        & np.isfinite(node_body_vector).all(axis=1)
        & direction_ok
        & (context.graph.node_area > 0.0)
    )
    ids = np.flatnonzero(valid)
    if len(ids) < context.min_metric_nodes:
        return WholePanelTransferMetrics(
            valid=False,
            reason="insufficient_valid_nodes",
            mask_sha256=mask_hash,
            node_count=int(len(ids)),
            area=None,
            body_motion_mean=None,
            body_motion_std=None,
            garment_motion_mean=None,
            garment_motion_std=None,
            alpha=None,
            alpha_positive=None,
            correlation=None,
            intercept=None,
        )
    weights = context.graph.node_area[ids]
    total_area = float(weights.sum())
    if total_area <= 1.0e-20:
        return WholePanelTransferMetrics(
            False, "zero_valid_area", mask_hash, int(len(ids)), None,
            None, None, None, None, None, None, None, None,
        )
    normalized = weights / total_area
    garment_translation = np.sum(
        normalized[:, None] * node_garment_vector[ids], axis=0
    )
    body_translation = np.sum(
        normalized[:, None] * node_body_vector[ids], axis=0
    )
    garment = np.einsum(
        "ij,ij->i",
        node_garment_vector[ids] - garment_translation,
        safe_direction[ids],
    )
    body = np.einsum(
        "ij,ij->i",
        node_body_vector[ids] - body_translation,
        safe_direction[ids],
    )
    body_mean = float(np.sum(normalized * body))
    garment_mean = float(np.sum(normalized * garment))
    centered_body = body - body_mean
    centered_garment = garment - garment_mean
    body_variance = float(np.sum(normalized * centered_body * centered_body))
    garment_variance = float(
        np.sum(normalized * centered_garment * centered_garment)
    )
    if body_variance <= context.body_signal_epsilon * context.body_signal_epsilon:
        return WholePanelTransferMetrics(
            valid=False,
            reason="insufficient_body_shape_signal",
            mask_sha256=mask_hash,
            node_count=int(len(ids)),
            area=total_area,
            body_motion_mean=body_mean,
            body_motion_std=float(np.sqrt(max(0.0, body_variance))),
            garment_motion_mean=garment_mean,
            garment_motion_std=float(np.sqrt(max(0.0, garment_variance))),
            alpha=None,
            alpha_positive=None,
            correlation=None,
            intercept=None,
        )
    covariance = float(np.sum(normalized * centered_garment * centered_body))
    alpha = covariance / body_variance
    correlation = _weighted_correlation(
        garment,
        body,
        weights,
        epsilon=context.body_signal_epsilon,
        minimum_count=context.min_metric_nodes,
    )
    return WholePanelTransferMetrics(
        valid=True,
        reason=None,
        mask_sha256=mask_hash,
        node_count=int(len(ids)),
        area=total_area,
        body_motion_mean=body_mean,
        body_motion_std=float(np.sqrt(max(0.0, body_variance))),
        garment_motion_mean=garment_mean,
        garment_motion_std=float(np.sqrt(max(0.0, garment_variance))),
        alpha=float(alpha),
        alpha_positive=float(max(0.0, alpha)),
        correlation=correlation,
        intercept=float(garment_mean - alpha * body_mean),
    )


def evaluate_whole_panel_transfer_gate(
    raw: WholePanelTransferMetrics,
    candidate: WholePanelTransferMetrics,
    *,
    max_alpha_positive_ratio: float = 0.75,
    max_correlation: float = 0.90,
    epsilon: float = DEFAULT_BODY_SIGNAL_EPSILON,
) -> WholePanelTransferGate:
    """Gate broad free-cloth body transfer against the capture's raw conform.

    ``raw`` is always measured from the capture itself, not from a candidate
    OBJ.  This makes the comparison immune to a candidate changing the mask.
    A null candidate correlation is treated as no shape correlation, not as a
    misleading numeric zero in the public metric.
    """

    if (
        not np.isfinite(max_alpha_positive_ratio)
        or max_alpha_positive_ratio < 0.0
    ):
        raise ValueError("max_alpha_positive_ratio must be finite and >= 0")
    if not np.isfinite(max_correlation) or not -1.0 <= max_correlation <= 1.0:
        raise ValueError("max_correlation must be finite and in [-1,1]")
    if not raw.valid:
        return WholePanelTransferGate(
            False, f"raw:{raw.reason}", False, raw.alpha_positive,
            candidate.alpha_positive, None, float(max_alpha_positive_ratio),
            candidate.correlation, float(max_correlation), ("raw_metric_invalid",),
        )
    if not candidate.valid:
        return WholePanelTransferGate(
            False, f"candidate:{candidate.reason}", False, raw.alpha_positive,
            candidate.alpha_positive, None, float(max_alpha_positive_ratio),
            candidate.correlation, float(max_correlation),
            ("candidate_metric_invalid",),
        )
    if raw.alpha_positive is None or raw.alpha_positive <= epsilon:
        return WholePanelTransferGate(
            False, "insufficient_raw_positive_transfer", False,
            raw.alpha_positive, candidate.alpha_positive, None,
            float(max_alpha_positive_ratio), candidate.correlation,
            float(max_correlation), ("insufficient_raw_positive_transfer",),
        )
    if candidate.alpha_positive is None:
        return WholePanelTransferGate(
            False, "candidate_alpha_missing", False, raw.alpha_positive,
            None, None, float(max_alpha_positive_ratio),
            candidate.correlation, float(max_correlation),
            ("candidate_alpha_missing",),
        )
    ratio = float(candidate.alpha_positive / raw.alpha_positive)
    failures: list[str] = []
    if ratio > max_alpha_positive_ratio:
        failures.append("alpha_positive_ratio")
    # A constant candidate field has no meaningful correlation. It is not a
    # body-shape copy and must not be failed merely for lacking variance.
    if candidate.correlation is not None and candidate.correlation > max_correlation:
        failures.append("correlation")
    return WholePanelTransferGate(
        valid=True,
        reason=None,
        passed=not failures,
        raw_alpha_positive=raw.alpha_positive,
        candidate_alpha_positive=candidate.alpha_positive,
        alpha_positive_ratio=ratio,
        max_alpha_positive_ratio=float(max_alpha_positive_ratio),
        candidate_correlation=candidate.correlation,
        max_correlation=float(max_correlation),
        failures=tuple(failures),
    )


def freeze_raw_error_threshold(
    context: LocalMetricContext,
    *,
    measurement_mask: np.ndarray | None = None,
    mad_multiplier: float = DEFAULT_MAD_MULTIPLIER,
    minimum_value: float = 1.0e-12,
) -> ErrorThreshold:
    """Freeze a robust island threshold from ``context.raw_vertices`` only."""

    fields = _metric_fields(context, context.raw_vertices, measurement_mask)
    ids = np.flatnonzero(fields.valid)
    if len(ids) < context.min_metric_nodes:
        return ErrorThreshold(
            valid=False,
            reason="insufficient_valid_nodes",
            value=None,
            median=None,
            mad=None,
            scaled_mad=None,
            mad_multiplier=float(mad_multiplier),
            minimum_value=float(minimum_value),
            node_count=int(len(ids)),
            mask_sha256=fields.mask_sha256,
        )
    weights = context.graph.node_area[ids]
    values = fields.aligned_score[ids]
    total_area = float(weights.sum())
    if total_area <= 1.0e-20:
        return ErrorThreshold(
            False, "zero_valid_area", None, None, None, None,
            float(mad_multiplier), float(minimum_value), int(len(ids)), fields.mask_sha256,
        )
    normalized = weights / total_area
    body_values = fields.body_laplacian[ids]
    body_power = float(np.sum(normalized * body_values * body_values))
    if body_power <= context.body_signal_epsilon * context.body_signal_epsilon:
        return ErrorThreshold(
            False, "insufficient_body_signal", None, None, None, None,
            float(mad_multiplier), float(minimum_value), int(len(ids)), fields.mask_sha256,
        )
    median = _weighted_quantile(values, weights, 0.5)
    if median is None:
        return ErrorThreshold(
            False, "zero_valid_area", None, None, None, None,
            float(mad_multiplier), float(minimum_value), int(len(ids)), fields.mask_sha256,
        )
    mad = _weighted_quantile(np.abs(values - median), weights, 0.5)
    if mad is None:
        mad = 0.0
    scaled_mad = 1.4826 * mad
    threshold = max(
        float(minimum_value),
        float(median + float(mad_multiplier) * scaled_mad),
    )
    return ErrorThreshold(
        valid=True,
        reason=None,
        value=threshold,
        median=float(median),
        mad=float(mad),
        scaled_mad=float(scaled_mad),
        mad_multiplier=float(mad_multiplier),
        minimum_value=float(minimum_value),
        node_count=int(len(ids)),
        mask_sha256=fields.mask_sha256,
    )


def _invalid_islands(reason: str, threshold: float | None) -> ErrorIslandMetrics:
    return ErrorIslandMetrics(
        valid=False,
        reason=reason,
        threshold=threshold,
        island_count=0,
        error_node_count=0,
        valid_area=None,
        error_area=None,
        error_area_fraction=None,
        area_weighted_excess=None,
        excess_integral=None,
        aligned_score_p95=None,
        largest_island_area=None,
        largest_island_area_fraction=None,
        largest_island_excess_integral=None,
        largest_island_node_count=0,
    )


def _measure_error_islands(
    context: LocalMetricContext,
    fields: _Fields,
    threshold: ErrorThreshold,
) -> ErrorIslandMetrics:
    if not threshold.valid or threshold.value is None:
        return _invalid_islands(threshold.reason or "invalid_threshold", threshold.value)
    if threshold.mask_sha256 != fields.mask_sha256:
        raise ValueError("error threshold and measurement mask differ")
    valid_ids = np.flatnonzero(fields.valid)
    valid_area = float(context.graph.node_area[valid_ids].sum())
    if valid_area <= 1.0e-20:
        return _invalid_islands("zero_valid_area", threshold.value)
    error_mask = fields.valid & (fields.aligned_score > threshold.value)
    error_ids = np.flatnonzero(error_mask)
    if not len(error_ids):
        return ErrorIslandMetrics(
            valid=True,
            reason=None,
            threshold=threshold.value,
            island_count=0,
            error_node_count=0,
            valid_area=valid_area,
            error_area=0.0,
            error_area_fraction=0.0,
            area_weighted_excess=0.0,
            excess_integral=0.0,
            aligned_score_p95=0.0,
            largest_island_area=0.0,
            largest_island_area_fraction=0.0,
            largest_island_excess_integral=0.0,
            largest_island_node_count=0,
        )

    excess = np.maximum(0.0, fields.aligned_score - threshold.value)
    error_area = float(context.graph.node_area[error_ids].sum())
    excess_integral = float(
        np.sum(context.graph.node_area[error_ids] * excess[error_ids])
    )
    p95 = _weighted_quantile(
        fields.aligned_score[error_ids], context.graph.node_area[error_ids], 0.95
    )
    seen = np.zeros(context.graph.node_count, dtype=bool)
    island_count = 0
    largest_area = 0.0
    largest_integral = 0.0
    largest_count = 0
    for seed in error_ids:
        if seen[seed]:
            continue
        island_count += 1
        component: list[int] = []
        stack = [int(seed)]
        seen[seed] = True
        while stack:
            node = stack.pop()
            component.append(node)
            for neighbour in context.graph.adjacency[node]:
                ni = int(neighbour)
                if error_mask[ni] and not seen[ni]:
                    seen[ni] = True
                    stack.append(ni)
        ids = np.asarray(component, dtype=np.int32)
        area = float(context.graph.node_area[ids].sum())
        integral = float(np.sum(context.graph.node_area[ids] * excess[ids]))
        if area > largest_area or (area == largest_area and integral > largest_integral):
            largest_area = area
            largest_integral = integral
            largest_count = len(component)
    return ErrorIslandMetrics(
        valid=True,
        reason=None,
        threshold=threshold.value,
        island_count=island_count,
        error_node_count=int(len(error_ids)),
        valid_area=valid_area,
        error_area=error_area,
        error_area_fraction=error_area / valid_area,
        area_weighted_excess=excess_integral / valid_area,
        excess_integral=excess_integral,
        aligned_score_p95=p95,
        largest_island_area=largest_area,
        largest_island_area_fraction=largest_area / valid_area,
        largest_island_excess_integral=largest_integral,
        largest_island_node_count=largest_count,
    )


def measure_imprint(
    context: LocalMetricContext,
    candidate_vertices: np.ndarray,
    *,
    measurement_mask: np.ndarray | None = None,
    error_threshold: ErrorThreshold | None = None,
) -> ImprintMetrics:
    """Measure local body-imprint transfer on a frozen candidate-independent mask."""

    fields = _metric_fields(context, candidate_vertices, measurement_mask)
    ids = np.flatnonzero(fields.valid)
    if len(ids) < context.min_metric_nodes:
        return ImprintMetrics(
            valid=False,
            reason="insufficient_valid_nodes",
            mask_sha256=fields.mask_sha256,
            node_count=int(len(ids)),
            area=None,
            positive_projection_energy=None,
            correlation=None,
            alpha=None,
            alpha_positive=None,
            garment_laplacian_abs_p95=None,
            aligned_score_p95=None,
            body_laplacian_rms=None,
            garment_laplacian_rms=None,
            islands=(
                _invalid_islands("insufficient_valid_nodes", error_threshold.value)
                if error_threshold is not None
                else None
            ),
        )
    weights = context.graph.node_area[ids]
    total_area = float(weights.sum())
    if total_area <= 1.0e-20:
        return ImprintMetrics(
            False, "zero_valid_area", fields.mask_sha256, int(len(ids)), None,
            None, None, None, None, None, None, None, None,
            _invalid_islands("zero_valid_area", error_threshold.value)
            if error_threshold is not None else None,
        )
    normalized = weights / total_area
    body_lap = fields.body_laplacian[ids]
    garment_lap = fields.garment_laplacian[ids]
    body_power = float(np.sum(normalized * body_lap * body_lap))
    if body_power <= context.body_signal_epsilon * context.body_signal_epsilon:
        return ImprintMetrics(
            False, "insufficient_body_signal", fields.mask_sha256, int(len(ids)),
            total_area, None, None, None, None, None, None,
            float(np.sqrt(max(0.0, body_power))), None,
            _invalid_islands("insufficient_body_signal", error_threshold.value)
            if error_threshold is not None else None,
        )
    garment_power = float(np.sum(normalized * garment_lap * garment_lap))
    projection = float(np.sum(normalized * garment_lap * body_lap))
    alpha = projection / body_power
    positive_energy = max(0.0, projection) / np.sqrt(body_power)
    correlation = _weighted_correlation(
        garment_lap,
        body_lap,
        weights,
        epsilon=context.body_signal_epsilon,
        minimum_count=context.min_metric_nodes,
    )
    garment_p95 = _weighted_quantile(np.abs(garment_lap), weights, 0.95)
    aligned_p95 = _weighted_quantile(fields.aligned_score[ids], weights, 0.95)
    islands = (
        _measure_error_islands(context, fields, error_threshold)
        if error_threshold is not None
        else None
    )
    return ImprintMetrics(
        valid=True,
        reason=None,
        mask_sha256=fields.mask_sha256,
        node_count=int(len(ids)),
        area=total_area,
        positive_projection_energy=float(positive_energy),
        correlation=correlation,
        alpha=float(alpha),
        alpha_positive=float(max(0.0, alpha)),
        garment_laplacian_abs_p95=garment_p95,
        aligned_score_p95=aligned_p95,
        body_laplacian_rms=float(np.sqrt(body_power)),
        garment_laplacian_rms=float(np.sqrt(max(0.0, garment_power))),
        islands=islands,
    )


def _difference(first: float | None, second: float | None) -> float | None:
    return None if first is None or second is None else float(second - first)


def compare_causal_delta(
    context: LocalMetricContext,
    entry_vertices: np.ndarray,
    candidate_vertices: np.ndarray,
    *,
    measurement_mask: np.ndarray | None = None,
    error_threshold: ErrorThreshold | None = None,
) -> CausalDeltaMetrics:
    """Compare two fields causally on one supplied/frozen mask.

    If no threshold is supplied it is frozen from ``context.raw_vertices``;
    neither ``entry_vertices`` nor ``candidate_vertices`` can affect it.
    """

    threshold = error_threshold or freeze_raw_error_threshold(
        context, measurement_mask=measurement_mask
    )
    entry = measure_imprint(
        context,
        entry_vertices,
        measurement_mask=measurement_mask,
        error_threshold=threshold,
    )
    candidate = measure_imprint(
        context,
        candidate_vertices,
        measurement_mask=measurement_mask,
        error_threshold=threshold,
    )
    reason = None
    if not threshold.valid:
        reason = threshold.reason or "invalid_threshold"
    elif not entry.valid:
        reason = f"entry:{entry.reason}"
    elif not candidate.valid:
        reason = f"candidate:{candidate.reason}"
    elif entry.mask_sha256 != candidate.mask_sha256:
        reason = "mask_mismatch"
    valid = reason is None
    energy_delta = _difference(
        entry.positive_projection_energy, candidate.positive_projection_energy
    )
    reduction = None
    if (
        entry.positive_projection_energy is not None
        and candidate.positive_projection_energy is not None
        and entry.positive_projection_energy > context.body_signal_epsilon
    ):
        reduction = float(
            (entry.positive_projection_energy - candidate.positive_projection_energy)
            / entry.positive_projection_energy
        )
    entry_islands = entry.islands
    candidate_islands = candidate.islands
    return CausalDeltaMetrics(
        valid=valid,
        reason=reason,
        mask_sha256=entry.mask_sha256,
        threshold=threshold,
        entry=entry,
        candidate=candidate,
        positive_projection_energy_delta=energy_delta,
        positive_projection_energy_reduction_fraction=reduction,
        correlation_delta=_difference(entry.correlation, candidate.correlation),
        alpha_delta=_difference(entry.alpha, candidate.alpha),
        garment_laplacian_abs_p95_delta=_difference(
            entry.garment_laplacian_abs_p95,
            candidate.garment_laplacian_abs_p95,
        ),
        aligned_score_p95_delta=_difference(
            entry.aligned_score_p95, candidate.aligned_score_p95
        ),
        error_area_delta=_difference(
            entry_islands.error_area if entry_islands else None,
            candidate_islands.error_area if candidate_islands else None,
        ),
        area_weighted_excess_delta=_difference(
            entry_islands.area_weighted_excess if entry_islands else None,
            candidate_islands.area_weighted_excess if candidate_islands else None,
        ),
        excess_integral_delta=_difference(
            entry_islands.excess_integral if entry_islands else None,
            candidate_islands.excess_integral if candidate_islands else None,
        ),
        largest_island_area_delta=_difference(
            entry_islands.largest_island_area if entry_islands else None,
            candidate_islands.largest_island_area if candidate_islands else None,
        ),
    )


def _grid_fixture(size: int = 9) -> dict[str, np.ndarray]:
    xs = np.linspace(-4.0, 4.0, size)
    ys = np.linspace(-4.0, 4.0, size)
    xy = np.asarray([(x, y) for y in ys for x in xs], dtype=np.float64)
    ref_body = np.column_stack((xy, np.zeros(len(xy))))
    base = np.column_stack((xy, np.full(len(xy), 4.0)))
    tris: list[tuple[int, int, int]] = []
    for y in range(size - 1):
        for x in range(size - 1):
            a = y * size + x
            b = a + 1
            c = a + size
            d = c + 1
            tris.extend(((a, b, d), (a, d, c)))
    faces = np.asarray(tris, dtype=np.int32)
    radius2 = xy[:, 0] ** 2 + xy[:, 1] ** 2
    bump = 0.75 * np.exp(-radius2 / (2.0 * 1.15**2))
    body = ref_body.copy()
    body[:, 2] += bump
    raw = base.copy()
    raw[:, 2] += bump
    return {
        "base": base,
        "raw": raw,
        "cloth_tris": faces,
        "drape": np.ones(len(base)),
        "ref_body": ref_body,
        "body": body,
        "body_tris": faces.copy(),
    }


def _assert_close(label: str, actual: float | None, expected: float, tol: float) -> None:
    if actual is None or abs(actual - expected) > tol:
        raise AssertionError(f"{label}: expected {expected} +/- {tol}, got {actual}")


def run_selftests() -> dict:
    """Run deterministic synthetic tests without writing artifacts."""

    fixture = _grid_fixture(9)
    context = prepare_context(
        base_vertices=fixture["base"],
        raw_vertices=fixture["raw"],
        cloth_tris=fixture["cloth_tris"],
        drape=fixture["drape"],
        ref_body_vertices=fixture["ref_body"],
        body_vertices=fixture["body"],
        body_tris=fixture["body_tris"],
        min_component_nodes=4,
        min_metric_nodes=4,
    )
    threshold = freeze_raw_error_threshold(context)
    if not threshold.valid:
        raise AssertionError(f"raw threshold invalid: {threshold.reason}")

    # g=b: the cloth copies exactly the corresponding body morph.
    copied = measure_imprint(context, fixture["raw"], error_threshold=threshold)
    if not copied.valid:
        raise AssertionError(f"g=b metric invalid: {copied.reason}")
    _assert_close("g=b alpha", copied.alpha, 1.0, 1.0e-10)
    _assert_close("g=b corr", copied.correlation, 1.0, 1.0e-10)
    if copied.positive_projection_energy is None or copied.positive_projection_energy <= 0.0:
        raise AssertionError("g=b must have positive projection energy")
    if copied.islands is None or copied.islands.island_count < 1:
        raise AssertionError("synthetic bump must form at least one error island")

    # The whole-panel metric intentionally retains the low-frequency body-copy
    # signal that the Laplacian metric discards.  A direct copy must therefore
    # be exactly one in both regression slope and correlation.
    copied_whole = measure_whole_panel_transfer(context, fixture["raw"])
    if not copied_whole.valid:
        raise AssertionError(f"whole g=b metric invalid: {copied_whole.reason}")
    _assert_close("whole g=b alpha", copied_whole.alpha, 1.0, 1.0e-10)
    _assert_close("whole g=b corr", copied_whole.correlation, 1.0, 1.0e-10)
    raw_copy_gate = evaluate_whole_panel_transfer_gate(copied_whole, copied_whole)
    if raw_copy_gate.passed or "alpha_positive_ratio" not in raw_copy_gate.failures:
        raise AssertionError("a raw whole-panel copy must fail the transfer gate")

    # A constant offset has zero degree-normalized Laplacian. Correlation must
    # be N/A rather than the misleading numeric zero used by the old audit.
    constant = fixture["base"].copy()
    constant[:, 2] += 0.2
    constant_metric = measure_imprint(context, constant, error_threshold=threshold)
    if not constant_metric.valid:
        raise AssertionError(f"constant metric invalid: {constant_metric.reason}")
    _assert_close(
        "constant positive energy",
        constant_metric.positive_projection_energy,
        0.0,
        1.0e-12,
    )
    if constant_metric.correlation is not None:
        raise AssertionError("constant-field correlation must be N/A")

    # Whole-panel alpha has an intercept.  A rigid cloth translation is not a
    # body-shape imprint even though the body bump has a non-zero mean.
    constant_whole = measure_whole_panel_transfer(context, constant)
    if not constant_whole.valid:
        raise AssertionError(
            f"whole constant metric invalid: {constant_whole.reason}"
        )
    _assert_close("whole constant alpha", constant_whole.alpha, 0.0, 1.0e-10)
    if constant_whole.correlation is not None:
        raise AssertionError("whole constant correlation must be N/A")
    clear_whole = measure_whole_panel_transfer(context, fixture["base"])
    clear_gate = evaluate_whole_panel_transfer_gate(copied_whole, clear_whole)
    if not clear_gate.valid or not clear_gate.passed:
        raise AssertionError(f"body-independent cloth failed gate: {clear_gate}")

    # With no body morph there is no causal body-imprint signal to measure.
    # Report N/A explicitly; do not turn absent variance into a numeric zero.
    no_morph = prepare_context(
        base_vertices=fixture["base"],
        raw_vertices=fixture["base"],
        cloth_tris=fixture["cloth_tris"],
        drape=fixture["drape"],
        ref_body_vertices=fixture["ref_body"],
        body_vertices=fixture["ref_body"],
        body_tris=fixture["body_tris"],
        min_component_nodes=4,
        min_metric_nodes=4,
    )
    no_morph_threshold = freeze_raw_error_threshold(no_morph)
    no_morph_metric = measure_imprint(no_morph, fixture["base"])
    no_morph_whole = measure_whole_panel_transfer(no_morph, fixture["base"])
    if no_morph_threshold.valid or no_morph_threshold.reason != "insufficient_body_signal":
        raise AssertionError("zero body signal must invalidate the raw threshold")
    if no_morph_metric.valid or no_morph_metric.reason != "insufficient_body_signal":
        raise AssertionError("zero body signal must produce an N/A metric")
    if (
        no_morph_whole.valid
        or no_morph_whole.reason != "insufficient_body_shape_signal"
    ):
        raise AssertionError("zero body shape signal must produce whole-panel N/A")

    # Removing the copied bump must reduce both energy and frozen-threshold
    # area; entry/candidate coverage is necessarily identical.
    delta = compare_causal_delta(
        context, fixture["raw"], fixture["base"], error_threshold=threshold
    )
    if not delta.valid:
        raise AssertionError(f"causal delta invalid: {delta.reason}")
    if (
        delta.positive_projection_energy_reduction_fraction is None
        or delta.positive_projection_energy_reduction_fraction < 0.999999
    ):
        raise AssertionError("bump removal did not reduce projection energy")
    if delta.error_area_delta is None or delta.error_area_delta >= 0.0:
        raise AssertionError("bump removal did not reduce error-island area")

    # Translating cloth and both body states together cannot change local
    # directions, bindings, mask, threshold, or scores.
    translation = np.asarray([17.0, -11.0, 6.5])
    translated = prepare_context(
        base_vertices=fixture["base"] + translation,
        raw_vertices=fixture["raw"] + translation,
        cloth_tris=fixture["cloth_tris"],
        drape=fixture["drape"],
        ref_body_vertices=fixture["ref_body"] + translation,
        body_vertices=fixture["body"] + translation,
        body_tris=fixture["body_tris"],
        min_component_nodes=4,
        min_metric_nodes=4,
    )
    translated_threshold = freeze_raw_error_threshold(translated)
    translated_metric = measure_imprint(
        translated, fixture["raw"] + translation,
        error_threshold=translated_threshold,
    )
    translated_whole = measure_whole_panel_transfer(
        translated, fixture["raw"] + translation
    )
    if context.frozen_mask.interior_node_count != translated.frozen_mask.interior_node_count:
        raise AssertionError("translation changed frozen-mask coverage")
    _assert_close(
        "translation energy",
        translated_metric.positive_projection_energy,
        float(copied.positive_projection_energy),
        1.0e-10,
    )
    _assert_close(
        "translation threshold",
        translated_threshold.value,
        float(threshold.value),
        1.0e-10,
    )
    _assert_close(
        "whole translation alpha",
        translated_whole.alpha,
        float(copied_whole.alpha),
        1.0e-10,
    )
    _assert_close(
        "whole translation corr",
        translated_whole.correlation,
        float(copied_whole.correlation),
        1.0e-10,
    )

    # Two UV/material halves with duplicate seam vertices must become one
    # connected six-node graph, with both seam pairs sharing node IDs.
    seam_verts = np.asarray(
        [
            [0, 0, 0], [1, 0, 0], [0, 1, 0], [1, 1, 0],
            [1, 0, 0], [1, 1, 0], [2, 0, 0], [2, 1, 0],
        ],
        dtype=np.float64,
    )
    seam_tris = np.asarray(
        [[0, 1, 3], [0, 3, 2], [4, 6, 7], [4, 7, 5]], dtype=np.int32
    )
    seam_graph = build_material_graph(seam_verts, seam_tris)
    if seam_graph.node_count != 6:
        raise AssertionError(f"expected 6 welded seam nodes, got {seam_graph.node_count}")
    if seam_graph.vertex_to_node[1] != seam_graph.vertex_to_node[4]:
        raise AssertionError("lower seam twins were not welded")
    if seam_graph.vertex_to_node[3] != seam_graph.vertex_to_node[5]:
        raise AssertionError("upper seam twins were not welded")
    reached = {0}
    stack = [0]
    while stack:
        node = stack.pop()
        for neighbour in seam_graph.adjacency[node]:
            ni = int(neighbour)
            if ni not in reached:
                reached.add(ni)
                stack.append(ni)
    if len(reached) != seam_graph.node_count:
        raise AssertionError("welded seam graph is disconnected")

    # Subdivision check: amplitude is topology-scale dependent for a one-ring
    # Laplacian, so only dimensionless alpha/correlation are expected invariant.
    fine_fixture = _grid_fixture(17)
    fine_context = prepare_context(
        base_vertices=fine_fixture["base"],
        raw_vertices=fine_fixture["raw"],
        cloth_tris=fine_fixture["cloth_tris"],
        drape=fine_fixture["drape"],
        ref_body_vertices=fine_fixture["ref_body"],
        body_vertices=fine_fixture["body"],
        body_tris=fine_fixture["body_tris"],
        min_component_nodes=4,
        min_metric_nodes=4,
    )
    fine_metric = measure_imprint(fine_context, fine_fixture["raw"])
    _assert_close("subdivision alpha", fine_metric.alpha, 1.0, 1.0e-10)
    _assert_close("subdivision corr", fine_metric.correlation, 1.0, 1.0e-10)
    fine_whole = measure_whole_panel_transfer(fine_context, fine_fixture["raw"])
    _assert_close("whole subdivision alpha", fine_whole.alpha, 1.0, 1.0e-10)
    _assert_close("whole subdivision corr", fine_whole.correlation, 1.0, 1.0e-10)

    return {
        "status": "PASS",
        "tests": [
            "translation_invariance",
            "body_copy_g_equals_b",
            "whole_panel_body_copy_g_equals_b",
            "whole_panel_raw_copy_gate_rejects",
            "constant_field_corr_is_null",
            "whole_panel_constant_translation_is_not_transfer",
            "whole_panel_body_independent_cloth_passes",
            "zero_body_signal_is_na",
            "whole_panel_zero_body_shape_signal_is_na",
            "local_bump_error_island",
            "causal_shared_mask_delta",
            "exact_seam_weld",
            "subdivision_dimensionless_alpha_corr",
            "whole_panel_subdivision_dimensionless_alpha_corr",
        ],
        "coarse": {
            "vertices": context.graph.vertex_count,
            "nodes": context.graph.node_count,
            "frozenInterior": context.frozen_mask.interior_node_count,
            "alpha": copied.alpha,
            "correlation": copied.correlation,
            "positiveEnergy": copied.positive_projection_energy,
            "errorIslands": copied.islands.island_count if copied.islands else None,
            "wholePanelAlpha": copied_whole.alpha,
            "wholePanelCorrelation": copied_whole.correlation,
        },
    }


def _parse_named_candidate(value: str) -> tuple[str, Path]:
    """Parse either ``path.obj`` or ``label=path.obj`` deterministically."""

    raw = value.strip()
    if not raw:
        raise argparse.ArgumentTypeError("candidate path must not be empty")
    if "=" in raw:
        label, raw_path = (part.strip() for part in raw.split("=", 1))
        if not label or not raw_path:
            raise argparse.ArgumentTypeError(
                "candidate must be OBJ_PATH or LABEL=OBJ_PATH"
            )
        return label, Path(raw_path)
    path = Path(raw)
    label = path.stem
    if not label:
        raise argparse.ArgumentTypeError("candidate path has no usable label")
    return label, path


def _validate_cli_float(
    parser: argparse.ArgumentParser,
    flag: str,
    value: float,
    *,
    lower: float | None = None,
    upper: float | None = None,
) -> None:
    if not np.isfinite(value):
        parser.error(f"{flag} must be finite")
    if lower is not None and value < lower:
        parser.error(f"{flag} must be >= {lower}")
    if upper is not None and value > upper:
        parser.error(f"{flag} must be <= {upper}")


def _candidate_mesh_or_error(path: Path, capture: ss.CaptureFront) -> ss.ObjMesh:
    if not path.is_file():
        raise SystemExit(f"candidate OBJ does not exist: {path}")
    mesh = ss.load_obj(path)
    if len(mesh.verts) != len(capture.base_verts):
        raise SystemExit(
            f"candidate OBJ vertex count differs from capture: "
            f"{len(mesh.verts)} != {len(capture.base_verts)} ({path})"
        )
    if not np.array_equal(mesh.tris, capture.tris):
        raise SystemExit(f"candidate OBJ topology differs from capture: {path}")
    if not np.isfinite(mesh.verts).all():
        raise SystemExit(f"candidate OBJ contains non-finite coordinates: {path}")
    return mesh


def _free_transfer_report(
    capture_path: Path,
    candidates: Sequence[tuple[str, Path]],
    *,
    gap: float,
    drape: float,
    min_component_nodes: int,
    min_metric_nodes: int,
    max_alpha_positive_ratio: float,
    max_correlation: float,
) -> tuple[dict, bool]:
    """Build one deterministic, candidate-independent transfer-gate report."""

    capture = ss.read_capture_front(capture_path)
    context = prepare_context_from_capture(
        capture,
        gap_threshold=gap,
        drape_threshold=drape,
        min_component_nodes=min_component_nodes,
        min_metric_nodes=min_metric_nodes,
    )
    raw_vertices = context.raw_vertices
    local_threshold = freeze_raw_error_threshold(context)
    raw_whole = measure_whole_panel_transfer(context, raw_vertices)
    raw_local = measure_imprint(
        context, raw_vertices, error_threshold=local_threshold
    )
    report: dict[str, object] = {
        "capture": str(capture_path.resolve()),
        "capture_sha256": hashlib.sha256(capture_path.read_bytes()).hexdigest(),
        "frozen_mask": {
            "vertex_count": context.frozen_mask.vertex_count,
            "node_count": context.frozen_mask.node_count,
            "interior_node_count": context.frozen_mask.interior_node_count,
            "mask_sha256": context.frozen_mask.mask_sha256,
            "gap_threshold": context.frozen_mask.gap_threshold,
            "drape_threshold": context.frozen_mask.drape_threshold,
            "min_component_nodes": context.frozen_mask.min_component_nodes,
        },
        "gate_thresholds": {
            "max_alpha_positive_ratio": max_alpha_positive_ratio,
            "max_correlation": max_correlation,
        },
        "raw": {
            "whole_panel": raw_whole.as_dict(),
            "local_laplacian": raw_local.as_dict(),
        },
        "candidates": {},
    }
    all_passed = True
    candidate_rows: dict[str, object] = {}
    for label, candidate_path in candidates:
        resolved = candidate_path.resolve()
        mesh = _candidate_mesh_or_error(resolved, capture)
        whole = measure_whole_panel_transfer(context, mesh.verts)
        local = measure_imprint(context, mesh.verts, error_threshold=local_threshold)
        verdict = evaluate_whole_panel_transfer_gate(
            raw_whole,
            whole,
            max_alpha_positive_ratio=max_alpha_positive_ratio,
            max_correlation=max_correlation,
        )
        candidate_rows[label] = {
            "path": str(resolved),
            "sha256": hashlib.sha256(resolved.read_bytes()).hexdigest(),
            "whole_panel": whole.as_dict(),
            "local_laplacian": local.as_dict(),
            "gate": verdict.as_dict(),
        }
        all_passed = all_passed and verdict.valid and verdict.passed
    report["candidates"] = candidate_rows
    return report, all_passed


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--selftest", action="store_true", help="run deterministic synthetic tests"
    )
    parser.add_argument(
        "--capture", type=Path,
        help="Drape capture used to freeze free-cloth coverage and body bindings",
    )
    parser.add_argument(
        "--candidate", action="append", type=_parse_named_candidate, default=[],
        metavar="[LABEL=]OBJ",
        help="candidate OBJ to gate; repeat for multiple candidates",
    )
    parser.add_argument("--gap", type=float, default=DEFAULT_GAP_THRESHOLD)
    parser.add_argument("--drape", type=float, default=DEFAULT_DRAPE_THRESHOLD)
    parser.add_argument(
        "--min-component-nodes", type=int, default=DEFAULT_MIN_COMPONENT_NODES
    )
    parser.add_argument(
        "--min-metric-nodes", type=int, default=DEFAULT_MIN_METRIC_NODES
    )
    parser.add_argument(
        "--max-alpha-positive-ratio", type=float, default=0.75,
        help=("candidate/free raw whole-panel transfer slope ratio ceiling "
              "(default: 0.75)"),
    )
    parser.add_argument(
        "--max-correlation", type=float, default=0.90,
        help="candidate whole-panel body-shape correlation ceiling (default: 0.90)",
    )
    parser.add_argument(
        "--report-only", action="store_true",
        help="write/report metrics but return success even when a gate fails",
    )
    parser.add_argument("--json", type=Path, dest="json_path")
    args = parser.parse_args(argv)

    _validate_cli_float(parser, "--gap", args.gap, lower=0.0)
    _validate_cli_float(parser, "--drape", args.drape, lower=0.0, upper=1.0)
    _validate_cli_float(
        parser, "--max-alpha-positive-ratio",
        args.max_alpha_positive_ratio, lower=0.0,
    )
    _validate_cli_float(
        parser, "--max-correlation", args.max_correlation, lower=-1.0, upper=1.0
    )
    if args.min_component_nodes < 1:
        parser.error("--min-component-nodes must be >= 1")
    if args.min_metric_nodes < 1:
        parser.error("--min-metric-nodes must be >= 1")

    selftest_report = run_selftests() if args.selftest else None
    if args.capture is None:
        if args.candidate:
            parser.error("--candidate requires --capture")
        if selftest_report is None:
            parser.error("use --selftest or provide --capture with --candidate")
        print(json.dumps(selftest_report, indent=2, sort_keys=True, allow_nan=False))
        return 0
    if not args.candidate:
        parser.error("--capture requires at least one --candidate")
    capture_path = args.capture.resolve()
    if not capture_path.is_file():
        parser.error(f"--capture does not exist: {capture_path}")
    labels = [label for label, _path in args.candidate]
    duplicates = sorted({label for label in labels if labels.count(label) > 1})
    if duplicates:
        parser.error("duplicate candidate label(s): " + ", ".join(duplicates))

    report, all_passed = _free_transfer_report(
        capture_path,
        args.candidate,
        gap=args.gap,
        drape=args.drape,
        min_component_nodes=args.min_component_nodes,
        min_metric_nodes=args.min_metric_nodes,
        max_alpha_positive_ratio=args.max_alpha_positive_ratio,
        max_correlation=args.max_correlation,
    )
    if selftest_report is not None:
        report["selftest"] = selftest_report
    rendered = json.dumps(report, indent=2, sort_keys=True, allow_nan=False)
    print(rendered)
    if args.json_path is not None:
        args.json_path.parent.mkdir(parents=True, exist_ok=True)
        args.json_path.write_text(rendered + "\n", encoding="utf-8")
    return 0 if args.report_only or all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
