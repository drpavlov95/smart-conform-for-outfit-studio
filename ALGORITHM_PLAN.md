# Drape Algorithm Refinement Plan

Status: **Implementation plan** — reviewed 3×, feasible against the real BodySlide/nifly source.

## 0. Goal

Make outfit conform **cloth-aware**: skin-tight garments follow the body fully; loose
cloth attenuates smoothly with distance; rigid accessories move as one piece without
deforming. Standalone DLL only (no modified BodySlide binaries).

## 1. Problems in the current algorithm (validated against source)

| # | Problem | Evidence | Fix |
|---|---------|----------|-----|
| 1 | **Distance ≠ rigidity.** Flood-fill turns any region with `dist > d_rigid` into a "rigid island". A long skirt hem is far from the body but should be *loose*, not rigid-translated. | `DrapeConformer.cpp:115 rigidIslands()` | Remove auto-rigid-by-distance. Rigid becomes an **explicit per-shape opt-in**. |
| 2 | **Rigid handling incomplete.** `filterDiffs` averages only verts that already have a diff and writes only those; verts without a diff entry keep deforming. | `DrapeConformer.cpp:305` | For explicit rigid shapes, apply a **uniform translation to every vertex of the shape**, derived from the body surface displacement under the shape. |
| 3 | **Distance uses nearest body *vertex*, not surface.** Weight varies with body mesh density; a sparse body face under the cloth gives wrong distance. | `DrapeConformer.cpp:96 computeDistances()` brute force vert-vert | Use **point-to-triangle** distance with BVH: `AABBTree::IntersectSphere` + `Triangle::DistanceToPoint` (both exist in source). Cap query radius at `looseDistance`. |
| 4 | **Reference body chosen by "largest shape".** A detailed outfit can have more verts than the body → wrong reference. | `DrapeConformer.cpp:25 largestShape()` | Detect by **known body target names**; if absent, require a **user-selected reference NIF**. Never silently pick the largest. |
| 5 | **OSD key matched by prefix.** `key.substr(0,shapeName.size()) == shapeName` makes `Dress` also capture `DressBelt`. | `algo.cpp:284` | Parse the `.osp` `<Data target="…" name="…">file.osd</Data>` for the **authoritative `dataName → shape → file` map**. Match OSD keys exactly. |
| 6 | **Stale-data race.** We trigger Conform All (diffs land in OS *memory*), then edit on-disk OSD (old data). On save, OS overwrites our edit. | `dialog.cpp` old flow | **Do not auto-Conform.** Operate on the **saved** project: user Conforms + Saves, Drape filters the fresh on-disk OSD, user reloads. Documented prerequisite. |

## 2. Data model (confirmed from source)

- `OSDataFile::GetDataDiffs()` → `unordered_map<string dataName, unique_ptr<unordered_map<uint16_t, Vector3>>>`.
  The map key is the **dataName alias**, not the shape name.
- `.osp` slider XML: `<Slider name="…"><Data target="ShapeName" name="dataName" local="true">file.osd</Data></Slider>`.
  `name` defaults to the slider `name` when omitted. This gives the exact
  `dataName → (shape, osdFile)` mapping. (`SliderData.h:48`, `SliderData.cpp:81-93`)
- `Triangle::DistanceToPoint(const Vector3* verts, const Vector3& p)` — true point-triangle
  distance, handles edges + face. (`Object3d.hpp:1231`)
- `AABBTree(Vector3* verts, Triangle* tris, nFacets, maxDepth=100, minFacets=2)` then
  `IntersectSphere(origin, radius, &results)` fills `IntersectResult{HitFacet, HitDistance}`
  for every facet within `radius`, with `HitDistance` = exact point-triangle distance.
  Min over results = true surface distance, capped at `radius`. (`AABBTree.cpp:571-595`)

## 3. Refined algorithm

### 3.1 Weight function (continuous loose falloff)
```
w(d) = 1                              , d <= tightDistance
w(d) = 1 - smoothstep((d - tD)/(lD - tD)) , tightDistance < d < looseDistance
w(d) = 0                              , d >= looseDistance
smoothstep(t) = t*t*(3 - 2*t)
```
- `tightDistance` default 2.0, `looseDistance` default 20.0 (game units).
- Validation: finite, `tight >= 0`, `loose > tight`; else weight = 1 (no-op, safe).

### 3.2 Distance (BVH, surface-accurate)
```
build AABBTree over (bodyVerts, bodyTris)
for each outfit vertex p:
    hits = tree.IntersectSphere(p, looseDistance)   // broad+narrow in one call
    dist[p] = min(hit.HitDistance for hit in hits, default looseDistance)
```
Beyond `looseDistance` → `dist = looseDistance` → `w = 0` (cloth ignores body there).

