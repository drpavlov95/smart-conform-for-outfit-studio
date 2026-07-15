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
- S04 completed the permitted support-boundary preflight.  Its final
  `coherent-v1 + surface-corot + clamp` package is safe and deterministic, but
  fails all three Heavy sentinels; RM_Thin and a C++ port remain blocked.
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

## S04 coherent support boundary

S04 separated support-target eligibility from mobility.  It did not change
`fixed`, inverse mass or the shell constraints.  The garment-agnostic
classifier consumes only capture geometry/topology, the raw target and the
co-rotational target; sculpt/ground truth, garment names and hand-picked IDs
are excluded.

The frozen v2 contract is continuation over six morph steps, `soft-anchor=on`,
`body-vanilla` fallback, contact-floor clamp on and clearance `0.02`.  Every
selected component must contain fixed support and at least one complete
material face after exact-seam welding.  At every morph step it rejects new
flips, seam regressions, expansion worse than `max(2x, raw)`, contraction worse
than `min(0.5x, raw)` and face-area collapse worse than `min(0.1x, raw)`.

| garment | broad | coherent-v1 | fixed selected | coverage of broad |
|---|---:|---:|---:|---:|
| Blacksmith | 1726 | 998 | 998 | 57.8% |
| Yarl Clothes | 1269 | 590 | 590 | 46.5% |
| Yarl Cloak | 526 | 11 | 11 | 2.1% |

All 18 per-step target checks pass, but the coverage result matters: every
retained vertex is hard-fixed and the Cloak treatment is nearly a no-op.  The
classifier is a conservative safety filter, not evidence of a useful universal
soft support frontier.

The loader is fail-closed on schema, capture/tool/environment hashes, solve
contract, approval, ID type/order/range, selected count and byte-mask hash.
Static `body-vanilla` controls close byte-for-byte to authored for all three
garments.  Repeated continuation solves are byte-deterministic and selected
hard supports have zero final contact-floor violations.

The strict Heavy gate compares all mean+p95 accuracy metrics and integrity to
production baseline x1.05:

| garment | coherent indexed/fwd/rev | vanilla-shell indexed/fwd/rev | result |
|---|---:|---:|---|
| Blacksmith | 1.8650 / 0.9930 / 1.0424 | 1.8508 / 0.9969 / 1.0488 | FAIL; mixed, <1% delta |
| Yarl Clothes | 1.2320 / 0.5013 / 0.6000 | 1.1980 / 0.4832 / 0.5906 | FAIL; all means worse |
| Yarl Cloak | 1.9342 / 1.1943 / 0.9422 | 1.9344 / 1.1940 / 0.9439 | FAIL; effectively no-op |

All three Heavy sentinels fail.  The paired control shows that Clothes gets
1.59-3.75% worse, Blacksmith is mixed and still far outside baseline, and
Cloak has insufficient selected coverage to matter.  Integrity is identical
to the vanilla-shell control.  This falsifies the package that could have been
shipped; it does not justify RM_Thin, tuning, live deployment or a C++ port.

## Tracked artifacts

- `fixtures/pathb/S03E_H3C_RESULTS.json`
  `4714aa2eaf413814338fc2610b29d0c94fed5f5f4fd0a00e871e152cd5aa1cb3`
- `fixtures/pathb/S03E_SECTION_RESULTS.json`
  `94dcf7128d8a3a7682c45dd6bba93abfe19cde04ef86ab521f75c6c749b16511`
- `fixtures/pathb/S03E_SURFACE_COROT_SCOREBOARD.md`
  `5c3591cc11bd689f3894dbd875f10511ad43a8d48d2d33c3889ec544ab9d6e0b`
- `fixtures/pathb/S03E_SURFACE_COROT_NO_CLAMP_SCOREBOARD.md`
  `078ffd3e372e0cda30a80365de7afe517c11ca6ec156acdceba5c0adbacd2adc`
- `fixtures/pathb/S04_SUPPORT_PREFLIGHT.json`
  `587da6ee2a060809cb7ff262133f94e4e4c76b28d43bcc69d1506dd7081e5a13`
- `fixtures/pathb/S04_COHERENT_V1_STRICT_SCOREBOARD.md`
  `99505aeaabba77b497f2b4f0839c2655093033f0afd86ac6adef6f3f17993f2a`
- `fixtures/pathb/S04_SHELL_VANILLA_CONTROL_STRICT_SCOREBOARD.md`
  `df9416a2c227988a7e603672b5938db4383d5f0d31bcb7b1a4f4398c2d46518b`

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

py -3 tools/pathb_support_classifier.py
py -3 tools/pathb_support_preflight.py --selftest --json-out fixtures/pathb/S04_SUPPORT_PREFLIGHT.json
py -3 tools/pathb_gate.py "shell:--rest base --morph-steps 6 --iterations 10 --gravity 0 --anchor-target surface-corot --surface-corot-mask-json fixtures/pathb/S04_SUPPORT_PREFLIGHT.json --surface-corot-classifier coherent-v1 --surface-corot-fallback body-vanilla --surface-corot-clamp on --soft-anchor on --body-mode continuation" --label S04_COHERENT_V1 --out fixtures/pathb/S04_COHERENT_V1_STRICT_SCOREBOARD.md --strict
py -3 tools/pathb_gate.py "shell:--rest base --morph-steps 6 --iterations 10 --gravity 0 --anchor-target vanilla --soft-anchor on --body-mode continuation" --label S04_SHELL_VANILLA_CONTROL --out fixtures/pathb/S04_SHELL_VANILLA_CONTROL_STRICT_SCOREBOARD.md --strict
```

The two static hashes must match each other (the recorded Cloak run is
`3d10426b7a7c673ee3e779d9b5581ec079369895dfed05957c947acf2cca2524`).
All four scoreboard commands intentionally exit nonzero because the tested
candidates/control fail; that is a recorded scientific result, not a harness
crash.

## Final validation

- `pathb_support_classifier.py` selftest and `shell_spike.py
  --attachment-selftest: PASS.
- S04 preflight regenerated byte-for-byte with SHA-256 `587da6ee...`.
- Static closure and continuation determinism: PASS for all three sentinels.
- Selected hard-support final contact-floor violations: `0 / 0 / 0`.
- Exact closest-point contact check: worst distance error `0` on all three.
- Strict production baseline smoke: PASS `3/3`; final candidate: FAIL `3/3`.
- Production corpus: all 18 tracked shapes match golden.
- Three independent read-only peer reviews found no remaining integration
  blocker; they agree that the Heavy failure is the scientific result.

## Next permitted research step

The support-boundary step is complete and rejected at Heavy.  Do not rescue it
with per-garment thresholds, broader coverage or RM_Thin: the paired test says
the approved package is neutral-to-worse before contraction testing.

Path B needs new information before another solver family is justified:

1. obtain a clean, user-confirmed gravity/free-hang sculpt and freeze its exact
   provenance;
2. define localized metrics for the visible under-butt/imprint defect instead
   of relying only on global distance;
3. preflight a genuinely different physical lever against raw and sculpt before
   implementing it, with the same three-garment Heavy gate.

The production universality/belt review can proceed independently on its own
branch.  It remains documented in `UNIVERSALITY_HANDOFF.md`; this checkpoint
does not authorize changes to the current `MERGED` DLL.
