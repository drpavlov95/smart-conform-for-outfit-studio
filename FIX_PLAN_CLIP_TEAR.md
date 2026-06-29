# Fix Plan — remaining clipping + lateral tear (Yarl Clothes 2, RM_Heavy)

Date: 2026-06-28
Status: PLAN for a lower-cost model to execute. Read SENIOR_REVIEW_AND_PLAN.md's
"Implementer's Field Guide" first — its meta-rule, forbidden moves, and discipline all apply.

## Context
- Build under test: the Jun-28 23:10 `drape-algo.dll` (Codex's latest, already deployed to
  both mod folders). ~4500 uncommitted lines on top of commit `bb172de`.
- The HARNESS works: `build/algo/RelWithDebInfo/drape-harness.exe`
  - `--replay-dir ./tmp/rm_heavy_latest` runs all 4 Yarl-2 meshes and prints metrics.
  - `--replay <cap.drcap> --debug-ply out.ply --result-obj out.obj` for one mesh.
- Active anti-clip = `ApplyLayerOrderPass` (conform_hook.cpp:3161, called 5174). The other
  mechanisms are OFF: `orderConstraintStrength=0.0` (2249), `kCoverCollapseEnabled=false`
  (5846), `kEnableAntiClipTuck=false` (6426).

## Diagnosis (from the harness on the CURRENT build)
Metrics for the 4 meshes (RM_Heavy):

| mesh | preset | worstEdgeStretch | garmentFaceResidual (clip) | worst face pen |
|---|---|---|---|---|
| Belt | Belt | 3.51 | 0 | 0 — clean |
| Cloak | Loose Draped | 2.51 | 0 | 0 — clean (outermost) |
| Clothes (dress) | Loose Draped | **5.34** | 51 (Cloak:43, Belt:8) | 1.26 |
| Pants | Tubular | 3.77 | **172** (Clothes:170) | **2.86** |

### Issue 1 — the lateral tear = Clothes worstEdgeStretch 5.34×
Root cause FOUND (not a guess): the post-finishing stretch cap (conform_hook.cpp:5347)
caps each edge relative to `supportRestLen` (the edge length in the *already-conformed*
support mesh), but the metric measures against `e.restLength` (the *original* garment edge).
`worstSupportEdgeStretch=1.08` + `worstEdgeStretch=5.34` ⟹ the conformed support edge is
already ~4.9× the original. **Native Conform All itself over-stretched that one lateral edge
fitting the heavy hips; our cap preserves it (caps vs support, not original).** That is why
"99% of tearing is gone but one lateral edge remains" — it is inherited from vanilla.

### Issue 2 — remaining clipping = garment-vs-garment face residual
- Pants poke through Clothes: 170 faces, worst 2.86u — the biggest clip (2.86u is large; the
  pants are NOT being pulled under the dress in that region, or the dress inflated away).
- Clothes poke through Cloak: 43 faces, worst 1.26u — the dress spiking through the fur.
The in-solve order constraint is still 0 and `ApplyLayerOrderPass` alone is not resolving it.

## STEP 0 — MANDATORY before any fix
1. **Commit Codex's work first.** ~4500 lines are uncommitted; create a rollback point:
   `git add -A && git commit` (so any change below is revertible).
2. **Trust-the-harness re-validation.** The `.drcap` captures in `tmp/rm_heavy_latest` are
   from Jun 21; the replay shows the same/worse numbers than the user sees live (user reports
   99% tearing gone). RESOLVE this discrepancy before optimizing, or you optimize blind:
   - Do ONE live conform in OutfitStudio with capture enabled (set the capture flag /
     `DRAPE_CAPTURE` path the code uses — see the capture hook near conform_hook.cpp:6143) to
     write FRESH Jun-28 captures.
   - Re-run `--replay-dir` on the fresh captures.
   - If fresh numbers ≈ these (worstEdgeStretch~5.34, big residual) → harness is faithful,
     proceed. If much lower → old captures were stale; use only fresh captures from now.
   - **Gate:** harness metrics must match what is on screen.

## STEP 1 — Localize (do NOT guess from pixels)
- `tmp/diag_clothes.ply` and `tmp/diag_pants.ply` are already generated. Open in a mesh
  viewer; red = high stretch / penetration. (Regenerate from fresh captures after Step 0.)
- Confirm: the red high-stretch vert on Clothes is the lateral tear the user sees; the red
  Pants region is the pants-through-dress clip.
- KEY decision this localization answers for Issue 1: is the 5.34 edge ONE isolated spike
  vert (vanilla mis-projection → an outlier to clamp) OR a broad side region (legitimate
  fit-stretch where the body genuinely grew → NOT really a tear, metric is misleading)?
  The fix differs; don't proceed to Step 2 until you know which.

## STEP 2 — Fix the lateral tear
Bisect with the harness first: log worstEdgeStretch (vs `e.restLength`) AFTER the solve,
AFTER `ApplyLayerOrderPass`, and AFTER the post-cap, to confirm where 5.34 survives.
Then, depending on Step 1:
- If ISOLATED spike (vanilla outlier): clamp that vert's vanilla diff before the solve (cap
  its displacement relative to its 1-ring neighbours), so the support edge never starts at
  ~5×. This fixes the cause (bad input), not the symptom.
- If BROAD legit fit-stretch: it is not a tear — but if it still reads as one, add a SECOND
  cap term in the existing post-cap loop (5347) measured against `e.restLength` with a looser
  ratio (e.g. ≤ ~1.6×), so genuinely-needed fit-stretch is allowed but a 5× rip is not.
  (No NEW standalone pass — extend the existing cap; respect "no new number without deleting
  one" by folding both ratios into one capped loop.)
- Forbidden: a new smoothing/relax pass to hide it (meta-rule: fix the cause).
- **Gate:** Clothes worstEdgeStretch ≤ ~1.6 on the harness AND the lateral tear gone in OS.
  Re-run `--replay-dir`; no other mesh's metrics may regress.

## STEP 3 — Fix the clipping (tear-safe levers ONLY)
Start with the biggest: Pants→Clothes (170 faces, 2.86u).
- First, from the PLY: is the dress inflating AWAY from the body (leaving pants exposed), or
  are the pants riding out? If the dress over-inflates, fixing its body-clearance is the real
  cause (cheaper than anti-clip).
- Tear-safe anti-clip lever: raise `orderConstraintStrength` above 0 (2249) so the in-solve
  order constraint pulls the inner garment in WHERE THERE IS ROOM (it relaxes inside the loop
  with stretch → cannot over-stretch an edge). Tune ONLY via the harness: garmentFaceResidual
  must drop while worstEdgeStretch stays ≤ ~1.6.
- For genuine no-room regions (dress behind the hugging fur cloak) the correct action is HIDE
  (covered-region collapse), but that is the larger Phase-3 work in SENIOR_REVIEW_AND_PLAN.md
  — defer unless the order constraint alone leaves gross pokethrough.
- Forbidden: moving thin cloth post-solve along the body normal (proven to tear).
- **Gate:** Pants→Clothes and Clothes→Cloak face residual drop substantially; worstEdgeStretch
  stays ≤ ~1.6; no new body penetrations.

## Working discipline (every change)
- Run `--replay-dir` after EVERY edit. A metric must improve and NONE may regress.
- One change at a time. If a change helps nothing, revert it (don't stack).
- No new tuning constant without deleting one.
- The harness metric is the truth, not the eyeball — but Step 0 must first prove the harness
  matches the eyeball.
