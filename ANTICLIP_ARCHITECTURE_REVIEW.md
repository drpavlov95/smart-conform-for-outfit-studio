# Anti-Clipping — Architectural Redesign

Date: 2026-06-08
Status: IMPLEMENTATION IN PROGRESS.

## 0. Why everything tried so far failed

A full day of attempts. Every one was a **post-solve correction** that moved the
garment's own vertices after the cloth solve. They all failed for the same root reason,
which is worth stating plainly because it kills a whole class of approaches:

> **You cannot fix inter-garment clipping by moving the thin garment's vertices.**

Three independent reasons, each fatal on its own (all observed with instrumentation):

1. **Moving thin cloth tears it.** Neighbouring verts move by different amounts →
   the edge between them over-stretches → rip. Measured: cloak edges hit 15× rest.
   This is geometric, not a tuning bug. Soft steps + stretch relax only delay it.

2. **The "correct" position is often geometrically impossible.** At the shoulders the
   outer garment (fur cloak) sits *on or inside* the body. "Under the cloak AND outside
   the body" has no solution there. Measured: `bodyConflict = 22-46 of ~50`. The rule
   correctly refuses (moving the dress in = body clip), so it does nothing.

3. **Passes fight each other.** seam-close → tuck → cap → collapse each undo the
   previous one's work. Measured: `bodyConflict` stays constant across all of them.

What DID work today were fixes at the SOURCE, not post-solve:
- Hip flap mis-classified as rigid → fixed the *classification* → tearing gone.
- Body-part meshes (vagina/anus) → skipped at the *entry*, kept vanilla.

**Lesson: fix causes at the source; do not correct symptoms after the solve.**

## 1. Reframe the problem

Inter-garment clipping is really TWO different problems that we kept conflating:

### Problem A — "Layer order is wrong" (FIXABLE by position)
Two garments swap their depth order somewhere they BOTH have room off the body
(e.g. dress drapes slightly outside the pants at the hip, but design has dress inside).
Here there IS a valid position, so a gentle correction is legitimate. This is rare and
mild; it is NOT the hard case.

### Problem B — "Inner garment shows through an outer one with no room" (NOT fixable by position)
The dress is behind the fur cloak, the cloak hugs the body, there is no space. The dress
is *supposed to be invisible there* (the cloak covers it in the source mesh). The only
correct action is to make it NOT RENDER there — exactly what an outfit author does by
hand. Moving it cannot work (Problem 2 above).

**These need different mechanisms. Conflating them is why nothing converged.**

## 2. The redesign

### 2.1 One ordering decision, computed ONCE, used everywhere
Stop recomputing "who is outer" in five places with three metrics. Compute, per garment
PAIR, a per-region depth relationship ONCE from the base mesh (which the user's layer
intuition is about), store it, and have every consumer read it. No per-pass re-derivation.

Metric: distance-to-body-surface (signed), NOT the thin sheet's own normal (which flips).
This was the one metric that survived scrutiny.

### 2.2 Problem A: a SINGLE gentle layer-order pass, integrated INTO the solve
Not a post-pass. Add inter-garment order as a *constraint inside the cloth relaxation
loop*, alongside stretch/bending/body-collision. The solver already balances competing
constraints every iteration without tearing (that is what a PBD solver is FOR). An order
constraint that says "this vert's body-gap ≤ that garment's body-gap − margin, where
there is room" relaxes together with stretch, so it never over-stretches an edge.
- Runs only where there IS room (Problem A). Where there is no room, it does nothing and
  hands off to 2.3.
- One pass, inside the loop, no fighting downstream passes (there are none after it for
  this purpose).

### 2.3 Problem B: render-suppression by COLLAPSE, done once, at the very end
For verts that are covered AND have no room (Problem B), hide them. The correct hide is
NOT moving them through the body (tears) — it is collapsing each covered triangle to zero
area so it disappears. Key rules to avoid artifacts:
- Operate on a CONNECTED covered region with an ERODED boundary (so the visible edge of
  the covered patch is never half-collapsed).
