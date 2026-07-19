#!/usr/bin/env python3
"""Summarize Path-B's opt-in per-vertex contact trace.

The C++ solver writes ``%TEMP%/drape_live_contact_trace.csv`` when
``DRAPE_SHELL_TRACE_CONTACT=1``.  This tool reports which constraints are
actually active in the posterior contact-release band; it never edits meshes.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from typing import Callable, Iterable


Row = dict[str, float]


def quantile(values: Iterable[float], q: float) -> float | None:
    ordered = sorted(v for v in values if math.isfinite(v))
    if not ordered:
        return None
    position = (len(ordered) - 1) * q
    lo = int(math.floor(position))
    hi = int(math.ceil(position))
    if lo == hi:
        return ordered[lo]
    weight = position - lo
    return ordered[lo] * (1.0 - weight) + ordered[hi] * weight


def read_trace(path: Path) -> tuple[list[Row], int]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        reader = csv.DictReader(handle)
        fields = reader.fieldnames or []
        steps = 0
        while f"min_gap_s{steps + 1}" in fields:
            steps += 1
        if steps == 0:
            raise ValueError("trace has no morph-step columns")
        required = {
            "vertex", "final_x", "final_y", "final_z", "fixed",
            "authored_gap", "final_gap",
        }
        for step in range(1, steps + 1):
            required.update({
                f"min_gap_s{step}", f"vertex_contact_s{step}",
                f"face_contact_s{step}", f"channel_floor_s{step}",
                f"channel_ceiling_s{step}", f"sticky_s{step}",
            })
        missing = sorted(required.difference(fields))
        if missing:
            raise ValueError(f"trace is missing columns: {', '.join(missing)}")

        rows: list[Row] = []
        for raw in reader:
            converted: Row = {}
            for key, value in raw.items():
                converted[key] = float(value) if value not in (None, "") else math.nan
            rows.append(converted)
    if not rows:
        raise ValueError("trace has no vertices")
    return rows, steps


def describe(
    rows: list[Row], steps: int, selection_rows: list[Row] | None = None
) -> dict[str, object]:
    selectors = {
        int(r["vertex"]): r for r in (selection_rows if selection_rows is not None else rows)
    }

    def region(name: str, predicate: Callable[[Row], bool]) -> tuple[str, dict[str, object]]:
        selected = [
            r for r in rows
            if r["fixed"] < 0.5 and predicate(selectors.get(int(r["vertex"]), r))
        ]
        result: dict[str, object] = {"vertices": len(selected)}
        if not selected:
            return name, result

        result["final_gap"] = {
            key: quantile((r["final_gap"] for r in selected), q)
            for key, q in (("min", 0.0), ("p10", 0.1), ("median", 0.5), ("p90", 0.9))
        }
        result["last_step_min_gap"] = {
            key: quantile((r[f"min_gap_s{steps}"] for r in selected), q)
            for key, q in (("min", 0.0), ("p10", 0.1), ("median", 0.5), ("p90", 0.9))
        }
        operators: dict[str, object] = {}
        for op in ("vertex_contact", "face_contact", "channel_floor", "channel_ceiling"):
            totals = [sum(r[f"{op}_s{s}"] for s in range(1, steps + 1)) for r in selected]
            last = [r[f"{op}_s{steps}"] for r in selected]
            operators[op] = {
                "vertices_any": sum(v > 0 for v in totals),
                "events_total": int(sum(totals)),
                "vertices_last_step": sum(v > 0 for v in last),
                "events_last_step": int(sum(last)),
            }
        result["operators"] = operators
        result["sticky_last_step_vertices"] = sum(
            r[f"sticky_s{steps}"] > 0.5 for r in selected
        )
        return name, result

    posterior = lambda r: r["final_y"] < 0.0
    regions = dict([
        region("posterior_lateral_transition", lambda r: posterior(r) and 5.0 < abs(r["final_x"]) < 11.0 and 60.0 <= r["final_z"] <= 70.0),
        region("posterior_central_transition", lambda r: posterior(r) and abs(r["final_x"]) < 3.0 and 60.0 <= r["final_z"] <= 70.0),
        region("posterior_lateral_lower", lambda r: posterior(r) and 5.0 < abs(r["final_x"]) < 11.0 and 34.0 <= r["final_z"] < 60.0),
        region("posterior_all_transition", lambda r: posterior(r) and 60.0 <= r["final_z"] <= 70.0),
    ])
    return {
        "vertices": len(rows),
        "morph_steps": steps,
        "selection": "reference_trace" if selection_rows is not None else "self",
        "regions": regions,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument(
        "--selection-trace", type=Path,
        help="freeze region membership from another trace's final coordinates",
    )
    parser.add_argument("--json", type=Path, dest="json_path")
    args = parser.parse_args()

    rows, steps = read_trace(args.trace)
    selection_rows = None
    if args.selection_trace:
        selection_rows, selection_steps = read_trace(args.selection_trace)
        if selection_steps != steps:
            raise ValueError("selection trace has a different morph-step count")
        if {int(r["vertex"]) for r in selection_rows} != {
            int(r["vertex"]) for r in rows
        }:
            raise ValueError("selection trace has different vertex IDs")
    summary = describe(rows, steps, selection_rows)
    rendered = json.dumps(summary, indent=2, sort_keys=True)
    print(rendered)
    if args.json_path:
        args.json_path.parent.mkdir(parents=True, exist_ok=True)
        args.json_path.write_text(rendered + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
