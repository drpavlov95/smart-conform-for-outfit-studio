# BlackSmithTorso live OBJ diagnosis

Date: 2026-07-12

## CRITICAL CORRECTION — superseded at 17:50

This document's original diagnosis used the Y axis backwards.  In these Skyrim
meshes `+Y` is the **front** and `-Y` is the **rear**.  Consequently the
tangential fan, all `y>0` metrics, and `DRAPE_TANGENTIAL_REPAIR` concerned the
front panel, not the glute cleft.  The user's exported
`latestblacksmithtorsopostsmartconform.obj` proves the pass ran, but it changed
exactly zero of the 86 vertices in the true rear-butt ROI.  Its live enable flag
has been removed.

The original user diagnosis was correct: the true rear defect is a
low-frequency surface tuck.  In the paired hand edit the central rear depth at
`z=66` bridges from about `15.20` to `17.61`, while the cheek mean is `18.21`.
The replacement is the opt-in unilateral `-Y` dynamic posterior feather,
documented in `TRUE_REAR_BLACKSMITH_HANDOFF.md`. Treat every conclusion
below that calls `+Y` "rear" or says the cloth already bridges the cleft as
superseded historical evidence only.

## Current conclusion

The remaining RM_Heavy defect is **not primarily body penetration or missing
outward clearance**.  The corrected live exports show a tangentially collapsed,
asymmetric triangle fan in the rear panel.  The user's hand edit keeps almost the
same cloth/body envelope while redistributing the vertices, restoring bilateral
symmetry, edge lengths, and triangle quality.

Do not promote another body-normal push, shell offset, cleft chord, or plumb pass
from the old frozen corpus.  `DRAPE_TENSION_PLUMB` remains OFF by default.

## Verified live inputs

All Outfit Studio exports use scale `0.1`; diagnostics multiply coordinates by
10 without modifying the source files.

| role | file | group | vertices | triangles |
|---|---|---|---:|---:|
| authored/base garment | `tmp/BlackSmithTorso_vanilla mesh.obj` | BlackSmithTorso | 2316 | 3979 |
| bad live result | `tmp/BlackSmithTorso_smart-conformed RM_Heavy slider.obj` | BlackSmithTorso | 2316 | 3979 |
| paired target | `tmp/BlackSmithTorso_hand-edit of the smart-conformed RM_Heavy slider.obj` | BlackSmithTorso | 2316 | 3979 |
| historical sculpt (not paired) | `tmp/BlackSmithTorso_previously hand-sculped RM_Heavy slider.obj` | BlackSmithTorso | 2316 | 3979 |
| current RM_Heavy body | `tmp/torso_live.obj` | 3BBB | 13651 | 26412 |

The four garment face arrays, vertex indices, and face order are exactly equal.
The live body topology/order equals the frozen body topology.

The old frozen inputs are geometrically stale:

- live bad garment vs frozen `cloth_current`: mean `1.061635`, p95 `3.689324`,
  max `8.780005`;
- live body vs frozen `body_morphed`: mean `1.298216`, p95 `3.985555`,
  max `9.280650`.

## Measured failure

The visible star/pinch is around `x=0..3`, `y~=10`, `z=45..55` in native solver
units.  Several authored left-side vertices cross to the right of the centreline.
For example, vertex 1600 moves from authored `x=-2.676` to bad `x=+0.780`; the
hand edit restores it to `x=-2.629`.

Central triangle/edge measurements:

| metric | bad live | hand edit |
|---|---:|---:|
| minimum-angle p05 | 8.74 degrees | 27.12 degrees |
| edge-aspect p95 / max | 6.56 / 7.60 | 2.19 / 2.32 |
| minimum edge length / authored | 0.204 | 0.774 |
| central edges below 0.5 authored length | 6 | 0 |
| triangles below 0.25 authored area in rear-butt region | 8 | 0 |

The bad garment already spans the body's cleft.  In the rear-butt sample it has
zero penetrations and minimum signed body gap `0.116`.  The mean rear gap is
essentially unchanged by the hand edit (`2.62934 -> 2.62921` in the paired
analysis).

The exact hand-edit vector is overwhelmingly tangential:

- rear z=35..70: about 94% of edit energy is tangential;
- cleft median tangential fraction: 99.665%;
- cleft mean normal component: 0.185 vs tangential 1.835;
- 86% of the remaining symmetric edit moves downward and aligns with gravity
  projected onto the tangent plane.