- Collapse the whole region's verts to a SINGLE shared interior point (all coincident →
  no edge between them can stretch → no rip), placed just under the body so the thin
  boundary triangles are hidden by the outer garment.
- This is deterministic, runs once, nothing after it.

### 2.4 Delete the redundant passes
Remove tuck, the disabled suppress, the forced-cleanup, the per-pass colliders. They are
the source of the fighting. Keep only: (a) the in-solve order constraint, (b) the final
collapse, (c) the existing seam-close (which solves a DIFFERENT problem — intra-garment
island seams — and works).

## 3. Honest scope & expectation

- This is a real refactor: ~1 new in-solve constraint + 1 clean collapse pass, and
  DELETION of ~4 tangled passes. Net code should SHRINK.
- The fur-cloak-over-dress + extreme-morph case may still not be perfect — it is a genuine
  worst case (gappy mesh, slim garment on heavy body, layers competing for one space).
  The goal is: no tears, no gross pokethrough, on NORMAL outfits; and graceful (hidden,
  not torn) degradation on the hard case.
- Validate on 2-3 NORMAL outfits, not only this pathological one.

## 4. Build order (APPROVED with conditions)

Reviewer conditions, accepted:
- **Order constraint** goes inside the PBD/XPBD loop (alongside stretch/bending/collision).
- **Collapse** must be per-COVERED-REGION (connected component), NOT one global point:
  one interior point PER island; interior point behind the outer garment; keep/soften a
  one-ring transition near any boundary that is close to a VISIBLE area; ensure degenerate
  tris don't create export/render artifacts (normals). Only collapse where the boundary is
  genuinely covered. If a region is 100% invisible, degenerating it is fine; if its border
  is visible, do NOT collapse there.
- **Rip-out = DISABLE BY FLAG first**, validate a clean comparable base, and only DELETE
  the dead code once the new architecture is proven. Don't lose the clean comparison.

Steps:
1. ✅ DONE & VALIDATED (2026-06-08): disabled the whole legacy anti-clip block via
   `kAntiClipLegacyEnabled=false`. User confirmed NO tearing on Yarl Clothes 1 & 2.
   Clean stable base established. (Clipping present — expected with no anti-clip.)
2. ⚠️ DISABLED FOR STABLE BASELINE (2026-06-08): the in-solve order constraint
   compiled and did not cause the catastrophic collapse, but it did not solve the
   visible no-room clipping case. It now defaults to `orderConstraintStrength=0.0`
   until it proves value on normal outfits.
3. ❌ REJECTED (2026-06-08): final covered-region collapse was implemented and
   deployed, but broke the mesh visually (large fan/black-hole artifacts). It is
   now disabled behind `kCoverCollapseEnabled=false`. Conclusion: collapsing render
   geometry through morph diffs is not a production-safe hiding strategy.
4. ❌ REJECTED (2026-06-08): soft covered-diff mask behind
   `kCoveredDiffMaskEnabled` was tested and produced a bad/broken result. It is
   now disabled. Conclusion: attenuating covered regions back toward base through
   `GenerateResultDiff` is still not a production-safe hiding strategy.
5. 🧪 CURRENT DIAGNOSTIC BUILD (2026-06-08): `kCoveredClipDiagnosticEnabled=true`
   runs the covered-region detector without changing any vertices. It writes
   `drape_cover_candidates__*.csv` files to `%TEMP%` and logs `cover-diag` lines
   with covered/core/component counts. Use this to inspect true zap candidates
   before attempting another correction.
6. ✅ CURRENT PRODUCTION BASELINE: no legacy anti-clip, no cover-collapse, no active
   order constraint. Keep the validated tear-free cloth/body/seam pipeline.
7. NEXT ARCHITECTURE: if the diagnostic confirms stable covered regions, solve persistent
   no-room clipping as a true occlusion/zap/mask workflow outside `GenerateResultDiff`,
   not as another vertex movement pass.
