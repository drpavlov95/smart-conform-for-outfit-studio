#!/usr/bin/env python3
"""Run corrected S0.3e section diagnostics on production and Path-B shell.

All garments use one explicit shell configuration.  Production and shell are
generated fail-closed in a fresh work directory, then compared side by side to
the frozen authored sculpt using ``pathb_band_distribution``.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

import numpy as np

from pathb_band_distribution import analyze_arrays
from pathb_band_descriptor import load_obj
from pathb_gate import HARNESS, ROOT, SENTINELS


def file_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def relative(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(ROOT.resolve())).replace("\\", "/")
    except ValueError:
        return str(path.resolve())


def run_checked(
    command: list[str],
    outputs: list[Path],
    label: str,
    accepted_returncodes: tuple[int, ...] = (0,),
) -> dict[str, object]:
    for output in outputs:
        if output.exists():
            output.unlink()
    completed = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    missing = [str(output) for output in outputs if not output.is_file() or output.stat().st_size == 0]
    if missing or completed.returncode not in accepted_returncodes:
        tail = (completed.stderr or completed.stdout).strip().splitlines()[-12:]
        raise RuntimeError(
            f"{label} failed rc={completed.returncode} missing={missing}\n" + "\n".join(tail)
        )
    return {
        "command": [relative(Path(part)) if Path(part).is_absolute() else part for part in command],
        "returncode": completed.returncode,
        "outputs": {
            relative(output): file_sha256(output) for output in outputs
        },
    }


def validate_topology(
    label: str,
    base_triangles: np.ndarray,
    vertices: np.ndarray,
    triangles: np.ndarray,
    expected_vertices: int,
) -> None:
    if len(vertices) != expected_vertices:
        raise ValueError(f"{label}: vertex count {len(vertices)} != {expected_vertices}")
    if not np.array_equal(base_triangles, triangles):
        raise ValueError(f"{label}: triangle topology differs from authored base")
    if not np.isfinite(vertices).all():
        raise ValueError(f"{label}: NaN or infinity in vertices")


def analyze_garment(
    garment: str,
    config: dict[str, object],
    workdir: Path,
    levels: int,
    morph_steps: int,
    iterations: int,
) -> dict[str, object]:
    garment_dir = workdir / garment
    dataset = garment_dir / "dataset"
    production = garment_dir / "production.obj"
    shell = garment_dir / "shell_base.obj"
    garment_dir.mkdir(parents=True, exist_ok=True)
    capture = ROOT / config["capture"]
    production_run = run_checked(
        [str(HARNESS), "--replay", str(capture), "--result-obj", str(production)],
        [production],
        f"{garment}/production",
        # The production harness deliberately returns 4 when diagnostics fail
        # its internal golden-style gates, while still writing the requested
        # candidate OBJ.  No other non-zero code is accepted here.
        accepted_returncodes=(0, 4),
    )
    # The spike exports base/body files only when cloth_vanilla is absent.  A
    # partial stale dataset must therefore be cleared as one fail-closed unit.
    for dataset_name in (
        "cloth_base.obj", "cloth_vanilla.obj", "body_base.obj", "body_morphed.obj"
    ):
        stale = dataset / dataset_name
        if stale.exists():
            stale.unlink()
    shell_run = run_checked(
        [
            sys.executable,
            str(ROOT / "tools/shell_spike.py"),
            "--capture", str(capture),
            "--dataset", str(dataset),
            "--solve",
            "--output", str(shell),
            "--rest", "base",
            "--morph-steps", str(morph_steps),
            "--iterations", str(iterations),
            "--gravity", "0",
            "--anchor-target", "vanilla",
            "--soft-anchor", "on",
            "--body-mode", "continuation",
        ],
        [shell, dataset / "cloth_base.obj"],
        f"{garment}/shell",
    )
    ground_truth_path = Path(config["gt"])
    base_path = dataset / "cloth_base.obj"
    base_vertices, triangles = load_obj(str(base_path))
    sculpt_vertices, sculpt_triangles = load_obj(str(ground_truth_path))
    production_vertices, production_triangles = load_obj(str(production))
    shell_vertices, shell_triangles = load_obj(str(shell))
    validate_topology("sculpt", triangles, sculpt_vertices, sculpt_triangles, len(base_vertices))
    validate_topology("production", triangles, production_vertices, production_triangles, len(base_vertices))
    validate_topology("shell_base", triangles, shell_vertices, shell_triangles, len(base_vertices))
    level_values = [float(value) for value in np.linspace(0.15, 0.85, max(1, levels))]
    diagnostics = analyze_arrays(
        base_vertices,
        triangles,
        sculpt_vertices,
        {"production": production_vertices, "shell_base": shell_vertices},
        level_values,
    )
    return {
        "role": config["role"],
        "capture": {"path": relative(capture), "sha256": file_sha256(capture)},
        "base": {"path": relative(base_path), "sha256": file_sha256(base_path)},
        "ground_truth": {
            "path": relative(ground_truth_path),
            "sha256": file_sha256(ground_truth_path),
        },
        "candidates": {
            "production": {"path": relative(production), "sha256": file_sha256(production)},
            "shell_base": {"path": relative(shell), "sha256": file_sha256(shell)},
        },
        "producer_journal": {"production": production_run, "shell_base": shell_run},
        "diagnostics": diagnostics,
    }


def summarize(result: dict[str, object]) -> None:
    print("# S0.3e production vs shell material-section summary")
    for garment, data in result["garments"].items():
        print(f"\n{garment} ({data['role']}):")
        rows_by_candidate: dict[str, list[dict[str, object]]] = {}
        for level in data["diagnostics"]["levels"].values():
            for candidate, row in level["candidates"].items():
                rows_by_candidate.setdefault(candidate, []).append(row)
        for candidate, rows in rows_by_candidate.items():
            down = [float(row["delta_down"]) for row in rows]
            shape_p = [float(row["shape_perp_rms"]) for row in rows]
            shape_a = [float(row["shape_axial_rms"]) for row in rows]
            widths = [float(row["width_ratio"]) for row in rows]
            incomplete = sum(int(row["sector_coverage_common"]) < 8 for row in rows)
            print(
                f"  {candidate}: dDown=[{min(down):+.3f},{max(down):+.3f}] "
                f"width=[{min(widths):.3f},{max(widths):.3f}] "
                f"shapePmax={max(shape_p):.3f} shapeAmax={max(shape_a):.3f} "
                f"incompleteSectors={incomplete}/{len(rows)}"
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workdir", type=Path, default=ROOT / "tmp/pathb/s03e_sections")
    parser.add_argument("--json-out", type=Path, default=ROOT / "tmp/pathb/s03e_sections.json")
    parser.add_argument("--garment", action="append", choices=tuple(SENTINELS), default=[])
    parser.add_argument("--levels", type=int, default=7)
    parser.add_argument("--morph-steps", type=int, default=6)
    parser.add_argument("--iterations", type=int, default=10)
    args = parser.parse_args()
    workdir = args.workdir if args.workdir.is_absolute() else ROOT / args.workdir
    json_out = args.json_out if args.json_out.is_absolute() else ROOT / args.json_out
    if not HARNESS.is_file():
        raise SystemExit(f"missing RelWithDebInfo harness: {HARNESS}")
    garments = args.garment or list(SENTINELS)
    result: dict[str, object] = {
        "schema": "pathb-s03e-sections-v1",
        "protocol": {
            "delta_convention": "sculpt-candidate; delta_down=-delta_z",
            "axis_orientation": "world_+Z",
            "rest": "base",
            "gravity": 0.0,
            "morph_steps": max(1, args.morph_steps),
            "iterations": max(1, args.iterations),
            "levels": max(1, args.levels),
            "no_per_garment_tuning": True,
        },
        "tool_sha256": {
            "runner": file_sha256(Path(__file__)),
            "band_distribution": file_sha256(ROOT / "tools/pathb_band_distribution.py"),
            "band_descriptor": file_sha256(ROOT / "tools/pathb_band_descriptor.py"),
            "shell_spike": file_sha256(ROOT / "tools/shell_spike.py"),
            "pathb_gate": file_sha256(ROOT / "tools/pathb_gate.py"),
            "harness": file_sha256(HARNESS),
        },
        "garments": {},
    }
    for garment in garments:
        print(f"running {garment} ...", flush=True)
        result["garments"][garment] = analyze_garment(
            garment,
            SENTINELS[garment],
            workdir,
            max(1, args.levels),
            max(1, args.morph_steps),
            max(1, args.iterations),
        )
    json_out.parent.mkdir(parents=True, exist_ok=True)
    json_out.write_text(
        json.dumps(result, indent=2, sort_keys=True, allow_nan=False) + "\n",
        encoding="utf-8",
    )
    summarize(result)
    print(f"wrote {relative(json_out)} sha256={file_sha256(json_out)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
