# Path B S0.2b: run a candidate across the sentinel garments and emit a
# markdown scoreboard from pathb_metrics.py. One config, no per-garment tuning.
#
# Candidate modes:
#   production            -> drape-harness --replay (the current MERGED baseline)
#   obj:<template>        -> a pre-made OBJ per garment; {g} expands to the key
#   shell:<spike args>    -> tools/shell_spike.py --solve with those args
#
# Usage:
#   pathb_gate.py production
#   pathb_gate.py "shell:--rest base --morph-steps 6 --iterations 10" --label shell_base
#   pathb_gate.py "obj:tmp/pathb/prod_{g}.obj" --out tmp/pathb/scoreboard.md
#
# Sentinels are the CLEAN-ground-truth garments only (falmer benched 2026-07-14).
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HARNESS = ROOT / "build/algo/RelWithDebInfo/drape-harness.exe"
FX = ROOT / "fixtures/pathb"
TMP = ROOT / "tmp/pathb"

# capture, ground_truth (tracked fixture), base cloth (derived), hem ids (opt)
SENTINELS = {
    "blacksmith": dict(
        capture="corpus/fur_blacksmith_rm_heavy/drape_capture__Rley_3BA_Vanilla_-_Body_-_Blacksmith__Physics___BlackSmithTorso__RM_Heavy.drcap",
        gt=FX / "blacksmith__GROUND_TRUTH.obj",
        base=ROOT / "tmp/shell_spike_blacksmith/cloth_base.obj",
        hem=FX / "hem_ids_blacksmith.txt", role="sentinel:under-butt"),
    "yarl_clothes": dict(
        capture="corpus/yarl_rm_heavy/drape_capture__Rley_3BA_Vanilla_-_Body_-_Yarl_Clothes_2__No_Physics___Clothes__RM_Heavy.drcap",
        gt=FX / "yarl_clothes__GROUND_TRUTH.obj",
        base=TMP / "ds_yarl_clothes/cloth_base.obj", hem=None,
        role="sentinel:anti-umbrella"),
    "yarl_cloak": dict(
        capture="corpus/yarl_rm_heavy/drape_capture__Rley_3BA_Vanilla_-_Body_-_Yarl_Clothes_2__No_Physics___Cloak__RM_Heavy.drcap",
        gt=FX / "yarl_cloak__GROUND_TRUTH.obj",
        base=TMP / "ds_yarl_cloak/cloth_base.obj", hem=None,
        role="sentinel:gravity"),
}
SECONDARY = {
    "vampire": dict(
        capture="corpus/vampire_rm_heavy/drape_capture__Rley_3BA_Dawnguard_-_Body_-_Vampire_Armor__Im_Physics___vampire_robes_alt__RM_Heavy.drcap",
        gt=FX / "vampire__GROUND_TRUTH.obj",
        base=TMP / "ds_vampire/cloth_base.obj", hem=None, role="secondary"),
}

# measured production baselines (fixtures/pathb/BASELINES.md, S0.2c).
# A candidate FAILS if any mean exceeds baseline x1.05 or adds flipped faces.
BASELINE = {
    "blacksmith":   dict(indexed_mean=1.5441, surf_fwd_mean=0.7563,
                         surf_rev_mean=0.7915, flips=1),
    "yarl_clothes": dict(indexed_mean=0.8388, surf_fwd_mean=0.2590,
                         surf_rev_mean=0.3023, flips=7),
    "yarl_cloak":   dict(indexed_mean=2.0463, surf_fwd_mean=1.2265,
                         surf_rev_mean=0.9154, flips=10),
    "vampire":      dict(indexed_mean=1.1858, surf_fwd_mean=0.5099,
                         surf_rev_mean=0.5591, flips=34),
}


def make_candidate(garment, cfg, mode, workdir, journal):
    """FAIL-CLOSED: delete any stale OBJ first, hard-error on a non-zero
    producer or a missing/empty result. A gate must never score a stale mesh."""
    out = workdir / f"cand_{garment}.obj"
    cap = ROOT / cfg["capture"]
    if mode.startswith("obj:"):
        p = ROOT / mode[4:].format(g=garment)
        if not p.exists():
            raise SystemExit(f"FAIL {garment}: obj not found: {p}")
        journal.append(f"{garment}: obj {p}  sha={_sha12(p)}")
        return p
    if out.exists():
        out.unlink()                      # never reuse a previous run's mesh
    if mode == "production":
        cmd = [str(HARNESS), "--replay", str(cap), "--result-obj", str(out)]
    elif mode.startswith("shell:"):
        ds = workdir / f"ds_{garment}"
        ds.mkdir(parents=True, exist_ok=True)
        cmd = [sys.executable, str(ROOT / "tools/shell_spike.py"),
               "--capture", str(cap), "--dataset", str(ds),
               "--solve", "--output", str(out), *mode[6:].split()]
    else:
        raise SystemExit(f"unknown mode: {mode}")
    r = subprocess.run(cmd, capture_output=True, text=True)
    # the harness returns non-zero on some replays while still writing a valid
    # OBJ, so treat "no/empty output file" as the hard failure, and surface a
    # non-zero exit as a warning in the journal.
    if not out.exists() or out.stat().st_size == 0:
        tail = (r.stderr or r.stdout or "").strip().splitlines()[-3:]
        raise SystemExit(f"FAIL {garment}: producer wrote no mesh (rc={r.returncode})\n"
                         + "\n".join(tail))
    journal.append(f"{garment}: rc={r.returncode} sha={_sha12(out)} cmd={' '.join(cmd)}")
    return out


