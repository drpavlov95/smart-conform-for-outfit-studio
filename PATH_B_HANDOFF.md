# Path B — operational handoff

Updated: 2026-07-23 (America/Sao_Paulo)

Branch: `wip/path-b-shell-solve`. The commit that contains this file is the
authoritative checkpoint; run `git log -1 --oneline` instead of copying an old
hash from this document.

---

# Codex continuation 2026-07-23 - thigh bump / butt entry fixed at solver handoff

## Exact scene and root cause

The authoritative reproduction is the live Blacksmith Torso Heavy capture:

`captures/yarl/drape_capture__Rley_3BA_Vanilla_-_Body_-_Blacksmith__Physics___BlackSmithTorso__RM_Heavy__081443.drcap`

The exact live authored/body/production/shell/final OBJs from the user's
08:14 scene are frozen under the ignored directory
`tmp/pathb/live_freeze_20260723_0814/`. The replay is vertex-identical to the
user's visible result.

`DRAPE_PATHB_TAIL_TRACE=1` now writes full-topology
`%TEMP%/drape_tail_trace_<index>_<stage>.obj` checkpoints and logs the same
rear-under-belt body-gap criterion at each one. It is measurement-only and OFF
by default.

The trace proved that the Path-B shell itself was not creating the reported
butt entry:

| stage | RMS / max displacement from shell | rear pens `< -0.3u` | worst rear gap |
|---|---:|---:|---:|
| accepted shell | `0 / 0` | `0` | `+0.0200u` |
| after legacy correction core | `2.5436 / 9.4114u` | `8` | `-1.1894u` |
| old final output | `2.5499 / 9.3111u` | `6` | `-0.9746u` |

The old diagnostic label `01-layer-order` bracketed more than layer order. The
actual fault was control-flow fallthrough: after `PathBSimV2Solve` published a
complete shell, the non-Verlet correction loop, surface smoother, layer pass
and authored-rest edge pass solved that shell a second time toward the
vanilla/raw correction pose. This single block also created the localized
right-thigh deformation. Later layer-tuck/cleft/hem passes were not the
origin.

## Fix: material-domain handoff, not an anatomical ROI

Simply skipping the correction core fixed the skirt but regressed attached
sleeves (`3 -> 15` visible arm penetrations). The shipped candidate therefore
keeps each solver in the material domain it actually handles:

- attached cloth (`drapeEl` near zero: sleeve, cuff, collar, fitted bodice)
  retains the legacy fitted correction;
- free cloth (`drapeEl >= 0.30`) returns exactly to the accepted Path-B shell;
- `drapeEl 0.15..0.30` uses smoothstep blending for a C0-continuous handoff.

This uses the existing universal material classifier that already controls
anchors, gravity and mobility. There are no Blacksmith names, coxa/butt
coordinates, vertex lists or garment-specific ROIs in the fix. On the exact
scene it classified `1541 attached / 94 transition / 681 free` vertices.

Runtime research/rollback controls:

- `DRAPE_PATHB_ATTACHED_CORE=0`: clean-shell diagnostic (known to regress
  attached sleeves; do not ship);
- `DRAPE_PATHB_ATTACHED_BLEND_START=0.15`;
- `DRAPE_PATHB_ATTACHED_BLEND_END=0.30`;
- `DRAPE_PATHB_LEGACY_CORE=1`: exact full legacy fallthrough. It reproduces all
  `2316` old output vertices byte-for-byte and is the instant rollback.

## Validation

The new Blacksmith output is deterministic over repeated replays:

`C1D4BFBAE23126EF39F3AA0387E97C69B044DF5ECA48B45B99D4ED1BF838967B`

Focused `pathb_defect_gate.py` result versus the previous deployed candidate:

| gate | old | material handoff | result |
|---|---:|---:|---|
| visible skin, all | `10` | `6` | better |
| visible skin, arms | `3` | `3` | held |
| body-through total | `75` | `71` | better |
| inverted faces | `23` | `23` | held |
| collapsed faces | `0` | `0` | held |
| open seam | `0` | `0` | held |
| rear-under-belt pens | `6` | `0` | fixed |
| garment face residuals | `1133` | `1106` | better |

`tools/pathb_render_sheet.py` was also inspected; the new side silhouette has
a continuous fall instead of the localized right-thigh bump, and the rear
close-up no longer enters the butt crease. The evidence sheet is ignored at
`tmp/pathb/tail_bisect/material_handoff_vs_old.png`.

Additional validation:

- harness self-test PASS;
- RelWithDebInfo and Release harnesses produce the exact same Heavy candidate
  SHA-256 (`C1D4BFBA...8967B`), closing the cross-build equilibrium check for
  this fix;
- default/shell-OFF corpus PASS: Falmer `8`, Blacksmith `4`, Vampire `2`, Yarl
  `4` shapes (18/18 total), no golden update;
- the user's modified
  `corpus/golden/fur_blacksmith_rm_heavy.json` was never staged or overwritten;
- `scripts/capture_outfitstudio_vfs.ps1` and runtime flags remain user state.

The live Path-B allowlist is still BlackSmithTorso + RM_Heavy. Shell-all
research replays were run on Yarl Clothes/Cloak, Vampire and Falmer without
crash or NaNs, but they are not promoted by this fix.

## Installed state

The 2026-07-23 Release build was deployed while Outfit Studio was closed. The
build artifact and both configured mod-folder copies are byte-identical:

