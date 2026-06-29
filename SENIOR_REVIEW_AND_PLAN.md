# Smart Conform — Plan to Make It WORK (the vision)

Date: 2026-06-18
Status: **PLANNING ONLY**. Implementation starts after the user confirms.

## The vision (locked, from the user)
A public Nexus mod. On **any non-extreme outfit**, the result must beat native Conform All
on FOUR behaviours, all equally required:

1. **Real drape** — cloth hangs, folds, keeps clearance, falls with gravity (not the
   native "shrink-wrap + interpenetrate").
2. **Zero garment-vs-garment clipping** — layers (cloak over dress, etc.) never cross.
3. **Follows the body without ugly deformation** — tracks the morph (belly/bust/hips)
   without stretching, tearing, or spiking.
4. **Rigid pieces keep their shape** — belts, buckles, plates, medallions translate, never
   warp.

Because it ships publicly, **global robustness is a hard requirement**, not a nice-to-have.
This is the single biggest constraint and it drives every decision below.

---

## The two insights that reshaped this plan

### Insight 1 — We were programming blind
Every prior day was: edit → compile → close OutfitStudio → copy DLL → reopen → load outfit →
move slider → judge by eyeball. There was no test, only a retina. A misclassified 12-vert
hip flap cost a full day because nothing *measured* it. **The single highest-ROI move in the
whole project is not an algorithm — it is a feedback loop.** Get out of the dark first.

### Insight 2 — The body is the deformation space, and the right math already exists
We kept hand-rolling closest-point follow + ad-hoc nudges, which tears when the body morphs
unevenly. The principled version is: bind each garment vertex to a point ON the body surface
(barycentric anchor + clearance) so it **moves with the skin** (envelope), then let an
**As-Rigid-As-Possible (ARAP)** solve fill the interior minimising local distortion. ARAP's
objective IS "follow the constraints but don't deform ugly", and turning up its rigidity
weight per island IS "keep rigid pieces' shape" — so it unifies Behaviours 3 and 4 in one
solver. And it is already in **libigl**, which we already cloned. We stop inventing; we
instrument what is proven.

---

## How each behaviour is doing today, and the REAL blocker for each

### Behaviour 1 — Real drape  ⚠️ works for ONE preset only
- **Today:** verletCloth produces genuine drape but only `LOOSE_DRAPED` uses it. Most
  garments run the old "vanilla + nudges" path and cling like native. Bending is off.
- **Make it work globally:** route all soft garments through the drape solver, fed by the
  envelope as a soft attractor; enable bending so fabric folds and keeps clearance.

### Behaviour 2 — Zero clipping  ⚠️ architecturally restarted (clean base)
- **Today:** disabled. Every prior attempt MOVED thin cloth → tore, or chased an impossible
  target.
