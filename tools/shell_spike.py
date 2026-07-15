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
import struct
from dataclasses import dataclass
from pathlib import Path

import numpy as np

try:
    from scipy.spatial import cKDTree
except ImportError:  # the exact expanding-grid fallback below remains valid
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
        out = np.full(len(indices), np.nan, dtype=np.float64)
        for oi, vi in enumerate(indices):
            result = self.closest(verts[vi])
            if result is not None:
                out[oi] = result[2]
        return out


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
    authored_fallback: np.ndarray,
) -> np.ndarray:
    """Transport support targets without copying body stretch/shear into offset."""
    # This is both an invariant and a numerical guard: a static body must make
    # surface-corot exactly identical to authored, not merely close enough for
    # an equidistant nearest-face tie to change contact sign.
    if np.array_equal(body_reference, body_current):
        return authored_fallback.copy()
    target = authored_fallback.copy()
    indices = np.flatnonzero(attachments.valid)
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
    closure = transport_surface_attachments(bindings, body, body, tris, cloth)
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
    transported = transport_surface_attachments(bindings, body, moved_body, tris, cloth)
    expected = cloth @ rotation.T + translation
    rigid_error = float(np.linalg.norm(transported - expected, axis=1).max())
    if rigid_error > 1.0e-9:
        raise RuntimeError(f"surface-corot rigid-motion test failed: {rigid_error}")
    print(
        f"attachment selftest PASS closure={closure_error:.3g} "
        f"rigidMotion={rigid_error:.3g} staticClamp=no-op "
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


def xpbd_bending_jacobi(
    verts: np.ndarray,
    hinges: np.ndarray,
    rest_angles: np.ndarray,
    inv_mass: np.ndarray,
    lambdas: np.ndarray,
    compliance: float,
    relaxation: float,
) -> None:
    if len(hinges) == 0:
        return
    local = verts[hinges]
    angle = signed_dihedrals_local(local)
    c = wrap_angle(angle - rest_angles)
    grad = dihedral_gradients_numeric(local)
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
    trace_json: Path | None = None,
    surface_corot_clamp: bool = True,
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
            anchor_target = transport_surface_attachments(
                surface_attachments,
                cap.ref_body_verts,
                body,
                cap.body_tris,
                cap.base_verts,
            )
            if surface_corot_clamp:
                clamp_count, clamp_worst = clamp_targets_to_contact_floor(
                    anchor_target,
                    np.flatnonzero(fixed).astype(np.int32),
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
        if gravity != 0.0:
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
                "surface_corot_clamp": surface_corot_clamp,
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
    ap.add_argument("--solve", action="store_true", help="run the experimental quasi-static shell solve")
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
    ap.add_argument("--soft-anchor", choices=("on", "off"), default="on")
    ap.add_argument("--body-mode", choices=("continuation", "static"), default="continuation")
    ap.add_argument(
        "--trace-json", type=Path,
        help="write per-vertex S0.3e contact/fixed history for causal analysis",
    )
    args = ap.parse_args()

    if not any((args.inspect, args.contact_check, args.attachment_selftest, args.attachment_report, args.solve)):
        ap.error(
            "choose --inspect, --contact-check, --attachment-selftest, "
            "--attachment-report and/or --solve"
        )
    if args.inspect:
        inspect_dataset(args.capture.resolve(), args.dataset.resolve())
    if args.contact_check:
        contact_check(args.capture.resolve())
    if args.attachment_selftest:
        attachment_selftest()
    if args.attachment_report:
        attachment_report(args.capture.resolve())
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
            trace_json=args.trace_json.resolve() if args.trace_json else None,
            surface_corot_clamp=args.surface_corot_clamp == "on",
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
