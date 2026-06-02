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

### Detect Phase
1. Parse `.osp` project file (SliderSets, data folders, reference body shape)
2. For each selected outfit NIF:
   - Load NIF, find reference body shape
   - For each non-body shape, measure distance from outfit verts to body surface
   - Classify zones: **SkinTight** (tight), **Loose** (distance in range), **Rigid** (far/disconnected)
3. Display zone breakdown in real-time ListView

### Conform Phase
1. User clicks "Conform with Drape"
2. Trigger vanilla OutfitStudio `Conform All` → generates diffs in memory
3. Load each outfit NIF + corresponding `.osd` slider files
4. For each OSD entry:
   - Calculate zone weights (smoothstep falloff between d_tight and d_rigid)
   - Scale diffs: tight=100%, loose=scaled by distance, rigid=island-average
   - Rewrite `.osd` with filtered diffs
5. User reloads project in OutfitStudio to see result

## Config

Dialog exposes two thresholds (game units, ≈ mm):
- **d_tight** (default 2.0): full deformation within this distance
- **d_rigid** (default 20.0): zero deformation beyond this distance
- Between them: smooth transition via smoothstep

## Limitations

- **OSP-Only**: Works with `.osp` projects (BodySlide, Cbbe Bodyslide, etc.)
- **Disk I/O**: Modifies `.osd` files on disk after conform (editor-level I/O, not real-time)
- **Reference Shape Detection**: Auto-detects by known name (CBBE, HIMBO, etc.) or picks largest shape
- **No Explicit Rigidity**: Rigidity inferred from distance; no per-shape "solid mode" like native implementations

## Files

- `proxy/` — version.dll: menu injection, dialog UI, drape-algo loader
- `algo/` — drape-algo.dll: NIF/OSD I/O, zone detection, diff filtering
- `cli/` — drape-cli.exe: standalone CLI for testing
- `proxy/drape_iface.h` — C ABI contract between proxy and algo

## License

Same as RleyBodyMorphs parent project.
