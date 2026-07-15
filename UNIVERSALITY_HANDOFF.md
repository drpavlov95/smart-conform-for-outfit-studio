# Smart Conform universality handoff

Updated: 2026-07-15 (America/Sao_Paulo)

## Objective

Make Smart Conform behave plausibly across modded and vanilla Skyrim outfits,
with special attention to free-hanging cloth, body/garment anti-clipping,
layer order, belts, seams and disconnected rigid decorations. Preserve the
Blacksmith and Vampire results already approved by the user.

Production/universality branch: `wip/codex-anticlip-harness`. Path-B shell
research is isolated on `wip/path-b-shell-solve`; its operational checkpoint is
`PATH_B_HANDOFF.md`. Use `git log -1 --oneline` on the relevant branch rather
than copying an old commit from this document.

The only unrelated tracked worktree change is the user's EOF/newline change in
`corpus/golden/fur_blacksmith_rm_heavy.json`. Never stage, overwrite or restore
that file. `scripts/capture_outfitstudio_vfs.ps1` is also an unrelated untracked
user file and must not be staged by Path-B commits.

## Path B checkpoint — corrected S0.3e (2026-07-15)

The earlier “preflight complete” conclusion was retracted after direct audit:

- S0.3d compared the hand-sculpt to production `MERGED`, not to the Path-B
  shell whose model it claimed to characterize.
- PCA-axis sign was interpreted as gravity. In world coordinates the Yarl
  Cloak sculpt is above both candidates in every measured band, so a `-Z`
  force worsening its score does not reject gravity.
- S0.3c's `authored gap>2` population contained hard anchors and vertices later
  reached by the body; `body-mode` was static-vs-continuation, not contact off/on.

S0.3e now provides material-correspondent section metrics, explicit XYZ/dDown,
coverage, globally exact offline closest-point distance/candidates under the
existing sign convention, and per-vertex collision-correction histories. The
corrected factorial only supports a narrow statement:
effects are small on the conservative subpopulation that is movable,
`drape>=0.55` and remains above clearance in every cell. It does not rule out
contact/anchors as a localized bump cause; that mask is selected after
observing all eight cells.

An offline-only `surface-corot` support target was implemented as the missing
physical middle between copied rawDiff and frozen authored anchors. Static-body
equivalence with authored targets is byte-identical after stabilizing the
clamp. It still fails all three implemented mean gates with clamp on and off;
with clamp it also produces `13/16/50` flips. It is blocked from RM_Thin, C++
and live deployment. The result is narrow: broad independent nearest-triangle
bindings under the current support mask failed; it does not reject every
coherent support-boundary model.

The production DLL, `MERGED` behavior and live flags were not changed. Yarl
Cloak is now a section-position/anti-imprint sentinel, not a gravity sentinel.
Falmer remains benched. Next research, if resumed, starts with support-mask /
boundary-condition classification or a clean gravity ground truth — not an
energy solver or sculpt-targeted plumb.

## User-confirmed Blacksmith reference

The correct post-Smart-Conform export is:

```text
E:\SkyrimDev\Drape\tmp\BlackSmithTorso_smart-conformed RM_Heavy slider.obj
SHA-256 3AB75C9F5636E528C4F421ABB70CA19C115F9E924356E5C634CB5EF7A1DBB2C9
2316 vertices / 3979 faces
```

Do not substitute `latestblacksmithtorsopostsmartconform.obj` or the older
hand-edit/sculpt exports. This confusion previously caused several incorrect
mesh diagnoses.

## Coverage reality

The committed corpus is a regression gate, not a universality claim. It has 18
RM_Heavy garment captures from five female 3BA outfits. It does not yet cover a
male body, bodyless accessory project, transverse bandolier, skin-tight outfit,
non-manifold stress case or enough rigid armor.

Ignored inventories and analyses:

- `tmp/universality/UNIVERSALITY_MATRIX.md`
- `tmp/universality/LEMORIA_INVENTORY.md`
- `tmp/universality/REALISM_AUDIT.md`
- `tmp/universality/LEMORIA_AB.md`
- `tmp/universality/lemoria_snapshot/`

The realism audit supports the user's body-imprint hypothesis. On authored-far,
currently-free cloth, local cloth displacement still correlates with local body
morph detail. Broad non-plumb lean is a separate low-frequency support/tension
problem; it should not be tuned with the same smoothing knob.

## Resolver and classifier corrections

The project resolver now:

1. decodes XML named and numeric entities, so an Outfit Studio title containing
   `Savior's` matches an OSP containing `Savior&apos;s`;
2. validates a logged reference against the same OSP and its shape list;
3. prefers a recognized body name such as `3BBB` when no valid logged reference
   is available;
4. has no first-shape garment fallback;
5. accepts an unknown logged reference only if its NIF geometry is broadly
   full-body-like (substantial closed human-height surface).

This prevents a bodyless project from silently treating a pouch as the body.
Verified results:

