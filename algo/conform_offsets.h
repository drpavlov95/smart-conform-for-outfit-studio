#pragma once
/*
 * conform_offsets.h — where OutfitStudio's internals live, per BodySlide build.
 *
 * At runtime: absolute address = (uintptr_t)GetModuleHandle(NULL) + RVA.
 *
 * These used to be a single set of constants for one exact build, guarded by an
 * exe-size check. That guard is right to refuse an unknown binary - jumping to a
 * stale address crashes OutfitStudio instead of printing a message - but it also
 * means EVERY BodySlide update turns Smart Conform off for every user, which it
 * did on 2026-08-04 when 5.6.3 went to 5.8.2. So this is now a TABLE, matched on
 * image size, and adding a build is data rather than surgery.
 *
 * HOW A NEW BUILD IS ADDED (tools/, all of it scripted):
 *   1. build the same BodySlide tag from source with symbols - each function
 *      lands in its own COMDAT section, so its machine code is isolated;
 *   2. tools/rank_host_funcs.py ranks the functions the binary declares in
 *      .pdata by size and masked byte similarity. Never trust a bare pattern
 *      match: MSVC prologues are stereotyped enough that one of these was found
 *      13 MB away from the truth and still "matched" 48 bytes exactly;
 *   3. tools/trace_calls.py disassembles a function already trusted and matches
 *      its call sequence against the same list read from our .obj relocations.
 *      Call order follows source order and survives recompilation, which is what
 *      finally pinned the small functions. NOTE: the official binary routes
 *      every call through incremental-link jmp thunks, so raw E8 scanning finds
 *      nothing at all - the tool resolves them;
 *   4. the member offsets come from the member ORDER in Automorph.h plus
 *      sizeof(DiffDataSets), which a probe compiled against that tag's headers
 *      prints. The formula is checked by reproducing the 5.6.3 numbers first.
 *
 * A build whose size is not in the table is REFUSED, exactly as before.
 */

#include <cstdint>

namespace drape_off {

constexpr uintptr_t kImageBase = 0x140000000ull;   // for sanity logging

// One BodySlide/OutfitStudio release.
struct HostBuild {
    const char* name;
    uint64_t    imageSize;              // identity: OutfitStudio.exe file size
    uintptr_t   generateResultDiff;     // hook target
    uintptr_t   diffSetsZeroVertDiff;   // 4-arg overload (set,target,vertSet,mask)
    uintptr_t   diffSetsGetDiffSet;
    uintptr_t   automorphGetRawResultDiff;
    uintptr_t   automorphSetResultDiff;
    uintptr_t   srcDiffDataPtrOffset;   // DiffDataSets* within Automorph
    uintptr_t   resultDiffDataOffset;   // DiffDataSets  within Automorph
    // 5.8.2 added a trailing `const std::string& resultTargetName = ""` to
    // GenerateResultDiff. The detour must take the same arguments the host
    // passes or the stack is misread, so the shape of the target is part of a
    // build's identity, not an afterthought.
    bool        generateHasResultTargetName;
    // First bytes expected at each RVA above, in the same order as the fields.
    // The size check says "this is the build I was told about"; this says "and
    // the function really is where I recorded it". Both matter, because these
    // addresses were recovered by analysis rather than from symbols, and the
    // failure mode of a wrong one is a crash inside the user's OutfitStudio
    // rather than a message. All zero = not recorded, verification skipped.
    uint8_t     prologue[5][12];
};

// Number of leading bytes compared; enough to be specific, short enough that a
// hotfix which only changes a function's body still verifies.
constexpr int kPrologueBytes = 12;

// Ordered newest first only for readability; lookup is by size.
inline constexpr HostBuild kHostBuilds[] = {
    // BodySlide 5.8.2 (2026-06-25). Located 2026-08-04 with the tooling above.
    //   GenerateResultDiff  94.8% over 128 bytes, runner-up 44%, size 5214 vs
    //                       our 5356, layout predicted it within 176 bytes
    //   GetRawResultDiff    unique exact match, ranked 100% by 27.8 points
    //   SetResultDiff       unique exact match, layout delta agrees
    //   GetDiffSet          call #1 and #17 of GenerateResultDiff, at the same
    //                       offsets our build calls it (+0x00f4, +0x03f1)
    //   ZeroVertDiff        whole call list of Automorph::ZeroVertDiff
    //                       (0x473590) aligns byte for byte
    // Offsets: sizeof(DiffDataSets) shrank 80 -> 64, so 8+16+16+64 = 0x68.
    {"BodySlide 5.8.2", 35088384ull,
     0x46e050, 0x4940e0, 0x491f80, 0x46fa10, 0x471880,
     0x68, 0x70, true,
     {{0x48,0x8b,0xc4,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41},  // generate
      {0x40,0x53,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xec},  // zeroVert
      {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x30,0x48,0x83},  // getDiffSet
      {0x40,0x53,0x56,0x57,0x48,0x83,0xec,0x50,0x48,0x8b,0x05,0x61},  // getRaw
      {0x48,0x89,0x5c,0x24,0x20,0x55,0x56,0x57,0x48,0x83,0xec,0x60}}},// setResult

    // BodySlide 5.6.3 (2023-08-27), the original Ghidra-located set.
    // Layout: 8+16+16+80 = 0x78, which the probe formula reproduces exactly.
    // Prologues not recorded: this build shipped working long before the
    // check existed, and its binary is no longer on hand to read them from.
    {"BodySlide 5.6.3", 19489280ull,
     0x1314f0, 0x12f1a0, 0x137a70, 0x130b90, 0x1312a0,
     0x78, 0x80, false, {}},
};

inline const HostBuild* FindHostBuild(uint64_t imageSize) {
    for (const HostBuild& b : kHostBuilds) {
        if (b.imageSize == imageSize && b.generateResultDiff != 0)
            return &b;
    }
    return nullptr;
}

} // namespace drape_off
