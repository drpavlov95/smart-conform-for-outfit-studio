# Vampire anti-clipping / integrity handoff

Date: 2026-07-12

> 2026-07-12 follow-up: the vanilla integrity fallback now preserves a guarded
> no-contact hang correction and reruns body clearance instead of discarding it.
> See `VAMPIRE_BODY_BUMP_HANDOFF.md` for the body/bump diagnosis, topology guards,
> current measurements and deployment state.  The older byte-exact-vanilla
> deployment section below is historical.

## User report and corrected classification

At `RM_Heavy=100`, `vampire_robes_alt` shows large dark/reversed lower panels in
Outfit Studio. The 34-vertex `Filler` is not the source of those panels: it is
restricted to the upper front torso (`z=86.7..109`) and its current-vs-vanilla
change is material at only one vertex. Hiding/removing it from offline renders
does not change the reported lower-skirt defect.

The robe is a single NIF shape containing many disconnected plates/panels. The
baseline Drape result has:

- 493 non-adjacent triangle crossings;
- 224 crossing pairs not present in the vanilla conform;
- 30 faces with normals flipped relative to vanilla;
- 4 faces below 10% of their vanilla area;
- worst authored edge stretch `8.94`;
- 21 body-penetrating vertices, worst harness gap `-3.70`.

`DRAPE_FINAL_FLOOR=0` proved the largest local cause: crossings fall to 372 and
stretch to 5.76, but body penetrations rise to 54/worst `-4.76`. The old floor
therefore traded clipping for panel/order corruption.

## Transactional final floor

`DRAPE_ANTICLIP_SAFE_FLOOR` replaces the legacy final floor only for exact shape
`vampire_robes_alt`. Every candidate body-floor push:

1. moves an exact-seam group as one unit;
2. is capped to `0.40u` per cycle;
3. preserves incident face area and orientation;
4. preserves incident authored-edge lower/upper bounds;
5. is rejected or bisected if it creates a new non-adjacent triangle crossing;
6. runs deterministically for at most three cycles.

The crossing test compares the local mesh immediately before/after the move, so
authored stacked/coplanar plates do not freeze the solver. It prevents new
non-coplanar crossings rather than attempting to erase every existing overlap.
It does not yet test the swept volume between both poses, so a sufficiently thin
panel could theoretically be crossed between endpoints. The pilot contains that
risk with the `0.40u` cap, the exact-shape allowlist and the final vanilla
integrity fallback.

Safe-floor candidate before the final guard:

| metric | old floor | safe floor | floor off |
|---|---:|---:|---:|
| crossings | 493 | 372 | 372 |
| flips vs vanilla | 30 | 24 | 25 |
| dot<0.25 faces | 42 | 34 | 34 |
| area expansions >5x | 17 | 0 | 0 |
| worst edge stretch | 8.94 | 4.57 | 5.76 |
| body penetrations | 21 | 19 | 54 |

Thus the transactional floor is a real safety improvement, although it does not
by itself repair the robe's upstream lower-shell shape.

## Integrity fallback

The clean user sculpt and historical live diagnosis show that the vanilla
conform is closer to the intended lower robe than the current solve:

- indexed sculpt error, `z=35..70`: vanilla `1.539`, safe `1.834`;
- indexed sculpt error, `z=50..65`: vanilla `1.847`, safe `2.156`;
- the old project log records the user's vanilla Conform All as visually clean.

For the first live anti-clipping gate, the feature therefore includes a hard
degradation guard. It compares the final robe to the already-computed vanilla
pose and falls back byte-exactly to vanilla when BOTH are true:

```text
flippedFaces > 20
collapsedFaces(area < 10% vanilla) > 2
```

The compound gate is intentional. Corpus audit found healthy loose garments
with many rotations but no collapsed faces (Falmer Cloth `38/0`) and garments
with a couple of small faces but few rotations (Yarl Cloak `13/2`). The Vampire
is the only current capture with both signals (`24/5` after safe floor).

The release ON replay is byte-identical to the harness vanilla OBJ. This is not
a claim that vanilla is a general solver; it is the degradation ladder doing its
job for one known case where Drape cannot safely beat the host result yet.

## Authored layer-order infrastructure (implemented, not live-enabled)