`241CFE81CC977F61672497B98B6589D535A4E3CA180BAFACEF29E164B519A77B`

Deployed paths:

- `BodySlide and Outfit Studio/CalienteTools/BodySlide/drape-algo.dll`;
- `Drape - Cloth Aware Conform/CalienteTools/BodySlide/drape-algo.dll`.

Active live flags remain `shell_solve_on`,
`shell_transactions_off`, `anticlip_safe_floor_on` and
`posterior_chord_on`; the posterior chord is skipped whenever Path-B is
accepted.

# Codex continuation 2026-07-19 - true hull closed, release transition isolated

## Installed/live state (do not confuse with the research build)

The installed algorithm remains the user-tested Release DLL from the previous
handoff. Both mod folders were re-verified after this session:

- MD5 `AD9530E916B3B45AAF15393BB17B1E24`
- SHA-256 `70FBC074335970D4EDE29A2EB9605DD5F99A4512532878971FB78A487CDA7A26`

No Path-B experiment from this continuation is deployed. `DRAPE_SHELL_CONVEX_CLEFT`
still defaults to `0`. Important build trap: the CMake target `drape_algo` has an
unconditional POST_BUILD copy to both mod folders. A RelWithDebInfo validation
build briefly copied itself there; it was immediately replaced with the exact
Release DLL/hash above. Future compile-only validation must build in a tree with
deployment disabled or restore the Release artifact immediately afterward.

The exact live dumps captured before any harness replay are preserved (ignored)
under `tmp/pathb/live_freeze_20260719_1903/`. Their SHA identities match the
late-session handoff: authored `300C0D...`, body `A21F87...`, production
`C36844...`, published `688DF7...`.

## True 2D hull: implemented correctly, validated, still opt-in

`tools/pathb_true_hull_spike.py` is the offline mirror and diagnostic. It has a
synthetic monotone-chain/ray-intersection self-test, exact plane/triangle pelvis
sections, continuous posterior profiles, front-only rejection, authored seam and
face-integrity gates, and displacement-field smoothing. It deliberately does not
smooth positions: the superseded positional Laplacian moved vertices up to `4.60u`
for a real hull correction of only about `0.30u`.

`algo/conform_hook.cpp::PathBConvexCleftBridge` now implements the same geometry:

- true Euclidean XY hull per overlapping z-slab;
- posterior wedge only (`BACK=-y`, half-angle 52 degrees);
- lower-body ceiling at 55% of body height (excludes spine/scapula grooves);
- body hull is only the concavity gate; cloth targets its own hull;
- two material rings, scalar outward-displacement diffusion, exact-seam tying;
- independent of the legacy HorizontalBridge positional smoother.

On the frozen live scene the Python spike moves 50 posterior vertices, mean
`0.06143u`, maximum `0.30137u`, zero front vertices, with source/candidate both
`104` flips, `9` collapsed faces and seam split `0`. On the rebuilt corpus C++
pilot it moves 49 posterior vertices (max `0.30161u`), zero front vertices, and
keeps baseline/candidate at `103` flips, `9` collapsed and seam split `0`.
Surface means improve slightly (`fwd 0.9075 -> 0.9064`, `rev 1.0855 -> 1.0839`).
C++ vs Python mirror differs by only `0.000048u` mean / `0.01494u` max.

Harness reproducibility from the rebuilt source:

- OFF: `B553947D...EF7D`, identical over two runs;
- ON: `B8ECEB96...2761`, identical over two runs;
- default/OFF corpus: 18/18 shapes green.

The focused evidence is `tmp/pathb/true_hull_cpp/CPP_true_hull_focused.png`.
It proves the hull is safe and geometrically correct, but also that green and
current orange almost overlap. This is a minor central-cleft repair, not the
dominant remaining cloth-sim fix. Do not deploy it before the user sees/approves
that focused image.

## Dominant residual: contact-release transition, not horizontal cleft

The live exact surface diagnostic separates two phenomena that earlier renders
mixed together:

- Horizontal XY sections at z 42/48/54/60 are already almost convex/flat across
  the posterior center; their true-hull deficit is at most about `0.30u`.
- Below roughly z=60 the panel already falls nearly plumb.
- In the buttock/flank band (`5<|x|<11`, z=60..70), live cloth-to-body unsigned
  distance is min/p10/p50/p90 `0.264/0.308/0.349/0.410u`: this is still the
  support/contact transition.
- In the central band (`|x|<3`, z=60..70), the corresponding median is `1.981u`.
  The cloth center is free while the lateral panel remains near the body.

Thus the visible under-butt curve is mostly the solver retaining body contact too
far down the lateral panel, then releasing into an already-straight fall. It is
not a long-range force on the lower hem and cannot be fixed by increasing gravity.

Two tempting channel edits were falsified and removed; do not repeat them:

1. Tightening `DRAPE_SHELL_CHAN_FLOOR_SLACK` globally from `1.2` to
   `0.9/0.6/0.3` moved the profile outward but added respectively `4/13/8` face
   flips relative to that build's baseline. A hard floor fights contact/stretch.
2. A smooth authored-gap release blend (old slack in the worn band, `0.6` only
   on authored-free cloth) still worsened surface means to `0.8849/1.0585` and
   added 5 flips. It changed the nonlinear equilibrium non-locally. The code was
   removed rather than left as another dead flag.

