# Belt Issue + Whole-Pipeline Review — Findings & Fix Plan

Date: 2026-07-01 (Parts 6–8 added same day: acquisition-review / premium-product plan)
Status: **PLANNING ONLY** (per user: implementation in a later Opus/Sonnet session).
Companion to `SENIOR_REVIEW_AND_PLAN.md` (the phased master plan — this doc feeds it).
Parts 1–5: the belt issue + pipeline review. Parts 6–8: "if a AAA team bought this
project today" — what we'd have done differently, and the roadmap to ship premium.

---

## Part 1 — What this session PROVED about the belt (all harness-measured)

1. **The belt is AUTHORED loose.** Base-pose clearance to the body: median ~2.1u
   (range 0.7–3.2). Vanilla's solved gap (1.95) ≈ the authored clearance (2.10).
   The "float" is mostly the *design*, not conform error. Any fix that treats
   clearance as error will fight the mesh.
2. **The trade-off wall** (why every quick fix failed):

   | Approach | Magnetism | Float | Distortion | Dress clip |
   |---|---|---|---|---|
   | Seat to uniform body-gap | YES (the complaint) | fixed | low (3.51) | mild (43→49-60) |
   | Faithful skin-bind | none | stays (by design) | +stretch 4.07 | none |
   | Skin-bind + inset/scale toward body | none | fixed | 3.9–4.1 | **SEVERE 43→114-227** |
   | Drape solver (verletCloth) | none | stays (no on-body anchor) | best (2.82) | none |

   De-floating toward the **body** always clips the **dress** because the dress
   (gap ~0.5) sits *between* the body and the belt. The belt's correct support
   surface is the dress, not the body.
3. **Magnetism root cause:** every pass that drives verts toward a *uniform
   absolute gap* (seat, bodyColumn, clearance, envelope) makes cloth follow the
   body into concavities. Real cloth: contact pushes OUT; only its own tension
   pulls IN; hollows get BRIDGED. (User articulated this; measurements agree:
   119/186 wrap verts were pulled inward vs vanilla by the seat stack.)
4. **Strap/appendage insight (user's "flap detection" idea works):** connected
   components + vertical aspect ratio cleanly separates wrap (aspect 0.27) from
   hanging strap (aspect 6.79). The strap is already vertical (3.7° off) — it only
   needed a shape-preserving translation, not per-vertex shaping. Per-vertex
   uniform-gap passes twisted it (up to 4.3u distortion).
5. **The radial drape envelope is skirt-shaped.** Applied to the belt it pushed
   the thin strap OUTWARD (3.46→4.52). It assumes "tube around the body axis";
   belts/straps/pauldrons violate that. Envelope must become island-aware.
6. **ARAP is unblocked.** Eigen cloned to `reference/eigen` (MPL-2.0, header-only);
   libigl ARAP compiles+links in MSVC via the isolated module `algo/skin_arap.{h,cpp}`
   (`SolveArap(verts, tris, handles, targets, iters)` — deterministic, fixed iters).
   The plan's "integration risk peak" is CLEARED.
7. **Body topology transfers.** Base and morphed body share identical triangulation
   (26412 tris) → barycentric anchors transfer exactly. Skin-bind is viable.
   Caveat: offsetting along the *face normal* is approximate at edges/corners
   (added stretch 3.51→4.07). A correct reconstruction needs per-anchor local
   frames (or ARAP absorbing the error — see fix plan).
8. **Rest-on infra half-exists.** `innerColliders` + `applyOuterAccessoryInnerClearance`
   already push BELT-preset garments OUT of inner garments (the correct one-way
   direction). BUT: it's only a violation-resolver (prevents clipping into the
   dress), never a support to settle ONTO — and because the belt solves before the
   dress, the inner collider is the **vanilla-conformed** dress, not the draped solve
   (conform_hook.cpp ~9570-9606: falls back to `HostGetRawResultDiff`).

## Part 2 — Systemic issues in the whole cloth pipeline (the review)

**A. Two competing sources of truth for position.** The anchor field
(`supportVerts` = vanilla conform, `anchorWeight`, influence blend-back) vs the
physics/finishing passes. They fight: this session alone, the influence blend
undid the seat, the envelope undid early smoothing, and the de-wrinkle re-floated
the seat. Every "fix" was "run later than the thing that undoes you" — fragile
ordering, not architecture.

**B. Body-gap is treated as THE coordinate, but garments stack.** Almost every
shaping pass measures/targets clearance-to-BODY. The belt's true support is the
dress; the pants' is the dress; the dress's (locally) is the belt pressing on it.
Only the tuck passes are layer-aware (outerColliders). Shaping passes being
body-only is why de-floating the belt clipped the dress.

**C. Missing layering relation: REST-ON vs TUCK-UNDER.** Today there is one
mechanism: inner tucks under outer (needs outer solved first — Cloak→Clothes→Pants
works this way). The belt needs the dual: a *hard* outer piece RESTS ON a *soft*
inner piece (needs inner solved first). Generalizes to pauldrons-on-cloak,
holsters, scarves, medallions-on-chest.

**D. Solve order is host order, not layer order.** OS calls shapes in list order;
the DLL chains whatever is already solved. Rest-on requires inner→outer for the
resting pair, or a deferred re-solve of the rester once its support is solved.

**E. ~20 sequential finishing passes with implicit dependencies.** Verlet solve →
smooth → envelope → layer order → edge constraints → belt finishing → influence
blend → idempotency → safety clamp → BVH finishing → seam weld → anti-clip →
layer tuck → accessory clearance → self-collision tuck → body clearance →
stretch cap. Each is an override of the previous ones' invariants. Three
measured undo-chains this session. Long-term: consolidate into constraint
projection where invariants coexist (the plan's XPBD note); short-term: document
each pass's invariant + which later pass may violate it, and gate each with a metric.

**F. Classification fragmentation.** Three conflicting vertex-classification
systems (see memory `project-classification-architecture`, refactor already
approved) — and passes re-derive structure ad hoc (my belt lambda recomputed
connected components locally). The island/aspect analysis from this session
belongs in ONE per-shape classifier computed once: preset, islands, per-island
{type: wrap/strap/plate/panel, vertical-aspect, rigid, skirt-like}.

**G. Metrics don't measure what the user sees.** Repeated pattern (tent-pole
lesson, again 3× this session): residual metrics were flat while the user saw
wrinkles (roughness), non-vertical hang (drape angle), inward suck (magnetism).
Missing harness metrics: `roughness` (mean vert→ring-centroid dist),
`magnetismIndex` (inward Δgap vs vanilla in concave regions), `floatIndex`
(gap minus AUTHORED base clearance — measures deviation from design, not from
zero), `interlayerGap` (per outer/inner collider pair), `drapeVerticality`.

**H. The one-way contact invariant (formalize it).** *A vertex may move toward
its support (body or inner garment) ONLY to resolve a penetration/layer
violation, or when pulled by its own edge/bending tension. NO pass may target a
uniform absolute gap.* Every existing and future pass should be audited against
this; `magnetismIndex` makes violations show up in regression.

## Part 3 — The belt fix (planned, not implemented)

**Concept: rest-on + skin-bind-to-SUPPORT + ARAP.** De-float toward the *dress*
(its actual support), not the body; preserve the authored belt↔dress offset
field (no uniform target → no magnetism); let ARAP keep the shape (no distortion,
straps translate instead of warping).

Steps (each harness-gated):
1. **Support availability.** Give the belt the SOLVED dress as its inner support:
   either (a) defer/re-solve: when a rest-on shape solves before its support,
   re-run its solve after the support is solved (the solved-cache + capture
   chaining already exists — `ReplayCaptureDir` proves per-slider re-sequencing
   works), or (b) reorder the dependency graph inner→outer for rest-on pairs.
   (a) is less invasive; start there.
2. **Base-pose bind.** For each belt vert: closest point on the DRESS BASE mesh
   (fall back to body where no dress within overlap radius) → barycentric anchor
   + signed offset. This captures the authored belt-to-dress gap field (small
   where the belt touches, larger at standoffs).
3. **Handle selection + targets.** Handles = contact-band verts (base offset ≤
   threshold, e.g. lower quartile of the wrap island; classifier supplies
   islands). Target = same barycentric point on the SOLVED dress + the SAME
   authored offset. Appendage islands (strap/buckle): no handles of their own —
   they ride with the wrap through ARAP (translated, never warped). 16-bit index
   cap respected (belt is 294 verts).
4. **ARAP interior.** `SolveArap(baseVerts, tris, handles, targets, 4)` — the
   already-building module. Fills non-handle verts minimizing local distortion;
   rest pose = original base verts (preserve authored shape; plan Phase-1 rule).
5. **Retire the nudges (delete-by-flag).** beltSeat / beltDrape / beltDeWrinkle /
   strap pull-in all OFF (superseded). Keep: `applyOuterAccessoryInnerClearance`
   (one-way safety vs other inners), final body-clearance floor, post stretch cap.
6. **Gates** (all must pass before deploy): worstEdgeStretch ≤ 3.51 (vanilla);
   Clothes faceResidual ≤ 43 (baseline); bodyPen = 0; NEW `interlayerGap`
   belt→dress ≈ base field (mean |Δ| < ~0.2); NEW `magnetismIndex` ≈ 0;
   Cloak/Pants byte-identical. Then the user's eyes.

**Risks / fallbacks.**
- ARAP too soft/stiff on the leather wrap → per-island rigidity weighting is the
  knob (plan Phase 1b already anticipates it).
- The solved dress locally intersects where the belt must sit (dress draped over
  the belt line) → the dress solve already tucks under the belt via
  outerColliders; if a re-solve loop is needed (dress⇄belt), cap at ONE
  iteration each, deterministic order (dress→belt→dress-tuck), never converge-loop.
- If ARAP misbehaves entirely: fallback = per-island least-squares RIGID fit to
  the handle targets (strap/buckle exact-rigid, wrap near-rigid) — cheaper,
  strictly shape-preserving, less smooth at the wrap.

## Part 4 — Pipeline improvement roadmap (ordered; each step independently shippable)

- **P0 — Metrics first (cheap, do before ARAP work):** add `magnetismIndex`,
  `floatIndex`, `roughness`, `interlayerGap`, `drapeVerticality` to
  `ComputeHarnessMetrics` + goldens; PLY debug coloring by Δgap-vs-vanilla (shows
  magnetism visually). This makes every later gate objective.
- **P1 — Single classifier (already approved):** one per-shape structure
  (preset, islands, per-island type/aspect/rigid/skirt-like), computed once,
  consumed by all passes. Fold in this session's component+aspect code.
- **P2 — Layer graph + rest-on:** relation detection (tuck-under vs rest-on) from
  base geometry + presets; deferred re-solve for rest-on shapes (belt = pilot).
- **P3 — ARAP core for structured garments** (belt, rigid/flex armor, accessories):
  bind + ARAP replaces their correction stacks. Belt is the PoC (Part 3).
- **P4 — Cloth consolidation:** fitted/tubular join loose on the drape solver with
  the one-way contact invariant; the radial envelope is RETIRED in favor of the
  gravity-shadow collision (Part 9) — no axis assumption, applies per-region.
- **P5 — Pass retirement:** every nudge superseded by P3/P4 goes flag-off → delete;
  each survivor gets a documented invariant + a regression metric.

## Part 5 — Session state (for the implementing session)

