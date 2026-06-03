#pragma once
/*
 * conform_offsets.h — RVAs/offsets located in OutfitStudio x64.exe via Ghidra.
 *
 * At runtime: absolute address = (uintptr_t)GetModuleHandle(NULL) + RVA.
 *
 * Target build identity (verify before trusting offsets):
 *   OutfitStudio x64.exe  19,489,280 bytes  (BodySlide 2023-08-27, MSVC 14.37,
 *   official ousnius release, imagebase 0x140000000).
 *
 * Located (see re/decompile chain ConformShapes->ConformSliders->ConformShape):
 *   Automorph::GenerateResultDiff  @ RVA 0x1314f0  (hook target, 3870 bytes)
 *     void GenerateResultDiff(Automorph* this, const std::string& shapeName,
 *         const std::string& sliderName, const std::string& refDataName,
 *         bool transformResults, int maxResults, bool noSqueeze, bool solidMode,
 *         bool axisX, bool axisY, bool axisZ)
 *
 *   DiffDataSets::ZeroVertDiff (4-arg) @ RVA 0x12f1a0  (confirmed: TargetMatch +
 *     per-vertex mask scale/erase)
 *     void ZeroVertDiff(DiffDataSets* this, const std::string& set,
 *         const std::string& target, std::vector<uint16_t>* vertSet,
 *         std::unordered_map<uint16_t,float>* mask)
 *
 *   Layout: morpher = OutfitProject+0x4d8; resultDiffData = morpher+0x80.
 *     In the GenerateResultDiff detour, `self` IS the morpher (Automorph*), so
 *     resultDiffData = (char*)self + kResultDiffDataOffset.
 *
 *   Result diff is keyed by dataName = shapeName + sliderName (Automorph.cpp:368,
 *     383-384), NOT ResultDataName — so we build the key ourselves (avoids
 *     receiving a host-allocated std::string across the /MT-/MD CRT boundary).
 *
 * RVA 0 = "not located": the installer logs and no-ops.
 */

#include <cstdint>

namespace drape_off {

constexpr uint64_t  kExpectedImageSize     = 19489280ull;
constexpr uintptr_t kImageBase             = 0x140000000ull;  // for sanity logging

constexpr uintptr_t kGenerateResultDiff    = 0x1314f0;  // hook target
constexpr uintptr_t kDiffSetsZeroVertDiff  = 0x12f1a0;  // call to scale the result
constexpr uintptr_t kResultDiffDataOffset  = 0x80;      // within Automorph (morpher)

constexpr bool Configured() {
    return kGenerateResultDiff != 0 && kDiffSetsZeroVertDiff != 0;
}

} // namespace drape_off