## Contact trace: the contact is active, but it is the reaction

`DRAPE_SHELL_TRACE_CONTACT=1` now writes
`%TEMP%/drape_live_contact_trace.csv`.  It records, per material vertex and
morph step, minimum/final body gap, fixed/sticky state, vertex and face contact,
and both channel directions. `tools/pathb_contact_trace.py` summarizes it and
can freeze region membership from a baseline with `--selection-trace`.  The
trace is measurement-only: OFF and ON produced the identical Heavy OBJ SHA-256
`B553947D...EF7D`.

For the ten baseline material vertices in the posterior lateral transition
(`5<|x|<11`, `z=60..70`), the final step shows:

- all `10/10` under vertex contact (`117` events), face contact (`1786` events),
  the channel ceiling (`279` events), and sticky memory;
- median minimum gap `-0.7285u`, reconciled to final median `0.3567u`.

So contact really persists.  But removing/arbitrating the channel proves it is
not the root inward driver:

- skipping the ceiling after contact improved surface mean only slightly and
  raised flips `104 -> 152`; it was removed;
- disabling the whole channel retained contact on `10/10`, worsened surface
  fwd `0.9075 -> 1.0454`, and raised flips `104 -> 121`;
- making bending 200x more compliant was almost a no-op (surface fwd `0.9078`,
  flips `102`).

Do not repeat channel or bending sweeps.  Contact is mostly reacting to an
intrinsic-metric mismatch rather than magnetically attracting the cloth.

## Graded-rest probe: real lever found, blocked on integrity/interlayer

`DRAPE_SHELL_GRADED_REST=1` is a new default-OFF harness probe.  Free-free edge
rest lengths follow `L(t)=Lbase*exp(t*log(Lraw/Lbase))`; raw positions are never
targets.  This fixes the current inconsistency where free material keeps the
base/slim metric while supports already follow Heavy. Two runs are byte-identical
at SHA-256 `8EC751ED...EF6B`.

On the exact same ten baseline material IDs, full graded rest changes the last
step as follows:

- final median body gap `0.3567 -> 0.8076u`;
- median minimum gap `-0.7285 -> +0.3146u`;
- vertex contact `10 verts / 117 events -> 4 / 4`;
- face contact `10 / 1786 -> 8 / 702`;
- surface fwd/rev means `0.9075/1.0855 -> 0.8041/0.9618`;
- harness magnetism count `1101 -> 831`.

This is the first measured mechanism that actually releases the affected panel
and improves both surface directions.  It is not deployable yet: flips rise
`104 -> 146`, collapsed faces `9 -> 12`, and torso-vs-BlackSmithBelt residuals
rise from vertex/face `24/87` to `58/397` (worst about `3.12u`).  This confirms
the next missing pieces: fold/self-intersection control for the newly available
material and interlayer contact/order during the solve.  Do not tune the rest
blend to hide those failures; keep full target metric as the physical contract
and repair the integrity mechanisms.

Immediate continuation:

1. Map the 42 added flipped faces and 310 added Belt face conflicts to material
   components/bands; distinguish local fold inversion from layer crossing.
2. Add a broad-phase self/interlayer collision probe behind a separate flag,
   with the Belt treated as the authored outer layer.
3. Re-run full graded rest and require baseline-or-better flips/collapses and
   interlayer residual before any visual/live pilot.
4. Independently close the cross-build equilibrium drift gate before any DLL
   deploy.  Determinism within one binary is already proven; equivalence across
   Release/research builds is not.

## State at handoff

- No experiment from this continuation is deployed.  Both installed DLLs must
  remain the user-tested Release SHA-256 `70FBC074...A7A26` / MD5
  `AD9530E9...B1E24` described at the top of this document.
- Source scope remains hard-coded to `BlackSmithTorso` + `RM_Heavy` + the
  merged UI preset `Cloth`; historical fitted/loose enums are accepted only for
  captures. `CONVEX_CLEFT`, `TRACE_CONTACT`, and `GRADED_REST` all default OFF.
- Current-source default Heavy is deterministic at `B553947D...EF7D`.  The
  measurement trace is byte-identical to that result.
- The local `shell_solve_on.flag` and `shell_transactions_off.flag` are runtime
  user state, not files to stage. `shell_solve_off.flag`, if created, wins.
- RM_Thin, Yarl, Vampire and every other garment remain on the legacy path.
  Do not broaden the pilot before a new multi-garment gate/review.
- Falmer remains benched because its saved OSD ground truth is contaminated.
- Preserve the user's unrelated worktree files. Never stage, overwrite or
  restore:
  - `corpus/golden/fur_blacksmith_rm_heavy.json`
  - `scripts/capture_outfitstudio_vfs.ps1`

The S0.3e-S05 material below is a historical research checkpoint. It is kept
for provenance, but its old statements that C++/live work is blocked are
superseded by Marcos 8 and 9. The Marco 9 section is authoritative for the
active pilot.

## Historical S0.3e correction (superseded)

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

## Historical S0.3e evidence

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

## S05 active checkpoint - localized Heavy defect (2026-07-15)

The next step remains on `RM_Heavy`, which is the primary slider and therefore
the release gate. Read-only causal analysis has now separated two effects that
the earlier whole-mesh scores mixed together:

