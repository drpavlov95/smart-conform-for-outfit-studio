/*
 * algo.cpp — drape-algo.dll
 *
 * Heavy algorithm layer: nifly NIF I/O, DiffData OSD I/O, DrapeConformer.
 * Loaded lazily by version.dll only when the user opens the Drape dialog.
 * Never present at BodySlide/OutfitStudio startup — avoids static-init crashes.
 *
 * Exports exactly one symbol: DrapeGetApi().
 *
 * Workflow (race-free): the user runs Conform All + Save in OutfitStudio FIRST.
 * Drape then filters the freshly-saved on-disk OSD by cloth zone and the user
 * reloads the project to view. We never trigger conform ourselves (the conform
 * result lives in OS memory until save, so editing disk before save would touch
 * stale data and be overwritten).
 */

#include "../proxy/drape_iface.h"
#include "../cli/DrapeConformer.h"
#include "DiffData.h"
#include "NifFile.hpp"

#include <windows.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace nifly;

// ── .osp model (authoritative dataName → shape → osd file) ─────────────

struct DataRef {
    std::string shape;      // <Data target="...">
    std::string dataName;   // <Data name="..."> (OSD map key); defaults to slider name
    std::string osdFile;    // text content: the .osd file basename
};

struct SetInfo {
    std::string name;
    std::string dataFolder;
    std::string sourceFile;
    std::vector<DataRef> data;
};

// ── Reference-body name matching (robust; never "largest shape") ───────

static bool isBodyShapeName(const std::string& t) {
    static const char* K[] = {
        "CBBE", "3BBB Body", "3BA", "HIMBO - Body HP", "HIMBO",
        "BHUNP Body", "BHUNP", "UNP Body", "UUNP Body", "UNP",
        "TBD Body", "TBD", "SAM Body", "SAM", "BaseShape", "Body", nullptr
    };
    for (int i = 0; K[i]; ++i)
        if (t.find(K[i]) != std::string::npos) return true;
    return false;
}

// ── XML line helpers (BodySlide writes compact one-line <Data> elements) ─

static std::string xmlText(const std::string& line, char open, char close) {
    auto p = line.find(open);
    if (p == std::string::npos) return {};
    auto q = line.find(close, p + 1);
    return q != std::string::npos ? line.substr(p + 1, q - p - 1) : std::string{};
}

static std::string xmlTag(const std::string& line, const char* open, const char* close) {
    auto p = line.find(open);
    if (p == std::string::npos) return {};
    p += strlen(open);
    auto q = line.find(close, p);
    return q != std::string::npos ? line.substr(p, q - p) : std::string{};
}

static std::string xmlAttr(const std::string& line, const char* attr) {
    std::string pat = std::string(attr) + "=\"";
    auto p = line.find(pat);
    if (p == std::string::npos) return {};
    p += pat.size();
    auto q = line.find('"', p);
    return q != std::string::npos ? line.substr(p, q - p) : std::string{};
}

static std::string baseName(std::string s) {
    auto bsl = s.find_last_of("\\/");
    if (bsl != std::string::npos) s = s.substr(bsl + 1);
    return s;
}

// <Data> text is "File.osd" or "File.osd\EntryName" (the entry equals the
// dataName). Extract just the .osd filename (cut at ".osd", strip any path).
static std::string osdFileFromText(std::string text) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    auto pos = lower.find(".osd");
    if (pos != std::string::npos) text = text.substr(0, pos + 4);
    return baseName(text);
}

static std::vector<SetInfo> parseOspSets(const char* ospPath) {
    std::vector<SetInfo> sets;
    std::ifstream f(ospPath);
    std::string line;
    SetInfo cur;
    std::string curSlider;
    bool inSet = false;

    while (std::getline(f, line)) {
        if (!inSet) {
            if (line.find("<SliderSet") != std::string::npos) {
                cur = {};
                cur.name = xmlAttr(line, "name");
                curSlider.clear();
                inSet = true;
            }
            continue;
        }

        if (cur.dataFolder.empty())
            cur.dataFolder = xmlTag(line, "<DataFolder>", "</DataFolder>");
        if (cur.sourceFile.empty())
            cur.sourceFile = xmlTag(line, "<SourceFile>", "</SourceFile>");

        if (line.find("<Slider ") != std::string::npos)
            curSlider = xmlAttr(line, "name");

        if (line.find("<Data ") != std::string::npos ||
            line.find("<Data\t") != std::string::npos) {
            // Only LOCAL data is the outfit's own conform output that we filter.
            // Non-local entries are shared reference-body source data — never touch.
            if (line.find("local=\"true\"") == std::string::npos)
                continue;

            DataRef ref;
            ref.shape    = xmlAttr(line, "target");
            ref.dataName = xmlAttr(line, "name");
            if (ref.dataName.empty()) ref.dataName = curSlider;
            // text between the opening tag's '>' and the closing '<'
            auto gt = line.find('>');
            if (gt != std::string::npos) {
                auto lt = line.find('<', gt + 1);
                if (lt != std::string::npos)
                    ref.osdFile = osdFileFromText(line.substr(gt + 1, lt - gt - 1));
            }
            if (!ref.shape.empty() && !ref.dataName.empty() && !ref.osdFile.empty())
                cur.data.push_back(std::move(ref));
        }

        if (line.find("</SliderSet>") != std::string::npos) {
            sets.push_back(std::move(cur));
            cur = {}; curSlider.clear(); inSet = false;
        }
    }
    return sets;
}

