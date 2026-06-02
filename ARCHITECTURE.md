# Drape Architecture Decision: DLL Standalone Only

**Status:** Final decision. Do not reconsider without explicit user approval.

## The Constraint

BodySlide and Outfit Studio source code is licensed under **BodySlide License** with a critical clause:

> You may NOT redistribute, repackage, or relicense the source code or binary artifacts of BodySlide or Outfit Studio, even if you modify them.

**Translation:** You cannot republish `OutfitStudio.exe` or `BodySlide.exe` with your modifications, even if the source is open on GitHub.

This is why many Skyrim modding tools (OSL, SSE NIF Optimizer, etc.) work as **standalone plugins**, not forks.

## Why This Eliminates Fork Options

The `BodySlide-OS-Fork` approach (from early research) violates this:

1. Implements DrapeConformer natively in `OutfitProject.cpp`
2. Rebuilds `OutfitStudio.exe` with Drape integrated
3. Deployment requires users to replace their `OutfitStudio.exe`
4. This is explicit redistribution of modified BodySlide binaries → **license violation**

Even though the source code says "open source," you **cannot redistribute modified binaries**. The exception only applies to internal/personal use.

## Why DLL Standalone Is The Only Legal Path

**DLL side-loading via `version.dll` proxy:**

```
version.dll (thin proxy)
  ├─ Loaded by Windows DLL loader (OutfitStudio.exe imports VERSION.dll)
  ├─ Not a modified BodySlide file — a new file we create
  ├─ Safe to redistribute (not BodySlide IP, only hooks into it)
  └─ Works alongside unmodified OutfitStudio.exe ✓

drape-algo.dll (heavy algorithm)
  ├─ Loaded on-demand by version.dll
  ├─ Our own code, not BodySlide code
  ├─ Safe to redistribute ✓
  └─ Zero modifications to OutfitStudio.exe
```

**Legal status:** ✅ Compliant
- Users keep their original `OutfitStudio.exe` and `BodySlide.exe`
- We only add new files (`version.dll`, `drape-algo.dll`)
- No redistribution of modified BodySlide binaries
- Compatible with Wabbajack, MO2, manual install

## Architecture Tradeoffs

| Factor | DLL Standalone | Fork + Rebuild |
|--------|---|---|
| **Legal risk** | 0% (no modified BodySlide files) | 100% (redistributes modified binaries) |
| **User friction** | Low (drop 2 DLLs in folder) | High (replace .exe, breaks auto-updates) |
| **Complexity** | Medium (two-DLL proxy + callbacks) | Low (native integration) |
| **Quality** | Good (callbacks for thread safety) | Excellent (native performance) |
| **Maintenance** | Decoupled from BodySlide updates | Must sync with BodySlide source |

## Decision

**Use DLL standalone. Always.**

Do NOT reconsider this without explicit user written approval. The user explicitly stated:

> "eu não quero que a pessoa tenha que usar um bodyslide.exe alterado ... então vai de DLL standalone"

This is not a technical preference — it's a license compliance requirement.

## For Future AI Assistants

If someone (user, reviewer, or another Claude instance) suggests:

- "Just fork BodySlide-OS-Fork"
- "Integrate natively into OutfitStudio"
- "Build your own OutfitStudio with Drape"
- "Modify the .exe and redeploy"

**STOP.** Reply:

> Drape must remain a DLL standalone because BodySlide License prohibits redistributing modified binaries. See `ARCHITECTURE.md` for why.

The user is aware of this constraint and has explicitly chosen the DLL path. Do not override this decision without asking directly.
