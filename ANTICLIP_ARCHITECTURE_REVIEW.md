# Anti-Clipping Architecture Review

Date: 2026-06-08
Context: Multi-session effort to eliminate garment-vs-garment clipping
(specifically a fur cloak over a dress/Clothes mesh, RM_Heavy slider).

## 1. The proven facts (from instrumentation, not theory)

| Observation | Source | Conclusion |
|---|---|---|
| `anti-clip shape='Clothes' fixedIn=70-90` per pass | log | The rule DETECTS and MOVES many dress verts under the cloak |
| `anti-clip-RESIDUAL shape='Clothes' stillBad=1-4 worstFlip=0.06-0.20` | log | After the fix, the dress is **essentially not clipping** (sub-pixel) |
| `anti-clip shape='Cloak' fixedOut=45-52` per pass | log | The CLOAK itself is being pushed OUT 45-52 verts/pass |
| `islands shape='Cloak' total=5 rigid=0` | log | Cloak is a multi-island draped sheet (fur strands) |
| Dark patches always in the SAME place (upper back/shoulders) | screenshots | Not random physics — a structural/geometric artifact |
| Visible clipping persists despite stillBad≈2 | screenshots+log | **What we see is NOT the dress poking through** |

### The real conclusion
The dress (Clothes) is correctly UNDER the cloak. The dark patches are **gaps in
the fur cloak mesh** (strands have natural openings) through which the correctly-
positioned dress shows. Furthermore, the anti-clip is **deforming the cloak** by
pushing its verts out individually (`fixedOut=45-52`), which opens/worsens those gaps.

## 2. The geometry-pass pipeline (order matters — this has been the recurring bug)

Inside `ApplyStaticClothPass`, per (shape, slider):

1. PBD/Verlet cloth solve (gravity, stretch, bending, body collision) — body only
2. Surface smooth
3. Drape envelope / layer-order (intra-mesh) / edge-constraint passes
4. Junction-stitch (weld near-touching cross-island verts)
5. **Final body anti-clip** (push every vert out of the BODY) [line ~4149]
6. **Rigid-island pass** (medallions/buckles rigid-translate) [line ~4172]
7. **Cross-island seam-close** (restore vanilla relative offset of butt-joints) [line ~4412]
8. **Per-vertex anti-clip** (garment-vs-garment local order) [line ~4501] ← LAST, correct
9. Write diffs

History of ordering bugs found:
- Anti-clip used to run BEFORE seam-close → seam-close dragged tucked verts back. FIXED (moved to last).
- Layer-sep used to run after seam-close on the cloak's cross-island seam → tore the cloak. (old approach, removed)

## 3. The anti-clip rule (current)

For each vertex of mesh M (excluding body meshes):
- Find nearest point on OTHER garments in BASE config → `baseDelta = radial(me) - radial(other)`.
  (radial = XY distance from body vertical axis; chosen because a sheet's normal is unreliable)
- Find nearest point on OTHER garments in CURRENT config → `curDelta`.
- If sign flipped (`baseDelta>0 && curDelta<0` or vice versa) → move ME radially to restore order.

### Why this is INSUFFICIENT for the cloak
1. **Radial metric is degenerate on near-horizontal surfaces** (shoulders/upper back),
   exactly where the patches are. There, radial distance barely varies between cloak
   and dress → noisy sign → wrong/no correction.
2. **It pushes the OUTERMOST mesh out.** The cloak is the outermost layer — nothing
   should be above it — so any `fixedOut` on the cloak is spurious and deforms the fur.
3. **It treats the cloak (a gappy fur sheet) as a watertight surface.** Moving its
   verts individually opens holes.

## 4. Root architectural issue

The whole approach moves the OUTFIT'S OWN vertices to resolve clipping. For two thin
cloth sheets this is fragile: every move fights the cloth solver, the seam-close, and
the other mesh's own corrections. The fur cloak makes it worse because it's not a
watertight surface — it's strands with gaps.

## 5. Options going forward

### Option A — Outermost mesh is never pushed out (surgical)
The outermost garment (highest, or the one the user marked as the top layer / the cloak)
only DRAPES; it is never pushed outward by the anti-clip. Inner garments are still pulled
in under it. Stops fur deformation. Does NOT fix the fur-gap show-through (but that may be
acceptable — it's the mesh's own nature, user finishes with zap slider).

### Option B — Accept and ship
The dress-under-cloak clip is solved (stillBad≈2). The fur gaps are the mesh's nature.
Commit the (large) progress; the framework now does free-hang, chains, layering, body-part
skip, per-vertex local-order anti-clip. Move on to test OTHER outfits to validate breadth.

### Option C — Coverage-based hiding (the "elegant global" solution the user intuited)
Instead of MOVING verts, HIDE inner geometry that is fully enclosed by an outer mesh
(like an automatic zap slider). A dress vert provably behind the cloak contributes nothing
visible, so zero its diff / collapse it. Robust to sheet gaps (only hides where genuinely
covered). Biggest change; needs an enclosure test (ray/parity or winding).

## 6. Recommendation

Do **A + B together**: stop the cloak self-deformation (A, ~10 lines), then ship and
validate breadth (B). Defer C unless gaps prove unacceptable across many outfits — C is
a real project, not a tweak, and the per-vertex order rule is already most of the value.
