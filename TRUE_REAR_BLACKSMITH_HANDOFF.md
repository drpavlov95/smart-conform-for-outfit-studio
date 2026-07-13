# BlackSmithTorso true-rear bridge handoff

Date: 2026-07-12

## Corrected root cause

The project previously interpreted the Outfit Studio Y axis backwards. In the
live Skyrim meshes `+Y` is the front and `-Y` is the rear. The former
`DRAPE_TANGENTIAL_REPAIR` genuinely ran, but repaired a collapsed fan on the
front panel and deliberately preserved the true rear entry surface. The user's
`latestblacksmithtorsopostsmartconform.obj` changed 157 front-region vertices
and exactly zero of the 86 true rear-butt vertices. Its live flag was removed.

The user's original diagnosis was correct: this is a low-frequency surface tuck
into the glute cleft/under-butt, not bad sampling. At horizontal section `z=66`:

| mesh | central posterior depth | cheek mean | cleft depth |
|---|---:|---:|---:|
| bad/latest | 15.20 | 18.47 | 3.27 |
| paired hand edit | 17.61 | 18.21 | 0.59 |
| historical sculpt | 19.10 | 19.63 | 0.53 |

The paired target is:

`tmp/BlackSmithTorso_hand-edit of the smart-conformed RM_Heavy slider.obj`

The historical sculpt has the same bridge concept but sits roughly 1-2 units
farther posterior over much more of the garment; it is not the paired target.

## Current algorithm: dynamic posterior feather

`ApplyPosteriorChordBridge` is implemented in `algo/conform_hook.cpp` behind
`DRAPE_POSTERIOR_CHORD`, compiled default OFF. It runs after the true-final body
floor and before the superseded tangential/tension experiments. The live flag
enables it only for exact shape name `BlackSmithTorso` and preset `Fitted Cloth`.

The first working version used fixed anchors at `x=-8,+8` and moved 19 vertices.
The current version follows the actual cloth lobes at every exact garment Z row:

```text
search left/right cloth minY independently at |x|=1.5..14, step 0.25
u       = (x - xLeft) / (xRight - xLeft)
q       = clamp(4*u*(1-u), 0, 1)
zWeight = smoothstep(z=57, z=64)
targetY = lerp(clothLeftY, clothRightY, u) + 0.90*zWeight*q^2
newY    = min(entryY, targetY)
```

Z is stored as a fraction of the reference-body span in production; `57/64`
map to `0.44135/0.50826` for the validated capture. Dynamic samples are cached
by exact Z float bits. The pass moves only toward `-Y`, never pulls cloth into
the body, and leaves X/Z unchanged. Activation still requires both a body cleft
and a cloth cleft.

Main defaults:

```text
DRAPE_POSTERIOR_CHORD_ANCHOR_MIN_X=1.50
DRAPE_POSTERIOR_CHORD_ANCHOR_MAX_X=14.0
DRAPE_POSTERIOR_CHORD_ANCHOR_STEP=0.25
DRAPE_POSTERIOR_CHORD_SAG=0.90
DRAPE_POSTERIOR_CHORD_SAG_Z_START=0.44135
DRAPE_POSTERIOR_CHORD_SAG_Z_END=0.50826
DRAPE_POSTERIOR_CHORD_X_POWER=2.0
DRAPE_POSTERIOR_CHORD_BODY_DIP=1.20
DRAPE_POSTERIOR_CHORD_CLOTH_DIP=0.15
DRAPE_POSTERIOR_CHORD_DEPTH=8.0
DRAPE_POSTERIOR_CHORD_Z_LO=0.42
DRAPE_POSTERIOR_CHORD_Z_HI=0.6038
DRAPE_POSTERIOR_CHORD_MAX_MOVE=3.30
DRAPE_POSTERIOR_CHORD_MAX_ROI=64
DRAPE_POSTERIOR_CHORD_MIN_AREA=0.65
DRAPE_POSTERIOR_CHORD_MIN_EDGE=0.65
DRAPE_POSTERIOR_CHORD_MIN_GAP=0.06
DRAPE_POSTERIOR_CHORD_COVER=0.75
```

`Z_HI=0.6038` maps the live body span to about `z=74`. Expanding it to `0.62`
admits the seam pair 2212/2214 under BlackSmithBelt and worsens the worst layer
penetration from `0.8625` to `1.7558`; do not raise it without a coupled
Torso+Belt treatment.

## Safety and isolation