- **Make it work globally:** treat it as **depth ordering**, not geometry. Per region, the
  garment with the larger body-gap is outer. Order wrong + room → gentle in-solve constraint
  (Problem A, can't tear). Order right but no room → the inner piece is *meant to be
  invisible* there → hide by per-region collapse, done right (Problem B).

### Behaviour 3 — Follow body, no ugly deform  ✅ becomes correct-by-construction
- **Today:** closest-point follow + a post-hoc stretch cap as a net. Tears come from
  uneven morph and from misclassification.
- **Make it work globally:** envelope + ARAP. Neighbours anchored to nearby skin points stay
  neighbours through the morph; ARAP refuses to over-distort the interior. The stretch cap
  stays only as a last-line net.

### Behaviour 4 — Rigid pieces keep shape  ✅ falls out of ARAP
- **Today:** rigid-island translate, gated by a fragile magic-number classifier (it caught
  the hip flap by mistake).
- **Make it work globally:** high ARAP rigidity weight per island → a plate/belt stays rigid
  by construction, far less dependent on a perfect class label.

---

## THE PLAN — phased, each phase ends testable

Discipline (non-negotiable, learned the hard way):
- **Determinism is sacred.** This is a button, not a simulation the user watches. Anything
  non-deterministic (real-time physics engines, neural draping) is rejected on principle.
- **One keystone change per phase, validated before the next.** The "5 hours, no change"
  disaster was too many coexisting systems.
- **Disable-by-flag before delete.** Never lose the clean comparison base.
- **No post-solve vertex-moving anti-clip, ever.** Proven to tear.
- **Permissive licences only** (public mod): libigl/Eigen (MPL-2.0), geometry-central (MIT),
  OpenVDB/Embree (Apache-2.0). **CGAL is banned** (GPL). Physics/ML engines banned (above).

### Phase 0 — Get out of the dark  (the feedback loop; do this FIRST)
The multiplier for everything else. No new garment behaviour yet.
- **0a. Headless harness.** Compile the core conform path (`ApplyStaticClothPass` + deps)
  into a tiny CLI: load `garment.obj + body_base.obj + body_morphed.obj` → run the pass →
  write `result.obj` + a metrics line (worst edge-stretch, body-penetration count, max
  displacement, NaN count). No OutfitStudio, no DLL lock, no open/close dance. Iterate in
  seconds.
- **0b. Trust it.** Verify the harness reproduces OutfitStudio's output on the 2 known
  outfits, so its metrics are trustworthy.
- **0c. Golden regression.** Store reference results for the known outfits; diff every build;
  fail loudly on regression. This is what ends "fix one / break another".
- **0d. Vertex-colour debug export.** Dump classification / drapeAmount / penetration as PLY
  vertex colour. SEE what the algorithm thinks instead of guessing.
- **Exit gate:** I can iterate without opening OS, and a deliberate regression is caught
  automatically.

### Phase 1 — New deformation core: envelope + ARAP  → Behaviours 3 & 4 by construction
- **1a. PoC first, not a refactor.** On ONE fitted outfit: bind each vertex to a barycentric
  anchor on the base body + signed clearance; on morph, move the anchor with the skin; run
  libigl ARAP with the body-contact verts as handles. Measure worst-stretch via the harness.
- **1b.** If worst-stretch drops and there is no tear, generalise to all fitted/skin-tight
  garments. Add per-island rigidity weight → Behaviour 4 falls out (rigid pieces preserved).
- **1c.** Demote the magic-number classifier to a coarse hint; ARAP makes the system
  error-tolerant, so we no longer need it to be perfect.
- **Exit gate:** Behaviours 3 & 4 correct on the 2 known outfits with lower worst-stretch
  than today; no tear; medallions/belt still rigid.

### Phase 2 — One drape solver, fed by the envelope  → Behaviour 1
- Route FITTED_CLOTH/TUBULAR through the verletCloth solver; enable bending. The envelope
  anchor is the soft attractor; invMass/drapeAmount sets follow-vs-hang; gravity + bending
  produce real fold and clearance.
- **Exit gate:** side-by-side vs Conform All on a fitted dress + a robe through the harness;
  drape reads as fabric, not shrink-wrap.

### Phase 3 — Anti-clip as depth-ordering  → Behaviour 2
- **3a. Problem A:** in-solve order constraint on body-gap, where there is room. Relaxes with
  stretch/bending inside the loop → cannot over-stretch an edge.
- **3b. Problem B:** per-connected-region collapse with an ERODED boundary, once at the end —
  the corrected version of what broke visually before (no fans, interior point behind the
  outer garment, soften the one-ring near visible borders).
- **Conditional tool:** if the body-gap metric is still noisy (normal flips on thin sheets),
  pull in an **OpenVDB** narrow-band SDF of the body for a smooth distance + gradient. Only
  if the envelope didn't already clean it up.
- **Exit gate:** cloak-over-dress doesn't poke; pants under dress hidden; harness reports no
  new tears.

### Phase 4 — Breadth hardening for public release  → the Nexus bar
- Run the 5–6 archetype matrix (fitted dress, loose robe, layered cloak, pants, rigid-heavy
  armour, weird-topology mod) THROUGH the harness, every build. Fix what regresses.
- **Conditional tool:** a small **k-NN classifier** on geometric features, only if ARAP did
  not make classification error-tolerant enough on the matrix.
- **Exit gate:** every archetype: no tear, no gross clip, visibly better than Conform All —
  enforced by automation, not eyeballing.

---

## External tooling decision (traffic light)

| Tool | For | Verdict |
|---|---|---|
| Headless harness + golden regression + colour debug | the feedback loop | 🟢 do first, max ROI |
| libigl (cloned, MPL-2.0) | winding/SDF/components/geodesics | 🟢 use what we have |
| ARAP (libigl) | tear-free deform + rigidity | 🟢 deformation keystone |
| geometry-central (MIT) | heat-method geodesics | 🟢 if we go full-geodesic |
| XPBD (math, not a dep) | solver stability | 🟡 real gain, touches solver core |
| OpenVDB (Apache-2.0) | body SDF field | 🟡 heavy build; only if Phase 3 needs it |
| Embree (Apache-2.0) | fast BVH | 🟡 only if BVH becomes a bottleneck |
| Pinocchio (cloned, old) | auto skin weights | 🟡 fragile integration |
| CGAL | robust geometry | 🔴 GPL — banned for a public mod |
| NvCloth/FleX/Bullet softbody/ARCSim | "real" cloth | 🔴 non-deterministic, real-time — wrong runtime |
| Neural draping (TailorNet/SMPL) | ML drape | 🔴 needs Python/GPU, non-deterministic |

The whole bottom of the table is red for ONE reason: **determinism**. Conform All is a
button that must give the same result every time.

---

## IMPLEMENTER'S FIELD GUIDE (Opus → Sonnet)

Read this before touching code. The phases above are the "what"; this is the "how / why /
do-NOT". It encodes the judgment that cost the most to learn: failure modes that look fine
until measured, seductive-wrong moves to refuse, and the exact gate per phase. **All
decisions below are PRE-MADE so implementation can run unattended — do not block to ask.**

### How to run this — autonomy unit & per-phase agent
- **Safe unit of autonomy = ONE phase, not the whole plan.** Errors compound in long
  unattended runs and sink any model. Stop at each phase GATE for a review before the next.
  The harness exists to make that check fast and binary (metric up/down), not aesthetic.
- **Phase 0 (harness):** low risk — MSVC only, no libigl. Any capable executor (Sonnet) is
  fine. Best first AFK unit: do ONLY Phase 0, then review.
- **Phase 1 (libigl + ARAP):** the integration RISK PEAK (template/CMake/Eigen build hell).
  Use the STRONGEST available agent (Codex/Opus) just to get libigl compiling + ARAP
  precomputation running; once it builds, the rest is mechanical again.
- **Phases 2–4:** mechanical once the core exists; the binding constraint is honoring the
  gates, not raw coding ability.
- **The real risk for ANY model is DISCIPLINE DRIFT** (stacking compensators past a STOP
  gate to "make it work"), not code generation. The measurable gates replace judgment with a
  yes/no — trust the harness metrics over eyeballing OutfitStudio.

### The one meta-rule (the lesson that cost the most)
When a behaviour is wrong, **find and fix the CAUSE; never add a compensating pass.** Every
lost day was a post-solve "nudge" stacked on a previous nudge until they fought each other.
If the output is wrong, the fix is upstream (bind, classification, solver), NOT a new
correction stage that runs after the solve. Feeling tempted to add a cleanup pass = stop.

### Forbidden moves (proven wrong — do not retry, however reasonable they look)
- **Moving thin-cloth verts along the body normal to fix clipping.** Measured to tear
  (neighbour edges hit 15× rest). The entire anti-clip redesign exists because of this.
- **Chasing "under the outer garment AND outside the body" at the shoulders.** Often has no
  solution (bodyConflict 22–46 of ~50). No room → HIDE, don't move.
- **Collapsing covered geometry to one global point or with an un-eroded boundary.** Produced
  fans / black holes. Only per-connected-component, eroded boundary, point behind outer mesh.
- **Adding a tuning constant to paper over a bug.** No new number without deleting one.

### Determinism is a CORRECTNESS property here (not a nicety)
A button must give byte-identical output every run, or users file "random results" bugs.
- Never iterate an `unordered_map`/`unordered_set` to decide output ORDER. Iterate sorted
  vectors / indices.
- No multithreaded float reduction with non-fixed order. If parallel, fix the reduction order.
- FIXED iteration counts, never a convergence-threshold loop (timing varies → output varies).
- No time / un-seeded RNG / uninitialised memory feeding any output.

### Hard constraints already baked in (don't fight them)
- Vertex indices are **uint16**; meshes cap at **65536 verts**
  (`n = min(baseVerts.size(), 0x10000)`, conform_hook.cpp:2967). libigl/ARAP must respect
  this — never assume 32-bit indices.
- Hook entry `Detour_GenResult` (conform_hook.cpp:5957) calls `ApplyStaticClothPass` (2931)
  once per (shape, slider). **Do not touch the hook, host ABI, MinHook, or HostSetResultDiff.**

### Do NOT touch (working / load-bearing / solves a DIFFERENT problem)
- Body-part mesh skip (vagina/anus/etc. → keep vanilla).
- Chain IDW / medallion translate (the cordão fix — works).
- Seam-close pass (intra-garment island seams — a different problem).
- Post-finishing stretch cap (`kPostCap=1.12`, conform_hook.cpp:4394) — keep as the
  last-line global net even after envelope+ARAP.
- The clean disabled bases `kAntiClipLegacyEnabled=false` (5477) and
  `kCoverCollapseEnabled=false` (4889) — leave OFF and intact (the comparison base).

### Naming caution (this WILL bite)
A param `envelopeStrength` already exists (StaticSolveParams:1914) — it is a clearance PUSH,
NOT the Phase-1 idea. The Phase-1 concept (barycentric bind to the body surface that moves
with the skin) is a DIFFERENT mechanism. Name it distinctly (`skinBind` / `g_skinAnchor`);
do not overload the existing `envelope*` fields.

### Phase 0 — Harness by CAPTURE-AND-REPLAY (not reconstruction)
- Add a capture hook just before conform_hook.cpp:6143: when `DRAPE_CAPTURE=1`, serialise
  EVERY argument to `ApplyStaticClothPass` (baseVerts, profiles, edges, tris, bodyShell,
  bodyBVH source verts, presetMask, rawDiff, outerColliders, preset, names) to one blob per
  (shape, slider).
- The standalone harness loads a blob and calls the SAME `ApplyStaticClothPass`. Fidelity is
  guaranteed by construction (same function, same bytes) — do NOT hand-rebuild the input
  pipeline.
- Trust gate (0b): replay output must equal the live `solvedDiff` to ≤1e-4 (ideally
  bit-identical). A difference IS a non-determinism bug — fix it, don't mask it.
- Build: compile the algo TU with a `DRAPE_HARNESS_BUILD` macro that `#ifdef`s out
  host/detour/MinHook code, leaving the pure solver.
- Metrics per run: worst edge-stretch, body-penetration vert count, max displacement, NaN
  count. Golden = stored metrics + result verts; regression = any metric worse or verts drift.
- Colour-debug PLY (0d): per-vert colour = class / drapeAmount / penetration. This is how you
  SEE misclassification (would have shown the hip flap in seconds).
- Do NOT over-build: no GUI, no general OBJ importer if capture-replay covers it.

### Phase 1 — Skin-bind + ARAP (pre-made decisions)
- Library: **libigl ARAP** (cloned). `igl::arap_precomputation` + `igl::arap_solve`, default
  spokes-and-rims energy, **fixed 4 iterations** (determinism). Precompute once per topology.
- ARAP rest pose = ORIGINAL garment `baseVerts` (preserve the garment's own shape).
- Handles (constrained) = verts whose REST distance to the base body is within the clearance
  band. Target = barycentric point on the MORPHED triangle + stored SIGNED normal offset
  (a vert 2cm proud stays ~2cm proud; validate correct side via offset·normal > 0). Free
  verts = ARAP interior.
- Solve PER connected component (never couple separate islands).
- Scope Phase 1 to FITTED / skin-tight presets ONLY. Loose stays on the current path until
  Phase 2 (its closest-point bind is ambiguous — that is WHY it is drape).
- Behaviour 4: raise ARAP rigidity weight per rigid-flagged island → plate/belt stays rigid
  WITHOUT the magic-number gate. Classifier becomes a coarse hint, not a knife-edge.
- Gate it behind `kEnvelopeArapEnabled` (ON in dev build; OFF restores today's clean base —
  keep both reachable).
- PoC outfit first: the Yarl Clothes dress (documented expectations + hip-flap history).
  Generalise only AFTER harness worst-stretch drops vs today.
- IF worst-stretch does not drop, or it tears: STOP. Record the metric, leave the flag OFF,
  write the finding. Do NOT stack compensators (meta-rule).

### Phase 2 — One drape solver fed by the skin-bind
- Route FITTED_CLOTH / TUBULAR through `verletCloth=true` + `physicsDrape=true` (as
  LOOSE_DRAPED already does, StaticParamsForPreset:1982). Enable `bendingStiffness` (~0.001,
  the documented soft-cloth value, :1946) and `gravity`.
- Skin-bind anchor = soft attractor; `invMass`/looseness sets follow-vs-hang.
- Gate: harness side-by-side vs vanilla on a fitted dress + a robe — folds/clearance like
  fabric, no penetration increase.

### Phase 3 — Anti-clip as depth ordering (on the clean base)
- 3a: revive the in-solve order constraint (`solveOrderConstraints`, conform_hook.cpp:3988 /
  4122) — raise `orderConstraintStrength` from 0 (:1921) only where body-gap shows room. It
  relaxes inside the loop → cannot over-stretch; no room → no-op.
- 3b: covered-region collapse, CORRECT version (per connected component, eroded boundary,
  interior point behind the outer garment, soften one-ring near visible borders, fix
  degenerate-tri normals). Drive it from `kCoveredClipDiagnosticEnabled` data (:5147) first.
- Conditional: OpenVDB body SDF ONLY if body-gap is still noisy after the skin-bind.
- Gate: cloak-over-dress no poke, pants under dress hidden, harness reports no new tears.

### Phase 4 — Breadth via the harness
- Capture blobs for the 5–6 archetypes; run every build; fix what the metrics flag.
- k-NN classifier ONLY if ARAP didn't make classification error-tolerant enough.

---

## Scope discipline (so we actually ship)
- Fur-cloak-over-dress at extreme morph is an EDGE CASE: target graceful degradation
  (hidden, not torn), not perfection.
- No new tuning number without deleting an old one.
- Don't touch the hook/host-interop (it works).

## First concrete action when implementation starts
**Phase 0a — the headless harness.** It is what takes us out of the dark; every later bet
needs a fast, trustworthy feedback loop. Build it, prove it matches OutfitStudio on the two
known outfits, then move to Phase 1 (envelope + ARAP).
