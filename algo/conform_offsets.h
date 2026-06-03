#pragma once
/*
 * conform_offsets.h — RVAs of the OutfitStudio x64.exe functions we hook.
 *
 * These are file/image RVAs (relative to the module base). At runtime the
 * absolute address = (uintptr_t)GetModuleHandle(NULL) + RVA.
 *
 * Target build identity (verify before trusting offsets):
 *   OutfitStudio x64.exe  ~19,489,280 bytes  (BodySlide 2023-08-27 release)
 *
 * Functions (from BodySlide-and-Outfit-Studio source, components/Automorph.cpp):
 *   Automorph::GenerateResultDiff(this, const std::string& shapeName,
 *       const std::string& sliderName, const std::string& refDataName,
 *       bool transformResults, int maxResults, bool noSqueeze, bool solidMode,
 *       bool axisX, bool axisY, bool axisZ)
 *   Automorph::ZeroVertDiff(this, const std::string& shapeName,
 *       const std::string& sliderName, std::vector<uint16_t>* vertSet,
 *       std::unordered_map<uint16_t,float>* mask)
 *
 * RVA 0 = "not yet located": the hook installer logs and no-ops so the rest of
 * Drape keeps working. Filled in from the Ghidra report (re/drape_re_report.txt).
 */

#include <cstdint>

namespace drape_off {

// Expected module size for a sanity check before hooking (0 = skip check).
constexpr uint64_t kExpectedImageSize = 19489280ull;

// RVAs — fill from Ghidra (address - imagebase).
constexpr uintptr_t kGenerateResultDiff = 0x0;   // Automorph::GenerateResultDiff
constexpr uintptr_t kZeroVertDiff        = 0x0;   // Automorph::ZeroVertDiff

constexpr bool Configured() {
    return kGenerateResultDiff != 0 && kZeroVertDiff != 0;
}

} // namespace drape_off