The bad morph also breaks bilateral symmetry.  In the rear z=35..70, morph
symmetry RMS is `2.161`; the hand edit reduces it to `0.000465`.  Projecting the
bad displacement field onto bilateral symmetry removes about 58% of its rear
squared error, but does not alone undo the compressed triangle fan.

## Offline repair spike

The validated formulation deliberately repairs sampling quality while retaining
the entry cloth surface:

1. enforce a lower edge-length barrier against **authored** rest lengths;
2. run four tangential umbrella/barrier rounds, reprojecting each vertex to its
   fixed local entry-surface neighbourhood after every round;
3. finish the authored compression barrier, exact-seam weld, and body floor;
4. only then project the **candidate minus authored** displacement field onto
   bilateral symmetry in a soft rear/glute ROI.

Applying symmetry before tangential relaxation reintroduced asymmetry, so order
is part of the fix.  The production candidate does not include the earlier
gravity experiment.  The balanced offline composition scores target index mean
`0.52140`, target-surface mean `0.19807`, minimum-angle p05 `26.792` degrees,
aspect p95 `2.211`, minimum authored edge ratio `0.76393`, and minimum body gap
`0.14895`, with zero flips or penetrations.  It still requires the user's live
visual approval.

## Experimental C++ port

The combined repair is implemented in `algo/conform_hook.cpp` as
`ApplyAuthoredTangentialRepair`, behind `DRAPE_TANGENTIAL_REPAIR` and default
OFF.  It runs after the true-final body floor and before the older tension-plumb
experiment.  If it moves the visual mesh, downstream garments receive the
unchanged entry mesh as their collision proxy.

Harness enable: `DRAPE_TANGENTIAL_REPAIR=1`.  Outfit Studio enable:
`E:/SkyrimDev/Drape/tangential_repair_on.flag`.  Removing that file restores the
baseline without recompiling; `tangential_repair_off.flag` is an explicit kill
switch.

Current handoff state: the `RelWithDebInfo` DLL was deployed to both configured
OutfitStudio mod folders and `tangential_repair_on.flag` is present for the next
live A/B.  The compiled default remains OFF; deleting the flag is an immediate
rollback.  Build and both deployed DLLs have SHA-256
`0A8ABABF60AF25DCA2967805D3D39F85763CCB02B8BF792AF0778063034E0DFA`.

Main defaults:

```text
DRAPE_TANGENTIAL_EDGE_FLOOR=0.75
DRAPE_TANGENTIAL_TRIGGER_FLOOR=0.50
DRAPE_TANGENTIAL_TRIGGER_EDGES=4
DRAPE_TANGENTIAL_MAX_ROI=512
DRAPE_TANGENTIAL_BODY_DIP=1.20
DRAPE_TANGENTIAL_LAP_ROUNDS=4
DRAPE_TANGENTIAL_LAP_K=0.80
DRAPE_TANGENTIAL_FINISH_ROUNDS=16
DRAPE_TANGENTIAL_BARRIER_K=0.90
DRAPE_TANGENTIAL_BARRIER_STEP=0.35
DRAPE_TANGENTIAL_LAP_STEP=0.30
DRAPE_TANGENTIAL_REPROJECT_MAX=0.10
DRAPE_TANGENTIAL_PRE_SYM_MAX_MOVE=2.20
DRAPE_TANGENTIAL_SYMMETRY=1
DRAPE_TANGENTIAL_SYMMETRY_PAIR_TOL=0.40
DRAPE_TANGENTIAL_SYMMETRY_BODY_RMS=0.15
DRAPE_TANGENTIAL_MAX_MOVE=3.30
DRAPE_TANGENTIAL_BODY_FLOOR=0.10
DRAPE_TANGENTIAL_COVER=2.0
```

The production-shaped version adds these guards to the Python spike:

- original preset must be `Fitted Cloth`;
- body-derived glute-cleft ROI with soft feather;
- hard activation ceiling of 512 ROI vertices;
- at least four core edges must be shorter than `0.50 * authored rest`;
- rigid islands and vertices covered by an outer garment are protected;
- surface reprojection searches only the fixed local two-ring, is capped at
  `0.10u`, and fails closed if no valid local point exists;
- exact seams move and participate in all safety checks together;
- tangent/barrier stage trust region `2.20u`, combined symmetry trust region
  `3.30u`;
- bilateral projection runs only when the reference-to-current body morph is
  itself symmetric;
- body floor, trust, and normalized face orientation are checked again after the
  final weld;
- any NaN, face flip, severe area collapse, penetration, failed local
  reprojection, or trust-region breach reverts the complete pass.

On the 2026-07-10 14:24 live-body replay (which reproduces today's bad OBJ to
mean `0.035u`), OFF -> ON gives:

