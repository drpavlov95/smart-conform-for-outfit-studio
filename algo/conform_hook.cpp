/*
 * conform_hook.cpp — in-memory "Smart Conform" via function hooking.
 *
 * Hooks Automorph::GenerateResultDiff in OutfitStudio x64.exe. After the vanilla
 * conform computes a slider's result diff for a shape, we scale each vertex's
 * diff by its cloth-aware weight (distance to the body surface) using the
 * engine's own Automorph::ZeroVertDiff(shape, slider, nullptr, &mask).
 *
 * Result: the normal "Conform All" produces a cloth-aware result in memory and
 * the view refreshes itself — feels like a native, smarter conform. No disk
 * round-trip, no save/reload.
 *
 * Geometry for the weights is loaded from the open project's NIF on disk (same
 * vertices/order as the in-memory shape), cached per shape.
 *
 * ABI note: we read const std::string* args and pass a std::unordered_map by
 * pointer to the host. This relies on host + DLL sharing the MSVC STL ABI
 * (true for all VS2015–2022 builds). Both are MSVC, so layouts match.
 */

#include "conform_offsets.h"
#include "../cli/DrapeConformer.h"
#include "NifFile.hpp"

#include <MinHook.h>
#include <windows.h>

#include <cstdio>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace nifly;

// ── Debug log (shared file) ────────────────────────────────────────────
static void HLog(const char* fmt, ...) {
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    char line[1200];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02d:%02d:%02d.%03d] HOOK %s\r\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    char path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, path)) return;
    strcat_s(path, MAX_PATH, "drape_debug.log");
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD w; WriteFile(h, line, (DWORD)n, &w, nullptr); CloseHandle(h);
}

// ── Hooked function signatures ─────────────────────────────────────────
typedef void (*PfnGenResult)(
    void* self,
    const std::string* shapeName,
    const std::string* sliderName,
    const std::string* refDataName,
    bool transformResults, int maxResults, bool noSqueeze, bool solidMode,
    bool axisX, bool axisY, bool axisZ);

// DiffDataSets::ZeroVertDiff(set, target, vertSet, mask) — scales each diff
// vertex by mask[vert]; erases verts with weight 0 or absent from mask.
typedef void (*PfnDiffZeroVert)(
    void* diffDataSets,
    const std::string* set,
    const std::string* target,
    void* vertSet,   // std::vector<uint16_t>*  (we pass nullptr)
    void* mask);     // std::unordered_map<uint16_t,float>*

static PfnGenResult    g_origGenResult = nullptr;
static PfnDiffZeroVert g_diffZeroVert  = nullptr;

// ── Config from version.dll ────────────────────────────────────────────
static HWND        g_osHwnd = nullptr;
static std::string g_bsDir;
static DrapeOptions g_opts;        // tight/loose distances
// Disabled by default so the vanilla Conform All / Conform Selected pass through
// UNCHANGED. version.dll flips this on only for the duration of a Smart Conform.
static bool        g_enabled = false;

// ── Per-shape weight cache (rebuilt when the open project changes) ─────
static std::mutex g_cacheMx;
static std::string g_projectKey;   // current resolved project name
static std::unordered_map<std::string, std::vector<float>> g_shapeWeights;
// Pre-built masks (vert -> weight) reused across a shape's sliders.
static std::unordered_map<std::string, std::unordered_map<uint16_t, float>> g_shapeMask;

// ── Compact project resolver (title -> .osp -> NIF) ────────────────────

static std::string ProjFromTitle() {
    if (!g_osHwnd) return {};
    char buf[512] = {}; GetWindowTextA(g_osHwnd, buf, sizeof(buf));
    std::string t(buf);
    auto p = t.rfind(" - Outfit Studio");
    if (p == std::string::npos) return {};
    std::string n = t.substr(0, p);
    if (!n.empty() && n.back() == '*') n.pop_back();
    return n;
}

