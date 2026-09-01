#!/usr/bin/env python3
"""Measure *externally visible* garment self-intersection curves.

Raw triangle-pair counts cannot distinguish a harmless crossing hidden inside a
layered robe from a crossing exposed through a slit.  This tool first extracts
the non-coplanar triangle/triangle intersection *segments*, then samples those
segments from deterministic directions on the sphere.  A sample is visible
when a two-sided ray from it reaches infinity without hitting another garment
triangle.  An optional body mesh is evaluated as a second occluder channel;
the garment-only channel is always reported.

The metric intentionally does not use outfit or mesh names, winding, a closed
volume, or a radial body model.  It therefore works on open, layered and
non-manifold garments.

Examples:

  python tools/pathb_visible_selfint.py --garment final.obj
  python tools/pathb_visible_selfint.py --garment candidate.obj \
      --baseline current.obj --body body.obj --json report.json \
      --curve-obj visible_curves.obj
  python tools/pathb_visible_selfint.py --selftest

The OBJ diagnostic contains the intersection segments visible from the worst
sampled direction plus an arrow pointing toward that viewer.  Import it beside
the garment to inspect exactly what owns the score.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

import numpy as np


METRIC_VERSION = 2
REFERENCE_DIVISOR = 100.0


@dataclass(frozen=True)
class Mesh:
    verts: np.ndarray
    tris: np.ndarray
    name: str = "mesh"


@dataclass(frozen=True)
class CrossingSegment:
    tri_a: int
    tri_b: int
    p0: np.ndarray
    p1: np.ndarray
    owner_triangles: tuple[int, ...] = ()
    source_pairs: tuple[tuple[int, int], ...] = ()

    @property
    def length(self) -> float:
        return float(np.linalg.norm(self.p1 - self.p0))

    @property
    def skipped_triangles(self) -> frozenset[int]:
        if self.owner_triangles:
            return frozenset(self.owner_triangles)
        return frozenset((self.tri_a, self.tri_b))

    @property
    def triangle_pairs(self) -> tuple[tuple[int, int], ...]:
        if self.source_pairs:
            return self.source_pairs
        return ((min(self.tri_a, self.tri_b), max(self.tri_a, self.tri_b)),)


@dataclass
class BVHNode:
    bmin: np.ndarray
    bmax: np.ndarray
    left: int = -1
    right: int = -1
    tri_ids: np.ndarray | None = None


@dataclass
class VisibilityResult:
    summary: dict
    projected_by_view: np.ndarray
    unprojected_by_view: np.ndarray
    segment_visible_by_view: np.ndarray
    sample_visible_by_view: list[np.ndarray]


def read_obj(path: Path) -> Mesh:
    verts: list[tuple[float, float, float]] = []
    tris: list[tuple[int, int, int]] = []
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        for line_number, line in enumerate(stream, 1):
            if line.startswith("v "):
                fields = line.split()
                if len(fields) < 4:
                    raise ValueError(f"{path}:{line_number}: malformed vertex")
                xyz = tuple(float(value) for value in fields[1:4])
                if not all(math.isfinite(value) for value in xyz):
                    raise ValueError(f"{path}:{line_number}: non-finite vertex")
                verts.append(xyz)
            elif line.startswith("f "):
                fields = line.split()[1:]
                if len(fields) < 3:
                    raise ValueError(f"{path}:{line_number}: malformed face")
                polygon: list[int] = []
                for field in fields:
                    raw = int(field.split("/", 1)[0])
                    index = raw - 1 if raw > 0 else len(verts) + raw
                    if index < 0 or index >= len(verts):
                        raise ValueError(
                            f"{path}:{line_number}: vertex index {raw} out of range"
                        )
                    polygon.append(index)
                for k in range(1, len(polygon) - 1):
                    tris.append((polygon[0], polygon[k], polygon[k + 1]))
    if not verts or not tris:
        raise ValueError(f"{path}: empty vertex or triangle list")
    return Mesh(
        np.asarray(verts, dtype=np.float64),
        np.asarray(tris, dtype=np.int64),
        str(path.resolve()),
    )


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def unique_edges(tris: np.ndarray) -> list[tuple[int, int]]:
    edges: set[tuple[int, int]] = set()
    for tri in tris:
        for k in range(3):
            a, b = int(tri[k]), int(tri[(k + 1) % 3])
            edges.add((min(a, b), max(a, b)))
    return sorted(edges)


def mean_edge_length(mesh: Mesh) -> float:
    edges = unique_edges(mesh.tris)
    if not edges:
        raise ValueError(f"{mesh.name}: topology has no edges")
    lengths = [np.linalg.norm(mesh.verts[a] - mesh.verts[b]) for a, b in edges]
    value = float(np.mean(lengths))
    if not math.isfinite(value) or value <= np.finfo(np.float64).tiny:
        raise ValueError(f"{mesh.name}: invalid mean edge length {value}")
    return value


def bounding_box_diagonal(mesh: Mesh) -> float:
    """Return a tessellation-independent physical scale for ``mesh``."""
    extent = mesh.verts.max(axis=0) - mesh.verts.min(axis=0)
    value = float(np.linalg.norm(extent))
    if not math.isfinite(value) or value <= np.finfo(np.float64).tiny:
        raise ValueError(f"{mesh.name}: invalid bounding-box diagonal {value}")
    return value


def reference_length(mesh: Mesh) -> float:
    """Metric unit: one percent of the mesh bounding-box diagonal.

    Dividing by a fixed constant keeps v1-like human-readable magnitudes while
    removing the old dependency on triangle density.  When a baseline exists,
    the caller deliberately supplies its unit to both meshes.
    """
    return bounding_box_diagonal(mesh) / REFERENCE_DIVISOR


def fibonacci_directions(count: int) -> np.ndarray:
    """Deterministic, approximately equal-solid-angle unit directions."""
    if count < 12:
        raise ValueError("at least 12 view directions are required")
    golden_angle = math.pi * (3.0 - math.sqrt(5.0))
    result = np.empty((count, 3), dtype=np.float64)
    for i in range(count):
        z = 1.0 - 2.0 * (i + 0.5) / count
        radius = math.sqrt(max(0.0, 1.0 - z * z))
        azimuth = golden_angle * i
        result[i] = (radius * math.cos(azimuth), radius * math.sin(azimuth), z)
    return result


def topology_neighbourhoods(
    mesh: Mesh,
    rings: int,
    representatives: np.ndarray,
) -> list[set[int]]:
    """Return ring neighbourhoods after welding exact split-seam twins.

    A geometric seam may use duplicate vertex indices on its two sides.  Ring
    exclusion must cross that seam just as it crosses a shared-index edge;
    otherwise the first faces on either side are falsely labelled as two cloth
    layers.  The returned sets contain canonical representative IDs.
    """
    one_ring = [set() for _ in range(len(mesh.verts))]
    for tri in mesh.tris:
        canonical = [int(representatives[int(vertex)]) for vertex in tri]
        for a in canonical:
            for b in canonical:
                if a != b:
                    one_ring[a].add(b)
    neighbourhood_by_rep: dict[int, set[int]] = {}
    for representative in sorted(set(int(value) for value in representatives)):
        reached = {representative}
        frontier = {representative}
        for _ in range(max(0, rings)):
            next_frontier: set[int] = set()
            for item in frontier:
                next_frontier.update(one_ring[item])
            next_frontier.difference_update(reached)
            reached.update(next_frontier)
            frontier = next_frontier
            if not frontier:
                break
        neighbourhood_by_rep[representative] = reached
    return [neighbourhood_by_rep[int(value)] for value in representatives]


def boundary_edge_seam_representatives(
    mesh: Mesh, epsilon: float
) -> np.ndarray:
    """Weld only endpoints of *coincident boundary-edge pairs*.

    OBJ split seams duplicate an entire material edge.  Coincident points by
    themselves are not evidence of a seam: unrelated panels routinely meet at
    corners.  Requiring a paired boundary edge makes the exclusion topological
    rather than a point-proximity heuristic.
    """
    epsilon = max(float(epsilon), np.finfo(np.float64).tiny)
    incidence: dict[tuple[int, int], int] = defaultdict(int)
    for tri in mesh.tris:
        for k in range(3):
            a, b = int(tri[k]), int(tri[(k + 1) % 3])
            incidence[(min(a, b), max(a, b))] += 1
    boundary_edges = sorted(edge for edge, count in incidence.items() if count == 1)

    quantized = np.rint(mesh.verts / epsilon).astype(np.int64)

    def point_key(index: int) -> tuple[int, int, int]:
        value = quantized[index]
        return int(value[0]), int(value[1]), int(value[2])

    buckets: dict[
        tuple[tuple[int, int, int], tuple[int, int, int]],
        list[tuple[int, int]],
    ] = defaultdict(list)
    for a, b in boundary_edges:
        key_a, key_b = point_key(a), point_key(b)
        buckets[tuple(sorted((key_a, key_b)))].append((a, b))

    sets = DisjointSet(len(mesh.verts))
    for key in sorted(buckets):
        edges = buckets[key]
        if len(edges) < 2:
            continue
        for first_index in range(len(edges)):
            a, b = edges[first_index]
            for second_index in range(first_index + 1, len(edges)):
                c, d = edges[second_index]
                direct = (
                    np.linalg.norm(mesh.verts[a] - mesh.verts[c]) <= epsilon
                    and np.linalg.norm(mesh.verts[b] - mesh.verts[d]) <= epsilon
                )
                reverse = (
                    np.linalg.norm(mesh.verts[a] - mesh.verts[d]) <= epsilon
                    and np.linalg.norm(mesh.verts[b] - mesh.verts[c]) <= epsilon
                )
                if direct:
                    sets.union(a, c)
                    sets.union(b, d)
                elif reverse:
                    sets.union(a, d)
                    sets.union(b, c)
    return np.asarray([sets.find(index) for index in range(len(mesh.verts))])


def _segment_triangle_point(
    p0: np.ndarray,
    p1: np.ndarray,
    a: np.ndarray,
    b: np.ndarray,
    c: np.ndarray,
    determinant_epsilon: float,
) -> np.ndarray | None:
    """Moller-Trumbore in segment form, returning the contact point."""
    direction = p1 - p0
    edge1, edge2 = b - a, c - a
    h = np.cross(direction, edge2)
    determinant = float(np.dot(edge1, h))
    if not math.isfinite(determinant) or abs(determinant) <= determinant_epsilon:
        return None
    inverse = 1.0 / determinant
    s = p0 - a
    u = inverse * float(np.dot(s, h))
    bary_epsilon = 1.0e-7
    if u < -bary_epsilon or u > 1.0 + bary_epsilon:
        return None
    q = np.cross(s, edge1)
    v = inverse * float(np.dot(direction, q))
    if v < -bary_epsilon or u + v > 1.0 + bary_epsilon:
        return None
    t = inverse * float(np.dot(edge2, q))
    if t < -bary_epsilon or t > 1.0 + bary_epsilon:
        return None
    return p0 + np.clip(t, 0.0, 1.0) * direction


def triangle_intersection_segment(
    verts: np.ndarray,
    tris: np.ndarray,
    tri_a: int,
    tri_b: int,
    scale: float,
) -> CrossingSegment | None:
    """Return a non-coplanar intersection curve, excluding point contact."""
    first = verts[tris[tri_a]]
    second = verts[tris[tri_b]]
    normal_a = np.cross(first[1] - first[0], first[2] - first[0])
    normal_b = np.cross(second[1] - second[0], second[2] - second[0])
    length_a = float(np.linalg.norm(normal_a))
    length_b = float(np.linalg.norm(normal_b))
    area_epsilon = scale**2 * 1.0e-14
    if length_a <= area_epsilon or length_b <= area_epsilon:
        return None
    normal_a /= length_a
    normal_b /= length_b
    if float(np.linalg.norm(np.cross(normal_a, normal_b))) <= 1.0e-8:
        return None

    # Closed-triangle overlap is required, but strict per-triangle straddling is
    # not: a real cloth crossing can lie exactly on a triangulation edge.  Point
    # touches still disappear below because they cannot produce a segment.
    plane_epsilon = scale * 1.0e-7
    distance_a = (first - second[0]) @ normal_b
    distance_b = (second - first[0]) @ normal_a
    if (
        float(distance_a.min()) > plane_epsilon
        or float(distance_a.max()) < -plane_epsilon
        or float(distance_b.min()) > plane_epsilon
        or float(distance_b.max()) < -plane_epsilon
    ):
        return None

    determinant_epsilon = scale**3 * 1.0e-12
    point_epsilon = scale * 1.0e-6
    points: list[np.ndarray] = []
    for source, target in ((first, second), (second, first)):
        for k in range(3):
            point = _segment_triangle_point(
                source[k],
                source[(k + 1) % 3],
                target[0],
                target[1],
                target[2],
                determinant_epsilon,
            )
            if point is not None and all(
                np.linalg.norm(point - previous) > point_epsilon
                for previous in points
            ):
                points.append(point)
    if len(points) < 2:
        return None

    best_length = 0.0
    best_pair: tuple[np.ndarray, np.ndarray] | None = None
    for i in range(len(points)):
        for j in range(i + 1, len(points)):
            length = float(np.linalg.norm(points[i] - points[j]))
            if length > best_length:
                best_length = length
                best_pair = (points[i], points[j])
    if best_pair is None or best_length <= point_epsilon:
        return None
    pair = (min(tri_a, tri_b), max(tri_a, tri_b))
    return CrossingSegment(
        tri_a,
        tri_b,
        best_pair[0],
        best_pair[1],
        tuple(sorted(pair)),
        (pair,),
    )


def _candidate_triangle_pairs(mesh: Mesh, cell_size: float) -> set[tuple[int, int]]:
    grid: dict[tuple[int, int, int], list[int]] = defaultdict(list)
    for tri_id, tri in enumerate(mesh.tris):
        lower = np.floor(mesh.verts[tri].min(axis=0) / cell_size).astype(np.int64)
        upper = np.floor(mesh.verts[tri].max(axis=0) / cell_size).astype(np.int64)
        for x in range(int(lower[0]), int(upper[0]) + 1):
            for y in range(int(lower[1]), int(upper[1]) + 1):
                for z in range(int(lower[2]), int(upper[2]) + 1):
                    grid[(x, y, z)].append(tri_id)
    pairs: set[tuple[int, int]] = set()
    for bucket in grid.values():
        for i in range(len(bucket)):
            for j in range(i + 1, len(bucket)):
                a, b = bucket[i], bucket[j]
                pairs.add((min(a, b), max(a, b)))
    return pairs


def find_crossing_segments(
    mesh: Mesh,
    scale: float,
    exclude_rings: int,
) -> list[CrossingSegment]:
    # A seam requires a duplicate boundary edge, never merely a duplicate point.
    seam_reps = boundary_edge_seam_representatives(mesh, scale * 1.0e-6)
    neighbourhoods = topology_neighbourhoods(mesh, exclude_rings, seam_reps)
    tri_neighbourhoods: list[set[int]] = []
    for tri in mesh.tris:
        reached: set[int] = set()
        for vertex in tri:
            reached.update(neighbourhoods[int(vertex)])
        tri_neighbourhoods.append(reached)

    tri_reps = [set(int(seam_reps[int(vertex)]) for vertex in tri) for tri in mesh.tris]
    # Broad-phase cell size affects cost only.  Include the measured mesh's own
    # extent so an exploded candidate cannot create billions of cells merely
    # because it intentionally shares the (smaller) baseline reference unit.
    broadphase_cell = max(scale * 4.0, bounding_box_diagonal(mesh) / 32.0)
    candidates = _candidate_triangle_pairs(mesh, broadphase_cell)
    result: list[CrossingSegment] = []
    for tri_a, tri_b in sorted(candidates):
        if tri_reps[tri_a] & tri_reps[tri_b]:
            continue
        if tri_reps[tri_b] & tri_neighbourhoods[tri_a]:
            continue
        segment = triangle_intersection_segment(
            mesh.verts, mesh.tris, tri_a, tri_b, scale
        )
        if segment is not None:
            result.append(segment)
    return merge_crossing_segments(result, scale)


def merge_crossing_segments(
    segments: Sequence[CrossingSegment], scale: float
) -> list[CrossingSegment]:
    """Return the geometric union of collinear crossing fragments.

    Triangle-pair enumeration fragments a continuous curve at tessellation
    edges.  If a crossing lies on such an edge, identical fragments can also be
    emitted more than once.  Grouping by supporting line and unioning intervals
    makes curve length independent of both effects while retaining every source
    triangle for visibility-ray exclusion and topology-matched diagnostics.
    """
    if not segments:
        return []
    angular_epsilon = 1.0e-7
    spatial_epsilon = scale * 1.0e-5
    direction_quantum = 1.0e-6
    line_groups: dict[tuple[int, int, int], list[dict]] = defaultdict(list)

    def canonical_direction(delta: np.ndarray) -> np.ndarray:
        direction = delta / np.linalg.norm(delta)
        for component in direction:
            if abs(float(component)) > angular_epsilon:
                if component < 0.0:
                    direction = -direction
                break
        return direction

    ordered = sorted(
        segments,
        key=lambda segment: (
            tuple(float(value) for value in np.minimum(segment.p0, segment.p1)),
            tuple(float(value) for value in np.maximum(segment.p0, segment.p1)),
            segment.tri_a,
            segment.tri_b,
        ),
    )
    for segment in ordered:
        direction = canonical_direction(segment.p1 - segment.p0)
        direction_key = tuple(
            int(value) for value in np.rint(direction / direction_quantum)
        )
        groups = line_groups[direction_key]
        chosen: dict | None = None
        for group in groups:
            group_direction = group["direction"]
            if np.linalg.norm(np.cross(direction, group_direction)) > angular_epsilon:
                continue
            offset0 = segment.p0 - group["origin"]
            offset1 = segment.p1 - group["origin"]
            if (
                np.linalg.norm(np.cross(offset0, group_direction)) <= spatial_epsilon
                and np.linalg.norm(np.cross(offset1, group_direction)) <= spatial_epsilon
            ):
                chosen = group
                break
        if chosen is None:
            chosen = {
                "origin": segment.p0.copy(),
                "direction": direction,
                "intervals": [],
            }
            groups.append(chosen)
        origin = chosen["origin"]
        direction = chosen["direction"]
        first = float(np.dot(segment.p0 - origin, direction))
        second = float(np.dot(segment.p1 - origin, direction))
        chosen["intervals"].append(
            (
                min(first, second),
                max(first, second),
                set(segment.skipped_triangles),
                set(segment.triangle_pairs),
            )
        )

    merged: list[CrossingSegment] = []
    for direction_key in sorted(line_groups):
        for group in line_groups[direction_key]:
            intervals = sorted(
                group["intervals"], key=lambda item: (item[0], item[1])
            )
            current_lo, current_hi, owners, pairs = intervals[0]
            unions: list[tuple[float, float, set[int], set[tuple[int, int]]]] = []
            for lo, hi, next_owners, next_pairs in intervals[1:]:
                if lo <= current_hi + spatial_epsilon:
                    current_hi = max(current_hi, hi)
                    owners.update(next_owners)
                    pairs.update(next_pairs)
                else:
                    unions.append((current_lo, current_hi, owners, pairs))
                    current_lo, current_hi = lo, hi
                    owners, pairs = next_owners, next_pairs
            unions.append((current_lo, current_hi, owners, pairs))
            for lo, hi, owners, pairs in unions:
                if hi - lo <= spatial_epsilon:
                    continue
                sorted_pairs = tuple(sorted(pairs))
                representative_pair = sorted_pairs[0]
                p0 = group["origin"] + group["direction"] * lo
                p1 = group["origin"] + group["direction"] * hi
                merged.append(
                    CrossingSegment(
                        representative_pair[0],
                        representative_pair[1],
                        p0,
                        p1,
                        tuple(sorted(owners)),
                        sorted_pairs,
                    )
                )
    return sorted(
        merged,
        key=lambda segment: (
            tuple(float(value) for value in segment.p0),
            tuple(float(value) for value in segment.p1),
            segment.tri_a,
            segment.tri_b,
        ),
    )


class TriangleBVH:
    """Small deterministic BVH used only for two-sided any-hit visibility rays."""

    def __init__(self, mesh: Mesh, leaf_size: int = 8):
        self.mesh = mesh
        self.leaf_size = max(2, int(leaf_size))
        points = mesh.verts[mesh.tris]
        self.tri_min = points.min(axis=1)
        self.tri_max = points.max(axis=1)
        self.centroid = points.mean(axis=1)
        self.tri_a = points[:, 0].copy()
        self.edge1 = (points[:, 1] - points[:, 0]).copy()
        self.edge2 = (points[:, 2] - points[:, 0]).copy()
        self.determinant_scale = np.linalg.norm(self.edge1, axis=1) * np.linalg.norm(
            self.edge2, axis=1
        )
        self.nodes: list[BVHNode] = []
        self._build(np.arange(len(mesh.tris), dtype=np.int64))

    def _build(self, tri_ids: np.ndarray) -> int:
        node_index = len(self.nodes)
        bmin = self.tri_min[tri_ids].min(axis=0)
        bmax = self.tri_max[tri_ids].max(axis=0)
        self.nodes.append(BVHNode(bmin, bmax))
        if len(tri_ids) <= self.leaf_size:
            self.nodes[node_index].tri_ids = tri_ids
            return node_index
        extent = np.ptp(self.centroid[tri_ids], axis=0)
        axis = int(np.argmax(extent))
        order = tri_ids[np.argsort(self.centroid[tri_ids, axis], kind="mergesort")]
        middle = len(order) // 2
        left = self._build(order[:middle])
        right = self._build(order[middle:])
        self.nodes[node_index].left = left
        self.nodes[node_index].right = right
        return node_index

    @staticmethod
    def _ray_box(
        origin: np.ndarray,
        direction: np.ndarray,
        bmin: np.ndarray,
        bmax: np.ndarray,
        t_min: float,
    ) -> bool:
        lower = t_min
        upper = float("inf")
        for axis in range(3):
            component = float(direction[axis])
            if abs(component) <= 1.0e-15:
                if origin[axis] < bmin[axis] or origin[axis] > bmax[axis]:
                    return False
                continue
            first = float((bmin[axis] - origin[axis]) / component)
            second = float((bmax[axis] - origin[axis]) / component)
            if first > second:
                first, second = second, first
            lower = max(lower, first)
            upper = min(upper, second)
            if upper < lower:
                return False
        return upper >= lower

    def _ray_triangle(
        self,
        tri_id: int,
        origin: np.ndarray,
        direction: np.ndarray,
        t_min: float,
    ) -> bool:
        a = self.tri_a[tri_id]
        edge1 = self.edge1[tri_id]
        edge2 = self.edge2[tri_id]
        dx, dy, dz = float(direction[0]), float(direction[1]), float(direction[2])
        e2x, e2y, e2z = float(edge2[0]), float(edge2[1]), float(edge2[2])
        hx = dy * e2z - dz * e2y
        hy = dz * e2x - dx * e2z
        hz = dx * e2y - dy * e2x
        e1x, e1y, e1z = float(edge1[0]), float(edge1[1]), float(edge1[2])
        determinant = e1x * hx + e1y * hy + e1z * hz
        determinant_scale = float(self.determinant_scale[tri_id])
        if (
            determinant_scale <= np.finfo(np.float64).tiny
            or abs(determinant) <= determinant_scale * 1.0e-12
        ):
            return False
        inverse = 1.0 / determinant
        s = origin - a
        sx, sy, sz = float(s[0]), float(s[1]), float(s[2])
        u = inverse * (sx * hx + sy * hy + sz * hz)
        if u < -1.0e-9 or u > 1.0 + 1.0e-9:
            return False
        qx = sy * e1z - sz * e1y
        qy = sz * e1x - sx * e1z
        qz = sx * e1y - sy * e1x
        v = inverse * (dx * qx + dy * qy + dz * qz)
        if v < -1.0e-9 or u + v > 1.0 + 1.0e-9:
            return False
        t = inverse * (e2x * qx + e2y * qy + e2z * qz)
        return math.isfinite(t) and t > t_min

    def any_hit(
        self,
        origin: np.ndarray,
        direction: np.ndarray,
        t_min: float,
        skip_triangles: frozenset[int] = frozenset(),
    ) -> bool:
        stack = [0]
        while stack:
            node_index = stack.pop()
            node = self.nodes[node_index]
            if not self._ray_box(origin, direction, node.bmin, node.bmax, t_min):
                continue
            if node.tri_ids is not None:
                for raw_tri_id in node.tri_ids:
                    tri_id = int(raw_tri_id)
                    if tri_id in skip_triangles:
                        continue
                    if self._ray_triangle(tri_id, origin, direction, t_min):
                        return True
            else:
                stack.append(node.left)
                stack.append(node.right)
        return False


class DisjointSet:
    def __init__(self, count: int):
        self.parent = list(range(count))
        self.rank = [0] * count

    def find(self, item: int) -> int:
        root = item
        while self.parent[root] != root:
            root = self.parent[root]
        while self.parent[item] != item:
            next_item = self.parent[item]
            self.parent[item] = root
            item = next_item
        return root

    def union(self, first: int, second: int) -> None:
        root_a, root_b = self.find(first), self.find(second)
        if root_a == root_b:
            return
        if self.rank[root_a] < self.rank[root_b]:
            root_a, root_b = root_b, root_a
        self.parent[root_b] = root_a
        if self.rank[root_a] == self.rank[root_b]:
            self.rank[root_a] += 1


def cluster_segments(
    segments: Sequence[CrossingSegment], scale: float
) -> tuple[np.ndarray, int]:
    """Join tessellation fragments of the same continuous crossing curve."""
    if not segments:
        return np.empty(0, dtype=np.int64), 0
    sets = DisjointSet(len(segments))
    # Sharing one large triangle does not imply continuity: two unrelated
    # panels can pierce distant parts of that face.  Curves join only where
    # their geometric endpoints coincide within a scale-relative numeric slop.
    endpoint_epsilon = scale * 1.0e-4
    endpoint_grid: dict[tuple[int, int, int], list[tuple[int, np.ndarray]]] = defaultdict(list)
    for index, segment in enumerate(segments):
        for point in (segment.p0, segment.p1):
            cell = tuple(np.floor(point / endpoint_epsilon).astype(np.int64))
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    for dz in (-1, 0, 1):
                        for other, other_point in endpoint_grid.get(
                            (cell[0] + dx, cell[1] + dy, cell[2] + dz), []
                        ):
                            if np.linalg.norm(point - other_point) <= endpoint_epsilon:
                                sets.union(index, other)
            endpoint_grid[cell].append((index, point))

    roots = [sets.find(index) for index in range(len(segments))]
    root_to_id = {root: index for index, root in enumerate(sorted(set(roots)))}
    ids = np.asarray([root_to_id[root] for root in roots], dtype=np.int64)
    return ids, len(root_to_id)


def visibility_metric(
    garment: Mesh,
    segments: Sequence[CrossingSegment],
    cluster_ids: np.ndarray,
    directions: np.ndarray,
    scale: float,
    sample_step_edges: float,
    body: Mesh | None,
    top_count: int,
) -> dict[str, VisibilityResult]:
    garment_bvh = TriangleBVH(garment)
    body_bvh = TriangleBVH(body) if body is not None else None
    direction_count = len(directions)
    segment_count = len(segments)
    projected = {
        "garment_only": np.zeros(direction_count, dtype=np.float64),
    }
    unprojected = {
        "garment_only": np.zeros(direction_count, dtype=np.float64),
    }
    visible = {
        "garment_only": np.zeros((segment_count, direction_count), dtype=bool),
    }
    sample_visibility: dict[str, list[np.ndarray]] = {
        "garment_only": [],
    }
    any_view_curve = {"garment_only": 0.0}
    if body_bvh is not None:
        projected["with_body"] = np.zeros(direction_count, dtype=np.float64)
        unprojected["with_body"] = np.zeros(direction_count, dtype=np.float64)
        visible["with_body"] = np.zeros((segment_count, direction_count), dtype=bool)
        sample_visibility["with_body"] = []
        any_view_curve["with_body"] = 0.0

    ray_epsilon = scale * 1.0e-5
    sample_step = scale * sample_step_edges
    for segment_index, segment in enumerate(segments):
        delta = segment.p1 - segment.p0
        length = float(np.linalg.norm(delta))
        if length <= 1.0e-14:
            continue
        tangent = delta / length
        sample_count = max(1, int(math.ceil(length / sample_step)))
        segment_sample_visibility = {
            channel: np.zeros((sample_count, direction_count), dtype=bool)
            for channel in projected
        }
        normalized_sample_length = length / (sample_count * scale)
        skipped = segment.skipped_triangles
        projection = np.sqrt(
            np.maximum(0.0, 1.0 - np.square(directions @ tangent))
        )
        for sample_index in range(sample_count):
            alpha = (sample_index + 0.5) / sample_count
            point = segment.p0 * (1.0 - alpha) + segment.p1 * alpha
            sample_visible = {channel: False for channel in projected}
            for direction_index, direction in enumerate(directions):
                origin = point + direction * ray_epsilon
                if garment_bvh.any_hit(origin, direction, ray_epsilon, skipped):
                    continue
                visible["garment_only"][segment_index, direction_index] = True
                segment_sample_visibility["garment_only"][
                    sample_index, direction_index
                ] = True
                sample_visible["garment_only"] = True
                unprojected["garment_only"][direction_index] += normalized_sample_length
                projected["garment_only"][direction_index] += (
                    normalized_sample_length * projection[direction_index]
                )
                if body_bvh is not None and not body_bvh.any_hit(
                    origin, direction, ray_epsilon
                ):
                    visible["with_body"][segment_index, direction_index] = True
                    segment_sample_visibility["with_body"][
                        sample_index, direction_index
                    ] = True
                    sample_visible["with_body"] = True
                    unprojected["with_body"][direction_index] += normalized_sample_length
                    projected["with_body"][direction_index] += (
                        normalized_sample_length * projection[direction_index]
                    )
            for channel, was_visible in sample_visible.items():
                if was_visible:
                    any_view_curve[channel] += normalized_sample_length
        for channel in projected:
            sample_visibility[channel].append(segment_sample_visibility[channel])

    results: dict[str, VisibilityResult] = {}
    for channel in projected:
        per_view = projected[channel]
        raw_per_view = unprojected[channel]
        segment_visible = visible[channel]
        exposed_segments = np.any(segment_visible, axis=1) if segment_count else np.zeros(0, bool)
        exposed_pairs = {
            pair
            for index in np.flatnonzero(exposed_segments)
            for pair in segments[int(index)].triangle_pairs
        }
        exposed_cluster_ids = set(
            int(cluster_ids[index])
            for index in np.flatnonzero(exposed_segments)
        )
        order = np.argsort(-per_view, kind="mergesort")[: max(1, top_count)]
        views = [
            {
                "index": int(direction_index),
                "direction": [float(value) for value in directions[direction_index]],
                "projected_curve": float(per_view[direction_index]),
                "unprojected_curve": float(raw_per_view[direction_index]),
            }
            for direction_index in range(direction_count)
        ]
        top_directions = []
        for direction_index in order:
            visible_indices = np.flatnonzero(segment_visible[:, direction_index])
            top_directions.append(
                {
                    "index": int(direction_index),
                    "direction": [float(value) for value in directions[direction_index]],
                    "projected_curve": float(per_view[direction_index]),
                    "unprojected_curve": float(raw_per_view[direction_index]),
                    "visible_segments": int(len(visible_indices)),
                    "visible_clusters": int(
                        len(set(int(cluster_ids[index]) for index in visible_indices))
                    ),
                }
            )
        summary = {
            "exposed_pair_count": int(len(exposed_pairs)),
            "exposed_merged_segment_count": int(np.count_nonzero(exposed_segments)),
            "exposed_cluster_count": int(len(exposed_cluster_ids)),
            "any_view_curve": float(any_view_curve[channel]),
            "solid_angle_curve": float(np.mean(raw_per_view)),
            "mean_projected_curve": float(np.mean(per_view)),
            "p95_projected_curve": float(np.percentile(per_view, 95.0)),
            "worst_projected_curve": float(np.max(per_view)),
            "visible_view_count": int(np.count_nonzero(raw_per_view > 0.0)),
            "top_directions": top_directions,
            "views": views,
        }
        results[channel] = VisibilityResult(
            summary,
            per_view,
            raw_per_view,
            segment_visible,
            sample_visibility[channel],
        )
    return results


def mesh_report(
    mesh: Mesh,
    body: Mesh | None,
    directions: np.ndarray,
    exclude_rings: int,
    sample_step_edges: float,
    top_count: int,
    shared_reference_length: float | None = None,
) -> tuple[dict, list[CrossingSegment], np.ndarray, dict[str, VisibilityResult], float]:
    edge_length = mean_edge_length(mesh)
    scale = (
        float(shared_reference_length)
        if shared_reference_length is not None
        else reference_length(mesh)
    )
    if not math.isfinite(scale) or scale <= np.finfo(np.float64).tiny:
        raise ValueError(f"{mesh.name}: invalid shared reference length {scale}")
    segments = find_crossing_segments(mesh, scale, exclude_rings)
    cluster_ids, cluster_count = cluster_segments(segments, scale)
    channels = visibility_metric(
        mesh,
        segments,
        cluster_ids,
        directions,
        scale,
        sample_step_edges,
        body,
        top_count,
    )
    curve_length = float(sum(segment.length for segment in segments))
    report = {
        "mesh": {
            "name": mesh.name,
            "vertices": int(len(mesh.verts)),
            "triangles": int(len(mesh.tris)),
            "mean_edge": edge_length,
            "bbox_diagonal": bounding_box_diagonal(mesh),
            "reference_length": scale,
            "reference_definition": (
                f"shared_baseline_bbox_diagonal/{REFERENCE_DIVISOR:g}"
                if shared_reference_length is not None
                else f"candidate_bbox_diagonal/{REFERENCE_DIVISOR:g}"
            ),
        },
        "intersections": {
            "pair_count": int(len(pair_set(segments))),
            "merged_segment_count": int(len(segments)),
            "cluster_count": int(cluster_count),
            "curve_length": curve_length,
            "normalized_curve_length": curve_length / scale,
        },
        "channels": {name: value.summary for name, value in channels.items()},
    }
    return report, segments, cluster_ids, channels, scale


def pair_set(segments: Iterable[CrossingSegment]) -> set[tuple[int, int]]:
    return {
        pair
        for segment in segments
        for pair in segment.triangle_pairs
    }


def compare_reports(
    candidate: dict,
    candidate_segments: Sequence[CrossingSegment],
    baseline: dict,
    baseline_segments: Sequence[CrossingSegment],
    topology_matches: bool,
) -> dict:
    comparison: dict = {
        "baseline": baseline["mesh"]["name"],
        "topology_matches": topology_matches,
    }
    if topology_matches:
        candidate_pairs = pair_set(candidate_segments)
        baseline_pairs = pair_set(baseline_segments)
        comparison["pair_identity"] = {
            "candidate_total": len(candidate_pairs),
            "baseline_total": len(baseline_pairs),
            "new": len(candidate_pairs - baseline_pairs),
            "lost": len(baseline_pairs - candidate_pairs),
            "shared": len(candidate_pairs & baseline_pairs),
        }
    channel_deltas = {}
    for channel in candidate["channels"].keys() & baseline["channels"].keys():
        channel_deltas[channel] = {
            key: float(candidate["channels"][channel][key])
            - float(baseline["channels"][channel][key])
            for key in (
                "solid_angle_curve",
                "mean_projected_curve",
                "p95_projected_curve",
                "worst_projected_curve",
            )
        }
    comparison["channel_deltas"] = channel_deltas
    return comparison


def write_curve_obj(
    path: Path,
    mesh: Mesh,
    segments: Sequence[CrossingSegment],
    visibility: VisibilityResult,
    directions: np.ndarray,
    channel: str,
) -> None:
    if visibility.summary["top_directions"]:
        direction_index = int(visibility.summary["top_directions"][0]["index"])
    else:
        direction_index = 0
    direction = directions[direction_index]
    center = (mesh.verts.min(axis=0) + mesh.verts.max(axis=0)) * 0.5
    diagonal = float(np.linalg.norm(mesh.verts.max(axis=0) - mesh.verts.min(axis=0)))
    arrow_end = center + direction * diagonal * 0.25
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as output:
        output.write("# Drape exterior-visible self-intersection diagnostic\n")
        output.write(f"# channel={channel} direction_index={direction_index}\n")
        output.write(
            "# viewer_direction=" + " ".join(f"{value:.9g}" for value in direction) + "\n"
        )
        output.write("o visible_self_intersection_curves\n")
        vertex_index = 1
        for segment_index, segment in enumerate(segments):
            samples = visibility.sample_visible_by_view[segment_index][
                :, direction_index
            ]
            if not np.any(samples):
                continue
            sample_count = len(samples)
            run_start: int | None = None
            runs: list[tuple[int, int]] = []
            for sample_index, is_visible in enumerate(samples):
                if is_visible and run_start is None:
                    run_start = sample_index
                if run_start is not None and (
                    not is_visible or sample_index == sample_count - 1
                ):
                    run_end = (
                        sample_index + 1
                        if is_visible and sample_index == sample_count - 1
                        else sample_index
                    )
                    runs.append((run_start, run_end))
                    run_start = None
            output.write(f"g pair_{segment.tri_a}_{segment.tri_b}\n")
            for run_start, run_end in runs:
                alpha0 = run_start / sample_count
                alpha1 = run_end / sample_count
                p0 = segment.p0 * (1.0 - alpha0) + segment.p1 * alpha0
                p1 = segment.p0 * (1.0 - alpha1) + segment.p1 * alpha1
                output.write(
                    f"# pair={segment.tri_a},{segment.tri_b} "
                    f"sample_range={run_start}:{run_end}/{sample_count} "
                    f"visible_length={np.linalg.norm(p1 - p0):.9g}\n"
                )
                output.write("v " + " ".join(f"{value:.9g}" for value in p0) + "\n")
                output.write("v " + " ".join(f"{value:.9g}" for value in p1) + "\n")
                output.write(f"l {vertex_index} {vertex_index + 1}\n")
                vertex_index += 2
        output.write("o worst_view_direction\n")
        output.write("v " + " ".join(f"{value:.9g}" for value in center) + "\n")
        output.write("v " + " ".join(f"{value:.9g}" for value in arrow_end) + "\n")
        output.write(f"l {vertex_index} {vertex_index + 1}\n")


def _cube_mesh(open_front: bool, name: str) -> Mesh:
    verts = np.asarray(
        [
            (-2, -2, -2), (2, -2, -2), (2, 2, -2), (-2, 2, -2),
            (-2, -2, 2), (2, -2, 2), (2, 2, 2), (-2, 2, 2),
        ],
        dtype=np.float64,
    )
    faces = [
        (0, 2, 1), (0, 3, 2),       # bottom
        (4, 5, 6), (4, 6, 7),       # top
        (0, 1, 5), (0, 5, 4),       # front, removed for the open fixture
        (3, 7, 6), (3, 6, 2),       # back
        (0, 4, 7), (0, 7, 3),       # left
        (1, 2, 6), (1, 6, 5),       # right
    ]
    if open_front:
        faces = faces[:4] + faces[6:]
    return Mesh(verts, np.asarray(faces, dtype=np.int64), name)


def _crossing_panels(name: str) -> Mesh:
    verts = np.asarray(
        [
            (-0.8, -0.8, 0), (0.8, -0.8, 0), (0.8, 0.8, 0), (-0.8, 0.8, 0),
            (0, -0.7, -0.8), (0, 0.7, -0.8), (0, 0.7, 0.8), (0, -0.7, 0.8),
        ],
        dtype=np.float64,
    )
    tris = np.asarray(
        [(0, 1, 2), (0, 2, 3), (4, 5, 6), (4, 6, 7)], dtype=np.int64
    )
    return Mesh(verts, tris, name)


def _combine_meshes(meshes: Sequence[Mesh], name: str) -> Mesh:
    verts: list[np.ndarray] = []
    tris: list[np.ndarray] = []
    offset = 0
    for mesh in meshes:
        verts.append(mesh.verts)
        tris.append(mesh.tris + offset)
        offset += len(mesh.verts)
    return Mesh(np.vstack(verts), np.vstack(tris), name)


def _scaled(mesh: Mesh, scale: float, name: str) -> Mesh:
    return Mesh(mesh.verts * scale, mesh.tris.copy(), name)


def _subdivided(mesh: Mesh, name: str) -> Mesh:
    """One deterministic 1-to-4 midpoint subdivision for planar fixtures."""
    verts = [vertex.copy() for vertex in mesh.verts]
    midpoint_by_edge: dict[tuple[int, int], int] = {}

    def midpoint(first: int, second: int) -> int:
        edge = (min(first, second), max(first, second))
        if edge not in midpoint_by_edge:
            midpoint_by_edge[edge] = len(verts)
            verts.append((mesh.verts[edge[0]] + mesh.verts[edge[1]]) * 0.5)
        return midpoint_by_edge[edge]

    tris: list[tuple[int, int, int]] = []
    for raw_a, raw_b, raw_c in mesh.tris:
        a, b, c = int(raw_a), int(raw_b), int(raw_c)
        ab, bc, ca = midpoint(a, b), midpoint(b, c), midpoint(c, a)
        tris.extend(
            ((a, ab, ca), (ab, b, bc), (ca, bc, c), (ab, bc, ca))
        )
    return Mesh(
        np.asarray(verts, dtype=np.float64),
        np.asarray(tris, dtype=np.int64),
        name,
    )


def _edge_aligned_crossing(name: str) -> Mesh:
    """Two panels whose crossing is an interior triangulation edge."""
    verts = np.asarray(
        [
            (0, -1, 0), (0, 1, 0),
            (-1, -1, 0), (-1, 1, 0),
            (1, -1, 0), (1, 1, 0),
            (0, -0.8, -1), (0, 0.8, -1),
            (0, 0.8, 1), (0, -0.8, 1),
        ],
        dtype=np.float64,
    )
    tris = np.asarray(
        [
            (0, 2, 3), (0, 3, 1),
            (0, 1, 5), (0, 5, 4),
            (6, 7, 8), (6, 8, 9),
        ],
        dtype=np.int64,
    )
    return Mesh(verts, tris, name)


def _selftest_require(condition: bool, message: str) -> None:
    # Do not use Python's ``assert`` here: ``python -O`` strips assertions and
    # would otherwise print a false PASS for an entirely unchecked test suite.
    if not condition:
        raise RuntimeError(f"SELFTEST FAIL: {message}")


def run_selftests() -> None:
    directions = fibonacci_directions(162)
    panels = _crossing_panels("panels")
    closed = _combine_meshes((panels, _cube_mesh(False, "closed_box")), "hidden")
    opened = _combine_meshes((panels, _cube_mesh(True, "open_box")), "exposed")

    hidden, hidden_segments, _, _, _ = mesh_report(
        closed, None, directions, 0, 0.5, 3
    )
    exposed, exposed_segments, _, _, _ = mesh_report(
        opened, None, directions, 0, 0.5, 3
    )
    _selftest_require(
        len(hidden_segments) == len(exposed_segments) > 0,
        "opening changed raw crossing topology",
    )
    _selftest_require(
        math.isclose(
            sum(segment.length for segment in exposed_segments),
            1.4,
            rel_tol=1.0e-12,
            abs_tol=1.0e-12,
        ),
        "orthogonal crossing curve length is not 1.4",
    )
    _selftest_require(
        hidden["channels"]["garment_only"]["solid_angle_curve"] < 1.0e-10,
        "closed shell leaked visibility",
    )
    _selftest_require(
        exposed["channels"]["garment_only"]["solid_angle_curve"] > 1.0e-3,
        "open shell did not expose its crossing",
    )

    # The body is a second channel, never a replacement for garment-only.
    body = _cube_mesh(False, "opaque_body")
    body_report, body_segments, _, _, _ = mesh_report(
        panels, body, directions, 0, 0.5, 3
    )
    _selftest_require(bool(body_segments), "body fixture has no crossing")
    _selftest_require(
        body_report["channels"]["garment_only"]["solid_angle_curve"] > 0.0,
        "garment-only channel was suppressed by optional body",
    )
    _selftest_require(
        body_report["channels"]["with_body"]["solid_angle_curve"] < 1.0e-10,
        "opaque body did not hide enclosed crossing",
    )

    # Dimensionless curve and exposure metrics are invariant under uniform scale.
    for scale in (7.3, 1.0e-6):
        scaled = _scaled(opened, scale, f"scaled_exposed_{scale:g}")
        scaled_report, _, _, _, _ = mesh_report(
            scaled, None, directions, 0, 0.5, 3
        )
        for key in ("normalized_curve_length",):
            _selftest_require(
                math.isclose(
                    exposed["intersections"][key],
                    scaled_report["intersections"][key],
                    rel_tol=1.0e-8,
                    abs_tol=1.0e-10,
                ),
                f"{key} changed under scale {scale:g}",
            )
        for key in ("solid_angle_curve", "worst_projected_curve"):
            _selftest_require(
                math.isclose(
                    exposed["channels"]["garment_only"][key],
                    scaled_report["channels"]["garment_only"][key],
                    rel_tol=1.0e-8,
                    abs_tol=1.0e-10,
                ),
                f"{key} changed under scale {scale:g}",
            )

    # Coplanar overlap is a separate z-fighting problem, not a crossing curve.
    coplanar = Mesh(
        np.asarray(
            [(-1, -1, 0), (1, -1, 0), (0, 1, 0),
             (-0.8, -0.5, 0), (0.8, -0.5, 0), (0, 0.9, 0)],
            dtype=np.float64,
        ),
        np.asarray([(0, 1, 2), (3, 4, 5)], dtype=np.int64),
        "coplanar",
    )
    coplanar_report, coplanar_segments, _, _, _ = mesh_report(
        coplanar, None, directions, 0, 0.5, 3
    )
    _selftest_require(not coplanar_segments, "coplanar overlap became a curve")
    _selftest_require(
        coplanar_report["intersections"]["pair_count"] == 0,
        "coplanar pair count is nonzero",
    )

    # A point touch between distinct panels is not a seam and is not a curve.
    point_touch = Mesh(
        np.asarray(
            [
                (0, 0, 0), (1, 0, 0), (0, 1, 0),
                (0, 0, 0), (0, -1, -1), (0, -1, 1),
            ],
            dtype=np.float64,
        ),
        np.asarray([(0, 1, 2), (3, 4, 5)], dtype=np.int64),
        "distinct_panel_point_touch",
    )
    point_scale = reference_length(point_touch)
    point_reps = boundary_edge_seam_representatives(
        point_touch, point_scale * 1.0e-6
    )
    _selftest_require(
        point_reps[0] != point_reps[3],
        "point coincidence falsely welded distinct panels",
    )
    _selftest_require(
        not find_crossing_segments(point_touch, point_scale, 0),
        "point contact became a crossing curve",
    )

    # A duplicated complete boundary edge is a split material seam.  Its
    # endpoint indices must be canonicalized before intersection testing.
    seam = Mesh(
        np.asarray(
            [
                (-1, 0, 0), (1, 0, 0), (0, 1, 0),
                (-1, 0, 0), (1, 0, 0), (0, 0, 1),
            ],
            dtype=np.float64,
        ),
        np.asarray([(0, 1, 2), (3, 4, 5)], dtype=np.int64),
        "duplicate_boundary_edge_seam",
    )
    seam_scale = reference_length(seam)
    seam_reps = boundary_edge_seam_representatives(seam, seam_scale * 1.0e-6)
    _selftest_require(
        seam_reps[0] == seam_reps[3] and seam_reps[1] == seam_reps[4],
        "duplicate boundary edge was not recognized as a split seam",
    )
    _selftest_require(
        not find_crossing_segments(seam, seam_scale, 0),
        "duplicate boundary-edge seam became a self-crossing",
    )

    # Identical geometry must score identically after retessellation.  Both
    # measurements deliberately share the coarse fixture's reference unit.
    subdivided = _subdivided(panels, "subdivided_panels")
    shared_scale = reference_length(panels)
    coarse_report, coarse_segments, _, _, _ = mesh_report(
        panels, None, directions, 0, 0.5, 3, shared_scale
    )
    subdiv_report, subdiv_segments, _, _, _ = mesh_report(
        subdivided, None, directions, 0, 0.5, 3, shared_scale
    )
    _selftest_require(
        math.isclose(
            sum(segment.length for segment in coarse_segments),
            sum(segment.length for segment in subdiv_segments),
            rel_tol=1.0e-10,
            abs_tol=1.0e-10,
        ),
        "curve union changed under midpoint subdivision",
    )
    for channel_key in (
        "solid_angle_curve",
        "mean_projected_curve",
        "p95_projected_curve",
        "worst_projected_curve",
    ):
        _selftest_require(
            math.isclose(
                coarse_report["channels"]["garment_only"][channel_key],
                subdiv_report["channels"]["garment_only"][channel_key],
                rel_tol=1.0e-10,
                abs_tol=1.0e-10,
            ),
            f"{channel_key} changed under midpoint subdivision",
        )

    # A true crossing may coincide exactly with an interior triangulation edge.
    # It must be found once, not rejected by strict per-triangle straddling or
    # counted repeatedly by adjacent triangle pairs.
    edge_aligned = _edge_aligned_crossing("edge_aligned_crossing")
    edge_segments = find_crossing_segments(
        edge_aligned, reference_length(edge_aligned), 0
    )
    _selftest_require(
        len(edge_segments) == 1,
        f"edge-aligned curve produced {len(edge_segments)} merged segments",
    )
    _selftest_require(
        math.isclose(edge_segments[0].length, 1.6, rel_tol=1.0e-10, abs_tol=1.0e-10),
        "edge-aligned crossing curve length is not 1.6",
    )

    # Winding is irrelevant: every occluder is deliberately two-sided.
    reversed_open = Mesh(opened.verts.copy(), opened.tris[:, ::-1].copy(), "reversed")
    reversed_report, reversed_segments, _, _, _ = mesh_report(
        reversed_open, None, directions, 0, 0.5, 3
    )
    _selftest_require(
        len(reversed_segments) == len(exposed_segments),
        "reversing winding changed raw crossings",
    )
    _selftest_require(
        math.isclose(
            reversed_report["channels"]["garment_only"]["solid_angle_curve"],
            exposed["channels"]["garment_only"]["solid_angle_curve"],
            rel_tol=1.0e-12,
            abs_tol=1.0e-12,
        ),
        "reversing winding changed exterior visibility",
    )

    print("SELFTEST PASS: hidden/open visibility with identical crossings")
    print("SELFTEST PASS: garment-only plus optional opaque-body channel")
    print("SELFTEST PASS: uniform-scale invariance")
    print("SELFTEST PASS: coplanar contact excluded from crossing curves")
    print("SELFTEST PASS: point touches remain distinct; duplicate-edge seams weld")
    print("SELFTEST PASS: coarse/subdivided metrics share one reference scale")
    print("SELFTEST PASS: edge-aligned crossing is detected once")
    print("SELFTEST PASS: winding invariant")


def print_human(report: dict, stream) -> None:
    mesh = report["candidate"]["mesh"]
    intersections = report["candidate"]["intersections"]
    print(
        f"{mesh['name']}: {mesh['vertices']} verts / {mesh['triangles']} tris",
        file=stream,
    )
    print(
        "  crossings "
        f"pairs={intersections['pair_count']} clusters={intersections['cluster_count']} "
        f"curve/edge={intersections['normalized_curve_length']:.4f}",
        file=stream,
    )
    for channel, values in report["candidate"]["channels"].items():
        print(
            f"  {channel:<13} exposedPairs={values['exposed_pair_count']} "
            f"solidAngle={values['solid_angle_curve']:.4f} "
            f"p95={values['p95_projected_curve']:.4f} "
            f"worst={values['worst_projected_curve']:.4f}",
            file=stream,
        )
        if values["top_directions"]:
            top = values["top_directions"][0]
            direction = ",".join(f"{value:+.3f}" for value in top["direction"])
            print(f"    top direction [{direction}]", file=stream)
    if "comparison" in report:
        pairs = report["comparison"].get("pair_identity")
        if pairs:
            print(
                "  vs baseline "
                f"new={pairs['new']} lost={pairs['lost']} shared={pairs['shared']}",
                file=stream,
            )
        for channel, deltas in report["comparison"]["channel_deltas"].items():
            print(
                f"    delta {channel}: solidAngle={deltas['solid_angle_curve']:+.4f} "
                f"p95={deltas['p95_projected_curve']:+.4f} "
                f"worst={deltas['worst_projected_curve']:+.4f}",
                file=stream,
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--garment", type=Path, help="candidate garment OBJ")
    parser.add_argument("--baseline", type=Path, help="optional baseline garment OBJ")
    parser.add_argument(
        "--body", type=Path,
        help="optional opaque body OBJ; adds a with_body channel",
    )
    parser.add_argument("--json", type=str, help="write JSON report to path, or - for stdout")
    parser.add_argument("--curve-obj", type=Path, help="write worst-view curve diagnostic OBJ")
    parser.add_argument(
        "--curve-channel", choices=("garment_only", "with_body"),
        help="channel used by --curve-obj (default: with_body when available)",
    )
    parser.add_argument("--directions", type=int, default=162)
    parser.add_argument("--exclude-rings", type=int, default=0)
    parser.add_argument(
        "--sample-step-edges", type=float, default=0.5,
        help=(
            "maximum curve-sample spacing in reference units; the legacy "
            "option name is retained for CLI compatibility"
        ),
    )
    parser.add_argument("--top-directions", type=int, default=8)
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()

    if args.selftest:
        run_selftests()
        return 0
    if args.garment is None:
        parser.error("--garment is required unless --selftest is used")
    if args.directions < 12:
        parser.error("--directions must be at least 12")
    if args.top_directions <= 0:
        parser.error("--top-directions must be positive")
    if args.exclude_rings < 0:
        parser.error("--exclude-rings must be non-negative")
    if not math.isfinite(args.sample_step_edges) or args.sample_step_edges <= 0.0:
        parser.error("--sample-step-edges must be positive")

    input_paths = [args.garment]
    if args.baseline:
        input_paths.append(args.baseline)
    if args.body:
        input_paths.append(args.body)
    input_resolved = {path.resolve() for path in input_paths}
    output_paths = []
    if args.json and args.json != "-":
        output_paths.append(Path(args.json))
    if args.curve_obj:
        output_paths.append(args.curve_obj)
    if any(path.resolve() in input_resolved for path in output_paths):
        parser.error("an output path may not overwrite an input OBJ")
    output_resolved = [path.resolve() for path in output_paths]
    if len(set(output_resolved)) != len(output_resolved):
        parser.error("--json and --curve-obj must use different output paths")

    directions = fibonacci_directions(args.directions)

    def load_input(label: str, path: Path) -> Mesh:
        try:
            return read_obj(path)
        except (OSError, ValueError, OverflowError) as error:
            parser.error(f"invalid {label} OBJ: {error}")

    def measure_input(
        label: str, mesh: Mesh
    ) -> tuple[dict, list[CrossingSegment], np.ndarray, dict[str, VisibilityResult], float]:
        try:
            return mesh_report(
                mesh,
                body,
                directions,
                args.exclude_rings,
                args.sample_step_edges,
                args.top_directions,
                shared_reference,
            )
        except (ValueError, OverflowError) as error:
            parser.error(f"cannot measure {label} OBJ: {error}")

    garment = load_input("--garment", args.garment)
    baseline_mesh = (
        load_input("--baseline", args.baseline) if args.baseline else None
    )
    body = load_input("--body", args.body) if args.body else None
    shared_reference = (
        reference_length(baseline_mesh) if baseline_mesh is not None else None
    )
    candidate, segments, cluster_ids, channels, _ = measure_input(
        "--garment", garment
    )
    report = {
        "metric": "exterior_visible_self_intersection_curve",
        "version": METRIC_VERSION,
        "config": {
            "directions": args.directions,
            "direction_set": "fibonacci_sphere",
            "exclude_topology_rings": args.exclude_rings,
            "sample_step_reference_units": args.sample_step_edges,
            "reference_definition": f"bbox_diagonal/{REFERENCE_DIVISOR:g}",
            "reference_source": "baseline" if baseline_mesh is not None else "candidate",
            "body_occluder": str(args.body) if args.body else None,
        },
        "inputs": {
            "garment": {
                "path": str(args.garment.resolve()),
                "sha256": sha256_file(args.garment),
            },
            "baseline": (
                {
                    "path": str(args.baseline.resolve()),
                    "sha256": sha256_file(args.baseline),
                }
                if args.baseline else None
            ),
            "body": (
                {
                    "path": str(args.body.resolve()),
                    "sha256": sha256_file(args.body),
                }
                if args.body else None
            ),
        },
        "candidate": candidate,
    }

    if args.baseline:
        if baseline_mesh is None:
            raise RuntimeError("internal error: baseline was not loaded")
        baseline, baseline_segments, _, _, _ = measure_input(
            "--baseline", baseline_mesh
        )
        report["baseline"] = baseline
        topology_matches = (
            len(garment.verts) == len(baseline_mesh.verts)
            and np.array_equal(garment.tris, baseline_mesh.tris)
        )
        report["comparison"] = compare_reports(
            candidate,
            segments,
            baseline,
            baseline_segments,
            topology_matches,
        )

    if args.curve_obj:
        channel = args.curve_channel or ("with_body" if body is not None else "garment_only")
        if channel not in channels:
            parser.error("--curve-channel with_body requires --body")
        try:
            write_curve_obj(
                args.curve_obj,
                garment,
                segments,
                channels[channel],
                directions,
                channel,
            )
        except OSError as error:
            parser.error(f"cannot write --curve-obj: {error}")
        report["curve_obj"] = {
            "path": str(args.curve_obj),
            "channel": channel,
        }

    human_stream = sys.stderr if args.json == "-" else sys.stdout
    if not args.quiet:
        print_human(report, human_stream)
    serialized = json.dumps(report, indent=2, sort_keys=True, allow_nan=False)
    if args.json == "-":
        print(serialized)
    elif args.json:
        json_path = Path(args.json)
        try:
            json_path.parent.mkdir(parents=True, exist_ok=True)
            json_path.write_text(serialized + "\n", encoding="utf-8")
        except OSError as error:
            parser.error(f"cannot write --json: {error}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
