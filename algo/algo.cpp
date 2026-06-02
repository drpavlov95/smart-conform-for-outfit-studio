/*
 * algo.cpp — drape-algo.dll
 *
 * Heavy algorithm layer: nifly NIF I/O, DiffData OSD I/O, DrapeConformer.
 * Loaded lazily by version.dll only when the user opens the Drape dialog.
 * Never present at BodySlide/OutfitStudio startup — avoids static-init crashes.
 *
 * Exports exactly one symbol: DrapeGetApi().
 */

#include "../proxy/drape_iface.h"
#include "../cli/DrapeConformer.h"
#include "DiffData.h"
#include "NifFile.hpp"

#include <windows.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace nifly;

// ── OSP parsing ───────────────────────────────────────────────────────

struct SetInfo {
    std::string name;
    std::string dataFolder;
    std::string sourceFile;
    std::string refBodyShape;
    std::set<std::string> osdFiles;
};

static bool isBodyShape(const std::string& t) {
    static const char* K[] = {
        "CBBE", "HIMBO - Body HP", "BHUNP Body", "UNP Body",
        "UUNP Body", "3BA", "TBD Body", "SAM Body", "CBBE Body", nullptr
    };
    for (int i = 0; K[i]; ++i)
        if (t == K[i]) return true;
    return false;
}

static std::string xmlText(const std::string& line,
                            const char* open, const char* close) {
    auto p = line.find(open);
    if (p == std::string::npos) return {};
    p += strlen(open);
    auto q = line.find(close, p);
    return q != std::string::npos ? line.substr(p, q - p) : std::string{};
}