- Flags in `conform_hook.cpp`: `kBeltDrapeSolver=false`, `kBeltSkinBind=false`
  (PoCs with findings recorded in comments above them). Belt currently ships the
  seat+drape+de-wrinkle nudge stack (last user-visible state).
- `algo/skin_arap.{h,cpp}` builds and links (harness target only so far; add to
  the DLL target when first used there). Eigen at `reference/eigen` (26 MB).
- Diagnostic OBJs in `tmp/diag/` (belt_input/solved/solver*/skinbind, body, dress).
- Regression: `drape-harness.exe --replay-dir tmp/rm_heavy_seq` — baseline:
  Cloak 2.51/pen2, Belt 3.51/pen0, Clothes 3.024/pen35/faceResid43-61,
  Pants 3.77/pen81-84/faceResid165-177.

---

## ACQUISITION REVIEW (Parts 6–8)

*Frame: a senior team just bought this project and must ship it as a premium
product. What was built right, what we'd have done differently from day 1, and
the path from here to "works on arbitrary Nexus outfits, never worse than vanilla."*

## Part 6 — What we'd have done differently from day 1 (honest retrospective)

**Keep — these were the RIGHT calls (don't relitigate):**
- Hooking OutfitStudio instead of building a standalone tool. Ships where users
  already work; zero workflow change. Correct product instinct.
- The capture-replay harness. Fidelity-by-construction (same function, same
  bytes) is better than most studio tooling. Keep as the backbone of everything.
- Determinism as a correctness property. Non-negotiable for a "button" feature;
  the project held this line correctly.
- The preset taxonomy (8 garment archetypes) as a coarse prior — right idea,
  wrong load-bearing role (see D1 below).
- Medallion/chain rigid handling, seam welds, the BVH.

**Different — six day-1 decisions we'd change (each grounded in a measured failure):**

**D1. Never anchor to vanilla Conform's OUTPUT.** The single deepest flaw. The
pipeline treats `rawDiff` (vanilla's result) as both *input to correct* and
*anchor to respect* (supportVerts, anchorWeight, influence blend, idempotency,
safety clamp all pull toward it). Vanilla over-projects, spikes, floats, tears —
so the system fights its own reference. Measured: the belt seat was UNDONE by
the influence blend; the tent-pole spikes were INHERITED from vanilla; the belt
float IS vanilla's float. Day-1 formulation should have been:
*"Given a garment authored against body A, reproduce the authored RELATIONSHIP
against body B"* — a bind → deform → project problem where vanilla's answer is,
at most, a hint or fallback. Everything else in this list follows from that.

**D2. One constraint solver, not a pass tower.** ~20 sequential finishing passes
are sequential OVERRIDES: each enforces its invariant by violating an earlier
one (3 measured undo-chains in one session). All of them — body collision,
layer order, stretch cap, bend limit, seam weld, self-collision, one-way
contact — are *constraints*, and constraints belong in ONE projection loop
(XPBD-style, fixed iterations, fixed order = still deterministic) where they
coexist by weight instead of by ordering accident.

**D3. Bind fields as first-class, precomputed data.** Per garment, ONCE (cached
by topology hash): multi-anchor smooth binds to the base body (k-nearest
triangles with falloff — NOT single closest-point, whose face-normal offset
artifacts we measured at +0.5 stretch), the authored offset FIELD (the belt's
2.1u is design intent, not error — Part 1), garment→garment support binds (the
layer graph), island decomposition + per-island type (wrap/strap/plate/panel),
rigidity weights. The runtime then only *evaluates* binds and projects
constraints. Fast, deterministic, explainable.

**D4. The layer GRAPH from day 1, with both relations.** Tuck-under AND rest-on
(Part 2C), detected in the base pose where geometry is clean, driving solve
order. Retro-fitted layering (outerColliders bolted onto per-shape solves in
host call order) is why the belt rests on the *vanilla* dress today.

**D5. Author-override before heuristics-tuning.** Months went into tuning
classification heuristics that a modded-content ecosystem will always defeat
(the 12-vert hip flap cost a day; 3 classifier systems accreted). A premium
tool ships: heuristics for the 90% + a per-outfit sidecar file + IN-UI paint
overrides for the 10% (OutfitStudio already has brush/mask infrastructure — let
the user PAINT "rigid / drape / follow" and store it in the project). Every
hour spent tuning a magic number should have been an hour on the override UX.

**D6. Metrics designed from user-visible defects, from day 1.** The recurring
failure mode (tent-pole; 3× this session) is "harness metric flat, user sees
defect." The metric suite should have started from the defect taxonomy users
report: clipping, tearing, spikes, wrinkles, hover/magnetism, popping — each
with a number. (Part 4 P0 finally does this; it should have been week 1.)

## Part 7 — Premium-product gaps (what "usable" means beyond "algorithm works")

- **U1. Crash containment.** A crash inside the hook kills OutfitStudio and the
  user's unsaved work. Wrap the entire pass in SEH/try; on ANY failure return
  vanilla's result. Verify this exists; if not it's the first ship-blocker.
- **U2. The graceful-degradation ladder (never worse than vanilla).** Per shape:
  full solve → if any gate fails (stretch/pen/NaN/time budget), degrade to
  bind-only → degrade to vanilla + safety caps → vanilla untouched. Log which
  rung fired and why. A premium tool's floor is "does no harm."
- **U3. Slider continuity (new metric + design constraint).** Users SCRUB
  sliders. Gated/threshold passes ("if gap < X, snap") create discontinuities =
  popping between 49% and 50%. The projection formulation (D2) is naturally
  continuous; add a harness metric: `|solve(s+δ) − solve(s)|` bounded, and
  solve(50%) ≈ midpoint behavior. No pass may introduce a discontinuous gate.
- **U4. Idempotency & re-entry.** Conform twice at the same slider → byte-drift
  zero. Re-conform after user edits → respects user edits (define the contract).
- **U5. Performance budget.** Measure current per-shape solve time; budget
  (e.g. <1.5s/shape on mid hardware); ARAP/bind precompute cached by topology
  hash; progress callback + cancel in the UI.
- **U6. Authoring/override UX (from D5).** Sidecar JSON per outfit (preset,
  per-island overrides, painted masks); in-UI paint layer; ship a curated
  sidecar pack for the top-N popular Nexus outfits — instant quality on the
  meshes most users actually load.
- **U7. Explainability.** Surface the solver's decisions in the log/UI:
  "Belt: authored 2.1u loose — kept design clearance; rests on Clothes."
  Turns bug reports into actionable data and builds user trust.
- **U8. Corpus + CI.** The archetype matrix (fitted dress, loose robe, layered
  cloak, pants, rigid armour, weird-topology mod) plus degenerate-mesh fuzzing
  (non-manifold, zero-area tris, 65k-vert cap, multi-island oddities), run on
  every build via the harness. Golden metrics per outfit; any regression fails.
- **U9. Twin-shell garments by construction.** The dress's inner/outer shell
  pokethrough was fixed by a post-hoc tuck; the bind-time fix is shell LINKING
  (bind inner shell to outer shell with fixed offsets). Add to D3's bind fields.
- **U10. Release hygiene.** Versioned output (a saved project never silently
  re-solves differently after an update), archived PDBs per release, a
  diagnostics bundle (log + metrics + capture blob) users can attach to reports
  — the capture IS the perfect repro, exploit it as the support workflow.

## Part 8 — Re-sequenced roadmap to ship (supersedes the order of Part 4 / aligns with SENIOR_REVIEW phases)

- **M0 — Stabilize & instrument (ship-safety first).** U1 crash containment,
  U2 degradation ladder, P0 metrics (`magnetismIndex`, `floatIndex`, roughness,
  `interlayerGap`, verticality) + U3 continuity metric, U8 corpus skeleton.
  *Exit: no input can crash OS or produce worse-than-vanilla; every known
  defect class has a number.*
- **M1 — The core swap (D1+D3): bind → deform, vanilla demoted to fallback.**
  Bind fields precompute (multi-anchor, offset field, islands, shells) + ARAP
  reconstruction for structured garments. Belt = pilot (Part 3). Behind a flag,
  per garment class, gated by M0 metrics.
  *Exit: fitted/structured garments beat vanilla on the corpus with vanilla
  fully disabled as anchor.*
- **M2 — Layer graph (D4): both relations, solve order, garment→garment binds.**
  Rest-on lands here properly (belt/pauldron/holster class).
  *Exit: zero inter-garment clipping on the corpus without post-hoc tucks.*
- **M3 — Unified projection (D2).** Fold body collision, layer order, stretch,
  bending, seam, self-collision, one-way contact into one XPBD-style loop;
  retire the finishing tower pass-by-pass (flag off → delete, each with its
  regression metric).
  *Exit: pass count in single digits; continuity metric green while scrubbing.*
- **M4 — Cloth quality.** Loose/fitted/tubular through the same projection with
  gravity+bending; the gravity-shadow collision (Part 9) replaces the radial
  envelope as THE loose-cloth support; twin-shell linking (U9).
  *Exit: drape reads as fabric on the corpus (plumb hang below protrusions,
  bendEnergy carries the drape, stretchEnergy flat); no shell pokethrough.*
- **M5 — Product polish (D5/U5-U7).** Paint overrides + sidecars + curated
  pack, perf budget + progress/cancel, explainability strings, release
  hygiene (U10).
  *Exit: a stranger's weird Nexus outfit either just works, degrades gracefully,
  or is fixable by the user in-UI in under a minute — that is the premium bar.*

**Sequencing rationale:** M0 before everything because every later milestone's
gates depend on the metrics and the safety floor. M1 before M3 because the
unified solver needs bind targets to project toward. The belt (Part 3) rides M1
as its pilot — it is deliberately the smallest structured garment with a
rest-on dependency, so it exercises D1, D3, and a slice of D4 at once.

---

## Part 9 — THE REALISM CORE: the Gravity-Shadow rule (user's radial rule, made geometry)

*This part supersedes every "envelope" mechanism. The user's rule — repeated
across sessions and repeatedly betrayed by passes — becomes the collision
GEOMETRY itself, not an invariant passes try to respect.*

### 9.1 The rule, formalized

User's statement: *"if the body pushes cloth outwards it follows; if the body
curves back inwards, loose cloth does NOT follow — it hangs vertically with
gravity until the body pushes it out again. Once pushed out, never pulled back
in, unless it's tight."*

Formalization: **the support surface for loose cloth is not the body — it is
the DOWNWARD SWEEP of the body** (its "gravity shadow"):

    Shadow = { (x,y,z) : some z' ≥ z has (x,y,z') inside body+clearance }

A loose-cloth vertex must lie OUTSIDE Shadow. Colliding against Shadow instead
of the body gives every clause of the rule *by construction*:
- body protrudes above → its shadow extends down → cloth pushed out ✔
- body recedes below a protrusion → shadow walls are VERTICAL → cloth hangs
  plumb, never follows the body back in ✔
- the only remaining inward force is the cloth's own tension (gravity + edge
  constraints pulling toward the support's plumb line) = "unless it's tight" ✔
- magnetism becomes IMPOSSIBLE, not discouraged: no code path targets the body.

### 9.2 Why the old envelope kept failing (credit + diagnosis)

`ApplyClothDrapeEnvelopePass` + `drapeReturnRate` was this rule's first draft —
right physics, wrong geometry and wrong scope: (a) radial bins around ONE
central axis — only valid for a tube-around-torso skirt; measured pushing the
belt's off-axis strap OUTWARD (3.46→4.52); wrong for limbs, split skirts,
sleeves; (b) applied per-PRESET (loose only), so belt/fitted/tubular never got
the rule — the belt magnetism complaint is exactly this scope hole; (c) run as
a post-pass fighting other passes. The swept shadow needs no axis, works for
any topology and any garment REGION, and lives inside the solve.

**Realism refinement kept from the old design:** strictly vertical hang is the
0th-order model; real cloth returns SLIGHTLY inward below a protrusion
(catenary/pendulum toward the contact's plumb line). `drapeReturnRate`
(~0.03/unit-z) is the correct knob and survives as the shadow's "wall slope":
walls tilt inward by at most that rate instead of being perfectly vertical.
One tunable, physical meaning, default small.

### 9.3 Bend, don't stretch (the deformation model the user is describing)

Cloth deforms nearly ISOMETRICALLY: edge lengths preserved, faces ROTATE around
shared edges (hinges). The historical stretching came from passes TRANSLATING
verts (radially/along normals) and capping stretch afterwards — translation
violates isometry, the cap then fights the pass. Requirements:
- Edge (distance) constraints near-HARD in the solver (XPBD stiff); bending
  (dihedral) constraints COMPLIANT. A shadow push then redistributes into hinge
  rotations across the neighborhood — the fabric folds over the protrusion
  instead of locally stretching.
- NO post-solve vertex translation outside the solver loop (already the plan's
  forbidden move; now with the mechanism explained).
- **New metric (add to P0/M0): deformation energy split** — stretchEnergy
  (Σ edge-length deviation²) vs bendEnergy (Σ dihedral change²). Gate for any
  drape change: bendEnergy may grow, stretchEnergy stays ~flat. This is the
  number that captures "how faces bend around edges, not stretch."

### 9.4 Implementation options (for the implementing session; pick ONE, gated)

- **(a) Voxel column-sweep (RECOMMENDED).** Dense occupancy grid over the body
  bounds (~1u voxels ≈ 100×50×50 = trivial memory), rasterize body tris, OR-sweep
  top→down (O(grid)), optionally tilt walls by drapeReturnRate. Collision =
  horizontal push-out of occupied columns + clearance; exact body BVH still
  handles the final contact. Rebuilt per morph in milliseconds; deterministic;
  handles multi-limb (two legs = two shadows) with zero special cases.
- **(b) Slab-restricted BVH query.** Reuse the existing body BVH but restrict
  closest-point queries to z ≥ p.z and measure HORIZONTAL distance. No new
  structure, but it's a 2D-projected query faked through a 3D tree — verify
  correctness at silhouette edges before trusting it.
- **(c) OpenVDB swept SDF.** Only if (a) shows resolution artifacts at gates.

### 9.5 Scope and composition

- Applies per-REGION (classifier decides drape-eligible regions), not per-preset:
  dress skirt, cloak, sleeves, belt flap AND strap — any loose region of any
  garment. Tight regions use bind/ARAP follow (they're ALLOWED inward — "tight").
- **Clipping unification:** garment k's shadow = sweep(body ∪ solved inner
  garments), union order from the layer graph (M2). Body-clip and
  garment-garment-clip both become in-solve collisions against a shadow —
  post-hoc tuck passes retire with M3. (The belt rests on the dress's shadow;
  the pants live under the dress's inner surface — same mechanism, two sides.)
- Determinism: fixed grid, fixed sweep order, fixed iterations. No RNG, no
  convergence loops.

### 9.6 Gates for the realism core

- `magnetismIndex` = 0 structurally (no inward-target code path exists).
- `stretchEnergy` flat vs vanilla while `bendEnergy` carries the drape.
- `drapeVerticality` ≈ 0° on plumb sections below protrusions (butt→skirt,
  bust→front, belt flap).
- Continuity while scrubbing (shadow varies continuously with the morph).
- Corpus visual check: the user's eye — the rule's author is the acceptance test.

---

## Part 10 — M0 COMPLETION LOG (2026-07-02, Sonnet session)

**Status: M0 DONE.** All exit-gate items shipped, harness-verified, DLL rebuilt
and deployed. Read this before starting M1 — it tells you what infrastructure
now exists and, more importantly, a finding that changes M1's scope.

### 10.1 What was built

- **U1 crash containment.** `ApplyStaticClothPass` (the solver body, renamed to
  `ApplyStaticClothPassImpl`) is now called through a thin SEH wrapper
  (`ApplyStaticClothPass`, conform_hook.cpp ~8740) that catches ANY hardware
  exception (access violation, stack overflow w/ `_resetstkoflw`, etc.) raised
  anywhere in the ~8000-line solver and returns a clean empty result instead of
  taking OutfitStudio down. Zero call-site changes (all 3 callers - live hook,
  harness replay, expected-diff trust gate - go through it transparently).
  **Proven live**, not just trusted-by-pattern: a temporary null-deref was
  injected, triggered via the harness, confirmed caught (`0xC0000005`, process
  survived, clean fallback logged), then removed.
- **U2 output validation gate.** `ValidateSolveOutput` (~8801) is a hard sanity
  ceiling (NOT a quality gate - deliberately generous thresholds: stretch >8x,
  displacement >80u) run on the solve output in the LIVE hook path before
  `HostSetResultDiff` is called. On failure, `HostSetResultDiff` is skipped
  entirely, so vanilla's already-computed result (written by `g_origGenResult`
  BEFORE any smart-conform code runs) stays in place untouched. This is the
  ladder's bottom rung; the "bind-only" middle rung from the plan text doesn't
  exist yet (no bind mechanism until M1) - noted honestly rather than faked.
- **P0 metrics**, added to `HarnessMetrics`/`ComputeHarnessMetrics` and the
  `--replay` output line: `roughnessSolved`/`roughnessVanilla`,
  `floatIndexMean`/`floatIndexWorst`, `magnetismCount`/`magnetismWorst`,
  `interlayerGapByOuter`/`interlayerGapByInner` (per-collider mean signed gap,
  a PLAIN distance not a violation check), `drapeVerticality` +
  `drapeVerticalityEdgeCount`, `stretchEnergy`/`bendEnergy`. Plus PLY debug
  magenta-coloring for magnetism (verified: colored-vertex count matches
  `magnetismCount` exactly). All computed from data already in `SolverCapture`
  - no new capture format needed.
- **U3 continuity tool.** `drape-harness.exe --continuity <capA> <capB>` solves
  both captures and reports max/mean per-vertex displacement between them.
  Verified two ways: (a) identical capture vs itself → exactly zero delta
  (doubles as a live determinism proof), (b) mismatched-shape guard rejects
  comparing different garments. NOTE: judging "is this delta too much for the
  actual slider gap" needs REAL near-slider capture pairs (e.g. RM_Heavy 45%
  vs 50%), which requires scrubbing in OutfitStudio - outside this coding
  session. The tool is ready; exercising it on real pop-detection is a data-
  gathering step for later.
- **U8 corpus skeleton.** `corpus/yarl_rm_heavy/` (the 4 existing Yarl Clothes 2
  RM_Heavy captures) + `corpus/golden/yarl_rm_heavy.json` (baseline) +
  `scripts/check_corpus.py` (`--update` to accept intentional drift, otherwise
  fails loudly on ANY metric changing beyond tolerance). Verified three ways:
  clean pass, injected-regression detection (correctly caught and reported),
  `--update` round-trip. Growing to the full archetype matrix (fitted dress,
  rigid armor, weird-topology mod) is now just "capture more outfits, drop the
  .drcap files in a new `corpus/<name>/` dir, run with `--update` once" - no
  code changes needed.

### 10.2 The finding that should reshape M1's scope

**Run `--replay-dir corpus/yarl_rm_heavy` (or `check_corpus.py`) and read the
new metrics before touching code.** The headline numbers, current shipped
state, all 4 shapes:

| Shape   | magnetismCount | floatIndexMean | drapeVerticality |
|---------|----------------|----------------|------------------|
| Belt    | 201 / 294 (68%)| -0.76          | n/a (no drape-eligible verts) |
| Cloak   | 211 / 553 (38%)| -0.89          | 32.2° |
| Clothes | 667 / 1777(38%)| -0.46          | 29.1° |
| Pants   | 309 / 575 (54%)| -0.38          | n/a |

**Every garment shows large magnetismCount and negative floatIndexMean, not
just the belt.** `floatIndexMean` negative means the solved result sits
CLOSER to the body than the AUTHORED base-pose relationship, on average, for
EVERY shape. Part 1-3 of this doc treated the belt's float/magnetism as a
belt-specific problem needing a belt-specific pilot. **It is not belt-specific
- it is the pipeline's systemic behavior**, measurable now for the first time.

This does not change the M1 approach (bind→ARAP, belt as pilot is still the
right SMALLEST first case - simplest topology, clear rest-on dependency).
It changes the INTERPRETATION of M1's success: fixing the belt alone will
leave Cloak/Clothes/Pants sitting on the SAME systemic magnetism, now with a
number attached. Recommend M1's exit gate explicitly include re-running this
table after the belt pilot ships and confirming AT LEAST the belt row
improves without the other three rows moving (would confirm the pilot is
correctly scoped/isolated) - and flag in the M1 handoff whether the other
three rows should become M1's stretch goal or wait for M2/M4 (per-region,
not per-preset, once the classifier and gravity-shadow collision generalize
past the belt).

