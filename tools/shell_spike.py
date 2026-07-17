#!/usr/bin/env python3
"""
Quasi-static shell spike for the BlackSmithTorso RM_Heavy capture.

This is an offline hypothesis test, not production code.  It deliberately reads
the capture's authored mesh, bone profiles and body pair so the experiment does
not depend on a butt-specific ROI.

Initial milestone: `--inspect` validates the frozen data contract and reports the
topological/material structures needed by the solver.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import struct
from dataclasses import dataclass
from pathlib import Path

import numpy as np

try:
    import scipy
    from scipy.spatial import cKDTree
except ImportError:  # the exact expanding-grid fallback below remains valid
    scipy = None
    cKDTree = None


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CAPTURE = (
    REPO_ROOT
    / "corpus"
    / "fur_blacksmith_rm_heavy"
    / "drape_capture__Rley_3BA_Vanilla_-_Body_-_Blacksmith__Physics___BlackSmithTorso__RM_Heavy.drcap"
)
DEFAULT_DATASET = REPO_ROOT / "tmp" / "shell_spike_blacksmith"


@dataclass
class ObjMesh:
    verts: np.ndarray
    tris: np.ndarray


@dataclass
class CaptureFront:
    version: int
    shape_name: str
    slider_name: str
    preset: int
    ref_center: np.ndarray
    ref_z: tuple[float, float]
    base_verts: np.ndarray
    profiles: np.ndarray
    edges: np.ndarray
    edge_rest: np.ndarray
    tris: np.ndarray
    body_verts: np.ndarray
    body_tris: np.ndarray
    ref_body_verts: np.ndarray
    ref_body_tris: np.ndarray
    preset_mask: np.ndarray
    raw_diff: np.ndarray


@dataclass
class SurfaceAttachments:
    """Material bindings from cloth support vertices to the reference body."""

    requested: np.ndarray
    valid: np.ndarray
    triangle: np.ndarray
    barycentric: np.ndarray
    authored_offset: np.ndarray
    authored_distance: np.ndarray


class BinReader:
    def __init__(self, path: Path):
        self.data = path.read_bytes()
        self.off = 0

    def unpack(self, fmt: str):
        size = struct.calcsize("<" + fmt)
        if self.off + size > len(self.data):
            raise ValueError(f"capture truncated at byte {self.off}")
        value = struct.unpack_from("<" + fmt, self.data, self.off)
        self.off += size
        return value[0] if len(value) == 1 else value

    def string(self) -> str:
        n = self.unpack("I")
        value = self.data[self.off : self.off + n]
        self.off += n
        return value.decode("utf-8", "replace")

    def count(self) -> int:
        return self.unpack("I")


def _read_vec3_array(r: BinReader) -> np.ndarray:
    n = r.count()
    out = np.empty((n, 3), dtype=np.float64)
    for i in range(n):
        out[i] = r.unpack("fff")
    return out


def _read_tri_array(r: BinReader) -> np.ndarray:
    n = r.count()
    out = np.empty((n, 3), dtype=np.int32)
    for i in range(n):
        out[i] = r.unpack("HHH")
    return out


def _skip_float_vec(r: BinReader) -> None:
    # Do not combine this with augmented assignment: `r.off += r.count() * 4`
    # captures the old lhs value before count() advances by four bytes, silently
    # rewinding the reader to a misaligned offset.
    n = r.count()
    r.off += n * 4


def _skip_u8_vec(r: BinReader) -> None:
    n = r.count()
    r.off += n


def read_capture_front(path: Path) -> CaptureFront:
    """Read capture fields through rawDiff; later collider payload is unnecessary."""
    r = BinReader(path)
    magic, version = r.unpack("II")
    # v4 (publication-aware, 2026-07-13) only adds fields AFTER the prefix this
    # reader consumes: the C++ ReadSolverCapture gates version>=3 innerColliders
    # and version>=4 publication state, both serialized after rawDiff. The
    # prefix through rawDiff is byte-identical across v3/v4, so accepting v4
    # here is safe (verified against conform_hook.cpp ReadSolverCapture).
    if magic != 0x43505244 or not 1 <= version <= 4:
        raise ValueError(f"unsupported capture magic/version: {magic:#x}/{version}")

    shape_name = r.string()
    slider_name = r.string()
    _project_key = r.string()
    preset = r.unpack("i")
    _processing_intensity = r.unpack("i")
    cx, cy, zmin, zmax = r.unpack("ffff")

    base_verts = _read_vec3_array(r)

    profile_count = r.count()
    profiles = np.empty((profile_count, 9), dtype=np.float64)
    for i in range(profile_count):
        profiles[i, :8] = r.unpack("ffffffff")
        profiles[i, 8] = r.unpack("i")

    edge_count = r.count()
    edges = np.empty((edge_count, 2), dtype=np.int32)
    edge_rest = np.empty(edge_count, dtype=np.float64)
    for i in range(edge_count):
        a, b, rest = r.unpack("HHf")
        edges[i] = (a, b)
        edge_rest[i] = rest

    if version >= 2:
        old_bending_count = r.count()
        r.off += old_bending_count * struct.calcsize("<HHf")

    tris = _read_tri_array(r)

    has_body_shell = bool(r.unpack("?"))
    if has_body_shell:
        r.unpack("iifff")
        _skip_float_vec(r)
        _skip_float_vec(r)
        _skip_u8_vec(r)
        r.unpack("?")

    has_body = bool(r.unpack("?"))
    body_verts = _read_vec3_array(r) if has_body else np.empty((0, 3))
    body_tris = _read_tri_array(r) if has_body else np.empty((0, 3), dtype=np.int32)

    has_ref_body = bool(r.unpack("?"))
    ref_body_verts = _read_vec3_array(r) if has_ref_body else np.empty((0, 3))
    ref_body_tris = _read_tri_array(r) if has_ref_body else np.empty((0, 3), dtype=np.int32)

    preset_mask = np.zeros(len(base_verts), dtype=np.float64)
    for _ in range(r.count()):
        idx, value = r.unpack("Hf")
        if idx < len(preset_mask):
            preset_mask[idx] = value

    raw_diff = np.zeros_like(base_verts)
    for _ in range(r.count()):
        idx, x, y, z = r.unpack("Hfff")
        if idx < len(raw_diff):
            raw_diff[idx] = (x, y, z)

    return CaptureFront(
        version=version,
        shape_name=shape_name,
        slider_name=slider_name,
        preset=preset,
        ref_center=np.array([cx, cy], dtype=np.float64),
        ref_z=(zmin, zmax),
        base_verts=base_verts,
        profiles=profiles,
        edges=edges,
        edge_rest=edge_rest,
        tris=tris,
        body_verts=body_verts,
        body_tris=body_tris,
        ref_body_verts=ref_body_verts,
        ref_body_tris=ref_body_tris,
        preset_mask=preset_mask,
        raw_diff=raw_diff,
    )


def load_obj(path: Path) -> ObjMesh:
    verts: list[tuple[float, float, float]] = []
    tris: list[tuple[int, int, int]] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("v "):
            fields = line.split()
            verts.append((float(fields[1]), float(fields[2]), float(fields[3])))
        elif line.startswith("f "):
            fields = line.split()[1:]
            face = [int(v.split("/", 1)[0]) - 1 for v in fields]
            if len(face) != 3:
                raise ValueError(f"non-triangle face in {path}: {line}")
            tris.append(tuple(face))
    return ObjMesh(np.asarray(verts, dtype=np.float64), np.asarray(tris, dtype=np.int32))


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class DSU:
    def __init__(self, n: int):
        self.parent = np.arange(n, dtype=np.int32)

    def find(self, x: int) -> int:
        while self.parent[x] != x:
            self.parent[x] = self.parent[self.parent[x]]
            x = int(self.parent[x])
        return x

    def union(self, a: int, b: int) -> None:
        ra, rb = self.find(a), self.find(b)
        if ra != rb:
            self.parent[rb] = ra


def topology_stats(verts: np.ndarray, tris: np.ndarray):
    edge_faces: dict[tuple[int, int], list[tuple[int, int, int, int]]] = {}
    for ti, tri in enumerate(tris):
        a, b, c = map(int, tri)
        for u, v, opposite in ((a, b, c), (b, c, a), (c, a, b)):
            key = (u, v) if u < v else (v, u)
            # Preserve the first face's oriented edge. It defines the signed
            # dihedral convention; a consistently oriented neighbour traverses
            # the shared edge in the opposite direction.
            edge_faces.setdefault(key, []).append((ti, opposite, u, v))

    hinges = []
    boundary = 0
    nonmanifold = 0
    for (a, b), refs in sorted(edge_faces.items()):
        if len(refs) == 2:
            hinges.append((refs[0][2], refs[0][3], refs[0][1], refs[1][1]))
        elif len(refs) == 1:
            boundary += 1
        else:
            nonmanifold += 1

    dsu = DSU(len(verts))
    for a, b in edge_faces:
        dsu.union(a, b)
    component_sizes: dict[int, int] = {}
    for i in range(len(verts)):
        root = dsu.find(i)
        component_sizes[root] = component_sizes.get(root, 0) + 1

    # Exact coincident vertices are the canonical first seam signal.  The solver
    # will later widen this to the production tolerance only if necessary.
    coincident: dict[tuple[float, float, float], list[int]] = {}
    for i, v in enumerate(verts):
        coincident.setdefault(tuple(v), []).append(i)
    seam_groups = [g for g in coincident.values() if len(g) >= 2]

    return {
        "edge_faces": edge_faces,
        "hinges": np.asarray(hinges, dtype=np.int32),
        "boundary_edges": boundary,
        "nonmanifold_edges": nonmanifold,
        "component_sizes": sorted(component_sizes.values(), reverse=True),
        "seam_groups": seam_groups,
    }


@dataclass(frozen=True)
class SeamReducedShell:
    vertex_to_node: np.ndarray
    node_members: tuple[np.ndarray, ...]
    base_verts: np.ndarray
    raw_verts: np.ndarray
    tris: np.ndarray


def reduce_exact_seam_dofs(
    base_verts: np.ndarray,
    raw_verts: np.ndarray,
    tris: np.ndarray,
    seam_groups: list[list[int]],
) -> SeamReducedShell:
    """Collapse exact authored seam twins into physical solver DOFs."""
    dsu = DSU(len(base_verts))
    for group in seam_groups:
        for vertex in group[1:]:
            dsu.union(int(group[0]), int(vertex))
    by_root: dict[int, list[int]] = {}
    for vertex in range(len(base_verts)):
        by_root.setdefault(dsu.find(vertex), []).append(vertex)
    members = tuple(
        np.asarray(group, dtype=np.int32)
        for group in sorted(by_root.values(), key=lambda group: min(group))
    )
    vertex_to_node = np.empty(len(base_verts), dtype=np.int32)
    node_base = np.empty((len(members), 3), dtype=np.float64)
    node_raw = np.empty_like(node_base)
    for node, group in enumerate(members):
        vertex_to_node[group] = node
        node_base[node] = np.mean(base_verts[group], axis=0)
        node_raw[node] = np.mean(raw_verts[group], axis=0)
        if np.max(np.linalg.norm(base_verts[group] - node_base[node], axis=1)) > 1.0e-10:
            raise ValueError("authored exact seam group is not actually coincident")
        if np.max(np.linalg.norm(raw_verts[group] - node_raw[node], axis=1)) > 1.0e-6:
            raise ValueError("raw conform splits an exact authored seam group")
    node_tris = vertex_to_node[np.asarray(tris, dtype=np.int32)]
    if np.any(
        (node_tris[:, 0] == node_tris[:, 1])
        | (node_tris[:, 1] == node_tris[:, 2])
        | (node_tris[:, 2] == node_tris[:, 0])
    ):
        raise ValueError("seam reduction creates a degenerate triangle")
    canonical = np.sort(node_tris, axis=1)
    if len({tuple(row) for row in canonical}) != len(canonical):
        raise ValueError("seam reduction creates duplicate triangles")
    return SeamReducedShell(
        vertex_to_node=vertex_to_node,
        node_members=members,
        base_verts=node_base,
        raw_verts=node_raw,
        tris=node_tris,
    )


def triangle_adjacency(tris: np.ndarray) -> list[list[int]]:
    edge_owner: dict[tuple[int, int], int] = {}
    adjacency: list[list[int]] = [[] for _ in range(len(tris))]
    for ti, tri in enumerate(tris):
        a, b, c = map(int, tri)
        for u, v in ((a, b), (b, c), (c, a)):
            key = (u, v) if u < v else (v, u)
            other = edge_owner.get(key)
            if other is None:
                edge_owner[key] = ti
            else:
                adjacency[ti].append(other)
                adjacency[other].append(ti)
    for nbr in adjacency:
        nbr.sort()
    return adjacency


def vertex_adjacency(vertex_count: int, edges: np.ndarray) -> list[list[int]]:
    adjacency: list[list[int]] = [[] for _ in range(vertex_count)]
    for a, b in edges:
        ai, bi = int(a), int(b)
        adjacency[ai].append(bi)
        adjacency[bi].append(ai)
    for nbr in adjacency:
        nbr.sort()
    return adjacency


def dilate_triangles(seeds: set[int], adjacency: list[list[int]], rings: int) -> set[int]:
    selected = set(seeds)
    frontier = sorted(seeds)
    for _ in range(max(0, rings)):
        next_frontier: list[int] = []
        for ti in frontier:
            for nb in adjacency[ti]:
                if nb not in selected:
                    selected.add(nb)
                    next_frontier.append(nb)
        frontier = sorted(next_frontier)
        if not frontier:
            break
    return selected


def triangle_ring_weights(
    seeds: set[int],
    adjacency: list[list[int]],
    rings: int,
    falloff: str,
) -> dict[int, float]:
    """Geodesic support weights around contacted body triangles.

    ``binary`` reproduces the original experiment. ``linear`` keeps contacted
    triangles at full strength and fades added rings, avoiding a hard support
    discontinuity at the dilation boundary.
    """
    distances = {ti: 0 for ti in seeds}
    frontier = sorted(seeds)
    for distance in range(1, max(0, rings) + 1):
        next_frontier: list[int] = []
        for ti in frontier:
            for nb in adjacency[ti]:
                if nb not in distances:
                    distances[nb] = distance
                    next_frontier.append(nb)
        frontier = sorted(next_frontier)
        if not frontier:
            break
    if falloff == "binary" or rings <= 0:
        return {ti: 1.0 for ti in distances}
    if falloff == "linear":
        scale = 1.0 / (rings + 1.0)
        return {ti: (rings + 1 - distance) * scale for ti, distance in distances.items()}
    raise ValueError(f"unknown shadow contact falloff: {falloff}")


def fitted_drape_signal(cap: CaptureFront) -> np.ndarray:
    """Mirror the current non-Verlet fitted classifier: pelvis+leg, sleeves suppressed."""
    if len(cap.profiles) != len(cap.base_verts):
        raise ValueError("capture profile count does not match cloth vertex count")
    pelvis = np.clip(cap.profiles[:, 1], 0.0, 1.0)
    leg = np.clip(cap.profiles[:, 2], 0.0, 1.0)
    arm = np.clip((cap.profiles[:, 3] + cap.profiles[:, 4]) * 1.6, 0.0, 1.0)
    return np.clip((pelvis + leg) * (1.0 - arm), 0.0, 1.0)


def bbox_text(verts: np.ndarray) -> str:
    lo = verts.min(axis=0)
    hi = verts.max(axis=0)
    return f"min={np.array2string(lo, precision=3)} max={np.array2string(hi, precision=3)}"


def write_obj(path: Path, verts: np.ndarray, tris: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as f:
        for x, y, z in verts:
            f.write(f"v {x:.9g} {y:.9g} {z:.9g}\n")
        for a, b, c in tris:
            f.write(f"f {int(a)+1} {int(b)+1} {int(c)+1}\n")


def closest_points_triangles(
    p: np.ndarray, a: np.ndarray, b: np.ndarray, c: np.ndarray
) -> np.ndarray:
    """Vectorized Ericson closest-point query for one point vs many triangles."""
    count = len(a)
    out = np.empty((count, 3), dtype=np.float64)
    done = np.zeros(count, dtype=bool)
    ab = b - a
    ac = c - a
    ap = p - a
    d1 = np.einsum("ij,ij->i", ab, ap)
    d2 = np.einsum("ij,ij->i", ac, ap)

    mask = (d1 <= 0.0) & (d2 <= 0.0)
    out[mask] = a[mask]
    done |= mask

    bp = p - b
    d3 = np.einsum("ij,ij->i", ab, bp)
    d4 = np.einsum("ij,ij->i", ac, bp)
    mask = (~done) & (d3 >= 0.0) & (d4 <= d3)
    out[mask] = b[mask]
    done |= mask

    vc = d1 * d4 - d3 * d2
    mask = (~done) & (vc <= 0.0) & (d1 >= 0.0) & (d3 <= 0.0)
    denom = d1 - d3
    v = np.zeros(count)
    v[mask] = d1[mask] / np.maximum(denom[mask], 1.0e-30)
    out[mask] = a[mask] + ab[mask] * v[mask, None]
    done |= mask

    cp = p - c
    d5 = np.einsum("ij,ij->i", ab, cp)
    d6 = np.einsum("ij,ij->i", ac, cp)
    mask = (~done) & (d6 >= 0.0) & (d5 <= d6)
    out[mask] = c[mask]
    done |= mask

    vb = d5 * d2 - d1 * d6
    mask = (~done) & (vb <= 0.0) & (d2 >= 0.0) & (d6 <= 0.0)
    denom = d2 - d6
    w = np.zeros(count)
    w[mask] = d2[mask] / np.maximum(denom[mask], 1.0e-30)
    out[mask] = a[mask] + ac[mask] * w[mask, None]
    done |= mask

    va = d3 * d6 - d5 * d4
    mask = (~done) & (va <= 0.0) & ((d4 - d3) >= 0.0) & ((d5 - d6) >= 0.0)
    denom = (d4 - d3) + (d5 - d6)
    w.fill(0.0)
    w[mask] = (d4[mask] - d3[mask]) / np.maximum(denom[mask], 1.0e-30)
    out[mask] = b[mask] + (c[mask] - b[mask]) * w[mask, None]
    done |= mask

    mask = ~done
    denom = va + vb + vc
    inv = np.zeros(count)
    inv[mask] = 1.0 / np.where(
        np.abs(denom[mask]) > 1.0e-30, denom[mask], np.copysign(1.0e-30, denom[mask])
    )
    v = vb * inv
    w = vc * inv
    out[mask] = a[mask] + ab[mask] * v[mask, None] + ac[mask] * w[mask, None]
    return out


class UniformBodyCollider:
    """Deterministic broad phase + exact point-to-triangle closest point."""

    def __init__(self, verts: np.ndarray, tris: np.ndarray, cell_size: float = 4.0):
        self.verts = verts
        self.tris = tris
        self.cell_size = float(cell_size)
        tri_v = verts[tris]
        self.a = tri_v[:, 0]
        self.b = tri_v[:, 1]
        self.c = tri_v[:, 2]
        raw_n = np.cross(self.b - self.a, self.c - self.a)
        lengths = np.linalg.norm(raw_n, axis=1)
        self.normals = raw_n / np.maximum(lengths[:, None], 1.0e-30)
        self.centroids = tri_v.mean(axis=1)
        self.triangle_radius = np.linalg.norm(
            tri_v - self.centroids[:, None, :], axis=2
        ).max(axis=1)
        self.max_triangle_radius = (
            float(self.triangle_radius.max()) if len(self.triangle_radius) else 0.0
        )
        self.centroid_tree = cKDTree(self.centroids) if cKDTree is not None and len(tris) else None
        self.grid: dict[tuple[int, int, int], list[int]] = {}
        lo = np.floor(tri_v.min(axis=1) / self.cell_size).astype(np.int32)
        hi = np.floor(tri_v.max(axis=1) / self.cell_size).astype(np.int32)
        for ti in range(len(tris)):
            for ix in range(int(lo[ti, 0]), int(hi[ti, 0]) + 1):
                for iy in range(int(lo[ti, 1]), int(hi[ti, 1]) + 1):
                    for iz in range(int(lo[ti, 2]), int(hi[ti, 2]) + 1):
                        self.grid.setdefault((ix, iy, iz), []).append(ti)

        if self.grid:
            grid_keys = np.asarray(list(self.grid), dtype=np.int32)
            self.grid_min = grid_keys.min(axis=0)
            self.grid_max = grid_keys.max(axis=0)
        else:
            self.grid_min = np.zeros(3, dtype=np.int32)
            self.grid_max = np.zeros(3, dtype=np.int32)

    def _closest_candidates(self, p: np.ndarray, candidates: np.ndarray):
        if len(candidates) == 0:
            return None
        candidates = np.unique(candidates.astype(np.int32))
        cps = closest_points_triangles(
            p, self.a[candidates], self.b[candidates], self.c[candidates]
        )
        d2 = np.einsum("ij,ij->i", cps - p, cps - p)
        # unique() sorts triangle IDs, so argmin has a deterministic smallest-ID
        # tie break when two surface pieces are exactly equidistant.
        local = int(np.argmin(d2))
        tri = int(candidates[local])
        cp = cps[local]
        n = self.normals[tri]
        dist = float(np.sqrt(d2[local]))
        sign = float(np.dot(p - cp, n))
        return cp, n, (-dist if sign < 0.0 else dist), tri

    def _closest_grid_exact(self, p: np.ndarray):
        """Dependency-free exact fallback using a proven expanding cube.

        A triangle is inserted into every grid cell touched by its AABB.  Once
        the current best distance is no larger than the distance from ``p`` to
        the exterior of the searched cell cube, no unseen triangle can win.
        """
        if not self.grid:
            return None
        center = np.floor(p / self.cell_size).astype(np.int32)
        max_ring = int(
            max(
                np.max(np.abs(center - self.grid_min)),
                np.max(np.abs(center - self.grid_max)),
            )
        )
        found: set[int] = set()
        for ring in range(max_ring + 1):
            for ix in range(int(center[0]) - ring, int(center[0]) + ring + 1):
                for iy in range(int(center[1]) - ring, int(center[1]) + ring + 1):
                    for iz in range(int(center[2]) - ring, int(center[2]) + ring + 1):
                        if max(abs(ix - int(center[0])), abs(iy - int(center[1])), abs(iz - int(center[2]))) != ring:
                            continue
                        found.update(self.grid.get((ix, iy, iz), ()))
            if not found:
                continue
            result = self._closest_candidates(
                p, np.fromiter(sorted(found), dtype=np.int32)
            )
            if result is None:
                continue
            best = abs(float(result[2]))
            cube_lo = (center - ring) * self.cell_size
            cube_hi = (center + ring + 1) * self.cell_size
            exterior_distance = float(np.min(np.minimum(p - cube_lo, cube_hi - p)))
            if best <= exterior_distance + 1.0e-12 or len(found) == len(self.tris):
                return result
        # Defensive exact fallback for a point beyond the populated grid.
        return self._closest_candidates(p, np.arange(len(self.tris), dtype=np.int32))

    def closest(self, p: np.ndarray):
        if self.centroid_tree is None:
            return self._closest_grid_exact(p)
        count = len(self.tris)
        k = min(32, count)
        while True:
            centroid_distance, candidates = self.centroid_tree.query(p, k=k)
            centroid_distance = np.atleast_1d(centroid_distance)
            candidates = np.atleast_1d(candidates).astype(np.int32)
            result = self._closest_candidates(p, candidates)
            if result is None:
                return None
            best = abs(float(result[2]))
            # For every unseen triangle: distance(surface,p) >=
            # distance(centroid,p) - radius.  The global maximum radius gives a
            # conservative bound and makes this query globally exact.
            if k >= count or float(centroid_distance[-1]) - self.max_triangle_radius >= best:
                return result
            k = min(k * 2, count)

    def closest_many(
        self, points: np.ndarray
    ) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
        """Exact deterministic closest queries with a vectorized KD broad phase."""
        points = np.asarray(points, dtype=np.float64)
        if points.ndim != 2 or points.shape[1] != 3:
            raise ValueError("closest_many expects an Nx3 point array")
        if not np.all(np.isfinite(points)):
            raise ValueError("closest_many received NaN/Inf")
        count_points = len(points)
        closest = np.full((count_points, 3), np.nan, dtype=np.float64)
        normals = np.full_like(closest, np.nan)
        gaps = np.full(count_points, np.nan, dtype=np.float64)
        triangle_ids = np.full(count_points, -1, dtype=np.int32)
        if not count_points:
            return closest, normals, gaps, triangle_ids
        if self.centroid_tree is None:
            for index, point in enumerate(points):
                result = self._closest_grid_exact(point)
                if result is None:
                    continue
                cp, normal, gap, triangle = result
                closest[index] = cp
                normals[index] = normal
                gaps[index] = gap
                triangle_ids[index] = triangle
            return closest, normals, gaps, triangle_ids

        triangle_count = len(self.tris)
        unresolved = np.arange(count_points, dtype=np.int32)
        k = min(32, triangle_count)
        while len(unresolved):
            centroid_distance, candidates = self.centroid_tree.query(
                points[unresolved], k=k
            )
            centroid_distance = np.asarray(centroid_distance).reshape(len(unresolved), k)
            candidates = np.asarray(candidates, dtype=np.int32).reshape(len(unresolved), k)
            # Stable smallest-triangle-ID tie break, matching closest().
            order = np.argsort(candidates, axis=1, kind="stable")
            candidates = np.take_along_axis(candidates, order, axis=1)
            centroid_distance = np.take_along_axis(
                centroid_distance, order, axis=1
            )
            flat_candidates = candidates.reshape(-1)
            repeated_points = np.repeat(points[unresolved], k, axis=0)
            candidate_points = closest_points_triangles(
                repeated_points,
                self.a[flat_candidates],
                self.b[flat_candidates],
                self.c[flat_candidates],
            ).reshape(len(unresolved), k, 3)
            delta = candidate_points - points[unresolved, None, :]
            distance2 = np.einsum("nki,nki->nk", delta, delta)
            local = np.argmin(distance2, axis=1)
            row = np.arange(len(unresolved))
            best_distance = np.sqrt(distance2[row, local])
            best_triangle = candidates[row, local]
            best_point = candidate_points[row, local]
            # The kth distance must come from the original distance order, not
            # the ID-sorted candidates used for deterministic tie breaking.
            kth_distance = np.max(centroid_distance, axis=1)
            proven = (
                (k >= triangle_count)
                | (kth_distance - self.max_triangle_radius >= best_distance)
            )
            accepted = unresolved[proven]
            accepted_local = np.flatnonzero(proven)
            accepted_tri = best_triangle[accepted_local]
            accepted_point = best_point[accepted_local]
            accepted_normal = self.normals[accepted_tri]
            accepted_delta = points[accepted] - accepted_point
            sign = np.einsum("ij,ij->i", accepted_delta, accepted_normal)
            accepted_distance = best_distance[accepted_local]
            closest[accepted] = accepted_point
            normals[accepted] = accepted_normal
            gaps[accepted] = np.where(sign < 0.0, -accepted_distance, accepted_distance)
            triangle_ids[accepted] = accepted_tri
            unresolved = unresolved[~proven]
            if not len(unresolved):
                break
            if k >= triangle_count:
                raise RuntimeError("closest_many failed to resolve an exact query")
            k = min(k * 2, triangle_count)
        return closest, normals, gaps, triangle_ids

    def project(
        self,
        verts: np.ndarray,
        indices: np.ndarray,
        clearance: float,
        stiffness: float = 1.0,
    ) -> tuple[int, float]:
        moved = 0
        worst = 0.0
        for vi in indices:
            result = self.closest(verts[vi])
            if result is None:
                continue
            cp, n, gap, _tri = result
            if gap >= clearance:
                continue
            target = cp + n * clearance
            delta = (target - verts[vi]) * stiffness
            verts[vi] += delta
            moved += 1
            worst = max(worst, float(np.linalg.norm(delta)))
        return moved, worst

    def gaps(self, verts: np.ndarray, indices: np.ndarray | None = None) -> np.ndarray:
        if indices is None:
            indices = np.arange(len(verts), dtype=np.int32)
        return self.closest_many(verts[indices])[2]


def barycentric_coordinates(point: np.ndarray, triangle: np.ndarray) -> np.ndarray | None:
    """Barycentric coordinates of a point in the plane of one triangle."""
    a, b, c = triangle
    v0 = b - a
    v1 = c - a
    v2 = point - a
    d00 = float(v0 @ v0)
    d01 = float(v0 @ v1)
    d11 = float(v1 @ v1)
    d20 = float(v2 @ v0)
    d21 = float(v2 @ v1)
    denom = d00 * d11 - d01 * d01
    if abs(denom) <= 1.0e-20:
        return None
    v = (d11 * d20 - d01 * d21) / denom
    w = (d00 * d21 - d01 * d20) / denom
    bary = np.array([1.0 - v - w, v, w], dtype=np.float64)
    # Closest points on edges can leave the analytic solve a few ulps outside.
    bary = np.clip(bary, 0.0, 1.0)
    total = float(bary.sum())
    return bary / total if total > 1.0e-20 else None


@dataclass
class BodyContactStats:
    projected: int = 0
    kinematic_violations: int = 0
    worst_correction: float = 0.0
    node_projected: int = 0
    edge_projected: int = 0
    face_projected: int = 0
    worst_kind: str = "none"
    worst_owner: int = -1
    worst_gap: float = 0.0
    worst_floor: float = 0.0


@dataclass(frozen=True)
class BodyContactFloors:
    node: np.ndarray
    edge: np.ndarray
    face: np.ndarray


def build_body_contact_floors(
    authored_verts: np.ndarray,
    final_raw_verts: np.ndarray,
    edges: np.ndarray,
    tris: np.ndarray,
    authored_body: UniformBodyCollider,
    final_body: UniformBodyCollider,
    clearance: float,
) -> BodyContactFloors:
    """Freeze a no-worse unilateral floor for every contact quadrature point.

    Clothing/body datasets intentionally contain inherited intersections.  A
    vertex floor cannot represent an edge midpoint or centroid that already
    crosses the body, so each unique sample receives its own endpoint floor.
    Samples that are clear at both endpoints receive physical ``clearance``;
    inherited crossings may not become deeper, but are not teleported through
    the body during initialization.
    """
    if not np.isfinite(clearance) or clearance < 0.0:
        raise ValueError("contact clearance must be finite and non-negative")

    def endpoint_floor(
        authored_points: np.ndarray, final_points: np.ndarray
    ) -> np.ndarray:
        authored_gap = authored_body.gaps(authored_points)
        final_gap = final_body.gaps(final_points)
        if not np.all(np.isfinite(authored_gap)) or not np.all(np.isfinite(final_gap)):
            raise ValueError("contact floor endpoint query returned NaN/Inf")
        return np.minimum(np.minimum(authored_gap, final_gap), clearance)

    node = endpoint_floor(authored_verts, final_raw_verts)
    edge = endpoint_floor(
        0.5 * (authored_verts[edges[:, 0]] + authored_verts[edges[:, 1]]),
        0.5 * (final_raw_verts[edges[:, 0]] + final_raw_verts[edges[:, 1]]),
    )
    face = endpoint_floor(
        authored_verts[tris].mean(axis=1),
        final_raw_verts[tris].mean(axis=1),
    )
    return BodyContactFloors(node=node, edge=edge, face=face)


def project_weighted_body_sample(
    verts: np.ndarray,
    indices: np.ndarray,
    bary: np.ndarray,
    inv_mass: np.ndarray,
    collider: UniformBodyCollider,
    floor: float,
) -> tuple[bool, bool, float, np.ndarray | None, float]:
    """Project one material sample against the body.

    ``p=sum(bary*x)`` is a node, unique material-edge midpoint, or triangle
    centroid.  The positional correction is distributed with the standard
    mass-weighted constraint gradient, so mixed fixed/mobile samples remain
    solvable without moving their kinematic support nodes.
    """
    if not np.isfinite(floor):
        return False, False, 0.0, None, float("nan")
    denominator = float(np.sum(inv_mass[indices] * bary * bary))
    # Kinematic samples cannot be projected.  Their inherited no-worse status
    # is checked once by the final diagnostics rather than paying a collider
    # query in every solver iteration.
    if denominator <= 1.0e-30:
        return False, False, 0.0, None, float("nan")
    point = np.sum(verts[indices] * bary[:, None], axis=0)
    result = collider.closest(point)
    if result is None:
        return False, False, 0.0, None, float("nan")
    closest, normal, gap, _body_tri = result
    if gap >= floor:
        return False, False, 0.0, normal, float(gap)
    # Use the full closest-point target.  At a body edge/vertex this also
    # contains the tangential component required to land on the offset
    # surface; a scalar normal-depth correction is exact only in a face plane.
    sample_delta = closest + normal * floor - point
    distribution = inv_mass[indices] * bary / denominator
    node_delta = distribution[:, None] * sample_delta
    verts[indices] += node_delta
    return (
        True,
        False,
        float(np.max(np.linalg.norm(node_delta, axis=1))),
        normal,
        float(gap),
    )


def project_cloth_body_samples(
    verts: np.ndarray,
    edges: np.ndarray,
    tris: np.ndarray,
    inv_mass: np.ndarray,
    collider: UniformBodyCollider,
    floors: BodyContactFloors,
    contact_count: np.ndarray | None = None,
) -> BodyContactStats:
    """Project unique nodes, material-edge midpoints, and face centroids.

    Sampling unique material edges avoids applying the same midpoint contact
    twice through adjacent triangles.  Together with one centroid per face it
    covers the four interior samples used by the production face-contact gate.
    """
    stats = BodyContactStats()

    def apply(
        indices: np.ndarray,
        bary: np.ndarray,
        kind: str,
        owner: int,
        floor: float,
    ) -> None:
        moved, kinematic, worst, _normal, gap = project_weighted_body_sample(
            verts, indices, bary, inv_mass, collider, floor
        )
        if kinematic:
            stats.kinematic_violations += 1
        if not moved:
            return
        stats.projected += 1
        if worst > stats.worst_correction:
            stats.worst_correction = worst
            stats.worst_kind = kind
            stats.worst_owner = owner
            stats.worst_gap = gap
            stats.worst_floor = floor
        if kind == "node":
            stats.node_projected += 1
        elif kind == "edge":
            stats.edge_projected += 1
        else:
            stats.face_projected += 1
        if contact_count is not None:
            mobile = indices[inv_mass[indices] > 0.0]
            contact_count[mobile] += 1

    edge_bary = np.asarray([0.5, 0.5], dtype=np.float64)
    face_bary = np.asarray([1.0 / 3.0] * 3, dtype=np.float64)

    # Exact vectorized pre-query: almost all quadrature points are already
    # outside their frozen floor, so only the small violating set needs a
    # sequential GS re-query/correction.
    node_ids = np.flatnonzero(inv_mass > 0.0).astype(np.int32)
    edge_ids = np.flatnonzero(
        np.sum(inv_mass[edges] * edge_bary[None, :] ** 2, axis=1) > 1.0e-30
    ).astype(np.int32)
    face_ids = np.flatnonzero(
        np.sum(inv_mass[tris] * face_bary[None, :] ** 2, axis=1) > 1.0e-30
    ).astype(np.int32)
    points = np.concatenate(
        (
            verts[node_ids],
            0.5 * (verts[edges[edge_ids, 0]] + verts[edges[edge_ids, 1]]),
            verts[tris[face_ids]].mean(axis=1),
        ),
        axis=0,
    )
    sample_floors = np.concatenate(
        (floors.node[node_ids], floors.edge[edge_ids], floors.face[face_ids])
    )
    if len(points):
        gap = collider.closest_many(points)[2]
        if not np.all(np.isfinite(gap)):
            raise RuntimeError("body contact pre-query returned NaN/Inf")
        violating = np.flatnonzero(gap < sample_floors).astype(np.int32)
    else:
        violating = np.empty(0, dtype=np.int32)
    node_end = len(node_ids)
    edge_end = node_end + len(edge_ids)
    for sample_id in violating:
        sid = int(sample_id)
        if sid < node_end:
            owner = int(node_ids[sid])
            apply(
                np.asarray([owner], dtype=np.int32),
                np.asarray([1.0]),
                "node",
                owner,
                float(floors.node[owner]),
            )
        elif sid < edge_end:
            owner = int(edge_ids[sid - node_end])
            apply(
                edges[owner], edge_bary, "edge", owner,
                float(floors.edge[owner]),
            )
        else:
            owner = int(face_ids[sid - edge_end])
            apply(
                tris[owner], face_bary, "face", owner,
                float(floors.face[owner]),
            )
    return stats


def body_surface_velocity(
    collider: UniformBodyCollider,
    body_velocity: np.ndarray,
    closest_point: np.ndarray,
    body_triangle: int,
) -> np.ndarray:
    bary = barycentric_coordinates(
        closest_point, collider.verts[collider.tris[body_triangle]]
    )
    if bary is None:
        return body_velocity[collider.tris[body_triangle]].mean(axis=0)
    return np.sum(
        body_velocity[collider.tris[body_triangle]] * bary[:, None], axis=0
    )


def resolve_weighted_contact_velocity(
    verts: np.ndarray,
    velocity: np.ndarray,
    indices: np.ndarray,
    bary: np.ndarray,
    inv_mass: np.ndarray,
    collider: UniformBodyCollider,
    body_velocity: np.ndarray,
    floor: float,
    manifold_tolerance: float,
) -> bool:
    """Remove only inward velocity relative to the moving body surface."""
    if not np.isfinite(floor):
        return False
    denominator = float(np.sum(inv_mass[indices] * bary * bary))
    if denominator <= 1.0e-30:
        return False
    point = np.sum(verts[indices] * bary[:, None], axis=0)
    result = collider.closest(point)
    if result is None:
        return False
    closest, normal, gap, body_tri = result
    if gap > floor + manifold_tolerance:
        return False
    cloth_velocity = np.sum(velocity[indices] * bary[:, None], axis=0)
    surface_velocity = body_surface_velocity(
        collider, body_velocity, closest, body_tri
    )
    relative_normal_speed = float(np.dot(cloth_velocity - surface_velocity, normal))
    if relative_normal_speed >= 0.0:
        return False
    impulse = -relative_normal_speed / denominator
    velocity[indices] += (
        inv_mass[indices] * bary * impulse
    )[:, None] * normal
    return True


def resolve_cloth_body_velocities(
    verts: np.ndarray,
    velocity: np.ndarray,
    edges: np.ndarray,
    tris: np.ndarray,
    inv_mass: np.ndarray,
    collider: UniformBodyCollider,
    body_velocity: np.ndarray,
    floors: BodyContactFloors,
    manifold_tolerance: float,
) -> int:
    resolved = 0

    def apply(indices: np.ndarray, bary: np.ndarray, floor: float) -> None:
        nonlocal resolved
        resolved += int(
            resolve_weighted_contact_velocity(
                verts, velocity, indices, bary, inv_mass, collider,
                body_velocity, floor, manifold_tolerance,
            )
        )

    edge_bary = np.asarray([0.5, 0.5])
    face_bary = np.asarray([1.0 / 3.0] * 3)
    node_ids = np.flatnonzero(inv_mass > 0.0).astype(np.int32)
    edge_ids = np.flatnonzero(
        np.sum(inv_mass[edges] * edge_bary[None, :] ** 2, axis=1) > 1.0e-30
    ).astype(np.int32)
    face_ids = np.flatnonzero(
        np.sum(inv_mass[tris] * face_bary[None, :] ** 2, axis=1) > 1.0e-30
    ).astype(np.int32)
    points = np.concatenate(
        (
            verts[node_ids],
            0.5 * (verts[edges[edge_ids, 0]] + verts[edges[edge_ids, 1]]),
            verts[tris[face_ids]].mean(axis=1),
        ),
        axis=0,
    )
    sample_floors = np.concatenate(
        (floors.node[node_ids], floors.edge[edge_ids], floors.face[face_ids])
    )
    gap = collider.closest_many(points)[2] if len(points) else np.empty(0)
    manifold = np.flatnonzero(
        gap <= sample_floors + manifold_tolerance
    ).astype(np.int32)
    node_end = len(node_ids)
    edge_end = node_end + len(edge_ids)
    for sample_id in manifold:
        sid = int(sample_id)
        if sid < node_end:
            owner = int(node_ids[sid])
            apply(
                np.asarray([owner], dtype=np.int32), np.asarray([1.0]),
                float(floors.node[owner]),
            )
        elif sid < edge_end:
            owner = int(edge_ids[sid - node_end])
            apply(edges[owner], edge_bary, float(floors.edge[owner]))
        else:
            owner = int(face_ids[sid - edge_end])
            apply(tris[owner], face_bary, float(floors.face[owner]))
    return resolved


def cloth_body_sample_diagnostics(
    verts: np.ndarray,
    edges: np.ndarray,
    tris: np.ndarray,
    inv_mass: np.ndarray,
    collider: UniformBodyCollider,
    floors: BodyContactFloors,
    tolerance: float = 1.0e-6,
) -> dict[str, float | int]:
    """Measure node plus unique edge/face interior contact constraints."""
    edge_bary = np.asarray([0.5, 0.5])
    face_bary = np.asarray([1.0 / 3.0] * 3)
    rows: dict[str, tuple[np.ndarray, np.ndarray, np.ndarray]] = {
        "node": (
            collider.closest_many(verts)[2],
            floors.node,
            inv_mass > 0.0,
        ),
        "edge": (
            collider.closest_many(
                0.5 * (verts[edges[:, 0]] + verts[edges[:, 1]])
            )[2],
            floors.edge,
            np.sum(
                inv_mass[edges] * edge_bary[None, :] ** 2, axis=1
            ) > 1.0e-30,
        ),
        "face": (
            collider.closest_many(verts[tris].mean(axis=1))[2],
            floors.face,
            np.sum(
                inv_mass[tris] * face_bary[None, :] ** 2, axis=1
            ) > 1.0e-30,
        ),
    }

    report: dict[str, float | int] = {}
    for kind, (gap, floor, movable) in rows.items():
        finite = np.isfinite(gap) & np.isfinite(floor)
        violation = finite & (gap < floor - tolerance)
        report[f"{kind}_samples"] = len(gap)
        report[f"{kind}_nonfinite"] = int((~finite).sum())
        report[f"{kind}_movable_violations"] = int((violation & movable).sum())
        report[f"{kind}_kinematic_violations"] = int((violation & ~movable).sum())
        report[f"{kind}_min_gap"] = (
            float(np.min(gap[np.isfinite(gap)]))
            if np.any(np.isfinite(gap)) else float("nan")
        )
    return report


def body_contact_selftest() -> None:
    body = np.asarray(
        [[-10.0, -10.0, 0.0], [10.0, -10.0, 0.0], [0.0, 10.0, 0.0]],
        dtype=np.float64,
    )
    body_tris = np.asarray([[0, 1, 2]], dtype=np.int32)
    collider = UniformBodyCollider(body, body_tris)
    cloth = np.asarray(
        [[-0.6, 0.0, -0.3], [0.4, 0.0, -0.3], [0.0, 1.0, -0.3]],
        dtype=np.float64,
    )
    indices = np.asarray([0, 1, 2], dtype=np.int32)
    bary = np.asarray([0.2, 0.3, 0.5], dtype=np.float64)
    inv_mass = np.asarray([1.0, 2.0, 0.0], dtype=np.float64)
    fixed_before = cloth[2].copy()
    moved, kinematic, _worst, _normal, _gap = project_weighted_body_sample(
        cloth, indices, bary, inv_mass, collider, floor=0.1
    )
    sample = np.sum(cloth * bary[:, None], axis=0)
    if not moved or kinematic or abs(float(sample[2]) - 0.1) > 1.0e-10:
        raise RuntimeError(f"weighted contact projection failed: sample={sample}")
    if not np.array_equal(cloth[2], fixed_before):
        raise RuntimeError("weighted contact moved a zero-inverse-mass node")

    immobile = cloth.copy()
    moved, kinematic, _worst, _normal, _gap = project_weighted_body_sample(
        immobile, indices, bary, np.zeros(3), collider, floor=0.2
    )
    if moved or kinematic or not np.array_equal(immobile, cloth):
        raise RuntimeError("all-fixed contact projection did not remain inert")

    velocity = np.zeros_like(cloth)
    body_velocity = np.zeros_like(body)
    body_velocity[:, 2] = 2.0
    resolved = resolve_weighted_contact_velocity(
        cloth, velocity, indices, bary, inv_mass, collider,
        body_velocity, floor=0.1, manifold_tolerance=1.0e-9,
    )
    sample_velocity = np.sum(velocity * bary[:, None], axis=0)
    if not resolved or abs(float(sample_velocity[2]) - 2.0) > 1.0e-10:
        raise RuntimeError(
            "moving-body relative contact velocity failed: "
            f"sampleVelocity={sample_velocity}"
        )
    if not np.array_equal(velocity[2], np.zeros(3)):
        raise RuntimeError("velocity contact moved a kinematic node")
    print("cloth/body sample contact selftest: PASS")


def build_surface_attachments(
    cloth_base: np.ndarray,
    requested: np.ndarray,
    body_base: np.ndarray,
    body_tris: np.ndarray,
    collider: UniformBodyCollider,
) -> SurfaceAttachments:
    """Bind requested cloth vertices to exact closest reference-body material."""
    count = len(cloth_base)
    valid = np.zeros(count, dtype=bool)
    triangle = np.full(count, -1, dtype=np.int32)
    barycentric = np.zeros((count, 3), dtype=np.float64)
    authored_offset = np.zeros((count, 3), dtype=np.float64)
    authored_distance = np.full(count, np.nan, dtype=np.float64)
    for vi in np.flatnonzero(requested):
        closest = collider.closest(cloth_base[vi])
        if closest is None:
            continue
        point, _normal, _gap, triangle_index = closest
        bary = barycentric_coordinates(point, body_base[body_tris[triangle_index]])
        if bary is None:
            continue
        valid[vi] = True
        triangle[vi] = triangle_index
        barycentric[vi] = bary
        authored_offset[vi] = cloth_base[vi] - point
        authored_distance[vi] = float(np.linalg.norm(authored_offset[vi]))
    missing = np.flatnonzero(np.logical_and(requested, ~valid))
    if len(missing):
        raise RuntimeError(
            f"surface attachment failed for {len(missing)} requested vertices; "
            f"first={missing[:8].tolist()}"
        )
    return SurfaceAttachments(
        requested=requested.copy(),
        valid=valid,
        triangle=triangle,
        barycentric=barycentric,
        authored_offset=authored_offset,
        authored_distance=authored_distance,
    )


def triangle_corot_rotation(reference: np.ndarray, current: np.ndarray) -> np.ndarray:
    """Proper Kabsch rotation of a material body triangle (stretch/shear removed)."""
    reference_centered = reference - reference.mean(axis=0)
    current_centered = current - current.mean(axis=0)
    covariance = reference_centered.T @ current_centered
    u, _singular, vt = np.linalg.svd(covariance)
    rotation = vt.T @ u.T
    if np.linalg.det(rotation) < 0.0:
        vt[-1] *= -1.0
        rotation = vt.T @ u.T
    if not np.isfinite(rotation).all() or np.linalg.det(rotation) <= 0.0:
        raise RuntimeError("invalid co-rotational body-triangle frame")
    return rotation


def transport_surface_attachments(
    attachments: SurfaceAttachments,
    body_reference: np.ndarray,
    body_current: np.ndarray,
    body_tris: np.ndarray,
    fallback_target: np.ndarray,
    authored_target: np.ndarray | None = None,
) -> np.ndarray:
    """Transport selected supports while preserving the requested fallback.

    On a static body, selected vertices close exactly to ``authored_target``;
    unselected vertices remain on ``fallback_target``.  Keeping those fields
    separate is essential for S04's paired static control.
    """
    if authored_target is None:
        authored_target = fallback_target
    target = fallback_target.copy()
    indices = np.flatnonzero(attachments.valid)
    # This is both an invariant and a numerical guard: a static body must make
    # surface-corot exactly identical to authored, not merely close enough for
    # an equidistant nearest-face tie to change contact sign.
    if np.array_equal(body_reference, body_current):
        target[indices] = authored_target[indices]
        return target
    if len(indices) == 0:
        return target
    triangles = attachments.triangle[indices]
    current_triangles = body_current[body_tris[triangles]]
    support_points = np.sum(
        attachments.barycentric[indices, :, None] * current_triangles,
        axis=1,
    )
    for triangle_index in np.unique(triangles):
        local = np.flatnonzero(triangles == triangle_index)
        rotation = triangle_corot_rotation(
            body_reference[body_tris[triangle_index]],
            body_current[body_tris[triangle_index]],
        )
        cloth_indices = indices[local]
        target[cloth_indices] = support_points[local] + (
            attachments.authored_offset[cloth_indices] @ rotation.T
        )
    return target


def clamp_targets_to_contact_floor(
    targets: np.ndarray,
    indices: np.ndarray,
    collider: UniformBodyCollider,
    contact_floor: np.ndarray,
    tolerance: float = 1.0e-6,
) -> tuple[int, float]:
    """Keep kinematic support targets from worsening authored body contact."""
    moved = 0
    worst = 0.0
    for vi in indices:
        result = collider.closest(targets[vi])
        if result is None:
            raise RuntimeError(f"no body surface found while clamping support vertex {vi}")
        point, normal, gap, _triangle = result
        floor = float(contact_floor[vi])
        if gap >= floor - tolerance:
            continue
        corrected = point + normal * floor
        delta = corrected - targets[vi]
        targets[vi] = corrected
        moved += 1
        worst = max(worst, float(np.linalg.norm(delta)))
    return moved, worst


def attachment_selftest() -> None:
    """Rigid-motion and authored-closure tests for surface-corot bindings."""
    body = np.array(
        [[-2.0, -1.0, 0.0], [2.0, -1.0, 0.0], [0.0, 2.0, 0.0]],
        dtype=np.float64,
    )
    tris = np.array([[0, 1, 2]], dtype=np.int32)
    cloth = np.array(
        [[0.0, 0.0, 0.5], [0.8, -0.2, 1.2], [-0.7, 0.4, 0.2]],
        dtype=np.float64,
    )
    requested = np.ones(len(cloth), dtype=bool)
    collider = UniformBodyCollider(body, tris)
    bindings = build_surface_attachments(cloth, requested, body, tris, collider)
    closure = transport_surface_attachments(bindings, body, body, tris, cloth, cloth)
    closure_error = float(np.linalg.norm(closure - cloth, axis=1).max())
    if closure_error > 1.0e-9:
        raise RuntimeError(f"surface-corot t=0 closure failed: {closure_error}")
    authored_gap = collider.gaps(cloth)
    clamped = closure.copy()
    clamp_count, clamp_worst = clamp_targets_to_contact_floor(
        clamped,
        np.arange(len(cloth), dtype=np.int32),
        collider,
        np.minimum(authored_gap, 0.02),
    )
    if clamp_count != 0 or clamp_worst != 0.0 or not np.array_equal(clamped, cloth):
        raise RuntimeError(
            f"surface-corot static clamp is not a no-op: count={clamp_count} "
            f"worst={clamp_worst}"
        )

    angle = np.deg2rad(37.0)
    rotation = np.array(
        [[np.cos(angle), -np.sin(angle), 0.0],
         [np.sin(angle), np.cos(angle), 0.0],
         [0.0, 0.0, 1.0]],
        dtype=np.float64,
    )
    translation = np.array([4.0, -3.0, 2.5], dtype=np.float64)
    moved_body = body @ rotation.T + translation
    transported = transport_surface_attachments(
        bindings, body, moved_body, tris, cloth, cloth
    )
    expected = cloth @ rotation.T + translation
    rigid_error = float(np.linalg.norm(transported - expected, axis=1).max())
    if rigid_error > 1.0e-9:
        raise RuntimeError(f"surface-corot rigid-motion test failed: {rigid_error}")
    partial_requested = np.array([True, False, True], dtype=bool)
    partial = build_surface_attachments(
        cloth, partial_requested, body, tris, collider
    )
    displaced_fallback = cloth + np.array([7.0, 0.0, 0.0])
    partial_static = transport_surface_attachments(
        partial, body, body, tris, displaced_fallback, cloth
    )
    if not np.array_equal(partial_static[partial_requested], cloth[partial_requested]):
        raise RuntimeError("surface-corot static selected vertices missed authored closure")
    if not np.array_equal(
        partial_static[~partial_requested], displaced_fallback[~partial_requested]
    ):
        raise RuntimeError("surface-corot static fallback leaked outside selected mask")
    print(
        f"attachment selftest PASS closure={closure_error:.3g} "
        f"rigidMotion={rigid_error:.3g} staticClamp=no-op partialFallback=isolated "
        f"valid={int(bindings.valid.sum())}"
    )


def attachment_report(capture_path: Path) -> None:
    """Report the inferred support population without running the solver."""
    cap = read_capture_front(capture_path)
    drape = fitted_drape_signal(cap)
    inverse_mass = np.clip((drape - 0.05) / 0.50, 0.0, 1.0)
    inverse_mass = inverse_mass * inverse_mass * (3.0 - 2.0 * inverse_mass)
    fixed = inverse_mass <= 1.0e-12
    soft_anchor = 0.18 * (1.0 - inverse_mass) ** 2
    requested = np.logical_or(fixed, soft_anchor > 0.0)
    collider = UniformBodyCollider(cap.ref_body_verts, cap.ref_body_tris)
    bindings = build_surface_attachments(
        cap.base_verts,
        requested,
        cap.ref_body_verts,
        cap.ref_body_tris,
        collider,
    )
    distances = bindings.authored_distance[bindings.valid]
    print(
        f"attachment report shape={cap.shape_name!r} verts={len(cap.base_verts)} "
        f"fixed={int(fixed.sum())} requested={int(requested.sum())} "
        f"valid={int(bindings.valid.sum())} "
        f"uniqueBodyTris={len(np.unique(bindings.triangle[bindings.valid]))} "
        f"distance[min/p50/p95/max]={distances.min():.3f}/"
        f"{np.percentile(distances,50):.3f}/{np.percentile(distances,95):.3f}/"
        f"{distances.max():.3f} gt4={(distances > 4.0).sum()} "
        f"gt8={(distances > 8.0).sum()}"
    )


def load_surface_corot_mask(
    artifact_path: Path,
    classifier: str,
    capture_path: Path,
    vertex_count: int,
    eligible: np.ndarray,
    *,
    surface_corot_fallback: str,
    surface_corot_clamp: bool,
    body_mode: str,
    soft_anchor_mode: str,
    morph_steps: int,
    clearance: float,
) -> np.ndarray:
    """Load an offline-preflighted support mask, fail-closed on provenance.

    The classifier itself lives in the S04 measurement runner.  The solve uses
    its frozen mask so the field tested by the gate is exactly the field that
    passed preflight, without duplicating or subtly drifting the classifier.
    """
    data = json.loads(artifact_path.read_text(encoding="utf-8"))
    if data.get("schema") != "pathb-support-preflight-v2":
        raise ValueError(f"{artifact_path}: unsupported support-preflight schema")
    if data.get("all_coherent_v1_gates_pass") is not True:
        raise ValueError(f"{artifact_path}: coherent-v1 global gate is not PASS")
    contract = data.get("solve_contract")
    if not isinstance(contract, dict):
        raise ValueError(f"{artifact_path}: missing solve_contract")
    if contract.get("anchor_target") != "surface-corot":
        raise ValueError(f"{artifact_path}: invalid anchor_target contract")
    expected_mode = (
        contract.get("candidate_body_mode")
        if body_mode == "continuation"
        else contract.get("static_control_body_mode")
        if body_mode == "static"
        else None
    )
    contract_mismatches = []
    if expected_mode != body_mode:
        contract_mismatches.append(f"body_mode={body_mode!r}")
    if contract.get("surface_corot_fallback") != surface_corot_fallback:
        contract_mismatches.append(
            f"fallback={surface_corot_fallback!r}"
        )
    if contract.get("surface_corot_clamp") is not surface_corot_clamp:
        contract_mismatches.append(f"clamp={surface_corot_clamp!r}")
    if contract.get("morph_steps") != morph_steps:
        contract_mismatches.append(f"morph_steps={morph_steps}")
    if contract.get("soft_anchor") != soft_anchor_mode:
        contract_mismatches.append(f"soft_anchor={soft_anchor_mode!r}")
    try:
        recorded_clearance = float(contract.get("clearance"))
    except (TypeError, ValueError):
        recorded_clearance = float("nan")
    if not np.isfinite(recorded_clearance) or abs(recorded_clearance - clearance) > 1.0e-12:
        contract_mismatches.append(f"clearance={clearance!r}")
    if contract_mismatches:
        raise ValueError(
            f"{artifact_path}: solve contract mismatch: "
            + ", ".join(contract_mismatches)
        )

    recorded_tools = data.get("tools")
    current_tools = {
        "pathb_support_classifier.py": REPO_ROOT / "tools/pathb_support_classifier.py",
        "pathb_support_preflight.py": REPO_ROOT / "tools/pathb_support_preflight.py",
        "shell_spike.py": Path(__file__).resolve(),
    }
    if not isinstance(recorded_tools, dict):
        raise ValueError(f"{artifact_path}: missing tool provenance")
    for name, path in current_tools.items():
        if not path.is_file() or recorded_tools.get(name) != sha256(path):
            raise ValueError(f"{artifact_path}: stale tool hash for {name}")
    recorded_environment = data.get("environment")
    current_environment = {
        "python": platform.python_version(),
        "numpy": np.__version__,
        "scipy": scipy.__version__ if scipy is not None else None,
    }
    if recorded_environment != current_environment:
        raise ValueError(
            f"{artifact_path}: runtime environment differs from preflight"
        )
    raw_entries = data.get("garments")
    if isinstance(raw_entries, dict):
        entries = list(raw_entries.values())
    elif isinstance(raw_entries, list):
        entries = raw_entries
    else:
        raise ValueError(f"{artifact_path}: missing garments list/map")

    capture_digest = sha256(capture_path)
    matches = []
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        entry_digest = entry.get("capture_sha256")
        if entry_digest is None and isinstance(entry.get("capture"), dict):
            entry_digest = entry["capture"].get("sha256")
        if entry_digest == capture_digest:
            matches.append(entry)
    if len(matches) != 1:
        raise ValueError(
            f"{artifact_path}: expected one garment for capture {capture_digest}, "
            f"found {len(matches)}"
        )
    entry = matches[0]
    recorded_count = int(entry.get("vertex_count", entry.get("counts", {}).get("vertices", -1)))
    if recorded_count != vertex_count:
        raise ValueError(
            f"{artifact_path}: vertex count {recorded_count} != capture {vertex_count}"
        )
    classifiers = entry.get("classifiers")
    if not isinstance(classifiers, dict) or classifier not in classifiers:
        raise ValueError(f"{artifact_path}: classifier {classifier!r} not found")
    record = classifiers[classifier]
    if not isinstance(record, dict):
        raise ValueError(f"{artifact_path}: malformed classifier {classifier!r}")
    if record.get("approved_for_solve") is not True:
        raise ValueError(
            f"{artifact_path}: classifier {classifier!r} is not approved for solve"
        )
    diagnostics = entry.get("diagnostics")
    if (
        not isinstance(diagnostics, dict)
        or diagnostics.get("coherent_v1_gate_pass") is not True
    ):
        raise ValueError(f"{artifact_path}: garment classifier gate is not PASS")
    if record.get("fallback") != surface_corot_fallback:
        raise ValueError(f"{artifact_path}: classifier fallback does not match solve")
    if record.get("surface_corot_clamp") is not surface_corot_clamp:
        raise ValueError(f"{artifact_path}: classifier clamp does not match solve")
    if record.get("morph_steps") != morph_steps:
        raise ValueError(f"{artifact_path}: classifier morph_steps does not match solve")
    if record.get("soft_anchor") != soft_anchor_mode:
        raise ValueError(f"{artifact_path}: classifier soft_anchor does not match solve")
    ids = record.get("selected_vertex_ids")
    if (
        not isinstance(ids, list)
        or not ids
        or any(type(vertex_id) is not int for vertex_id in ids)
        or ids != sorted(ids)
    ):
        raise ValueError(
            f"{artifact_path}: classifier {classifier!r} has invalid selected_vertex_ids"
        )
    selected = np.asarray(ids, dtype=np.int64)
    if len(selected) != len(np.unique(selected)):
        raise ValueError(f"{artifact_path}: duplicate support vertex IDs")
    if len(selected) and (int(selected.min()) < 0 or int(selected.max()) >= vertex_count):
        raise ValueError(f"{artifact_path}: support vertex ID out of range")
    mask = np.zeros(vertex_count, dtype=bool)
    mask[selected] = True
    if record.get("selected_count") != int(mask.sum()):
        raise ValueError(f"{artifact_path}: selected_count does not match mask")
    mask_digest = hashlib.sha256(mask.astype(np.uint8).tobytes()).hexdigest()
    if record.get("mask_u8_sha256") != mask_digest:
        raise ValueError(f"{artifact_path}: mask_u8_sha256 does not match mask")
    outside = np.flatnonzero(np.logical_and(mask, ~eligible))
    if len(outside):
        raise ValueError(
            f"{artifact_path}: {len(outside)} selected vertices are outside fixed|soft; "
            f"first={outside[:8].tolist()}"
        )
    return mask


class GravityShadow:
    """Downward sweep of body support, ported from the production prototype."""

    NEG = -1.0e30

    def __init__(
        self,
        verts: np.ndarray,
        tris: np.ndarray,
        cell_size: float,
        ceiling: float,
        triangle_weights: np.ndarray | None = None,
    ):
        if cell_size <= 1.0e-3:
            raise ValueError("shadow cell size must be positive")
        self.cell = float(cell_size)
        self.inv_cell = 1.0 / self.cell
        self.min_xy = verts[:, :2].min(axis=0) - self.cell
        max_xy = verts[:, :2].max(axis=0) + self.cell
        dims = np.ceil((max_xy - self.min_xy) / self.cell).astype(np.int32) + 1
        self.nx, self.ny = int(dims[0]), int(dims[1])
        if self.nx <= 1 or self.ny <= 1 or self.nx * self.ny > 4_000_000:
            raise ValueError(f"invalid shadow grid {self.nx}x{self.ny}")
        self.top_z = np.full((self.ny, self.nx), self.NEG, dtype=np.float64)
        self.top_weight = np.zeros((self.ny, self.nx), dtype=np.float64)

        tri_indices = (
            np.flatnonzero(triangle_weights > 0.0).astype(np.int32)
            if triangle_weights is not None
            else np.arange(len(tris), dtype=np.int32)
        )
        for ti in tri_indices:
            tri_weight = 1.0 if triangle_weights is None else float(triangle_weights[ti])
            a, b, c = verts[tris[ti]]
            tri_lo = np.minimum(np.minimum(a[:2], b[:2]), c[:2])
            tri_hi = np.maximum(np.maximum(a[:2], b[:2]), c[:2])
            i0, j0 = self.cell_of_xy(tri_lo)
            i1, j1 = self.cell_of_xy(tri_hi)
            i0, i1 = max(0, i0), min(self.nx - 1, i1)
            j0, j1 = max(0, j0), min(self.ny - 1, j1)
            d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
            if abs(float(d)) < 1.0e-9:
                for v in (a, b, c):
                    if v[2] > ceiling:
                        continue
                    i, j = self.cell_of_xy(v[:2])
                    if self.in_grid(i, j):
                        z = float(v[2])
                        if z > self.top_z[j, i] + 1.0e-9:
                            self.top_z[j, i] = z
                            self.top_weight[j, i] = tri_weight
                        elif abs(z - self.top_z[j, i]) <= 1.0e-9:
                            self.top_weight[j, i] = max(self.top_weight[j, i], tri_weight)
                continue
            inv_d = 1.0 / float(d)
            for j in range(j0, j1 + 1):
                py = self.min_xy[1] + (j + 0.5) * self.cell
                for i in range(i0, i1 + 1):
                    px = self.min_xy[0] + (i + 0.5) * self.cell
                    l0 = ((b[1] - c[1]) * (px - c[0]) + (c[0] - b[0]) * (py - c[1])) * inv_d
                    l1 = ((c[1] - a[1]) * (px - c[0]) + (a[0] - c[0]) * (py - c[1])) * inv_d
                    l2 = 1.0 - l0 - l1
                    if l0 < -1.0e-4 or l1 < -1.0e-4 or l2 < -1.0e-4:
                        continue
                    z = l0 * a[2] + l1 * b[2] + l2 * c[2]
                    if z <= ceiling and z > self.top_z[j, i] + 1.0e-9:
                        self.top_z[j, i] = z
                        self.top_weight[j, i] = tri_weight
                    elif z <= ceiling and abs(z - self.top_z[j, i]) <= 1.0e-9:
                        self.top_weight[j, i] = max(self.top_weight[j, i], tri_weight)

    def cell_of_xy(self, xy: np.ndarray) -> tuple[int, int]:
        c = np.floor((xy - self.min_xy) * self.inv_cell).astype(np.int32)
        return int(c[0]), int(c[1])

    def in_grid(self, i: int, j: int) -> bool:
        return 0 <= i < self.nx and 0 <= j < self.ny

    def top_at(self, i: int, j: int) -> float:
        return float(self.top_z[j, i]) if self.in_grid(i, j) else self.NEG

    def push_target(
        self,
        p: np.ndarray,
        clearance: float,
        max_ring: int,
        ref_center: np.ndarray,
        search_mode: str,
    ) -> tuple[np.ndarray, float] | None:
        ci, cj = self.cell_of_xy(p[:2])
        if not self.in_grid(ci, cj):
            return None
        if self.top_at(ci, cj) < p[2]:
            return None
        current_r = float(np.linalg.norm(p[:2] - ref_center))
        if search_mode == "radial":
            radial = p[:2] - ref_center
            radial_len = float(np.linalg.norm(radial))
            if radial_len < 1.0e-8:
                return None
            direction = radial / radial_len
            half_steps = max(1, 2 * max_ring)
            for step in range(1, half_steps + 1):
                distance = step * 0.5 * self.cell
                sample = p[:2] + direction * distance
                i, j = self.cell_of_xy(sample)
                if not self.in_grid(i, j):
                    return None
                if self.top_at(i, j) >= p[2]:
                    continue
                target_distance = max(distance - 0.5 * self.cell, 0.0) + clearance
                return (
                    p[:2] + direction * target_distance,
                    float(self.top_weight[cj, ci]),
                )
            return None
        if search_mode != "nearest":
            raise ValueError(f"unknown shadow search mode: {search_mode}")
        for ring in range(1, max_ring + 1):
            best = None
            for dj in range(-ring, ring + 1):
                for di in range(-ring, ring + 1):
                    if max(abs(di), abs(dj)) != ring:
                        continue
                    i, j = ci + di, cj + dj
                    if not self.in_grid(i, j) or self.top_at(i, j) >= p[2]:
                        continue
                    xy = self.min_xy + (np.array([i, j], dtype=np.float64) + 0.5) * self.cell
                    d2 = float(np.dot(xy - p[:2], xy - p[:2]))
                    key = (d2, j, i)
                    if best is None or key < best[0]:
                        best = (key, xy)
            if best is None:
                continue
            clear_xy = best[1]
            direction = clear_xy - p[:2]
            length = float(np.linalg.norm(direction))
            if length < 1.0e-8:
                return None
            target_distance = max(length - 0.5 * self.cell, 0.0) + clearance
            target_xy = p[:2] + direction / length * target_distance
            # The shadow is an outward-only obstacle. Nearest-clear searches on
            # asymmetric grids may point sideways/inward; those are not support.
            target_r = float(np.linalg.norm(target_xy - ref_center))
            if target_r <= current_r + 1.0e-8:
                return None
            return target_xy, float(self.top_weight[cj, ci])
        return None


def apply_graph_plumb_constraint(
    x: np.ndarray,
    active: np.ndarray,
    adjacency: list[list[int]],
    ref_center: np.ndarray,
    return_rate: float,
    strength: float,
    support: np.ndarray | None = None,
    material_rest_positions: np.ndarray | None = None,
) -> tuple[int, float]:
    """Propagate a unilateral hang line downward through garment topology.

    The constraint is axis-free around the authored body center. An active
    vertex may be pushed outward to the radius carried by an adjacent vertex
    above it, minus optional taper, but is never pulled inward.
    """
    if strength <= 0.0 or len(active) == 0:
        return 0, 0.0
    radii = np.linalg.norm(x[:, :2] - ref_center[None, :], axis=1)
    order = sorted((int(i) for i in active), key=lambda i: (-float(x[i, 2]), i))
    moved = 0
    worst = 0.0
    for vi in order:
        current_r = float(radii[vi])
        if current_r < 1.0e-8:
            continue
        target_r = current_r
        target_support = 1.0 if support is None else float(support[vi])
        propagated_support = target_support
        zi = float(x[vi, 2])
        for nb in adjacency[vi]:
            dz = float(x[nb, 2]) - zi
            if dz <= 1.0e-4:
                continue
            parent_support = 1.0 if support is None else float(support[nb])
            if parent_support <= 0.0:
                continue
            if material_rest_positions is not None:
                rest_edge = float(
                    np.linalg.norm(material_rest_positions[nb] - material_rest_positions[vi])
                )
                current_edge = float(np.linalg.norm(x[nb] - x[vi]))
                if rest_edge <= 1.0e-8 or current_edge <= rest_edge:
                    continue
                edge_tension = min(1.0, 1.0 - rest_edge / current_edge)
                parent_support = min(parent_support, edge_tension)
            propagated_support = max(propagated_support, parent_support)
            candidate = float(radii[nb]) - return_rate * dz
            if candidate > target_r:
                target_r = candidate
                target_support = parent_support
        if support is not None:
            support[vi] = max(float(support[vi]), propagated_support)
        if target_r <= current_r + 1.0e-8:
            continue
        step = (target_r - current_r) * strength * target_support
        direction = (x[vi, :2] - ref_center) / current_r
        x[vi, :2] += direction * step
        radii[vi] = current_r + step
        moved += 1
        worst = max(worst, step)
    return moved, worst


def build_band_circumference_constraints(
    base: np.ndarray,
    rest_positions: np.ndarray,
    edges: np.ndarray,
    mobile: np.ndarray,
    z_cell: float,
) -> tuple[list[np.ndarray], np.ndarray]:
    """Group near-horizontal material edges by component and height band."""
    dsu = DSU(len(base))
    for a, b in edges:
        dsu.union(int(a), int(b))
    z_min = float(base[:, 2].min())
    groups: dict[tuple[int, int], list[int]] = {}
    horizontal_base = np.linalg.norm(
        base[edges[:, 1], :2] - base[edges[:, 0], :2], axis=1
    )
    horizontal_rest = np.linalg.norm(
        rest_positions[edges[:, 1], :2] - rest_positions[edges[:, 0], :2], axis=1
    )
    dz = np.abs(base[edges[:, 1], 2] - base[edges[:, 0], 2])
    for ei, (a, b) in enumerate(edges):
        ai, bi = int(a), int(b)
        h = float(horizontal_base[ei])
        if h <= 1.0e-6 or dz[ei] > max(0.35, 0.75 * h):
            continue
        if mobile[ai] <= 0.0 and mobile[bi] <= 0.0:
            continue
        mid_z = 0.5 * float(base[ai, 2] + base[bi, 2])
        key = (dsu.find(ai), int(np.floor((mid_z - z_min) / z_cell)))
        groups.setdefault(key, []).append(ei)
    selected: list[np.ndarray] = []
    rest: list[float] = []
    for key in sorted(groups):
        edge_ids = np.asarray(groups[key], dtype=np.int32)
        if len(edge_ids) < 3:
            continue
        selected.append(edge_ids)
        rest.append(float(horizontal_rest[edge_ids].sum()))
    return selected, np.asarray(rest, dtype=np.float64)


def xpbd_band_circumference(
    x: np.ndarray,
    edges: np.ndarray,
    groups: list[np.ndarray],
    rest: np.ndarray,
    inv_mass: np.ndarray,
    lambdas: np.ndarray,
    compliance: float,
    relaxation: float,
) -> tuple[int, float]:
    """Unilateral XPBD upper bound on material circumference per height band."""
    active_count = 0
    worst_ratio = 1.0
    for gi, edge_ids in enumerate(groups):
        band_edges = edges[edge_ids]
        delta = x[band_edges[:, 1], :2] - x[band_edges[:, 0], :2]
        lengths = np.linalg.norm(delta, axis=1)
        current = float(lengths.sum())
        target = float(rest[gi])
        if target <= 1.0e-8:
            continue
        worst_ratio = max(worst_ratio, current / target)
        c = current - target
        if c <= 0.0 and lambdas[gi] == 0.0:
            continue
        unit = delta / np.maximum(lengths[:, None], 1.0e-30)
        vertices = np.unique(band_edges.reshape(-1))
        gradients = np.zeros((len(vertices), 2), dtype=np.float64)
        local = {int(vi): li for li, vi in enumerate(vertices)}
        for ei, (a, b) in enumerate(band_edges):
            gradients[local[int(a)]] -= unit[ei]
            gradients[local[int(b)]] += unit[ei]
        weights = inv_mass[vertices]
        denom = float(np.sum(weights * np.einsum("ij,ij->i", gradients, gradients)))
        if denom + compliance <= 1.0e-20:
            continue
        dlambda = (-c - compliance * lambdas[gi]) / (denom + compliance)
        new_lambda = min(0.0, float(lambdas[gi] + dlambda))
        dlambda = (new_lambda - lambdas[gi]) * relaxation
        lambdas[gi] += dlambda
        x[vertices, :2] += weights[:, None] * dlambda * gradients
        if abs(dlambda) > 1.0e-12:
            active_count += 1
    return active_count, worst_ratio


def wrap_angle(a: np.ndarray) -> np.ndarray:
    return (a + np.pi) % (2.0 * np.pi) - np.pi


def signed_dihedrals_local(local: np.ndarray) -> np.ndarray:
    """Signed angle for [edge0, edge1, opposite0, opposite1] hinge positions."""
    p0, p1, p2, p3 = (local[:, i] for i in range(4))
    edge = p1 - p0
    edge /= np.maximum(np.linalg.norm(edge, axis=1, keepdims=True), 1.0e-30)
    n0 = np.cross(p1 - p0, p2 - p0)
    n1 = np.cross(p3 - p0, p1 - p0)
    n0 /= np.maximum(np.linalg.norm(n0, axis=1, keepdims=True), 1.0e-30)
    n1 /= np.maximum(np.linalg.norm(n1, axis=1, keepdims=True), 1.0e-30)
    sin_a = np.einsum("ij,ij->i", np.cross(n0, n1), edge)
    cos_a = np.clip(np.einsum("ij,ij->i", n0, n1), -1.0, 1.0)
    return np.arctan2(sin_a, cos_a)


def dihedral_gradients_numeric(local: np.ndarray, eps: float = 1.0e-4) -> np.ndarray:
    """Central-difference gradients, vectorized over all hinges.

    This is intentionally a spike implementation: it is slow compared with the
    analytic C++ gradient, but keeps the hypothesis test compact and auditable.
    """
    grad = np.empty_like(local)
    for lv in range(4):
        for axis in range(3):
            plus = local.copy()
            minus = local.copy()
            plus[:, lv, axis] += eps
            minus[:, lv, axis] -= eps
            grad[:, lv, axis] = wrap_angle(
                signed_dihedrals_local(plus) - signed_dihedrals_local(minus)
            ) / (2.0 * eps)
    return grad


def dihedral_gradients_analytic(local: np.ndarray) -> np.ndarray:
    """Analytic gradient matching ``signed_dihedrals_local``.

    The hinge convention is [edge0, edge1, opposite0, opposite1].  Degenerate
    triangles deliberately receive a zero gradient so the solver can reject or
    gate them without producing NaNs.
    """
    p0, p1, p2, p3 = (local[:, i] for i in range(4))
    edge = p1 - p0
    length2 = np.einsum("ij,ij->i", edge, edge)
    length = np.sqrt(np.maximum(length2, 0.0))
    n0 = np.cross(edge, p2 - p0)
    n1 = np.cross(p3 - p0, edge)
    n0_len2 = np.einsum("ij,ij->i", n0, n0)
    n1_len2 = np.einsum("ij,ij->i", n1, n1)
    valid = np.logical_and.reduce((length2 > 1.0e-24, n0_len2 > 1.0e-24, n1_len2 > 1.0e-24))

    grad = np.zeros_like(local)
    if not np.any(valid):
        return grad

    g2 = np.zeros_like(p0)
    g3 = np.zeros_like(p0)
    g2[valid] = -length[valid, None] * n0[valid] / n0_len2[valid, None]
    g3[valid] = -length[valid, None] * n1[valid] / n1_len2[valid, None]

    t2 = np.zeros(len(local), dtype=np.float64)
    t3 = np.zeros(len(local), dtype=np.float64)
    t2[valid] = np.einsum("ij,ij->i", p2[valid] - p0[valid], edge[valid]) / length2[valid]
    t3[valid] = np.einsum("ij,ij->i", p3[valid] - p0[valid], edge[valid]) / length2[valid]

    grad[:, 2] = g2
    grad[:, 3] = g3
    grad[:, 0] = -(1.0 - t2)[:, None] * g2 - (1.0 - t3)[:, None] * g3
    grad[:, 1] = -t2[:, None] * g2 - t3[:, None] * g3
    return grad


def dihedral_gradient_selftest() -> None:
    """Freeze the analytic hinge convention against central differences."""
    rng = np.random.default_rng(0xD4A9E)
    cases = []
    while len(cases) < 64:
        candidate = rng.normal(size=(4, 3))
        edge = candidate[1] - candidate[0]
        n0 = np.cross(edge, candidate[2] - candidate[0])
        n1 = np.cross(candidate[3] - candidate[0], edge)
        if np.linalg.norm(edge) < 0.2 or np.linalg.norm(n0) < 0.2 or np.linalg.norm(n1) < 0.2:
            continue
        cases.append(candidate)
    local = np.asarray(cases, dtype=np.float64)
    numeric = dihedral_gradients_numeric(local, eps=1.0e-6)
    analytic = dihedral_gradients_analytic(local)
    worst = float(np.max(np.abs(numeric - analytic)))
    translation_residual = float(np.max(np.linalg.norm(np.sum(analytic, axis=1), axis=1)))
    if worst > 2.0e-6 or translation_residual > 2.0e-10:
        raise RuntimeError(
            "analytic dihedral gradient mismatch: "
            f"worst={worst:.9g} translationResidual={translation_residual:.9g}"
        )
    print(
        "dihedral analytic selftest: PASS "
        f"worst={worst:.3g} translationResidual={translation_residual:.3g}"
    )


def seam_weld(
    verts: np.ndarray,
    seam_groups: list[list[int]],
    fixed: np.ndarray,
    fixed_targets: np.ndarray,
) -> None:
    for group in seam_groups:
        idx = np.asarray(group, dtype=np.int32)
        fixed_idx = idx[fixed[idx]]
        target = fixed_targets[fixed_idx[0]] if len(fixed_idx) else verts[idx].mean(axis=0)
        verts[idx] = target


def xpbd_stretch_jacobi(
    verts: np.ndarray,
    edges: np.ndarray,
    rest: np.ndarray,
    inv_mass: np.ndarray,
    lambdas: np.ndarray,
    compliance: float,
    relaxation: float,
    tension_only: bool = False,
) -> None:
    a = edges[:, 0]
    b = edges[:, 1]
    d = verts[a] - verts[b]
    length = np.linalg.norm(d, axis=1)
    normal = d / np.maximum(length[:, None], 1.0e-30)
    c = length - rest
    denom = inv_mass[a] + inv_mass[b] + compliance
    valid = denom > 1.0e-30
    dl = np.zeros(len(edges), dtype=np.float64)
    dl[valid] = (-c[valid] - compliance * lambdas[valid]) / denom[valid]
    if tension_only:
        # C = length-rest <= 0, therefore the unilateral XPBD multiplier is
        # lambda <= 0.  Project the *accumulated* multiplier every iteration.
        # Merely skipping C <= 0 leaves stale negative lambda behind when an
        # edge becomes slack, producing a non-physical "sticky thread" force.
        projected = np.minimum(lambdas + dl, 0.0)
        dl = projected - lambdas
        lambdas[:] = projected
    else:
        lambdas += dl

    delta = np.zeros_like(verts)
    count = np.zeros(len(verts), dtype=np.float64)
    ca = inv_mass[a, None] * dl[:, None] * normal
    cb = -inv_mass[b, None] * dl[:, None] * normal
    np.add.at(delta, a, ca)
    np.add.at(delta, b, cb)
    np.add.at(count, a, (inv_mass[a] > 0.0).astype(np.float64))
    np.add.at(count, b, (inv_mass[b] > 0.0).astype(np.float64))
    movable = count > 0.0
    verts[movable] += relaxation * delta[movable] / count[movable, None]


def xpbd_unilateral_selftest() -> None:
    """Catch stale/positive tension multipliers without a garment fixture."""
    edges = np.array([[0, 1]], dtype=np.int32)
    rest = np.array([1.0], dtype=np.float64)
    inv_mass = np.ones(2, dtype=np.float64)
    lambdas = np.zeros(1, dtype=np.float64)

    stretched = np.array([[0.0, 0.0, 0.0], [2.0, 0.0, 0.0]])
    xpbd_stretch_jacobi(
        stretched, edges, rest, inv_mass, lambdas,
        compliance=0.0, relaxation=1.0, tension_only=True,
    )
    if not lambdas[0] < 0.0:
        raise RuntimeError(f"tension multiplier was not activated: {lambdas[0]}")

    previous = float(lambdas[0])
    slack = np.array([[0.0, 0.0, 0.0], [0.5, 0.0, 0.0]])
    xpbd_stretch_jacobi(
        slack, edges, rest, inv_mass, lambdas,
        compliance=0.0, relaxation=1.0, tension_only=True,
    )
    if not previous < lambdas[0] <= 0.0:
        raise RuntimeError(
            f"slack edge did not discharge stored tension: {previous} -> {lambdas[0]}"
        )

    # A fully slack, degenerate edge must clear the multiplier without inventing
    # a direction or moving either endpoint.
    collapsed = np.zeros((2, 3), dtype=np.float64)
    before = collapsed.copy()
    xpbd_stretch_jacobi(
        collapsed, edges, rest, inv_mass, lambdas,
        compliance=0.0, relaxation=1.0, tension_only=True,
    )
    if abs(float(lambdas[0])) > 1.0e-12 or not np.array_equal(collapsed, before):
        raise RuntimeError(
            f"collapsed slack edge retained tension/moved: lambda={lambdas[0]}"
        )
    print("xpbd unilateral selftest: PASS")


def xpbd_bending_jacobi(
    verts: np.ndarray,
    hinges: np.ndarray,
    rest_angles: np.ndarray,
    inv_mass: np.ndarray,
    lambdas: np.ndarray,
    compliance: float,
    relaxation: float,
    analytic_gradient: bool = False,
) -> None:
    if len(hinges) == 0:
        return
    local = verts[hinges]
    angle = signed_dihedrals_local(local)
    c = wrap_angle(angle - rest_angles)
    grad = (
        dihedral_gradients_analytic(local)
        if analytic_gradient
        else dihedral_gradients_numeric(local)
    )
    weights = inv_mass[hinges]
    denom = np.sum(weights[:, :, None] * grad * grad, axis=(1, 2)) + compliance
    valid = denom > 1.0e-30
    dl = np.zeros(len(hinges), dtype=np.float64)
    dl[valid] = (-c[valid] - compliance * lambdas[valid]) / denom[valid]
    lambdas += dl

    delta = np.zeros_like(verts)
    count = np.zeros(len(verts), dtype=np.float64)
    corr = weights[:, :, None] * dl[:, None, None] * grad
    for lv in range(4):
        np.add.at(delta, hinges[:, lv], corr[:, lv])
        np.add.at(count, hinges[:, lv], (weights[:, lv] > 0.0).astype(np.float64))
    movable = count > 0.0
    verts[movable] += relaxation * delta[movable] / count[movable, None]


def greedy_constraint_batches(constraints: np.ndarray, vertex_count: int) -> tuple[np.ndarray, ...]:
    """Deterministically color constraints into conflict-free GS batches."""
    constraints = np.asarray(constraints, dtype=np.int32)
    if constraints.ndim != 2:
        raise ValueError("constraints must be a 2D integer array")
    used: list[set[int]] = [set() for _ in range(vertex_count)]
    colors = np.empty(len(constraints), dtype=np.int32)
    color_count = 0
    for ci, row in enumerate(constraints):
        forbidden: set[int] = set()
        for vi in row:
            if vi < 0 or vi >= vertex_count:
                raise ValueError(f"constraint vertex out of range: {vi}")
            forbidden.update(used[int(vi)])
        color = 0
        while color in forbidden:
            color += 1
        colors[ci] = color
        color_count = max(color_count, color + 1)
        for vi in row:
            used[int(vi)].add(color)
    return tuple(np.flatnonzero(colors == color).astype(np.int32) for color in range(color_count))


def xpbd_stretch_colored_gs(
    verts: np.ndarray,
    edges: np.ndarray,
    batches: tuple[np.ndarray, ...],
    rest: np.ndarray,
    inv_mass: np.ndarray,
    lambdas: np.ndarray,
    compliance: float,
    relaxation: float = 1.0,
) -> None:
    """Bilateral XPBD distance constraints with conflict-free colored GS."""
    for batch in batches:
        e = edges[batch]
        a = e[:, 0]
        b = e[:, 1]
        d = verts[a] - verts[b]
        length = np.linalg.norm(d, axis=1)
        normal = d / np.maximum(length[:, None], 1.0e-30)
        c = length - rest[batch]
        denom = inv_mass[a] + inv_mass[b] + compliance
        dl = np.zeros(len(batch), dtype=np.float64)
        valid = denom > 1.0e-30
        dl[valid] = (
            -c[valid] - compliance * lambdas[batch[valid]]
        ) / denom[valid]
        dl *= relaxation
        lambdas[batch] += dl
        verts[a] += inv_mass[a, None] * dl[:, None] * normal
        verts[b] -= inv_mass[b, None] * dl[:, None] * normal


def xpbd_bending_colored_gs(
    verts: np.ndarray,
    hinges: np.ndarray,
    batches: tuple[np.ndarray, ...],
    rest_angles: np.ndarray,
    constraint_scale: np.ndarray,
    inv_mass: np.ndarray,
    lambdas: np.ndarray,
    compliance: float,
    relaxation: float = 1.0,
) -> None:
    """Signed-dihedral XPBD with analytic gradients and colored GS."""
    for batch in batches:
        h = hinges[batch]
        local = verts[h]
        scale = constraint_scale[batch]
        c = wrap_angle(signed_dihedrals_local(local) - rest_angles[batch]) * scale
        grad = dihedral_gradients_analytic(local) * scale[:, None, None]
        weights = inv_mass[h]
        denom = np.sum(weights[:, :, None] * grad * grad, axis=(1, 2)) + compliance
        dl = np.zeros(len(batch), dtype=np.float64)
        valid = denom > 1.0e-30
        dl[valid] = (
            -c[valid] - compliance * lambdas[batch[valid]]
        ) / denom[valid]
        dl *= relaxation
        lambdas[batch] += dl
        correction = weights[:, :, None] * dl[:, None, None] * grad
        for lv in range(4):
            verts[h[:, lv]] += correction[:, lv]


def hinge_geometry_scale(vertices: np.ndarray, hinges: np.ndarray) -> np.ndarray:
    """Mesh-density normalization for signed-dihedral constraints."""
    local = vertices[hinges]
    edge = local[:, 1] - local[:, 0]
    edge_len2 = np.einsum("ij,ij->i", edge, edge)
    area0 = 0.5 * np.linalg.norm(np.cross(edge, local[:, 2] - local[:, 0]), axis=1)
    area1 = 0.5 * np.linalg.norm(
        np.cross(local[:, 3] - local[:, 0], edge), axis=1
    )
    denom = area0 + area1
    scale = np.zeros(len(hinges), dtype=np.float64)
    valid = (edge_len2 > 1.0e-24) & (denom > 1.0e-24)
    scale[valid] = np.sqrt(3.0 * edge_len2[valid] / denom[valid])
    return scale


def lumped_area_mass(
    vertices: np.ndarray,
    triangles: np.ndarray,
    surface_density: float,
) -> np.ndarray:
    """Absolute barycentric node mass for a triangle shell."""
    if not np.isfinite(surface_density) or surface_density <= 0.0:
        raise ValueError("surface density must be finite and positive")
    tri = vertices[triangles]
    area = 0.5 * np.linalg.norm(
        np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0]), axis=1
    )
    if not np.all(np.isfinite(area)) or np.any(area <= 1.0e-12):
        raise ValueError("area mass requires finite non-degenerate triangles")
    mass = np.zeros(len(vertices), dtype=np.float64)
    for local_vertex in range(3):
        np.add.at(mass, triangles[:, local_vertex], surface_density * area / 3.0)
    return mass


def colored_gs_selftest() -> None:
    edges = np.asarray([[0, 1], [1, 2], [2, 3], [0, 3]], dtype=np.int32)
    batches = greedy_constraint_batches(edges, 4)
    seen = np.zeros(len(edges), dtype=np.int32)
    for batch in batches:
        seen[batch] += 1
        flat = edges[batch].ravel()
        if len(np.unique(flat)) != len(flat):
            raise RuntimeError("constraint color contains a shared vertex")
    if not np.all(seen == 1):
        raise RuntimeError("constraint coloring lost or duplicated a constraint")

    line_edges = np.asarray([[0, 1], [1, 2]], dtype=np.int32)
    line_batches = greedy_constraint_batches(line_edges, 3)
    verts = np.asarray([[0.0, 0.0, 0.0], [1.8, 0.0, 0.0], [3.6, 0.0, 0.0]])
    rest = np.ones(2, dtype=np.float64)
    inv_mass = np.asarray([0.0, 1.0, 1.0])
    lambdas = np.zeros(2, dtype=np.float64)
    for _ in range(64):
        xpbd_stretch_colored_gs(
            verts, line_edges, line_batches, rest, inv_mass, lambdas,
            compliance=0.0,
        )
    lengths = np.linalg.norm(verts[line_edges[:, 0]] - verts[line_edges[:, 1]], axis=1)
    if np.max(np.abs(lengths - rest)) > 1.0e-8:
        raise RuntimeError(f"colored GS distance solve did not converge: {lengths}")

    seam_base = np.asarray(
        [
            [0.0, 0.0, 0.0], [1.0, 0.0, 0.0],
            [1.0, 0.0, 0.0], [2.0, 0.0, 0.0],
            [0.0, 0.0, 1.0], [1.0, 0.0, 1.0],
            [1.0, 0.0, 1.0], [2.0, 0.0, 1.0],
        ],
        dtype=np.float64,
    )
    seam_tris = np.asarray(
        [[0, 1, 4], [1, 5, 4], [2, 3, 6], [3, 7, 6]], dtype=np.int32
    )
    reduced = reduce_exact_seam_dofs(
        seam_base, seam_base.copy(), seam_tris, [[1, 2], [5, 6]]
    )
    if len(reduced.base_verts) != 6 or len(reduced.tris) != 4:
        raise RuntimeError("exact seam DOF reduction produced the wrong topology")
    square = np.asarray(
        [[0.0, 0.0, 0.0], [1.0, 0.0, 0.0],
         [1.0, 1.0, 0.0], [0.0, 1.0, 0.0]],
        dtype=np.float64,
    )
    square_tris = np.asarray([[0, 1, 2], [0, 2, 3]], dtype=np.int32)
    area_mass = lumped_area_mass(square, square_tris, surface_density=2.5)
    if abs(float(area_mass.sum()) - 2.5) > 1.0e-12:
        raise RuntimeError(f"lumped area mass is not conservative: {area_mass}")
    print(
        "xpbd colored-GS selftest: PASS "
        f"edgeColors={len(batches)} lineError={np.max(np.abs(lengths-rest)):.3g} "
        f"seamNodes=8->{len(reduced.base_verts)} mass={area_mass.sum():.3g}"
    )


def build_meridian_triplets(
    base: np.ndarray,
    edges: np.ndarray,
    inv_mass: np.ndarray,
) -> tuple[np.ndarray, np.ndarray]:
    """Infer local material generators using only garment topology + gravity axis."""
    neighbours: list[list[int]] = [[] for _ in range(len(base))]
    for a, b in edges:
        neighbours[int(a)].append(int(b))
        neighbours[int(b)].append(int(a))
    triplets = []
    blend = []
    for i, nbr in enumerate(neighbours):
        if inv_mass[i] < 0.25:
            continue
        up_best = None
        down_best = None
        for j in nbr:
            d = base[j] - base[i]
            length = float(np.linalg.norm(d))
            if length <= 1.0e-8 or abs(d[2]) < 0.15:
                continue
            verticality = abs(float(d[2])) / length
            if verticality < 0.45:
                continue
            candidate = (verticality, abs(float(d[2])), j)
            if d[2] > 0.0 and (up_best is None or candidate > up_best):
                up_best = candidate
            if d[2] < 0.0 and (down_best is None or candidate > down_best):
                down_best = candidate
        if up_best is None or down_best is None:
            continue
        up = up_best[2]
        down = down_best[2]
        span = float(base[up, 2] - base[down, 2])
        if span <= 0.3:
            continue
        t = float((base[i, 2] - base[down, 2]) / span)
        if not 0.05 < t < 0.95:
            continue
        triplets.append((down, i, up))
        blend.append(t)
    return np.asarray(triplets, dtype=np.int32), np.asarray(blend, dtype=np.float64)


def xpbd_meridian_jacobi(
    verts: np.ndarray,
    triplets: np.ndarray,
    blend: np.ndarray,
    inv_mass: np.ndarray,
    lambdas: np.ndarray,
    compliance: float,
    relaxation: float,
) -> None:
    """Penalize low-frequency horizontal curvature along vertical generators."""
    if len(triplets) == 0:
        return
    down, center, up = triplets.T
    t = blend
    c = verts[center, :2] - (
        (1.0 - t[:, None]) * verts[down, :2] + t[:, None] * verts[up, :2]
    )
    gd = -(1.0 - t)
    gc = np.ones(len(t))
    gu = -t
    denom = (
        inv_mass[down] * gd * gd
        + inv_mass[center] * gc * gc
        + inv_mass[up] * gu * gu
        + compliance
    )
    dl = (-c - compliance * lambdas) / np.maximum(denom[:, None], 1.0e-30)
    lambdas += dl

    delta = np.zeros_like(verts)
    count = np.zeros(len(verts), dtype=np.float64)
    for idx, grad in ((down, gd), (center, gc), (up, gu)):
        corr = inv_mass[idx, None] * grad[:, None] * dl
        np.add.at(delta[:, :2], idx, corr)
        np.add.at(count, idx, (inv_mass[idx] > 0.0).astype(np.float64))
    movable = count > 0.0
    verts[movable, :2] += relaxation * delta[movable, :2] / count[movable, None]


def edge_metrics(verts: np.ndarray, edges: np.ndarray, rest: np.ndarray) -> tuple[float, float, float]:
    length = np.linalg.norm(verts[edges[:, 0]] - verts[edges[:, 1]], axis=1)
    ratio = length / np.maximum(rest, 1.0e-30)
    return float(np.mean(np.abs(ratio - 1.0))), float(np.percentile(ratio, 95)), float(np.max(ratio))


def surface_distance_stats(source: np.ndarray, target: ObjMesh) -> tuple[float, float, float]:
    collider = UniformBodyCollider(target.verts, target.tris, cell_size=3.0)
    dist = np.abs(collider.gaps(source))
    return float(np.nanmean(dist)), float(np.nanpercentile(dist, 95)), float(np.nanmax(dist))


def make_rest_lengths(
    mode: str,
    base: np.ndarray,
    vanilla: np.ndarray,
    edges: np.ndarray,
    growth_blend: float,
    ref_center: np.ndarray,
) -> np.ndarray:
    base_len = np.linalg.norm(base[edges[:, 0]] - base[edges[:, 1]], axis=1)
    if mode == "base":
        return base_len
    if mode == "uniform":
        vanilla_len = np.linalg.norm(vanilla[edges[:, 0]] - vanilla[edges[:, 1]], axis=1)
        # One global robust growth factor tests whether the base cloth merely
        # needs resizing; it cannot encode a local cleft by construction.
        growth = float(np.median(vanilla_len / np.maximum(base_len, 1.0e-30)))
        factor = 1.0 + growth_blend * (growth - 1.0)
        print(f"rest uniform medianVanillaGrowth={growth:.6f} appliedFactor={factor:.6f}")
        return base_len * factor
    if mode == "lowpass":
        edge_vec = base[edges[:, 1]] - base[edges[:, 0]]
        edge_dir = edge_vec / np.maximum(base_len[:, None], 1.0e-30)
        mid = 0.5 * (base[edges[:, 0]] + base[edges[:, 1]])
        radial = mid[:, :2] - ref_center[None, :]
        radial /= np.maximum(np.linalg.norm(radial, axis=1, keepdims=True), 1.0e-30)
        tangent = np.column_stack((-radial[:, 1], radial[:, 0], np.zeros(len(edges))))
        circum_weight = np.einsum("ij,ij->i", edge_dir, tangent) ** 2

        vanilla_len = np.linalg.norm(vanilla[edges[:, 0]] - vanilla[edges[:, 1]], axis=1)
        raw_log = np.log(np.maximum(vanilla_len, 1.0e-30) / np.maximum(base_len, 1.0e-30))
        raw_log = np.clip(raw_log, -0.20, 0.50)

        # Convert noisy edge strain to a vertex field, then diffuse it over the
        # intrinsic graph. Twenty rounds intentionally remove anatomical-scale
        # variation while preserving the garment's broad size change.
        v_sum = np.zeros(len(base), dtype=np.float64)
        v_weight = np.zeros(len(base), dtype=np.float64)
        ew = np.maximum(circum_weight, 0.02)
        np.add.at(v_sum, edges[:, 0], raw_log * ew)
        np.add.at(v_sum, edges[:, 1], raw_log * ew)
        np.add.at(v_weight, edges[:, 0], ew)
        np.add.at(v_weight, edges[:, 1], ew)
        field = v_sum / np.maximum(v_weight, 1.0e-30)
        for _ in range(20):
            neighbour_sum = np.zeros(len(base), dtype=np.float64)
            neighbour_n = np.zeros(len(base), dtype=np.float64)
            np.add.at(neighbour_sum, edges[:, 0], field[edges[:, 1]])
            np.add.at(neighbour_sum, edges[:, 1], field[edges[:, 0]])
            np.add.at(neighbour_n, edges[:, 0], 1.0)
            np.add.at(neighbour_n, edges[:, 1], 1.0)
            average = neighbour_sum / np.maximum(neighbour_n, 1.0)
            field = 0.5 * field + 0.5 * average

        edge_log = 0.5 * (field[edges[:, 0]] + field[edges[:, 1]])
        factor = np.exp(growth_blend * edge_log * circum_weight)
        print(
            "rest lowpass circumFactor[p05/p50/p95]="
            f"{np.percentile(factor,5):.4f}/{np.median(factor):.4f}/{np.percentile(factor,95):.4f} "
            f"max={factor.max():.4f}"
        )
        return base_len * factor
    raise ValueError(f"unknown rest metric: {mode}")


def solve_shell(
    capture_path: Path,
    dataset: Path,
    output: Path,
    rest_mode: str,
    bend_rest_mode: str,
    growth_blend: float,
    morph_steps: int,
    iterations: int,
    stretch_compliance: float,
    bend_compliance: float,
    meridian_compliance: float,
    shadow_source: str,
    shadow_cell: float,
    shadow_strength: float,
    shadow_clearance: float,
    shadow_max_ring: int,
    shadow_contact_rings: int,
    shadow_contact_falloff: str,
    shadow_search: str,
    graph_plumb_strength: float,
    graph_plumb_return: float,
    graph_plumb_order: str,
    graph_plumb_seeds: str,
    band_circumference_compliance: float,
    band_z_cell: float,
    band_rest_mode: str,
    gravity: float,
    clearance: float,
    anchor_target_mode: str = "vanilla",
    soft_anchor_mode: str = "on",
    body_mode: str = "continuation",
    gravity_mode: str = "step",
    stretch_mode: str = "bilateral",
    trace_json: Path | None = None,
    surface_corot_clamp: bool = True,
    surface_corot_mask_json: Path | None = None,
    surface_corot_classifier: str = "coherent-v1",
    surface_corot_fallback: str = "authored",
) -> None:
    cap = read_capture_front(capture_path)
    vanilla_path = dataset / "cloth_vanilla.obj"
    reference_path = dataset / "cloth_sculpt.obj"
    if vanilla_path.is_file():
        vanilla = load_obj(vanilla_path)
    else:
        vanilla = ObjMesh(cap.base_verts + cap.raw_diff, cap.tris)
        write_obj(dataset / "cloth_base.obj", cap.base_verts, cap.tris)
        write_obj(vanilla_path, vanilla.verts, cap.tris)
        write_obj(dataset / "body_base.obj", cap.ref_body_verts, cap.ref_body_tris)
        write_obj(dataset / "body_morphed.obj", cap.body_verts, cap.body_tris)
        print(f"dataset exported from capture to {dataset}")
    if reference_path.is_file():
        reference = load_obj(reference_path)
        reference_label = "sculpt"
    else:
        reference = vanilla
        reference_label = "vanilla"
    if len(vanilla.verts) != len(cap.base_verts) or len(reference.verts) != len(cap.base_verts):
        raise ValueError("solve references must match capture cloth vertex count")
    print(f"comparisonReference={reference_label}")
    topo = topology_stats(cap.base_verts, cap.tris)
    edges = np.asarray(sorted(topo["edge_faces"]), dtype=np.int32)
    cloth_adjacency = vertex_adjacency(len(cap.base_verts), edges)
    hinges = topo["hinges"]
    seam_groups = topo["seam_groups"]
    rest_len = make_rest_lengths(
        rest_mode, cap.base_verts, vanilla.verts, edges, growth_blend, cap.ref_center
    )
    base_rest_angles = signed_dihedrals_local(cap.base_verts[hinges])

    drape = fitted_drape_signal(cap)
    # Continuous mobility is essential at the bodice/panel boundary. Mirroring
    # the production hard cut at 0.20 made two 0.2u rest edges straddle fixed vs
    # free and stretch to 6-8x. Upper-body vertices still follow vanilla, but the
    # transition is an energy blend rather than a categorical discontinuity.
    u = np.clip((drape - 0.05) / 0.50, 0.0, 1.0)
    u = u * u * (3.0 - 2.0 * u)
    inv_mass = u
    fixed = inv_mass <= 1.0e-12
    soft_anchor = 0.18 * (1.0 - u) ** 2
    # S0.3c causal ablation (2026-07-14): the anchors inject the VANILLA rawDiff,
    # so "the body only acts as an obstacle" is not yet true in this experiment.
    # soft_anchor_mode=off disables the soft field only - hard anchors always
    # stay pinned (releasing them leaves the problem with no boundary condition).
    if soft_anchor_mode == "off":
        soft_anchor = np.zeros_like(soft_anchor)
    active = np.flatnonzero(inv_mass > 0.0).astype(np.int32)
    shadow_indices = np.flatnonzero(drape >= 0.20).astype(np.int32)
    meridian_triplets, meridian_blend = build_meridian_triplets(cap.base_verts, edges, inv_mass)
    band_rest_positions = cap.base_verts if band_rest_mode == "base" else vanilla.verts
    band_groups, band_rest = build_band_circumference_constraints(
        cap.base_verts, band_rest_positions, edges, inv_mass, band_z_cell
    )
    rest_angles = base_rest_angles.copy()
    if bend_rest_mode == "flat-panel":
        panel_hinges = np.min(inv_mass[hinges], axis=1) > 0.0
        rest_angles[panel_hinges] = 0.0
        print(f"bend rest flat-panel hinges={int(panel_hinges.sum())}/{len(hinges)}")
    elif bend_rest_mode != "base":
        raise ValueError(f"unknown bend rest metric: {bend_rest_mode}")
    print(
        f"solve rest={rest_mode} bendRest={bend_rest_mode} morphSteps={morph_steps} iterations={iterations} "
        f"active={len(active)} fixed={fixed.sum()} hinges={len(hinges)} seams={len(seam_groups)}"
    )
    plumb_iteration_strength = (
        1.0 - (1.0 - graph_plumb_strength) ** (1.0 / iterations)
        if graph_plumb_strength < 1.0
        else 1.0
    )
    if graph_plumb_strength > 0.0:
        print(
            f"graphPlumb stepStrength={graph_plumb_strength:g} "
            f"iterationStrength={plumb_iteration_strength:.6f} "
            f"returnRate={graph_plumb_return:g} order={graph_plumb_order} "
            f"seeds={graph_plumb_seeds}"
        )
    print(
        f"shadow source={shadow_source} verts={len(shadow_indices)} cell={shadow_cell:g} "
        f"strength={shadow_strength:g} clearance={shadow_clearance:g} "
        f"contactFalloff={shadow_contact_falloff} search={shadow_search}"
    )
    if meridian_compliance >= 0.0:
        print(
            f"meridian constraints={len(meridian_triplets)} compliance={meridian_compliance:g}"
        )
    if band_circumference_compliance >= 0.0:
        print(
            f"bandCircumference constraints={len(band_groups)} zCell={band_z_cell:g} "
            f"compliance={band_circumference_compliance:g} rest={band_rest_mode}"
        )

    # Preserve the authored base contact state, including shallow intersections
    # from the source asset / face-normal pseudo-sign. Contact may not get worse,
    # but the spike does not blast the t=0 garment into a different rest pose.
    base_collider = UniformBodyCollider(cap.ref_body_verts, cap.ref_body_tris)
    authored_gap = base_collider.gaps(cap.base_verts)
    contact_floor = np.minimum(authored_gap, clearance)
    surface_attachments: SurfaceAttachments | None = None
    attachment_indices = np.empty(0, dtype=np.int32)
    if anchor_target_mode == "surface-corot":
        if not np.array_equal(cap.ref_body_tris, cap.body_tris):
            raise RuntimeError("surface-corot requires identical reference/morphed body topology")
        attachment_mask = np.logical_or(fixed, soft_anchor > 0.0)
        if surface_corot_mask_json is not None:
            attachment_mask = load_surface_corot_mask(
                surface_corot_mask_json,
                surface_corot_classifier,
                capture_path,
                len(cap.base_verts),
                attachment_mask,
                surface_corot_fallback=surface_corot_fallback,
                surface_corot_clamp=surface_corot_clamp,
                body_mode=body_mode,
                soft_anchor_mode=soft_anchor_mode,
                morph_steps=morph_steps,
                clearance=clearance,
            )
            print(
                f"surfaceCorot classifier={surface_corot_classifier} "
                f"mask={surface_corot_mask_json} maskSha={sha256(surface_corot_mask_json)[:12]} "
                f"fallback={surface_corot_fallback} selected={int(attachment_mask.sum())}"
            )
        surface_attachments = build_surface_attachments(
            cap.base_verts,
            attachment_mask,
            cap.ref_body_verts,
            cap.ref_body_tris,
            base_collider,
        )
        attachment_indices = np.flatnonzero(surface_attachments.valid).astype(np.int32)
        distances = surface_attachments.authored_distance[attachment_indices]
        print(
            f"surfaceCorot requested={int(attachment_mask.sum())} "
            f"valid={len(attachment_indices)} "
            f"uniqueBodyTris={len(np.unique(surface_attachments.triangle[attachment_indices]))} "
            f"authoredDistance min/p50/p95/max={distances.min():.3f}/"
            f"{np.percentile(distances,50):.3f}/{np.percentile(distances,95):.3f}/"
            f"{distances.max():.3f}"
        )

    x = cap.base_verts.copy()
    plumb_support = np.zeros(len(x), dtype=np.float64)
    # S0.3e causal instrumentation.  A vertex is "contacted" only when the
    # unilateral collider actually applies a correction.  This is distinct
    # from the old authored-gap proxy and lets factorial analysis use one
    # common never-contacted, non-fixed population across all cells.
    contact_count = np.zeros(len(x), dtype=np.int64)
    contact_step_count = np.zeros(len(x), dtype=np.int64)
    minimum_observed_gap = np.full(len(x), np.inf, dtype=np.float64)
    support_clamp_total = 0
    support_clamp_worst = 0.0
    final_anchor_target = cap.base_verts.copy()
    contact_triangles_global: set[int] = set()
    body_tri_adjacency = triangle_adjacency(cap.body_tris) if shadow_source == "contact" else []
    for step in range(1, morph_steps + 1):
        t = step / morph_steps
        contacted_this_step = np.zeros(len(x), dtype=bool)
        # S0.3c factor "body geometry": both modes retain collision.  Static
        # keeps the reference obstacle while continuation morphs it; this is
        # not a contact-off/contact-on experiment.
        if body_mode == "static":
            body = cap.ref_body_verts
        else:
            body = cap.ref_body_verts + (cap.body_verts - cap.ref_body_verts) * t
        collider = UniformBodyCollider(body, cap.body_tris)
        # Anchor targets: vanilla copies rawDiff; authored freezes the source;
        # surface-corot follows a material body point and transports only its
        # local rigid rotation + the authored cloth offset (no sculpt target).
        if anchor_target_mode == "surface-corot":
            assert surface_attachments is not None
            if surface_corot_fallback == "authored":
                corot_fallback = cap.base_verts
            elif surface_corot_fallback == "vanilla":
                corot_fallback = cap.base_verts + cap.raw_diff * t
            elif surface_corot_fallback == "body-vanilla":
                body_t = 0.0 if body_mode == "static" else t
                corot_fallback = cap.base_verts + cap.raw_diff * body_t
            else:
                raise ValueError(
                    f"unknown surface-corot fallback: {surface_corot_fallback}"
                )
            anchor_target = transport_surface_attachments(
                surface_attachments,
                cap.ref_body_verts,
                body,
                cap.body_tris,
                corot_fallback,
                cap.base_verts,
            )
            if surface_corot_clamp:
                clamp_mask = np.logical_and(fixed, surface_attachments.requested)
                clamp_count, clamp_worst = clamp_targets_to_contact_floor(
                    anchor_target,
                    np.flatnonzero(clamp_mask).astype(np.int32),
                    collider,
                    contact_floor,
                )
                support_clamp_total += clamp_count
                support_clamp_worst = max(support_clamp_worst, clamp_worst)
        elif anchor_target_mode == "authored":
            anchor_target = cap.base_verts
        else:
            anchor_target = cap.base_verts + cap.raw_diff * t
        final_anchor_target = anchor_target
        x[fixed] = anchor_target[fixed]
        if gravity != 0.0 and gravity_mode == "step":
            # legacy: one dose of gravity per morph step, then the constraint
            # iterations relax it away (6 doses vs 60 projections) - kept as
            # the default for reproducibility of earlier marcos.
            x[:, 2] -= gravity * inv_mass

        stretch_lambda = np.zeros(len(edges), dtype=np.float64)
        bend_lambda = np.zeros(len(hinges), dtype=np.float64)
        meridian_lambda = np.zeros((len(meridian_triplets), 2), dtype=np.float64)
        band_lambda = np.zeros(len(band_groups), dtype=np.float64)
        moved_total = 0
        worst_contact = 0.0
        shadow_moved_total = 0
        shadow_worst = 0.0
        plumb_moved_total = 0
        plumb_worst = 0.0
        band_active_total = 0
        band_worst_ratio = 1.0
        shadow_ceiling = float(np.max(x[shadow_indices, 2])) if len(shadow_indices) else -1.0e30
        shadow = None
        shadow_triangle_count = 0
        if shadow_source == "body":
            shadow = GravityShadow(body, cap.body_tris, shadow_cell, shadow_ceiling)
        elif shadow_source == "contact" and contact_triangles_global:
            support = triangle_ring_weights(
                contact_triangles_global,
                body_tri_adjacency,
                shadow_contact_rings,
                shadow_contact_falloff,
            )
            weights = np.zeros(len(cap.body_tris), dtype=np.float64)
            support_indices = np.fromiter(sorted(support), dtype=np.int32)
            weights[support_indices] = np.fromiter(
                (support[int(ti)] for ti in support_indices), dtype=np.float64
            )
            shadow_triangle_count = len(support)
            shadow = GravityShadow(body, cap.body_tris, shadow_cell, shadow_ceiling, weights)
        for _iteration in range(iterations):
            x += soft_anchor[:, None] * (anchor_target - x)
            x[fixed] = anchor_target[fixed]
            if gravity != 0.0 and gravity_mode == "iteration":
                # quasi-static XPBD: the external pull is applied EVERY
                # iteration, alongside the projections, so equilibrium emerges
                # from the balance (gravity vs taut threads vs contact) instead
                # of the projections erasing a once-per-step displacement.
                x[:, 2] -= gravity * inv_mass
            if graph_plumb_order == "pre-stretch":
                plumb_moved, plumb_step = apply_graph_plumb_constraint(
                    x,
                    active,
                    cloth_adjacency,
                    cap.ref_center,
                    graph_plumb_return,
                    plumb_iteration_strength,
                    plumb_support.copy() if graph_plumb_seeds != "all" else None,
                    cap.base_verts if graph_plumb_seeds == "contact-tension" else None,
                )
                plumb_moved_total += plumb_moved
                plumb_worst = max(plumb_worst, plumb_step)
            xpbd_stretch_jacobi(
                x, edges, rest_len, inv_mass, stretch_lambda,
                compliance=stretch_compliance, relaxation=0.95,
                tension_only=(stretch_mode == "tension"),
            )
            if band_circumference_compliance >= 0.0:
                band_active, band_ratio = xpbd_band_circumference(
                    x,
                    edges,
                    band_groups,
                    band_rest,
                    inv_mass,
                    band_lambda,
                    band_circumference_compliance,
                    relaxation=0.8,
                )
                band_active_total += band_active
                band_worst_ratio = max(band_worst_ratio, band_ratio)
            xpbd_bending_jacobi(
                x, hinges, rest_angles, inv_mass, bend_lambda,
                compliance=bend_compliance, relaxation=0.45,
            )
            if meridian_compliance >= 0.0:
                xpbd_meridian_jacobi(
                    x, meridian_triplets, meridian_blend, inv_mass, meridian_lambda,
                    compliance=meridian_compliance, relaxation=0.65,
                )
            seam_weld(x, seam_groups, fixed, anchor_target)
            # Per-vertex authored floors make t=0 a valid state. The collider's
            # projection is applied one vertex at a time to retain those floors.
            moved = 0
            worst = 0.0
            for vi in active:
                result = collider.closest(x[vi])
                if result is None:
                    continue
                cp, n, gap, body_tri = result
                minimum_observed_gap[vi] = min(minimum_observed_gap[vi], gap)
                floor = contact_floor[vi]
                if gap >= floor:
                    continue
                target = cp + n * floor
                delta = target - x[vi]
                x[vi] = target
                contact_triangles_global.add(int(body_tri))
                plumb_support[vi] = 1.0
                contact_count[vi] += 1
                contacted_this_step[vi] = True
                moved += 1
                worst = max(worst, float(np.linalg.norm(delta)))
            moved_total += moved
            worst_contact = max(worst_contact, worst)
            if shadow_source == "contact" and shadow is None and contact_triangles_global:
                support = triangle_ring_weights(
                    contact_triangles_global,
                    body_tri_adjacency,
                    shadow_contact_rings,
                    shadow_contact_falloff,
                )
                weights = np.zeros(len(cap.body_tris), dtype=np.float64)
                support_indices = np.fromiter(sorted(support), dtype=np.int32)
                weights[support_indices] = np.fromiter(
                    (support[int(ti)] for ti in support_indices), dtype=np.float64
                )
                shadow_triangle_count = len(support)
                shadow = GravityShadow(body, cap.body_tris, shadow_cell, shadow_ceiling, weights)
            if shadow is not None and shadow_strength > 0.0:
                for vi in shadow_indices:
                    push = shadow.push_target(
                        x[vi],
                        shadow_clearance,
                        shadow_max_ring,
                        cap.ref_center,
                        shadow_search,
                    )
                    if push is None:
                        continue
                    target_xy, support_weight = push
                    delta = (
                        (target_xy - x[vi, :2])
                        * shadow_strength
                        * support_weight
                    )
                    x[vi, :2] += delta
                    shadow_moved_total += 1
                    shadow_worst = max(shadow_worst, float(np.linalg.norm(delta)))
            if graph_plumb_order == "post-contact":
                plumb_moved, plumb_step = apply_graph_plumb_constraint(
                    x,
                    active,
                    cloth_adjacency,
                    cap.ref_center,
                    graph_plumb_return,
                    plumb_iteration_strength,
                    plumb_support.copy() if graph_plumb_seeds != "all" else None,
                    cap.base_verts if graph_plumb_seeds == "contact-tension" else None,
                )
                plumb_moved_total += plumb_moved
                plumb_worst = max(plumb_worst, plumb_step)
            x[fixed] = anchor_target[fixed]

        contact_step_count += contacted_this_step.astype(np.int64)
        mean_strain, p95_ratio, max_ratio = edge_metrics(x, edges, rest_len)
        angle_err = np.abs(wrap_angle(signed_dihedrals_local(x[hinges]) - rest_angles))
        gaps = collider.gaps(x, active)
        print(
            f"step={step}/{morph_steps} t={t:.3f} contactMoves={moved_total} "
            f"worstContact={worst_contact:.3f} shadowMoves={shadow_moved_total} "
            f"shadowWorst={shadow_worst:.3f} contactTris={len(contact_triangles_global)} "
            f"shadowTris={shadow_triangle_count} plumbMoves={plumb_moved_total} "
            f"plumbWorst={plumb_worst:.3f} bandActive={band_active_total} "
            f"bandWorst={band_worst_ratio:.3f} minGap={np.nanmin(gaps):.3f} "
            f"supportClamps={support_clamp_total} supportClampWorst={support_clamp_worst:.3f} "
            f"meanAbsStrain={mean_strain:.4f} p95Stretch={p95_ratio:.3f} maxStretch={max_ratio:.3f} "
            f"meanBendDeg={np.degrees(angle_err).mean():.3f}"
        )

    write_obj(output, x, cap.tris)
    index_err = np.linalg.norm(x - reference.verts, axis=1)
    surf = surface_distance_stats(x, reference)
    reverse_surf = surface_distance_stats(reference.verts, ObjMesh(x, cap.tris))
    print(
        f"result={output} reference={reference_label} "
        f"indexError mean/p95/max={index_err.mean():.4f}/"
        f"{np.percentile(index_err,95):.4f}/{index_err.max():.4f} "
        f"surface result->reference mean/p95/max={surf[0]:.4f}/{surf[1]:.4f}/{surf[2]:.4f} "
        f"reference->result={reverse_surf[0]:.4f}/{reverse_surf[1]:.4f}/{reverse_surf[2]:.4f}"
    )
    if trace_json is not None:
        def finite_list(values: np.ndarray) -> list[float | None]:
            return [float(value) if np.isfinite(value) else None for value in values]

        final_collider = UniformBodyCollider(
            cap.ref_body_verts if body_mode == "static" else cap.body_verts,
            cap.body_tris,
        )
        final_gap = final_collider.gaps(x)
        observed = np.isfinite(minimum_observed_gap)
        effective_classifier = None
        effective_mask_sha256 = None
        effective_fallback = None
        effective_clamp = None
        if anchor_target_mode == "surface-corot":
            effective_classifier = (
                surface_corot_classifier
                if surface_corot_mask_json is not None
                else "broad-current"
            )
            effective_mask_sha256 = hashlib.sha256(
                surface_attachments.requested.astype(np.uint8).tobytes()
            ).hexdigest()
            effective_fallback = surface_corot_fallback
            effective_clamp = surface_corot_clamp
        trace = {
            "schema": "pathb-s03e-contact-trace-v1",
            "capture": str(capture_path),
            "capture_sha256": sha256(capture_path),
            "output": str(output),
            "output_sha256": sha256(output),
            "parameters": {
                "rest": rest_mode,
                "bend_rest": bend_rest_mode,
                "morph_steps": morph_steps,
                "iterations": iterations,
                "gravity_per_step": gravity,
                "anchor_target": anchor_target_mode,
                "soft_anchor": soft_anchor_mode,
                "body_mode": body_mode,
                "surface_corot_clamp": effective_clamp,
                "surface_corot_mask_json": (
                    str(surface_corot_mask_json) if surface_corot_mask_json else None
                ),
                "surface_corot_mask_sha256": (
                    sha256(surface_corot_mask_json) if surface_corot_mask_json else None
                ),
                "surface_corot_classifier": effective_classifier,
                "surface_corot_effective_mask_sha256": effective_mask_sha256,
                "surface_corot_fallback": effective_fallback,
                "clearance": clearance,
            },
            "counts": {
                "vertices": int(len(x)),
                "fixed": int(fixed.sum()),
                "active": int(len(active)),
                "ever_contacted_active": int(np.logical_and(contact_count > 0, ~fixed).sum()),
                "never_contacted_active": int(np.logical_and(contact_count == 0, ~fixed).sum()),
                "support_target_clamps": int(support_clamp_total),
            },
            "vertices": {
                "fixed": fixed.astype(np.uint8).tolist(),
                "drape": drape.tolist(),
                "inverse_mass": inv_mass.tolist(),
                "soft_anchor": soft_anchor.tolist(),
                "authored_gap": finite_list(authored_gap),
                "contact_floor": finite_list(contact_floor),
                "contact_corrections": contact_count.tolist(),
                "contacted_morph_steps": contact_step_count.tolist(),
                "minimum_observed_gap": finite_list(
                    np.where(observed, minimum_observed_gap, np.nan)
                ),
                "final_gap": finite_list(final_gap),
            },
        }
        if surface_attachments is not None:
            attachment_distance = surface_attachments.authored_distance[attachment_indices]
            raw_target = cap.base_verts + cap.raw_diff
            target_delta = np.linalg.norm(
                final_anchor_target[attachment_indices] - raw_target[attachment_indices], axis=1
            )
            trace["surface_attachment"] = {
                "requested": int(surface_attachments.requested.sum()),
                "valid": int(surface_attachments.valid.sum()),
                "unique_body_triangles": int(
                    len(np.unique(surface_attachments.triangle[attachment_indices]))
                ),
                "authored_distance_min": float(attachment_distance.min()),
                "authored_distance_p50": float(np.percentile(attachment_distance, 50)),
                "authored_distance_p95": float(np.percentile(attachment_distance, 95)),
                "authored_distance_max": float(attachment_distance.max()),
                "vs_raw_target_rms": float(np.sqrt(np.mean(target_delta * target_delta))),
                "vs_raw_target_max": float(target_delta.max()),
                "contact_floor_clamps": int(support_clamp_total),
                "contact_floor_worst": float(support_clamp_worst),
                "vertex_requested": surface_attachments.requested.astype(np.uint8).tolist(),
                "vertex_valid": surface_attachments.valid.astype(np.uint8).tolist(),
                "vertex_triangle": surface_attachments.triangle.tolist(),
                "vertex_authored_distance": finite_list(
                    surface_attachments.authored_distance
                ),
            }
        trace_json.parent.mkdir(parents=True, exist_ok=True)
        trace_json.write_text(
            json.dumps(trace, indent=2, sort_keys=True, allow_nan=False) + "\n",
            encoding="utf-8",
        )
        print(f"trace={trace_json} sha256={sha256(trace_json)}")


def solve_shell_dynamic(
    capture_path: Path,
    dataset: Path,
    output: Path,
    frames: int,
    morph_frames: int,
    morph_steps: int,
    iterations: int,
    substeps: int,
    dt: float,
    gravity_acceleration: float,
    velocity_damping: float,
    damping_rate: float,
    stretch_compliance: float,
    bend_compliance: float,
    clearance: float,
    rest_metric_mode: str,
    start_mode: str,
    solver_mode: str,
    mass_mode: str,
    surface_density: float,
    contact_warmup: int,
    stabilization_frames: int,
    settle_window: int,
    contact_mode: str,
    bend_rest_mode: str,
) -> bool:
    """Experimental physical settle for a static slider (Path-B shell v3).

    Unlike the marco-8 positional load, this mode integrates velocity and
    gravity over time. Continuation moves only hard supports toward their raw
    target. ``raw-settle`` initializes the whole shell at raw as an initial
    condition, but neither mode has a distributed positional raw attractor.
    Mobile nodes subsequently receive body motion only through unilateral
    contact and cloth constraints connected to supports. The state is settled
    at the final body and velocities are discarded.
    """
    if (
        frames < 1 or morph_frames < 1 or morph_steps < 1
        or iterations < 1 or substeps < 1 or contact_warmup < 0
        or stabilization_frames < 0
    ):
        raise ValueError(
            "dynamic frames/morph-frames/morph-steps/iterations/substeps must be "
            "positive and contact warmup/stabilization non-negative"
        )
    if morph_frames > frames:
        raise ValueError("dynamic morph-frames may not exceed total frames")
    if start_mode == "continuation" and morph_frames + stabilization_frames > frames:
        raise ValueError(
            "continuation requires frames >= morph-frames + stabilization-frames"
        )
    if not np.isfinite(dt) or dt <= 0.0:
        raise ValueError("dynamic dt must be finite and positive")
    if not 0.0 <= velocity_damping <= 1.0:
        raise ValueError("dynamic damping must be in [0,1]")
    if damping_rate < 0.0 or not np.isfinite(damping_rate):
        raise ValueError("dynamic damping rate must be finite and non-negative")
    if surface_density <= 0.0 or not np.isfinite(surface_density):
        raise ValueError("dynamic surface density must be finite and positive")

    cap = read_capture_front(capture_path)
    finite_arrays = {
        "cloth base": cap.base_verts,
        "cloth raw diff": cap.raw_diff,
        "body base": cap.ref_body_verts,
        "body final": cap.body_verts,
    }
    for label, array in finite_arrays.items():
        if not np.all(np.isfinite(array)):
            raise ValueError(f"dynamic {label} contains NaN/Inf")
    for label, triangles, vertex_count in (
        ("cloth", cap.tris, len(cap.base_verts)),
        ("body base", cap.ref_body_tris, len(cap.ref_body_verts)),
        ("body final", cap.body_tris, len(cap.body_verts)),
    ):
        if triangles.ndim != 2 or triangles.shape[1] != 3 or not len(triangles):
            raise ValueError(f"dynamic {label} triangle array is empty or malformed")
        if int(triangles.min()) < 0 or int(triangles.max()) >= vertex_count:
            raise ValueError(f"dynamic {label} triangle index is out of range")
    if not np.array_equal(cap.ref_body_tris, cap.body_tris):
        raise ValueError("dynamic body base/final topology differs")
    reference_path = dataset / "cloth_sculpt.obj"
    if reference_path.is_file():
        reference = load_obj(reference_path)
        reference_label = "sculpt"
    else:
        reference = ObjMesh(cap.base_verts + cap.raw_diff, cap.tris)
        reference_label = "vanilla"
    if (
        len(reference.verts) != len(cap.base_verts)
        or not np.array_equal(reference.tris, cap.tris)
        or not np.all(np.isfinite(reference.verts))
    ):
        raise ValueError("dynamic reference must match finite capture cloth topology")

    original_topology = topology_stats(cap.base_verts, cap.tris)
    original_raw_verts = cap.base_verts + cap.raw_diff
    drape = fitted_drape_signal(cap)
    mobility = np.clip((drape - 0.05) / 0.50, 0.0, 1.0)
    mobility = mobility * mobility * (3.0 - 2.0 * mobility)
    original_fixed = mobility <= 1.0e-12
    reduced = reduce_exact_seam_dofs(
        cap.base_verts,
        original_raw_verts,
        cap.tris,
        original_topology["seam_groups"],
    )
    base_verts = reduced.base_verts
    raw_metric_verts = reduced.raw_verts
    tris = reduced.tris
    topo = topology_stats(base_verts, tris)
    if topo["nonmanifold_edges"]:
        raise ValueError(
            "dynamic seam-reduced shell contains non-manifold edges: "
            f"{topo['nonmanifold_edges']}"
        )
    edges = np.asarray(sorted(topo["edge_faces"]), dtype=np.int32)
    hinges = topo["hinges"]
    base_rest_len = np.linalg.norm(
        base_verts[edges[:, 0]] - base_verts[edges[:, 1]], axis=1
    )
    raw_rest_len = np.linalg.norm(
        raw_metric_verts[edges[:, 0]] - raw_metric_verts[edges[:, 1]], axis=1
    )
    if rest_metric_mode not in ("base", "raw-metric"):
        raise ValueError(f"unknown dynamic rest metric: {rest_metric_mode}")
    if np.any(base_rest_len <= 1.0e-12) or np.any(raw_rest_len <= 1.0e-12):
        raise ValueError("dynamic rest metric contains a degenerate edge")
    rest_angles = signed_dihedrals_local(base_verts[hinges])
    base_bend_scale = hinge_geometry_scale(base_verts, hinges)
    raw_bend_scale = hinge_geometry_scale(raw_metric_verts, hinges)
    if np.any(base_bend_scale <= 0.0) or np.any(raw_bend_scale <= 0.0):
        raise ValueError("dynamic bend scale contains a degenerate hinge")

    fixed = np.asarray(
        [bool(np.any(original_fixed[group])) for group in reduced.node_members],
        dtype=bool,
    )
    # v3 treats mobility as an attachment classifier, not as physical mass.
    # Every non-attached cloth vertex has the same mass and gravitational
    # acceleration; this removes the old feathered body-follow field.
    inv_mass = (~fixed).astype(np.float64)
    active = np.flatnonzero(~fixed).astype(np.int32)

    if start_mode not in ("continuation", "raw-settle"):
        raise ValueError(f"unknown dynamic start mode: {start_mode}")
    if solver_mode not in ("legacy-jacobi", "colored-gs"):
        raise ValueError(f"unknown dynamic solver mode: {solver_mode}")
    if mass_mode not in ("uniform", "area"):
        raise ValueError(f"unknown dynamic mass mode: {mass_mode}")
    if contact_mode not in ("on", "off"):
        raise ValueError(f"unknown dynamic contact mode: {contact_mode}")
    if contact_mode == "on" and clearance < 0.0:
        raise ValueError("dynamic physical contact thickness must be non-negative")
    if mass_mode == "area":
        mass = lumped_area_mass(base_verts, tris, surface_density)
        positive = mass[~fixed]
        if not len(positive) or np.any(positive <= 1.0e-12):
            raise ValueError("dynamic area mass contains a zero-mass mobile vertex")
        inv_mass[~fixed] = 1.0 / mass[~fixed]
    else:
        mass = np.full(len(base_verts), surface_density, dtype=np.float64)

    if bend_rest_mode == "flat-panel":
        panel_hinges = np.min(inv_mass[hinges], axis=1) > 0.0
        rest_angles = rest_angles.copy()
        rest_angles[panel_hinges] = 0.0
    elif bend_rest_mode not in ("base", "off"):
        raise ValueError(f"unknown dynamic bend rest metric: {bend_rest_mode}")

    base_collider = UniformBodyCollider(cap.ref_body_verts, cap.ref_body_tris)
    final_body_collider = UniformBodyCollider(cap.body_verts, cap.body_tris)
    # Every quadrature sample gets its own endpoint floor.  This prevents a
    # centroid already inside the body from being misrepresented by three
    # clear vertices and then receiving a catastrophic teleport correction.
    contact_floors = (
        BodyContactFloors(
            node=np.full(len(base_verts), -np.inf, dtype=np.float64),
            edge=np.full(len(edges), -np.inf, dtype=np.float64),
            face=np.full(len(tris), -np.inf, dtype=np.float64),
        )
        if contact_mode == "off"
        else build_body_contact_floors(
            base_verts,
            raw_metric_verts,
            edges,
            tris,
            base_collider,
            final_body_collider,
            clearance,
        )
    )

    edge_batches: tuple[np.ndarray, ...] = ()
    hinge_batches: tuple[np.ndarray, ...] = ()
    if solver_mode == "colored-gs":
        edge_batches = greedy_constraint_batches(edges, len(base_verts))
        hinge_batches = greedy_constraint_batches(hinges, len(base_verts))

    x = (
        raw_metric_verts.copy()
        if start_mode == "raw-settle"
        else base_verts.copy()
    )
    velocity = np.zeros_like(x)
    contact_count = np.zeros(len(x), dtype=np.int64)
    effective_substeps = substeps if solver_mode == "colored-gs" else 1
    h = dt / effective_substeps
    stretch_alpha = stretch_compliance / (h * h)
    bend_alpha = bend_compliance / (h * h)
    print(
        "dynamic-v3 "
        f"frames={frames} morphFrames={morph_frames} morphSteps={morph_steps} "
        f"iterations={iterations} substeps={effective_substeps} dt={dt:g} h={h:g} "
        f"gravity={gravity_acceleration:g} "
        f"damping={velocity_damping:g}/rate={damping_rate:g} "
        f"verts={len(cap.base_verts)}->{len(base_verts)} "
        f"fixed={int(fixed.sum())} active={len(active)} edges={len(edges)} "
        f"hinges={len(hinges)} seams={len(original_topology['seam_groups'])} "
        f"start={start_mode} "
        f"solver={solver_mode} mass={mass_mode}(density={surface_density:g},"
        f"total={mass.sum():.4f},mobileMedian={np.median(mass[~fixed]):.6f}) "
        f"contact={contact_mode}(warmup={contact_warmup},"
        f"stabilizationFrames={stabilization_frames}) "
        f"colors={len(edge_batches)}/{len(hinge_batches)} "
        f"restMetric={rest_metric_mode} bendRest={bend_rest_mode} "
        f"stretchCompliance={stretch_compliance:g}(alpha={stretch_alpha:g}) "
        f"bendCompliance={bend_compliance:g}(alpha={bend_alpha:g})"
    )
    metric_ratio = raw_rest_len / base_rest_len
    bend_scale_ratio = raw_bend_scale / base_bend_scale
    print(
        "dynamic metric raw/base "
        f"mean/p50/p95/max={metric_ratio.mean():.4f}/"
        f"{np.percentile(metric_ratio,50):.4f}/"
        f"{np.percentile(metric_ratio,95):.4f}/{metric_ratio.max():.4f}"
    )
    print(
        "dynamic hinge scale raw/base "
        f"p05/p50/p95/min/max={np.percentile(bend_scale_ratio,5):.4f}/"
        f"{np.percentile(bend_scale_ratio,50):.4f}/"
        f"{np.percentile(bend_scale_ratio,95):.4f}/"
        f"{bend_scale_ratio.min():.4f}/{bend_scale_ratio.max():.4f}"
    )

    initial_t = 1.0 if start_mode == "raw-settle" else 0.0
    initial_body = cap.ref_body_verts + (
        cap.body_verts - cap.ref_body_verts
    ) * initial_t
    initial_anchor = base_verts + (raw_metric_verts - base_verts) * initial_t
    initial_rest_len = (
        base_rest_len * np.exp(initial_t * np.log(raw_rest_len / base_rest_len))
        if rest_metric_mode == "raw-metric"
        else base_rest_len
    )
    initial_bend_scale = (
        base_bend_scale
        * np.exp(initial_t * np.log(raw_bend_scale / base_bend_scale))
        if rest_metric_mode == "raw-metric"
        else base_bend_scale
    )
    if contact_mode == "on" and contact_warmup:
        warm_collider = UniformBodyCollider(initial_body, cap.body_tris)
        stretch_lambda = np.zeros(len(edges), dtype=np.float64)
        bend_lambda = np.zeros(len(hinges), dtype=np.float64)
        warm_stats = BodyContactStats()
        for _ in range(contact_warmup):
            if solver_mode == "colored-gs":
                xpbd_stretch_colored_gs(
                    x, edges, edge_batches, initial_rest_len, inv_mass,
                    stretch_lambda, compliance=stretch_alpha,
                )
            else:
                xpbd_stretch_jacobi(
                    x, edges, initial_rest_len, inv_mass, stretch_lambda,
                    compliance=stretch_alpha, relaxation=0.95,
                    tension_only=False,
                )
            if bend_rest_mode != "off":
                if solver_mode == "colored-gs":
                    xpbd_bending_colored_gs(
                        x, hinges, hinge_batches, rest_angles,
                        initial_bend_scale, inv_mass, bend_lambda,
                        compliance=bend_alpha,
                    )
                else:
                    xpbd_bending_jacobi(
                        x, hinges, rest_angles, inv_mass, bend_lambda,
                        compliance=bend_alpha, relaxation=0.45,
                        analytic_gradient=True,
                    )
            warm_stats = project_cloth_body_samples(
                x, edges, tris, inv_mass, warm_collider, contact_floors,
                contact_count,
            )
            x[fixed] = initial_anchor[fixed]
        print(
            "dynamic contact warmup "
            f"lastProjected={warm_stats.projected} "
            f"lastKinematic={warm_stats.kinematic_violations} "
            f"lastWorst={warm_stats.worst_correction:.4f} "
            f"at={warm_stats.worst_kind}:{warm_stats.worst_owner} "
            f"gap/floor={warm_stats.worst_gap:.4f}/{warm_stats.worst_floor:.4f}"
        )
    initial_x = x.copy()
    velocity.fill(0.0)

    collider: UniformBodyCollider | None = None
    previous_body = (
        cap.body_verts.copy()
        if start_mode == "raw-settle"
        else cap.ref_body_verts.copy()
    )
    stable_frames = 0
    stable_origin: np.ndarray | None = None
    converged = False
    rest_len = base_rest_len.copy()
    median_edge = float(np.median(raw_rest_len if rest_metric_mode == "raw-metric" else base_rest_len))
    for frame in range(1, frames + 1):
        stabilizing = frame <= stabilization_frames
        physical_frame = frame - stabilization_frames
        frame_start = x.copy()
        frame_contacts = 0
        worst_contact = 0.0
        worst_contact_info = "none"
        rms_speed = 0.0
        peak_rms_speed = 0.0
        t = 1.0 if start_mode == "raw-settle" else 0.0

        for substep in range(effective_substeps):
            if start_mode == "raw-settle":
                t = 1.0
            elif stabilizing:
                t = 0.0
            elif solver_mode == "colored-gs":
                # Continuous body/anchor/rest motion avoids the six impulses
                # used by the historical quasi-static spike.
                progress = (
                    physical_frame - 1 + (substep + 1) / effective_substeps
                ) / morph_frames
                t = min(1.0, progress)
            else:
                stage = min(
                    morph_steps,
                    int(np.ceil(physical_frame * morph_steps / morph_frames)),
                )
                t = min(1.0, stage / morph_steps)

            body = cap.ref_body_verts + (cap.body_verts - cap.ref_body_verts) * t
            body_velocity = (body - previous_body) / h
            collider = UniformBodyCollider(body, cap.body_tris)
            previous_body = body
            # The physical state lives on seam-reduced material nodes.  Raw is
            # used only for the moving hard supports and intrinsic edge metric;
            # mobile nodes never receive a positional pull toward raw conform.
            anchor_target = base_verts + (raw_metric_verts - base_verts) * t
            rest_len = (
                base_rest_len * np.exp(t * np.log(raw_rest_len / base_rest_len))
                if rest_metric_mode == "raw-metric"
                else base_rest_len
            )
            bend_scale = (
                base_bend_scale
                * np.exp(t * np.log(raw_bend_scale / base_bend_scale))
                if rest_metric_mode == "raw-metric"
                else base_bend_scale
            )
            previous = x.copy()

            damping = (
                np.exp(-damping_rate * h)
                if solver_mode == "colored-gs"
                else velocity_damping
            )
            velocity[active] *= damping
            if not stabilizing:
                velocity[active, 2] -= gravity_acceleration * h
            x[active] += velocity[active] * h
            x[fixed] = anchor_target[fixed]

            stretch_lambda = np.zeros(len(edges), dtype=np.float64)
            bend_lambda = np.zeros(len(hinges), dtype=np.float64)
            for constraint_iteration in range(iterations):
                if solver_mode == "colored-gs":
                    xpbd_stretch_colored_gs(
                        x, edges, edge_batches, rest_len, inv_mass,
                        stretch_lambda, compliance=stretch_alpha,
                    )
                else:
                    xpbd_stretch_jacobi(
                        x, edges, rest_len, inv_mass, stretch_lambda,
                        compliance=stretch_alpha, relaxation=0.95,
                        tension_only=False,
                    )
                if bend_rest_mode != "off":
                    if solver_mode == "colored-gs":
                        xpbd_bending_colored_gs(
                            x, hinges, hinge_batches, rest_angles, bend_scale, inv_mass,
                            bend_lambda, compliance=bend_alpha,
                        )
                    else:
                        xpbd_bending_jacobi(
                            x, hinges, rest_angles, inv_mass, bend_lambda,
                            compliance=bend_alpha, relaxation=0.45,
                            analytic_gradient=True,
                        )
                # The Python harness performs the expensive full node/edge/face
                # contact quadrature once per substep.  Constraint iterations
                # reconcile it on the following substep; a future C++ port may
                # afford interleaving every iteration without changing the law.
                if contact_mode == "on" and constraint_iteration == iterations - 1:
                    contact_stats = project_cloth_body_samples(
                        x, edges, tris, inv_mass, collider, contact_floors,
                        contact_count,
                    )
                    frame_contacts += contact_stats.projected
                    if contact_stats.worst_correction > worst_contact:
                        worst_contact = contact_stats.worst_correction
                        worst_contact_info = (
                            f"{contact_stats.worst_kind}:"
                            f"{contact_stats.worst_owner} "
                            f"gap/floor={contact_stats.worst_gap:.3f}/"
                            f"{contact_stats.worst_floor:.3f}"
                        )
                x[fixed] = anchor_target[fixed]

            if not np.all(np.isfinite(x)):
                raise RuntimeError(
                    f"dynamic-v3 produced non-finite positions at frame {frame}"
                )
            velocity[:] = (x - previous) / h
            # Rebuild the final manifold instead of reusing stale normals from
            # an earlier projection.  Only inward velocity relative to the
            # moving body is removed; tangential motion stays frictionless.
            if contact_mode == "on":
                resolve_cloth_body_velocities(
                    x, velocity, edges, tris, inv_mass, collider,
                    body_velocity, contact_floors,
                    manifold_tolerance=max(1.0e-5, 0.05 * clearance),
                )
            if stabilizing:
                # Initial overlap removal is a positional initialization, not
                # kinetic energy.  Repeated zero-velocity frames let membrane,
                # bend and contact find a compatible state before gravity/body
                # motion begins.
                velocity.fill(0.0)
            speed = np.linalg.norm(velocity[active], axis=1)
            if not stabilizing and np.any(speed > 100.0):
                if solver_mode == "colored-gs":
                    raise RuntimeError(
                        f"dynamic physical solve exceeded speed guard at frame {frame}"
                    )
                too_fast = speed > 100.0
                velocity[active[too_fast]] *= (100.0 / speed[too_fast])[:, None]
                speed[too_fast] = 100.0
            rms_speed = float(np.sqrt(np.mean(speed * speed))) if len(speed) else 0.0
            peak_rms_speed = max(peak_rms_speed, rms_speed)

        displacement = np.linalg.norm(x[active] - frame_start[active], axis=1)
        rms_displacement_ratio = (
            float(np.sqrt(np.mean(displacement * displacement))) / median_edge
            if len(displacement) else 0.0
        )
        max_displacement_ratio = (
            float(np.max(displacement)) / median_edge if len(displacement) else 0.0
        )
        local_settled = (
            not stabilizing
            and t >= 1.0
            and rms_displacement_ratio < 1.0e-3
            and max_displacement_ratio < 5.0e-3
            and peak_rms_speed * dt / median_edge < 1.0e-3
        )
        if local_settled:
            if stable_origin is None:
                stable_origin = x.copy()
            window_drift = np.linalg.norm(
                x[active] - stable_origin[active], axis=1
            )
            window_max_ratio = (
                float(np.max(window_drift)) / median_edge
                if len(window_drift) else 0.0
            )
            if window_max_ratio <= 5.0e-3:
                stable_frames += 1
            else:
                stable_frames = 1
                stable_origin = x.copy()
        else:
            stable_frames = 0
            stable_origin = None
        converged = settle_window > 0 and stable_frames >= settle_window

        if (
            frame == 1 or frame == morph_frames or frame == frames
            or frame % 15 == 0 or converged
        ):
            mean_strain, p95_ratio, max_ratio = edge_metrics(x, edges, rest_len)
            print(
                f"dynamic frame={frame}/{frames} t={t:.3f} contacts={frame_contacts} "
                f"worstContact={worst_contact:.3f}({worst_contact_info}) "
                f"rmsSpeed={rms_speed:.4f} "
                f"peakRmsSpeed={peak_rms_speed:.4f} "
                f"dispRms/Max={rms_displacement_ratio:.6f}/{max_displacement_ratio:.6f} "
                f"stable={stable_frames}/{settle_window} "
                f"meanAbsStrain={mean_strain:.5f} p95Stretch={p95_ratio:.4f} "
                f"maxStretch={max_ratio:.4f}"
            )
        if converged:
            break

    print(
        f"dynamic convergence converged={int(converged)} "
        f"stableFrames={stable_frames}/{settle_window}"
    )

    # Expand the shared material DOFs only at the I/O boundary.  This preserves
    # the original OBJ indexing while making exact seams impossible to split
    # during integration or constraint projection.
    expanded_x = x[reduced.vertex_to_node]
    expanded_fixed = fixed[reduced.vertex_to_node]
    expanded_active = ~expanded_fixed
    expanded_floor = contact_floors.node[reduced.vertex_to_node]
    output.parent.mkdir(parents=True, exist_ok=True)
    write_obj(output, expanded_x, cap.tris)
    index_err = np.linalg.norm(expanded_x - reference.verts, axis=1)
    surf = surface_distance_stats(expanded_x, reference)
    reverse_surf = surface_distance_stats(
        reference.verts, ObjMesh(expanded_x, cap.tris)
    )

    base_tri = cap.base_verts[cap.tris]
    out_tri = expanded_x[cap.tris]
    base_normal = np.cross(base_tri[:, 1] - base_tri[:, 0], base_tri[:, 2] - base_tri[:, 0])
    out_normal = np.cross(out_tri[:, 1] - out_tri[:, 0], out_tri[:, 2] - out_tri[:, 0])
    base_area2 = np.linalg.norm(base_normal, axis=1)
    out_area2 = np.linalg.norm(out_normal, axis=1)
    flips = int((np.einsum("ij,ij->i", base_normal, out_normal) < 0.0).sum())
    collapsed = int((out_area2 < 0.10 * np.maximum(base_area2, 1.0e-30)).sum())
    final_collider = UniformBodyCollider(cap.body_verts, cap.body_tris)
    final_gap = final_collider.gaps(expanded_x)
    penetrating = int((final_gap < expanded_floor - 1.0e-6).sum())
    active_penetrating = int(
        (final_gap[expanded_active] < expanded_floor[expanded_active] - 1.0e-6).sum()
    )
    fixed_penetrating = int(
        (final_gap[expanded_fixed] < expanded_floor[expanded_fixed] - 1.0e-6).sum()
    )
    sample_diagnostics: dict[str, float | int] = {}
    if contact_mode == "on":
        sample_diagnostics = cloth_body_sample_diagnostics(
            x, edges, tris, inv_mass, final_collider, contact_floors
        )
    final_mean_strain, final_p95_stretch, final_max_stretch = edge_metrics(
        x, edges, rest_len
    )
    final_edge_length = np.linalg.norm(
        x[edges[:, 0]] - x[edges[:, 1]], axis=1
    )
    final_min_ratio = float(np.min(final_edge_length / rest_len))
    finite_ok = bool(np.all(np.isfinite(expanded_x)))
    sample_finite_ok = all(
        int(sample_diagnostics.get(f"{kind}_nonfinite", 0)) == 0
        for kind in ("node", "edge", "face")
    )
    sample_mobile_ok = all(
        int(sample_diagnostics.get(f"{kind}_movable_violations", 0)) == 0
        for kind in ("node", "edge", "face")
    )
    physical_core_accepted = bool(
        converged
        and finite_ok
        and flips == 0
        and collapsed == 0
        and active_penetrating == 0
        and sample_finite_ok
        and sample_mobile_ok
        and final_p95_stretch <= 1.10
        and final_max_stretch <= 4.10
        and final_min_ratio >= 0.10
    )
    if contact_mode == "off":
        # Contact-off exists only for causal invariance checks.  It is never a
        # shippable physical candidate, but may return success when it preserves
        # its input pose exactly and converges.
        diagnostic_accepted = bool(
            converged
            and finite_ok
            and np.max(np.linalg.norm(x - initial_x, axis=1)) <= 1.0e-9
        )
    else:
        diagnostic_accepted = physical_core_accepted
    print(
        f"dynamic-result={output} reference={reference_label} "
        f"indexError mean/p95/max={index_err.mean():.4f}/"
        f"{np.percentile(index_err,95):.4f}/{index_err.max():.4f} "
        f"surface result->reference mean/p95/max={surf[0]:.4f}/{surf[1]:.4f}/{surf[2]:.4f} "
        f"reference->result={reverse_surf[0]:.4f}/{reverse_surf[1]:.4f}/{reverse_surf[2]:.4f} "
        f"flips={flips} collapsed={collapsed} "
        f"floorViolations={penetrating}(active={active_penetrating},fixed={fixed_penetrating}) "
        f"strainMean/p95/max/min={final_mean_strain:.5f}/"
        f"{final_p95_stretch:.4f}/{final_max_stretch:.4f}/{final_min_ratio:.4f} "
        f"everContactedNodes={int((contact_count > 0).sum())}"
    )
    if sample_diagnostics:
        print(
            "dynamic contact samples "
            + " ".join(
                f"{key}={value}" for key, value in sorted(sample_diagnostics.items())
            )
        )
    print(
        "dynamic acceptance "
        f"coreAccepted={int(physical_core_accepted)} "
        f"diagnosticAccepted={int(diagnostic_accepted)} "
        "(core excludes self-collision/crossing and is not a deploy gate)"
    )
    return diagnostic_accepted


def inspect_dataset(capture_path: Path, dataset: Path) -> None:
    cap = read_capture_front(capture_path)
    names = [
        "cloth_base.obj",
        "cloth_vanilla.obj",
        "cloth_current.obj",
        "cloth_captured_expected.obj",
        "cloth_sculpt.obj",
        "body_base.obj",
        "body_morphed.obj",
    ]
    meshes = {name: load_obj(dataset / name) for name in names}

    cloth_names = names[:5]
    base = meshes["cloth_base.obj"]
    for name in cloth_names[1:]:
        mesh = meshes[name]
        if len(mesh.verts) != len(base.verts) or not np.array_equal(mesh.tris, base.tris):
            raise ValueError(f"cloth topology mismatch: {name}")

    if not np.array_equal(meshes["body_base.obj"].tris, meshes["body_morphed.obj"].tris):
        raise ValueError("base/morphed body topology mismatch")
    if not np.array_equal(cap.tris, base.tris):
        raise ValueError("capture/OBJ cloth topology mismatch")
    if not np.array_equal(cap.body_tris, cap.ref_body_tris):
        raise ValueError("capture base/morphed body topology mismatch")

    topo = topology_stats(base.verts, base.tris)
    drape = fitted_drape_signal(cap)

    print(f"capture={capture_path}")
    print(f"version={cap.version} shape={cap.shape_name!r} slider={cap.slider_name!r} preset={cap.preset}")
    print(f"cloth verts={len(base.verts)} tris={len(base.tris)} captureEdges={len(cap.edges)}")
    print(
        f"topology uniqueEdges={len(topo['edge_faces'])} hinges={len(topo['hinges'])} "
        f"boundary={topo['boundary_edges']} nonmanifold={topo['nonmanifold_edges']}"
    )
    print(f"components={len(topo['component_sizes'])} largest={topo['component_sizes'][:12]}")
    print(
        f"exactSeamGroups={len(topo['seam_groups'])} "
        f"seamVerts={sum(len(g) for g in topo['seam_groups'])} "
        f"largestSeams={sorted((len(g) for g in topo['seam_groups']), reverse=True)[:12]}"
    )
    print(
        f"fittedClassifier anchored(<0.20)={(drape < 0.20).sum()} "
        f"transition={(np.logical_and(drape >= 0.20, drape < 0.55)).sum()} "
        f"free(>=0.55)={(drape >= 0.55).sum()}"
    )
    print(f"cloth bbox {bbox_text(base.verts)}")
    print(f"sculpt bbox {bbox_text(meshes['cloth_sculpt.obj'].verts)}")
    print(f"body0 bbox {bbox_text(cap.ref_body_verts)}")
    print(f"body1 bbox {bbox_text(cap.body_verts)}")
    print(f"max |OBJ base - capture base|={np.linalg.norm(base.verts - cap.base_verts, axis=1).max():.9g}")
    print(
        "max |OBJ body0 - capture body0|="
        f"{np.linalg.norm(meshes['body_base.obj'].verts - cap.ref_body_verts, axis=1).max():.9g}"
    )
    for name in names:
        print(f"sha256 {name} {sha256(dataset / name)}")


def contact_check(capture_path: Path) -> None:
    cap = read_capture_front(capture_path)
    collider = UniformBodyCollider(cap.ref_body_verts, cap.ref_body_tris)
    indices = np.arange(len(cap.base_verts), dtype=np.int32)
    gaps = collider.gaps(cap.base_verts, indices)
    finite = np.isfinite(gaps)
    print(
        f"base contact finite={finite.sum()}/{len(gaps)} inside={(gaps < 0).sum()} "
        f"gap[min/p05/p50]={np.nanmin(gaps):.4f}/"
        f"{np.nanpercentile(gaps, 5):.4f}/{np.nanmedian(gaps):.4f}"
    )

    # Validate the uniform-grid broad phase against a brute-force exact query on
    # deterministic cloth samples. This catches a too-small search neighborhood.
    regular_sample = np.linspace(0, len(cap.base_verts) - 1, 32, dtype=np.int32)
    finite_indices = indices[finite]
    far_count = min(32, len(finite_indices))
    far_sample = (
        finite_indices[np.argsort(np.abs(gaps[finite]))[-far_count:]]
        if far_count
        else np.empty(0, dtype=np.int32)
    )
    sample = np.unique(np.concatenate((regular_sample, far_sample))).astype(np.int32)
    tri_v = cap.ref_body_verts[cap.ref_body_tris]
    worst_dist_error = 0.0
    tri_mismatches = 0
    for vi in sample:
        p = cap.base_verts[vi]
        fast = collider.closest(p)
        cps = closest_points_triangles(p, tri_v[:, 0], tri_v[:, 1], tri_v[:, 2])
        d2 = np.einsum("ij,ij->i", cps - p, cps - p)
        brute_tri = int(np.argmin(d2))
        brute_dist = float(np.sqrt(d2[brute_tri]))
        if fast is None:
            raise RuntimeError(f"broad phase found no candidate for sample vertex {vi}")
        worst_dist_error = max(worst_dist_error, abs(abs(fast[2]) - brute_dist))
        if fast[3] != brute_tri and abs(abs(fast[2]) - brute_dist) > 1.0e-7:
            tri_mismatches += 1
    print(
        f"broadphase sample={len(sample)} (regular+farthest) "
        f"worstDistanceError={worst_dist_error:.9g} "
        f"materialTriMismatches={tri_mismatches}"
    )

    projected = cap.base_verts.copy()
    moved, worst = collider.project(projected, indices, clearance=0.02)
    after = collider.gaps(projected, indices)
    print(
        f"base projection clearance=0.02 moved={moved} worstMove={worst:.4f} "
        f"remainingInside={(after < -1.0e-6).sum()} minGap={np.nanmin(after):.6f}"
    )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", type=Path, default=DEFAULT_CAPTURE)
    ap.add_argument("--dataset", type=Path, default=DEFAULT_DATASET)
    ap.add_argument("--inspect", action="store_true", help="validate and report the frozen dataset")
    ap.add_argument("--contact-check", action="store_true", help="validate offline body contact queries")
    ap.add_argument(
        "--attachment-selftest", action="store_true",
        help="validate surface-corot authored closure and rigid transport",
    )
    ap.add_argument(
        "--attachment-report", action="store_true",
        help="report inferred surface-corot support bindings without solving",
    )
    ap.add_argument(
        "--solver-selftest", action="store_true",
        help="validate the unilateral XPBD stretch multiplier projection",
    )
    ap.add_argument("--solve", action="store_true", help="run the experimental quasi-static shell solve")
    ap.add_argument(
        "--solve-dynamic", action="store_true",
        help="run the isolated Path-B v3 time-integrated physical settle",
    )
    ap.add_argument("--output", type=Path, default=DEFAULT_DATASET / "shell_base.obj")
    ap.add_argument("--rest", choices=("base", "uniform", "lowpass"), default="base")
    ap.add_argument("--bend-rest", choices=("base", "flat-panel"), default="base")
    ap.add_argument("--growth-blend", type=float, default=1.0)
    ap.add_argument("--morph-steps", type=int, default=6)
    ap.add_argument("--iterations", type=int, default=10)
    ap.add_argument("--stretch-compliance", type=float, default=2.0e-4)
    ap.add_argument("--bend-compliance", type=float, default=2.0e-2)
    ap.add_argument(
        "--meridian-compliance", type=float, default=-1.0,
        help="enable straight-generator energy when >=0 (negative disables)",
    )
    ap.add_argument("--shadow-source", choices=("none", "body", "contact"), default="none")
    ap.add_argument("--shadow-cell", type=float, default=1.0)
    ap.add_argument("--shadow-strength", type=float, default=0.45)
    ap.add_argument("--shadow-clearance", type=float, default=0.08)
    ap.add_argument("--shadow-max-ring", type=int, default=24)
    ap.add_argument("--shadow-contact-rings", type=int, default=0)
    ap.add_argument(
        "--shadow-contact-falloff", choices=("binary", "linear"), default="binary"
    )
    ap.add_argument("--shadow-search", choices=("nearest", "radial"), default="nearest")
    ap.add_argument("--graph-plumb-strength", type=float, default=0.0)
    ap.add_argument("--graph-plumb-return", type=float, default=0.0)
    ap.add_argument(
        "--graph-plumb-order",
        choices=("pre-stretch", "post-contact"),
        default="post-contact",
    )
    ap.add_argument(
        "--graph-plumb-seeds",
        choices=("all", "contact", "contact-tension"),
        default="all",
    )
    ap.add_argument("--band-circumference-compliance", type=float, default=-1.0)
    ap.add_argument("--band-z-cell", type=float, default=2.5)
    ap.add_argument("--band-rest", choices=("base", "vanilla"), default="base")
    ap.add_argument("--gravity", type=float, default=0.0)
    ap.add_argument("--clearance", type=float, default=0.02)
    # S0.3c causal ablation factors (default = current behaviour)
    ap.add_argument(
        "--anchor-target",
        choices=("vanilla", "authored", "surface-corot"),
        default="vanilla",
    )
    ap.add_argument(
        "--surface-corot-clamp", choices=("on", "off"), default="on",
        help="clamp surface-corot hard targets to authored contact floors",
    )
    ap.add_argument(
        "--surface-corot-mask-json", type=Path,
        help="consume a capture-hashed support mask frozen by the S04 preflight",
    )
    ap.add_argument(
        "--surface-corot-classifier", default="coherent-v1",
        help="classifier record to select from --surface-corot-mask-json",
    )
    ap.add_argument(
        "--surface-corot-fallback",
        choices=("authored", "vanilla", "body-vanilla"),
        default="authored",
        help=("target for fixed/soft vertices outside the selected corot mask; "
              "body-vanilla is authored for a static body and vanilla during continuation"),
    )
    ap.add_argument("--soft-anchor", choices=("on", "off"), default="on")
    ap.add_argument("--body-mode", choices=("continuation", "static"), default="continuation")
    ap.add_argument("--gravity-mode", choices=("step", "iteration"), default="step")
    ap.add_argument("--stretch-mode", choices=("bilateral", "tension"), default="bilateral")
    ap.add_argument("--dynamic-frames", type=int, default=90)
    ap.add_argument("--dynamic-morph-frames", type=int, default=30)
    ap.add_argument("--dynamic-morph-steps", type=int, default=6)
    ap.add_argument("--dynamic-iterations", type=int, default=8)
    ap.add_argument("--dynamic-substeps", type=int, default=2)
    ap.add_argument("--dynamic-dt", type=float, default=1.0 / 30.0)
    ap.add_argument("--dynamic-gravity", type=float, default=10.0)
    ap.add_argument("--dynamic-damping", type=float, default=0.80)
    ap.add_argument("--dynamic-damping-rate", type=float, default=5.0)
    ap.add_argument("--dynamic-settle-window", type=int, default=30)
    ap.add_argument("--dynamic-stretch-compliance", type=float, default=2.0e-7)
    ap.add_argument("--dynamic-bend-compliance", type=float, default=2.0e-5)
    ap.add_argument(
        "--dynamic-rest", choices=("base", "raw-metric"), default="raw-metric",
        help=(
            "dynamic in-plane rest metric: authored base lengths, or lengths "
            "interpolated to raw conform without copying raw positions"
        ),
    )
    ap.add_argument(
        "--dynamic-start", choices=("continuation", "raw-settle"),
        default="continuation",
        help="continue base/body morph, or physically settle from the final raw pose",
    )
    ap.add_argument(
        "--dynamic-solver", choices=("legacy-jacobi", "colored-gs"),
        default="colored-gs",
        help="legacy valence-averaged projection or mathematically consistent colored GS",
    )
    ap.add_argument(
        "--dynamic-mass", choices=("uniform", "area"), default="area",
        help="uniform mobile mass or triangle-area lumped mass",
    )
    ap.add_argument(
        "--dynamic-surface-density", type=float, default=1.0,
        help="absolute shell surface density used by the dynamic mass model",
    )
    ap.add_argument(
        "--dynamic-contact-warmup", type=int, default=8,
        help="zero-velocity constraint/contact iterations for the initial pose",
    )
    ap.add_argument(
        "--dynamic-stabilization-frames", type=int, default=4,
        help="initial zero-velocity frames before gravity/body continuation",
    )
    ap.add_argument(
        "--dynamic-contact", choices=("on", "off"), default="on",
        help=(
            "sampled unilateral body contact on nodes, unique edge midpoints "
            "and face centroids; off is reserved for invariance tests"
        ),
    )
    ap.add_argument(
        "--dynamic-bend-rest", choices=("base", "flat-panel", "off"),
        default="base",
    )
    ap.add_argument(
        "--trace-json", type=Path,
        help="write per-vertex S0.3e contact/fixed history for causal analysis",
    )
    args = ap.parse_args()

    if not any((args.inspect, args.contact_check, args.attachment_selftest,
                args.attachment_report, args.solver_selftest, args.solve,
                args.solve_dynamic)):
        ap.error(
            "choose --inspect, --contact-check, --attachment-selftest, "
            "--attachment-report, --solver-selftest, --solve and/or --solve-dynamic"
        )
    if args.surface_corot_mask_json and args.anchor_target != "surface-corot":
        ap.error("--surface-corot-mask-json requires --anchor-target surface-corot")
    if args.inspect:
        inspect_dataset(args.capture.resolve(), args.dataset.resolve())
    if args.contact_check:
        contact_check(args.capture.resolve())
    if args.attachment_selftest:
        attachment_selftest()
    if args.attachment_report:
        attachment_report(args.capture.resolve())
    if args.solver_selftest:
        xpbd_unilateral_selftest()
        dihedral_gradient_selftest()
        colored_gs_selftest()
        body_contact_selftest()
    dynamic_accepted = True
    if args.solve_dynamic:
        dynamic_accepted = solve_shell_dynamic(
            args.capture.resolve(),
            args.dataset.resolve(),
            args.output.resolve(),
            max(1, args.dynamic_frames),
            max(1, args.dynamic_morph_frames),
            max(1, args.dynamic_morph_steps),
            max(1, args.dynamic_iterations),
            max(1, args.dynamic_substeps),
            args.dynamic_dt,
            args.dynamic_gravity,
            args.dynamic_damping,
            args.dynamic_damping_rate,
            max(0.0, args.dynamic_stretch_compliance),
            max(0.0, args.dynamic_bend_compliance),
            args.clearance,
            args.dynamic_rest,
            args.dynamic_start,
            args.dynamic_solver,
            args.dynamic_mass,
            args.dynamic_surface_density,
            max(0, args.dynamic_contact_warmup),
            max(0, args.dynamic_stabilization_frames),
            max(0, args.dynamic_settle_window),
            args.dynamic_contact,
            args.dynamic_bend_rest,
        )
    if args.solve:
        solve_shell(
            args.capture.resolve(),
            args.dataset.resolve(),
            args.output.resolve(),
            args.rest,
            args.bend_rest,
            args.growth_blend,
            max(1, args.morph_steps),
            max(1, args.iterations),
            max(0.0, args.stretch_compliance),
            max(0.0, args.bend_compliance),
            args.meridian_compliance,
            args.shadow_source,
            max(0.25, args.shadow_cell),
            float(np.clip(args.shadow_strength, 0.0, 1.0)),
            max(0.0, args.shadow_clearance),
            max(1, args.shadow_max_ring),
            max(0, args.shadow_contact_rings),
            args.shadow_contact_falloff,
            args.shadow_search,
            float(np.clip(args.graph_plumb_strength, 0.0, 1.0)),
            max(0.0, args.graph_plumb_return),
            args.graph_plumb_order,
            args.graph_plumb_seeds,
            args.band_circumference_compliance,
            max(0.5, args.band_z_cell),
            args.band_rest,
            args.gravity,
            args.clearance,
            anchor_target_mode=args.anchor_target,
            soft_anchor_mode=args.soft_anchor,
            body_mode=args.body_mode,
            gravity_mode=args.gravity_mode,
            stretch_mode=args.stretch_mode,
            trace_json=args.trace_json.resolve() if args.trace_json else None,
            surface_corot_clamp=args.surface_corot_clamp == "on",
            surface_corot_mask_json=(
                args.surface_corot_mask_json.resolve()
                if args.surface_corot_mask_json else None
            ),
            surface_corot_classifier=args.surface_corot_classifier,
            surface_corot_fallback=args.surface_corot_fallback,
        )
    return 0 if dynamic_accepted else 4


if __name__ == "__main__":
    raise SystemExit(main())