- exact shape allowlist: `BlackSmithTorso`;
- original preset must be `Fitted Cloth`;
- pinned/rigid and near-covered outer-layer vertices are protected;
- at most 64 post-detector vertices and 256 broad candidates;
- anchor sampling is finite-checked and capped at 256 samples per side;
- exact seams use the most-posterior proposal, preserving one-sided motion;
- maximum per-vertex travel is 3.30 units;
- proposed body gap must be at least 0.06 and may not decrease from entry;
- affected faces must retain area and normalized orientation;
- normal authored edges retain the 0.65 compression floor and upper bound;
- edges already entering below 0.65 may lose at most another 1.5%;
- any violation reverts the complete pass;
- downstream garments collide with the unchanged entry proxy;
- non-finite tuning values fail closed.

The 1.5% grandfather tolerance is local to already-compressed affected edges.
The accepted live candidate needs at most 1.134%; globally lowering the edge
floor to 0.50 was explicitly rejected.

## Final harness result

On the live-body `142410` capture, using the exact release source:

- 32 detector decisions and exactly 26 moved vertices;
- Y-only movement, maximum `2.8952`;
- minimum moved body gap `0.2376`;
- zero penetrations, orientation flips, non-Y movement, or seam spread;
- minimum affected face area ratio `0.7231`;
- minimum affected face normal dot `0.7587`;
- paired-hand Y error within `z<=74`, broad: `0.195933 -> 0.118736`;
- paired-hand Y error within `z<=74`, core: `0.190137 -> 0.067194`;
- C++ versus offline candidate maximum coordinate delta `0.0000534`;
- Torso/Belt counts and worst residuals remain exactly at baseline:
  `83 / 369 / 0.862527 / 0.931093`;
- repeated ON OBJ SHA-256:
  `0D73FA09442BA747ADDF31B1B76473C0378F3816022EF979CE75E8849FE995E6`.

Default OFF still matches all 18 golden corpus shapes. With the feature ON,
none of the 17 non-target captures changes. The older corpus BlackSmithTorso
capture also remains byte-exact because its different protection/topology state
would push previously normal edges below 0.65, so the complete proposal fails
closed. The newer live `142410` capture is the authoritative target replay and
does change deterministically as listed above.

## Live A/B, deployment, and rollback

Live enable:

`E:/SkyrimDev/Drape/posterior_chord_on.flag`

Current state: `posterior_chord_on.flag` and the independently gated
`anticlip_safe_floor_on.flag` are the only `*on.flag`/`*off.flag` files present.
OutfitStudio was closed during deployment. The final `RelWithDebInfo` DLL was
copied to both configured Outfit Studio mod folders; build and both deployed
copies have SHA-256:

`EE6019BDD866CB47D323D6D5E2EC96CC77436EA83772357BAB6DF71C54F162E9`

Guaranteed rollback: remove the on flag, create `posterior_chord_off.flag`, then
rerun Smart Conform from the vanilla/base mesh. The off flag wins over both the
on flag and an inherited environment variable.

Do not enable these superseded experiments for the visual gate:

- `tangential_repair_on.flag` - front-panel sampling repair;
- `cleft_true_rear_on.flag` - broader offset/plumb experiment;
- `tension_plumb_on.flag` - older weak radial propagation.

The user approved the previous fixed-anchor version as an improvement, but has
not yet visually checked this dynamic-feather release. Offline metrics and OBJ
replay prove that the correct `-Y` surface moves; they are not a substitute for
the final live Outfit Studio gate.

## Diagnostics

- `tmp/posterior_feather_spike/REPORT.md`
- `tmp/posterior_feather_spike/metrics.json`
- `tmp/posterior_feather_spike/horizontal_profiles.png`
- `tmp/posterior_feather_spike/sagittal_profiles.png`
- `tmp/cpp_posterior_feather_release_off.obj`
- `tmp/cpp_posterior_feather_release_on.obj`
- `tmp/posterior_residual/REPORT.md`
- `tmp/underbutt_spike/REPORT.md`
- `tmp/diagnostics/15_latest_true_rear_horizontal_profiles.png`
- `tmp/diagnostics/16_latest_true_rear_sagittal_profiles.png`
- `tmp/diagnostics/17_latest_true_rear_depth_maps.png`

The separate under-butt spike was not integrated: most remaining lower residual
is tangential reparameterization, while the dynamic feather already improves
the material lower dent with one unified pass.
