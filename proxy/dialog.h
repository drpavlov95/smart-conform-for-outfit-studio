#pragma once
#include <windows.h>
#include <string>
#include "drape_iface.h"

struct DrapeHookApi {
    PfnDrapeSetHookEnabled setHookEnabled = nullptr;
    PfnDrapeGetCurrentShapes getCurrentShapes = nullptr;
    PfnDrapeClearShapePresets clearShapePresets = nullptr;
    PfnDrapeSetShapePreset setShapePreset = nullptr;
    PfnDrapeSetProcessingIntensity setProcessingIntensity = nullptr;
};

// Show the Drape Conform dialog as a modal window owned by `parent`.
// bsDir = directory containing OutfitStudio.exe (and drape-cli.exe).
void ShowDrapeDialog(HWND parent, const std::string& bsDir);

// Shows the in-memory Smart Conform preset dialog. Returns true when the user
// chose to run Smart Conform and presets were applied to drape-algo.dll.
bool ShowDrapePresetDialog(HWND parent, const DrapeHookApi& api);
