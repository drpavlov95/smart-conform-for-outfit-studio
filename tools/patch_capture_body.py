#!/usr/bin/env python3
"""Replace the morphed body mesh in a Drape replay capture.

This is a diagnostic utility for cases where an Outfit Studio body export is
newer than the frozen ``.drcap``.  It preserves every other capture field and,
when possible, rebuilds the radial BodyShell from the capture's reference body
and the replacement morphed body.

The input capture is never modified in place.
"""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from shell_spike import load_obj


MAGIC = 0x43505244  # DRPC


class Cursor:
    def __init__(self, data: bytearray):
        self.data = data
        self.off = 0

    def unpack(self, fmt: str):
        size = struct.calcsize("<" + fmt)
        if self.off + size > len(self.data):
            raise ValueError(f"capture truncated at byte {self.off}")
        values = struct.unpack_from("<" + fmt, self.data, self.off)
        self.off += size
        return values[0] if len(values) == 1 else values

    def skip(self, size: int) -> int:
        start = self.off
        self.off += size
        if self.off > len(self.data):
            raise ValueError(f"capture truncated at byte {start}")
        return start

    def string(self) -> str:
        size = self.unpack("I")
        start = self.skip(size)
        return bytes(self.data[start : start + size]).decode("utf-8", "replace")

    def vec_span(self, item_size: int) -> tuple[int, int]:
        count = self.unpack("I")
        start = self.skip(count * item_size)
        return count, start


@dataclass
class ShellSpan:
    radial_bins: int
    z_bins: int
    min_z_off: int
    max_z_off: int
    inv_dz_off: int
    base_count: int
    base_off: int
    morph_count: int
    morph_off: int
    occupied_count: int
    occupied_off: int
    valid_off: int


def read_vec3(data: bytearray, count: int, offset: int) -> np.ndarray:
    values = np.frombuffer(data, dtype="<f4", count=count * 3, offset=offset)
    return values.reshape(count, 3).astype(np.float64)


def read_tris(data: bytearray, count: int, offset: int) -> np.ndarray:
    values = np.frombuffer(data, dtype="<u2", count=count * 3, offset=offset)
    return values.reshape(count, 3).astype(np.int32)


def parse_front(data: bytearray):
    r = Cursor(data)
    magic, version = r.unpack("II")
    if magic != MAGIC or not 1 <= version <= 3:
        raise ValueError(f"unsupported capture magic/version: {magic:#x}/{version}")

    shape = r.string()
    slider = r.string()
    project = r.string()
    _preset = r.unpack("i")
    _intensity = r.unpack("i")
    center_x, center_y, _min_z, _max_z = r.unpack("ffff")

    r.vec_span(12)  # cloth base vertices
    r.vec_span(36)  # VertexBoneProfile: 8 floats + int
    r.vec_span(8)  # EdgeConstraint: 2 uint16 + float
    if version >= 2:
        r.vec_span(8)  # BendingConstraint: 2 uint16 + float
    r.vec_span(6)  # cloth triangles

    shell = None
    has_shell = bool(r.unpack("?"))
    if has_shell:
        radial_bins = r.unpack("i")
        z_bins = r.unpack("i")
        min_z_off = r.off
        r.unpack("f")
        max_z_off = r.off
        r.unpack("f")
        inv_dz_off = r.off
        r.unpack("f")
        base_count, base_off = r.vec_span(4)
        morph_count, morph_off = r.vec_span(4)
        occupied_count, occupied_off = r.vec_span(1)
        valid_off = r.off
        r.unpack("?")
        shell = ShellSpan(
            radial_bins,
            z_bins,
            min_z_off,
            max_z_off,
            inv_dz_off,
            base_count,
            base_off,
            morph_count,
            morph_off,
            occupied_count,
            occupied_off,
            valid_off,
        )

    if not bool(r.unpack("?")):
        raise ValueError("capture does not contain a morphed body BVH")
    body_count, body_off = r.vec_span(12)
    body_tri_count, body_tri_off = r.vec_span(6)

    ref_count = 0
    ref_off = 0
    ref_tri_count = 0
    ref_tri_off = 0
    if bool(r.unpack("?")):
        ref_count, ref_off = r.vec_span(12)
        ref_tri_count, ref_tri_off = r.vec_span(6)

    return {
        "version": version,
        "shape": shape,
        "slider": slider,
        "project": project,
        "center": (float(center_x), float(center_y)),
        "shell": shell,
        "body_count": body_count,
        "body_off": body_off,
        "body_tri_count": body_tri_count,
        "body_tri_off": body_tri_off,
        "ref_count": ref_count,
        "ref_off": ref_off,
        "ref_tri_count": ref_tri_count,
        "ref_tri_off": ref_tri_off,
    }