static std::string lower(std::string s){ for(auto&c:s)c=(char)tolower((unsigned char)c); return s; }

static bool isBodyName(const std::string& t) {
    static const char* K[] = {"CBBE","3BBB Body","3BA","HIMBO - Body HP","HIMBO",
        "BHUNP Body","BHUNP","UNP Body","UUNP Body","UNP","TBD Body","TBD",
        "SAM Body","SAM","BaseShape","Body",nullptr};
    for (int i=0;K[i];++i) if (t.find(K[i])!=std::string::npos) return true;
    return false;
}

// Resolve the open project's NIF path (SourceFile under its DataFolder).
static std::string ResolveNifPath(const std::string& proj) {
    fs::path ss = fs::path(g_bsDir) / "SliderSets";
    std::error_code ec;
    if (!fs::is_directory(ss, ec)) return {};
    std::string projL = lower(proj);
    for (auto& e : fs::directory_iterator(ss, ec)) {
        if (e.path().extension() != ".osp") continue;
        bool nameMatch = (lower(e.path().stem().string()) == projL);
        std::ifstream f(e.path()); std::string line, dataFolder, sourceFile;
        bool inThis = nameMatch;
        while (std::getline(f, line)) {
            auto sp = line.find("<SliderSet name=\"");
            if (sp != std::string::npos) {
                sp += 17; auto q = line.find('"', sp);
                inThis = (q!=std::string::npos) && (lower(line.substr(sp,q-sp))==projL);
            }
            if (!inThis) continue;
            auto getTag = [&](const char* o, const char* c)->std::string{
                auto a=line.find(o); if(a==std::string::npos) return {};
                a+=strlen(o); auto b=line.find(c,a);
                return b!=std::string::npos? line.substr(a,b-a):std::string{}; };
            if (dataFolder.empty()) dataFolder = getTag("<DataFolder>","</DataFolder>");
            if (sourceFile.empty()) sourceFile = getTag("<SourceFile>","</SourceFile>");
            if (!dataFolder.empty() && !sourceFile.empty())
                return g_bsDir + "ShapeData\\" + dataFolder + "\\" + sourceFile;
        }
    }
    return {};
}

// (Re)build per-shape weights for the currently open project. Caller holds lock.
static void RebuildCacheLocked() {
    g_shapeWeights.clear();
    g_shapeMask.clear();

    std::string proj = ProjFromTitle();
    g_projectKey = proj;
    if (proj.empty()) { HLog("cache: no project in title"); return; }

    std::string nifPath = ResolveNifPath(proj);
    if (nifPath.empty() || !fs::exists(nifPath)) {
        HLog("cache: NIF not found for '%s'", proj.c_str());
        return;
    }
    NifFile nif;
    if (nif.Load(nifPath) != 0) { HLog("cache: NIF load failed"); return; }

    // reference body shape
    NiShape* body = nullptr;
    for (auto* s : nif.GetShapes()) if (s && isBodyName(s->name.get())) { body = s; break; }
    if (!body) { HLog("cache: no body shape in project NIF"); return; }

    std::vector<Vector3> bv; nif.GetVertsForShape(body, bv);
    std::vector<Vec3> bodyVerts; bodyVerts.reserve(bv.size());
    for (auto& v : bv) bodyVerts.push_back({v.x,v.y,v.z});
    std::vector<Triangle> bt; body->GetTriangles(bt);
    TriList bodyTris; bodyTris.reserve(bt.size());
    for (auto& t : bt) bodyTris.push_back({t.p1,t.p2,t.p3});

    DrapeConformer dc(g_opts);
    std::string bodyName = body->name.get();
    int built = 0;
    for (auto* s : nif.GetShapes()) {
        if (!s || s->name.get() == bodyName) continue;
        std::string name = s->name.get();
        std::vector<Vector3> ov; nif.GetVertsForShape(s, ov);
        std::vector<Vec3> outVerts; outVerts.reserve(ov.size());
        for (auto& v : ov) outVerts.push_back({v.x,v.y,v.z});

        auto w = dc.buildWeights(outVerts, bodyVerts, bodyTris);
        // Full mask: every vertex must be present (absent => ZeroVertDiff erases).
        std::unordered_map<uint16_t,float> mask;
        mask.reserve(w.size());
        for (size_t i = 0; i < w.size() && i <= 0xFFFF; ++i)
            mask[(uint16_t)i] = w[i];
        g_shapeWeights[name] = std::move(w);
        g_shapeMask[name]    = std::move(mask);
        ++built;
    }
    HLog("cache: built weights for %d shape(s), project '%s'", built, proj.c_str());
}