`drapeVerticality` also has a finding: Belt/Pants report `n/a` (0
drape-eligible edges) because BOTH presets force follow-weight above the 0.35
physicsDrape threshold everywhere (`PresetFollowWeight`: BELT =
`0.65+0.35*w`, min 0.65; TUBULAR's `pow(w,1.65)` can dip below 0.35 but
apparently doesn't on this specific Pants mesh at RM_Heavy). Cloak/Clothes
(LOOSE_DRAPED) sit at 29-32° from vertical - real drape-eligible material,
real room to improve once Part 9 lands, and a concrete before/after number
for that milestone's gate.

### 10.3 For the M1 (Opus) session: how to start

1. Read this Part 10, then Part 3 (belt fix plan) and Part 9 (gravity-shadow -
   informs HOW the bind/ARAP target should be chosen, not just THAT it exists).
2. `py -3 scripts/check_corpus.py` to confirm you're starting from a green
   baseline before any change.
3. `algo/skin_arap.{h,cpp}` already builds and links (harness target only -
   add to `drape_algo` CMake target when M1 code first calls it). `SolveArap`
   signature in skin_arap.h.
4. `kBeltDrapeSolver`/`kBeltSkinBind` PoC flags are OFF with findings recorded
   in comments directly above their declarations in conform_hook.cpp (search
   for those names) - read them, they document two dead ends so you don't
   re-walk them.
5. Belt currently ships the seat+drape+de-wrinkle nudge stack from the prior
   session (visually accepted by the user as "much better" at one point, then
   flagged for remaining magnetism/hover - see project-belt-behavior memory).
   M1 replaces this stack; don't tune it further, replace it.
6. After each change: `check_corpus.py`, plus eyeball the NEW metrics
   (magnetismCount should drop toward 0 for Belt; floatIndexMean should move
   toward 0, i.e. toward the AUTHORED relationship, not toward the body).
   `--continuity` the belt capture against itself as a cheap determinism
   smoke-test after any solver change (should always be exactly zero delta).

---

## Part 11 — M1 COMPLETION LOG (2026-07-02, Opus session)

**Status: M1 DONE (belt pilot), deployed.** The belt now runs bind→ARAP; the
seat/drape/de-wrinkle nudge tower is retired (flag-gated). Golden updated.

### 11.1 What was built
- **`kBeltArap` (ON):** in `ApplyStaticClothPassImpl`, right before the diff write,
  the belt result is REPLACED by a bind→ARAP solve (`algo/skin_arap.{h,cpp}`,
  `SolveArap`). Per belt vert: closest point on the SUPPORT's base surface
  (barycentric anchor + AUTHORED signed offset); the same anchor on the support's
  CURRENT surface + that offset is the target. Interior-closest-point verts (accurate
  face-normal offset) are ARAP handles; others are ARAP interior. One `SolveArap`
  over the whole belt (disconnected islands = independent ARAP blocks; each island
  guaranteed >=4 handles via promotion). Fixed 4 iterations -> DETERMINISTIC
  (verified: `--continuity` self-delta exactly 0 on all shapes, both paths).
  A one-way body floor (kBodyFloor 0.08) pushes only penetrating verts out
  afterward (ARAP has no body-collision; negative authored offsets can land inside).
- **`kBeltArapRestOn` (ON):** the SUPPORT is a solved inner LOOSE_DRAPED garment
  (the dress) when present -> the belt RESTS ON the dress (de-float); else the body.
- **Support availability (harness):** `ReplayCaptureDir` now (a) sorts a BELT after
  the loose garments (+1000 dep key) and (b) injects each solved LOOSE inner as the
  belt's `innerColliders` (`REPLAY_DIR_INNER_UPDATE`). This is the pilot heuristic;
  the general layer graph + dress<->belt cycle is M2.
- Nudge tower zeroed under `if (!kBeltArap)` in the BELT preset (delete-by-flag).

### 11.2 Results (dir-replay, now the golden)
- **Belt:** worstEdgeStretch 3.51->**2.88** (ARAP beats vanilla); bodyPen 0;
  rests on the dress at its AUTHORED offset (`authoredOffsetMean=0.77` ==
  achieved `interlayerGap->Clothes 0.74`) - de-floated from the ~2u body float,
  by construction, no uniform target. Deterministic.
- **Clothes** faceResidual 54->49, **Pants** 170->167 (both marginally BETTER from
  the reorder), **Cloak** unchanged. No regressions.
- Fallback path (single capture, no dress): binds to body at authored ~2.4u
  clearance, stretch 3.85, bodyPen 0 - reproduces the authored float WITHOUT
  magnetism (rule-correct: nothing pushes it in, so it floats).

### 11.3 IMPORTANT caveats / what M1 did NOT do (for M2)
- **magnetismIndex/floatIndex are BODY-relative and now MISLEADING for the belt.**
  A belt resting on the dress legitimately sits closer to the body, so
  magnetismCount rose (201->280) even though there is NO magnetism vs its actual
  support. Confirm rest-on correctness via `interlayerGap->dress ~= authoredOffset`
  and "belt gap to dress > 0" (above it, not sucked below), NOT via body magnetism.
  **M2 metric fix:** add a support-relative float/magnetism variant.
- **LIVE de-float is NOT guaranteed.** The harness controls solve order so the
  belt always gets the solved dress. LIVE (OutfitStudio drives call order), the
  belt de-floats onto the dress ONLY IF the dress is solved/available as its inner
  collider when the belt solves - which is why the original capture had zero
  colliders (belt solved first). The DLL logs which support it used
  (`belt M1 ARAP: support='Clothes'` vs `'body'` in drape_debug.log). GUARANTEEING
  live order (defer the belt until its support is solved) is the M2 layer-graph
  work. The deployed DLL is strictly better regardless (no magnetism, ARAP shape),
  and de-floats when the order is favorable.
- **dress<->belt cycle** is NOT resolved: with the belt solving last, the dress
  tucks under the VANILLA belt, not the solved belt. Impact was negligible here
  (Clothes even improved) but the correct one-iteration cycle (dress->belt->
  dress-retuck) is M2.
- The face-normal offset reconstruction is still approximate (fine at the dress's
  small 0.77u offset; noisier at the body's 2.4u). A deformation-gradient transfer
  would remove the residual; only needed if the body-fallback quality matters.

### 11.4 Diagnostics left in place
`belt M1 ARAP: support=... handles=... comps=... bodyFloored=... authoredOffsetMean=...`
(HLog, every belt solve) - the single most useful line for M2. Temporary M1DIAG
collider-dump removed.

## Part 12 - M2 COMPLETION LOG (2026-07-02, Opus session)

**Status: M2 DONE (belt rest-on layer graph). Deployed. Live-validation pending user.**
M2 guarantees the belt de-floats onto OUR solved dress regardless of OutfitStudio's
solve order, and adds honest rest-on metrics. The dress<->belt fixpoint (M2.2) was
found to be already closed by the existing solved-mesh cache in the common order and
was deliberately NOT given a compensating re-solve pass.

### 12.1 What was built
- **`kLayerSolveOrder` (ON) - live solve-order guarantee (M2.1).** New helper
  `EnsureSupportSolvedLocked(self, supportName, slider, bodyShell, bodyBVH)` (DLL-only,
  before `Detour_GenResult`). In the belt's inner-collider builder, when the belt is
  solved BEFORE its LOOSE_DRAPED support (the dress) in OS's call order - so the dress
  is not yet in `g_solvedShapes` - the support is solved ON DEMAND now, once, cached in
  `g_solvedShapes[slider]`, and used as the belt's inner collider. The belt therefore
  rests on OUR de-floated dress, never the vanilla-conformed one (which still carries the
  magnetism M1 removes). Scoped to the belt->dress relationship (the M1 pilot).
  - **Depth cap:** the on-demand support solve passes NO outer/inner colliders (nullptr),
    so it cannot trigger a further on-demand solve => no recursion, no cycle. It is the
    natural "dress draped on the body" rest state.
  - **No host re-entry:** pure `ApplyStaticClothPass` computation; never calls
    `g_origGenResult` or any host entry point. Runs under the caller's `g_cacheMx`.
    Result validated with `ValidateSolveOutput` (same gate as the live path) before caching.
  - **Deterministic:** same inputs + fixed iterations. When OS later calls the dress for
    real it re-solves identically (and overwrites the cache with the belt-tucked dress).
  - Log line: `M2 on-demand support solve: '<dress>' for slider '<s>' verts=N preset='...'`.
- **Support-relative honest metrics (M2.3).** M1.3 caveat: body-relative float/magnetism
  MISLEAD for a rest-on belt (it legitimately sits closer to the body). Added to
  `HarnessMetrics` + the `--replay` line: `hasSupport`, `supportFloatIndexMean/Worst`
  (solved gap-to-support minus AUTHORED base gap-to-support; 0 = tracks the design offset),
  `supportMagnetismCount/Worst` (verts pulled BELOW the authored offset toward/into the
  support - the design clearance is the reference, NOT vanilla, so the intended de-float
  is not miscounted). Nearest inner collider chosen per-vert. `hasSupport=0` => the four
  numbers are 0, distinguishing "no support" from "at offset 0".

### 12.2 Results (dir-replay = golden, updated)
- **Belt (rests on dress):** `hasSupport=1`, `supportFloatIndexMean=-0.028`
  (tracks its authored ~0.77u offset onto the dress almost exactly),
  `supportMagnetismCount=50` of 294 (worst 1.07u below design; bodyPen 0, no garment
  residual - not clips, just the approximate face-normal offset at the flap/wrap).
  Contrast the MISLEADING body-relative `magnetismCount=280` / `floatIndexMean=-1.30`:
  the honest rest-on numbers now separate "de-floated onto support (good)" from the 50
  verts worth watching.
- All PRE-EXISTING metrics on all 4 shapes are **byte-stable** vs the M1 golden
  (checker showed ONLY the 5 new fields as drift). No regression.
- **Determinism:** `--continuity` on the belt capture = `maxDelta=0 meanDelta=0`.
  The single-capture belt (no inner colliders) still body-binds (M1a fallback): stretch
  3.85, `supportFloat=0`, `floatIndexMean=-0.11` - fallback path intact.

### 12.3 M2.2 (dress<->belt cycle) - DEFERRED, with reason
The one-iteration retuck (dress re-solved under the SOLVED belt) was NOT implemented.
In the common unfavorable order (belt solved first), M2.1 already makes the belt
on-demand-solve the dress; then when OS solves the dress for real, the belt is ALREADY
cached as solved, so the dress tucks under the SOLVED belt automatically - the cycle is
closed by the existing `g_solvedShapes` cache, no extra pass needed. Only in the
favorable order (dress first) does the dress tuck under the not-yet-solved (conformed)
belt, and that difference is sub-offset in the narrow belt band (plan measured it
negligible; Clothes even improved). Adding a re-solve pass for that residual would be
exactly the compensating pass the project meta-rule forbids. Left for M4 if a measured
need appears.

### 12.4 Known limitation
M2.1's on-demand path (`EnsureSupportSolvedLocked`) is DLL-only and can only be exercised
LIVE (the harness has no `g_shape*` maps, so `--replay-dir` still fakes order via the
+1000 sort + `REPLAY_DIR_INNER` injection). The OUTCOME it produces - belt resting on the
solved dress - IS covered by the dir-replay golden. Live confirmation = the drape_debug.log
lines `M2 on-demand support solve: 'Clothes' ...` followed by `belt M1 ARAP: support='Clothes'`
(vs `support='body'`). If the belt still logs `support='body'` live, the dress isn't being
classed as the belt's inner LOOSE_DRAPED collider - a layer-classification issue, not M2.1.

## Part 13 - M4 COMPLETION LOG (2026-07-02, Opus session)

**Status: M4 DONE (gravity-shadow realism core, Part 9). Deployed, `kGravityShadow=true`
by default. User-confirmed live: "just tested and it looks great" after the jag fix.**

### 13.1 What was built
- **`GravityShadow`** (~conform_hook.cpp 2960): an axis-free voxel column-sweep over the
  morphed body's horizontal footprint. `BuildGravityShadow(verts, tris, cellSize, ceiling,
  out)` rasterizes each body triangle's 2D footprint into a dense (x,y) grid, keeping the
  per-column MAX Z **of body at or below `ceiling`** (body above the garment's drape-top is
  ignored at BUILD time, not query time - a tall column with only shoulder above a waist
  skirt casts no shadow onto that skirt). Deterministic: fixed grid, fixed triangle order,
  no RNG.
- **`GravityShadowPushOut`**: for a cloth vertex, checks whether its own column is
  "in shadow" (topZ >= vertex Z); if so, ring-searches outward (deterministic scan order)
  for the nearest column below the wall (optionally tilted inward by `wallSlope`, the
  surviving `drapeReturnRate` catenary knob) and returns a HORIZONTAL push target. Returns
  false (no push) for a free vertex - **the collision is one-sided by construction**, so a
  vertex with nothing above it in its column can never be pulled toward the body. This is
  the geometric proof of the user's rule, not a heuristic approximation of it.
- **Deterministic self-test** (`--shadow-selftest`, no capture needed): builds a synthetic
  flat plate at z=20 and asserts all three rule clauses geometrically - (1) a vertex below
  the plate is pushed OUT to the silhouette (plumb hang), (1b) the wall stays vertical just
  below the contact, (2) a vertex beyond the footprint is FREE (one-sided, never pulled in),
  (3) a vertex above the plate is FREE, (3b) a low ceiling suppresses the plate's shadow
  entirely. All 5 assertions pass.
- **Integration** (~conform_hook.cpp 6060, inside `ApplyStaticClothPassImpl`'s verletCloth
  finishing loop): replaces the old single-central-axis radial anti-tuck (`RadialBin`,
  48 angular bins around ONE axis - exactly the "right physics, wrong geometry" Part 9.2
  diagnosed: correct for a tube-around-torso skirt, wrong for limbs/off-axis straps/split
  skirts) for **LOOSE_DRAPED garments only** (dress skirt, cloak - the pieces the
  hang-in-gravity model actually describes; belt is rest-on/ARAP, pants are Tubular/follow,
  per Part 9.5's per-region scoping). The shadow is built ONCE per solve (morphed body is
  fixed during finishing) and the push runs INSIDE the finishing loop (not a final
  post-pass), interleaved with the existing edge-averaging smooth - each round's push gets
  redistributed into coherent hinge rotations by the very next smoothing pass, instead of
  freezing as a per-vertex jag (bend-don't-stretch, Part 9.3).
- Flag: `kGravityShadow` (default **true** - shipped). Env overrides for harness A/B without
  a rebuild: `DRAPE_SHADOW=0/1` (three-state: unset falls back to the compile flag),
  `DRAPE_SHADOW_PUSH` (default 0.45), `DRAPE_SHADOW_SMOOTH` (default 4).

### 13.2 The jag bug and its fix (mid-session, live-caught)
First deploy pushed each vertex the FULL distance to the silhouette in one shot. Harness
numbers looked good (dress magnetism 665->525, pants clip 81->51) and the user confirmed
the **back** of the dress now hangs perfectly plumb - but the **sides** were "janky/jagged".
Root cause: an absolute per-vertex snap to a stair-stepped grid boundary makes neighbouring
vertices quantise to different columns wherever the silhouette curves (the back is one flat
wall, so it was already smooth; the sides are the curved part). `roughnessSolved` (the P0
wrinkle metric) confirmed it numerically: 1.039 (off) -> 1.074 (on, absolute push) - a real,
measurable regression the visual complaint corresponds to.

Two fixes attempted and rejected before the one that worked:
- **Blurring the shadow's height field** (3x3 box, 2 passes) to smooth the silhouette
  BOUNDARY - made roughness WORSE (1.142) and broke the self-test (bled the ceiling clamp).
  Wrong lever: smoothing the COLLIDER doesn't smooth the RESULT if the push is still a hard
  snap.
- **Global lower push + smoothing for all presets** - fixed the dress but silently killed
  the pants' body-clip win (67->81 pen), because the pants' improvement had come from the
  strong absolute push, not the shadow's geometry per se, and pants are outside the
  gravity-shadow's actual scope (Tubular, not a hanger).

**What worked:** (a) a PARTIAL push per finishing round (`kShadowPush=0.45`, ease toward the
target instead of snapping) so consecutive rounds' own smoothing keeps neighbours coherent;
(b) a dedicated FINAL tangential Laplacian smooth (`kShadowFinalSmooth=4` rounds) after the
finishing loop, scoped to LOOSE_DRAPED only - pure neighbour-averaging with no body target,
so it cannot introduce magnetism (it has nothing to do with the body) and can only remove
HIGH-frequency jag while leaving the LOW-frequency plumb-hang shape intact; (c) narrowing
the shadow's scope from "all non-belt presets" to "LOOSE_DRAPED only", since the pants'
"win" was noise from a scope that never should have included them.

### 13.3 Results (dir-replay = golden, updated; shipped defaults push=0.45 smooth=4)
- **Clothes (dress):** magnetismCount 665->**544** (-18%), bodyPenetrations 35->40 (+5,
  noise-level - the shadow trades a few new marginal contacts for far fewer inward pulls),
  **roughnessSolved 1.039->1.035** (matches/beats baseline - the jag is gone),
  stretchEnergy 0.2912->0.2957 (flat, isometry preserved), bendEnergy carries the drape
  (0.2782->0.2828) - the bend-don't-stretch gate holds.
- **Cloak:** magnetismCount 211->224 (+6%, minor), roughness flat (1.4883->1.4881),
  drapeVerticality basically flat (32.17->31.19, still dominated by the plumb back the user
  confirmed).
- **Pants, Belt:** unchanged (out of shadow scope by design - Tubular/rest-on keep their
  existing paths).
- **Determinism:** `--continuity` self-delta = 0; `--shadow-selftest` ALL PASS (5/5);
  `--self-test` unaffected (SelfTestCloth is Fitted Cloth, out of shadow scope).
- **Live-confirmed by the user** after the jag fix, both the dress back (plumb) and sides
  (smooth, no more jank).

### 13.4 What M4 did NOT do (scope notes for later)
- The shadow is only wired into the VERLET finishing loop's loose anti-tuck slot. The
  legacy non-verlet `ApplyClothDrapeEnvelopePass`/`ApplyBodyEnvelopeDrapePass` (radial,
  central-axis) are UNTOUCHED and still used by non-verletCloth presets and the fallback
  body-shell branch when no BVH is available - those still carry the wrong-geometry
  diagnosis from Part 9.2 if a preset ever routes through them for a loose garment.
- Grid resolution is fixed at `kShadowCell=1.0` world units, rebuilt fresh every solve
  (cheap at this outfit's body density; not measured on a much denser body/mod).
- `wallSlope` (the catenary "return slightly below a contact" knob) is wired to
  `params.drapeReturnRate` but not independently tuned in this session - it inherits
  whatever the preset already had.
- Multi-limb / non-torso garments (sleeves, boots) were not in this corpus; the axis-free
  design should generalize (that was the whole point of replacing `RadialBin`), but is
  unverified beyond the belt/dress/cloak/pants corpus.

## Part 14 - M3 INVESTIGATION (2026-07-03, Opus, autonomous session)

**Status: M3.1 built + MEASURED + left gated-OFF as a knob. Key finding: folding the
finishing tower's contact/stretch does NOT improve this outfit - the tower is already
near the pen<->stretch Pareto frontier. M3's payoff is architectural, not metric.**

### 14.1 What was tried
M3's first fold (D2): a FINAL convergence loop after the whole finishing tower (placed
after the belt-ARAP block, before the diff-write) that projects body-contact (one-way
push) and edge-length (stretch cap) TOGETHER in one Gauss-Seidel loop, so the two stop
overriding each other (the tower's LAST body pushes run after its LAST stretch cap, so an
edge can leave over-stretched - `worstStretch('final')` exists to catch exactly this).
Flag `kUnifiedContact` (default false) + `DRAPE_UNIFIED=1` / `DRAPE_UNIFIED_PUSH` env
knobs for harness A/B. Belt excluded (its ARAP result is authoritative).

### 14.2 The finding (dress, dir-replay)
A hard joint projection eliminated body penetration but TORE the mesh; softening the push
(compliance) traced the whole frontier - and every point is a straight pen<->stretch
trade, none beats the tower:

| push stiffness | worstEdgeStretch | bodyPenetrations | faceResidual |
|---|---|---|---|
| OFF (tower) | **3.02** | 40 | 62 |
| 0.15 | 3.18 | 35 | 61 |
| 0.30 | 3.20 | 29 | 56 |
| 0.50 | 3.45 | 23 | 55 |
| 1.00 | 4.50 | **0** | 47 |

There is NO corner with stretch <= 3.02 AND pen < 40. Folding contact+stretch just
re-parameterizes the frontier the tower already sits near. So: (a) NOT auto-enabled -
picking a corner (e.g. halve body-poke at +0.4 stretch) is an aesthetic call that needs
the user's eye, left as a gated knob; (b) gated-off output is byte-identical to the M4
golden (corpus checker green), so zero risk to the shipped result.

### 14.3 Implication for M3 (course-correction)
The premise "passes fight => folding them improves results" is only PARTLY true. For
contact/stretch on this outfit the tower already reaches a decent frontier point (low
stretch, some pen), so folding is a lateral move. M3's genuine value is therefore
**code cleanliness + robustness** (fewer passes that can't fight on FUTURE changes /
other outfits), NOT a visible win here. Given the user's priority ("resultado ser bom")
and that further folding is (a) invisible-payoff and (b) needs a live visual gate that an
autonomous session can't provide, the safe M3 progress is **retiring the already-dead /
flag-off passes** (the `!kBeltArap` nudge tower, retired PoC flags) - corpus byte-verifies
they're dead, so deleting them cuts pass count with zero result risk - rather than folding
live passes blind. The unified-solver rewrite (all constraints compliant, XPBD stiffness
weights) remains the eventual D2 endpoint but wants visual tuning, i.e. the user present.

## Part 15 - THIGH-BUMP ROOT CAUSE + PANEL-FLATTEN (2026-07-03, Opus session)

**Symptom (user, multi-outfit test):** a rounded "bump" floating off the back of the
thigh at RM_Heavy on Fur/Blacksmith (and "almost all" short garments). Confirmed by the
user to ALSO appear with VANILLA Conform All -> inherited from the input (tent-pole
precedent #2), NOT created by our passes. M4 shadow ruled out by live A/B (bump identical
with shadow compiled off - those garments are Fitted/Tubular, the shadow never ran on them).

### 15.1 Root cause (measured via new `--float-csv` + z-profile analysis)
A garment authored HANGING off the base body has a per-vert authored gap that GROWS where
the body recedes (apron over the back of the thigh: gap 2-5u; fitted chest: 0.4u). Vanilla
conform re-applies each vert's authored gap over the MORPHED body; when proportions change
the gap field stops describing a straight hang and materializes as a LOCAL LUMP (measured:
back-thigh profile gap jumps 1.8->4.5u at z 38-46, a +0.6u local silhouette max). Our
correction-mode presets (Fitted/Tubular, `bodyGapPreserve`) faithfully preserve it.

### 15.2 Fix: hanging-panel flatten (gated `kPanelFlatten` / DRAPE_PANELS / panels_on.flag)
Rule-based minimal fix: authored gap >= 1.5u means "authored hanging, not fitted" (D3's
offset field as classifier - presetMask does NOT separate here, measured). For those verts
only: NORMAL-component Laplacian toward the vert's AUTHORED curvature (c0 from the base
mesh), in a convergence loop with the edge cap + one-way body floor (M3.1 mechanics).
- normal-only: full Laplacian slid verts tangentially (Fur Pants moved 7.4u - hem curl).
- toward c0, not 0: flattening to zero curvature erased AUTHORED detail (fur tufts).
- Results: bump silhouette max +0.56 -> +0.14; Fur Pants (neg control) meanMove 0.06;
  Yarl corpus: Pants stretchEnergy/supportStretch actually improved slightly; dress/cloak
  untouched (verlet path). Deterministic.
- Live A/B without env vars: file gate `E:/SkyrimDev/Drape/panels_on.flag` (DLL only,
  ignored by harness so the golden stays clean).

### 15.3 Catenary return (M4.1) - measured HARMFUL, default 0
The first bump hypothesis (shadow hangs dead-plumb -> shelf) was WRONG. The catenary
return implemented for it (push distance shrinks with depth below the contact) at 0.10
regressed the Yarl corpus badly (dress magnetism 544->751, worstGarmentPen 1.4->6.7,
cascading Pants bodyPen 80->136). Reverted to 0.0 (the user-approved plumb); the knob
(`DRAPE_SHADOW_RETURN`) and the cleaner push-distance formulation remain. The
`GravityShadowPushOut` isClear refactor (slope moved out of the clear-test) causes a
5th-decimal golden drift; Pants worstGarmentPen improved 5.19->3.84. Golden updated.

### 15.4 Infrastructure added
- `--float-csv <file>` on `--replay`: per-vert authored/vanilla/solved body-gap dump -
  the tool that localized the bump and measured both fix candidates.
- `corpus/fur_blacksmith_rm_heavy/` (4 captures) + golden: the bump outfits are now
  permanent regression cases. Falmer captures also exist in captures/yarl (not yet corpus).
- Pending live visual: panels_on.flag is ACTIVE - user eyeballs Blacksmith/Fur bump and
  re-confirms the Yarl dress; then flip kPanelFlatten default or keep file-gated.

## Part 16 - VAMPIRE TEAR ROOT CAUSE + IN-ROUND JUNCTION WELD (2026-07-03, autonomous)

**Symptom:** Vampire Armor (10k-vert multi-plate robes) visibly tearing between plates at
RM_Heavy. User-confirmed OURS: vanilla Conform All is clean - opposite of the bump case.

### 16.1 Root cause (measured on the user's capture)
272 of 282 torn near-pairs are base-COINCIDENT twins across plate-island junctions.
The log tells the exact story: `seam-close` closes everything to 0.000, then the
`loose-outer-body-volume` rounds (anti-collapse: one-way outward push toward a base-gap
window) move verts/face-samples along their LOCAL body normal - twins across a junction
ride DIVERGING normals and split to 10.4u. The final prox cap recovered only ~9.4u->7.7.
Two latent gates made it invisible before: `applyFinalExactSeamWeld` excluded this mesh
(compact-loose gate: n<=2500, groups<=260; vampire = 10100/848) and the between-round
support cap does not include prox pairs.

### 16.2 Fix (committed f3fb977)
Restore the invariant every round instead of repairing the wreck at the end: `force`
param on `applyFinalExactSeamWeld` (bypasses the preset/size gate for callers that
themselves break seams) + short `applyFinalProxSeamCap(6)` after each volume round.
- Vampire: torn pairs 282 -> 29 (-90%), worst gap 10.44 -> 2.59, final worstStretch
  2.04 -> 1.40 with over-1.5x edges 13 -> 0.
- Yarl corpus drift all-improvement (Cloak bodyPen 2 -> 0, Belt supportMagnetism
  3.30 -> 3.14). Goldens updated. Deterministic.
- Remaining 29 pairs (<=2.6u) are prox pairs within the final cap's allowed ratio -
  tightening that globally risks fighting legitimate stretch; needs eyes first.

### 16.3 Corpus
+ `corpus/vampire_rm_heavy/` (2 shapes) and `corpus/falmer_rm_heavy/` (8 shapes) with
goldens. Corpus now: 4 outfits / 18+ shapes at RM_Heavy.

### 16.4 Same-day related work (Part 15 follow-ups, committed 406f46b/590b8a9/02851e0)
Panel-flatten hardening from live feedback: (a) seam-twin weld per round; (b) UNION
neighbourhoods for twins (kills one-sided half-disc bias); (c) c0 scaled by local
edge-length ratio; (d) contact-aware feathering (inward corrections gated near the body -
the butt apex is the body PUSHING, a legitimate contact shape); (e) directional weights -
outward corrections bypass the contact gate so cloth BRIDGES concave features (softens
the "drawn" gluteal fold, the rule's own third clause). Blacksmith/Fur bump confirmed
~98% fixed by the user; butt-soften awaiting eyes.

## Part 17 - HORIZONTAL HOLLOW BRIDGE (2026-07-05, Codex session)

**Status: implemented as a gated live A/B, not accepted into goldens.** User clarified
the butt issue with a cross-section sketch: the requested behavior is not mesh inflation,
but loose cloth bridging a SAME-Z horizontal hollow (butt cleft) instead of following the
body's V inward. M4 gravity-shadow only bridges VERTICAL hollows (body receding below a
protrusion), so the missing complement is a horizontal slice rule.

Built `kHorizontalBridge=false` / `DRAPE_HBRIDGE=1` / live `hbridge_on.flag`:
- Builds a body profile by Z slice + radial angle.
- Builds the current loose-cloth profile each finishing round.
- Moves only LOOSE_DRAPED verts outward when BOTH profiles agree: the body slice has
  left/right convex support and the cloth dipped inward between its neighboring cloth
  supports.
- Target is conservative: body convex bridge + clearance, capped by neighboring cloth
  radius and by `DRAPE_HBRIDGE_MAX_STEP` per round. It runs inside the existing finishing
  loop before smoothing/body collision/stretch caps.

Validation:
- Default gate OFF: `scripts/check_corpus.py` green on all 4 corpora; self-test and
  shadow-selftest green. Goldens unchanged.
- Gate ON A/B (`DRAPE_HBRIDGE=1`): intended metric drift only. Vampire robe moved mildly
  (`floatIndexMean -0.039 -> -0.029`, bodyPen 73 -> 72). Falmer Cloth/Yarl loose layers
  show magnetism reductions with small dependent-layer drift. Needs live eyes before
  accepting/tuning/updating goldens.
- DLL rebuilt and deployed. `hbridge_on.flag` is active for OutfitStudio live A/B and
  ignored by git; delete/restore that file to toggle without rebuild.

### 17.1 Follow-up from live Blacksmith test
User confirmed the butt/thigh issue is on Blacksmith/Fur-style correction-mode panels, not
only LOOSE_DRAPED robes. The first HBridge pass was too narrow because it only ran inside
the verlet/LOOSE finishing path. Added a radial-curvature component to `panel-flatten`:
it restores the panel's authored radial curvature directly, so same-Z cleft dips can bridge
outward in Fitted/Tubular panels too. Inward radial relax (for thigh convexity) exists as
`DRAPE_PANELS_RADIAL_IN` / `panel_radial_in.flag`, but is default-off because harness A/B
showed it raises BlackSmithTorso magnetism/penetration unless visually justified.

Current live A/B state: `panels_on.flag` active, `hbridge_on.flag` active,
`panel_radial_in.flag` absent. Harness default remains green; with `DRAPE_PANELS=1`,
radial-out improves BlackSmithTorso versus the prior live panel path (magnetism 511 -> 471,
roughness 0.9759 -> 0.9746) but still needs eyes.

## Part 18 - RE-ARCHITECTURE PLAN: "a engrenagem" (2026-07-07, user-mandated)

**User's mandate (verbatim spirit):** surface smoothness up close (small bumps/pointy
edges on the "straight" dress) + general realism; fear that accumulated gambiarras won't
compose globally; "the radial rule seemed satisfied, but what came BEFORE it was wrong -
the body pushing the dress without touching it."

### 18.1 The diagnosis, quantified (all previously measured)
- **~35 distinct passes/stages** log per solve (was ~20 at Part 2E). Each repair pass
  fixes its own metric by nudging SOME verts and leaving 0.1-0.5u steps against
  untouched neighbours - the up-close bumps ARE this stacking (M4 jag lesson:
  collision-correctness != visual smoothness; roughnessSolved is the proxy).
- **The user's example is real:** `loose-outer-body-volume` pushes to a body-gap WINDOW
  with no contact test (forbidden uniform-gap family). Measured collateral: vampire
  junction tears (10.4u), 36951 adjustments/solve, needed an in-round weld patch (patch
  on patch). Same family: clearance/bodyGapPreserve in correction mode.
- **Vanilla-as-anchor (D1 violation) keeps resurfacing:** influence blend (Part 2A),
  centre-line lock targeting supportVerts (56e8160), seam-close restoring vanilla
  offsets. Every lock/anchor MUST target AUTHORED data.
- **M3.1 Pareto finding:** the tower sits near the pen<->stretch frontier - more patches
  only trade defects. Only architecture moves the frontier.

### 18.2 Target architecture (= RADIAL_DRAPE_RULE.md section 8, executed)
ONE preset-aware TARGET ENVELOPE built BEFORE the solve + ONE projection loop:
1. **Envelope builder** (per shape, deterministic): body radial/column profile ->
   per-preset support envelope (draped = gravity-shadow sweep; fitted = convex bridge
   over hollows; skin-tight = body+authored offset). Replaces as the single source:
   loose-outer-body-volume, drape envelopes, hanging-chain, hbridge, contact-radial,
   gravity-shadow post-push.
2. **Unified projection** (extend the M3.1 seed): edges (near-hard) + bending
   (compliant) + one-way envelope contact + seam/junction welds + layer order,
   projected TOGETHER, fixed iterations. Smoothness emerges from constraints coexisting
   instead of being sanded on afterwards.
3. **Authored-data locks only**: c0/r0/a0 basis + line locks target base mesh, never
   supportVerts.
4. **Repair tail retired pass-by-pass**: each retirement = corpus + goldens + live A/B
   gate. Kill-list candidates (in order): loose-outer-body-volume, final-body-reclear,
   post-prox caps, second seam-close, de-wrinkle remnants, dead PoC flags.

### 18.3 Migration order (each step corpus-gated, flag-gated live)
- **P0 (short):** flag consolidation - decide panels_on/hbridge_on/contact_radial_on
  fates, fold winners into defaults, update goldens, delete flags. "Live == git."
- **P1:** pass inventory classification (core/rule/repair/experimental) + retire the
  provably-dead ones. Add roughnessSolved + supportFloat to every golden gate.
- **P2:** envelope builder + feed it to the EXISTING solve as the collision target
  (replacing the post-solve pushes for LOOSE first, then correction mode).
- **P3:** fold finishing tower into the unified loop; retire repair tail.
- **P4:** M5 packaging on top of the consolidated engine.

Safety net that did NOT exist when M3 was first deferred: 5-outfit corpus (Yarl, Fur,
Blacksmith, Vampire, Falmer), determinism harness, honest metrics, preset-resolve
logging, capture pipeline. The refactor is now measurable end-to-end.

### 18.4 P0 DONE (2026-07-07): flag consolidation - "live == git"
Measured triage of the 3 live flags (full-config corpus A/B vs baseline, then isolation):
- **PROMOTED to default ON:** panel-flatten (kPanelFlatten; bump fix, user-approved),
  horizontal bridge (kHorizontalBridge; falmer residuals -112, vampire pen -6,
  dress-neutral), centre-line lock (DRAPE_CENTER_LINE_LOCK default true; seam meander
  fix, roughness cost +1.5%).
- **KEPT OFF:** contact-radial stack (loose-contact-rest, contact-radial, implied
  shadowMaxGap 0.85) - measured MAGNETISM signature: Yarl dress magnetism +175,
  roughness +11%, and it pulls loose cloth to <=0.85u of the envelope = the very
  "skintight" complaint; the dates match its activation (Jul 6). Hem z-lock also off
  (+11% roughness from full hem snap to VANILLA Z). Both remain env-gated for study.
- All three live flag FILES deleted; goldens rebaselined on the new defaults;
  deterministic; DLL deployed. Note: BlackSmithTorso magnetism +144 with panels ON is
  the honest-metric caveat again (bump flattening moves verts inward vs vanilla's float
  - user-approved visually).
- P1 kill-list additions: no-contact-radial + hem-pins (inactive, unvalidated),
  contact-radial stack (if live A/B confirms the skintight hypothesis), hem z-lock.

### 18.5 P2 ENGINE DIAGNOSIS COMPLETE (2026-07-07, autonomous; SEEN not just measured)
New ritual that changed everything: **render the solved OBJ wireframe (matplotlib) and
LOOK** (tmp/bump_analysis/wire*.png). Metrics said "fine"; the render showed the truth.
The engine has THREE structural holes; every patch to date approximates one of them:
1. **Lateral null-space** (isometric shear): verlet edges don't constrain sideways slide.
   Partially closed by azimuthal authored-restore (post-pass ON; in-loop machinery dark).
2. **Compression pile-ups**: solveStretch is stretch-only ("fold freely"); compression
   knob built (DRAPE_COMPRESS_K, dark - measured no benefit on the fan, because...)
3. **NO INTER-LAYER BINDING (the fan's real cause, = roadmap U9 twin-shell)**: the
   Blacksmith front under-flap (authored dense arch, z 8-32) ROTATES/FALLS around its
   attachment by exactly clothMaxDrop=8u and smears over the outer panel. Vanilla holds
   it at its authored offset; we hold NOTHING surface-to-surface: prox pairs only stitch
   boundary<->boundary edges. The fix is the M1 belt mechanism generalized INTRA-garment:
   bind stacked-layer verts to their parent layer's face (barycentric + authored offset,
   bidirectional). This also covers vampire plate overlaps and the front crease (the
   "wave" is the flap's top edge dragging the outer panel).
P2 BUILD LIST (priority order): (a) intra-garment layer detection (base-pose face-to-face
overlap, opposing normals) + authored-offset binds in the solve; (b) tune in-loop azimuth
(retire post-pass); (c) region shape-matching (ARAP) for wave-scale folds if (a) doesn't
subsume it. Acceptance: the Blacksmith fan/arch keeps authored shape; front crease
5.76 -> ~4.3; wire.png clean; corpus green; then USER eyes.

## Part 19 - PASS INVENTORY + KILL-LIST (2026-07-07, P1 analysis, autonomous)

Every logged pass classified so retirement is deliberate, not archaeological. CORE = the
solve itself; RULE = implements the radial rule (keep, will move in-loop at P2); REPAIR =
fixes a defect an earlier pass caused (retire as the engine subsumes it); DEAD = unreachable
in the shipped config (env default false + its live flag file deleted) - safe to delete now,
corpus stays byte-identical.

### CORE (the engine)
- islands, exact-seam-groups, prox-constraints, boundary-seam-constraints, junction-stitch
  - topology/structure setup.
- (verlet solve: gravity + solveStretch + bending + collision + weldSeams per substep).
- order-constraints - layer solve ordering.

### RULE (keep; P2 target = fold into the solve)
- gravity-shadow-gap (vertical hollow bridge, M4), hbridge (horizontal hollow bridge).
- panel-flatten (authored c0/r0/a0 shape restore - the workhorse; ON for fitted, dark for loose).
- loose-azimuth-restore (lateral null-space; post-pass ON, in-loop machinery dark).
- layer-binds (U9 intra-garment; detection works, constraint dark pending eyes).

### REPAIR (retire as the engine covers them - Part 18.2 order)
- loose-outer-body-volume (anti-collapse; the "body pushes without touching" the user flagged
  - PRIME retirement target once the envelope is a solve input).
- final-body-reclear, final-body-clearance, final-feasible-layer-tuck, final-selfcollision-tuck,
  anti-clip{,-cleanup,-collapse,-suppress,-collider}, outer-layer-inner-clearance,
  inner-clearance-collider, rigid-border, seam-close, prox-seam caps.
- cover-{diag,diff-mask,collapse} - covered-vertex suppression.

### DEAD (delete now; corpus byte-identical - contact-radial stack + Codex PoCs)
- contact-radial, loose-contact-rest, no-contact-radial, authored-hem-pins (part of
  authored-line-locks): the whole Jul-6 contact-radial experiment. Gates: env toggles
  default false AND their flag files (contact_radial_on / no_contact_radial_on) were deleted
  in P0. Rejected in P0 (magnetism signature; superseded by azimuth-restore for the lateral
  goal). Kept in git history.
- final-despike (already `(void)`-disabled), applyBeltFinishing nudge tower (dead since M1
  kBeltArap; ~370 lines).

### DEAD ENGINE KNOBS (dark, kept for P2 tuning, NOT deletion)
- DRAPE_ENGINE_AZIMUTH_K, DRAPE_COMPRESS_K/_FLOOR, DRAPE_PROX_DIST, DRAPE_LAYER_BIND_K/_MAX,
  DRAPE_PANELS_LOOSE, DRAPE_SHADOW_MAX_GAP, hem z-lock.

**Retirement is a no-op on the shipped result (corpus proves it) but removes ~15 of the ~35
passes.** Do it in the next session (build-risk: large deletions want a human watching the
compile), starting with the DEAD block, then REPAIR pass-by-pass as each engine hole closes.

---

## Part 20 — Pit-bridge: RADIAL §6 concave-contact bridge (2026-07-08)

**User report (live, Blacksmith RM_Heavy):** "protuberância na frente continua" + "buraco/
concavidade enorme na axila". Replayed his fresh capture (maxExpectedVertError=0) and
rendered (tools/render_obj.py, shaded 3-panel — the render ritual, now a reusable tool):

- **Front protuberance = THE BODY.** RM_Heavy's own belly maxY jumps 7.5→14.7u at z 65-70;
  the cloth rides 0.1-0.5u off it (legit convex contact). Vanilla identical. Not fixable,
  not a defect. Our under-belly drape is already better than vanilla's crease.
- **Armpit crater = vanilla-inherited defect #6.** Gap stats byte-identical vanilla vs ours
  (mean 0.76/max 1.92); sliver fan at x±13-15 z≈95 in both (q 0.44→0.15 vs authored).
  Root cause: nearest-point conform dives the sleeve junction into the arm↔torso canyon —
  exactly RADIAL_DRAPE_RULE §6's horizontal concavity between separate body masses.

**Fix (engine, not patch): pit-bridge inside panel-flatten.** Second eligibility source for
the existing authored-curvature restore — no new pass:
- Clause A (valley bottoms): ring-span gap minus own gap, NOW vs AUTHORED (morph-created
  hollow only), near-contact feather.
- Clause B (knife-fold WALLS): triangle-quality collapse vs authored (q drop > 0.12) —
  the walls slid TANGENTIALLY into the crease, so the ring-depth test misses them (their
  depth is negative); quality drop measures the tangential distortion basis-free. Seam
  twins excluded (a sewn seam's q-dip is design; lifting a welded column makes a pucker).
- Correction: the same c→c0·s normal restore, outward gated by max(wOut, wPit) — in the
  crater the ring average sits outward, so the restore lifts to the bridge. Inward stays
  blocked. Runs on BOTH solve paths (defect is inherited from vanilla input).
- **wNear = 1.8u contact boundary** (same as the hanging clause's contact feather). 3.0
  leaked onto the underbutt band (gap 1-3): lifts + edge caps dragged the skirt below into
  a pucker star (v395 moved 1.46u with gap 4.97).
- **Skin-tight excluded by the rule itself** (§5: following concavities is correct there).
- **Interlayer ceiling** for pit lifts (layer-tuck semantics: my body-gap ≤ outer anchor's
  body-gap − 0.30), anchors authored, computed once.
- Edge cap now **scoped to eligible-touching edges** (global cap re-policed edges other
  passes left >1.12 on the loose path → pucker stars far from any panel) and pit edges cap
  against AUTHORED rest (vanilla rest IS the defect there — the fold squeezed them).
- weldSeams welds **ALL twin groups every round** (edge caps move non-eligible verts too;
  the old any-eligible filter let the apron's centre-back seam open — vampire lesson #4).
- applyFinalProxSeamCap(6) after the loop when pits fired (armpit IS the arm↔torso
  junction stitch).

**Validated (renders + corpus, goldens rebaselined):** Blacksmith armpit fan shortened,
worstSupportEdgeStretch 7.63→1.60, centre-back byte-clean vs antes; falmer Undies straps
un-fragment (visibly better; its stretch/bendEnergy metrics rise because vanilla's squeezed
rest IS the baseline — visual beats metric); falmer assembled cuirass: no visible change,
bodyPenetrations -26/-36/-56/-8 across shapes; vampire: no new tearing, pens -13; yarl
dress: unchanged (armpits slightly cleaner). Known cosmetic debt: falmer Cuirass_Torso
worstGarmentFacePen 2.34→4.26 hidden under the outer plate (not visible assembled; the
interlayer ceiling misses anchors authored >2.5u from the outer).

**Codex audit (user asked):** nothing after c49672d — no commits/branches/stashes, tree
clean, no flag files, deployed DLLs timestamped 23:18 (my fe656c5 deploy). Nothing to undo.

### Part 20 act 2 — CHORD bridge: the fix that actually shows (2026-07-08)

User retested v1: "continua a mesmíssima coisa" — correct. v1 fixed the local slivers;
the VISIBLE crater is a REGION-scale basin. Diagnostic chain (each step measured):
1. 3-ring hollow: defect verts 2.0+u deeper than authored — but 20 rounds of outward
   c0-diffusion moved it 2.63→2.62. Normal-only moves DEAD-END: in the wedge, the vert
   normal points at the opposite canyon wall (body floor stops it at 0.2u).
2. Rim-CENTROID pull failed: the 3-ring rim wraps the convex sleeve, its centroid falls
   INSIDE the arm (BVHSignedGap = -3.0). Cloth-centroid targets are invalid on convex
   anatomy.
3. KEY measurement: pit verts sit at aGap≈cGap (0.35≈0.37) — the cloth never "dived in";
   the MORPH created the wedge and the cloth copies the body concavity at authored
   clearance. Purest §6 violation. Axis-radial hbridge cannot see it (the arm occludes
   the wedge at the same (z,θ): bodyR bin = arm outer radius, no dip).

**Fix (v2): antipodal CHORD bridge.** For each pit vert: 3-ring rim via BFS → most
antipodal pair in AUTHORED space (the two sides of the crease mouth) → lift the vert
toward the SEGMENT between them, stopping at the depth the authored design kept below
its own chord. Full-vector (exits the wedge tangentially), per-round step 0.35×remaining,
basin coverage via 2-ring eligibility dilation (contact-gated ≤2.2u). Everything else
holds: scoped edge caps (authored rest on pit edges), body floor, interlayer ceiling,
weld-all-groups per round, post prox cap. 16 rounds when chords exist.

**Validated:** Blacksmith armpit crater GONE in render (the whole wedge fan bridged);
centre-back/front/skirt clean. Corpus: falmer Undies bodyPens 662→142, Cuirass 33→2,
Cuirass_Torso 27→5 AND its facePen debt 4.26→2.05 (better than the 2.34 baseline);
pauldron 8→0; vampire/yarl visually unchanged-or-cleaner. magnetismCount rises on several
shapes — the metric misreads the bridge passing near the arm wall as attraction; the
renders overrule it (established lesson). Goldens rebaselined, deployed 12:17.

### Part 20 act 3 — GROUND TRUTH: the author's hand sculpt changes everything (2026-07-08)

User: "continua ruim... cratera nas axilas e na canela" + THE gift: "todos esses outfits eu
tinha esculpido a mão o RM_Heavy — o estado atual pré-smartconform é a referência."

**New capability:** tools/osd_extract.py parses BodySlide OSD slider data and rebuilds the
hand-sculpted RM_Heavy mesh (base + diff); tools/error_map.py scores any result against it
(per-vert distance, z-band stats, heatmap). --refbody-obj and DRAPE_FORCE_PRESET added to
the harness. This is the project's first real loss function.

**What the reference proved (Blacksmith):**
- The sculpt's armpit is a SHALLOW FILLED transition — no cavity. Neither vanilla nor any
  of our variants produce it. The v2 chord left a visible pinwheel from the front (my
  "crater gone" render was one angle; the front view showed the truth — ALWAYS render
  multiple angles).
- Scores vs sculpt: FITTED path 1.49 mean (shin band 0.58-0.71) · LOOSE path 2.19 (shin
  3.7-4.9, skirt narrowed, authored front vent Λ ERASED) · vanilla 1.42.
- The "cratera na canela" = the LOOSE ROUTING REGRESSION: Codex's longGarmentBySpan
  (2026-07-05, added expressly to flip BlackSmithTorso to Loose) reversed. Authored-gap
  profiling does NOT separate the two dress classes (6.25 vs 6.80 skirt means) — no honest
  geometric discriminator exists, so the conservative original rule stands (yarl stays
  Loose at lowerFrac 0.25, blacksmith returns Fitted via shape-name "torso").
- Pit-bridge (v1+v2) ships OFF: it LOSES against the sculpt on both paths (1.54 vs 1.49
  fitted) and the pinwheel is user-visible. Code + knob kept for v3, which must be
  designed against the sculpt reference (target: fill the pit to the sculpt's shallow
  arm↔chest bridge).

**Deployed 2026-07-08 17:44.** Expected user-visible: Blacksmith returns to the Jul-3
approved fitted look (skirt full, vent preserved, thigh-bump fix active), armpit cavity
still present (open item, now with a measurable target). NEXT: extract sculpt references
for all 4 outfits, add reference-score to the harness metrics, design armpit v3 and tune
engine knobs against the references.

---

## Part 21 — Armpit v3: region shape-matching (DRAPE_SHAPEMATCH, sculpt-validated) (2026-07-08)

User confirmed the Part 20 routing revert live ("a barra do vestido melhorou absurdamente")
and pointed the session at the one remaining item: the armpit.

**What the sculpt actually does there (measured):** the hand sculpt holds the cloth at
~1.4u off the morphed body (ours: 0.5u = authored clearance) but the displacement from
our result to the sculpt is ~3.8u and mostly TANGENTIAL (out+down, only 0.39 normal
component). Renders showed the real defect: the side-of-chest panel is CRUMPLED (spiky
normal fans — compression is free, engine null-space #2) and dives into the arm↔torso
wedge; the sculpt keeps one smooth slightly-curved sheet chest→arm.

**Dead ends measured before the fix (all scored vs the sculpt):**
- Authored-bind morph transfer (k=1/k=8): 3.79 ≈ 3.78 off. Bind ambiguity in the canyon;
  regresses the skirt (bind dist 5-8u applies wrong deltas).
- Continuous body sweep (interpolate authored→morphed body, push cloth out per step):
  3.80. The canyon never closes below ~1u, so watermelon-seed squeeze physics never
  engages. (Also: point-cloud collision must use signed plane distance, not vertex
  distance — cloth sinks between body verts otherwise.)
- Oracle (BEST of 64 nearby body deltas per vert): 2.56 — even perfect correspondence
  choice cannot reproduce the sculpt. The user created new shape.

**The fix — Müller region shape-matching, run after panel-flatten on the corrected
result:** each vert's authored 2-ring patch is refit as a similarity transform
(warm-started rotation extraction + scale clamped [1, 1.25] — regions may grow with the
morph, never shrink below authored) and the vert is pulled toward its authored local
shape. Iterated ×20 at α=0.5 with per-iter body floor (0.30), interlayer gap ceiling,
per-pass move cap (4u), seam-twin welds (base-coincident AND still coincident), and a
post-loop prox-seam cap.

**The gate (frozen from the input state, dilated 2 rings) — every clause is a measured
verdict:**
1. AFFINE-fit residual (crumple detector): an affine absorbs any smooth deformation incl.
   anisotropic morph growth; leftover = high-freq chaos. (Similarity residual flags the
   skirt's legit anisotropy; normal-dispersion alone misses the smooth basin.)
2. §6 wedge = body-normal dispersion in r=4 × CONCAVITY SIGN (normals rotating against
   positions). Without the sign the detector fires on any thin convex mass (shin!).
3. MORPH-DELTA only: subtract the same wedge measure on the AUTHORED body at the authored
   position. The armpit is 100% morph-created (wgM=1.000, wgA=0.000 measured); the groin
   concavity exists authored and is design (§5).
4. Basin floor 0.35 inside wedges (the basin interior is locally smooth but regionally
   wrong).

**Scope = the validated envelope (sculpt-scored on ALL 18 corpus garments — the loss
function catching what proxies cannot):** the pass HELPS exactly one class and HURTS the
rest, so the default gates encode that: FITTED_CLOTH preset only (Tubular/Auto/Rigid all
lose: falmer Leg_Armor 1.37→1.86, fur BaseArmor 1.60→1.75, yarl Pants 1.47→1.59);
correction path only (verlet re-drapes on purpose; yarl Clothes stretch 3.4→6.2 when
included); authored-gap band p75 ∈ [0.45, 2.0] (below = skin-tight-like, falmer Undies
follows the groin BY DESIGN and spiked stretch 1.13→9.33; above = hanging/drape-dominated,
falmer underdress 2.65 scores worse 1.68→1.91); under-outer-layer excluded per-vert
(current-space r=4; belt pen was 0.34→1.40 before) and per-garment (covered>30%).
Knobs: DRAPE_SHAPEMATCH / SM_ALL_PRESETS / SM_LOOSE / SM_TIGHT / SM_LOOSE_MAX / SM_ALPHA /
SM_ITERS / SM_SMAX / SM_R0 / SM_WEDGE_R / SM_FLOOR / SM_BASIN / SM_OUTER_R / SM_COVER_MAX /
SM_MAX_MOVE / SM_DBG; live kill-switch file sm_off.flag.

**Result (BlackSmithTorso vs sculpt):** global mean 1.482→1.403, p95 3.88→3.29; armpit
hot region 3.78→2.82, max 6.92→4.91. Renders front+left: shoulder spike fans gone, chest
smooth, armpit reads as the sculpt's shallow filled transition. Shin band untouched
(1.13/4.48 identical to off — the concavity sign closed the front-knee leak the prototype
still had). Corpus: falmer/vampire/yarl all green; drift confined to BlackSmithTorso
(energies/roughness improve; magnetismCount +157 is the known bridge-reads-as-attraction
misread; worstGarmentPen 0.65 sits under the belt) + a 2-metric fur Pants cascade.
Goldens rebaselined; corpus green.

**Port traps worth remembering:** (1) the fitted path pins exactly the contact verts the
pass exists to fix — restore passes must ignore invMass (panel-flatten precedent); (2)
the pass ran on 'Auto'-labelled garments via resolved presets — scope by the RESOLVED
preset, and verify which garments actually ran via the HLog line, not assumptions.

**Deployed 2026-07-08 23:29.** Expected user-visible: armpit crater filled to a smooth
chest→arm transition on the Blacksmith (and any exposed fitted-cloth garment in the gap
band). NEXT: keep score_all.py handy (tmp/smval has base/refbody/off/on per corpus
garment); consider folding sculpt-score into the harness; widen the SM envelope only
with reference-validated evidence per class.

---

## Part 22 — Armpit ceiling + reference-driven defect triage (2026-07-09, AFK session)

**Armpit tuning (shipped 5b8dcd0, DLL 02:46):** the user tested the first shape-match
build and still saw the crater — it had only reached armpit gap 0.83 vs the sculpt's 1.43.
Root of the shortfall (measured): the pit's crumpled FRONT-CHEST rim (v66/69/140/142) reads
convex to the wedge detector (wgM=0.000) and sat frozen 3 rings outside the gate. Defaults
α 0.5→0.8, iters 20→40, gate dilation 2→3 (new DRAPE_SM_DILATE / SM_RINGS knobs). Result:
gap 0.83→1.05, hot sculpt-error 2.88→2.46, max 4.98→4.18. SAFE: worstEdgeStretch (real
cloth stretch) is byte-flat at 4.07 across every variant — only the vanilla-relative
support-edge deviation rises (the intended correction, misread like magnetismCount). Full
front/back renders confirm skirt/vent/back untouched. Only BlackSmithTorso resolves
Fitted+in-band in the whole corpus, so the drift is confined to it; goldens rebaselined.

**ARMPIT CEILING (measured, decisive):** decomposing the remaining hot residual
(sculpt − tuned) against the body normal: total 2.46u = 1.13 normal (signed +0.60 lift) +
2.05 TANGENTIAL, and the tangential is mostly VERTICAL-down (z 1.60) + outward-to-arm. The
sculpt lets the underarm cloth DRAPE DOWN AND OUT along the arm; our conform tucks it UP
into the crease. No solve-time observable predicts the sculpt gap (auth_gap / ring_span /
morph_reced correlations all ≈0 — the sculpt just holds a ~constant 1.4u clearance sheet).
So the last ~2u is a hand-made drape RELOCATION, not a depth fill; shape-matching restores
authored LOCAL shape (gap ~0.5) and structurally cannot relocate a vert 2.5u. An additive
outward bridge does nothing here (the shape-matched sheet is already locally smooth, so
verts aren't below their rim — prototyped, gap 1.05→1.06). The armpit is at the ceiling of
what a local authored-shape restore reaches; closing it further is a fitted-drape problem
(let underarm cloth sag toward the arm), high-risk, deferred to user direction.

**Reference-driven triage (the new routine — score every garment vs its hand sculpt, render
the worst, separate real defect from legit drape variation):** tools/score_sculpt.py over
all 18 corpus garments, ranked by mean error vs sculpt. The tuned pass touches ONLY
BlackSmithTorso (1.482→1.406 BETTER); everything else byte-identical (no collateral,
confirmed). The furthest-from-sculpt garments, RENDERED and diagnosed:

| garment | mean | p95/max | verdict from the render |
|---|---|---|---|
| falmer Cloth (loincloth) | 4.45 | 7.9/13.9 | REAL: our loose flaps collapse inward+short; sculpt hangs full & long |
| vampire robes_alt | 2.19 | 6.2/12.9 | REAL: lower robe panels collapse in around legs; sculpt flares out |
| yarl Cloak | 2.11 | 5.0/7.9 | minor: hanging tail pinches thin vs sculpt's fuller drape |
| yarl Clothes (dress) | 1.71 | 6.6/12.8 | REAL: front skirt caves into a crotch cavity; sculpt drapes over thighs |
| yarl Pants | 1.47 | 6.6/12.1 | minor: pants match; 12u max is a localized crotch-seam spot only |

**DOMINANT REMAINING DEFECT CLASS = loose-drape inward collapse.** Every loose skirt/flap/
robe pulls IN toward the body (or into the leg gap) instead of hanging at the authored
fullness the sculpt preserves; the fitted/tubular/rigid garments are all within ~1.9u and
look correct. This is the single biggest corpus-wide opportunity by sculpt-error and it is
one coherent root cause on the LOOSE/verlet path (the monotonic radial envelope + gravity
pulling cloth inward at wide RM_Heavy hips, instead of bridging the hollow / holding
authored flare — cf. [[project-gravity-shadow-rule]], RADIAL_DRAPE_RULE §6). NEXT when the
user directs loose-path work: first establish OUR-defect vs vanilla-inherited via
--input-obj (the harness already carries vanilla), then target the envelope's inward bias
against these five sculpt references. Left untouched this session — loose-path changes are
high-risk and need the user in the loop. Sculpt refs for all five are built in tmp/smval;
tools/score_sculpt.py + build_sculpts triage are reusable.
