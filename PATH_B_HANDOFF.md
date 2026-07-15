# Path B — operational handoff

Updated: 2026-07-15 (America/Sao_Paulo)

Branch: `wip/path-b-shell-solve`. The commit that contains this file is the
authoritative checkpoint; run `git log -1 --oneline` instead of copying an old
hash from this document.

## State at handoff

- This checkpoint is **offline research only**. Production `MERGED`, the DLL,
  live flags and deployed Outfit Studio copies were not changed.
- Phase 1 remains blocked. Do not port `surface-corot`, start an energy solver,
  add sculpt-targeted plumb or tune per garment from this checkpoint.
- Falmer remains benched because its saved OSD ground truth is contaminated.
- Yarl Cloak is a clean geometry reference for section-position/anti-imprint,
  but **not** a validated gravity reference.
- Preserve the user's unrelated worktree files. Never stage, overwrite or
  restore:
  - `corpus/golden/fur_blacksmith_rm_heavy.json`
  - `scripts/capture_outfitstudio_vfs.ps1`

## What S0.3e corrected

The previous “preflight complete” verdict was not reliable:

1. S0.3d characterized production `MERGED`, not the Path-B shell it discussed.
2. PCA-axis sign was read as gravity even though the Yarl Cloak sculpt is above
   both candidates in every measured world-Z band.
3. H3c used authored distance as a proxy for contact and included hard anchors.
4. The offline body collider stopped at the first occupied spatial-grid ring,
   which was not a globally exact closest-point query.

S0.3e fixes these with material-correspondent section sampling, explicit XYZ
and `dDown=-dZ`, coverage reporting, globally exact closest-point
distance/candidates under the existing pseudo-sign convention, per-vertex
collision-correction traces and reproducible runners.

## Current evidence

### Material sections

`dDown>0` means the sculpt is below the candidate; `dDown<0` means above.

| garment/candidate | dDown range | width ratio | shapeP max | shapeA max | incomplete sectors |
|---|---:|---:|---:|---:|---:|
| Blacksmith production | -1.056…+0.397 | 0.973…1.076 | 0.133 | 0.088 | 0/7 |
| Blacksmith shell | +0.311…+3.419 | 1.030…1.175 | 0.133 | 0.098 | 0/7 |
| Yarl Clothes production | -1.791…+0.676 | 0.976…1.011 | 0.158 | 0.081 | 0/7 |
| Yarl Clothes shell | -0.008…+3.928 | 1.000…1.170 | 0.205 | 0.111 | 0/7 |
| Yarl Cloak production | -4.367…-0.012 | 0.692…1.015 | 0.346 | 0.164 | 6/7 |
| Yarl Cloak shell | -2.574…-0.067 | 0.672…1.127 | 0.347 | 0.304 | 6/7 |

The data reject the earlier purely-positional characterization: position,
scale and material distribution all differ, and the open Cloak lacks full
angular coverage in six of seven bands.

### H3c factorial

The primary mask is `fully_free_always_clear`: movable, `drape>=0.55`, and
`minimum_observed_gap > clearance+1e-6` in all eight cells. It happens to equal
`fully_free_never_corrected` in this corpus.

| main effect on indexed mean (+ = worse) | Blacksmith n=254 | Clothes n=350 | Cloak n=10 |
|---|---:|---:|---:|
| anchor authored−vanilla | +0.0047 | +0.0102 | +0.0294 |
| soft off−on | +0.0054 | +0.0150 | +0.1159 |
| body static−continuation | +0.0585 | +0.0651 | +0.0365 |

This only describes a conservative post-treatment subpopulation. It does not
prove that contact or anchors cannot cause a localized bump. On all fully-free
vertices, including corrected ones, the body-mode effect remains large:
`+1.6453 / +0.4700 / +0.4946`.

### `surface-corot`

The offline spike now supports exact barycentric body bindings, local Kabsch
rotation of authored offsets and an optional unilateral target clamp. The
static-body control is byte-identical to authored targets after stabilizing the
clamp (`3d10426b7a7c673e...` for both outputs).

The current classifier binds very broad and distant populations:

| garment | fixed / total | requested bindings | distance p95 / max | >4u / >8u |
|---|---:|---:|---:|---:|
| Blacksmith | 1444/2316 | 1726 | 5.726 / 17.101 | 137 / 48 |
| Yarl Clothes | 1145/1777 | 1269 | 2.721 / 14.587 | 42 / 35 |
| Yarl Cloak | 497/553 | 526 | 11.453 / 15.747 | 176 / 68 |

It fails the implemented three-mean gate with clamp on and off:

| variant | Blacksmith indexed / fwd / rev; flips | Clothes | Cloak |
|---|---:|---:|---:|
| clamp on | 1.9378 / 1.0010 / 1.0478; 13 | 1.3224 / 0.5440 / 0.6241; 16 | 2.4608 / 1.4769 / 0.9908; 50 |
| clamp off | 1.9210 / 0.9995 / 1.0460; 0 | 1.3215 / 0.5443 / 0.6238; 15 | 2.4608 / 1.4769 / 0.9908; 50 |

The narrow falsified hypothesis is: this baseline configuration of independent
nearest-triangle co-rotational bindings over the current broad support mask
passes the gates. There was no support/solver-strength sweep, so this does
**not** reject the whole binding family or a coherent material support-boundary
model.

