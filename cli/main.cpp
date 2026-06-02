// drape-cli: cloth-aware BodySlide conform
//
// Usage:
//   drape-cli --detect  --nif <outfit.nif> --ref-nif <body.nif> [--d-tight F] [--d-rigid F]
//   drape-cli --conform --nif <outfit.nif> --ref-nif <body.nif>
//             --ref-osd <body_RM.osd> --output-osd <result.osd>
//             [--shapes name1,name2,...] [--d-tight F] [--d-rigid F]
//
// --detect output (one line per shape):
//   DETECT shape=<name> zone=<SkinTight|Loose|Rigid> tight=N loose=N rigid=N total=N
//
// --conform: reads RM_Heavy/RM_Muscular/RM_Thin diffs from ref-osd, applies Drape
//            zone-filtered conform to selected shapes, writes result to output-osd.
//            Output OSD entry names: <shapeName>RM_Heavy, <shapeName>RM_Muscular, etc.

#include "DrapeConformer.h"
#include "DiffData.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

// ── Arg parsing ────────────────────────────────────────────────────────

static std::string getArg(const std::vector<std::string>& args,
                          const std::string& flag,
                          const std::string& def = "") {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == flag) return args[i + 1];
    return def;
}

static bool hasFlag(const std::vector<std::string>& args, const std::string& flag) {
    return std::find(args.begin(), args.end(), flag) != args.end();
}

static std::unordered_set<std::string> splitCSV(const std::string& s) {
    std::unordered_set<std::string> out;
    std::istringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ','))
        if (!tok.empty()) out.insert(tok);
    return out;
}

// ── Zone to string ─────────────────────────────────────────────────────

static const char* zoneName(ShapeResult::Zone z) {
    switch (z) {
        case ShapeResult::Zone::SkinTight: return "SkinTight";
        case ShapeResult::Zone::Loose:     return "Loose";
        case ShapeResult::Zone::Far:       return "Far";
    }
    return "Unknown";
}

// ── Detect mode ────────────────────────────────────────────────────────

static int runDetect(const std::vector<std::string>& args) {
    std::string nifPath    = getArg(args, "--nif");
    std::string refNifPath = getArg(args, "--ref-nif");
    float dTight = std::stof(getArg(args, "--d-tight", "2.0"));
    float dRigid = std::stof(getArg(args, "--d-rigid", "20.0"));

    if (nifPath.empty() || refNifPath.empty()) {
        std::cerr << "Error: --detect requires --nif and --ref-nif\n";
        return 1;
    }

    DrapeOptions opts;
    opts.tightDistance = dTight;
    opts.looseDistance = dRigid;
    DrapeConformer dc(opts);

    auto results = dc.detect(nifPath, refNifPath);
    if (results.empty()) {
        std::cerr << "Error: failed to load NIFs or no shapes found\n";
        return 1;
    }

    for (const auto& sr : results) {
        std::cout << "DETECT"
                  << " shape=" << sr.name
                  << " zone="  << zoneName(sr.dominantZone)
                  << " tight=" << sr.tightVerts
                  << " loose=" << sr.looseVerts
                  << " far="   << sr.farVerts
                  << " total=" << sr.vertexCount
                  << "\n";
    }
    return 0;
}

// ── Conform mode ───────────────────────────────────────────────────────

// Slider suffixes Drape handles (same RM_* set as mppatcher)
static constexpr const char* RM_SLIDERS[] = {"RM_Heavy", "RM_Muscular", "RM_Thin"};

// Find which RM_* slider (if any) a key ends with.
// Returns the slider name or empty string.
static std::string matchSlider(const std::string& key) {
    for (const char* sl : RM_SLIDERS) {
        std::string suffix(sl);
        if (key.size() >= suffix.size() &&
            key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0)
            return suffix;
    }
    return "";
}