- with anchors, rest state and soft-anchor settings held identical, changing
  only `body-mode=static` to `body-mode=continuation` moves the conservative
  always-clear/free population by mean/p95/max `0.251/0.963/2.495u` on
  Blacksmith and `0.151/0.821/1.636u` on Yarl Clothes;
- most of that response is within one or two material-graph hops of vertices
  corrected by body contact; at three to five hops it falls to roughly
  `0.03-0.05u`;
- the same ablation changes broad edge verticality by only about `0.3 deg`.

Therefore the visible local bump/contact imprint and the broad non-plumb fall
are separate objectives. S05 is promoting frozen, seam-aware local metrics
before changing the solver: causal continuation-minus-static displacement and
its material Laplacian, area-weighted error islands against clean sculpts, and
a diagnostic body-morph transfer score with candidate-independent coverage.
Ground truth remains evaluator-only and cannot select solver vertices.

Two candidate levers are permitted only after those metrics and missing
integrity gates are frozen: unilateral tension-only stretch, and disabling the
dihedral constraint only on a baseline-frozen contact-to-always-clear frontier.
This is a no-sweep `2x2`, paired again with static/continuation on every Heavy
sentinel. Production, DLL and deployment remain untouched.

Important gate debt discovered during S05: `pathb_metrics.py` documents a
non-adjacent crossing check that its current `integrity()` function does not
implement, and it has no final body-floor set check. Do not accept a
tension-only result until new crossing-set inclusion, frozen-clear contact,
edge/area and section/hem collapse gates exist.

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

## Historical S0.3e-S05 validation

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

## Historical next-step decision (superseded by Marcos 8-9)

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

## Marco 8 audit and live-pilot scope (2026-07-16)

Commit `b3b7c45` added an offline experiment with a positional gravity load on
every solver iteration and tension-only stretch.  A post-commit peer audit
found that its original unilateral implementation skipped compressed edges
without projecting the accumulated multiplier.  The worktree now uses the
proper inequality projection `lambda=min(lambda+dlambda, 0)` and includes
`shell_spike.py --solver-selftest`.  The corrected Blacksmith g=0.02 output is
visually/numerically almost identical to the old one (mean displacement
between them `0.000287u`, max `0.0614u`).

The broader marco-8 release claim is retracted:

- Blacksmith has zero authored-normal flips, but the g=0.02 candidate replaces
  part of the non-adjacent crossing set: 64 crossing pairs are new even though
  the total falls from 308 to 218.  Crossing count alone was a false gate.
- Yarl Clothes g=0.02 reaches max base-edge stretch `10.595x`.
- Yarl Cloak g=0.02 has 21 authored-normal flips and max stretch `13.313x`.
- Vampire g=0.02 has 61 flips and 30 faces below 10% authored area.
- The repeated positional load is iteration-budget dependent.  It is an
  experimental iterative load, not a demonstrated converged quasi-static
  equilibrium.
- `anchor-target=vanilla` and the soft-anchor field still inject `rawDiff` into
  supported/transition vertices.  Therefore the marco-8 candidate does **not**
  yet satisfy the strong claim that the body influences cloth only through
  contact; that remains a model task after the live mechanics pilot.

The immediate C++ work is therefore a **dark live pilot only**, hard-scoped to
`BlackSmithTorso` + `RM_Heavy`, behind `DRAPE_SHELL_SOLVE=1` or
`shell_solve_on.flag` and OFF by default.  No other shape/slider may enter it.
The C++ core must match the corrected spike and publish transactionally.

A viable crossing degradation was measured offline for Blacksmith g=0.02:
iteratively restore only the vertices of newly crossing face pairs (plus exact
seam twins) to the trusted entry.  The ring-0 closure converges in 3 rounds,
restores 156/2316 vertices (6.74%), leaves zero new crossing pairs, preserves
production flip/collapse/max-edge bounds, and differs from the raw simulation
by only `0.1334u` RMS.  One- and two-ring dilation regress face orientation and
must not be used.  This closure plus downstream body/inter-layer floors and a
final transaction are required before live deployment.

Universal Yarl/Vampire enablement remains blocked on a better support/anchor
model and self-contact/crossing treatment.  Never describe the current sim-v2
as corpus-clean or universally deployable.

## Marco 9: transactional C++ Heavy pilot (2026-07-16)

The dark C++ pilot is implemented in `algo/conform_hook.cpp`, still hard-scoped
to `BlackSmithTorso` + `RM_Heavy` + the merged UI preset `Cloth` and OFF by
default. Runtime enablement is `DRAPE_SHELL_SOLVE=1` in the harness or
`shell_solve_on.flag` live; `shell_solve_off.flag` has explicit precedence.

An earlier literal-port build was correctly rejected by a new corresponding
body-face gate: vertex penetrations fell slightly, but canonical triangle
samples rose from 1,228 to 3,196. The solver now projects contact at the triangle centroid
and three edge midpoints as well as at vertices. This is an intentional live
safety extension over the Python marco-8 spike. It reduced the raw candidate to
1,184 penetrating samples before transactional closure.

Publication uses a monotone fixed point of three local ring-0 closures. Every
repair only restores implicated face vertices plus exact-seam twins to a
trusted entry pose:

1. no new non-adjacent crossing pair;
2. no new/deeper corresponding body penetration at any vertex or canonical
   triangle sample (tolerance `1e-4u`);
3. no new authored-normal flip, <10% area collapse, or >8x area explosion.

