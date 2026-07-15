#!/usr/bin/env python3
"""Freeze and gate Path-B broad/coherent-v1 support masks.

The runner consumes only captures: no sculpt, ground truth, garment-name rule,
or hand-picked vertex IDs enters classification.  Non-selected vertices use the
capture's vanilla/raw target.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import sys
from pathlib import Path

import numpy as np
try:
    import scipy
except ImportError:
    scipy = None

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import pathb_support_classifier as classifier
import shell_spike as ss


MORPH_STEPS = 6
CLEARANCE = 0.02
SURFACE_COROT_CLAMP = True
SURFACE_COROT_FALLBACK = "body-vanilla"


DEFAULT_CAPTURES = (
    (
        "blacksmith",
        ROOT / "corpus/fur_blacksmith_rm_heavy/drape_capture__Rley_3BA_Vanilla_-_Body_-_Blacksmith__Physics___BlackSmithTorso__RM_Heavy.drcap",
    ),
    (
        "yarl_clothes",
        ROOT / "corpus/yarl_rm_heavy/drape_capture__Rley_3BA_Vanilla_-_Body_-_Yarl_Clothes_2__No_Physics___Clothes__RM_Heavy.drcap",
    ),
    (
        "yarl_cloak",
        ROOT / "corpus/yarl_rm_heavy/drape_capture__Rley_3BA_Vanilla_-_Body_-_Yarl_Clothes_2__No_Physics___Cloak__RM_Heavy.drcap",
    ),
)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def mask_sha256(mask: np.ndarray) -> str:
    return hashlib.sha256(np.asarray(mask, dtype=np.uint8).tobytes()).hexdigest()


def parse_capture(value: str) -> tuple[str, Path]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("capture must be LABEL=PATH")
    label, raw_path = value.split("=", 1)
    path = Path(raw_path)
    if not path.is_absolute():
        path = ROOT / path
    if not label or not path.is_file():
        raise argparse.ArgumentTypeError(f"invalid capture {value!r}")
    return label, path.resolve()


def audit_capture(label: str, path: Path) -> dict:
    cap = ss.read_capture_front(path)
    if not len(cap.ref_body_verts) or not len(cap.body_verts):
        raise RuntimeError(f"{path}: capture lacks reference/morphed body")
    if not np.array_equal(cap.ref_body_tris, cap.body_tris):
        raise RuntimeError(f"{path}: reference/morphed body topology differs")
    topo = ss.topology_stats(cap.base_verts, cap.tris)
    edges = np.asarray(sorted(topo["edge_faces"]), dtype=np.int32)

    drape = ss.fitted_drape_signal(cap)
    u = np.clip((drape - 0.05) / 0.50, 0.0, 1.0)
    inverse_mass = u * u * (3.0 - 2.0 * u)
    fixed = inverse_mass <= 1.0e-12
    soft_anchor = 0.18 * (1.0 - inverse_mass) ** 2
    requested = fixed | (soft_anchor > 0.0)

    collider = ss.UniformBodyCollider(cap.ref_body_verts, cap.ref_body_tris)
    attachments = ss.build_surface_attachments(
        cap.base_verts,
        requested,
        cap.ref_body_verts,
        cap.ref_body_tris,
        collider,
    )
    authored_gap = collider.gaps(cap.base_verts)
    contact_floor = np.minimum(authored_gap, CLEARANCE)
    raw_targets = []
    corot_targets = []
    clamp_counts = []
    clamp_worsts = []
    clamp_indices = np.flatnonzero(np.logical_and(fixed, requested)).astype(
        np.int32
    )
    for step in range(1, MORPH_STEPS + 1):
        t = step / MORPH_STEPS
        body = cap.ref_body_verts + (cap.body_verts - cap.ref_body_verts) * t
        raw_target = cap.base_verts + cap.raw_diff * t
        corot_target = ss.transport_surface_attachments(
            attachments,
            cap.ref_body_verts,
            body,
            cap.body_tris,
            raw_target,
            cap.base_verts,
        )
        if SURFACE_COROT_CLAMP:
            clamp_count, clamp_worst = ss.clamp_targets_to_contact_floor(
                corot_target,
                clamp_indices,
                ss.UniformBodyCollider(body, cap.body_tris),
                contact_floor,
            )
        else:
            clamp_count, clamp_worst = 0, 0.0
        raw_targets.append(raw_target)
        corot_targets.append(corot_target)
        clamp_counts.append(clamp_count)
        clamp_worsts.append(clamp_worst)
    raw_targets = np.asarray(raw_targets, dtype=np.float64)
    corot_targets = np.asarray(corot_targets, dtype=np.float64)

    support_points = np.full_like(cap.base_verts, np.nan)
    for vi in np.flatnonzero(requested):
        triangle = int(attachments.triangle[vi])
        support_points[vi] = np.sum(
            attachments.barycentric[vi, :, None]
            * cap.ref_body_verts[cap.ref_body_tris[triangle]],
            axis=0,
        )
    body_triangles = cap.ref_body_verts[cap.ref_body_tris]
    body_edges = np.stack(
        (
            np.linalg.norm(body_triangles[:, 0] - body_triangles[:, 1], axis=1),
            np.linalg.norm(body_triangles[:, 1] - body_triangles[:, 2], axis=1),
            np.linalg.norm(body_triangles[:, 2] - body_triangles[:, 0], axis=1),
        ),
        axis=1,
    )
    triangle_scale = np.median(body_edges, axis=1)
    body_scale = np.full(len(cap.base_verts), np.nan)
    body_scale[requested] = triangle_scale[attachments.triangle[requested]]

    result = classifier.classify_support(
        base=cap.base_verts,
        tris=cap.tris,
        edges=edges,
        fixed=fixed,
        requested=requested,
        authored_distance=attachments.authored_distance,
        body_scale=body_scale,
        support_points=support_points,
        raw_targets=raw_targets,
        corot_targets=corot_targets,
        seam_groups=topo["seam_groups"],
    )
    classifiers = {}
    for name in ("broad", "coherent-v1"):
        mask = result["masks"][name]
        target_key = "broad_target" if name == "broad" else "coherent_v1_target"
        classifiers[name] = {
            "selected_count": int(mask.sum()),
            "selected_vertex_ids": np.flatnonzero(mask).astype(int).tolist(),
            "mask_u8_sha256": mask_sha256(mask),
            "fallback": SURFACE_COROT_FALLBACK,
            "surface_corot_clamp": SURFACE_COROT_CLAMP,
            "morph_steps": MORPH_STEPS,
            "soft_anchor": "on",
            "approved_for_solve": (
                name == "coherent-v1"
                and bool(result["diagnostics"]["coherent_v1_gate_pass"])
            ),
            "target_metrics": result["diagnostics"][target_key],
        }
    return {
        "label": label,
        "capture": path.relative_to(ROOT).as_posix() if path.is_relative_to(ROOT) else str(path),
        "capture_sha256": sha256(path),
        "vertex_count": int(len(cap.base_verts)),
        "material_edge_count": int(len(edges)),
        "triangle_count": int(len(cap.tris)),
        "exact_seam_group_count": int(len(topo["seam_groups"])),
        "classifiers": classifiers,
        "diagnostics": {
            key: value
            for key, value in result["diagnostics"].items()
            if key not in ("broad_target", "coherent_v1_target")
        },
        "broad_target_clamp_telemetry": {
            "counts_per_step": clamp_counts,
            "worst_per_step": clamp_worsts,
        },
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", action="append", type=parse_capture, default=[])
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        classifier.selftest()
        if not args.capture and args.json_out is None:
            return
    captures = args.capture or list(DEFAULT_CAPTURES)
    garments = [audit_capture(label, path) for label, path in captures]
    artifact = {
        "schema": "pathb-support-preflight-v2",
        "solve_contract": {
            "anchor_target": "surface-corot",
            "candidate_body_mode": "continuation",
            "static_control_body_mode": "static",
            "surface_corot_fallback": SURFACE_COROT_FALLBACK,
            "surface_corot_clamp": SURFACE_COROT_CLAMP,
            "morph_steps": MORPH_STEPS,
            "clearance": CLEARANCE,
            "soft_anchor": "on",
        },
        "policy": {
            "broad": (
                "current drape-derived requested mask; clamped surface-corot target; "
                "body-vanilla fallback"
            ),
            "coherent-v1": (
                "requested and material-connected to fixed; lever/body-local-median-edge <=2; "
                ">=75% incident material edges pass all-step geometric coherence; deterministic "
                "closure across every morph step removes new target flips, seam regressions, "
                "expansion worse than max(2x, raw), and contraction worse than min(0.5x, raw); "
                "face area may not collapse below min(0.1x, raw); post-prune "
                "components must retain fixed support and a material face patch"
            ),
            "classifier_inputs_excluded": ["ground_truth", "sculpt", "garment_name_rules", "hand_picked_vertex_ids"],
        },
        "thresholds": classifier.THRESHOLDS.__dict__,
        "tools": {
            "pathb_support_classifier.py": sha256(ROOT / "tools/pathb_support_classifier.py"),
            "pathb_support_preflight.py": sha256(ROOT / "tools/pathb_support_preflight.py"),
            "shell_spike.py": sha256(ROOT / "tools/shell_spike.py"),
        },
        "environment": {
            "python": platform.python_version(),
            "numpy": np.__version__,
            "scipy": scipy.__version__ if scipy is not None else None,
        },
        "garments": garments,
        "all_coherent_v1_gates_pass": all(
            garment["diagnostics"]["coherent_v1_gate_pass"] for garment in garments
        ),
    }
    output = args.json_out or (ROOT / "tmp/pathb/S04_SUPPORT_PREFLIGHT.json")
    if not output.is_absolute():
        output = ROOT / output
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(artifact, indent=2, sort_keys=True, allow_nan=False) + "\n",
        encoding="utf-8",
    )
    print(f"wrote {output} sha256={sha256(output)}")
    for garment in garments:
        broad = garment["classifiers"]["broad"]
        coherent = garment["classifiers"]["coherent-v1"]
        diag = garment["diagnostics"]
        print(
            f"{garment['label']}: broad={broad['selected_count']} "
            f"coherent-v1={coherent['selected_count']} orphan={diag['requested_orphan']} "
            f"pruned={diag['integrity_pruned_vertices']} gate={diag['coherent_v1_gate_pass']}"
        )


if __name__ == "__main__":
    main()