static std::vector<SetInfo> parseOspSets(const char* ospPath) {
    std::vector<SetInfo> sets;
    std::ifstream f(ospPath);
    std::string line;
    SetInfo cur;
    bool inSet = false;

    while (std::getline(f, line)) {
        if (!inSet) {
            auto p = line.find("<SliderSet name=\"");
            if (p != std::string::npos) {
                p += 17;
                auto q = line.find('"', p);
                if (q != std::string::npos) { cur.name = line.substr(p, q-p); inSet = true; }
            }
        } else {
            if (cur.dataFolder.empty()) cur.dataFolder = xmlText(line, "<DataFolder>", "</DataFolder>");
            if (cur.sourceFile.empty()) cur.sourceFile = xmlText(line, "<SourceFile>",  "</SourceFile>");

            // Detect embedded reference body shape
            if (cur.refBodyShape.empty()) {
                auto p = line.find("<Shape target=\"");
                if (p != std::string::npos) {
                    p += 15;
                    auto q = line.find('"', p);
                    if (q != std::string::npos) {
                        std::string tgt = line.substr(p, q-p);
                        if (isBodyShape(tgt)) cur.refBodyShape = tgt;
                    }
                }
            }

            // Collect OSD basenames: <Data ... local="true">file.osd\entry</Data>
            if (line.find("local=\"true\"") != std::string::npos) {
                auto p = line.find('>');
                if (p != std::string::npos) {
                    auto q = line.find('<', ++p);
                    if (q != std::string::npos) {
                        std::string val = line.substr(p, q-p);
                        auto bsl = val.find('\\');
                        if (bsl != std::string::npos)
                            cur.osdFiles.insert(val.substr(0, bsl));
                    }
                }
            }

            if (line.find("</SliderSet>") != std::string::npos) {
                sets.push_back(std::move(cur));
                cur = {}; inSet = false;
            }
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

static std::vector<std::array<uint16_t,3>> shapeTris(NiShape* s) {
    std::vector<Triangle> t; s->GetTriangles(t);
    std::vector<std::array<uint16_t,3>> out; out.reserve(t.size());
    for (auto& x : t) out.push_back({x.p1, x.p2, x.p3});
    return out;
}

// Returns the body-copy shape, or the largest shape as fallback.
static NiShape* findRefShape(NifFile& nif, const std::string& refName) {
    for (auto* s : nif.GetShapes())
        if (s && s->name.get() == refName) return s;
    // Fallback: largest shape
    NiShape* best = nullptr; int maxV = 0;
    for (auto* s : nif.GetShapes()) {
        if (!s) continue;
        std::vector<Vector3> v; nif.GetVertsForShape(s, v);
        if ((int)v.size() > maxV) { maxV = (int)v.size(); best = s; }
    }
    return best;
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

    DrapeOptions opts; opts.d_tight = dTight; opts.d_rigid = dRigid;
    DrapeConformer dc(opts);

    auto log = [&](const std::string& msg) {
        if (logCb) logCb(logCtx, msg.c_str());
    };

    int processed = 0;
    for (int si = 0; si < nSets; ++si) {
        int idx = selectedSets[si];
        if (idx < 0 || idx >= (int)sets.size()) continue;
        const SetInfo& set = sets[idx];

        std::string nifPath = std::string(bsDir) + "ShapeData\\" +
                              set.dataFolder + "\\" + set.sourceFile;

        log("Detecting: " + set.name);

        NifFile nif;
        if (!fs::exists(nifPath) || nif.Load(nifPath) != 0) {
            log("  SKIP — NIF not found: " + nifPath);
            continue;
        }

        NiShape* refShape = findRefShape(nif, set.refBodyShape);
        if (!refShape) { log("  SKIP — no shapes in NIF"); continue; }

        std::string refName = refShape->name.get();
        auto refVerts = shapeVerts(nif, refShape);

        int tight = 0, loose = 0, rigid = 0;
        for (auto* s : nif.GetShapes()) {
            if (!s || s->name.get() == refName) continue;
            auto sr = dc.detectShape(shapeVerts(nif, s), refVerts,
                                     shapeTris(s), s->name.get());
            tight += sr.tightVerts;
            loose += sr.looseVerts;
            rigid += sr.rigidVerts;
        }

        int total = tight + loose + rigid;
        const char* zone = (rigid > tight && rigid > loose) ? "Rigid" :
                           (loose > tight)                  ? "Loose" : "SkinTight";

        log(std::string("  ") + zone +
            "  tight=" + std::to_string(tight) +
            "  loose=" + std::to_string(loose) +
            "  rigid=" + std::to_string(rigid));

        if (zoneCb) zoneCb(zoneCtx, si, zone, tight, loose, rigid, total);
        ++processed;
    }
    return processed;
}

// ── Conform ───────────────────────────────────────────────────────────

static int drapeConform(
    void*        osHwnd,
    unsigned int conformAllId,
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

    auto log = [&](const std::string& msg) {
        if (logCb) logCb(logCtx, msg.c_str());
    };

    // ── Step 1: trigger vanilla Conform All (synchronous) ──────────────
    // SendMessage from worker thread → blocked until main thread processes it.
    // The modal Drape dialog pumps all messages, so Conform All will execute.
    log("Running Conform All...");
    SendMessage(static_cast<HWND>(osHwnd),
                WM_COMMAND, MAKEWPARAM(conformAllId, 0), 0);
    log("Conform All done. Applying Drape zone filter...");

    // ── Step 2: filter OSD files by cloth zone ─────────────────────────
    DrapeOptions opts; opts.d_tight = dTight; opts.d_rigid = dRigid;
    DrapeConformer dc(opts);

    int done = 0;
    for (int si = 0; si < nSets; ++si) {
        int idx = selectedSets[si];
        if (idx < 0 || idx >= (int)sets.size()) continue;
        const SetInfo& set = sets[idx];

        std::string nifPath = std::string(bsDir) + "ShapeData\\" +
                              set.dataFolder + "\\" + set.sourceFile;

        NifFile nif;
        if (!fs::exists(nifPath) || nif.Load(nifPath) != 0) {
            log("SKIP " + set.name + " — NIF not found");
            continue;
        }

        NiShape* refShape = findRefShape(nif, set.refBodyShape);
        if (!refShape) { log("SKIP " + set.name + " — no shapes"); continue; }

        std::string refName = refShape->name.get();
        auto refVerts = shapeVerts(nif, refShape);
        log("Filtering: " + set.name + "  (ref: " + refName + ")");

        for (const std::string& osdFile : set.osdFiles) {
            std::string osdPath = std::string(bsDir) + "ShapeData\\" +
                                  set.dataFolder + "\\" + osdFile;
            if (!fs::exists(osdPath)) continue;

            OSDataFile osd;
            if (!osd.Read(osdPath)) {
                log("  SKIP " + osdFile + " — read failed");
                continue;
            }

            bool modified = false;
            for (auto* shape : nif.GetShapes()) {
                if (!shape || shape->name.get() == refName) continue;
                std::string shapeName = shape->name.get();
                auto outVerts = shapeVerts(nif, shape);
                auto outTris  = shapeTris(shape);

                for (auto& [key, tdiffs] : osd.GetDataDiffs()) {
                    if (!tdiffs) continue;
                    // Match entries keyed as <shapeName><sliderName>
                    if (key.size() < shapeName.size()) continue;
                    if (key.substr(0, shapeName.size()) != shapeName) continue;

                    VertexDiffs vd;
                    for (auto& [vidx, v] : *tdiffs)
                        vd[vidx] = {v.x, v.y, v.z};

                    auto filtered = dc.filterDiffs(outVerts, refVerts, outTris, vd);

                    TargetDataDiffs tdd;
                    for (auto& [vidx, v] : filtered)
                        tdd[vidx] = {v.x, v.y, v.z};

                    osd.SetDataDiff(key, tdd);
                    modified = true;
                }
            }

            if (modified) {
                if (osd.Write(osdPath)) log("  Written: " + osdFile);
                else                    log("  ERROR writing: " + osdFile);
            }
        }
        ++done;
    }

    log(done > 0
        ? "Done — " + std::to_string(done) + " set(s) filtered.\n"
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