### 3.3 Apply to diffs (loose mode, default)
```
for each (vertIdx, delta) in conformDiffs:
    delta *= w(dist[vertIdx])
    drop entries that become ~zero      // keeps OSD small, matches BodySlide semantics
```

### 3.4 Rigid mode (explicit, per-shape opt-in)
When the user marks a shape "Rigid/Solid":
```
T = average over the shape's vertices of the *body surface displacement* beneath them
    (sample the nearest body facet's interpolated diff; fall back to mean conform delta)
for every vertex v of the shape (whether or not it had a diff):
    out[v] = T
```
This moves the accessory as one piece. Default OFF. No distance-based auto-rigid.

> v1 simplification: if per-shape body-displacement sampling is unavailable (we only
> have the outfit's own conform diffs, not the body's per-slider diffs in filter mode),
> set `T = mean(existing conform deltas of the shape)` and write it to **all** vertices.
> This is the correct "uniform translation, no deform" behaviour for filter mode.

### 3.5 Detect (preview)
Report per shape: `vertexCount`, `tightVerts (d<=tight)`, `looseVerts (tight<d<loose)`,
`farVerts (d>=loose)`. Dominant zone = max bucket. "Rigid" is shown only when the user
flagged the shape rigid (it is a choice, not a measurement).

## 4. Workflow (correct, race-free)

1. In OutfitStudio: load reference body + outfit, **Conform All**, **Save Project**.
2. `Slider → Drape Conform…` → dialog (auto-detects the saved `.osp`).
3. Pick shapes; optionally flag some **Rigid**; set `tight`/`loose`.
4. Drape reads the **saved** OSD, computes weights against the reference body, writes
   filtered OSD back. (Per-shape, per-slider, exact key mapping.)
5. **Reload** the project in OutfitStudio to view. Save normally to keep.

The dialog states the Conform+Save prerequisite explicitly. We never trigger Conform
ourselves (removes the stale-data race entirely).

## 5. Reference body resolution (robust, ordered)

1. A shape inside the outfit NIF whose name matches a known body target
   (`CBBE`, `3BA`, `3BBB Body`, `HIMBO`, `BHUNP`, `UNP`, `UUNP`, `TBD`, `SAM`, `Body`).
2. Else: a reference NIF the user selects in the dialog (Browse…).
3. Never the "largest shape". If neither resolves, skip the set with a clear log line.

## 6. Module layout

- `cli/DrapeConformer.{h,cpp}` — refined core. Uses nifly `Vector3`/`Triangle` +
  `AABBTree` directly (the heavy algo DLL and the CLI both already link these).
  Public surface:
  - `BuildWeights(body, outfit, options) -> vector<float>` (per outfit vertex)
  - `ApplyLoose(diffs, weights)` (in place; drops ~zero)
  - `ApplyRigid(diffs, shapeVertCount) ` (uniform translation to all verts)
  - `Classify(weights, options) -> {tight, loose, far}` (preview)
- `algo/algo.cpp` — `.osp` parse → `dataName→shape→file` map; per shape load verts/tris;
  build reference; call core; write OSD. Exposes `detect`/`conform` via the C ABI.
- `proxy/*` — unchanged (menu + dialog + C ABI). Dialog gains a per-shape "Rigid" check
  column and a Browse-reference button (incremental; not required for core correctness).

## 7. Validation

- Offline: `drape-cli` harness loads a real outfit + body NIF, prints per-shape
  tight/loose/far counts and writes a filtered OSD; diff the OSD sizes/values.
- In-app: deploy DLLs, Conform+Save a known outfit, run Drape, reload, eyeball that
  loose cloth no longer balloons with large body sliders while tight areas still follow.

## 8. Review notes (3 passes)

- **Pass 1 (correctness):** BVH distance capped at `looseDistance` is safe — verts with no
  facet in range get `w=0`, the intended "cloth detaches" behaviour. ✓
- **Pass 2 (data integrity):** Authoritative `.osp` mapping removes prefix ambiguity; we
  only touch OSD keys we can attribute to a known shape. Unknown keys are left untouched. ✓
- **Pass 3 (workflow):** Filtering the *saved* OSD (not memory) and requiring a reload
  removes the overwrite race. Rigid is explicit, so no skirt-hem misclassification. The
  only manual steps (Conform+Save, Reload) are unavoidable for a standalone, non-fork
  tool and are clearly surfaced. ✓
```