The same fixed point runs again after the topology-safe final body floor. The
old posterior-chord, tangential-repair and tension-plumb positional passes are
skipped only for an accepted Path-B pilot, so nothing remodels it after its
transaction. Any solve, convergence, seam, body, crossing, area or edge gate
failure publishes the trusted accepted shell instead; failure before initial
acceptance leaves the production pipeline unchanged.

Canonical Heavy replay results (`DRAPE_SHELL_SOLVE=1`):

- initial accepted shell: flips `14 -> 2`, vertex penetrations `193 -> 193`,
  face samples `1231 -> 1209`, no corresponding regression, no new crossings;
- final accepted output: flips `14 -> 0`, collapsed faces `0`, vertex
  penetrations `193 -> 110`, face samples `1231 -> 1002`, max authored area
  ratio `7.106x`, max edge ratio `4.065x`, exact seam split `0`;
- two full replays are byte-identical, SHA-256
  `14C332CD83C3AC59C092F97A0224A6EFFE6F75C8072F8C6AC50976251EB210F7`;
- non-scoped `BlackSmithBelt/RM_Heavy` is byte-identical with the flag ON/OFF;
- feature OFF: all four corpora pass (`18/18` shapes);
- self-test, shadow-selftest and unilateral XPBD selftest pass;
- informative sculpt metric: surface forward p95 improves from production
  `2.7564u` to `2.6379u`; mean is nearly neutral/slightly worse
  (`0.7563u -> 0.7699u`). Visual judgment remains the deciding gate.

The harness returns code 4 for an ON replay because its expected diff is the
tracked production golden; that is the expected signal that the opt-in result
changed. Treat byte identity and the explicit Path-B log gates above as the
pilot acceptance criteria, not equality to the old golden.

Do not broaden this code to Thin, other sliders, Yarl or Vampire. The current
gravity remains an iteration load and vanilla soft/hard anchors still inject
`rawDiff`; a converged energy model and a better support law remain future Path
B research, not claims of this pilot.

### Current-source reproduction and live deployment

Run from `E:\SkyrimDev\Drape` with Outfit Studio closed:

```powershell
$h = '.\build\algo\RelWithDebInfo\drape-harness.exe'
$py = 'C:\Users\T-Gamer\AppData\Local\Python\bin\python.exe'
& $h --self-test
& $h --shadow-selftest
& $py .\tools\shell_spike.py --solver-selftest
& $py .\scripts\check_corpus.py --harness $h --corpus-root .\corpus

$env:DRAPE_SHELL_SOLVE = '1'
& $h --replay '.\corpus\fur_blacksmith_rm_heavy\drape_capture__Rley_3BA_Vanilla_-_Body_-_Blacksmith__Physics___BlackSmithTorso__RM_Heavy.drcap' `
  --result-obj '.\tmp\pathb\repro_heavy_on.obj'