```text
Savior's Hide (Im Physics) -> reference=3BBB, status=ok
Rley Wayward Ranger - Satchel -> reference="", status=no-usable-reference
RLY_TEST_BDO Lemoria Armor -> reference=3BBB, status=ok
```

The core and UI classifier also no longer find the token `arm` inside the word
`armor`. Shoulder/pauldron/plate are handled before limb tokens; generic
`Armor1/2/3` remains Auto.

## Publication-aware capture v4

`algo/conform_hook.cpp` implements append-only `.drcap` v4 publication state:

- solver candidate remains available for diagnosis;
- `publicationAware` explicitly distinguishes new captures from rewritten
  legacy captures;
- `expectedPublishMode` records StaticCloth or fallback-mask publication;
- `expectedPublishedDiff` materializes what Outfit Studio receives;
- metrics, OBJ output and sequential colliders use the published result for v4;
- v1-v3 reader behavior remains legacy-compatible.

The Outfit Studio 5.6.3 semantics are reproduced exactly: `SetResultDiff`
overwrites supplied keys while retaining omitted raw keys; `ZeroVertDiff`
removes absent/zero-mask keys, preserves weight 1 and scales other values.
`CAPTURE_PUBLISH_VANILLA` exists as a replay/schema mode but is not currently
emitted by a live call site.

Self-tests cover StaticCloth merge, fallback mask and v4 round-trip. A live v4
Lemoria capture has now exercised the StaticCloth publication path end to end;
the fallback-mask path remains covered headlessly but still needs a dedicated
live fixture.

## Lemoria baseline

The original reference-correct v3 captures remain under
`captures/universality/lemoria_rm_heavy` for historical A/B. The publication-
aware baseline is now under
`captures/universality/lemoria_rm_heavy_v4` (both ignored): 11 garments,
`RM_Heavy`, reference `3BBB`, 13,651 body vertices and 26,412 body triangles.
The project was closed without saving and `captures/enable_capture.flag` is
absent.

Do not use the older poisoned Lemoria run which selected `Armor1` as the body.
Do not promote the v3 captures to committed goldens: BeltDec's stored candidate
can differ from the fallback actually published by the old live build. In the
v4 run all 11 shapes recorded `publish='static'`; notably, guarded BeltDec is
now accepted and its original capture replays with
`expectedPublishMode=static`, zero expected diff mismatches and zero expected
vertex error.

Validation of the v4 directory:

- sequential `--replay-dir`: 11 files, zero failures;
- original individual captures: expected publication is StaticCloth and output
  matches the live host exactly;
- every capture was replayed to OBJ twice in fresh directories: 11/11 SHA-256
  pairs are byte-identical;
- no `REFERENCE MISMATCH`, capture failure or solver exception was logged.

The ignored automation transcript is
`tmp/universality/capture_lemoria_v4.ps1`. OutfitStudio 5.6.3 requires both
`--project NAME OSP`. Because the target runs at the MO2 integrity level,
native `Conforming...` and close-without-save buttons must also be clicked from
an MO2-run PowerShell child; a lower-integrity shell receives access denied.

### Belt M1 transaction

Lemoria exposed that belt ARAP runs after structural guards and can reintroduce
severe damage. The current pass-local transaction snapshots immediately before
ARAP; the proposal includes ARAP, thin-strap fallback and body floor. It builds
material components, unites exact-seam twins and either accepts or restores a
whole material component.

Implemented gates: authored edge band, face orientation/area, exact-seam split,
body vertex and triangle samples, and non-adjacent crossing-set. The crossing
audit is now exact-seam-aware so triangles adjacent through duplicated UV/seam
vertices are not false positives. There is no inter-layer collider gate in the
belt component transaction; do not claim one.

`DRAPE_BELT_COMPONENT_GUARD` is an override. Its automatic default is enabled
only for belts with at least eight disconnected components, which keeps the
approved Blacksmith/Yarl belts unchanged while covering Lemoria BeltA/B/BeltDec.

Measured Lemoria result:

| Mesh | Components rolled | Old -> guarded worst authored edge | Other result |
|---|---:|---:|---|
| BeltA | 28 / 34 | `6.30 -> 6.30` | the inherited worst edge predates ARAP; support peak `2.05 -> 1.12` |
| BeltB | 10 / 10 | `4.40 -> 2.43` | roughness `1.218 -> .891` |
| BeltDec | 68 / 133 | `21.46 -> 3.22` | magnetism count `268 -> 4`; candidate becomes globally publishable |

## Finisher transaction research (not promoted)

Panel flatten, cleft bridge and hem smoothing run after the last structural
checkpoint. On Lemoria they turn trusted support stretch near `1.12` into
`3.09` (SkirtA) and `6.06` (SkirtB), reintroducing topology/layer debt while
improving roughness.

Two experimental gates exist and are OFF by default:

- `DRAPE_FINISHER_COMPONENT_GUARD=1`: whole material-component rollback using
  edge/face/seam/body/crossing gates. It has no layer gate and is too coarse for
  a one-component skirt.
