#!/usr/bin/env python3
"""Re-score shell-ON vs production on PHYSICS ONLY -- never touches a hand sculpt.

Context (2026-07-26): the user corrected a framing error that had been baked into
the project's notes -- the hand sculpts were being treated as the loss function.
They are a FLOOR (imperfect, merely better than vanilla), not a ceiling; Smart
Conform is supposed to BEAT them.  Every "worse in 5 of 6" verdict produced by
minimising distance-to-sculpt is therefore unsafe.  This tool replaces that ruler
with quantities that are true or false independently of anyone's artistic intent:

  drapeVerticality  mean angle (deg) from straight-down over free-cloth edges.
                    LOWER = hangs more like real cloth under gravity.
  bodyPenetrations  vertices buried in the body.  LOWER.  Never a matter of taste.
  worstBodyGap      depth of the deepest penetration.  Closer to 0.
  worstEdgeStretch  material deformation.  LOWER.  Above kMaxStretchCeiling=8.0
                    the host silently discards the shape, so this is also a cliff.
  stretch/bendEnergy mean squared deviation from rest.  LOWER = less fought-for pose.
  roughness         surface quality; compared against the vanilla value it carries.

Runs the corpus twice (flag file toggled, then restored) and prints the delta.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
HARNESS = REPO / "build" / "algo" / "Release" / "drape-harness.exe"
CORPORA = ["falmer_rm_heavy", "fur_blacksmith_rm_heavy", "vampire_rm_heavy", "yarl_rm_heavy"]

# NOTE: the harness deliberately compiles out the flag-file lookup
# (`#if !defined(DRAPE_HARNESS_BUILD)` around the shell_solve_*.flag test), so in
# this build the ONLY switch is the env var.  Planting flag files here does
# nothing but disturb the user's live config -- do not do it.
SHELL_ENV = "DRAPE_SHELL_SOLVE"

# MIRROR THE LIVE CONFIG, or the run measures a different program than the user
# sees.  His deployed flags are shell_transactions_off + anticlip_safe_floor_on +
# posterior_chord_on.  `shell_transactions_off` matters most: with transactions ON
# (the built-in default) the comparative closure gate demands the raw candidate
# have ZERO initial crossings/body/face regressions -- repairs that fully succeed
# (crossing=100->0, accepted=1/1/1) still count as failure -- so the shell is
# discarded and production publishes, making every metric identical by
# construction.  That is a measurement artifact, not a result.
LIVE_ENV = {
    "DRAPE_SHELL_TRANSACTIONS": "0",
    "DRAPE_ANTICLIP_SAFE_FLOOR": "1",
    "DRAPE_POSTERIOR_CHORD": "1",
}

# metric -> (label, direction) where direction -1 means lower is better.
PHYSICS = [
    ("drapeVerticality", "prumo(deg)", -1),
    ("bodyPenetrations", "pen(n)", -1),
    ("worstBodyGap", "pen(fundo)", +1),   # negative values; toward 0 is better
    ("worstEdgeStretch", "stretch", -1),
    ("stretchEnergy", "E-stretch", -1),
    ("bendEnergy", "E-bend", -1),
    ("roughnessSolved", "rugosidade", -1),
]

KV = re.compile(r'(\w+)=("[^"]*"|\S+)')


def parse(out: str) -> dict:
    shapes = {}
    for line in out.splitlines():
        if not line.startswith("REPLAY file="):
            continue
        d = {}
        for k, v in KV.findall(line):
            v = v.strip('"')
            try:
                d[k] = float(v)
            except ValueError:
                d[k] = v
        name = d.get("shape")
        if name:
            shapes[name] = d
    return shapes


def run(shell_on: bool, extra: dict | None = None) -> dict:
    """Run the whole corpus with the shell forced on or off, return shape->metrics."""
    env = dict(os.environ)
    env.update(LIVE_ENV)
    env.update(extra or {})
    env[SHELL_ENV] = "1" if shell_on else "0"
    shapes = {}
    for corpus in CORPORA:
        proc = subprocess.run(
            [str(HARNESS), "--replay-dir", str(REPO / "corpus" / corpus)],
            capture_output=True, text=True, cwd=str(REPO), env=env,
        )
        shapes.update(parse(proc.stdout))
    return shapes


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--harness", type=Path, default=HARNESS)
    ap.add_argument("--mode", choices=["live", "repair"], default="live")
    args = ap.parse_args()
    if not args.harness.exists():
        print(f"harness nao encontrado: {args.harness}", file=sys.stderr)
        return 2

    print("rodando PRODUCAO (shell off) ...", file=sys.stderr)
    off = run(shell_on=False)
    if args.mode == "live":
        print("rodando MOTOR (live: transactions off) ...", file=sys.stderr)
        on = run(shell_on=True)
    else:
        print("rodando MOTOR (closure-repair: gate ligado + reparo adotado) ...", file=sys.stderr)
        on = run(shell_on=True, extra={"DRAPE_SHELL_TRANSACTIONS": "1",
                                       "DRAPE_SHELL_CLOSURE_REPAIR": "1"})

    names = sorted(set(off) & set(on))
    print(f"\n{len(names)} pecas medidas.  Regua = FISICA (nenhum sculpt lido).\n")

    hdr = f"{'peca':<22}" + "".join(f"{lbl:>26}" for _, lbl, _ in PHYSICS)
    print(hdr)
    print("-" * len(hdr))

    wins = {k: 0 for k, _, _ in PHYSICS}
    losses = {k: 0 for k, _, _ in PHYSICS}
    rows = []
    for n in names:
        cells = []
        for key, _lbl, direction in PHYSICS:
            a, b = off.get(key), on.get(key)
            a = off[n].get(key)
            b = on[n].get(key)
            if not isinstance(a, float) or not isinstance(b, float):
                cells.append(f"{'--':>26}")
                continue
            if direction < 0:
                better = b < a - 1e-6
                worse = b > a + 1e-6
            else:  # toward zero
                better = abs(b) < abs(a) - 1e-6
                worse = abs(b) > abs(a) + 1e-6
            mark = "+" if better else ("-" if worse else " ")
            if better:
                wins[key] += 1
            elif worse:
                losses[key] += 1
            cells.append(f"{a:>10.3f}->{b:>9.3f} {mark:<3}")
        rows.append((n, cells))
        print(f"{n:<22}" + "".join(cells))

    print("\nplacar por criterio (+ = motor ganha da producao):")
    for key, lbl, _ in PHYSICS:
        print(f"  {lbl:<14} ganha {wins[key]:>2}   perde {losses[key]:>2}   empata {len(names)-wins[key]-losses[key]:>2}")

    # The cliff that silently destroys a shape in the host.
    print("\nteto do validador do host (stretch 8.0 = forma descartada em silencio):")
    for n in names:
        a, b = off[n].get("worstEdgeStretch"), on[n].get("worstEdgeStretch")
        if isinstance(a, float) and isinstance(b, float) and (a >= 8.0 or b >= 8.0):
            print(f"  {n:<22} producao {a:6.2f}   motor {b:6.2f}"
                  f"   {'MOTOR SALVA' if a >= 8.0 > b else ('MOTOR QUEBRA' if b >= 8.0 > a else 'ambos acima')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
