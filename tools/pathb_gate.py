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


def make_candidate(garment, cfg, mode, workdir):
    out = workdir / f"cand_{garment}.obj"
    cap = ROOT / cfg["capture"]
    if mode == "production":
        subprocess.run([str(HARNESS), "--replay", str(cap), "--result-obj", str(out)],
                       capture_output=True)
    elif mode.startswith("obj:"):
        return ROOT / mode[4:].format(g=garment)
    elif mode.startswith("shell:"):
        ds = workdir / f"ds_{garment}"
        ds.mkdir(parents=True, exist_ok=True)
        args = mode[6:].split()
        subprocess.run([sys.executable, str(ROOT / "tools/shell_spike.py"),
                        "--capture", str(cap), "--dataset", str(ds),
                        "--solve", "--output", str(out), *args],
                       capture_output=True)
    else:
        raise SystemExit(f"unknown mode: {mode}")
    return out


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
    rows = []
    for g, cfg in sset.items():
        obj = make_candidate(g, cfg, a.mode, workdir)
        m = metrics(obj, cfg, f"{a.label}_{g}")
        rows.append((g, cfg["role"], m))
    lines = [f"# scoreboard: {a.label}  (mode: `{a.mode}`)", "",
             "| garment | role | " + " | ".join(cols) + " |",
             "|---|---|" + "|".join(["---"] * len(cols)) + "|"]
    for g, role, m in rows:
        lines.append("| " + g + " | " + role + " | " +
                     " | ".join(str(m.get(c, m.get("error", "-"))) for c in cols) + " |")
    text = "\n".join(lines)
    print(text)
    if a.out:
        Path(a.out).write_text(text + "\n", encoding="utf-8")
        print(f"\nwrote {a.out}")


if __name__ == "__main__":
    main()