- `DRAPE_FINISHER_BLEND_GUARD=1`: applies the largest global alpha accepted by
  authored/support edges, faces, seam-aware crossing-set and body audits. It
  also contains an experimental direct signed-gap layer gate; set
  `DRAPE_FINISHER_BLEND_LAYER_GUARD=0` for topology-only diagnosis because open
  garment colliders cause nearest-face discontinuities.

With the layer gate disabled, accepted alphas are still visually small:

| Mesh/pass | Accepted alpha |
|---|---:|
| SkirtA panel / cleft / hem | `.03125 / .007812 / .007812` |
| SkirtB panel / cleft / hem | `.001953 / .015625 / .000977` |

Final A/B remains close to all-finishers-off:

| Mesh | support stretch | roughness | sequential layer residual vertex/face |
|---|---:|---:|---:|
| SkirtA | `1.132` | `.786` | single replay `48 / 283` |
| SkirtB | `1.137` | `.385` | single replay `385 / 2684` |

Therefore neither finisher guard should be enabled in a release yet. The next
research step is a pass-local support-edge proposal repair: exact-seam groups as
one delta DOF, entry-relative edge bands, under-relaxed projection, body floor
per cycle, followed by the complete alpha audit. It must improve roughness
materially without exceeding approximately `1.13` support stretch.

## Validated checks at this handoff

- `drape_harness` RelWithDebInfo builds.
- `--self-test` passes, including XML decode, fail-closed reference policy,
  publication merge/mask and v4 round-trip.
- all 18 committed corpus shapes pass unchanged;
- exact Savior/Satchel/Lemoria resolver matrix matches the expected results;
- `drape_algo` and `drape_proxy` RelWithDebInfo build and deploy successfully;
- build output and both configured deployment copies are byte-identical;
- the 11-file live Lemoria v4 capture passes sequential, expected-output and
  two-run determinism checks;
- `git diff --check` is clean aside from normal CRLF conversion warnings.

Current deployed hashes (build output plus both MO2 destinations):

```text
drape-algo.dll F56EA6DDA64F0E0CE50E92AD4391675ED3113EA2CFEA8B1036E5BF4884EBB235
version.dll    FAC922875A0AB9899E5CE8C04B9298E8A4F904E4F91FF57148E396848A5FF9C7
```

## Universality queue

Use this order, one capture directory per project and slider:

1. `RLY_TEST_BDO Lemoria Armor`, `RM_Heavy`, 11 garments (v4 baseline complete;
   use it as the modded regression case).
2. `Rley 3BA Vanilla - Body - Archmage (Physics)`, `RM_Heavy`, 5 garments:
   `Bags`, `Cape`, `Pants`, `robes:0`, `robes:1`.
3. `Rley 3BA Vanilla - Body - Thalmor Robe (No Physics)`, `RM_Heavy`, 6
   garments including `LongSkirt`, `ThalmorRobesF`, buckle/collar and pants.
4. `Rley 3BA Vanilla - Body - Ebony Mail`, `RM_Heavy`, 10 garments: rigid
   multi-layer cuirass, two belts, two cloths and leg armor.
5. `Rley Wayward Ranger - Armor`, `Chubby Refit` (no RM_Heavy), 12 garments:
   corset/belts, laces, breastplate, straps and several layers.
6. `Rley Wayward Ranger - Satchel`: negative fail-closed resolver test only
   until the SliderSet has a body reference; do not run Smart Conform normally.

Then test `Ancient Nord (Im Physics)` for chain skirt, straps, fur and plates.
The longer-term belt/bandolier design backlog remains in
`BELT_AND_PIPELINE_REVIEW.md`, Part 24.

## Reproduction commands

```powershell
cmake --build build --config RelWithDebInfo --target drape_harness
build\algo\RelWithDebInfo\drape-harness.exe --self-test
py -3 scripts\check_corpus.py

build\algo\RelWithDebInfo\drape-harness.exe `
  --replay-dir captures\universality\lemoria_rm_heavy_v4

build\algo\RelWithDebInfo\drape-harness.exe `
  --resolve-project "Rley 3BA Vanilla - Body - Savior's Hide (Im Physics)" `
  --bodyslide-dir "E:\Wabbajack\InstallPacks\mods\Rley Mods - Vanilla Refits\CalienteTools\BodySlide"
```

For live capture, create `captures/enable_capture.flag` only immediately before
launching Outfit Studio through MO2, remove it immediately after Smart Conform,
close the project without saving, and verify the new files replay as v4 with an
explicit `publishMode`.

## Commit discipline

- Stage only intentional source/docs; exclude the user's Blacksmith golden.
- Keep experimental finisher gates dark until both metrics and renders pass.
- Push universality checkpoints to `origin/wip/codex-anticlip-harness` and
  Path-B research checkpoints to `origin/wip/path-b-shell-solve`; never mix the
  two workstreams in one commit.