// ── NIF helpers ───────────────────────────────────────────────────────

static std::vector<Vec3> shapeVerts(NifFile& nif, NiShape* s) {
    std::vector<Vector3> v; nif.GetVertsForShape(s, v);
    std::vector<Vec3> out; out.reserve(v.size());
    for (auto& x : v) out.push_back({x.x, x.y, x.z});
    return out;
}

static TriList shapeTris(NiShape* s) {
    std::vector<Triangle> t; s->GetTriangles(t);
    TriList out; out.reserve(t.size());
    for (auto& x : t) out.push_back({x.p1, x.p2, x.p3});
    return out;
}

// Find an embedded reference body shape by known names. No "largest" fallback:
// without a real body we cannot measure distances, so we skip instead of guessing.
static NiShape* findRefBodyShape(NifFile& nif) {
    for (auto* s : nif.GetShapes())
        if (s && isBodyShapeName(s->name.get())) return s;
    return nullptr;
}

static std::string osdCandidate(const std::string& bsDir, const std::string& ospPath,
                                const SetInfo& set, const std::string& osdFile) {
    // Prefer ShapeData/<DataFolder>/<file>; fall back to the slider-set dir.
    std::string a = bsDir + "ShapeData\\" + set.dataFolder + "\\" + osdFile;
    if (fs::exists(a)) return a;
    std::string b = fs::path(ospPath).parent_path().string() + "\\" + osdFile;
    if (fs::exists(b)) return b;
    return a;  // report the primary path in errors
}

// ── Shared per-set context ─────────────────────────────────────────────

struct SetGeometry {
    NifFile nif;
    std::vector<Vec3> bodyVerts;
    TriList bodyTris;
    std::unordered_map<std::string, NiShape*> shapeByName;
    bool ok = false;
    std::string refName;
};

static SetGeometry loadSetGeometry(const std::string& bsDir, const SetInfo& set,
                                   const std::function<void(const std::string&)>& log) {
    SetGeometry g;
    std::string nifPath = bsDir + "ShapeData\\" + set.dataFolder + "\\" + set.sourceFile;
    if (!fs::exists(nifPath) || g.nif.Load(nifPath) != 0) {
        log("  SKIP " + set.name + " — NIF not found: " + nifPath);
        return g;
    }
    NiShape* refShape = findRefBodyShape(g.nif);
    if (!refShape) {
        log("  SKIP " + set.name + " — no embedded reference body shape "
            "(known body name). Drape needs the body in the project to measure "
            "distances.");
        return g;
    }
    g.refName   = refShape->name.get();
    g.bodyVerts = shapeVerts(g.nif, refShape);
    g.bodyTris  = shapeTris(refShape);
    for (auto* s : g.nif.GetShapes())
        if (s) g.shapeByName[s->name.get()] = s;
    g.ok = true;
    return g;
}

// ── Detect ────────────────────────────────────────────────────────────

static int drapeDetect(
    const char*  bsDir,
    const char*  ospPath,
    const int*   selectedSets,
    int          nSets,
    float        dTight,
    float        dRigid,
    DrapeLogCb   logCb,
    void*        logCtx,
    DrapeZoneCb  zoneCb,
    void*        zoneCtx)
{
    auto sets = parseOspSets(ospPath);
    DrapeOptions opts; opts.tightDistance = dTight; opts.looseDistance = dRigid;
    DrapeConformer dc(opts);
    auto log = [&](const std::string& m){ if (logCb) logCb(logCtx, m.c_str()); };

    int processed = 0;
    for (int si = 0; si < nSets; ++si) {
        int idx = selectedSets[si];
        if (idx < 0 || idx >= (int)sets.size()) continue;
        const SetInfo& set = sets[idx];

        log("Detecting: " + set.name);
        SetGeometry g = loadSetGeometry(bsDir, set, log);
        if (!g.ok) continue;
        log("  reference body: " + g.refName);

        // Unique conformed shapes = distinct DataRef targets (excludes the body).
        std::vector<std::string> shapes;
        for (const auto& d : set.data)
            if (d.shape != g.refName &&
                std::find(shapes.begin(), shapes.end(), d.shape) == shapes.end())
                shapes.push_back(d.shape);

        int tight = 0, loose = 0, farV = 0;   // 'far' is a legacy Windows macro
        for (const auto& sName : shapes) {
            auto it = g.shapeByName.find(sName);
            if (it == g.shapeByName.end() || !it->second) continue;
            auto sr = dc.detectShape(shapeVerts(g.nif, it->second),
                                     g.bodyVerts, g.bodyTris, sName);
            tight += sr.tightVerts;
            loose += sr.looseVerts;
            farV  += sr.farVerts;
        }

        int total = tight + loose + farV;
        const char* zone = (farV > tight && farV > loose) ? "Far" :
                           (loose > tight)                ? "Loose" : "SkinTight";
        log(std::string("  ") + zone +
            "  tight=" + std::to_string(tight) +
            "  loose=" + std::to_string(loose) +
            "  far="   + std::to_string(farV));
        if (zoneCb) zoneCb(zoneCtx, si, zone, tight, loose, farV, total);
        ++processed;
    }
    return processed;
}