// ── Detour ─────────────────────────────────────────────────────────────

static void Detour_GenResult(
    void* self,
    const std::string* shapeName,
    const std::string* sliderName,
    const std::string* refDataName,
    bool transformResults, int maxResults, bool noSqueeze, bool solidMode,
    bool axisX, bool axisY, bool axisZ)
{
    // 1) Let the vanilla conform compute the slider's result diff.
    g_origGenResult(self, shapeName, sliderName, refDataName,
                    transformResults, maxResults, noSqueeze, solidMode,
                    axisX, axisY, axisZ);

    if (!g_enabled || !g_diffZeroVert || !self || !shapeName || !sliderName) return;

    std::lock_guard<std::mutex> lk(g_cacheMx);

    // Rebuild cache if the open project changed.
    std::string proj = ProjFromTitle();
    if (proj != g_projectKey) RebuildCacheLocked();

    auto it = g_shapeMask.find(*shapeName);
    if (it == g_shapeMask.end() || it->second.empty()) return;  // body or unknown

    // 2) Scale the just-generated result diff per vertex, in memory, via the
    //    engine's own DiffDataSets::ZeroVertDiff on morpher.resultDiffData.
    //    Result is keyed by dataName = shapeName + sliderName; build it with our
    //    own CRT (never receive a host-allocated std::string -> no /MT-/MD free).
    std::string setName = *shapeName + *sliderName;
    void* resultDiffData = reinterpret_cast<char*>(self) + drape_off::kResultDiffDataOffset;
    g_diffZeroVert(resultDiffData, &setName, shapeName, nullptr, &it->second);
}

// ── Public API (called by version.dll after injection) ─────────────────

extern "C" __declspec(dllexport)
int DrapeInstallConformHook(void* osHwnd, const char* bsDir,
                            float tightDistance, float looseDistance) {
    g_osHwnd = static_cast<HWND>(osHwnd);
    if (bsDir) g_bsDir = bsDir;
    g_opts.tightDistance = tightDistance;
    g_opts.looseDistance = looseDistance;

    if (!drape_off::Configured()) {
        HLog("install: offsets not configured for this build — hook disabled");
        return -2;
    }

    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    HLog("install: module base=0x%p (expected imagebase 0x%llx)",
         (void*)base, (unsigned long long)drape_off::kImageBase);

    void* pGen     = reinterpret_cast<void*>(base + drape_off::kGenerateResultDiff);
    g_diffZeroVert = reinterpret_cast<PfnDiffZeroVert>(base + drape_off::kDiffSetsZeroVertDiff);

    if (MH_Initialize() != MH_OK) { HLog("install: MH_Initialize failed"); return -3; }
    if (MH_CreateHook(pGen, reinterpret_cast<void*>(&Detour_GenResult),
                      reinterpret_cast<void**>(&g_origGenResult)) != MH_OK) {
        HLog("install: MH_CreateHook failed"); return -4;
    }
    if (MH_EnableHook(pGen) != MH_OK) { HLog("install: MH_EnableHook failed"); return -5; }

    HLog("install: conform hook active (tight=%.2f loose=%.2f)",
         tightDistance, looseDistance);
    return 0;
}

extern "C" __declspec(dllexport) void DrapeSetHookEnabled(int enabled) {
    g_enabled = (enabled != 0);
}