## Tracked artifacts

- `fixtures/pathb/S03E_H3C_RESULTS.json`
  `3e3eb3f267301ef1a525a7e75a6b38d37f536b3ee566dc31c591b549d09e4ef6`
- `fixtures/pathb/S03E_SECTION_RESULTS.json`
  `f5b52bf3278e9aeaef2d00bf5a3ae185daa2d25d40f32ac4c5845c09eb74c399`
- `fixtures/pathb/S03E_SURFACE_COROT_SCOREBOARD.md`
  `e710f5ca51c61313f1aefccb3c7a8684ea2d5b32d4dd6f73897b39e9977c770f`
- `fixtures/pathb/S03E_SURFACE_COROT_NO_CLAMP_SCOREBOARD.md`
  `683479894751fa03b051090775e02c352de20efc7cd8aba6f8577d1ccae5cc01`

The JSON artifacts embed exact commands, tool hashes and paths/hashes/summaries
for generated OBJs and traces. The raw per-vertex traces remain under `tmp/`
and must be regenerated after a clean checkout. If any runner, metric or spike
source changes, regenerate the corresponding artifact.

## Reproduction

From the repository root:

```powershell
$blacksmith = 'corpus/fur_blacksmith_rm_heavy/drape_capture__Rley_3BA_Vanilla_-_Body_-_Blacksmith__Physics___BlackSmithTorso__RM_Heavy.drcap'
$clothes = 'corpus/yarl_rm_heavy/drape_capture__Rley_3BA_Vanilla_-_Body_-_Yarl_Clothes_2__No_Physics___Clothes__RM_Heavy.drcap'
$cloak = 'corpus/yarl_rm_heavy/drape_capture__Rley_3BA_Vanilla_-_Body_-_Yarl_Clothes_2__No_Physics___Cloak__RM_Heavy.drcap'

py -3 tools/pathb_band_distribution.py --selftest
py -3 tools/shell_spike.py --attachment-selftest
py -3 tools/shell_spike.py --capture $blacksmith --contact-check --attachment-report
py -3 tools/shell_spike.py --capture $clothes --contact-check --attachment-report
py -3 tools/shell_spike.py --capture $cloak --contact-check --attachment-report

py -3 tools/shell_spike.py --capture $cloak --dataset tmp/pathb/s03e_static_cloak_dataset --solve --output tmp/pathb/s03e_static_authored.obj --rest base --morph-steps 6 --iterations 10 --gravity 0 --anchor-target authored --soft-anchor on --body-mode static
py -3 tools/shell_spike.py --capture $cloak --dataset tmp/pathb/s03e_static_cloak_dataset --solve --output tmp/pathb/s03e_static_corot.obj --rest base --morph-steps 6 --iterations 10 --gravity 0 --anchor-target surface-corot --surface-corot-clamp on --soft-anchor on --body-mode static
Get-FileHash -Algorithm SHA256 tmp/pathb/s03e_static_authored.obj,tmp/pathb/s03e_static_corot.obj

py -3 tools/pathb_s03e_h3c.py --jobs 3 --workdir tmp/pathb/s03e_h3c_final_v2 --json-out fixtures/pathb/S03E_H3C_RESULTS.json
py -3 tools/pathb_s03e_sections.py --workdir tmp/pathb/s03e_sections_final_v2 --json-out fixtures/pathb/S03E_SECTION_RESULTS.json
py -3 tools/pathb_gate.py "shell:--rest base --morph-steps 6 --iterations 10 --gravity 0 --anchor-target surface-corot --surface-corot-clamp on --soft-anchor on --body-mode continuation" --label S03E_SURFACE_COROT_CLAMP_ON --out fixtures/pathb/S03E_SURFACE_COROT_SCOREBOARD.md
py -3 tools/pathb_gate.py "shell:--rest base --morph-steps 6 --iterations 10 --gravity 0 --anchor-target surface-corot --surface-corot-clamp off --soft-anchor on --body-mode continuation" --label S03E_SURFACE_COROT_CLAMP_OFF --out fixtures/pathb/S03E_SURFACE_COROT_NO_CLAMP_SCOREBOARD.md
```

The two static hashes must match each other (the recorded Cloak run is
`3d10426b7a7c673ee3e779d9b5581ec079369895dfed05957c947acf2cca2524`).
The two gate commands intentionally exit nonzero because the candidate fails;
that is a recorded scientific result, not a harness crash.

## Next permitted research step

Preflight the **support boundary/classifier** before adding another solver law:

1. Measure binding coherence on material adjacency: triangle-ID discontinuity,
   lever-arm distribution and local target-gradient energy.
2. Propose garment-agnostic boundary candidates (connected material boundary,
   stable proximity over the morph, or a coherent body patch), without names or
   hand-picked vertex IDs.
3. Require the static byte-identical control, lower lever arms and no new flips
   before rerunning the three heavy sentinels.
4. Only after one universal heavy candidate passes may RM_Thin run. Only after
   heavy + thin + visual review may a C++ port be discussed.

Separately, obtain a clean user-confirmed gravity sculpt before making claims
about gravity. The broader universality/belt review remains documented in
`UNIVERSALITY_HANDOFF.md` and is not superseded by this Path-B checkpoint.
