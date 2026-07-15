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
import math
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
        role="sentinel:section-position/anti-imprint (not gravity-validated)"),
}
SECONDARY = {
    "vampire": dict(
        capture="corpus/vampire_rm_heavy/drape_capture__Rley_3BA_Dawnguard_-_Body_-_Vampire_Armor__Im_Physics___vampire_robes_alt__RM_Heavy.drcap",
        gt=FX / "vampire__GROUND_TRUTH.obj",
        base=TMP / "ds_vampire/cloth_base.obj", hem=None, role="secondary"),
}

# Measured production baselines (fixtures/pathb/BASELINES.md, S0.2c).
# The default gate intentionally retains the historical three-means + flips
# policy so tracked S03 scoreboards remain reproducible.  --strict additionally
# gates every p95 and the integrity fields recorded in BASELINES.md.
BASELINE = {
    "blacksmith": dict(
        indexed_mean=1.5441, indexed_p95=3.9001,
        surf_fwd_mean=0.7563, surf_fwd_p95=2.7564,
        surf_rev_mean=0.7915, surf_rev_p95=1.9770,
        flips=1, degenerate_faces=3, worst_seam_split=0.0001, nan=0,
    ),
    "yarl_clothes": dict(
        indexed_mean=0.8388, indexed_p95=3.0751,
        surf_fwd_mean=0.2590, surf_fwd_p95=0.9616,
        surf_rev_mean=0.3023, surf_rev_p95=1.2969,
        flips=7, degenerate_faces=0, worst_seam_split=0.0, nan=0,
    ),
    "yarl_cloak": dict(
        indexed_mean=2.0463, indexed_p95=5.3611,
        surf_fwd_mean=1.2265, surf_fwd_p95=3.4131,
        surf_rev_mean=0.9154, surf_rev_p95=3.1823,
        flips=10, degenerate_faces=0, worst_seam_split=0.0001, nan=0,
    ),
    "vampire": dict(
        indexed_mean=1.1858, indexed_p95=2.8337,
        surf_fwd_mean=0.5099, surf_fwd_p95=1.7602,
        surf_rev_mean=0.5591, surf_rev_p95=1.8340,
        flips=34, degenerate_faces=0, worst_seam_split=0.0, nan=0,
    ),
}


def make_candidate(garment, cfg, mode, workdir, journal, strict=False):
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
        if strict:
            # shell_spike exports capture-paired fixtures only when
            # cloth_vanilla.obj is absent.  Remove the complete derived unit so
            # a repeated strict label cannot silently validate a stale dataset.
            for name in (
                "cloth_base.obj", "cloth_vanilla.obj",
                "body_base.obj", "body_morphed.obj",
            ):
                derived = ds / name
                if derived.exists():
                    derived.unlink()
        cmd = [sys.executable, str(ROOT / "tools/shell_spike.py"),
               "--capture", str(cap), "--dataset", str(ds),
               "--solve", "--output", str(out), *mode[6:].split()]
    else:
        raise SystemExit(f"unknown mode: {mode}")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if strict and mode.startswith("shell:") and r.returncode != 0:
        tail = (r.stderr or r.stdout or "").strip().splitlines()[-3:]
        raise SystemExit(
            f"FAIL {garment}: strict shell producer returned rc={r.returncode}\n"
            + "\n".join(tail)
        )
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
    return _sha256(p)[:12]


