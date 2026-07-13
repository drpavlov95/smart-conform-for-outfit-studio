#!/usr/bin/env python3
"""Verify that an opt-in geometry feature changes only intended captures."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_HARNESS = ROOT / "build/algo/RelWithDebInfo/drape-harness.exe"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def replay(
    harness: Path,
    capture: Path,
    output: Path,
    enabled: bool,
    feature_env: str,
    extra_env: dict[str, str],
) -> None:
    env = os.environ.copy()
    env[feature_env] = "1" if enabled else "0"
    env.update(extra_env)
    output.unlink(missing_ok=True)
    proc = subprocess.run(
        [str(harness), "--replay", str(capture), "--result-obj", str(output)],
        cwd=ROOT,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    # The harness returns 1 when a deliberately changed result differs from the
    # capture's expected mesh; that is valid for the ON side of this isolation
    # test.  Require a replay record and output OBJ instead.
    if "REPLAY file=" not in proc.stdout or not output.is_file():
        raise RuntimeError(
            f"replay produced no usable result for {capture.name} "
            f"(feature={int(enabled)}, exit={proc.returncode}):\n{proc.stdout}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--harness", type=Path, default=DEFAULT_HARNESS)
    parser.add_argument("--corpus", type=Path, default=ROOT / "corpus")
    parser.add_argument("--feature-env", default="DRAPE_TANGENTIAL_REPAIR")
    parser.add_argument(
        "--set",
        action="append",
        default=[],
        metavar="NAME=VALUE",
        help="additional environment value applied to both OFF and ON replays",
    )
    parser.add_argument(
        "--output-dir", type=Path, default=ROOT / "tmp/tangential_corpus_isolation"
    )
    parser.add_argument(
        "--expected-change",
        action="append",
        default=None,
        help="capture-name substring expected to differ; may be repeated",
    )
    parser.add_argument(
        "--clear-expected-changes",
        action="store_true",
        help="require only isolation (no unexpected changes), without requiring a target change",
    )
    args = parser.parse_args()

    if args.clear_expected_changes:
        args.expected_change = []
    elif args.expected_change is None:
        args.expected_change = ["BlackSmithTorso"]

    captures = sorted(args.corpus.glob("**/*.drcap"))
    if not args.harness.is_file() or not captures:
        parser.error("harness or corpus captures not found")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    extra_env: dict[str, str] = {}
    for item in args.set:
        if "=" not in item:
            parser.error(f"invalid --set value: {item!r}")
        name, value = item.split("=", 1)
        extra_env[name] = value

    changed: list[Path] = []
    unexpected_same: list[Path] = []
    unexpected_changed: list[Path] = []
    for index, capture in enumerate(captures):
        stem = f"{index:02d}_{capture.parent.name}_{capture.stem}"
        off = args.output_dir / f"{stem}_off.obj"
        on = args.output_dir / f"{stem}_on.obj"
        replay(args.harness, capture, off, False, args.feature_env, extra_env)
        replay(args.harness, capture, on, True, args.feature_env, extra_env)
        differs = sha256(off) != sha256(on)
        expected = any(token in capture.name for token in args.expected_change)
        if differs:
            changed.append(capture)
        if expected and not differs:
            unexpected_same.append(capture)
        if differs and not expected:
            unexpected_changed.append(capture)
        print(f"[{'CHANGED' if differs else 'no-op':7}] {capture.parent.name}/{capture.name}")

    print(
        f"summary: captures={len(captures)} changed={len(changed)} "
        f"unexpected_changed={len(unexpected_changed)} "
        f"unexpected_same={len(unexpected_same)}"
    )
    for capture in unexpected_changed:
        print(f"unexpected change: {capture}", file=sys.stderr)
    for capture in unexpected_same:
        print(f"expected change missing: {capture}", file=sys.stderr)
    return 1 if unexpected_changed or unexpected_same else 0


if __name__ == "__main__":
    raise SystemExit(main())
