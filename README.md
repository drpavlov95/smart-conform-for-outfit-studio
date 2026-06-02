# Drape Conform — DLL Standalone Plugin

Cloth-aware deformation filter for OutfitStudio. Reduces mesh movement in loose clothing zones while keeping skin-tight garments fully articulated.

## Why DLL Standalone (Not a Fork)

BodySlide and Outfit Studio are open source but carry a **redistribution restriction**: you cannot republish modified binaries or sources. Therefore, Drape is implemented as a standalone DLL plugin, not as a fork or modified exe.

**Do not use `BodySlide-OS-Fork` in production.** That approach violates the license.

## Architecture

```text
┌─ version.dll (thin proxy, ~1 MB)
│  ├─ Windows DLL loader places it next to OutfitStudio.exe
│  ├─ Guards itself: only activates in OutfitStudio, not BodySlide
│  ├─ Injects menu item: Slider > "Drape Conform..."
│  └─ Loads drape-algo.dll on first use via LoadLibrary
│
└─ drape-algo.dll (heavy algorithm, ~5 MB)
   ├─ Contains: nifly (NIF I/O), DiffData (OSD I/O), DrapeConformer
   ├─ Loaded on-demand (not at OutfitStudio startup)
   └─ Communicates via C ABI (callbacks + function pointers)
```

## Deployment

1. **Copy both DLLs** to OutfitStudio folder:

```text
E:\Wabbajack\InstallPacks\mods\BodySlide and Outfit Studio\CalienteTools\BodySlide\
├─ OutfitStudio.exe
├─ version.dll         ← Copy from build/proxy/Release/
└─ drape-algo.dll      ← Copy from build/algo/Release/
```

1. **Open OutfitStudio** — no changes needed, no config files.

1. **Use:** `Slider > Drape Conform...` opens the Drape dialog.

## Build

```bash
cd E:\SkyrimDev\Drape
mkdir build && cd build
cmake -G "Visual Studio 17 2022" ..
msbuild Drape.sln /p:Configuration=Release /p:Platform=x64 /m
```

Output:
- `build/proxy/Release/version.dll`
- `build/algo/Release/drape-algo.dll`
- `build/cli/Release/drape-cli.exe` (CLI for testing)

## How It Works

The cloth weight is a pure function of each outfit vertex's distance to the
**reference body surface** (true point-to-triangle distance via a BVH):

```text
w(d) = 1                                   d <= tightDistance   (full follow)
w(d) = 1 - smoothstep((d-tD)/(lD-tD))      tight < d < loose    (smooth falloff)
w(d) = 0                                   d >= looseDistance   (cloth detaches)
```

Each conform delta is scaled by `w`. Skin-tight areas track the body fully;
loose cloth attenuates smoothly; very far cloth stops following large body
sliders (no ballooning). Rigidity is **explicit per shape**, never inferred
from distance (a long skirt hem is far but still *loose*, not rigid).

### Workflow (race-free)

1. In OutfitStudio: load reference body + outfit, **Conform All**, **Save Project**.
2. `Slider → Drape Conform…` → dialog (auto-detects the saved `.osp`).
3. Pick shapes, set `tight`/`loose`, run.
4. Drape reads the **saved** OSD, applies cloth weights, writes filtered OSD back.
5. **Reload** the project in OutfitStudio to view; save normally to keep.

> Drape never triggers conform itself. OutfitStudio holds fresh conform diffs in
> memory until you save, so editing the disk OSD before saving would touch stale
> data and be overwritten. Filtering the *saved* result avoids that race.

### Key algorithm properties

- **Surface-accurate distance**: BVH (`AABBTree` + `Triangle::DistanceToPoint`),
  capped at `looseDistance`; weight does not vary with body mesh density.
- **Authoritative OSD mapping**: the `.osp` `<Data target="…" name="…">file.osd</Data>`
  gives the exact `dataName → shape → file` map — no ambiguous prefix matching.
- **Robust reference body**: detected by known body names (CBBE / 3BA / HIMBO /
  BHUNP / UNP / TBD / SAM / BaseShape …). Never the "largest shape". If the
  project has no embedded body, the set is skipped with a clear message.
- **Explicit rigid mode** (per shape): replaces per-vertex deform with one uniform
  translation applied to every vertex, so accessories move as a single piece.

## Config

Dialog thresholds (game units, ≈ mm):
- **d_tight** (default 2.0): full movement within this distance.
- **d_rigid** (default 20.0): zero movement beyond this distance (this is the
  *loose* cutoff — rigidity is a separate explicit choice, not this value).
- Between them: smooth `smoothstep` transition.

## Limitations

- **Saved-project post-processor**: operates on on-disk OSD; requires Conform +
  Save first and a reload to view (editor-level I/O, not real-time).
- **Needs an embedded reference body** in the project NIF to measure distances.
- **Rigid mode** writes a diff entry for every vertex of the flagged shape
  (intended for small accessories).

## Files

- `proxy/` — version.dll: menu injection, dialog UI, drape-algo loader
- `algo/` — drape-algo.dll: NIF/OSD I/O, zone detection, diff filtering
- `cli/` — drape-cli.exe: standalone CLI for testing
- `proxy/drape_iface.h` — C ABI contract between proxy and algo

## License

Same as RleyBodyMorphs parent project.