def _sha256(p):
    import hashlib
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def _obj_validation(path):
    """Return exact triangulated topology and reject non-finite coordinates.

    pathb_metrics reports counts but deliberately accepts arbitrary OBJ
    topology.  Strict gating needs both: agreement with its reported counts
    and exact connectivity agreement with the authored base.
    """
    vertex_count = 0
    triangles = []
    finite = True
    with open(path, encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if line.startswith("v "):
                fields = line.split()
                if len(fields) < 4:
                    raise ValueError(f"{path}:{line_number}: malformed vertex")
                xyz = [float(value) for value in fields[1:4]]
                finite = finite and all(math.isfinite(value) for value in xyz)
                vertex_count += 1
            elif line.startswith("f "):
                fields = line.split()[1:]
                if len(fields) < 3:
                    raise ValueError(f"{path}:{line_number}: malformed face")
                polygon = []
                for field in fields:
                    raw = int(field.split("/", 1)[0])
                    index = raw - 1 if raw > 0 else vertex_count + raw
                    if index < 0 or index >= vertex_count:
                        raise ValueError(
                            f"{path}:{line_number}: vertex index {raw} out of range"
                        )
                    polygon.append(index)
                triangles.extend(
                    (polygon[0], polygon[k], polygon[k + 1])
                    for k in range(1, len(polygon) - 1)
                )
    if vertex_count == 0 or not triangles:
        raise ValueError(f"{path}: empty mesh")
    return vertex_count, tuple(triangles), finite


def _strict_mesh_errors(obj, cfg, fields):
    errors = []
    base = Path(cfg["base"])
    if not base.is_file():
        return [f"strict base missing: {base}"]
    try:
        test_vertices, test_topology, test_finite = _obj_validation(obj)
        base_vertices, base_topology, base_finite = _obj_validation(base)
        gt_vertices, gt_topology, gt_finite = _obj_validation(cfg["gt"])
    except (OSError, ValueError) as exc:
        return [f"mesh validation failed: {exc}"]
    if not test_finite:
        errors.append("OBJ contains non-finite vertex coordinates")
    if not base_finite or not gt_finite:
        errors.append("baseline fixture contains non-finite vertex coordinates")
    if test_vertices != base_vertices:
        errors.append(f"verts {test_vertices}!={base_vertices} base")
    if test_vertices != gt_vertices:
        errors.append(f"verts {test_vertices}!={gt_vertices} ground-truth")
    if test_topology != base_topology:
        errors.append("triangulated topology differs from authored base")
    if gt_topology != base_topology:
        errors.append("ground-truth topology differs from authored base")
    try:
        if int(fields["verts"]) != test_vertices:
            errors.append(f"metrics verts {fields['verts']}!={test_vertices} parsed")
        if int(fields["faces"]) != len(test_topology):
            errors.append(
                f"metrics faces {fields['faces']}!={len(test_topology)} parsed"
            )
    except (KeyError, ValueError):
        errors.append("metrics vertex/face counts missing or invalid")
    return errors


def metrics(obj, cfg, label, strict=False):
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
    if strict and r.returncode != 0:
        fields["error"] = (
            f"metrics rc={r.returncode}: "
            + ((r.stderr or r.stdout).strip().splitlines() or ["no output"])[-1][:80]
        )
    elif not fields:
        fields["error"] = (r.stderr.strip().splitlines() or ["no output"])[-1][:80]
    return fields


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode")
    ap.add_argument("--label", default="cand")
    ap.add_argument("--out")
    ap.add_argument("--secondary", action="store_true")
    ap.add_argument(
        "--strict", action="store_true",
        help=("also require shell rc=0, exact base topology/finite coordinates, "
              "all mean+p95 thresholds and every recorded integrity baseline"),
    )
    a = ap.parse_args()
    output_path = Path(a.out) if a.out else None
    if output_path is not None and output_path.exists():
        output_path.unlink()  # an aborted run must not leave a stale scoreboard
    workdir = TMP / f"gate_{a.label}"
    workdir.mkdir(parents=True, exist_ok=True)
    sset = dict(SENTINELS)
    if a.secondary:
        sset.update(SECONDARY)
    cols = ["indexed_mean", "indexed_p95", "surf_fwd_mean", "surf_fwd_p95",
            "surf_rev_mean", "surf_rev_p95", "int_flipped_faces",
            "int_degenerate_faces", "int_worst_seam_split", "int_nan"]
    journal = [
        "tools: "
        f"pathb_gateSha={_sha12(Path(__file__))} "
        f"shell_spikeSha={_sha12(ROOT / 'tools/shell_spike.py')} "
        f"pathb_metricsSha={_sha12(ROOT / 'tools/pathb_metrics.py')}"
    ]
    if a.mode.startswith("shell:"):
        mode_tokens = a.mode[6:].split()
        if "--surface-corot-mask-json" in mode_tokens:
            flag_index = mode_tokens.index("--surface-corot-mask-json")
            if flag_index + 1 >= len(mode_tokens):
                raise SystemExit("FAIL: --surface-corot-mask-json has no value")
            support_artifact = Path(mode_tokens[flag_index + 1])
            if not support_artifact.is_absolute():
                support_artifact = ROOT / support_artifact
            if not support_artifact.is_file():
                raise SystemExit(f"FAIL: support artifact missing: {support_artifact}")
            journal.append(
                f"supportArtifact={support_artifact} sha256={_sha256(support_artifact)}"
            )
    rows = []
    for g, cfg in sset.items():
        obj = make_candidate(g, cfg, a.mode, workdir, journal, strict=a.strict)
        run_cfg = cfg
        if a.strict and a.mode.startswith("shell:"):
            # shell_spike exports an authored base into this run's dataset.
            # Prefer that fresh, causally paired mesh over a pre-existing tmp/
            # dataset so strict topology/integrity cannot score stale state.
            run_cfg = dict(cfg)
            run_cfg["base"] = workdir / f"ds_{g}" / "cloth_base.obj"
        m = metrics(obj, run_cfg, f"{a.label}_{g}", strict=a.strict)
        if "error" in m:
            raise SystemExit(f"FAIL {g}: metrics produced no SUMMARY: {m['error']}")
        m["_strict_errors"] = (
            _strict_mesh_errors(obj, run_cfg, m) if a.strict else []
        )
        journal.append(
            f"{g}: captureSha={_sha12(ROOT / cfg['capture'])} "
            f"groundTruthSha={_sha12(cfg['gt'])} "
            f"baseSha={_sha12(run_cfg['base'])}"
        )
        rows.append((g, cfg["role"], m))
    # PASS/FAIL vs the measured production baselines (fixtures/pathb/BASELINES.md)
    verdicts = []
    for g, role, m in rows:
        b = BASELINE.get(g)
        if not b:
            verdicts.append((g, "n/a", []))
            continue
        bad = list(m.get("_strict_errors", []))
        accuracy_fields = ["indexed_mean", "surf_fwd_mean", "surf_rev_mean"]
        if a.strict:
            accuracy_fields += ["indexed_p95", "surf_fwd_p95", "surf_rev_p95"]
        for k in accuracy_fields:
            try:
                value = float(m[k])
                limit = b[k] * 1.05
                if a.strict and not math.isfinite(value):
                    bad.append(f"{k} non-finite")
                elif value > limit:
                    bad.append(f"{k} {m[k]}>{round(limit,4)}")
            except (KeyError, ValueError):
                bad.append(f"{k} missing")
        try:
            if int(m["int_flipped_faces"]) > b["flips"]:
                bad.append(f"flips {m['int_flipped_faces']}>{b['flips']}")
        except (KeyError, ValueError):
            if a.strict:
                bad.append("int_flipped_faces missing")
        if a.strict:
            for metric_key, baseline_key in (
                ("int_degenerate_faces", "degenerate_faces"),
                ("int_worst_seam_split", "worst_seam_split"),
                ("int_nan", "nan"),
            ):
                try:
                    value = float(m[metric_key])
                    limit = float(b[baseline_key])
                    if not math.isfinite(value):
                        bad.append(f"{metric_key} non-finite")
                    elif value > limit:
                        bad.append(f"{metric_key} {m[metric_key]}>{b[baseline_key]}")
                except (KeyError, ValueError):
                    bad.append(f"{metric_key} missing")
        verdicts.append((g, "FAIL" if bad else "PASS", bad))
    strict_label = " [STRICT]" if a.strict else ""
    lines = [f"# scoreboard: {a.label}{strict_label}  (mode: `{a.mode}`)", "",
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
    if output_path is not None:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = output_path.with_name(output_path.name + ".tmp")
        if temporary.exists():
            temporary.unlink()
        temporary.write_text(text + "\n", encoding="utf-8")
        temporary.replace(output_path)
        print(f"\nwrote {output_path}")
    overall = "FAIL" if fails else "PASS"
    print(f"\nOVERALL: {overall}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