static int runConform(const std::vector<std::string>& args) {
    std::string nifPath    = getArg(args, "--nif");
    std::string refNifPath = getArg(args, "--ref-nif");
    std::string refOsdPath = getArg(args, "--ref-osd");
    std::string outOsdPath = getArg(args, "--output-osd");
    std::string shapesCSV  = getArg(args, "--shapes");
    float dTight = std::stof(getArg(args, "--d-tight", "2.0"));
    float dRigid = std::stof(getArg(args, "--d-rigid", "20.0"));

    if (nifPath.empty() || refNifPath.empty() || refOsdPath.empty() || outOsdPath.empty()) {
        std::cerr << "Error: --conform requires --nif, --ref-nif, --ref-osd, --output-osd\n";
        return 1;
    }

    // Optional shape filter
    auto shapeFilter = shapesCSV.empty()
        ? std::unordered_set<std::string>{}
        : splitCSV(shapesCSV);
    bool filterShapes = !shapeFilter.empty();

    // ── Load reference OSD ──────────────────────────────────────────
    OSDataFile refOsd;
    if (!refOsd.Read(refOsdPath)) {
        std::cerr << "Error: failed to read ref OSD: " << refOsdPath << "\n";
        return 1;
    }

    // Group reference diffs by slider name
    // ref OSD entries are named  <refShapeName><sliderName>, e.g. "CBBERM_Heavy"
    std::unordered_map<std::string, VertexDiffs> sliderDiffs;  // sliderName → VertexDiffs
    for (auto& [key, tdiffs] : refOsd.GetDataDiffs()) {
        if (!tdiffs) continue;
        std::string slider = matchSlider(key);
        if (slider.empty()) continue;
        // Merge into combined VertexDiffs for this slider (multiple ref shapes → same body)
        auto& vd = sliderDiffs[slider];
        for (auto& [idx, vec] : *tdiffs)
            vd[idx] = {vec.x, vec.y, vec.z};
    }

    if (sliderDiffs.empty()) {
        std::cerr << "Error: no RM_* slider diffs found in ref OSD: " << refOsdPath << "\n";
        return 1;
    }

    // ── Run Drape conform per slider ─────────────────────────────────
    DrapeOptions opts;
    opts.tightDistance = dTight;
    opts.looseDistance = dRigid;
    DrapeConformer dc(opts);

    OSDataFile outOsd;

    for (const auto& [slider, vd] : sliderDiffs) {
        auto results = dc.conform(nifPath, refNifPath, vd);
        if (results.empty()) {
            std::cerr << "Error: conform failed for slider " << slider << "\n";
            return 1;
        }

        for (const auto& sr : results) {
            if (filterShapes && shapeFilter.find(sr.name) == shapeFilter.end())
                continue;

            // Only write non-trivial diffs
            if (sr.diffs.empty()) continue;

            std::string dataName = sr.name + slider;
            TargetDataDiffs tdd;
            for (auto& [idx, v] : sr.diffs)
                tdd[idx] = {v.x, v.y, v.z};

            outOsd.SetDataDiff(dataName, tdd);

            std::cout << "CONFORM"
                      << " shape=" << sr.name
                      << " slider=" << slider
                      << " zone=" << zoneName(sr.dominantZone)
                      << " tight=" << sr.tightVerts
                      << " loose=" << sr.looseVerts
                      << " far=" << sr.farVerts
                      << " diffs=" << sr.diffs.size()
                      << "\n";
        }
    }

    // ── Write output OSD ─────────────────────────────────────────────
    fs::create_directories(fs::path(outOsdPath).parent_path());
    if (!outOsd.Write(outOsdPath)) {
        std::cerr << "Error: failed to write output OSD: " << outOsdPath << "\n";
        return 1;
    }

    std::cout << "Done: " << outOsdPath << "\n";
    return 0;
}

// ── main ───────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    std::vector<std::string> args(argv + 1, argv + argc);

    if (hasFlag(args, "--detect"))  return runDetect(args);
    if (hasFlag(args, "--conform")) return runConform(args);

    std::cerr
        << "Usage:\n"
        << "  drape-cli --detect  --nif <path> --ref-nif <path>\n"
        << "            [--d-tight F] [--d-rigid F]\n\n"
        << "  drape-cli --conform --nif <path> --ref-nif <path>\n"
        << "            --ref-osd <path> --output-osd <path>\n"
        << "            [--shapes name1,name2] [--d-tight F] [--d-rigid F]\n";
    return 1;
}
