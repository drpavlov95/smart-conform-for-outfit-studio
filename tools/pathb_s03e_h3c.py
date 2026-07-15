#!/usr/bin/env python3
"""Reproducible S0.3e H3c factorial with actual contact history.

The retired S0.3c analysis called ``authored gap > 2`` a region that had never
touched the body.  That proxy included hard anchors and vertices later reached
by the continuing body.  This runner executes the complete 2x2x2 factorial,
collects per-vertex traces from ``shell_spike.py`` and evaluates every cell on
the same masks, including populations that are movable, fully draped and either
never receive a collision correction or remain above clearance in *every* one
of the eight cells.

It does not infer a localized bump cause from a global mean.  The result only
states which switches move indexed error on each explicitly named population.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import itertools
import json
import subprocess
import sys
from pathlib import Path

import numpy as np

from pathb_gate import ROOT, SENTINELS
from shell_spike import fitted_drape_signal, load_obj, read_capture_front


FACTOR_LEVELS = {
    "anchor": ("vanilla", "authored"),
    "soft": ("on", "off"),
    "body": ("continuation", "static"),
}


def file_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def relative(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(ROOT.resolve())).replace("\\", "/")
    except ValueError:
        return str(path.resolve())


def score_mask(test: np.ndarray, sculpt: np.ndarray, mask: np.ndarray) -> dict[str, float | int | None]:
    indices = np.flatnonzero(mask)
    if len(indices) == 0:
        return {"n": 0, "indexed_mean": None, "indexed_p95": None, "indexed_max": None}
    error = np.linalg.norm(test[indices] - sculpt[indices], axis=1)
    return {
        "n": int(len(indices)),
        "indexed_mean": float(error.mean()),
        "indexed_p95": float(np.percentile(error, 95)),
        "indexed_max": float(error.max()),
    }


def factorial_contrasts(cells: dict[str, dict[str, object]], mask_name: str) -> dict[str, float | None]:
    rows = []
    for cell in cells.values():
        value = cell["scores"][mask_name]["indexed_mean"]
        if value is None:
            return {name: None for name in ("anchor", "soft", "body", "anchor:soft", "anchor:body", "soft:body", "anchor:soft:body")}
        signs = {
            "anchor": 1.0 if cell["factors"]["anchor"] == "authored" else -1.0,
            "soft": 1.0 if cell["factors"]["soft"] == "off" else -1.0,
            "body": 1.0 if cell["factors"]["body"] == "static" else -1.0,
        }
        rows.append((float(value), signs))

    contrasts: dict[str, float] = {}
    for name, factors in {
        "anchor": ("anchor",),
        "soft": ("soft",),
        "body": ("body",),
        "anchor:soft": ("anchor", "soft"),
        "anchor:body": ("anchor", "body"),
        "soft:body": ("soft", "body"),
        "anchor:soft:body": ("anchor", "soft", "body"),
    }.items():
        positive = []
        negative = []
        for value, signs in rows:
            sign = float(np.prod([signs[factor] for factor in factors]))
            (positive if sign > 0.0 else negative).append(value)
        contrasts[name] = float(np.mean(positive) - np.mean(negative))
    return contrasts


def run_cell(
    garment: str,
    config: dict[str, object],
    factors: dict[str, str],
    workdir: Path,
    morph_steps: int,
    iterations: int,
) -> dict[str, object]:
    tag = f"{factors['anchor']}_{factors['soft']}_{factors['body']}"
    output = workdir / garment / f"{tag}.obj"
    trace = workdir / garment / f"{tag}.trace.json"
    dataset = workdir / garment / "dataset"
    output.parent.mkdir(parents=True, exist_ok=True)
    for stale in (output, trace):
        if stale.exists():
            stale.unlink()
    command = [
        sys.executable,
        str(ROOT / "tools/shell_spike.py"),
        "--capture", str(ROOT / config["capture"]),
        "--dataset", str(dataset),
        "--solve",
        "--output", str(output),
        "--rest", "base",
        "--morph-steps", str(morph_steps),
        "--iterations", str(iterations),
        "--gravity", "0",
        "--anchor-target", factors["anchor"],
        "--soft-anchor", factors["soft"],
        "--body-mode", factors["body"],
        "--trace-json", str(trace),
    ]
    completed = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if completed.returncode != 0 or not output.is_file() or not trace.is_file():
        tail = (completed.stderr or completed.stdout).strip().splitlines()[-12:]
        raise RuntimeError(
            f"{garment}/{tag} failed rc={completed.returncode}\n" + "\n".join(tail)
        )
    trace_data = json.loads(trace.read_text(encoding="utf-8"))
    journal_command = [
        relative(Path(part)) if Path(part).is_absolute() else part for part in command
    ]
    return {
        "factors": factors,
        "command": journal_command,
        "returncode": completed.returncode,
        "output": relative(output),
        "output_sha256": file_sha256(output),
        "trace": relative(trace),
        "trace_sha256": file_sha256(trace),
        "trace_counts": trace_data["counts"],
        "_trace_data": trace_data,
    }


def analyze_garment(
    garment: str,
    config: dict[str, object],
    workdir: Path,
    morph_steps: int,
    iterations: int,
) -> dict[str, object]:
    ground_truth = load_obj(Path(config["gt"]))
    cap = read_capture_front(ROOT / config["capture"])
    if len(ground_truth.verts) != len(cap.base_verts):
        raise ValueError(f"{garment}: ground-truth vertex count mismatch")
    if not np.array_equal(ground_truth.tris, cap.tris):
        raise ValueError(f"{garment}: ground-truth triangle topology mismatch")
    if not np.isfinite(ground_truth.verts).all():
        raise ValueError(f"{garment}: ground truth contains NaN or infinity")
    expected_drape = fitted_drape_signal(cap)

    cells: dict[str, dict[str, object]] = {}
    traces = []
    for anchor, soft, body in itertools.product(
        FACTOR_LEVELS["anchor"], FACTOR_LEVELS["soft"], FACTOR_LEVELS["body"]
    ):
        factors = {"anchor": anchor, "soft": soft, "body": body}
        tag = f"{anchor}_{soft}_{body}"
        cell = run_cell(garment, config, factors, workdir, morph_steps, iterations)
        trace_data = cell.pop("_trace_data")
        traces.append(trace_data)
        cells[tag] = cell

    fixed_arrays = [np.asarray(trace["vertices"]["fixed"], dtype=bool) for trace in traces]
    drape_arrays = [np.asarray(trace["vertices"]["drape"], dtype=np.float64) for trace in traces]
    if any(not np.array_equal(fixed_arrays[0], values) for values in fixed_arrays[1:]):
        raise RuntimeError(f"{garment}: fixed mask changes between factorial cells")
    if any(not np.allclose(drape_arrays[0], values, atol=0.0, rtol=0.0) for values in drape_arrays[1:]):
        raise RuntimeError(f"{garment}: drape field changes between factorial cells")
    if not np.allclose(drape_arrays[0], expected_drape, atol=1.0e-12, rtol=0.0):
        raise RuntimeError(f"{garment}: trace drape field differs from capture classifier")

    ever_corrected = np.logical_or.reduce(
        [np.asarray(trace["vertices"]["contact_corrections"], dtype=np.int64) > 0 for trace in traces]
    )
    minimum_gap_by_cell = []
    clearances = []
    for trace in traces:
        minimum_gap_by_cell.append(
            np.asarray(
                [np.nan if value is None else float(value)
                 for value in trace["vertices"]["minimum_observed_gap"]],
                dtype=np.float64,
            )
        )
        clearances.append(float(trace["parameters"]["clearance"]))
    if max(clearances) - min(clearances) > 1.0e-12:
        raise RuntimeError(f"{garment}: clearance changes between factorial cells")
    always_clear = np.all(
        np.asarray(minimum_gap_by_cell) > (clearances[0] + 1.0e-6), axis=0
    )
    fixed = fixed_arrays[0]
    drape = drape_arrays[0]
    masks = {
        "active_all": ~fixed,
        "fully_free": np.logical_and(~fixed, drape >= 0.55),
        "active_never_corrected": np.logical_and(~fixed, ~ever_corrected),
        "fully_free_never_corrected": np.logical_and.reduce((~fixed, drape >= 0.55, ~ever_corrected)),
        "active_always_clear": np.logical_and(~fixed, always_clear),
        "fully_free_always_clear": np.logical_and.reduce((~fixed, drape >= 0.55, always_clear)),
    }
    for tag, cell in cells.items():
        test = load_obj(ROOT / cell["output"])
        if len(test.verts) != len(cap.base_verts) or not np.array_equal(test.tris, cap.tris):
            raise ValueError(f"{garment}/{tag}: output topology mismatch")
        if not np.isfinite(test.verts).all():
            raise ValueError(f"{garment}/{tag}: output contains NaN or infinity")
        cell["scores"] = {
            name: score_mask(test.verts, ground_truth.verts, mask)
            for name, mask in masks.items()
        }
    return {
        "role": config["role"],
        "capture": relative(ROOT / config["capture"]),
        "capture_sha256": file_sha256(ROOT / config["capture"]),
        "ground_truth": relative(Path(config["gt"])),
        "ground_truth_sha256": file_sha256(Path(config["gt"])),
        "mask_counts": {name: int(mask.sum()) for name, mask in masks.items()},
        "ever_collision_corrected_in_any_cell": int(ever_corrected.sum()),
        "cells": cells,
        "contrasts": {
            mask_name: factorial_contrasts(cells, mask_name) for mask_name in masks
        },
    }


def print_summary(result: dict[str, object]) -> None:
    print("# S0.3e H3c factorial — actual contact history")
    for garment, data in result["garments"].items():
        print(f"\n{garment}: masks={data['mask_counts']}")
        for mask_name, contrasts in data["contrasts"].items():
            values = " ".join(
                f"{name}={'n/a' if value is None else f'{value:+.4f}'}"
                for name, value in contrasts.items()
            )
            print(f"  {mask_name}: {values}")
    print(
        "\nContrast signs: anchor=authored-vanilla, soft=off-on, "
        "body=static-continuation; positive means larger indexed error. "
        "Interaction contrasts use the same +/- factorial coding."
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workdir", type=Path, default=ROOT / "tmp/pathb/s03e_h3c")
    parser.add_argument("--json-out", type=Path, default=ROOT / "tmp/pathb/s03e_h3c_results.json")
    parser.add_argument("--garment", action="append", choices=tuple(SENTINELS), default=[])
    parser.add_argument("--morph-steps", type=int, default=6)
    parser.add_argument("--iterations", type=int, default=10)
    parser.add_argument("--jobs", type=int, default=1, help="garments to run concurrently")
    args = parser.parse_args()
    workdir = args.workdir if args.workdir.is_absolute() else ROOT / args.workdir
    json_out = args.json_out if args.json_out.is_absolute() else ROOT / args.json_out
    garments = args.garment or list(SENTINELS)
    result: dict[str, object] = {
        "schema": "pathb-s03e-h3c-factorial-v2",
        "protocol": {
            "factors": FACTOR_LEVELS,
            "rest": "base",
            "gravity": 0.0,
            "morph_steps": max(1, args.morph_steps),
            "iterations": max(1, args.iterations),
            "common_never_corrected_mask": "contact_corrections == 0 in every factorial cell",
            "common_always_clear_mask": "minimum_observed_gap > clearance+1e-6 in every factorial cell",
            "fully_free_threshold": 0.55,
            "scope_warning": (
                "masks are conservative post-treatment subpopulations and global indexed "
                "means do not identify a localized imprint cause"
            ),
        },
        "tool_sha256": {
            "runner": file_sha256(Path(__file__)),
            "shell_spike": file_sha256(ROOT / "tools/shell_spike.py"),
            "pathb_gate": file_sha256(ROOT / "tools/pathb_gate.py"),
        },
        "garments": {},
    }
    completed_garments: dict[str, dict[str, object]] = {}
    with ThreadPoolExecutor(max_workers=max(1, min(args.jobs, len(garments)))) as executor:
        futures = {}
        for garment in garments:
            print(f"running {garment} ...", flush=True)
            future = executor.submit(
                analyze_garment,
                garment,
                SENTINELS[garment],
                workdir,
                max(1, args.morph_steps),
                max(1, args.iterations),
            )
            futures[future] = garment
        for future in as_completed(futures):
            garment = futures[future]
            completed_garments[garment] = future.result()
            print(f"completed {garment}", flush=True)
    result["garments"] = {
        garment: completed_garments[garment] for garment in garments
    }
    json_out.parent.mkdir(parents=True, exist_ok=True)
    json_out.write_text(
        json.dumps(result, indent=2, sort_keys=True, allow_nan=False) + "\n",
        encoding="utf-8",
    )
    print_summary(result)
    print(f"wrote {relative(json_out)} sha256={file_sha256(json_out)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