| metric | OFF | ON | hand edit |
|---|---:|---:|---:|
| target index mean in rear ROI | 1.080 | 0.569 | 0 |
| target index p95 in rear ROI | 2.490 | 1.000 | 0 |
| target surface mean in rear ROI | 0.276 | 0.223 | 0 |
| central minimum-angle p05 | 8.81 deg | 27.80 deg | 27.12 deg |
| central aspect p95 | 6.51 | 2.14 | 2.19 |
| minimum central edge/authored rest | 0.204 | 0.767 | 0.774 |
| central edges below 0.5 rest | 6 | 0 | 0 |

The pass moves 159 vertices, maximum `3.186u`, changes no broad non-ROI vertex
by more than `0.0071u`, creates no penetration or face flip, and is deterministic
(repeated combined-result OBJ SHA-256
`CEBE2CCFACE61EB5B53946112BE2F1297B4591F87A96BC5F0F140EA22AD26AE8`).

The final default-OFF regression run matches all 18 golden corpus shapes.  A
paired OFF/ON OBJ isolation replay also checks all 18 captures byte for byte:
17 are exact no-ops and only BlackSmithTorso changes.  Belt, Pants, BaseArmor,
Falmer, Vampire, and Yarl therefore retain their baseline output.  In the old
Blacksmith corpus the torso's
`worstSupportEdgeStretch` rises because the deliberately repaired edges were
already collapsed in support (for example `0.363u support -> 2.164u repaired`
against `2.672u authored`); authored `worstEdgeStretch` remains unchanged.  This
is the failure mode the repair is designed to correct, not a new tear.

Useful artifacts:

- `tmp/diagnostics/01_live_bad_vs_handedit_full_views.png`
- `tmp/diagnostics/04_live_butt_horizontal_slices.png`
- `tmp/diagnostics/07_live_butt_triangle_quality.png`
- `tmp/diagnostics/08_live_butt_normal_vs_tangential_edit.png`
- `tmp/diagnostics/11_symmetry_projection_variants.png`
- `tmp/diagnostics/12_tangential_repair_candidate.png`
- `tmp/diagnostics/14_cpp_combined_tangential_repair.png`
- `tmp/diagnostics/live_export_geometry_report.txt`
- `tmp/diagnostics/symmetry_projection_live.py`
- `tmp/diagnostics/symmetry_projection_metrics.json`
- `tmp/diagnostics/cpp_tangential_repair_metrics.json`
- `tools/check_live_tangential_repair.py`
- `tools/check_tangential_corpus_isolation.py`

`tools/patch_capture_body.py` can replace a stale capture's morphed body and
rebuild its radial BodyShell for diagnostics.  It cannot recover a newer
`rawDiff`; the current stage diagnosis therefore uses the genuine `142410`
live-body capture, not a patched capture.

## Captured-stage result

The live-body capture
`drape_capture__Rley_3BA_Vanilla_-_Body_-_Blacksmith__Physics___BlackSmithTorso__RM_Heavy__142410.drcap`
uses a morphed body bit-identical to `tmp/torso_live.obj`.  Its replay matches the
user's bad live garment to about `0.035u` mean distance.  The companion Belt
capture is `...BlackSmithBelt__RM_Heavy__142411.drcap`.

`supportVerts` is exactly `baseVerts + rawDiff` and is serialized by capture v3.
Inspection of this capture shows the same six sub-0.5 authored edges and the
collapsed fan already in `support_raw` (minimum edge ratio about `0.182`).  The
defect therefore originates upstream in vanilla Automorph/rawDiff/support, not
in a later Drape pass.  This is also why the older compression pass, whose rest
lengths come from support, accepts the bad fan as its rest state.  The capture
flag is absent; no additional capture is currently required.

## Production constraints

- Keep any new feature OFF by default until the user's Outfit Studio visual gate.
- Gate symmetry on a symmetric body morph and mirrored authored topology; do not
  symmetrize intentionally asymmetric garments/sliders.
- Use soft ROI weights; a hard symmetry ROI creates a visible correction boundary.
- Preserve exact seam groups every iteration.
- Use authored rest lengths for the compression barrier.  The existing dark
  `DRAPE_COMPRESS_K` uses `supportRestLen`, which cannot see a collapse already
  present in vanilla conform/support.
- Reapply body floor after tangential work and keep a downstream collider proxy
  if corpus layers drift.
- Validate Blacksmith, Yarl, Vampire, Falmer, and the full 18-shape corpus before
  a default-on decision.