// ── Conform (filter the saved OSD) ─────────────────────────────────────

static int drapeConform(
    void*        /*osHwnd*/,
    unsigned int /*conformAllId*/,
    const char*  bsDir,
    const char*  ospPath,
    const int*   selectedSets,
    int          nSets,
    float        dTight,
    float        dRigid,
    DrapeLogCb   logCb,
    void*        logCtx)
{
    auto sets = parseOspSets(ospPath);
    DrapeOptions opts; opts.tightDistance = dTight; opts.looseDistance = dRigid;
    DrapeConformer dc(opts);
    auto log = [&](const std::string& m){ if (logCb) logCb(logCtx, m.c_str()); };

    log("Drape filters the SAVED conform result. Make sure you ran "
        "Conform All + Save first; reload the project afterwards.");

    int done = 0;
    for (int si = 0; si < nSets; ++si) {
        int idx = selectedSets[si];
        if (idx < 0 || idx >= (int)sets.size()) continue;
        const SetInfo& set = sets[idx];

        log("Filtering: " + set.name);
        SetGeometry g = loadSetGeometry(bsDir, set, log);
        if (!g.ok) continue;
        log("  reference body: " + g.refName);

        // Group DataRefs by their on-disk OSD path → {dataName → shape}.
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> byFile;
        for (const auto& d : set.data) {
            if (d.shape == g.refName) continue;  // body has no cloth diffs
            std::string path = osdCandidate(bsDir, ospPath, set, d.osdFile);
            byFile[path][d.dataName] = d.shape;
        }

        // Cache per-shape weights (geometry only — identical across that shape's sliders).
        std::unordered_map<std::string, std::vector<float>> weightCache;
        auto weightsFor = [&](const std::string& shapeName) -> const std::vector<float>* {
            auto wIt = weightCache.find(shapeName);
            if (wIt != weightCache.end()) return &wIt->second;
            auto sIt = g.shapeByName.find(shapeName);
            if (sIt == g.shapeByName.end() || !sIt->second) return nullptr;
            auto w = dc.buildWeights(shapeVerts(g.nif, sIt->second), g.bodyVerts, g.bodyTris);
            return &(weightCache[shapeName] = std::move(w));
        };

        int setEntries = 0;
        for (auto& [osdPath, keyToShape] : byFile) {
            if (!fs::exists(osdPath)) {
                log("  SKIP missing OSD: " + osdPath);
                continue;
            }
            OSDataFile osd;
            if (!osd.Read(osdPath)) { log("  SKIP unreadable OSD: " + osdPath); continue; }

            bool modified = false;
            for (auto& [key, tdiffs] : osd.GetDataDiffs()) {
                if (!tdiffs) continue;
                auto kit = keyToShape.find(key);
                if (kit == keyToShape.end()) continue;   // exact match only — no prefixes
                const std::string& shapeName = kit->second;

                const std::vector<float>* w = weightsFor(shapeName);
                if (!w) continue;

                bool changed = false;
                for (auto it = tdiffs->begin(); it != tdiffs->end(); ) {
                    uint16_t vi = it->first;
                    float wt = (vi < w->size()) ? (*w)[vi] : 1.0f;
                    it->second *= wt;
                    if (it->second.IsZero(true)) { it = tdiffs->erase(it); }
                    else { ++it; }
                    changed = true;
                }
                if (changed) { modified = true; ++setEntries; }
            }

            if (modified) {
                if (osd.Write(osdPath)) log("  Written: " + baseName(osdPath));
                else                    log("  ERROR writing: " + osdPath);
            }
        }
        log("  " + std::to_string(setEntries) + " slider/shape entries filtered.");
        ++done;
    }

    log(done > 0
        ? "Done — " + std::to_string(done) + " set(s) filtered. "
          "Reload the project in OutfitStudio to see the result."
        : "Nothing was filtered.");
    return done;
}

// ── Export ────────────────────────────────────────────────────────────

extern "C" __declspec(dllexport) int DrapeGetApi(DrapeAlgoApi* api) {
    if (!api) return -1;
    api->version = DRAPE_ALGO_API_VERSION;
    api->detect  = drapeDetect;
    api->conform = drapeConform;
    return 0;
}