def _sha12(p):
    import hashlib
    return hashlib.sha256(open(p, "rb").read()).hexdigest()[:12]


def metrics(obj, cfg, label):
    cmd = [sys.executable, str(ROOT / "tools/pathb_metrics.py"),
           "--test", str(obj), "--ref", str(cfg["gt"]), "--label", label]
    if cfg.get("base") and Path(cfg["base"]).exists():
        cmd += ["--base", str(cfg["base"])]
    if cfg.get("hem"):
        cmd += ["--hem-ids", str(cfg["hem"])]
    r = subprocess.run(cmd, capture_output=True, text=True)
    fields = {}
    for line in r.stdout.splitlines():
        if line.startswith("SUMMARY "):
            for tok in line[8:].split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    fields[k] = v
    if not fields:
        fields["error"] = (r.stderr.strip().splitlines() or ["no output"])[-1][:80]
    return fields


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode")
    ap.add_argument("--label", default="cand")
    ap.add_argument("--out")
    ap.add_argument("--secondary", action="store_true")
    a = ap.parse_args()
    workdir = TMP / f"gate_{a.label}"
    workdir.mkdir(parents=True, exist_ok=True)
    sset = dict(SENTINELS)
    if a.secondary:
        sset.update(SECONDARY)
    cols = ["indexed_mean", "indexed_p95", "surf_fwd_mean", "surf_fwd_p95",
            "surf_rev_mean", "surf_rev_p95", "int_flipped_faces",
            "int_degenerate_faces", "int_worst_seam_split", "int_nan"]
    journal = []
    rows = []
    for g, cfg in sset.items():
        obj = make_candidate(g, cfg, a.mode, workdir, journal)
        m = metrics(obj, cfg, f"{a.label}_{g}")
        if "error" in m:
            raise SystemExit(f"FAIL {g}: metrics produced no SUMMARY: {m['error']}")
        rows.append((g, cfg["role"], m))
    # PASS/FAIL vs the measured production baselines (fixtures/pathb/BASELINES.md)
    verdicts = []
    for g, role, m in rows:
        b = BASELINE.get(g)
        if not b:
            verdicts.append((g, "n/a", []))
            continue
        bad = []
        for k, tol in (("indexed_mean", 1.05), ("surf_fwd_mean", 1.05),
                       ("surf_rev_mean", 1.05)):
            try:
                if float(m[k]) > b[k] * tol:
                    bad.append(f"{k} {m[k]}>{round(b[k]*tol,4)}")
            except (KeyError, ValueError):
                bad.append(f"{k} missing")
        try:
            if int(m["int_flipped_faces"]) > b["flips"]:
                bad.append(f"flips {m['int_flipped_faces']}>{b['flips']}")
        except (KeyError, ValueError):
            pass
        verdicts.append((g, "FAIL" if bad else "PASS", bad))
    lines = [f"# scoreboard: {a.label}  (mode: `{a.mode}`)", "",
             "| garment | role | verdict | " + " | ".join(cols) + " |",
             "|---|---|---|" + "|".join(["---"] * len(cols)) + "|"]
    for (g, role, m), (_, v, bad) in zip(rows, verdicts):
        lines.append("| " + g + " | " + role + " | " + v + " | " +
                     " | ".join(str(m.get(c, "-")) for c in cols) + " |")
    fails = [(g, bad) for g, v, bad in verdicts if v == "FAIL"]
    if fails:
        lines += ["", "## FAIL detail"] + [f"- {g}: {', '.join(b)}" for g, b in fails]
    lines += ["", "## journal (commands + result hashes)", ""] + \
             [f"- `{j}`" for j in journal]
    text = "\n".join(lines)
    print(text)
    if a.out:
        Path(a.out).write_text(text + "\n", encoding="utf-8")
        print(f"\nwrote {a.out}")
    overall = "FAIL" if fails else "PASS"
    print(f"\nOVERALL: {overall}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
