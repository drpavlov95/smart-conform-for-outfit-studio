#!/usr/bin/env python3
"""One command that can REFUSE. The point is that I cannot report a win without it.

Every wrong "está bom" in this project came from reading one number. Today's was
bodyPenetrations, while garmentResidual sat on the same output line showing our
solve doubling cloth-in-cloth on every garment it touches. The fix is not to
remember harder; it is to have a check that fails on its own.

So this compares a candidate against a NAMED baseline over his live captures and
prints PASS/FAIL per axis, with the pieces responsible:

  publicacao   a garment falling back to vanilla is the worst outcome there is,
               and it is invisible in every other number
  corpo        penetrating vertices and worst depth
  roupa        cloth inside cloth - vertices, faces and worst depth
  esticamento  the ratio the host discards a garment on

Nothing here is weighted or scored into a single figure. A single figure is how
a regression hides behind an improvement.
"""
from __future__ import annotations
import argparse
import concurrent.futures as cf
import os
import re
import subprocess
from pathlib import Path

FIELDS = ["wouldPublish", "bodyPenetrations", "worstBodyGap", "garmentResidual",
          "garmentFaceResidual", "worstGarmentPen", "worstEdgeStretch"]

BASE_ENV = dict(DRAPE_SHELL_SOLVE="1", DRAPE_SHELL_TRANSACTIONS="0",
                DRAPE_SHELL_BODY_EVICT="1", DRAPE_ANTICLIP_SAFE_FLOOR="1",
                DRAPE_POSTERIOR_CHORD="1")


def run(harness, cap, extra):
    env = dict(os.environ)
    env.update(BASE_ENV)
    for kv in extra.split():
        if "=" in kv:
            k, v = kv.split("=", 1)
            env[k] = v
    p = subprocess.run([harness, "--replay", str(cap)], env=env,
                       capture_output=True, text=True)
    out = (p.stdout or "") + (p.stderr or "")
    got = {}
    for f in FIELDS:
        m = re.search(rf"\b{f}=(-?[\d.]+)", out)
        got[f] = float(m.group(1)) if m else 0.0
    return got


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--caps", default="tmp/live0726")
    ap.add_argument("--harness", default="./build/algo/Release/drape-harness.exe")
    ap.add_argument("--baseline", default="", help="env da BASELINE (vazio = defaults atuais)")
    ap.add_argument("--candidate", default="", help="env do CANDIDATO")
    ap.add_argument("--tol", type=float, default=1e-4)
    ap.add_argument("--jobs", type=int, default=max(2, (os.cpu_count() or 4)))
    a = ap.parse_args()

    caps = sorted(Path(a.caps).rglob("*.drcap"))
    if not caps:
        print("nenhuma captura em", a.caps)
        return 2

    # In parallel, because the length of this loop changes how I THINK.
    #
    # Sequential this took 12 minutes, and a 12-minute wait is what turns
    # "test a hypothesis" into "while I'm waiting, try another constant too."
    # Six runs went by on one day and three of them were nothing but a
    # different floor value - not diagnosis, just a knob, which is exactly the
    # habit I was told not to have. The replays are independent and the box has
    # cores; a loop that answers in a minute is one you can actually ask
    # questions with.
    jobs = []
    for cap in caps:
        shape = cap.name.split("___")[-1].split("__RM")[0]
        name = f"{cap.parent.name[:12]}/{shape}"
        jobs.append((name, cap, a.baseline))
        jobs.append((name, cap, a.candidate))
    got = {}
    with cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        fut = {ex.submit(run, a.harness, c, e): (nm, i)
               for i, (nm, c, e) in enumerate(jobs)}
        for f in cf.as_completed(fut):
            got[fut[f][1]] = f.result()
    rows = [(jobs[i][0], got[i], got[i + 1]) for i in range(0, len(jobs), 2)]

    fails, wins = [], []
    print(f"{'peca':<34} {'corpo':>14} {'roupa':>18} {'pub':>4}")
    print("-" * 78)
    for name, b, c in rows:
        cb = f"{int(b['bodyPenetrations'])}@{b['worstBodyGap']:.2f}"
        cc = f"{int(c['bodyPenetrations'])}@{c['worstBodyGap']:.2f}"
        gb = f"{int(b['garmentResidual'])}/{int(b['garmentFaceResidual'])}"
        gc = f"{int(c['garmentResidual'])}/{int(c['garmentFaceResidual'])}"
        mark = ""
        if c["wouldPublish"] < b["wouldPublish"]:
            fails.append(f"{name}: PERDEU PUBLICACAO (cai pro vanilla)")
            mark = " <<<"
        if (c["bodyPenetrations"] > b["bodyPenetrations"] + a.tol and
                abs(c["worstBodyGap"]) > abs(b["worstBodyGap"]) + a.tol):
            fails.append(f"{name}: corpo {cb} -> {cc}")
            mark = " <<<"
        if (c["garmentResidual"] > b["garmentResidual"] + a.tol or
                c["garmentFaceResidual"] > b["garmentFaceResidual"] + a.tol):
            fails.append(f"{name}: roupa-roupa {gb} -> {gc}"
                         f" (pior {b['worstGarmentPen']:.2f} -> {c['worstGarmentPen']:.2f})")
            mark = " <<<"
        if (c["bodyPenetrations"] < b["bodyPenetrations"] or
                c["garmentResidual"] < b["garmentResidual"]):
            wins.append(name)
        if cb != cc or gb != gc or mark:
            print(f"{name:<34} {cb:>6}->{cc:<7} {gb:>8}->{gc:<9}"
                  f" {int(c['wouldPublish']):>3}{mark}")

    print()
    if wins:
        print(f"melhorou em: {', '.join(sorted(set(wins)))}")
    if fails:
        print(f"\n*** REPROVADO — {len(fails)} regressao(oes) ***")
        for f in fails:
            print("  -", f)
        print("\nNAO declare melhora. Renderize a cena montada e olhe antes de qualquer coisa.")
        return 1
    print("\n*** PASSOU *** — nenhuma peca regrediu em corpo, roupa-roupa ou publicacao.")
    print("Ainda assim: renderize a cena montada e OLHE. O portao nao ve caimento.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