def rebuild_shell(
    data: bytearray,
    span: ShellSpan,
    reference: np.ndarray,
    morphed: np.ndarray,
    center: tuple[float, float],
) -> None:
    cells = span.radial_bins * span.z_bins
    if (
        span.base_count != cells
        or span.morph_count != cells
        or span.occupied_count != cells
    ):
        raise ValueError("BodyShell vector sizes do not match its bin dimensions")
    if len(reference) != len(morphed):
        raise ValueError("reference and morphed bodies have different vertex counts")

    min_z = float(reference[:, 2].min())
    max_z = float(reference[:, 2].max())
    z_span = max(1.0, max_z - min_z)
    min_z -= z_span * 0.03
    max_z += z_span * 0.03
    inv_dz = (span.z_bins - 1) / max(1.0e-4, max_z - min_z)

    base_r = np.full(cells, -1.0, dtype=np.float32)
    morph_r = np.full(cells, -1.0, dtype=np.float32)
    occupied = np.zeros(cells, dtype=np.uint8)
    cx, cy = center
    inserted = 0
    two_pi = 2.0 * np.pi

    for base, current in zip(reference, morphed):
        dx = float(base[0] - cx)
        dy = float(base[1] - cy)
        radius = float(np.hypot(dx, dy))
        if radius <= 1.0e-5:
            continue
        angle = float(np.arctan2(dy / radius, dx / radius))
        radial_bin = int(((angle + np.pi) / two_pi) * span.radial_bins)
        radial_bin = max(0, min(span.radial_bins - 1, radial_bin))
        z_bin = int((float(base[2]) - min_z) * inv_dz)
        z_bin = max(0, min(span.z_bins - 1, z_bin))
        index = z_bin * span.radial_bins + radial_bin
        current_r = float(np.hypot(current[0] - cx, current[1] - cy))
        base_r[index] = max(float(base_r[index]), radius)
        morph_r[index] = max(float(morph_r[index]), current_r)
        occupied[index] = 1
        inserted += 1

    struct.pack_into("<fff", data, span.min_z_off, min_z, max_z, inv_dz)
    data[span.base_off : span.base_off + cells * 4] = base_r.astype("<f4").tobytes()
    data[span.morph_off : span.morph_off + cells * 4] = morph_r.astype("<f4").tobytes()
    data[span.occupied_off : span.occupied_off + cells] = occupied.tobytes()
    struct.pack_into("<?", data, span.valid_off, inserted > 0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("--body-obj", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--scale",
        type=float,
        default=10.0,
        help="OBJ-to-capture coordinate scale (Outfit Studio exports use 10)",
    )
    parser.add_argument(
        "--no-refresh-shell",
        action="store_true",
        help="replace only the exact body BVH and leave BodyShell unchanged",
    )
    args = parser.parse_args()

    source = args.capture.resolve()
    output = args.output.resolve()
    if source == output:
        raise ValueError("refusing to modify the input capture in place")

    data = bytearray(source.read_bytes())
    info = parse_front(data)
    replacement = load_obj(args.body_obj)
    verts = replacement.verts * float(args.scale)

    if len(verts) != info["body_count"]:
        raise ValueError(
            f"body vertex count mismatch: OBJ={len(verts)} capture={info['body_count']}"
        )
    captured_tris = read_tris(data, info["body_tri_count"], info["body_tri_off"])
    if not np.array_equal(replacement.tris, captured_tris):
        raise ValueError("body OBJ topology/order does not match the capture")

    packed = np.asarray(verts, dtype="<f4").tobytes()
    start = info["body_off"]
    data[start : start + len(packed)] = packed

    refreshed = False
    if not args.no_refresh_shell and info["shell"] is not None:
        if info["ref_count"] != len(verts):
            raise ValueError("reference body count does not match replacement body")
        ref_tris = read_tris(data, info["ref_tri_count"], info["ref_tri_off"])
        if not np.array_equal(ref_tris, replacement.tris):
            raise ValueError("reference-body topology/order does not match replacement body")
        reference = read_vec3(data, info["ref_count"], info["ref_off"])
        rebuild_shell(data, info["shell"], reference, verts, info["center"])
        refreshed = True

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)
    print(
        f"wrote {output} shape={info['shape']} slider={info['slider']} "
        f"body={len(verts)} tris={len(replacement.tris)} scale={args.scale:g} "
        f"shell_refreshed={int(refreshed)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
