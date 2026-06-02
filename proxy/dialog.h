#pragma once
#include <windows.h>
#include <string>

// Show the Drape Conform dialog as a modal window owned by `parent`.
// bsDir = directory containing OutfitStudio.exe (and drape-cli.exe).
void ShowDrapeDialog(HWND parent, const std::string& bsDir);
