#pragma once
/*
 * drape_iface.h — C ABI contract between version.dll (thin proxy) and
 *                 drape-algo.dll (heavy algorithm).
 *
 * Rules:
 *  - No C++ types cross this boundary (no std::string, no std::vector).
 *  - Callbacks are invoked from a worker thread; implementations must be
 *    thread-safe (e.g. PostMessage, not SendMessage or direct UI writes).
 *  - All functions return 0 on success, negative errno-style on failure.
 *  - API struct is versioned: add new fields at the END only.
 */

#ifdef __cplusplus
extern "C" {
#endif

// ── Callback types ─────────────────────────────────────────────────────

// One line of human-readable progress/log text.
typedef void (*DrapeLogCb)(void* ctx, const char* line);

// Zone detection result for one SliderSet.
// Called once per set as detection completes (real-time ListView update).
typedef void (*DrapeZoneCb)(
    void*       ctx,
    int         setIndex,   // index in the passed selectedSets array
    const char* zone,       // "SkinTight" | "Loose" | "Rigid"
    int         tightVerts,
    int         looseVerts,
    int         rigidVerts,
    int         totalVerts
);

// ── API function signatures ────────────────────────────────────────────

// Detect cloth zones for selected SliderSets.
//   bsDir        : BodySlide data dir (trailing backslash)
//   ospPath      : full path to the .osp file
//   selectedSets : caller-allocated array of set indices (0-based)
//   nSets        : length of selectedSets
//   dTight/dRigid: zone thresholds (game units ≈ mm)
//   logCb/logCtx : progress log callback
//   zoneCb/zoneCtx: per-set zone result callback
// Returns: number of sets processed, or <0 on fatal error.
typedef int (*PfnDrapeDetect)(
    const char*  bsDir,
    const char*  ospPath,
    const int*   selectedSets,
    int          nSets,
    float        dTight,
    float        dRigid,
    DrapeLogCb   logCb,
    void*        logCtx,
    DrapeZoneCb  zoneCb,
    void*        zoneCtx
);

// Hook Conform All: trigger vanilla OutfitStudio conform, then filter
// the resulting OSD files by cloth zone.
//   osHwnd       : OutfitStudio main HWND (cast to void* for C compat)
//   conformAllId : WM_COMMAND ID for "Conform All" menu item
//   selectedSets : set indices to process
//   nSets        : length of selectedSets
// Returns: number of sets successfully filtered, or <0 on fatal error.
typedef int (*PfnDrapeConform)(
    void*        osHwnd,
    unsigned int conformAllId,
    const char*  bsDir,
    const char*  ospPath,
    const int*   selectedSets,
    int          nSets,
    float        dTight,
    float        dRigid,
    DrapeLogCb   logCb,
    void*        logCtx
);

typedef enum {
    DRAPE_PRESET_AUTO = 0,
    DRAPE_PRESET_SKIN_TIGHT = 1,
    DRAPE_PRESET_FITTED_CLOTH = 2,
    DRAPE_PRESET_LOOSE_DRAPED = 3,
    DRAPE_PRESET_TUBULAR = 4,
    DRAPE_PRESET_FLEXIBLE_ARMOR = 5,
    DRAPE_PRESET_RIGID_ARMOR = 6,
    DRAPE_PRESET_BELT = 7,
    DRAPE_PRESET_RIGID_ACCESSORY = 8
} DrapeShapePreset;

// In-memory hook preset API. These are resolved directly with GetProcAddress
// by version.dll; they are intentionally kept as plain C functions.
typedef int (*PfnDrapeGetCurrentShapes)(char* outUtf8, int outSize);
typedef int (*PfnDrapeClearShapePresets)();
typedef int (*PfnDrapeSetShapePreset)(const char* shapeNameUtf8, int preset);
typedef void (*PfnDrapeSetHookEnabled)(int enabled);

// ── Versioned API struct ───────────────────────────────────────────────

typedef struct {
    int             version;    // = DRAPE_ALGO_API_VERSION
    PfnDrapeDetect  detect;
    PfnDrapeConform conform;
} DrapeAlgoApi;

#define DRAPE_ALGO_API_VERSION 1

// ── drape-algo.dll entry point ─────────────────────────────────────────
// Returns 0 on success and fills *api. The caller does NOT own the
// function pointers — they are valid for the lifetime of the module.
// Declaration in algo.cpp only; proxy loads via GetProcAddress("DrapeGetApi")

#ifdef __cplusplus
}
#endif
