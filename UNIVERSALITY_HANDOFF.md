# Smart Conform universality handoff

Updated: 2026-07-12

## Objective

Expand Smart Conform beyond the current small 3BA/RM_Heavy regression corpus,
prioritizing visually plausible free-hanging cloth while preserving authored
topology, seams, rigid pieces, layer order and the already approved Blacksmith
and Vampire behavior.  The first modded stress project is
`RLY_TEST_BDO Lemoria Armor`; vanilla projects follow after its baseline is
captured.

## Frozen starting point

- Branch: `wip/codex-anticlip-harness`
- Starting commit for this pass: `04e9a53`
- Approved live anti-clipping/body-floor implementation: `71e3c5b`
- Active release flags: `anticlip_safe_floor_on.flag` and
  `posterior_chord_on.flag`
- The only unrelated tracked worktree change is the user's newline change in
  `corpus/golden/fur_blacksmith_rm_heavy.json`.  Do not stage or overwrite it.

## Coverage audit

The committed corpus is a regression gate, not a universality claim.  It has
18 shapes from five female 3BA outfits and only exercises `RM_Heavy`.  It has no
committed modded armor, male body, bandolier, skin-tight garment, flexible
armor, non-manifold stress case or rigid accessory.  Most loose captures also
come from two Yarl projects, so raw capture count greatly overstates coverage.

Detailed inventories are ignored working artifacts:

- `tmp/universality/UNIVERSALITY_MATRIX.md`
- `tmp/universality/LEMORIA_INVENTORY.md`
- `tmp/universality/lemoria_snapshot/`

Recommended next projects after Lemoria are Wayward Satchel/Armor, Ebony Mail
and Archmage robes.  Belts and transverse straps have a separate design backlog
in `BELT_AND_PIPELINE_REVIEW.md`, Part 24.

## Lemoria baseline diagnosis

`RLY_TEST_BDO Lemoria Armor` is structurally valid: 12 shapes, 173 sliders and
951 OSD datasets.  Its NIF and OSD live under the MO2 overwrite BodySlide tree.
The actual reference shape is `3BBB`, but the previous Drape run used the first
SliderSet shape, `00BDOR0008Armor1`, as the body.  That plate has only 708
vertices while the `3BBB` RM_Heavy diff addresses a 13,651-vertex body.  Drape
then classified the real body as a loose garment and built invalid body,
layering and cloth constraints.  That old visual/output baseline must not be
used to tune solver parameters.

The race occurs because Outfit Studio can invoke the hook before `Log_OS.txt`
has flushed its `Loading reference shape '3BBB'` line.  The OSP itself contains
the correct `3BBB` shape, but it is the last shape rather than the first.

## Production corrections in the current worktree

### Robust project reference resolution

`algo/conform_hook.cpp` now scans the entire matching SliderSet and recognizes
well-known body reference names.  Resolution precedence is:

1. reference reported by Outfit Studio's own load log;
2. recognized body name in the matching SliderSet;
3. legacy first-shape fallback.

The selected source, first shape and recognized body are logged.  A new harness
command provides headless verification:

```text
drape-harness --resolve-project <name> --bodyslide-dir <path>
```

The live hook also fails closed to vanilla conform when a source reference diff
contains a vertex index outside the cached reference mesh.  This protects
unknown naming schemes and stale projects from the same poisoned partial solve.

### `arm` versus `armor` classifier bug

Both the core classifier and Smart Conform dialog previously searched for the
substring `arm`, which is present in every English `armor` name.  Generic
`Armor1/2/3` and even shoulder plates were consequently routed to Tubular.  The
core and UI now put shoulder/pauldron/plate first and recognize arm/leg only as
explicit tokens or suffixes.  Ambiguous generic armor remains Auto so geometry
or the user can classify it.

## Validation completed

- `drape_harness` RelWithDebInfo build succeeds.
- Harness self-test includes the armor/arm classifier cases and passes.
- Headless resolution selects `3BBB` for both RLY Lemoria and Blacksmith.
- All 18 committed corpus goldens pass unchanged.
- `drape_algo` and `drape_proxy` RelWithDebInfo builds succeed.
- Current builds were deployed to both configured MO2 mod folders with Outfit
  Studio closed.  Build output and both deployed copies are byte-identical:
  - `drape-algo.dll` SHA-256
    `A9927BEA9C5C686B001C7E7D90925595D3B681D4BD29BB56FB397779EFAC7DE0`
  - `version.dll` SHA-256
    `FAC922875A0AB9899E5CE8C04B9298E8A4F904E4F91FF57148E396848A5FF9C7`

## Capture state and immediate continuation

There was no existing valid Lemoria `.drcap`.  A temporary ignored capture gate
exists at `captures/enable_capture.flag`, and the intended capture directory is
`captures/universality/lemoria_rm_heavy`.  Remove the gate immediately after the
live capture so normal user sessions do not create captures.

The first attempt to start Outfit Studio with `ModOrganizer.exe -p ... -r ...`
opened another MO2 process instead of the configured executable.  Do not tune
the solver from the old Lemoria run.  The safe continuation is:

1. launch the registered `OutfitStudio x64` through the active portable MO2
   instance/profile so the VFS sees the RLY overwrite project;
2. ensure `DRAPE_CAPTURE_DIR` points to the dedicated Lemoria directory in the
   launched process;
3. open `RLY_TEST_BDO Lemoria Armor`, run Smart Conform with only `RM_Heavy`,
   and do not save the project;
4. verify the debug log says reference `3BBB`, not `Armor1`;
5. expect captures for garment shapes only; replay twice, compare hashes and
   generate topology/penetration/stretch summaries and renders;
6. remove `captures/enable_capture.flag` before any normal launch.

Only after this corrected baseline should a general cloth-realism change be
promoted.  Candidate changes must improve at least one new archetype and remain
dark or unchanged on the 18-shape corpus plus the authoritative Blacksmith and
Vampire replays.

## Commit discipline

Commit the resolver/classifier checkpoint separately from later solver tuning.
Never include `corpus/golden/fur_blacksmith_rm_heavy.json`.  Push each validated
checkpoint to `origin/wip/codex-anticlip-harness` and keep this file updated.
