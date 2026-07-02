#!/usr/bin/env python3
"""
Corpus regression check (plan U8 / M0 exit gate).

Runs `drape-harness.exe --replay-dir <corpus>` for one or more capture-set
directories, parses every REPLAY metrics line, and compares each shape's
metrics against a stored golden file. Any metric drifting beyond tolerance is
reported and fails the check (exit 1) unless --update is passed, in which
case the golden file is (re)written to match the current run.

This is the SKELETON the plan calls for: it works today against whatever
capture directories exist under corpus/ (currently just the one Yarl Clothes 2
RM_Heavy set, copied from tmp/rm_heavy_seq). Growing the corpus to the full
archetype matrix (fitted dress, loose robe, rigid armor, weird-topology mod)
is a data-gathering step (capture more outfits via DRAPE_CAPTURE in
OutfitStudio) - no script changes needed; drop a new directory of .drcap
files under corpus/<name>/ and add a golden/<name>.json is one --update away.

Usage:
    py -3 scripts/check_corpus.py [--update] [--harness PATH] [--corpus-root DIR]
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_HARNESS = REPO_ROOT / "build" / "algo" / "RelWithDebInfo" / "drape-harness.exe"
DEFAULT_CORPUS_ROOT = REPO_ROOT / "corpus"
GOLDEN_DIR = REPO_ROOT / "corpus" / "golden"

# Numeric-value metrics only; string fields (file, shape, slider, preset) are
# kept for identification but not diffed.
KV_RE = re.compile(r'(\w+)=("[^"]*"|[^\s]+)')
REL_TOL = 0.01     # 1% relative
ABS_TOL = 1e-3     # floor for near-zero values


def parse_replay_line(line: str) -> dict | None:
    if not line.startswith("REPLAY file="):
        return None
    kvs = {}
    for key, val in KV_RE.findall(line):
        val = val.strip('"')
        kvs[key] = val
    return kvs


def run_corpus(harness: Path, corpus_dir: Path) -> dict:
    """Returns {shape_name: {metric: value}} for every REPLAY line produced."""
    proc = subprocess.run(
        [str(harness), "--replay-dir", str(corpus_dir)],
        capture_output=True, text=True, timeout=300,
    )
    results = {}
    for line in proc.stdout.splitlines():
        kvs = parse_replay_line(line)
        if not kvs or "shape" not in kvs:
            continue
        results[kvs["shape"]] = kvs
    if not results:
        print(f"  WARNING: no REPLAY lines parsed from {corpus_dir} "
              f"(harness exit={proc.returncode})", file=sys.stderr)
        if proc.stderr:
            print(proc.stderr, file=sys.stderr)
    return results


def numeric_fields(kvs: dict) -> dict:
    out = {}
    for k, v in kvs.items():
        if k in ("file", "shape", "slider", "preset"):
            continue
        try:
            out[k] = float(v)
        except ValueError:
            continue  # skip byOuter/byInner map strings for now (see NOTE below)
    return out


def compare(current: dict, golden: dict) -> list[str]:
    diffs = []
    cur_num = numeric_fields(current)
    gold_num = numeric_fields(golden)
    all_keys = sorted(set(cur_num) | set(gold_num))
    for k in all_keys:
        c = cur_num.get(k)
        g = gold_num.get(k)
        if c is None:
            diffs.append(f"    {k}: MISSING in current run (golden had {g})")
            continue
        if g is None:
            diffs.append(f"    {k}: NEW metric (current={c}), golden has no baseline")
            continue
        tol = max(ABS_TOL, abs(g) * REL_TOL)
        if abs(c - g) > tol:
            diffs.append(f"    {k}: {g} -> {c}  (delta={c - g:+.4f}, tol={tol:.4f})")
    return diffs


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--update", action="store_true",
                     help="Overwrite golden files with the current run's output")
    ap.add_argument("--harness", type=Path, default=DEFAULT_HARNESS)
    ap.add_argument("--corpus-root", type=Path, default=DEFAULT_CORPUS_ROOT)
    args = ap.parse_args()

    if not args.harness.exists():
        print(f"Error: harness not found at {args.harness}", file=sys.stderr)
        return 2
    if not args.corpus_root.exists():
        print(f"Error: corpus root not found at {args.corpus_root}", file=sys.stderr)
        return 2

    corpus_dirs = sorted(d for d in args.corpus_root.iterdir()
                         if d.is_dir() and d.name != "golden")
    if not corpus_dirs:
        print(f"Error: no corpus subdirectories under {args.corpus_root}", file=sys.stderr)
        return 2

    GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
    any_fail = False
    for corpus_dir in corpus_dirs:
        name = corpus_dir.name
        print(f"[{name}] running harness against {corpus_dir} ...")
        current = run_corpus(args.harness, corpus_dir)
        golden_path = GOLDEN_DIR / f"{name}.json"

        if args.update or not golden_path.exists():
            action = "updated" if golden_path.exists() else "created"
            golden_path.write_text(json.dumps(current, indent=2, sort_keys=True))
            print(f"[{name}] golden {action}: "
                  f"{golden_path.relative_to(REPO_ROOT)} ({len(current)} shapes)")
            continue

        golden = json.loads(golden_path.read_text())
        shape_names = sorted(set(current) | set(golden))
        corpus_fail = False
        for shape in shape_names:
            if shape not in current:
                print(f"[{name}] {shape}: MISSING from current run (was in golden)")
                corpus_fail = True
                continue
            if shape not in golden:
                print(f"[{name}] {shape}: NEW shape (not in golden - run --update to accept)")
                corpus_fail = True
                continue
            diffs = compare(current[shape], golden[shape])
            if diffs:
                print(f"[{name}] {shape}: {len(diffs)} metric(s) changed:")
                for d in diffs:
                    print(d)
                corpus_fail = True
        if not corpus_fail:
            print(f"[{name}] OK - {len(shape_names)} shapes match golden within tolerance")
        any_fail = any_fail or corpus_fail

    if any_fail:
        print("\nREGRESSION DETECTED. If this drift is intentional, re-run with --update.")
        return 1
    print("\nAll corpora match golden.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