The user's radial-order idea is sound after one change: use signed distance to
the local body surface instead of a ray from one global cylinder center. Global
rays are ambiguous at sleeves, axillae, separated legs and non-star-shaped
skirts.

The experimental `DRAPE_LAYER_ORDER_PRESERVE` path now:

- reconstructs inverse authored relations in harness sequence replay;
- admits an explicitly covered tiny AUTO garment as INNER collision geometry,
  while excluding body-part names and never admitting AUTO as OUTER;
- lets an outermost fitted/loose cloth layer make bounded room for that inner;
- caps total movement to `0.10u` per application;
- reapplies support, exact-seam and proximity caps every round.

On the Vampire/Filler capture it improves some structural counters but does not
visibly repair the lower panels, because those panels belong to the same merged
robe shape and the dominant remaining error is authored rest-shape/fullness.
Consequently no `layer_order_preserve_on.flag` is shipped. Keep the code gated
for the next coupled/multi-shape pilot.

An offline material-layer spike repaired 145 lower-panel vertices
deterministically (`tmp/vampire_clipping_diag/lower_repair/layer_direct75.obj`,
SHA-256 `9B09DC102948E843EFBC1E0CE987E1B0DCFB25B7FFCD7F28111EC6613434544F`).
It reduced crossings and flips, but the render did not yet prove removal of the
large visible panels. It is deliberately not part of this release.

## Rejected experiments this session

- Global triangle-sample push-out: triggered the integrity guard (`30/3`) and
  worsened body penetration to 39/worst `-5.42`; removed from production source.
- Re-enabling the old loose-volume expansion under the merged fitted route:
  produced `72` flips and `12` collapsed faces; removed.
- Restoring the pre-merge Loose solver: `243` flips, `45` collapsed faces and
  `80` body penetrations; remains rejected.
- Treating all self-intersections or all body-inside samples as invalid: the
  clean sculpt intentionally contains 380 crossings and many hidden/interior
  samples, so those scalar objectives are not valid loss functions.

## Flags and rollback

Live A/B enable:

`E:/SkyrimDev/Drape/anticlip_safe_floor_on.flag`

Hard rollback:

1. remove `anticlip_safe_floor_on.flag`;
2. optionally create `anticlip_safe_floor_off.flag` (OFF wins);
3. rerun Smart Conform from the vanilla/base slider state.

Do not create `layer_order_preserve_on.flag` for this visual gate.

## Validation artifacts

- `tmp/vampire_clipping_diag/safe_floor_comparison.png`
- `tmp/vampire_clipping_diag/safe_floor_backface_comparison.png`
- `tmp/vampire_clipping_diag/safe_floor_stats.txt`
- `tmp/vampire_clipping_diag/sculpt_geometry_compare.txt`
- `tmp/vampire_clipping_diag/body_sample_penetration.txt`
- `tmp/face_guard_corpus/REPORT.md`
- `tmp/vampire_clipping_diag/release_off.obj`
- `tmp/vampire_clipping_diag/release_on.obj`
- `tmp/vampire_clipping_diag/release_vanilla.obj`

## Deployment status

Final `RelWithDebInfo` DLL was built and copied to both configured Outfit Studio
mod folders while Outfit Studio was closed. Build and deployed copies all have
SHA-256:

`EE6019BDD866CB47D323D6D5E2EC96CC77436EA83772357BAB6DF71C54F162E9`

Active live flags at deployment are `anticlip_safe_floor_on.flag` and
`posterior_chord_on.flag`. With anti-clipping enabled, two independent Vampire
replays and the explicit vanilla replay are byte-identical, all with OBJ
SHA-256:

`EFACA1B6F1F2FF2CBED2D0A0A73628CF4087F3A546217806B89D8C4586B54A88`

Final verification passed:

- harness self-test;
- all 18 golden captures with the feature off;
- feature isolation: only `vampire_robes_alt` changes with safe floor on;
- combined safe-floor/posterior isolation: only the expected Vampire capture
  changes in the old corpus;
- live BlackSmith `142410` replay retains the approved 26-vertex posterior
  bridge result and belt metrics.

The user must still perform the live textured Outfit Studio gate. Offline
geometry proves deterministic safety and exact fallback, not final visual
acceptance.