Remove-Item Env:DRAPE_SHELL_SOLVE
```

The final ON replay intentionally exits `4` against the old production golden.
It must log both local closures with `stable=1`, `publish ... accepted=1`, and
`final accepted=1`; its OBJ SHA-256 must be `14C332CD...210F7`.

The DLL was copied only to:

- `E:\Wabbajack\InstallPacks\mods\BodySlide and Outfit Studio\CalienteTools\BodySlide\drape-algo.dll`
- `E:\Wabbajack\InstallPacks\mods\Drape - Cloth Aware Conform\CalienteTools\BodySlide\drape-algo.dll`

The **last deployed marco-9 DLL artifact** and both installed copies have
SHA-256
`6B325B888A4D1C9A0FA8AD63F7B3314FA62F619C4BE697C7A4F1BA305350B790`.
The branch source and harness now also contain the later bilateral research
toggle; that source has not been rebuilt into or copied over the live DLL.
Live state at handoff: `shell_solve_on.flag` present,
`shell_solve_off.flag` absent. Restart Outfit Studio before visual testing.

### User visual gate - still open (2026-07-16)

The user tested the deployed live DLL. The first impression was that the new
gravitational fall was functional on Blacksmith Heavy, but the user then
clarified that the result is not yet 100%. This is a partial positive signal,
not an approval: do not expand the pilot to other garments from this result.

The user then refined the diagnosis after rotating and inspecting the live
mesh: the residual is broader than the butt cleft.  Parts of the skirt that no
longer touch the body still inherit its curve; where the panel should hang
almost vertically it retains a visible body-following "magnetism".  The next
research direction requested by the user is therefore a closer approximation
to a real cloth simulation, not another positional posterior correction.

This report is now measured on the canonical post-conform Heavy OBJ
`tmp/pathb/current_heavy_on_1.obj` (SHA-256 `14C332CD...210F7`).  On a frozen
rear free-cloth mask (`authoredGap >= 2.5u`, `rawGap >= 2.5u`, full drape
mobility, rear `z=35..65`), the current shell still retains about 72.5% of the
horizontal raw-conform displacement and reduces mean body gap from `3.750u`
to `3.106u`.  Two different residuals exist:

- a central island is only partly corrected by the raw shell and is then
  partly restored by safety closures;
- a second inclined island is born inside the raw shell itself, so removing
  downstream finishing cannot solve the whole defect.

All inspected hotspot vertices have full mobility and zero soft-anchor weight.
Turning soft anchors off moves the genuinely free population by only about
`0.005u`; it is not the missing lever.  Ablating cleft bridge, layer tuck,
support cap, or all three also leaves the plumb gate effectively unchanged,
while removing layer tuck regresses Torso/Belt separation.  Keep those legacy
passes for the current accepted pilot.

Two aggressive alternatives were rejected and were **not deployed**:

- publishing the raw shell before all legacy tail operations produced severe
  diagonal waviness (`13.16 deg` mean, `46.52 deg` p90 on the sculpt-informed
  plumb mask);
- the first time-integrated dynamic prototype (uniform mobile mass, hard
  attachments only, bilateral edges, analytic diedro, gravity and unilateral
  body contact) reduced raw-follow alpha but did so by laying the panel onto
  the body: mean gap fell to `0.959u`, with `37/60` sampled vertices below
  `1u`.  It also had not settled (`0.456u/s` RMS speed at frame 60).  This is a
  useful falsification of that parameterization, not a candidate DLL.

#### No-sculpt plumb/magnetism gates

Freeze a candidate-independent face set from the authored/raw inputs:

1. every face vertex has `authoredGap >= 2.5u`, `rawGap >= 2.5u`, and full
   drape mobility;
2. the authored face is already near vertical, using
   `thetaBase = asin(abs(nBase dot gravity)) <= 10 deg`;
3. evaluate `theta = asin(abs(n dot gravity))` on exactly those frozen faces.

Report area-weighted mean, ordinary and area-weighted p90, and the largest
connected material island above `8 deg`.  Always pair this with mean/p10 body
gap and counts below `1u/2.5u`; otherwise a candidate can appear straighter
merely by collapsing onto the collider.  This is now implemented by
`tools/pathb_plumb_gate.py`, including a synthetic selftest and optional frozen
authored-coordinate bounds.

There are deliberately two judges.  The fully global, garment-agnostic
Blacksmith mask has 306 faces: production OFF is `2.895/6.783 deg`
area-mean/area-p90 and current live is `3.164/7.189 deg`.  A localized
Blacksmith posterior sentinel adds authored bounds `y<=-6, z=35..65`; it has 78
faces and reproduces the user's visible zone: OFF `2.609/6.0 deg`
area-mean/ordinary-p90, current `3.180/7.2 deg`.  The coordinate-bounded judge
is a frozen capture sentinel, **not** the universal selector for other
garments.  Keep the sculpt-specific 41-face judge only as an informative fine
validation.

Current decision: leave the deployed DLL and flags untouched.  The first
isolated experiment was bilateral in-plane edge preservation (triangle shape);
after its negative result, work moved to a corrected time-integrated XPBD
settle.  The
physical contract is: body influence enters a mobile panel only through real
attachments, unilateral contact, and cloth constraints; no free-vertex
`rawDiff` attraction.  No candidate may ship unless it improves the frozen
plumb gate **and** preserves the gap, crossing, face/body, seam, area, edge and
Torso/Belt gates of marco 9.

#### Physical-solver audit and rest-metric correction

The isolated C++ bilateral-edge probe is complete.  Its default
`DRAPE_SHELL_TENSION_ONLY=1` reproduces the canonical OBJ byte-for-byte.  With
the toggle set to `0`, all transactional gates still pass, but the pose changes
only `0.0211u` mean / `0.0719u` p95 and the old magnetism diagnostic becomes
slightly worse (`444 -> 448` flagged vertices).  This proves that simply making
the existing positional solver resist compression is not the missing lever.
On the canonical new gates the global result is mixed/minuscule (live
`3.164/7.189 deg`, bilateral `3.137/7.212 deg`, with gap `5.041 -> 5.064u`),
while the user's posterior zone is unchanged/slightly worse (`3.180/7.196 ->
3.183/7.215 deg`, gap `3.106 -> 3.104u`).  The experiment was not deployed.

A review of the first dynamic v3 found the principal reason it collapsed onto
the Heavy body: it used base/slim edge lengths as the in-plane rest metric.
The final raw conform requires substantial graded expansion:

- all raw/base edges: p50 `1.2186x`, p95 `1.9630x`;
- active-active edges: p50 `1.1236x`, p95 `1.9857x`;
- the hand sculpt independently has active-active p50 `1.1024x`, p95
  `2.0627x`.

Therefore the raw conform already contains approximately the right
**intrinsic metric**; its defect is mainly extrinsic position/curvature.  The
next solver must take edge lengths from the graded raw mesh without taking raw
vertex positions as a distributed target.  For continuation use
`L(t)=Lbase*exp(t*log(Lraw/Lbase))`; for the causal first probe start at the
final raw pose/body with `L0=Lraw`, fixed supports at raw, no soft attraction,
then settle under gravity and frictionless contact.

The audit also found that the legacy vectorized Jacobi update is not valid
XPBD: it stores the full multiplier update while applying position correction
divided by vertex valence and relaxation.  Compliance consequently depends on
topology and the solver never reaches the metric it believes it enforced.
`tools/shell_spike.py` now contains an isolated analytic diedro gradient and a
deterministic conflict-free colored-Gauss-Seidel prototype plus synthetic
selftests.  This remains harness-only research.  Before any C++ port it must
demonstrate convergence, timestep stability, non-negative physical contact,
and the complete plumb+gap+integrity gate.

#### Current physical-probe verdict (no candidate, 2026-07-16)

The exact-raw metric and corrected GS family have now been exercised far
enough to reject the current parameterizations.  Against the global live
plumb gate (`3.164/7.189 deg` area-mean/area-p90):

- continuation with corrected raw metric: `4.313/8.440 deg`;
- raw-settle with legacy Jacobi: `4.122/7.473 deg`;
- raw-settle colored GS, soft bend: `4.386/8.814 deg`;
- colored GS, medium bend/contact `0.02u`: `4.141/8.414 deg`;
- area-mass physical GS, strong bend: `4.398/7.871 deg`.

The later candidates preserve/increase gap instead of cheating by body contact,
which validates the rest-metric diagnosis, but all worsen plumb.  The strong
physical run also fails its own solver gate: no convergence in 90 frames,
`5` flips, `1` collapsed face, and p95 stretch `1.0667x`; its active vertices
do finish above the contact floor (`0` active violations), while `145` fixed
legacy supports remain inside it.  Nothing in this table is deployable.

A contact-disabled invariant test (`g=0`, `x0=raw`, `L0=Lraw`, bend off) stays
exactly at raw: zero indexed/surface error, zero strain and convergence in one
frame.  This validates the corrected membrane/integrator core but is not a
visual candidate; raw itself contains the body-following pose.  The next
research blocker is physical contact/support treatment plus fold control
(ultimately self-collision), not another gravity-strength sweep.

### Outfit Studio startup hang diagnosis (2026-07-19)

The repeated startup freeze is in the thin `version.dll` proxy, not in the
Path-B solver.  Windows recorded four consecutive `Application Hang` events;
the latest installed proxy (`96b0c1a`, SHA-256
`D808FA2767C192F2186FAD251E6520A24E9BB740D98593EA6EDE394A439FE864`) reached
`DrapeWndProc: menu bar redrawn on UI thread` without loading
`drape-algo.dll` or installing MinHook, and the process still hung.

The usvfs log supplies the root cause.  `proxy/version_proxy.cpp::LoadReal()`
calls `LoadLibraryExW(L"version.dll", ..., LOAD_LIBRARY_SEARCH_SYSTEM32)`, but
MO2 intercepts the name before the Windows search policy is applied and
reroutes it to:

`.../mods/Drape - Cloth Aware Conform/CalienteTools/BodySlide/version.dll`

That is another copy of the Drape proxy, not the Windows system DLL.  Its
forwarded VERSION exports therefore resolve back to the same proxy and recurse
or tail-loop on Outfit Studio's UI thread.  The per-process mutex only prevents
the second copy from installing another menu; it cannot prevent export-forward
recursion.  This also explains why deferring `DrawMenuBar`, MinHook and
`drape-algo.dll` did not cure the final hang.

Fix implemented and deployed: `proxy/version_proxy.cpp` now constructs the
absolute path returned by `GetSystemDirectoryW` plus `\\version.dll`, loads
that exact path, verifies the module path returned by the loader, and fails
cleanly instead of forwarding through an invalid module.  Release proxy
SHA-256: `8108AEE92C0B74EF8E7BDEA226A293B844C2B60E48F32C4540211E161EBCE8F8`.
The identical DLL is installed in both the BodySlide and Drape mod folders;
the prior proxy is backed up under
`tmp/startup_proxy_backup_20260719_1834/`.

Validation passed on 2026-07-19:

- `dumpbin` reports the same 17 named exports in the proxy and the Windows
  System32 DLL;
- a direct forwarded `GetFileVersionInfoSizeW` smoke returned `1932` bytes
  with last-error zero;
- an Outfit Studio instance launched through the MO2 shortcut stayed
  responsive for every 0.5-second sample over 12 seconds, reached the normal
  main window, and produced no new Windows `Application Hang` event;
- the new run logged only the primary proxy attach, whereas the failing run
  loaded a second mod proxy during VERSION forwarding.

The test Outfit Studio process (`PID 9992`) was still responsive when the
automated close message was blocked by the MO2 process privilege boundary; it
can be closed normally by the interactive user.

### Worktree ownership / safe staging

The intended checkpoint allowlist is `PATH_B_HANDOFF.md`,
`SHELL_SPIKE_LOG.md`, `tools/pathb_plumb_gate.py`, `tools/shell_spike.py`, and
the default-preserving `algo/conform_hook.cpp` toggle.  Never stage the
user-owned `corpus/golden/fur_blacksmith_rm_heavy.json`, the untracked
`scripts/capture_outfitstudio_vfs.ps1`, or the runtime
`shell_solve_on.flag`.  Do not use `git add -A`.

---

# Handoff 2026-07-19 (late session) — for Codex

Branch `wip/path-b-shell-solve`. Checkpoint commit = the one containing this
text; use `git log -1 --oneline`, do not trust hashes copied from prose.

## 1. Deployed state RIGHT NOW

- `drape-algo.dll` md5 `ad9530e9…`, `version.dll` from `53d09ab`, both mod
  copies in sync. Corpus OFF 18/18 green, byte-identical determinism verified.
- Live flags: `shell_solve_on`, `shell_transactions_off`,
  `anticlip_safe_floor_on`, `posterior_chord_on`. (`shell_solve_off` was used
  during the freeze bisect and has been removed — the sim is ON again.)
- `DRAPE_SHELL_CONVEX_CLEFT` defaults **0** (see §4). `DRAPE_SHELL_HULL`
  defaults 0. `DRAPE_SHELL_SHADOW_RETURN` defaults 0.

## 2. The startup-freeze saga — resolved, root cause was NOT the solver

Outfit Studio froze on open, every time, whenever the DLL was enabled. Root
cause (found by the user, `d114ded` + `53d09ab`): MO2/usvfs intercepts
`LoadLibraryExW(L"version.dll")` in `proxy/version_proxy.cpp::LoadReal()` and
reroutes it to the **other copy of our own proxy** in the Drape mod folder, so
the forwarded VERSION exports recurse on the UI thread. Fix = resolve
`GetSystemDirectoryW()` + `\version.dll` absolutely and hard-fail if the
resolved module is either proxy.

Three additional latent bugs were found and fixed while bisecting. They did
not cure this hang but are real and should stay fixed:

| Commit | Bug | Evidence |
|---|---|---|
| `d4eb455` | `DrawMenuBar` called from InjectThread — synchronous cross-thread call into the UI thread | log always died between `InsertMenu` and the next line |
| `00066f3` | MinHook install from InjectThread suspends the UI thread to patch | injection logged SUCCESS but the posted redraw was never serviced |
| `96b0c1a` | heavy `drape-algo.dll` LoadLibrary during host startup | UI alive after injection, still hung |

Net architectural gain: **startup is now free of all our heavy code**. The
proxy only subclasses the WndProc and inserts the menu item; the algo DLL is
loaded and the detour installed lazily, on the UI thread, at first click.
Keep it that way — it is also the right shape for shipping.

## 3. Seeing-pipeline findings (these invalidated a lot of earlier analysis)

1. **My renders never reached the user.** For most of the project I described
   images only I could see. Every visual claim must now be published as an
   Artifact (or a file he opens). Two exist:
   `claude.ai/code/artifact/0f94c4d2-…` (orientation audit) and
   `…/12492e6f-…` (cleft proposal).
2. **Corpus ≠ live.** I validated on the corpus capture while he judged his
   live session; the two bodies differ materially (his butt back-extent 18.6
   vs corpus 16.2; entry facePen 2226 vs 1231). The DLL now dumps his live
   scene each solve: `%TEMP%\drape_live_{body,production,authored,published}.obj`.
   **Careful:** running the harness overwrites those files with corpus data.
3. **Orientation flip-flop, settled.** FRONT = **+y**, BACK = **−y**, decided
   by LEG anatomy (calf bulges back, patella forward) in
   `tools/pathb_view.py::Orientation.detect`, which refuses to guess when the
   two voters disagree. Torso landmarks are ambiguous on a heavy body and
   produced a confidently WRONG verdict earlier the same day. Any leakmap data
   or profile table from before 2026-07-17 may carry the mirrored labels.

## 4. Cloth state — where the actual work stands

**User-confirmed defect (his words, confirmed against a render of his own
mesh): "a saia não pode estar entrando na bunda em nenhum lugar"** — the
skirt shrink-wraps each buttock and **dips into the gluteal cleft**. It is not
penetration (measured: zero body-through-cloth faces at the butt) and not hem
width. He wants the skirt to pass smoothly over the cleft like a curtain.

**His calibration (he circled the best per view on a contact sheet):**
- BACK → the sim's plumb fall (he preferred it over the sculpt itself)
- FRONT and PROFILE → production is the ruler
- The sculpt was circled nowhere ⇒ **directional rail, never a literal target**

**What is in the code:**
- `6321996` ported the production `HorizontalBridge` into the sim as a final
  pass. It is honest but far too conservative — on his mesh it barely dented
  the cleft (the sim back is only slightly smoother than production).
- `ac5d1ca` added the original `PathBConvexCleftBridge`, **gated OFF, and that
  historical revision was wrong**: it treated a straight line in (azimuth, radius) space as a chord,
  but that is a spiral in the plane, so on a cross-section with varying radius
  it lifts points that are already convex. Measured on corpus: front
  9.1 → 11.0, back 15.5 → 18.3 — it inflates the garment instead of bridging.
  The Python prototype (`tools/pathb_shapematch_spike.py` lineage) hid this by
  restricting to the back arc, where the profile is nearly flat.

**Superseded/completed by Marco 20:** the bridge is now a true 2D convex hull
per z-slice with a body-concavity gate and displacement-field smoothing.  It is
safe but only a minor correction; the contact/graded-rest findings at the top
of this handoff are the current continuation point. It remains default OFF.

## 5. Standing process rules (earned the hard way)

1. **Sculpt is the primary gate** before any deploy, with an anti-overfit
   guard: one global material parameter set, cross-garment check, and the
   primary gate never drives a per-garment knob.
2. **Every visual claim goes through `tools/pathb_view.py`** (anatomy-verified
   axes, contact sheets, body+garment together) and is **published where he
   can see it**.
3. **Read the live debug log before theorising** about any test he ran.
4. **No new geometric prosthesis without declaring it as one** and saying why.
5. A defect that is already visible in our own logs must never be shipped for
   his eye to find (the `facePen` lesson).

## 6. Open, in priority order

1. True 2D-hull cleft bridge (§4) — the one thing he actually asked for.
2. The sim's back is right by his eye; the FRONT should read like production.
3. Speed: ~13 s per conform. Now that the hook is lazy this is tolerable, but
   it is still the reason a conform feels like a hang.
4. Never re-landed: the "true final output" dump (`476e937`, reverted in
   `992520d`) — useful, but verify it cannot affect startup.
