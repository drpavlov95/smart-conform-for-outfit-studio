# Vampire body clearance and posterior bump handoff

Date: 2026-07-12

## User report

After the first anti-clipping release, the Vampire robe was visually about 86%
better, but the morphed body still showed through parts of the garment and the
rear thigh acquired a recurring local bulge.  Free-hanging lower cloth also
looked as if it retained part of the body's radial silhouette after it had
stopped touching the body.

## Root cause

The final integrity ladder correctly rejected the severely folded full Drape
candidate, but then returned the raw Outfit Studio/vanilla conform verbatim.
That fallback also discarded Drape's final body-clearance pass.  The raw conform
contains the same posterior S-profile visible in the report: lower panels that
are authored away from the body still inherit a radial component of the body
morph.

This was not treated as a generic smoothing problem.  Smoothing positions would
erase authored folds and can trade an old hidden intersection for a new visible
one.  The correction instead starts from the trusted vanilla fallback and uses
the rejected solver candidate only as a bounded direction hint.

## Live RM_Heavy fix

The guarded cleanup is allowlisted to the exact pair
`vampire_robes_alt + RM_Heavy`:

1. A vertex is eligible only when it was separated from the reference body and
   remains separated from the morphed body.  This is the no-contact region where
   continued body influence is physically implausible.
2. Only the candidate's radial XY component is recovered, at 50% strength and
   at most `1.0u`; vertical coordinates are not pulled or smoothed.
3. Exact-seam twins move as one unit, and a seam containing a protected rigid
   vertex is not moved.
4. Every unit is bisected/rejected if an incident authored edge leaves its local
   safe band, a face collapses/flips, or a non-adjacent triangle pair begins to
   cross when that same pair did not cross in the trusted entry mesh.
5. A conservative swept-AABB broad phase keeps the crossing-set test interactive
   without weakening it.
6. The body BVH floor is run again after fallback.  It moves only the garment,
   never the `3BBB` body, and uses the same seam/face/edge/crossing transaction.
7. A downstream garment receives the final trusted robe as collider geometry;
   no proxy from the rejected solver is allowed to leak into the next shape.

The distinction between a crossing *count* and a crossing *set* is important.
An early spike reduced the total count while silently adding 66 different pairs;
that version was rejected.  The release adds zero pairs relative to vanilla.

## Measured result

Against the vanilla fallback that the previous live release published:

| metric | previous fallback | guarded result |
|---|---:|---:|
| body-penetrating garment vertices (serialized OBJ) | 102 | 65 |
| worst vertex body gap | -4.806 | -4.756 |
| authored-external body-face samples | 805 | 739 |
| newly introduced body-face samples | - | 0 |
| non-adjacent crossing pairs | 561 | 519 |
| crossing pairs newly introduced | - | 0 |
| exact-seam maximum split | 0 | 0 |
| worst authored-edge ratio | 5.265 | 5.283 |
| posterior sculpt error | 2.1900 | 2.1652 |
| critical posterior sculpt error | 2.0222 | 1.9934 |
| posterior radial-curvature p95 | 1.0603 | 1.0141 |
| harness replay time | about 0.5 s | about 3.9 s |

The worst authored edge entered already damaged; it increases by `0.0179`,
within the explicit per-edge `+0.02` ceiling.  No healthy edge is allowed beyond
`max(1.25, entry + 0.02)`.  The improvement to the posterior profile is modest
offline, so the textured Outfit Studio view remains the decisive visual gate.

Two identical release replays produced OBJ SHA-256:

`42E9E193199AC519052383FBA7CADBB5EBDBE2A88F1E90B5B99C98C2B0223675`

## Body scope and remaining experiment

Body participation is live for garment vertices through the immutable morphed
`3BBB` BVH.  A denser centroid/edge-midpoint body-face solver was implemented as
an experimental path, but remains dark (`DRAPE_TRUSTED_BODY_FACE=0` by default):
the captured body is open/non-watertight and the robe contains intentional
hidden/internal plates, so raw inside/outside face samples are not a valid loss
without an authored visible-surface mask.  The prototype took about 57 seconds
and produced no publishable movement under the strict topology guard.

Likewise, the generic curvature fallback remains dark via
`DRAPE_TRUSTED_CURVATURE_FALLBACK=0`.  It is too slow without a spatial broad
phase and correctly rejected every attempted movement on this capture.

## Regression gates

- Harness self-test passes.
- All 18 corpus shapes match their goldens with the feature off.
- With anti-clipping on, changes are confined to the two coupled Vampire shapes:
  the robe and the small `Filler` that consumes the robe as collider.  Falmer,
  Blacksmith and Yarl corpora remain unchanged.
- The authoritative live Blacksmith `142410` replay with both release features
  enabled is byte-identical to the previously approved OBJ
  (`0D73FA09442BA747ADDF31B1B76473C0378F3816022EF979CE75E8849FE995E6`).
- The release Vampire replay is deterministic and has no new crossing pair,
  flipped face, collapsed face or seam split relative to its trusted baseline.

## Runtime flags and rollback

Normal live enable remains:

`E:/SkyrimDev/Drape/anticlip_safe_floor_on.flag`

Hard rollback remains:

1. remove `anticlip_safe_floor_on.flag`;
2. optionally create `anticlip_safe_floor_off.flag`;
3. rerun Smart Conform from the base/vanilla slider state.

Do not enable `DRAPE_TRUSTED_BODY_FACE` or
`DRAPE_TRUSTED_CURVATURE_FALLBACK` for the user visual gate.

## Validation artifacts

- `tmp/vampire_clipping_diag/trusted_hybrid_body_v13.obj`
- `tmp/vampire_clipping_diag/audit_trusted_v13.log`
- `tmp/vampire_clipping_diag/trusted_release_renders/`
- `tmp/vampire_body_bump_spike/REPORT.md`
- `tmp/vampire_bump_cleanup/REPORT.md`

## Deployment

Outfit Studio was closed for the final RelWithDebInfo build.  The build output
and both configured mod-folder copies are byte-identical, SHA-256:

`D437984514D924D1332D43D02BEB7F3837524F0E10FD7F24744A8D1384170D95`

Deployed paths:

- `BodySlide and Outfit Studio/CalienteTools/BodySlide/drape-algo.dll`
- `Drape - Cloth Aware Conform/CalienteTools/BodySlide/drape-algo.dll`

Active live flags remain `anticlip_safe_floor_on.flag` and
`posterior_chord_on.flag`.
