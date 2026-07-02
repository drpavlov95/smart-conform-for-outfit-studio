/*
 * conform_hook.cpp â€” in-memory "Smart Conform" via function hooking.
 *
 * Hooks Automorph::GenerateResultDiff in OutfitStudio x64.exe. After the vanilla
 * conform computes a slider's result diff for a shape, we scale each vertex's
 * diff by its cloth-aware weight (distance to the body surface) using the
 * engine's own Automorph::ZeroVertDiff(shape, slider, nullptr, &mask).
 *
 * Result: the normal "Conform All" produces a cloth-aware result in memory and
 * the view refreshes itself â€” feels like a native, smarter conform. No disk
 * round-trip, no save/reload.
 *
 * Geometry for the weights is loaded from the open project's NIF on disk (same
 * vertices/order as the in-memory shape), cached per shape.
 *
 * ABI note: we read const std::string* args and pass a std::unordered_map by
 * pointer to the host. This relies on host + DLL sharing the MSVC STL ABI
 * (true for all VS2015â€“2022 builds). Both are MSVC, so layouts match.
 */

#include "conform_offsets.h"
#include "../cli/DrapeConformer.h"
#include "../proxy/drape_iface.h"
#include "DiffData.h"
#include "NifFile.hpp"
#include "skin_arap.h"   // M1: As-Rigid-As-Possible deformation (libigl, isolated TU)

#include <MinHook.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <malloc.h>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace nifly;

// â”€â”€ Debug log (shared file) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
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

// â”€â”€ Hooked function signatures â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static std::string SafeFilePart(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char ch : s) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.')
            out.push_back((char)ch);
        else
            out.push_back('_');
    }
    if (out.empty())
        out = "unnamed";
    if (out.size() > 80)
        out.resize(80);
    return out;
}

static fs::path TempDrapePath(const std::string& fileName) {
    char path[MAX_PATH] = {};
    if (!GetTempPathA(MAX_PATH, path))
        return fs::path(fileName);
    return fs::path(path) / fileName;
}

typedef void (*PfnGenResult)(
    void* self,
    const std::string* shapeName,
    const std::string* sliderName,
    const std::string* refDataName,
    bool transformResults, int maxResults, bool noSqueeze, bool solidMode,
    bool axisX, bool axisY, bool axisZ);

// DiffDataSets::ZeroVertDiff(set, target, vertSet, mask) â€” scales each diff
// vertex by mask[vert]; erases verts with weight 0 or absent from mask.
typedef void (*PfnDiffZeroVert)(
    void* diffDataSets,
    const std::string* set,
    const std::string* target,
    void* vertSet,   // std::vector<uint16_t>*  (we pass nullptr)
    void* mask);     // std::unordered_map<uint16_t,float>*

typedef TargetDataDiffs* (*PfnDiffGetDiffSet)(
    void* diffDataSets,
    const std::string* targetDataName);

// Automorph host methods. These are safer than dereferencing resultDiffData
// directly because the host owns the DiffDataSets internals and all STL nodes.
typedef void (*PfnGetRawResultDiff)(
    void* self,
    const std::string* shapeName,
    const std::string* sliderName,
    TargetDataDiffs* outDiff);

typedef void (*PfnSetResultDiff)(
    void* self,
    const std::string* shapeName,
    const std::string* sliderName,
    const TargetDataDiffs* diff);

static PfnGenResult        g_origGenResult     = nullptr;
static PfnDiffZeroVert     g_diffZeroVert      = nullptr;
static PfnDiffGetDiffSet   g_diffGetDiffSet    = nullptr;
static PfnGetRawResultDiff g_getRawResultDiff  = nullptr;
static PfnSetResultDiff    g_setResultDiff     = nullptr;

// â”€â”€ Config from version.dll â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static HWND        g_osHwnd = nullptr;
static std::string g_bsDir;
static DrapeOptions g_opts;        // tight/loose distances
// Disabled by default so the vanilla Conform All / Conform Selected pass through
// UNCHANGED. version.dll flips this on only for the duration of a Smart Conform.
static std::atomic_bool g_enabled{false};
static std::atomic_bool g_hookInstalled{false};
// 0=Low (fewer passes, faster), 1=Medium (default), 2=High (extra passes).
static std::atomic_int  g_processingIntensity{1};

struct EdgeConstraint {
    uint16_t a = 0;
    uint16_t b = 0;
    float restLength = 0.0f;
};

struct BendingConstraint {
    uint16_t c = 0;
    uint16_t d = 0;
    float restLength = 0.0f;
};

struct VertexBoneProfile {
    float torso = 0.0f;
    float pelvis = 0.0f;
    float leg = 0.0f;
    float arm = 0.0f;
    float handFoot = 0.0f;
    float head = 0.0f;
    float custom = 0.0f;
    float maxWeight = 0.0f;
    int influences = 0;
};

struct BodyShell {
    int radialBins = 48;
    int zBins = 96;
    float minZ = 0.0f;
    float maxZ = 0.0f;
    float invDz = 1.0f;
    std::vector<float> baseR;
    std::vector<float> morphR;
    std::vector<uint8_t> occupied;
    bool valid = false;
};

// â”€â”€ 3D body collider: BVH over the morphed body triangles â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Real geometric collision (closest point on the actual body surface), the
// foundation that replaces the radial shell approximation. Built once per body
// morph (keyed by refDataName) and cached. Answers "where does the body actually
// push the cloth out?" using point-to-triangle closest-point queries + the real
// surface normal â€” works for belly, hips, sides, bust, and limbs alike.
struct BodyBVH {
    struct Node {
        Vec3 bmin{0.0f, 0.0f, 0.0f};
        Vec3 bmax{0.0f, 0.0f, 0.0f};
        int  left  = -1;   // child node index; -1 â‡’ leaf
        int  start = 0;    // [start,start+count) into triIndex for a leaf
        int  count = 0;
    };
    std::vector<Vec3> verts;                       // morphed body vertices
    std::vector<std::array<uint16_t, 3>> tris;     // triangle vertex indices
    std::vector<Vec3> centroid;                    // per-triangle centroid
    std::vector<Vec3> faceNormal;                  // per-triangle unit normal
    std::vector<int>  triIndex;                    // leaf triangle ordering
    std::vector<Node> nodes;
    bool valid = false;
};

// â”€â”€ Per-shape weight cache (rebuilt when the open project changes) â”€â”€â”€â”€â”€
static std::mutex g_cacheMx;
static std::string g_projectKey;   // current resolved project name
static std::unordered_map<std::string, std::vector<float>> g_shapeWeights;
static std::unordered_map<std::string, std::vector<VertexBoneProfile>> g_shapeBoneProfiles;
static std::unordered_map<std::string, std::vector<Vec3>> g_shapeVerts;
static std::unordered_map<std::string, std::vector<EdgeConstraint>> g_shapeEdges;
static std::unordered_map<std::string, std::vector<BendingConstraint>> g_shapeBending;
static std::unordered_map<std::string, int> g_shapeGeometryPresets;
// Inter-mesh layer order: higher = more outer (farther from body). Computed once
// per project in RebuildCacheLocked by sampling base-mesh overlaps. An outer mesh
// is pushed out to clear any inner (lower-layer) mesh during the solve.
static std::unordered_map<std::string, int> g_shapeLayer;
static std::unordered_map<std::string, std::unordered_set<std::string>> g_shapeOutersByInner;
// Per-shape triangles (base topology) â€” used to build a per-mesh collider BVH so an
// outer garment can be pushed off an inner one DURING the solve (like the body).
static std::unordered_map<std::string, std::vector<std::array<uint16_t, 3>>> g_shapeTris;
static std::unordered_map<std::string, BodyBVH> g_shapeBaseBVH;
// Cache of each mesh's SOLVED (physics-final) positions, keyed by slider THEN shape,
// so a later-solved mesh can collide against an earlier-solved one's actual draped
// shape (not just its conformed position). OutfitStudio iterates mesh-outer /
// slider-inner (all sliders for Cloak, then all for Clothes), so we must keep results
// for EVERY slider â€” a single-slider cache would be wiped before the inner mesh runs.
struct SolvedMesh { std::vector<Vec3> verts; BodyBVH bvh; int layer = 0; };
static std::unordered_map<std::string,
                          std::unordered_map<std::string, SolvedMesh>> g_solvedShapes; // [slider][shape]
static std::mutex g_sliderFilterMx;
static std::vector<std::string> g_sliderFilter;
static std::unordered_set<std::string> g_sliderFilterSkipLogged;
static int g_sliderFilterSkippedCalls = 0;
static std::vector<std::string> g_sliderFilterSkipExamples;
static std::vector<Vec3> g_refVerts;
static std::vector<std::array<uint16_t, 3>> g_refTris;  // body triangles (for BVH)
static BodyBVH g_refBodyBVH;                            // base-body BVH for base-order tests
static std::unordered_map<std::string, BodyShell> g_bodyShellCache;
static std::unordered_map<std::string, BodyBVH> g_bodyBVHCache;
// Pre-built masks (vert -> weight) reused across a shape's sliders.
static std::unordered_map<std::string, std::unordered_map<uint16_t, float>> g_shapeMask;
struct CachedPresetMask {
    int preset = DRAPE_PRESET_AUTO;
    std::unordered_map<uint16_t, float> mask;
};
static std::unordered_map<std::string, CachedPresetMask> g_shapePresetMaskCache;
static std::unordered_map<std::string, int> g_shapePresets;
static std::unordered_set<std::string> g_missingShapesLogged;
static std::unordered_set<std::string> g_appliedShapesLogged;
static std::unordered_set<std::string> g_staticShapesLogged;
static std::unordered_set<std::string> g_fallbackShapesLogged;
static float g_refCenterX = 0.0f;
static float g_refCenterY = 0.0f;
static float g_refMinZ = 0.0f;
static float g_refMaxZ = 0.0f;

// Directly dereferencing OutfitStudio's DiffDataSets from this injected DLL is
// not ABI-safe enough: the object owns MSVC STL containers allocated by the
// host process. Keep the advanced write-back disabled until it is routed through
// host Automorph methods with confirmed RVAs.
static constexpr bool kEnableDirectDiffRewrite = false;

// â”€â”€ WRITE-PATH DIAGNOSTIC â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// When true, ApplyStaticClothPass ignores the physics result and instead writes
// an UNMISTAKABLE deformation (drape-eligible verts shoved 15 units down). If the
// outfit visibly drops/stretches in OutfitStudio after Smart Conform, the
// SetResultDiff write path works and the problem is the algorithm. If nothing
// moves, the write path is dead and that is the real root cause.
// MUST be false for normal use.
static constexpr bool kDiagWritePathTest = false;
static constexpr float kDiagDropZ = 15.0f;

// â”€â”€ Compact project resolver (title -> .osp -> NIF) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

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

static float Clamp01(float v);
static bool NameHas(const std::string& lowerName, const char* token);
static int NormalizePreset(int preset);
static float PresetFollowWeight(int preset, float rawWeight);

static uint32_t EdgeKey(uint16_t a, uint16_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<uint32_t>(a) << 16) | b;
}

static std::vector<EdgeConstraint> BuildEdgeConstraints(NiShape* shape,
                                                        const std::vector<Vec3>& verts) {
    std::vector<EdgeConstraint> edges;
    if (!shape || verts.empty())
        return edges;

    std::vector<Triangle> tris;
    shape->GetTriangles(tris);
    std::unordered_set<uint32_t> seen;
    seen.reserve(tris.size() * 3);

    auto addEdge = [&](uint16_t a, uint16_t b) {
        if (a >= verts.size() || b >= verts.size() || a == b)
            return;

        uint32_t key = EdgeKey(a, b);
        if (!seen.insert(key).second)
            return;

        float dx = verts[a].x - verts[b].x;
        float dy = verts[a].y - verts[b].y;
        float dz = verts[a].z - verts[b].z;
        EdgeConstraint e;
        e.a = a;
        e.b = b;
        e.restLength = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (e.restLength > 1.0e-6f)
            edges.push_back(e);
    };

    for (const auto& t : tris) {
        addEdge(t.p1, t.p2);
        addEdge(t.p2, t.p3);
        addEdge(t.p3, t.p1);
    }

    return edges;
}

// Bending constraint: for each pair of triangles sharing edge (a,b),
// store the two opposite vertices (c, d) and their rest distance.
// Resists folding along the shared edge â€” gives cloth a smooth, fabric-like
// appearance instead of the "chain mail" look from stretch-only solvers.
static std::vector<BendingConstraint> BuildBendingConstraints(
    NiShape* shape, const std::vector<Vec3>& verts)
{
    std::vector<BendingConstraint> bending;
    if (!shape || verts.empty()) return bending;

    std::vector<Triangle> tris;
    shape->GetTriangles(tris);
    if (tris.empty()) return bending;

    // Map each edge key â†’ list of triangles that contain it (identified by
    // the opposite vertex â€” the vertex NOT on the shared edge).
    std::unordered_map<uint32_t, std::pair<int16_t, int16_t>> edgeToOpposite;
    edgeToOpposite.reserve(tris.size() * 3);

    auto processEdge = [&](uint16_t a, uint16_t b, uint16_t opposite) {
        if (a >= verts.size() || b >= verts.size() || opposite >= verts.size())
            return;
        uint32_t key = EdgeKey(a, b);
        auto it = edgeToOpposite.find(key);
        if (it == edgeToOpposite.end()) {
            edgeToOpposite[key] = {static_cast<int16_t>(opposite), -1};
        } else if (it->second.second < 0) {
            it->second.second = static_cast<int16_t>(opposite);
        }
        // >2 triangles per edge (non-manifold): ignore
    };

    for (const auto& t : tris) {
        processEdge(t.p1, t.p2, t.p3);
        processEdge(t.p2, t.p3, t.p1);
        processEdge(t.p3, t.p1, t.p2);
    }

    bending.reserve(edgeToOpposite.size() / 2);
    std::vector<uint32_t> edgeKeys;
    edgeKeys.reserve(edgeToOpposite.size());
    for (const auto& kv : edgeToOpposite)
        edgeKeys.push_back(kv.first);
    std::sort(edgeKeys.begin(), edgeKeys.end());
    for (uint32_t key : edgeKeys) {
        const auto& kv = *edgeToOpposite.find(key);
        if (kv.second.second < 0) continue;  // boundary edge (only one triangle)
        uint16_t c = static_cast<uint16_t>(kv.second.first);
        uint16_t d = static_cast<uint16_t>(kv.second.second);
        if (c >= verts.size() || d >= verts.size() || c == d) continue;

        float dx = verts[c].x - verts[d].x;
        float dy = verts[c].y - verts[d].y;
        float dz = verts[c].z - verts[d].z;
        float len = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (len > 1.0e-6f) {
            BendingConstraint bc;
            bc.c = c; bc.d = d; bc.restLength = len;
            bending.push_back(bc);
        }
    }
    return bending;
}

enum class BoneGroup {
    Torso,
    Pelvis,
    Leg,
    Arm,
    HandFoot,
    Head,
    Custom
};

static BoneGroup ClassifyBoneName(const std::string& boneName) {
    std::string b = lower(boneName);

    if (NameHas(b, "pelvis") || NameHas(b, "npc pelvis"))
        return BoneGroup::Pelvis;

    if (NameHas(b, "spine") || NameHas(b, "clavicle") ||
        NameHas(b, "breast") || NameHas(b, "belly") ||
        NameHas(b, "butt"))
        return BoneGroup::Torso;

    if (NameHas(b, "thigh") || NameHas(b, "calf") ||
        NameHas(b, "leg") || NameHas(b, "knee"))
        return BoneGroup::Leg;

    if (NameHas(b, "upperarm") || NameHas(b, "forearm") ||
        NameHas(b, "arm") || NameHas(b, "elbow"))
        return BoneGroup::Arm;

    if (NameHas(b, "hand") || NameHas(b, "finger") ||
        NameHas(b, "foot") || NameHas(b, "toe"))
        return BoneGroup::HandFoot;

    if (NameHas(b, "head") || NameHas(b, "neck"))
        return BoneGroup::Head;

    return BoneGroup::Custom;
}

static void AddBoneWeight(VertexBoneProfile& p, BoneGroup group, float w) {
    if (w <= 0.0f)
        return;

    switch (group) {
    case BoneGroup::Torso: p.torso += w; break;
    case BoneGroup::Pelvis: p.pelvis += w; break;
    case BoneGroup::Leg: p.leg += w; break;
    case BoneGroup::Arm: p.arm += w; break;
    case BoneGroup::HandFoot: p.handFoot += w; break;
    case BoneGroup::Head: p.head += w; break;
    case BoneGroup::Custom: p.custom += w; break;
    }

    p.maxWeight = std::max(p.maxWeight, w);
    ++p.influences;
}

static std::vector<VertexBoneProfile> BuildBoneProfiles(NifFile& nif,
                                                        NiShape* shape,
                                                        size_t vertexCount) {
    std::vector<VertexBoneProfile> profiles(vertexCount);
    if (!shape || vertexCount == 0)
        return profiles;

    std::vector<std::string> bones;
    uint32_t nBones = nif.GetShapeBoneList(shape, bones);
    if (nBones == 0 || bones.empty())
        return profiles;

    for (uint32_t bi = 0; bi < nBones && bi < bones.size(); ++bi) {
        BoneGroup group = ClassifyBoneName(bones[bi]);
        std::unordered_map<uint16_t, float> weights;
        nif.GetShapeBoneWeights(shape, bi, weights);
        for (const auto& kv : weights) {
            if (kv.first >= profiles.size())
                continue;
            AddBoneWeight(profiles[kv.first], group, Clamp01(kv.second));
        }
    }

    return profiles;
}

static float DominantLimbWeight(const VertexBoneProfile& p) {
    return std::max({p.leg, p.arm, p.handFoot});
}

static float AnchorWeightForPreset(int preset, const VertexBoneProfile& p) {
    switch (NormalizePreset(preset)) {
    case DRAPE_PRESET_LOOSE_DRAPED:
        return Clamp01(p.pelvis * 0.80f + p.torso * 0.45f);
    case DRAPE_PRESET_TUBULAR:
        return Clamp01(DominantLimbWeight(p) * 0.65f + p.handFoot * 0.25f);
    case DRAPE_PRESET_BELT:
        return Clamp01(p.pelvis * 0.75f + p.torso * 0.55f);
    case DRAPE_PRESET_FLEXIBLE_ARMOR:
    case DRAPE_PRESET_RIGID_ARMOR:
        return Clamp01(p.pelvis * 0.45f + p.torso * 0.45f + DominantLimbWeight(p) * 0.35f);
    default:
        return Clamp01(p.pelvis * 0.35f + p.torso * 0.35f + DominantLimbWeight(p) * 0.25f);
    }
}

static float BoneProfileFollowWeight(int preset,
                                     float rawFollow,
                                     const VertexBoneProfile* profile) {
    float w = PresetFollowWeight(preset, rawFollow);
    if (!profile || profile->influences <= 0)
        return w;

    const VertexBoneProfile& p = *profile;
    float anchor = AnchorWeightForPreset(preset, p);
    float limb = DominantLimbWeight(p);

    switch (NormalizePreset(preset)) {
    case DRAPE_PRESET_LOOSE_DRAPED:
        // Draped cloth: reduce follow weight by limb influence so lower body
        // verts become loose (small follow weight). Physics will then position
        // them. NO forced minimum here â€” let limbs reduce w to near-zero.
        // (The anchorStiffness parameter later applies waist-anchoring to tight verts.)
        w = w * (1.0f - 0.55f * limb);
        break;
    case DRAPE_PRESET_TUBULAR:
        // Pants/sleeves need enough limb follow to avoid clipping, while still
        // resisting concavity chasing through the raw distance falloff.
        w = std::max(w, limb * 0.70f);
        break;
    case DRAPE_PRESET_BELT:
        // Belts are waist-following but shape-preserving. Keep a strong pelvis
        // anchor and avoid reducing them to nearly zero at distance.
        w = std::max(w, anchor * 0.95f);
        break;
    case DRAPE_PRESET_RIGID_ARMOR:
        w = std::max(w, anchor * 0.65f);
        break;
    case DRAPE_PRESET_RIGID_ACCESSORY:
        w = std::max(w, anchor * 0.45f);
        break;
    default:
        w = std::max(w, anchor * 0.35f);
        break;
    }

    return Clamp01(w);
}

static int GuessPresetFromGeometry(const std::vector<Vec3>& verts,
                                   const std::vector<float>& weights,
                                   const std::vector<VertexBoneProfile>* profiles = nullptr) {
    if (verts.empty() || g_refMaxZ <= g_refMinZ + 1.0e-4f)
        return DRAPE_PRESET_AUTO;

    float minZ = verts[0].z;
    float maxZ = verts[0].z;
    int lowerVerts = 0;
    int upperVerts = 0;
    float bodyH = g_refMaxZ - g_refMinZ;
    float lowerCut = g_refMinZ + bodyH * 0.45f;
    float upperCut = g_refMinZ + bodyH * 0.58f;

    for (const Vec3& v : verts) {
        minZ = std::min(minZ, v.z);
        maxZ = std::max(maxZ, v.z);
        if (v.z < lowerCut) ++lowerVerts;
        if (v.z > upperCut) ++upperVerts;
    }

    float shapeH = maxZ - minZ;
    float lowerFrac = (float)lowerVerts / (float)verts.size();
    float upperFrac = (float)upperVerts / (float)verts.size();

    float avgW = 1.0f;
    if (!weights.empty()) {
        float sum = 0.0f;
        for (float w : weights) sum += Clamp01(w);
        avgW = sum / (float)weights.size();
    }

    bool longGarment = shapeH > bodyH * 0.38f &&
        lowerFrac > 0.20f &&
        upperFrac > 0.12f;
    bool notSkinTightOverall = avgW < 0.86f;

    // Bone-weight based classification (more reliable than geometry/distance alone).
    if (profiles && !profiles->empty()) {
        float torsoSum = 0.0f, limbSum = 0.0f;
        for (const auto& p : *profiles) {
            torsoSum += p.torso + p.pelvis;
            limbSum  += DominantLimbWeight(p);
        }
        float avgTorso = torsoSum / (float)profiles->size();
        float avgLimb  = limbSum  / (float)profiles->size();

        // Strong limb dominance with weak torso/pelvis = pants / sleeve / boot.
        // Note: dresses CAN have thigh weights for animation, but a true tubular
        // garment has avgLimb clearly dominant AND avgTorso clearly weak.
        if (avgLimb > 0.55f && avgTorso < 0.28f)
            return DRAPE_PRESET_TUBULAR;

        // Long torso-dominant garment spanning upper+lower body = draped shell.
        if (longGarment && avgTorso > 0.42f && notSkinTightOverall)
            return DRAPE_PRESET_LOOSE_DRAPED;
    }

    // Fallback geometry-only check (no bone profiles available).
    if (longGarment && notSkinTightOverall)
        return DRAPE_PRESET_LOOSE_DRAPED;

    return DRAPE_PRESET_AUTO;
}

static void SmoothMaskOnEdges(std::unordered_map<uint16_t, float>& mask,
                              const std::vector<EdgeConstraint>& edges,
                              int iterations,
                              float strength) {
    if (mask.empty() || edges.empty() || iterations <= 0 || strength <= 0.0f)
        return;

    strength = Clamp01(strength);
    for (int iter = 0; iter < iterations; ++iter) {
        std::unordered_map<uint16_t, float> sum;
        std::unordered_map<uint16_t, int> count;
        sum.reserve(mask.size());
        count.reserve(mask.size());

        for (const auto& e : edges) {
            auto ia = mask.find(e.a);
            auto ib = mask.find(e.b);
            if (ia == mask.end() || ib == mask.end())
                continue;

            sum[e.a] += ib->second;
            count[e.a] += 1;
            sum[e.b] += ia->second;
            count[e.b] += 1;
        }

        for (auto& kv : mask) {
            auto ci = count.find(kv.first);
            if (ci == count.end() || ci->second <= 0)
                continue;
            float avg = sum[kv.first] / (float)ci->second;
            kv.second = Clamp01(kv.second + (avg - kv.second) * strength);
        }
    }
}

static std::string trim(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

static bool WildcardMatchLower(const char* pat, const char* text) {
    const char* star = nullptr;
    const char* retry = nullptr;
    while (*text) {
        if (*pat == '?' || *pat == *text) {
            ++pat;
            ++text;
            continue;
        }
        if (*pat == '*') {
            star = pat++;
            retry = text;
            continue;
        }
        if (star) {
            pat = star + 1;
            text = ++retry;
            continue;
        }
        return false;
    }
    while (*pat == '*')
        ++pat;
    return *pat == '\0';
}

static std::vector<std::string> SplitSliderFilter(const char* filterUtf8) {
    std::vector<std::string> out;
    if (!filterUtf8)
        return out;

    std::string token;
    auto flush = [&]() {
        std::string t = lower(trim(token));
        token.clear();
        if (!t.empty())
            out.push_back(std::move(t));
    };

    for (const char* p = filterUtf8; *p; ++p) {
        char ch = *p;
        if (ch == ',' || ch == ';' || ch == '\n' || ch == '\r' || ch == '\t')
            flush();
        else
            token.push_back(ch);
    }
    flush();

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

static bool SliderAllowedForSmartConform(const std::string& sliderName) {
    std::string s = lower(sliderName);
    std::lock_guard<std::mutex> lk(g_sliderFilterMx);
    if (g_sliderFilter.empty())
        return true;

    for (const std::string& pat : g_sliderFilter) {
        if (pat == s || WildcardMatchLower(pat.c_str(), s.c_str()))
            return true;
    }
    return false;
}

static void RecordSliderFilterSkip(const std::string& sliderName) {
    std::string key = lower(sliderName);
    std::lock_guard<std::mutex> lk(g_sliderFilterMx);
    ++g_sliderFilterSkippedCalls;
    if (g_sliderFilterSkipLogged.insert(key).second && g_sliderFilterSkipExamples.size() < 8) {
        g_sliderFilterSkipExamples.push_back(sliderName);
    }
}

static void LogSliderFilterSummary() {
    int skipped = 0;
    std::vector<std::string> examples;
    {
        std::lock_guard<std::mutex> lk(g_sliderFilterMx);
        skipped = g_sliderFilterSkippedCalls;
        examples = g_sliderFilterSkipExamples;
        g_sliderFilterSkippedCalls = 0;
        g_sliderFilterSkipLogged.clear();
        g_sliderFilterSkipExamples.clear();
    }

    if (skipped <= 0)
        return;

    std::string joined;
    for (const std::string& ex : examples) {
        if (!joined.empty())
            joined += ", ";
        joined += ex;
    }
    HLog("detour: slider filter skipped=%d example(s)=[%s]", skipped, joined.c_str());
}

static float Clamp01(float v) {
    return std::max(0.0f, std::min(1.0f, v));
}

static int NormalizePreset(int preset) {
    if (preset < DRAPE_PRESET_AUTO || preset > DRAPE_PRESET_RIGID_ACCESSORY)
        return DRAPE_PRESET_AUTO;
    return preset;
}

static const char* PresetName(int preset) {
    switch (NormalizePreset(preset)) {
    case DRAPE_PRESET_SKIN_TIGHT: return "Skin-tight";
    case DRAPE_PRESET_FITTED_CLOTH: return "Fitted Cloth";
    case DRAPE_PRESET_LOOSE_DRAPED: return "Loose Draped";
    case DRAPE_PRESET_TUBULAR: return "Tubular";
    case DRAPE_PRESET_FLEXIBLE_ARMOR: return "Flexible Armor";
    case DRAPE_PRESET_RIGID_ARMOR: return "Rigid Armor";
    case DRAPE_PRESET_BELT: return "Belt / Waist Item";
    case DRAPE_PRESET_RIGID_ACCESSORY: return "Rigid Accessory";
    default: return "Auto";
    }
}

static int LayerTieBreakPriority(int preset) {
    switch (NormalizePreset(preset)) {
    case DRAPE_PRESET_RIGID_ACCESSORY: return 60;
    case DRAPE_PRESET_BELT:            return 55;
    case DRAPE_PRESET_RIGID_ARMOR:     return 50;
    case DRAPE_PRESET_FLEXIBLE_ARMOR:  return 45;
    case DRAPE_PRESET_LOOSE_DRAPED:    return 35;
    case DRAPE_PRESET_FITTED_CLOTH:    return 25;
    case DRAPE_PRESET_TUBULAR:         return 10;
    case DRAPE_PRESET_SKIN_TIGHT:      return 0;
    default:                           return 0;
    }
}

static bool PresetTieBreaksOuter(int outerPreset, int innerPreset) {
    int inner = NormalizePreset(innerPreset);
    if (inner != DRAPE_PRESET_TUBULAR && inner != DRAPE_PRESET_FITTED_CLOTH)
        return false;
    return LayerTieBreakPriority(outerPreset) > LayerTieBreakPriority(innerPreset);
}

static bool ShapeCoversInnerLocked(const std::string& outer, const std::string& inner) {
    auto it = g_shapeOutersByInner.find(inner);
    return it != g_shapeOutersByInner.end() &&
           it->second.find(outer) != it->second.end();
}

static bool NameHas(const std::string& lowerName, const char* token) {
    return lowerName.find(token) != std::string::npos;
}

static int GuessPresetFromShapeName(const std::string& shapeName) {
    std::string n = lower(shapeName);
    if (NameHas(n, "belt") || NameHas(n, "waist") ||
        NameHas(n, "girdle") || NameHas(n, "corset"))
        return DRAPE_PRESET_BELT;

    if (NameHas(n, "buckle") || NameHas(n, "medallion") ||
        NameHas(n, "pendant") || NameHas(n, "amulet") ||
        NameHas(n, "pouch") || NameHas(n, "bag") ||
        NameHas(n, "deco") || NameHas(n, "dec"))
        return DRAPE_PRESET_RIGID_ACCESSORY;

    if (NameHas(n, "skirt") || NameHas(n, "dress") ||
        NameHas(n, "robe") || NameHas(n, "tunic") ||
        NameHas(n, "fur") || NameHas(n, "cloak"))
        return DRAPE_PRESET_LOOSE_DRAPED;

    if (NameHas(n, "pants") || NameHas(n, "sleeve") ||
        NameHas(n, "glove") || NameHas(n, "boot") ||
        NameHas(n, "arm") || NameHas(n, "leg"))
        return DRAPE_PRESET_TUBULAR;

    if (NameHas(n, "pauldron") || NameHas(n, "shoulder") ||
        NameHas(n, "plate"))
        return DRAPE_PRESET_RIGID_ARMOR;

    if (NameHas(n, "mail") || NameHas(n, "leather"))
        return DRAPE_PRESET_FLEXIBLE_ARMOR;

    if (NameHas(n, "torso") || NameHas(n, "body") || NameHas(n, "cloth"))
        return DRAPE_PRESET_FITTED_CLOTH;

    return DRAPE_PRESET_AUTO;
}

static int PresetForShapeLocked(const std::string& shapeName) {
    auto it = g_shapePresets.find(shapeName);
    int preset = it != g_shapePresets.end()
        ? NormalizePreset(it->second)
        : DRAPE_PRESET_AUTO;

    int geometryPreset = DRAPE_PRESET_AUTO;
    auto geoIt = g_shapeGeometryPresets.find(shapeName);
    if (geoIt != g_shapeGeometryPresets.end())
        geometryPreset = NormalizePreset(geoIt->second);

    if (preset == DRAPE_PRESET_AUTO) {
        if (geometryPreset != DRAPE_PRESET_AUTO)
            return geometryPreset;
        return GuessPresetFromShapeName(shapeName);
    }

    // The UI can only guess from names. "Torso" is often a whole dress/skirt
    // mesh, so let geometry override the weak Fitted Cloth default.
    if (preset == DRAPE_PRESET_FITTED_CLOTH &&
        geometryPreset == DRAPE_PRESET_LOOSE_DRAPED) {
        return geometryPreset;
    }

    return preset;
}

static float PresetFollowWeight(int preset, float rawWeight) {
    float w = Clamp01(rawWeight);
    switch (NormalizePreset(preset)) {
    case DRAPE_PRESET_SKIN_TIGHT:
        return 1.0f;
    case DRAPE_PRESET_FITTED_CLOTH:
        return std::pow(w, 1.25f);
    case DRAPE_PRESET_LOOSE_DRAPED:
        return std::pow(w, 2.40f);
    case DRAPE_PRESET_TUBULAR:
        return std::pow(w, 1.65f);
    case DRAPE_PRESET_FLEXIBLE_ARMOR:
        return Clamp01(0.45f + 0.55f * std::pow(w, 1.35f));
    case DRAPE_PRESET_RIGID_ARMOR:
        return Clamp01(0.30f + 0.50f * w);
    case DRAPE_PRESET_BELT:
        return Clamp01(0.65f + 0.35f * w);
    case DRAPE_PRESET_RIGID_ACCESSORY:
        return Clamp01(0.15f + 0.35f * w);
    default:
        return w;
    }
}

static void BuildPresetMaskLocked(const std::string& shapeName,
                                  const std::vector<float>& weights,
                                  const std::vector<VertexBoneProfile>* profiles,
                                  const std::vector<EdgeConstraint>* edges,
                                   std::unordered_map<uint16_t, float>& outMask,
                                   int& outPreset) {
    outPreset = PresetForShapeLocked(shapeName);
    auto cacheIt = g_shapePresetMaskCache.find(shapeName);
    if (cacheIt != g_shapePresetMaskCache.end() && cacheIt->second.preset == outPreset) {
        outMask = cacheIt->second.mask;
        return;
    }

    outMask.clear();
    outMask.reserve(weights.size());
    for (size_t i = 0; i < weights.size() && i <= 0xFFFF; ++i) {
        const VertexBoneProfile* profile =
            (profiles && i < profiles->size()) ? &(*profiles)[i] : nullptr;
        outMask[(uint16_t)i] = BoneProfileFollowWeight(outPreset, weights[i], profile);
    }

    int smoothIterations = 1;
    float smoothStrength = 0.18f;
    switch (NormalizePreset(outPreset)) {
    case DRAPE_PRESET_SKIN_TIGHT:
        smoothIterations = 0;
        smoothStrength = 0.0f;
        break;
    case DRAPE_PRESET_FITTED_CLOTH:
        smoothIterations = 1;
        smoothStrength = 0.16f;
        break;
    case DRAPE_PRESET_LOOSE_DRAPED:
        smoothIterations = 3;
        smoothStrength = 0.34f;
        break;
    case DRAPE_PRESET_TUBULAR:
        smoothIterations = 2;
        smoothStrength = 0.24f;
        break;
    case DRAPE_PRESET_FLEXIBLE_ARMOR:
        smoothIterations = 2;
        smoothStrength = 0.18f;
        break;
    case DRAPE_PRESET_RIGID_ARMOR:
        smoothIterations = 2;
        smoothStrength = 0.14f;
        break;
    case DRAPE_PRESET_BELT:
        smoothIterations = 3;
        smoothStrength = 0.38f;
        break;
    case DRAPE_PRESET_RIGID_ACCESSORY:
        smoothIterations = 1;
        smoothStrength = 0.12f;
        break;
    default:
        break;
    }

    if (edges)
        SmoothMaskOnEdges(outMask, *edges, smoothIterations, smoothStrength);

    CachedPresetMask cached;
    cached.preset = outPreset;
    cached.mask = outMask;
    g_shapePresetMaskCache[shapeName] = std::move(cached);
}

static Vec3 ToVec3(const Vector3& v) {
    return {v.x, v.y, v.z};
}

static Vector3 ToVector3(const Vec3& v) {
    return Vector3(v.x, v.y, v.z);
}

static Vec3 Add(const Vec3& a, const Vec3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

static Vec3 Sub(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

static Vec3 Scale(const Vec3& v, float s) {
    return {v.x * s, v.y * s, v.z * s};
}

static float Len2(const Vec3& v) {
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

static float Len(const Vec3& v) {
    return std::sqrt(Len2(v));
}

static float ClampRange(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

static float Dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static Vec3 Cross(const Vec3& a, const Vec3& b) {
    return Vec3{ a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x };
}
static Vec3 VMin(const Vec3& a, const Vec3& b) {
    return Vec3{ std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) };
}
static Vec3 VMax(const Vec3& a, const Vec3& b) {
    return Vec3{ std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) };
}
static float Comp(const Vec3& v, int axis) {
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

// â”€â”€ BVH build (iterative median split over triangle centroids) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static void BuildBodyBVH(const std::vector<Vec3>& verts,
                         const std::vector<std::array<uint16_t, 3>>& tris,
                         BodyBVH& bvh) {
    bvh = BodyBVH{};
    if (verts.empty() || tris.empty()) return;
    bvh.verts = verts;

    const size_t nt = tris.size();
    bvh.tris.reserve(nt);
    bvh.centroid.reserve(nt);
    bvh.faceNormal.reserve(nt);
    bvh.triIndex.reserve(nt);
    for (size_t i = 0; i < nt; ++i) {
        const auto& t = tris[i];
        if (t[0] >= verts.size() || t[1] >= verts.size() || t[2] >= verts.size())
            continue;
        const Vec3& a = verts[t[0]];
        const Vec3& b = verts[t[1]];
        const Vec3& c = verts[t[2]];
        Vec3 nrm = Cross(Sub(b, a), Sub(c, a));
        float l = Len(nrm);
        int idx = (int)bvh.tris.size();
        bvh.tris.push_back(t);
        bvh.centroid.push_back(Scale(Add(Add(a, b), c), 1.0f / 3.0f));
        bvh.faceNormal.push_back(l > 1.0e-8f ? Scale(nrm, 1.0f / l) : Vec3{0.0f, 0.0f, 1.0f});
        bvh.triIndex.push_back(idx);
    }
    if (bvh.tris.empty()) return;

    struct BuildTask { int node, start, count; };
    bvh.nodes.reserve(bvh.tris.size() * 2);
    bvh.nodes.push_back(BodyBVH::Node{});
    std::vector<BuildTask> stack;
    stack.push_back({0, 0, (int)bvh.tris.size()});

    while (!stack.empty()) {
        BuildTask task = stack.back(); stack.pop_back();
        Vec3 bmin{ 1e30f,  1e30f,  1e30f};
        Vec3 bmax{-1e30f, -1e30f, -1e30f};
        Vec3 cmin = bmin, cmax = bmax;
        for (int i = task.start; i < task.start + task.count; ++i) {
            int ti = bvh.triIndex[i];
            const auto& t = bvh.tris[ti];
            bmin = VMin(bmin, VMin(bvh.verts[t[0]], VMin(bvh.verts[t[1]], bvh.verts[t[2]])));
            bmax = VMax(bmax, VMax(bvh.verts[t[0]], VMax(bvh.verts[t[1]], bvh.verts[t[2]])));
            cmin = VMin(cmin, bvh.centroid[ti]);
            cmax = VMax(cmax, bvh.centroid[ti]);
        }
        bvh.nodes[task.node].bmin = bmin;
        bvh.nodes[task.node].bmax = bmax;

        if (task.count <= 4) {
            bvh.nodes[task.node].left  = -1;
            bvh.nodes[task.node].start = task.start;
            bvh.nodes[task.node].count = task.count;
            continue;
        }

        Vec3 ext = Sub(cmax, cmin);
        int axis = (ext.x >= ext.y && ext.x >= ext.z) ? 0 : (ext.y >= ext.z ? 1 : 2);
        float mid = 0.5f * (Comp(cmin, axis) + Comp(cmax, axis));

        int l = task.start, r = task.start + task.count - 1;
        while (l <= r) {
            if (Comp(bvh.centroid[bvh.triIndex[l]], axis) < mid) ++l;
            else { std::swap(bvh.triIndex[l], bvh.triIndex[r]); --r; }
        }
        int leftCount = l - task.start;
        if (leftCount == 0 || leftCount == task.count) leftCount = task.count / 2;

        int leftNode = (int)bvh.nodes.size();
        bvh.nodes.push_back(BodyBVH::Node{});
        bvh.nodes.push_back(BodyBVH::Node{});
        bvh.nodes[task.node].left  = leftNode;
        bvh.nodes[task.node].count = 0;   // internal
        stack.push_back({leftNode,     task.start,             leftCount});
        stack.push_back({leftNode + 1, task.start + leftCount, task.count - leftCount});
    }
    bvh.valid = true;
}

// Closest point on triangle abc to p (Ericson, Real-Time Collision Detection).
static Vec3 ClosestPtTriangleBary(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c,
                                  float& outU, float& outV, float& outW) {
    Vec3 ab = Sub(b, a), ac = Sub(c, a), ap = Sub(p, a);
    float d1 = Dot(ab, ap), d2 = Dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        outU = 1.0f; outV = 0.0f; outW = 0.0f;
        return a;
    }
    Vec3 bp = Sub(p, b);
    float d3 = Dot(ab, bp), d4 = Dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        outU = 0.0f; outV = 1.0f; outW = 0.0f;
        return b;
    }
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        outU = 1.0f - v; outV = v; outW = 0.0f;
        return Add(a, Scale(ab, v));
    }
    Vec3 cp = Sub(p, c);
    float d5 = Dot(ab, cp), d6 = Dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        outU = 0.0f; outV = 0.0f; outW = 1.0f;
        return c;
    }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float w = d2 / (d2 - d6);
        outU = 1.0f - w; outV = 0.0f; outW = w;
        return Add(a, Scale(ac, w));
    }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        outU = 0.0f; outV = 1.0f - w; outW = w;
        return Add(b, Scale(Sub(c, b), w));
    }
    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom, w = vc * denom;
    outU = 1.0f - v - w; outV = v; outW = w;
    return Add(a, Add(Scale(ab, v), Scale(ac, w)));
}

static Vec3 ClosestPtTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    float u = 0.0f, v = 0.0f, w = 0.0f;
    return ClosestPtTriangleBary(p, a, b, c, u, v, w);
}

static float Dist2PointAABB(const Vec3& p, const Vec3& bmin, const Vec3& bmax) {
    float dx = std::max(0.0f, std::max(bmin.x - p.x, p.x - bmax.x));
    float dy = std::max(0.0f, std::max(bmin.y - p.y, p.y - bmax.y));
    float dz = std::max(0.0f, std::max(bmin.z - p.z, p.z - bmax.z));
    return dx * dx + dy * dy + dz * dz;
}

// Closest point on the body surface to p, with that triangle's face normal.
static bool BVHClosestPointEx(const BodyBVH& bvh, const Vec3& p,
                              Vec3& outCP, Vec3& outNormal, float& outDist2,
                              int* outTri, float* outU, float* outV, float* outW) {
    if (!bvh.valid || bvh.nodes.empty()) return false;
    float best = 1e30f;
    int bestTri = -1;
    Vec3 bestCP{0.0f, 0.0f, 0.0f};
    float bestU = 0.0f, bestV = 0.0f, bestW = 0.0f;

    int stack[128];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        int ni = stack[--sp];
        const BodyBVH::Node& node = bvh.nodes[ni];
        if (Dist2PointAABB(p, node.bmin, node.bmax) > best) continue;
        if (node.left < 0) {
            for (int i = node.start; i < node.start + node.count; ++i) {
                int ti = bvh.triIndex[i];
                const auto& t = bvh.tris[ti];
                float u = 0.0f, v = 0.0f, w = 0.0f;
                Vec3 cp = ClosestPtTriangleBary(p, bvh.verts[t[0]], bvh.verts[t[1]], bvh.verts[t[2]],
                                                u, v, w);
                float d2 = Len2(Sub(p, cp));
                if (d2 < best) {
                    best = d2; bestTri = ti; bestCP = cp;
                    bestU = u; bestV = v; bestW = w;
                }
            }
        } else {
            int lc = node.left, rc = node.left + 1;
            float dl = Dist2PointAABB(p, bvh.nodes[lc].bmin, bvh.nodes[lc].bmax);
            float dr = Dist2PointAABB(p, bvh.nodes[rc].bmin, bvh.nodes[rc].bmax);
            // push farther first so nearer is processed first
            if (dl < dr) {
                if (dr <= best && sp < 127) stack[sp++] = rc;
                if (dl <= best && sp < 127) stack[sp++] = lc;
            } else {
                if (dl <= best && sp < 127) stack[sp++] = lc;
                if (dr <= best && sp < 127) stack[sp++] = rc;
            }
        }
    }
    if (bestTri < 0) return false;
    outCP = bestCP;
    outNormal = bvh.faceNormal[bestTri];
    outDist2 = best;
    if (outTri) *outTri = bestTri;
    if (outU) *outU = bestU;
    if (outV) *outV = bestV;
    if (outW) *outW = bestW;
    return true;
}

static bool BVHClosestPoint(const BodyBVH& bvh, const Vec3& p,
                            Vec3& outCP, Vec3& outNormal, float& outDist2) {
    return BVHClosestPointEx(bvh, p, outCP, outNormal, outDist2,
                             nullptr, nullptr, nullptr, nullptr);
}

static Vec3 TriPointBary(const BodyBVH& bvh, int tri, float u, float v, float w) {
    if (tri < 0 || tri >= (int)bvh.tris.size()) return Vec3{0.0f, 0.0f, 0.0f};
    const auto& t = bvh.tris[(size_t)tri];
    return Add(Scale(bvh.verts[t[0]], u),
               Add(Scale(bvh.verts[t[1]], v), Scale(bvh.verts[t[2]], w)));
}

struct TriangleSample {
    float a, b, c;
};

static const TriangleSample kTriangleSamples[] = {
    {1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f},
    {0.5f, 0.5f, 0.0f},
    {0.5f, 0.0f, 0.5f},
    {0.0f, 0.5f, 0.5f},
};

static Vec3 SampleTrianglePoint(const std::vector<Vec3>& verts,
                                const std::array<uint16_t, 3>& t,
                                const TriangleSample& s) {
    return Add(Scale(verts[t[0]], s.a),
               Add(Scale(verts[t[1]], s.b), Scale(verts[t[2]], s.c)));
}

static bool BVHSignedGap(const BodyBVH& bvh, const Vec3& p, float& outGap, Vec3* outNrm) {
    Vec3 cp{0,0,0}, nrm{0,0,1};
    float d2 = 0.0f;
    if (!BVHClosestPoint(bvh, p, cp, nrm, d2)) return false;
    float dist = std::sqrt(d2);
    float sign = Dot(Sub(p, cp), nrm);
    outGap = sign < 0.0f ? -dist : dist;
    if (outNrm) *outNrm = nrm;
    return true;
}

// Push a cloth vertex outside the body to `clearance` along the real surface
// normal, if it is inside the body or closer than clearance. Returns true if moved.
static bool BVHCollideVertex(const BodyBVH& bvh, Vec3& v, float clearance) {
    Vec3 cp{0,0,0}, nrm{0,0,1};
    float d2 = 0.0f;
    if (!BVHClosestPoint(bvh, v, cp, nrm, d2)) return false;
    float dist = std::sqrt(d2);
    float sign = Dot(Sub(v, cp), nrm);   // >0 outside, <0 inside
    bool inside = sign < 0.0f;
    if (!inside && dist >= clearance) return false;  // already clear
    v = Add(cp, Scale(nrm, clearance));  // sit exactly `clearance` off the surface
    return true;
}

static uint64_t CurrentExeSize() {
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(nullptr, path, MAX_PATH)) return 0;

    HANDLE h = CreateFileA(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;

    LARGE_INTEGER size = {};
    uint64_t result = 0;
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0)
        result = static_cast<uint64_t>(size.QuadPart);

    CloseHandle(h);
    return result;
}

static fs::path FindMo2Root() {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::path(g_bsDir), ec);
    if (ec) p = fs::path(g_bsDir);

    for (fs::path cur = p; !cur.empty(); cur = cur.parent_path()) {
        if (lower(cur.filename().string()) == "mods")
            return cur.parent_path();
        if (cur == cur.parent_path()) break;
    }
    return {};
}

static std::string SelectedMo2Profile(const fs::path& mo2Root) {
    std::ifstream f(mo2Root / "ModOrganizer.ini");
    std::string line;
    while (std::getline(f, line)) {
        const std::string key = "selected_profile=@ByteArray(";
        auto p = line.find(key);
        if (p == std::string::npos) continue;
        p += key.size();
        auto q = line.find(')', p);
        return q == std::string::npos ? trim(line.substr(p)) : trim(line.substr(p, q - p));
    }
    return {};
}

static void AddBodySlideRoot(std::vector<fs::path>& roots,
                             std::unordered_set<std::string>& seen,
                             const fs::path& root) {
    std::error_code ec;
    if (!fs::is_directory(root / "SliderSets", ec)) return;

    std::string key = lower(root.lexically_normal().string());
    if (seen.insert(key).second)
        roots.push_back(root);
}

static std::vector<fs::path> BodySlideSearchRoots() {
    std::vector<fs::path> roots;
    std::unordered_set<std::string> seen;

    fs::path mo2Root = FindMo2Root();
    if (!mo2Root.empty()) {
        fs::path modsRoot = mo2Root / "mods";
        AddBodySlideRoot(roots, seen, mo2Root / "overwrite" / "CalienteTools" / "BodySlide");

        std::string profile = SelectedMo2Profile(mo2Root);
        fs::path modlist = mo2Root / "profiles" / profile / "modlist.txt";

        std::ifstream f(modlist);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(f, line))
            lines.push_back(line);

        // MO2 conflict priority increases downward in modlist.txt. Search from
        // bottom to top so duplicate SliderSets resolve like the VFS does.
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            std::string entry = trim(*it);
            if (entry.size() < 2 || entry[0] != '+') continue;
            std::string modName = trim(entry.substr(1));
            AddBodySlideRoot(roots, seen, modsRoot / modName / "CalienteTools" / "BodySlide");
        }

        // Fallback for portable/manual MO2 setups with no readable modlist.
        if (roots.empty()) {
            std::error_code ec;
            for (auto& e : fs::directory_iterator(modsRoot, ec)) {
                if (!e.is_directory()) continue;
                AddBodySlideRoot(roots, seen, e.path() / "CalienteTools" / "BodySlide");
            }
        }
    }

    AddBodySlideRoot(roots, seen, fs::path(g_bsDir));
    return roots;
}

struct ProjectNifRef {
    fs::path bodySlideRoot;
    fs::path ospPath;
    std::string dataFolder;
    std::string sourceFile;
    std::string firstShape;
};

struct ProjectLoadInfo {
    fs::path loggedOspPath;
    std::string referenceShape;
    bool matched = false;
};

static std::string ParseSingleQuotedAfter(const std::string& line, const char* marker) {
    auto p = line.find(marker);
    if (p == std::string::npos) return {};
    p += strlen(marker);
    auto q = line.find('\'', p);
    return q == std::string::npos ? std::string{} : line.substr(p, q - p);
}

static ProjectLoadInfo LatestProjectLoadInfo(const std::string& proj) {
    ProjectLoadInfo best;

    std::ifstream f(fs::path(g_bsDir) / "Log_OS.txt");
    if (!f) return best;

    std::string projL = lower(trim(proj));
    ProjectLoadInfo cur;
    bool curMatches = false;
    std::string line;
    while (std::getline(f, line)) {
        auto lp = line.find("Loading project '");
        if (lp != std::string::npos) {
            lp += strlen("Loading project '");
            auto mid = line.find("' from file '", lp);
            if (mid == std::string::npos) {
                curMatches = false;
                continue;
            }

            std::string loggedProj = line.substr(lp, mid - lp);
            auto fp = mid + strlen("' from file '");
            auto fq = line.find('\'', fp);

            cur = ProjectLoadInfo{};
            curMatches = (lower(trim(loggedProj)) == projL);
            if (curMatches) {
                cur.matched = true;
                if (fq != std::string::npos)
                    cur.loggedOspPath = line.substr(fp, fq - fp);
                best = cur;
            }
            continue;
        }

        if (!curMatches) continue;

        std::string ref = ParseSingleQuotedAfter(line, "Loading reference shape '");
        if (!ref.empty()) {
            cur.referenceShape = ref;
            best = cur;
        }
    }

    if (best.matched) {
        HLog("resolve: OS log says osp='%s' reference='%s'",
             best.loggedOspPath.string().c_str(), best.referenceShape.c_str());
    }
    return best;
}

static bool FindProjectInRoot(const fs::path& bodySlideRoot,
                              const std::string& projL,
                              const std::string& preferredOspFileL,
                              ProjectNifRef& out) {
    fs::path ss = bodySlideRoot / "SliderSets";
    std::error_code ec;
    if (!fs::is_directory(ss, ec)) return false;

    for (auto& e : fs::directory_iterator(ss, ec)) {
        if (lower(e.path().extension().string()) != ".osp") continue;
        if (!preferredOspFileL.empty() &&
            lower(e.path().filename().string()) != preferredOspFileL) {
            continue;
        }

        std::ifstream f(e.path());
        std::string line, dataFolder, sourceFile, firstShape;
        bool inThis = (lower(e.path().stem().string()) == projL);
        while (std::getline(f, line)) {
            auto sp = line.find("<SliderSet name=\"");
            if (sp != std::string::npos) {
                sp += 17; auto q = line.find('"', sp);
                inThis = (q != std::string::npos) &&
                         (lower(line.substr(sp, q - sp)) == projL);
            }
            if (!inThis) continue;

            auto getTag = [&](const char* o, const char* c)->std::string{
                auto a = line.find(o); if (a == std::string::npos) return {};
                a += strlen(o); auto b = line.find(c, a);
                return b != std::string::npos ? line.substr(a, b - a) : std::string{};
            };
            if (dataFolder.empty()) dataFolder = getTag("<DataFolder>","</DataFolder>");
            if (sourceFile.empty()) sourceFile = getTag("<SourceFile>","</SourceFile>");

            if (firstShape.empty()) {
                auto sh = line.find("<Shape");
                if (sh != std::string::npos) {
                    auto gt = line.find('>', sh);
                    auto end = gt == std::string::npos ? std::string::npos : line.find("</Shape>", gt);
                    if (gt != std::string::npos && end != std::string::npos)
                        firstShape = trim(line.substr(gt + 1, end - gt - 1));
                }
            }

            if (!dataFolder.empty() && !sourceFile.empty() &&
                (!firstShape.empty() || line.find("</SliderSet>") != std::string::npos)) {
                out.bodySlideRoot = bodySlideRoot;
                out.ospPath = e.path();
                out.dataFolder = dataFolder;
                out.sourceFile = sourceFile;
                out.firstShape = firstShape;
                return true;
            }
        }
    }

    return false;
}

struct ResolvedProject {
    std::string nifPath;
    std::string referenceShape;
    fs::path ospPath;
};

// Resolve the open project's NIF path and the actual OutfitStudio reference
// shape. The hook cannot see OutfitProject::baseShape directly, so it uses
// Outfit Studio's own load log first and falls back to the SliderSet's first
// shape if the log is unavailable.
static ResolvedProject ResolveProject(const std::string& proj) {
    std::string projL = lower(proj);
    ProjectLoadInfo loadInfo = LatestProjectLoadInfo(proj);
    std::string preferredOspFileL = lower(loadInfo.loggedOspPath.filename().string());
    auto roots = BodySlideSearchRoots();
    HLog("resolve: searching %zu BodySlide root(s) for '%s'",
         roots.size(), proj.c_str());

    auto tryResolve = [&](const std::string& ospFileFilterL) -> ResolvedProject {
        ProjectNifRef ref;
        for (const auto& root : roots) {
            if (!FindProjectInRoot(root, projL, ospFileFilterL, ref)) continue;

            std::string referenceShape = !loadInfo.referenceShape.empty()
                ? loadInfo.referenceShape
                : ref.firstShape;

            fs::path relShape = fs::path("ShapeData") / ref.dataFolder / ref.sourceFile;
            fs::path sameMod = ref.bodySlideRoot / relShape;
            if (fs::exists(sameMod)) {
                HLog("resolve: project found in '%s'", ref.ospPath.string().c_str());
                HLog("resolve: reference shape '%s'", referenceShape.c_str());
                HLog("resolve: NIF found in same mod '%s'", sameMod.string().c_str());
                return {sameMod.string(), referenceShape, ref.ospPath};
            }

            for (const auto& shapeRoot : roots) {
                fs::path candidate = shapeRoot / relShape;
                if (fs::exists(candidate)) {
                    HLog("resolve: project found in '%s'", ref.ospPath.string().c_str());
                    HLog("resolve: reference shape '%s'", referenceShape.c_str());
                    HLog("resolve: NIF found via VFS-style search '%s'", candidate.string().c_str());
                    return {candidate.string(), referenceShape, ref.ospPath};
                }
            }

            HLog("resolve: project found in '%s' but NIF missing: ShapeData\\%s\\%s",
                 ref.ospPath.string().c_str(), ref.dataFolder.c_str(), ref.sourceFile.c_str());
        }
        return {};
    };

    if (!preferredOspFileL.empty()) {
        ResolvedProject resolved = tryResolve(preferredOspFileL);
        if (!resolved.nifPath.empty())
            return resolved;
    }

    ResolvedProject resolved = tryResolve({});
    if (!resolved.nifPath.empty())
        return resolved;

    HLog("resolve: no SliderSet match for '%s'", proj.c_str());
    return {};
}

static std::string XmlAttrValue(const std::string& line, const char* attr) {
    std::string needle = std::string(attr) + "=\"";
    auto p = line.find(needle);
    if (p == std::string::npos)
        return {};
    p += needle.size();
    auto q = line.find('"', p);
    return q == std::string::npos ? std::string{} : line.substr(p, q - p);
}

static std::vector<std::string> ParseSliderNamesFromOsp(const fs::path& ospPath,
                                                        const std::string& proj) {
    std::ifstream f(ospPath);
    std::vector<std::string> exact;
    std::vector<std::string> fallback;
    if (!f)
        return exact;

    std::string projL = lower(trim(proj));
    std::string line;
    bool inSet = false;
    bool inExactSet = false;
    while (std::getline(f, line)) {
        if (line.find("<SliderSet") != std::string::npos) {
            inSet = true;
            inExactSet = (lower(trim(XmlAttrValue(line, "name"))) == projL);
            continue;
        }
        if (line.find("</SliderSet>") != std::string::npos) {
            inSet = false;
            inExactSet = false;
            continue;
        }
        if (!inSet || line.find("<Slider ") == std::string::npos)
            continue;

        std::string name = trim(XmlAttrValue(line, "name"));
        if (name.empty())
            continue;
        fallback.push_back(name);
        if (inExactSet)
            exact.push_back(name);
    }

    std::vector<std::string> out = exact.empty() ? fallback : exact;
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// (Re)build per-shape weights for the currently open project. Caller holds lock.
static void RebuildCacheLocked() {
    g_shapeWeights.clear();
    g_shapeBoneProfiles.clear();
    g_shapeVerts.clear();
    g_shapeEdges.clear();
    g_shapeBending.clear();
    g_shapeGeometryPresets.clear();
    g_shapeLayer.clear();
    g_shapeOutersByInner.clear();
    g_shapeTris.clear();
    g_shapeBaseBVH.clear();
    g_solvedShapes.clear();
    g_refVerts.clear();
    g_refTris.clear();
    g_refBodyBVH = BodyBVH{};
    g_bodyShellCache.clear();
    g_bodyBVHCache.clear();
    g_shapeMask.clear();
    g_shapePresetMaskCache.clear();
    g_missingShapesLogged.clear();
    g_appliedShapesLogged.clear();
    g_staticShapesLogged.clear();
    g_fallbackShapesLogged.clear();

    std::string proj = ProjFromTitle();
    g_projectKey = proj;
    if (proj.empty()) { HLog("cache: no project in title"); return; }

    ResolvedProject resolved = ResolveProject(proj);
    if (resolved.nifPath.empty() || !fs::exists(resolved.nifPath)) {
        HLog("cache: NIF not found for '%s'", proj.c_str());
        return;
    }
    NifFile nif;
    if (nif.Load(resolved.nifPath) != 0) { HLog("cache: NIF load failed"); return; }
    if (resolved.referenceShape.empty()) {
        HLog("cache: no OutfitStudio reference shape resolved for '%s'", proj.c_str());
        return;
    }

    // Use the exact reference shape selected by OutfitStudio. Do not infer from
    // names like CBBE/3BA/HIMBO; vanilla Conform works from baseShape.
    NiShape* body = nullptr;
    for (auto* s : nif.GetShapes()) {
        if (!s) continue;
        std::string name = s->name.get();
        if (name == resolved.referenceShape) {
            body = s;
            break;
        }
    }
    if (!body) {
        for (auto* s : nif.GetShapes()) {
            if (!s) continue;
            std::string name = s->name.get();
            if (lower(name) == lower(resolved.referenceShape)) {
                body = s;
                break;
            }
        }
    }
    if (!body) {
        std::string names;
        for (auto* s : nif.GetShapes()) {
            if (!s) continue;
            if (!names.empty()) names += ", ";
            names += s->name.get();
            if (names.size() > 700) { names += ", ..."; break; }
        }
        HLog("cache: reference shape '%s' not found in project NIF; shapes=[%s]",
             resolved.referenceShape.c_str(), names.c_str());
        return;
    }

    std::vector<Vector3> bv; nif.GetVertsForShape(body, bv);
    std::vector<Vec3> bodyVerts; bodyVerts.reserve(bv.size());
    for (auto& v : bv) bodyVerts.push_back({v.x,v.y,v.z});
    g_refVerts = bodyVerts;
    if (!bodyVerts.empty()) {
        float minX = bodyVerts[0].x, maxX = bodyVerts[0].x;
        float minY = bodyVerts[0].y, maxY = bodyVerts[0].y;
        float minZ = bodyVerts[0].z, maxZ = bodyVerts[0].z;
        for (const auto& v : bodyVerts) {
            minX = std::min(minX, v.x); maxX = std::max(maxX, v.x);
            minY = std::min(minY, v.y); maxY = std::max(maxY, v.y);
            minZ = std::min(minZ, v.z); maxZ = std::max(maxZ, v.z);
        }
        g_refCenterX = (minX + maxX) * 0.5f;
        g_refCenterY = (minY + maxY) * 0.5f;
        g_refMinZ = minZ;
        g_refMaxZ = maxZ;
    }
    std::vector<Triangle> bt; body->GetTriangles(bt);
    TriList bodyTris; bodyTris.reserve(bt.size());
    for (auto& t : bt) bodyTris.push_back({t.p1,t.p2,t.p3});
    // Persist body triangles globally for the 3D BVH collider.
    g_refTris.clear();
    g_refTris.reserve(bt.size());
    for (auto& t : bt) g_refTris.push_back({t.p1, t.p2, t.p3});
    BuildBodyBVH(g_refVerts, g_refTris, g_refBodyBVH);
    if (g_refBodyBVH.valid)
        HLog("cache: base body BVH built tris=%zu nodes=%zu",
             g_refBodyBVH.tris.size(), g_refBodyBVH.nodes.size());

    DrapeConformer dc(g_opts);
    std::string bodyName = body->name.get();
    HLog("cache: using OutfitStudio reference shape '%s'", bodyName.c_str());
    int built = 0;
    for (auto* s : nif.GetShapes()) {
        if (!s) continue;
        std::string name = s->name.get();
        if (name == bodyName)
            continue;

        std::vector<Vector3> ov; nif.GetVertsForShape(s, ov);
        std::vector<Vec3> outVerts; outVerts.reserve(ov.size());
        for (auto& v : ov) outVerts.push_back({v.x,v.y,v.z});

        auto w = dc.buildWeights(outVerts, bodyVerts, bodyTris);
        float minW = 1.0f, maxW = 0.0f, sumW = 0.0f;
        int full = 0, partial = 0, zero = 0;
        for (float x : w) {
            minW = std::min(minW, x);
            maxW = std::max(maxW, x);
            sumW += x;
            if (x >= 0.999f) ++full;
            else if (x <= 0.001f) ++zero;
            else ++partial;
        }
        float avgW = w.empty() ? 1.0f : sumW / (float)w.size();

        // Full mask: every vertex must be present (absent => ZeroVertDiff erases).
        std::unordered_map<uint16_t,float> mask;
        mask.reserve(w.size());
        for (size_t i = 0; i < w.size() && i <= 0xFFFF; ++i)
            mask[(uint16_t)i] = w[i];
        auto boneProfiles = BuildBoneProfiles(nif, s, outVerts.size());
        int boneInfluenced = 0;
        float torsoSum = 0.0f, limbSum = 0.0f;
        for (const auto& p : boneProfiles) {
            if (p.influences > 0)
                ++boneInfluenced;
            torsoSum += p.torso + p.pelvis;
            limbSum += DominantLimbWeight(p);
        }
        float avgTorsoBone = boneProfiles.empty() ? 0.0f : torsoSum / (float)boneProfiles.size();
        float avgLimbBone = boneProfiles.empty() ? 0.0f : limbSum / (float)boneProfiles.size();

        g_shapeWeights[name] = std::move(w);
        // Pass boneProfiles before moving it â€” needed for TUBULAR/DRAPED detection.
        int geometryPreset = GuessPresetFromGeometry(outVerts, g_shapeWeights[name], &boneProfiles);
        if (geometryPreset != DRAPE_PRESET_AUTO)
            g_shapeGeometryPresets[name] = geometryPreset;
        auto edges   = BuildEdgeConstraints(s, outVerts);
        auto bending = BuildBendingConstraints(s, outVerts);
        g_shapeBoneProfiles[name] = std::move(boneProfiles);
        g_shapeVerts[name]   = std::move(outVerts);
        g_shapeEdges[name]   = std::move(edges);
        g_shapeBending[name] = std::move(bending);
        {
            std::vector<Triangle> st; s->GetTriangles(st);
            auto& dst = g_shapeTris[name];
            dst.reserve(st.size());
            for (auto& t : st) dst.push_back({t.p1, t.p2, t.p3});
        }
        HLog("cache: shape '%s' created %zu bending constraints", name.c_str(), g_shapeBending[name].size());
        g_shapeMask[name]    = std::move(mask);
        ++built;
        HLog("cache: shape '%s' weights min=%.3f max=%.3f avg=%.3f full=%d partial=%d zero=%d edges=%zu boneVerts=%d avgTorsoBone=%.3f avgLimbBone=%.3f geomPreset='%s'",
             name.c_str(), minW, maxW, avgW, full, partial, zero,
             g_shapeEdges[name].size(), boneInfluenced, avgTorsoBone, avgLimbBone,
             PresetName(geometryPreset));
    }

    // â”€â”€ Inter-mesh layer order pre-pass â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    for (const auto& kv : g_shapeVerts) {
        auto triIt = g_shapeTris.find(kv.first);
        if (triIt == g_shapeTris.end() || triIt->second.empty() ||
            kv.second.empty() || kv.second.size() > 0xFFFF)
            continue;

        BodyBVH bvh;
        BuildBodyBVH(kv.second, triIt->second, bvh);
        if (bvh.valid)
            g_shapeBaseBVH.emplace(kv.first, std::move(bvh));
    }
    HLog("cache: built base garment BVHs=%zu", g_shapeBaseBVH.size());

    // For every pair of garment meshes, sample where they overlap (same vertical
    // column near the body) in the BASE mesh and record which sits farther from the
    // body's central axis there (outer). Aggregate into a per-shape layer rank so
    // the solver can push an outer mesh out of any inner mesh it would clip through.
    //
    // Distance-from-axis is a cheap, robust "outer-ness" proxy: a cloak worn over a
    // tunic has a larger XY radius at the same Z/column than the tunic. We pair up
    // meshes by binning verts into a coarse (column, Z) grid and comparing mean radii
    // where both meshes have samples.
    {
        struct ShapeRadial {
            std::string name;
            // grid key (qx,qy,qz packed) -> {sumRadius, count}
            std::unordered_map<uint64_t, std::pair<float,int>> cells;
        };
        const float kCell = 4.0f;  // coarse column size (units)
        auto gkey = [&](const Vec3& p) -> uint64_t {
            auto q = [&](float v) -> uint64_t {
                return (uint64_t)(uint32_t)(int32_t)std::llround(v / kCell);
            };
            return (q(p.x) * 73856093ull) ^ (q(p.y) * 19349663ull) ^ (q(p.z) * 83492791ull);
        };
        auto radiusOf = [&](const Vec3& p) -> float {
            float dx = p.x - g_refCenterX, dy = p.y - g_refCenterY;
            return std::sqrt(dx * dx + dy * dy);
        };

        std::vector<ShapeRadial> shapes;
        shapes.reserve(g_shapeVerts.size());
        for (const auto& kv : g_shapeVerts) {
            ShapeRadial sr;
            sr.name = kv.first;
            for (const auto& v : kv.second) {
                auto& cell = sr.cells[gkey(v)];
                cell.first  += radiusOf(v);
                cell.second += 1;
            }
            shapes.push_back(std::move(sr));
        }

        // Pairwise: for shapes A,B, over shared cells, count how often A is outside B
        // (and vice versa). The net decides which is outer in their overlap, and gives
        // a strict winner per pair. We accumulate a per-shape score (higher = outer).
        // A simple majority (>60% of decisive cells) wins the pair, so two garments in
        // the same vertical band (pants under a long dress) get separated even with
        // modest overlap â€” the previous 2x ratio + shared<3 gate left them tied.
        std::unordered_map<std::string, int> score;  // higher = more outer
        for (auto& kv : g_shapeVerts) score[kv.first] = 0;
        auto recordOuter = [&](const std::string& outer,
                               const std::string& inner,
                               int weight) {
            if (outer == inner)
                return;
            auto reverseIt = g_shapeOutersByInner.find(outer);
            if (reverseIt != g_shapeOutersByInner.end() &&
                reverseIt->second.erase(inner) > 0 &&
                score[inner] > 0) {
                --score[inner];
            }

            bool inserted = g_shapeOutersByInner[inner].insert(outer).second;
            score[outer] += inserted ? weight : std::max(0, weight - 1);
        };
        for (size_t a = 0; a < shapes.size(); ++a) {
            for (size_t b = a + 1; b < shapes.size(); ++b) {
                int aOuter = 0, bOuter = 0, shared = 0;
                for (const auto& ca : shapes[a].cells) {
                    auto it = shapes[b].cells.find(ca.first);
                    if (it == shapes[b].cells.end()) continue;
                    float ra = ca.second.first / (float)ca.second.second;
                    float rb = it->second.first / (float)it->second.second;
                    ++shared;
                    if (ra > rb + 0.25f) ++aOuter;       // lower margin: thin garments differ little
                    else if (rb > ra + 0.25f) ++bOuter;
                }
                if (shared < 2) continue;                 // need at least minimal overlap
                int decisive = aOuter + bOuter;
                if (decisive < 1) continue;               // no clear winner anywhere
                // Simple majority of decisive cells wins the pair.
                if (aOuter * 5 > decisive * 3)      recordOuter(shapes[a].name, shapes[b].name, 1);  // a outside >60%
                else if (bOuter * 5 > decisive * 3) recordOuter(shapes[b].name, shapes[a].name, 1);  // b outside >60%
            }
        }

        if (g_refBodyBVH.valid && !g_shapeBaseBVH.empty()) {
            const float kPairNear2 = 4.50f * 4.50f;
            const float kGapMargin = 0.08f;
            auto sampleAgainst = [&](const std::vector<Vec3>& sourceVerts,
                                     const BodyBVH& targetBvh,
                                     bool sourceIsA,
                                     int& aOuter,
                                     int& bOuter,
                                     int& nearSamples) {
                if (sourceVerts.empty() || !targetBvh.valid)
                    return;
                size_t stride = std::max<size_t>(1, sourceVerts.size() / 384);
                size_t sampled = 0;
                for (size_t i = 0; i < sourceVerts.size(); i += stride) {
                    if (++sampled > 512)
                        break;
                    Vec3 cp{0.0f, 0.0f, 0.0f}, nrm{0.0f, 0.0f, 1.0f};
                    float d2 = 0.0f;
                    if (!BVHClosestPoint(targetBvh, sourceVerts[i], cp, nrm, d2))
                        continue;
                    if (d2 > kPairNear2)
                        continue;
                    float sourceGap = 0.0f, targetGap = 0.0f;
                    if (!BVHSignedGap(g_refBodyBVH, sourceVerts[i], sourceGap, nullptr) ||
                        !BVHSignedGap(g_refBodyBVH, cp, targetGap, nullptr))
                        continue;

                    ++nearSamples;
                    if (sourceGap > targetGap + kGapMargin) {
                        if (sourceIsA) ++aOuter; else ++bOuter;
                    } else if (targetGap > sourceGap + kGapMargin) {
                        if (sourceIsA) ++bOuter; else ++aOuter;
                    }
                }
            };

            for (size_t a = 0; a < shapes.size(); ++a) {
                for (size_t b = a + 1; b < shapes.size(); ++b) {
                    const std::string& aName = shapes[a].name;
                    const std::string& bName = shapes[b].name;
                    auto avIt = g_shapeVerts.find(aName);
                    auto bvIt = g_shapeVerts.find(bName);
                    auto abvhIt = g_shapeBaseBVH.find(aName);
                    auto bbvhIt = g_shapeBaseBVH.find(bName);
                    if (avIt == g_shapeVerts.end() || bvIt == g_shapeVerts.end() ||
                        abvhIt == g_shapeBaseBVH.end() || bbvhIt == g_shapeBaseBVH.end())
                        continue;

                    int aOuter = 0, bOuter = 0, nearSamples = 0;
                    sampleAgainst(avIt->second, bbvhIt->second, true,  aOuter, bOuter, nearSamples);
                    sampleAgainst(bvIt->second, abvhIt->second, false, aOuter, bOuter, nearSamples);
                    int decisive = aOuter + bOuter;
                    if (nearSamples < 8 || decisive < 3)
                        continue;

                    if (aOuter * 5 > decisive * 3) {
                        recordOuter(aName, bName, 2);
                        HLog("layer-pair: outer='%s' inner='%s' source='body-gap' votes=%d/%d near=%d",
                             aName.c_str(), bName.c_str(), aOuter, decisive, nearSamples);
                    } else if (bOuter * 5 > decisive * 3) {
                        recordOuter(bName, aName, 2);
                        HLog("layer-pair: outer='%s' inner='%s' source='body-gap' votes=%d/%d near=%d",
                             bName.c_str(), aName.c_str(), bOuter, decisive, nearSamples);
                    }
                }
            }
        }

        for (auto& kv : score) g_shapeLayer[kv.first] = kv.second;

        for (const auto& kv : g_shapeLayer)
            HLog("layer: shape '%s' rank=%d", kv.first.c_str(), kv.second);
    }

    HLog("cache: built weights for %d shape(s), project '%s'", built, proj.c_str());
}

struct EnvelopePoint {
    uint16_t idx = 0;
    int bin = 0;
    float z = 0.0f;
    float supportR = 0.0f;
    float currentR = 0.0f;
    float dirX = 0.0f;
    float dirY = 0.0f;
    float follow = 1.0f;
    Vec3 base{};
    Vec3 current{};
};

static bool ApplyVerticalEnvelopeDrape(void* resultDiffData,
                                       const std::string& setName,
                                       const std::string& shapeName,
                                       const std::string& sliderName,
                                       const std::vector<Vec3>& baseVerts,
                                       const std::vector<float>& followWeights,
                                       const std::vector<EdgeConstraint>& edges) {
    if (!resultDiffData || baseVerts.empty())
        return false;

    auto* diffSets = reinterpret_cast<DiffDataSets*>(resultDiffData);
    TargetDataDiffs* diffs = diffSets->GetDiffSet(setName);
    if (!diffs)
        return false;

    static constexpr int kBins = 32;
    static constexpr float kPi = 3.14159265358979323846f;
    static constexpr float kTwoPi = 2.0f * kPi;
    static constexpr float kEps = 1.0e-5f;

    std::vector<EnvelopePoint> points;
    points.reserve(std::min<size_t>(baseVerts.size(), 0x10000));
    std::array<std::vector<size_t>, kBins> bins;

    size_t baseDiffVerts = diffs->size();
    size_t n = std::min<size_t>(baseVerts.size(), 0x10000);
    std::vector<Vec3> finalVerts(baseVerts.begin(), baseVerts.begin() + n);
    std::vector<float> invMass(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        Vec3 base = baseVerts[i];
        Vec3 vanillaDiff{};
        auto diffIt = diffs->find(static_cast<uint16_t>(i));
        if (diffIt != diffs->end())
            vanillaDiff = ToVec3(diffIt->second);

        float follow = i < followWeights.size() ? Clamp01(followWeights[i]) : 1.0f;
        invMass[i] = Clamp01(1.0f - follow);
        Vec3 support = Add(base, vanillaDiff);
        Vec3 current = Add(base, Scale(vanillaDiff, follow));
        finalVerts[i] = current;

        float sx = support.x - g_refCenterX;
        float sy = support.y - g_refCenterY;
        float supportR = std::sqrt(sx * sx + sy * sy);
        if (supportR <= kEps)
            continue;

        float angle = std::atan2(sy, sx);
        int bin = static_cast<int>(((angle + kPi) / kTwoPi) * kBins);
        if (bin < 0) bin = 0;
        if (bin >= kBins) bin = kBins - 1;

        float cx = current.x - g_refCenterX;
        float cy = current.y - g_refCenterY;
        float currentR = std::sqrt(cx * cx + cy * cy);

        EnvelopePoint p;
        p.idx = static_cast<uint16_t>(i);
        p.bin = bin;
        p.z = support.z;
        p.supportR = supportR;
        p.currentR = currentR;
        p.dirX = sx / supportR;
        p.dirY = sy / supportR;
        p.follow = follow;
        p.base = base;
        p.current = current;
        bins[bin].push_back(points.size());
        points.push_back(p);
    }

    int envelopeAdjusted = 0;
    int stretchAdjusted = 0;
    int written = 0;
    int erased = 0;

    for (auto& bin : bins) {
        std::sort(bin.begin(), bin.end(), [&](size_t a, size_t b) {
            return points[a].z > points[b].z;
        });

        float maxRFromAbove = -1.0f;
        for (size_t pointIndex : bin) {
            EnvelopePoint& p = points[pointIndex];
            maxRFromAbove = std::max(maxRFromAbove, p.supportR);

            float envelope = g_opts.envelopeStrength *
                std::max(1.0f - p.follow, g_opts.minEnvelopeStrength);
            envelope = Clamp01(envelope);

            if (maxRFromAbove > p.currentR + kEps && envelope > 0.0f) {
                float targetR = p.currentR + (maxRFromAbove - p.currentR) * envelope;
                p.current.x = g_refCenterX + p.dirX * targetR;
                p.current.y = g_refCenterY + p.dirY * targetR;
                ++envelopeAdjusted;
            }

            finalVerts[p.idx] = p.current;
        }
    }

    int iterations = std::max(0, g_opts.constraintIterations);
    float k = Clamp01(g_opts.stretchStiffness);
    float kPrime = iterations > 0 ? 1.0f - std::pow(1.0f - k, 1.0f / (float)iterations) : 0.0f;
    static constexpr float kLenEps = 1.0e-6f;
    if (kPrime > 0.0f && !edges.empty()) {
        for (int iter = 0; iter < iterations; ++iter) {
            for (const auto& e : edges) {
                if (e.a >= finalVerts.size() || e.b >= finalVerts.size())
                    continue;

                float w1 = invMass[e.a];
                float w2 = invMass[e.b];
                float wSum = w1 + w2;
                if (wSum <= kLenEps)
                    continue;

                Vec3 dir = Sub(finalVerts[e.a], finalVerts[e.b]);
                float len = Len(dir);
                if (len <= kLenEps)
                    continue;

                Vec3 delta = Scale(dir, ((len - e.restLength) / len) * kPrime / wSum);
                if (w1 > 0.0f)
                    finalVerts[e.a] = Sub(finalVerts[e.a], Scale(delta, w1));
                if (w2 > 0.0f)
                    finalVerts[e.b] = Add(finalVerts[e.b], Scale(delta, w2));
                ++stretchAdjusted;
            }
        }
    }

    for (size_t i = 0; i < n; ++i) {
        Vec3 finalDiff = Sub(finalVerts[i], baseVerts[i]);
        uint16_t idx = static_cast<uint16_t>(i);
        if (Len2(finalDiff) <= 1.0e-10f) {
            auto it = diffs->find(idx);
            if (it != diffs->end()) {
                diffs->erase(it);
                ++erased;
            }
            continue;
        }

        (*diffs)[idx] = ToVector3(finalDiff);
        ++written;
    }

    if (g_appliedShapesLogged.insert(shapeName).second) {
        HLog("detour: smart drape shape '%s' first slider '%s': baseDiffVerts=%zu written=%d erased=%d envelopeAdjusted=%d stretchAdjusted=%d edges=%zu",
             shapeName.c_str(), sliderName.c_str(), baseDiffVerts, written, erased,
             envelopeAdjusted, stretchAdjusted, edges.size());
    }

    return true;
}

// â”€â”€ Detour â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static bool HostGetRawResultDiff(void* self,
                                 const std::string* shapeName,
                                 const std::string* sliderName,
                                 TargetDataDiffs& outDiff) {
    if (!g_getRawResultDiff || !self || !shapeName || !sliderName)
        return false;

#if defined(_MSC_VER)
    __try {
        g_getRawResultDiff(self, shapeName, sliderName, &outDiff);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        HLog("detour: GetRawResultDiff raised SEH for shape '%s' slider '%s'",
             shapeName->c_str(), sliderName->c_str());
        return false;
    }
#else
    g_getRawResultDiff(self, shapeName, sliderName, &outDiff);
    return true;
#endif
}

static bool HostSetResultDiff(void* self,
                              const std::string* shapeName,
                              const std::string* sliderName,
                              const TargetDataDiffs& diff) {
    if (!g_setResultDiff || !self || !shapeName || !sliderName)
        return false;

#if defined(_MSC_VER)
    __try {
        g_setResultDiff(self, shapeName, sliderName, &diff);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        HLog("detour: SetResultDiff raised SEH for shape '%s' slider '%s'",
             shapeName->c_str(), sliderName->c_str());
        return false;
    }
#else
    g_setResultDiff(self, shapeName, sliderName, &diff);
    return true;
#endif
}

static bool HostCopySourceDiff(void* self,
                               const std::string* refDataName,
                               TargetDataDiffs& outDiff) {
    if (!g_diffGetDiffSet || !self || !refDataName)
        return false;

#if defined(_MSC_VER)
    __try {
        void* srcDiffData = *reinterpret_cast<void**>(
            reinterpret_cast<char*>(self) + drape_off::kSrcDiffDataPtrOffset);
        if (!srcDiffData)
            return false;

        TargetDataDiffs* hostDiff = g_diffGetDiffSet(srcDiffData, refDataName);
        if (!hostDiff)
            return false;

        outDiff.clear();
        outDiff.reserve(hostDiff->size());
        for (const auto& kv : *hostDiff)
            outDiff[kv.first] = kv.second;
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        HLog("detour: source GetDiffSet raised SEH for refData '%s'",
             refDataName->c_str());
        return false;
    }
#else
    void* srcDiffData = *reinterpret_cast<void**>(
        reinterpret_cast<char*>(self) + drape_off::kSrcDiffDataPtrOffset);
    if (!srcDiffData)
        return false;
    TargetDataDiffs* hostDiff = g_diffGetDiffSet(srcDiffData, refDataName);
    if (!hostDiff)
        return false;
    outDiff = *hostDiff;
    return true;
#endif
}

struct StaticSolveParams {
    bool enabled = true;
    int bins = 48;
    float maxDisplacementMult = 3.0f;  // safety: finalDisp <= rawDisp * mult
    // Inward suppression: for drape-eligible loose verts, cancel the inward
    // radial component of the vanilla conform delta proportional to looseness.
    // 0 = keep vanilla (skin-tight), 1 = fully suppress inward concavity following.
    float inwardSuppressionStrength = 0.0f;
    float clearanceStrength = 0.50f;
    float clearanceSlack = 0.35f;
    float bodyCollisionStrength = 0.55f;
    float bodyClearance = 0.20f;
    float bodyGapPreserve = 0.45f;
    float bodyGapCap = 2.0f;
    float bodyColumnStrength = 0.45f;
    float bodyColumnRelaxPerUnitZ = 0.06f;
    float envelopeStrength = 0.50f;
    float relaxPerUnitZ = 0.08f;
    float layerStrength = 0.30f;
    int layerIterations = 1;
    // Experimental layer-order constraint. Disabled for the stable baseline:
    // it did not solve no-room covered clipping and should not affect production
    // conforms until validated on normal outfits.
    float orderConstraintStrength = 0.0f;
    float orderConstraintMaxStep = 0.10f;
    float orderConstraintBodyFloor = 0.08f;
    int constraintIterations = 2;
    float stretchStiffness = 0.35f;
    int surfaceSmoothIterations = 1;
    float surfaceSmoothStrength = 0.08f;
    // Late tangential de-wrinkle (belt): runs after the deforming passes so it
    // sticks; tangential (preserves body gap) + boundary-skip + edge-cap so it
    // flattens folds without melting straps/buckle or opening the silhouette.
    int beltDeWrinkleIterations = 0;
    float beltDeWrinkleStrength = 0.0f;
    // Late "seat" pass (belt): vanilla Conform floats the belt ~2u off the body
    // (a hula hoop above the dress). Pull each vert IN along the body normal toward
    // a target gap that lands it just above where the dress sits, clamped so it
    // never crosses the target (no body penetration). Follows the body contour ->
    // de-hoops it. Target ~= dress body-gap (~0.65) + leather thickness.
    int beltSeatIterations = 0;
    float beltSeatStrength = 0.0f;
    float beltSeatTargetGap = 0.9f;
    float beltSeatMaxStep = 0.35f;
    // Belt flap drape (scoped PBD): anchor the top cord to the body, then let the
    // rest of the wrap HANG under gravity with shape-preserving edges and one-way
    // (push-out only) body collision, so the flap drapes instead of hugging.
    float beltAnchorFrac = 0.30f;      // top fraction of the wrap = anchored cord
    int   beltDrapeIterations = 0;     // PBD iterations for the flap (0 = off)
    float beltDrapeGravity = 0.0f;     // downward pull per iteration (stiff leather = small)
    float beltDrapeFloor = 0.15f;      // min body gap kept during drape (push-out only)
    float beltDrapeMaxGap = 3.0f;      // flap can't float past this off the body (kills flank hover)
    int   beltDrapeEdgePasses = 4;     // Gauss-Seidel edge-length passes per iteration
    int pbdIterations = 8;
    float gravity = 0.0f;
    float anchorStiffness = 0.55f;
    float collisionStiffness = 0.90f;
    float limbGravityDamp = 0.55f;
    float torsoAnchorBoost = 0.30f;
    float drapeReturnRate = 0.0f;
    float drapeEnvelopeStrength = 0.80f;
    int   drapeEnvelopeBins = 64;
    // Physics drape mode (LOOSE_DRAPED): start from base mesh, let physics
    // (body collision, gravity, stretch, envelope) determine cloth position.
    // supportVerts is only used as anchor target for TIGHT verts.
    // Contrast with correction mode (false): solver corrects vanilla conform.
    bool physicsDrape = false;
    float physicsDrapeInitPushStiffness = 0.80f;
    int   physicsDrapeInitPushPasses    = 8;
    // Bending constraints: resist folding along shared edges (fabric stiffness).
    // 0 = disabled (stretch only â†’ chain-mail look). ~0.001 = soft cloth.
    float bendingStiffness = 0.0f;
    int   bendingIterations = 1;    // extra bending passes per PBD iteration
    // Real Verlet cloth solve (physicsDrape only). pbdIterations is reused as the
    // substep count; constraintIterations as the per-substep constraint passes.
    // gravity is the per-substep downward pull (Skyrim units). Distance
    // constraints resist ONLY stretching, so the cloth folds and hangs.
    bool  verletCloth = false;
    float clothDamping = 0.06f;     // velocity damping per substep
    float clothMaxDrop = 10.0f;     // max Z a drape vert may sag below conformed fit
};

struct StaticSolveStats {
    size_t rawDiffVerts = 0;
    int clearanceAdjusted = 0;
    int bodyAdjusted = 0;
    int bodySamples = 0;
    int bodyColumnAdjusted = 0;
    int envelopeAdjusted = 0;
    int layerAdjusted = 0;
    int stretchAdjusted = 0;
    int smoothAdjusted = 0;
    int gravityAdjusted = 0;
    int collisionAdjusted = 0;
    int anchorAdjusted = 0;
    int bendingAdjusted = 0;
    int written = 0;
    // Per-vertex region eligibility
    int drapeEligibleVerts = 0;
    int anchoredVerts = 0;
    int tubularVerts = 0;
    int influenceDirectVerts = 0;
    int influencePropagVerts = 0;
    int influenceZeroVerts = 0;
    bool physicsDrapeActive = false;  // was physicsDrape mode actually used?
};

// Belt-as-cloth PoC (drape solver route). FINDING (2026-07-01, harness): routing the
// belt through verletCloth+physicsDrape gives lower edge-stretch (3.51->2.82) and NO
// magnetism, but it FLOATS (main ~1.34, strap ~4.5) because the belt's vanilla conform
// floats it uniformly and the drape solver has no on-body anchor to pull it in (the
// dress works only because its fitted top anchors to an on-body vanilla position; the
// belt has none). Pinning per-island tops to contact was not enough (stretch-preserved
// bodies keep the vanilla out-shape; leather-gravity too weak to straighten). The
// correct fix is the plan's Phase 1 SKIN-BIND (bind each vert to a base-body point +
// clearance, move it with the morphed body) to establish contact by construction -
// a careful gated phase, not a blind param tweak. Flag left OFF per plan discipline;
// drape-solver code kept in place (disable-by-flag before delete).
static constexpr bool kBeltDrapeSolver = false;

// Belt SKIN-BIND (plan Phase 1a for the belt). The real cause of both the float AND the
// magnetism is that the belt is positioned by the vanilla conform (over-projects = float)
// then corrected by inward nudges (= magnetism). FINDING (2026-07-01, harness):
//  * Topology transfers cleanly (base & morphed body share 26412 tris); binding works.
//  * KEY DISCOVERY: the belt is AUTHORED LOOSE - base clearance median ~2.1u (range
//    0.7-3.2). The "float" is largely the DESIGN, not the conform (vanilla gap 1.95 ~=
//    base clearance 2.10). Faithful skin-bind preserves it -> still floats.
//  * De-floating (clearance scale or fixed inset) has TWO hard conflicts: (a) it distorts
//    (non-uniform move -> edge-stretch 3.5->3.9-4.1), and (b) it drives the belt BELOW the
//    dress surface (dress ~0.5 sits between body and belt) -> dress clips through the belt
//    (Clothes faceResidual 43 -> 114-227). Insetting toward the BODY ignores the dress.
//  * Even faithful skin-bind adds stretch (4.07): the face-normal offset is approximate at
//    edges/corners; a correct version needs deformation-gradient transfer.
//  CONCLUSION: a clean de-float needs the plan's Phase 1b ARAP (deform tighter, minimise
//  distortion) + Phase 3 layering (stay above the dress) - a gated libigl integration, not
//  a blind tweak. Flag left OFF; PoC code kept (disable-by-flag before delete).
static constexpr bool kBeltSkinBind = false;

// M1 (2026-07-02): the real fix from the CONCLUSION above. Bind each belt vert to the
// base body (barycentric anchor + AUTHORED signed offset); the same anchor evaluated on
// the morphed body is the target. A well-conditioned subset are ARAP HANDLES; ARAP fills
// the rest, minimising distortion (fixes the +stretch the raw skin-bind had, and keeps
// straps rigid instead of warping). Reproduces the AUTHORED belt<->support relationship:
// no uniform target => magnetism is structurally impossible (the user's rule).
//   M1a (this flag): binds to the BODY - the only support a lone belt capture has. This
//     reproduces the authored ~2u float WITHOUT magnetism/distortion. Per the user's own
//     rule, a belt with nothing pushing it in SHOULD sit at its authored clearance; the
//     visible de-float only happens when it RESTS ON something (the dress).
//   M1b (kBeltArapRestOn): when a solved inner garment (the dress) is available as its
//     support, bind to THAT instead of the body -> the belt rests on the dress = de-floats
//     for real, still no magnetism. Needs the dress solved before the belt (solve-order /
//     support-availability plumbing).
static constexpr bool kBeltArap = true;
static constexpr bool kBeltArapRestOn = true;  // prefer a solved inner-garment support when present

// M2 (2026-07-02): guarantee the rest-on solve ORDER live. M1b de-floats the belt onto
// the dress ONLY IF the dress happens to be solved before the belt in OutfitStudio's own
// call order (otherwise the belt rests on the VANILLA-conformed dress, which still carries
// magnetism). With this flag, when the belt is solved before its LOOSE_DRAPED support, the
// support is solved ON DEMAND first (depth-capped, deterministic, host never re-entered),
// cached in g_solvedShapes, and used as the belt's inner collider -> the belt always rests
// on OUR solved dress regardless of order. See BELT_AND_PIPELINE_REVIEW.md Part 12.
static constexpr bool kLayerSolveOrder = true;

static StaticSolveParams StaticParamsForPreset(int preset) {
    StaticSolveParams p;
    switch (NormalizePreset(preset)) {
    case DRAPE_PRESET_SKIN_TIGHT:
        p.enabled = false;
        break;
    case DRAPE_PRESET_FITTED_CLOTH:
        p.clearanceStrength = 0.12f;
        p.clearanceSlack = 0.45f;
        p.bodyCollisionStrength = 0.30f;
        p.bodyClearance = 0.08f;
        p.bodyGapPreserve = 0.18f;
        p.bodyGapCap = 1.25f;
        p.bodyColumnStrength = 0.08f;
        p.bodyColumnRelaxPerUnitZ = 0.18f;
        p.envelopeStrength = 0.08f;
        p.relaxPerUnitZ = 0.14f;
        p.layerStrength = 0.16f;
        p.constraintIterations = 2;
        p.stretchStiffness = 0.32f;
        p.surfaceSmoothIterations = 1;
        p.surfaceSmoothStrength = 0.08f;
        p.pbdIterations = 5;
        p.gravity = 0.35f;
        p.anchorStiffness = 0.75f;
        p.collisionStiffness = 0.90f;
        break;
    case DRAPE_PRESET_LOOSE_DRAPED:
        // Physics drape: start from BASE MESH (slim body), let physics determine
        // cloth position. supportVerts is only used as anchor target for tight verts.
        // This gives true cloth-aware behavior instead of vanilla-conform correction.
        p.physicsDrape = true;
        p.physicsDrapeInitPushStiffness = 0.80f;
        p.physicsDrapeInitPushPasses    = 8;
        p.maxDisplacementMult = 0.0f;
        p.inwardSuppressionStrength = 0.0f;
        p.clearanceStrength = 0.0f;
        p.clearanceSlack = 0.0f;
        p.bodyCollisionStrength = 0.0f;
        p.bodyGapPreserve = 0.0f;
        p.bodyGapCap = 0.0f;
        p.bodyColumnStrength = 0.0f;
        p.envelopeStrength = 0.0f;
        p.drapeEnvelopeStrength = 1.0f;
        p.drapeEnvelopeBins = 96;
        p.relaxPerUnitZ = 0.0f;
        p.layerStrength = 0.0f;
        p.layerIterations = 0;
        p.constraintIterations = 5;
        // Momentum-free cloth drape (stable Gauss-Seidel relaxation).
        // pbdIterations = relaxation steps; constraintIterations = passes/step;
        // gravity = per-step downward nudge; clothMaxDrop bounds the sag.
        // The monotonic radial envelope (run last) is the dominant shaper for
        // loose. The Verlet here is only a gentle settle â€” kept weak so it doesn't
        // melt the hem with uneven vertical sag.
        p.verletCloth = true;
        p.collisionStiffness = 0.85f;
        // Tuned drape (2026-06-05): moderate values â€” visibly looser than the body
        // (clearance 0.9) with a real but bounded sag. gravity is PER-SUBSTEP and
        // there are ~50 substeps, so keep it small (0.06 Ã— 50 â‰ˆ 3 units of pull);
        // clothMaxDrop bounds the total sag so the hem can't melt/tear.
        p.bodyClearance = 0.90f;       // looser than 0.65, not the 2.0 overdrive
        p.bendingStiffness = 0.10f;    // a touch stiffer â†’ fewer jagged folds
        p.bendingIterations = 2;
        p.surfaceSmoothIterations = 4; // extra smoothing â†’ less jagged hem
        p.surfaceSmoothStrength = 0.18f;
        p.anchorStiffness = 0.50f;
        p.drapeReturnRate = 0.028f;
        p.drapeEnvelopeStrength = 1.0f;
        p.drapeEnvelopeBins = 96;
        p.gravity = 0.07f;             // per-substep; Ã—50 â‰ˆ 3.5 units total sag
        p.clothMaxDrop = 8.0f;         // bounded sag â€” prevents melt/tear at hem
        p.stretchStiffness = 0.75f;    // stiffer cloth â†’ resists tearing
        // Processing intensity: Low=faster/fewer passes, Medium=default,
        // High=extra iterations for difficult extreme morphs.
        switch (g_processingIntensity.load(std::memory_order_relaxed)) {
        case 0: // Low
            p.pbdIterations       = 25;
            p.constraintIterations = 3;
            break;
        default: // Medium (1)
            p.pbdIterations       = 50;
            p.constraintIterations = 5;
            break;
        case 2: // High
            p.pbdIterations       = 90;
            p.constraintIterations = 7;
            break;
        }
        break;
    case DRAPE_PRESET_TUBULAR:
        p.clearanceStrength = 0.18f;
        p.clearanceSlack = 0.35f;
        p.bodyCollisionStrength = 0.48f;
        p.bodyClearance = 0.10f;
        p.bodyGapPreserve = 0.16f;
        p.bodyGapCap = 1.25f;
        p.bodyColumnStrength = 0.00f;
        p.bodyColumnRelaxPerUnitZ = 0.25f;
        p.envelopeStrength = 0.03f;
        p.relaxPerUnitZ = 0.22f;
        p.layerStrength = 0.10f;
        p.constraintIterations = 2;
        p.stretchStiffness = 0.32f;
        p.surfaceSmoothIterations = 1;
        p.surfaceSmoothStrength = 0.06f;
        p.pbdIterations = 5;
        p.gravity = 0.12f;
        p.anchorStiffness = 0.70f;
        p.collisionStiffness = 0.92f;
        break;
    case DRAPE_PRESET_FLEXIBLE_ARMOR:
        p.clearanceStrength = 0.55f;
        p.clearanceSlack = 0.10f;
        p.bodyCollisionStrength = 0.58f;
        p.bodyClearance = 0.28f;
        p.bodyGapPreserve = 0.55f;
        p.bodyColumnStrength = 0.38f;
        p.bodyColumnRelaxPerUnitZ = 0.055f;
        p.envelopeStrength = 0.45f;
        p.relaxPerUnitZ = 0.07f;
        p.layerStrength = 0.32f;
        p.constraintIterations = 3;
        p.stretchStiffness = 0.55f;
        p.pbdIterations = 6;
        p.gravity = 0.45f;
        p.anchorStiffness = 0.70f;
        p.collisionStiffness = 0.94f;
        break;
    case DRAPE_PRESET_RIGID_ARMOR:
        p.clearanceStrength = 0.42f;
        p.clearanceSlack = 0.06f;
        p.bodyCollisionStrength = 0.46f;
        p.bodyClearance = 0.35f;
        p.bodyGapPreserve = 0.70f;
        p.bodyColumnStrength = 0.28f;
        p.bodyColumnRelaxPerUnitZ = 0.040f;
        p.envelopeStrength = 0.32f;
        p.relaxPerUnitZ = 0.05f;
        p.layerStrength = 0.36f;
        p.constraintIterations = 4;
        p.stretchStiffness = 0.72f;
        p.pbdIterations = 4;
        p.gravity = 0.05f;
        p.anchorStiffness = 0.85f;
        p.collisionStiffness = 0.95f;
        break;
    case DRAPE_PRESET_BELT:
        if (kBeltDrapeSolver) {
            // Belt as CLOTH via the real drape solver (fixes the "magnetism" at the
            // cause instead of stacking post-solve nudges). verletCloth settles it ONTO
            // the body (kills the vanilla float); drapeReturnRate stops it snapping back
            // into hollows (bridges concavities = the anti-magnetism, by construction).
            // Tuned STIFF (leather): high stretch stiffness, higher bending, small
            // gravity, short bounded sag. All inward-pulling correction passes zeroed
            // (physics owns position, like LOOSE_DRAPED). The belt* nudge params stay 0
            // so applyBeltFinishing is disabled (kept in code for comparison).
            p.physicsDrape = true;
            p.verletCloth = true;
            p.physicsDrapeInitPushStiffness = 0.80f;
            p.physicsDrapeInitPushPasses = 8;
            p.maxDisplacementMult = 0.0f;
            p.inwardSuppressionStrength = 0.0f;
            p.clearanceStrength = 0.0f;
            p.clearanceSlack = 0.0f;
            p.bodyCollisionStrength = 0.0f;
            p.bodyGapPreserve = 0.0f;
            p.bodyColumnStrength = 0.0f;
            p.envelopeStrength = 0.0f;
            p.relaxPerUnitZ = 0.0f;
            p.layerStrength = 0.0f;
            p.layerIterations = 0;
            p.drapeEnvelopeStrength = 0.0f;
            p.drapeEnvelopeBins = 96;
            p.drapeReturnRate = 0.028f;   // anti-magnetism: bridge hollows, don't snap in
            p.bodyClearance = 0.45f;      // leather sits close to the dress
            p.bendingStiffness = 0.30f;   // stiff leather: soft, few folds
            p.bendingIterations = 3;
            p.surfaceSmoothIterations = 3;
            p.surfaceSmoothStrength = 0.14f;
            p.anchorStiffness = 0.55f;
            p.gravity = 0.05f;            // short piece: modest sag
            p.clothMaxDrop = 2.5f;        // bounded (belt is short)
            p.stretchStiffness = 0.90f;   // leather barely stretches
            p.collisionStiffness = 0.90f;
            p.constraintIterations = 5;
            p.pbdIterations = 50;
            break;
        }
        p.clearanceStrength = 0.82f;
        p.clearanceSlack = 0.04f;
        p.bodyCollisionStrength = 0.88f;
        p.bodyClearance = 0.12f;
        p.bodyGapPreserve = 0.35f;
        p.bodyColumnStrength = 0.52f;
        p.bodyColumnRelaxPerUnitZ = 0.030f;
        p.envelopeStrength = 0.90f;
        p.relaxPerUnitZ = 0.015f;
        p.layerStrength = 0.42f;
        p.layerIterations = 2;
        p.constraintIterations = 4;
        p.stretchStiffness = 0.82f;
        p.pbdIterations = 5;
        p.gravity = 0.04f;
        p.anchorStiffness = 0.90f;
        p.collisionStiffness = 0.96f;
        // Legacy post-solve nudge stack (the seat/drape/de-wrinkle "magnetism" tower).
        // RETIRED by M1 (kBeltArap) - the bind+ARAP override owns the belt result now.
        // Kept flag-gated (delete-by-flag) so flipping kBeltArap OFF restores it for
        // comparison. Zeroed when kBeltArap is ON so applyBeltFinishing doesn't run.
        if (!kBeltArap) {
            p.beltSeatIterations = 6;
            p.beltSeatStrength = 0.85f;
            p.beltSeatTargetGap = 0.5f;
            p.beltSeatMaxStep = 0.35f;
            p.beltAnchorFrac = 0.30f;
            p.beltDrapeIterations = 24;
            p.beltDrapeGravity = 0.08f;
            p.beltDrapeFloor = 0.15f;
            p.beltDrapeMaxGap = 5.0f;
            p.beltDrapeEdgePasses = 5;
            p.beltDeWrinkleIterations = 6;
            p.beltDeWrinkleStrength = 0.45f;
        }
        break;
    case DRAPE_PRESET_RIGID_ACCESSORY:
        p.clearanceStrength = 0.20f;
        p.clearanceSlack = 0.08f;
        p.bodyCollisionStrength = 0.22f;
        p.bodyClearance = 0.25f;
        p.bodyGapPreserve = 0.80f;
        p.bodyColumnStrength = 0.06f;
        p.bodyColumnRelaxPerUnitZ = 0.20f;
        p.envelopeStrength = 0.12f;
        p.relaxPerUnitZ = 0.20f;
        p.layerStrength = 0.18f;
        p.constraintIterations = 4;
        p.stretchStiffness = 0.90f;
        p.pbdIterations = 3;
        p.gravity = 0.00f;
        p.anchorStiffness = 0.90f;
        p.collisionStiffness = 0.90f;
        break;
    default:
        break;
    }

    p.constraintIterations = std::max(p.constraintIterations, g_opts.constraintIterations);
    p.stretchStiffness = std::max(p.stretchStiffness, g_opts.stretchStiffness);
    return p;
}

static Vec3 DiffAt(const TargetDataDiffs& diffs, uint16_t index) {
    auto it = diffs.find(index);
    return it == diffs.end() ? Vec3{} : ToVec3(it->second);
}

static float RadialRadius(const Vec3& v) {
    float x = v.x - g_refCenterX;
    float y = v.y - g_refCenterY;
    return std::sqrt(x * x + y * y);
}

static bool RadialDir(const Vec3& v, float& outX, float& outY, float& outR) {
    float x = v.x - g_refCenterX;
    float y = v.y - g_refCenterY;
    outR = std::sqrt(x * x + y * y);
    if (outR <= 1.0e-5f)
        return false;
    outX = x / outR;
    outY = y / outR;
    return true;
}

static int RadialBin(float dirX, float dirY, int bins) {
    static constexpr float kPi = 3.14159265358979323846f;
    static constexpr float kTwoPi = 2.0f * kPi;
    float angle = std::atan2(dirY, dirX);
    int bin = static_cast<int>(((angle + kPi) / kTwoPi) * (float)bins);
    if (bin < 0) bin = 0;
    if (bin >= bins) bin = bins - 1;
    return bin;
}

static int BodyShellIndex(const BodyShell& shell, int radialBin, int zBin) {
    return zBin * shell.radialBins + radialBin;
}

static bool BuildBodyShell(const std::vector<Vec3>& bodyVerts,
                           const TargetDataDiffs& bodyDiff,
                           BodyShell& shell) {
    shell = BodyShell{};
    if (bodyVerts.empty())
        return false;

    shell.radialBins = 48;
    shell.zBins = 96;
    shell.minZ = bodyVerts[0].z;
    shell.maxZ = bodyVerts[0].z;
    for (const Vec3& v : bodyVerts) {
        shell.minZ = std::min(shell.minZ, v.z);
        shell.maxZ = std::max(shell.maxZ, v.z);
    }

    float zSpan = std::max(1.0f, shell.maxZ - shell.minZ);
    shell.minZ -= zSpan * 0.03f;
    shell.maxZ += zSpan * 0.03f;
    shell.invDz = (float)(shell.zBins - 1) / std::max(1.0e-4f, shell.maxZ - shell.minZ);

    size_t cells = (size_t)shell.radialBins * (size_t)shell.zBins;
    shell.baseR.assign(cells, -1.0f);
    shell.morphR.assign(cells, -1.0f);
    shell.occupied.assign(cells, 0);

    int inserted = 0;
    for (size_t i = 0; i < bodyVerts.size() && i <= 0xFFFF; ++i) {
        const Vec3& base = bodyVerts[i];
        Vec3 morphed = Add(base, DiffAt(bodyDiff, static_cast<uint16_t>(i)));

        float dirX = 0.0f, dirY = 0.0f, baseR = 0.0f;
        if (!RadialDir(base, dirX, dirY, baseR))
            continue;

        int rb = RadialBin(dirX, dirY, shell.radialBins);
        int zb = static_cast<int>((base.z - shell.minZ) * shell.invDz);
        zb = std::max(0, std::min(shell.zBins - 1, zb));

        float morphR = RadialRadius(morphed);
        int idx = BodyShellIndex(shell, rb, zb);
        shell.baseR[idx] = std::max(shell.baseR[idx], baseR);
        shell.morphR[idx] = std::max(shell.morphR[idx], morphR);
        shell.occupied[idx] = 1;
        ++inserted;
    }

    shell.valid = inserted > 0;
    return shell.valid;
}

static bool SampleBodyShell(const BodyShell& shell,
                            const Vec3& basePoint,
                            float& outBaseR,
                            float& outMorphR) {
    if (!shell.valid || shell.baseR.empty())
        return false;

    float dirX = 0.0f, dirY = 0.0f, pointR = 0.0f;
    if (!RadialDir(basePoint, dirX, dirY, pointR))
        return false;

    int rb0 = RadialBin(dirX, dirY, shell.radialBins);
    int zb0 = static_cast<int>((basePoint.z - shell.minZ) * shell.invDz);
    zb0 = std::max(0, std::min(shell.zBins - 1, zb0));

    bool found = false;
    float bestBase = -1.0f;
    float bestMorph = -1.0f;
    for (int dz = 0; dz <= 4; ++dz) {
        for (int da = 0; da <= 2; ++da) {
            for (int zSign = -1; zSign <= 1; zSign += 2) {
                int zb = zb0 + dz * zSign;
                if (dz == 0 && zSign > 0)
                    continue;
                if (zb < 0 || zb >= shell.zBins)
                    continue;

                for (int aSign = -1; aSign <= 1; aSign += 2) {
                    int rb = rb0 + da * aSign;
                    if (da == 0 && aSign > 0)
                        continue;
                    while (rb < 0) rb += shell.radialBins;
                    while (rb >= shell.radialBins) rb -= shell.radialBins;

                    int idx = BodyShellIndex(shell, rb, zb);
                    if (!shell.occupied[idx])
                        continue;

                    bestBase = std::max(bestBase, shell.baseR[idx]);
                    bestMorph = std::max(bestMorph, shell.morphR[idx]);
                    found = true;
                }
            }
        }
        if (found)
            break;
    }

    if (!found || bestBase < 0.0f || bestMorph < 0.0f)
        return false;

    outBaseR = bestBase;
    outMorphR = bestMorph;
    return true;
}

// Returns the magnitude of body change at a given 3D position for this slider.
// Uses the existing body shell (|morphR - baseR| per cell). Used for slider-source
// locality: a Belly slider has large change at the abdomen, near-zero at the ankle.
static float SampleBodyChangeMagnitude(const BodyShell& shell, const Vec3& p) {
    float baseR = 0.0f, morphR = 0.0f;
    if (!SampleBodyShell(shell, p, baseR, morphR)) return 0.0f;
    return std::fabs(morphR - baseR);
}

static bool MoveRadiallyOut(Vec3& v,
                            float dirX,
                            float dirY,
                            float targetR,
                            float strength) {
    strength = Clamp01(strength);
    if (strength <= 0.0f)
        return false;

    float curR = RadialRadius(v);
    if (targetR <= curR + 1.0e-5f)
        return false;

    float newR = curR + (targetR - curR) * strength;
    v.x = g_refCenterX + dirX * newR;
    v.y = g_refCenterY + dirY * newR;
    return true;
}

static void ApplyClearancePass(std::vector<Vec3>& finalVerts,
                               const std::vector<Vec3>& supportVerts,
                               const std::vector<float>& follow,
                               const StaticSolveParams& params,
                               StaticSolveStats& stats) {
    for (size_t i = 0; i < finalVerts.size(); ++i) {
        float dirX = 0.0f, dirY = 0.0f, supportR = 0.0f;
        if (!RadialDir(supportVerts[i], dirX, dirY, supportR))
            continue;

        float loose = 1.0f - (i < follow.size() ? Clamp01(follow[i]) : 1.0f);
        float targetR = std::max(0.0f, supportR - params.clearanceSlack);
        float strength = params.clearanceStrength * ClampRange(0.35f + 0.65f * loose, 0.0f, 1.0f);
        if (MoveRadiallyOut(finalVerts[i], dirX, dirY, targetR, strength))
            ++stats.clearanceAdjusted;
    }
}

static void ApplyBodyShellCollisionPass(std::vector<Vec3>& finalVerts,
                                        const std::vector<Vec3>& baseVerts,
                                        const std::vector<Vec3>& supportVerts,
                                        const std::vector<float>& follow,
                                        const BodyShell* bodyShell,
                                        const StaticSolveParams& params,
                                        StaticSolveStats& stats) {
    if (!bodyShell || !bodyShell->valid || params.bodyCollisionStrength <= 0.0f)
        return;

    for (size_t i = 0; i < finalVerts.size(); ++i) {
        float baseBodyR = 0.0f;
        float morphBodyR = 0.0f;
        if (!SampleBodyShell(*bodyShell, baseVerts[i], baseBodyR, morphBodyR))
            continue;

        float dirX = 0.0f, dirY = 0.0f, baseClothR = 0.0f;
        if (!RadialDir(baseVerts[i], dirX, dirY, baseClothR) &&
            !RadialDir(supportVerts[i], dirX, dirY, baseClothR)) {
            continue;
        }

        float loose = 1.0f - (i < follow.size() ? Clamp01(follow[i]) : 1.0f);
        float baseGap = std::max(0.0f, baseClothR - baseBodyR);
        float rawSupportR = RadialRadius(supportVerts[i]);

        float preservedGap = baseGap * params.bodyGapPreserve;
        float targetR = morphBodyR + params.bodyClearance + preservedGap;
        targetR = std::max(targetR, rawSupportR - params.clearanceSlack);

        float strength = params.bodyCollisionStrength *
            ClampRange(0.40f + 0.60f * loose, 0.0f, 1.0f);
        if (MoveRadiallyOut(finalVerts[i], dirX, dirY, targetR, strength))
            ++stats.bodyAdjusted;
        ++stats.bodySamples;
    }
}

// Cloth-based drape envelope: prevents the outfit from returning inward too quickly
// after a body protrusion (e.g., belly, hips). Uses the cloth's OWN current radial
// distances, NOT the body shell, to set the envelope floor.
//
// Algorithm: for each angular bin, scan verts top-to-bottom. Track the maximum
// outward radius seen so far (envelopeFloor). The floor relaxes by returnRatePerUnitZ
// per unit of Z descent. Verts below the floor get pushed outward.
//
// Example (Belly=100%): belly pushes dress out to R=14. Below belly, the vanilla
// conform has the dress returning to R=8. With returnRate=0.05 and 40 units of Z,
// the envelope floor decays to 14 - 0.05*40 = 12. Dress hem pushed to R=12
// instead of collapsing back to R=8.
static void ApplyClothDrapeEnvelopePass(
    std::vector<Vec3>& finalVerts,
    const std::vector<float>& drapeEl,
    const std::vector<float>& effectiveInfluence,
    int radialBins,
    float returnRatePerUnitZ,
    float strength,
    StaticSolveStats& stats,
    bool ignoreInfluence = false)
{
    size_t n = finalVerts.size();
    // returnRate == 0 is valid: pure monotonic envelope (radius never decreases
    // going down) â€” the hardcoded "loose" rule. Only negative is invalid.
    if (n == 0 || radialBins <= 0 || returnRatePerUnitZ < 0.0f || strength <= 0.0f)
        return;

    struct ClothPt { uint16_t idx; float z, r, dirX, dirY; };
    std::vector<std::vector<ClothPt>> bins(radialBins);
    bins.reserve(radialBins);

    for (size_t i = 0; i < n && i <= 0xFFFF; ++i) {
        float eff = (i < effectiveInfluence.size()) ? effectiveInfluence[i] : 1.0f;
        // ignoreInfluence: enforce the monotonic radial rule on EVERY drape vert,
        // not just ones the active slider changed. Slider-source influence must not
        // gate the loose-cloth envelope, or the rule fails exactly in the recessed
        // areas (under-belly, groin) where the body changed little.
        if (drapeEl[i] < 0.15f) continue;
        if (!ignoreInfluence && eff < 0.05f) continue;

        float dirX = 0.0f, dirY = 0.0f, r = 0.0f;
        if (!RadialDir(finalVerts[i], dirX, dirY, r)) continue;

        int bin = RadialBin(dirX, dirY, radialBins);
        bins[bin].push_back({static_cast<uint16_t>(i), finalVerts[i].z, r, dirX, dirY});
    }

    for (auto& bin : bins) {
        if (bin.size() < 2) continue;
        std::sort(bin.begin(), bin.end(),
                  [](const ClothPt& a, const ClothPt& b) { return a.z > b.z; });

        float envelopeFloor = bin[0].r;
        float lastZ         = bin[0].z;

        for (size_t pi = 1; pi < bin.size(); ++pi) {
            auto& p  = bin[pi];
            float dZ = std::max(0.0f, lastZ - p.z);

            // Relax the envelope floor by return rate per unit Z
            float curFloor = std::max(0.0f, envelopeFloor - returnRatePerUnitZ * dZ);

            // Push vert outward if below envelope floor
            if (p.r < curFloor - 0.01f) {
                float eff    = (!ignoreInfluence && p.idx < effectiveInfluence.size())
                               ? effectiveInfluence[p.idx] : 1.0f;
                float newR   = p.r + (curFloor - p.r) * strength * eff;
                finalVerts[p.idx].x = g_refCenterX + p.dirX * newR;
                finalVerts[p.idx].y = g_refCenterY + p.dirY * newR;
                p.r = newR;
                ++stats.envelopeAdjusted;
            }

            // Update floor: new maximum (protrusion raises floor, return rate lowers it)
            envelopeFloor = std::max(curFloor, p.r);
            lastZ = p.z;
        }
    }
}

// â”€â”€ Body-shell monotonic drape envelope (the hardcoded LOOSE rule) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Implements the user's rule using the dense, smooth BODY SHELL instead of the
// sparse cloth verts: going DOWN, the cloth radius per angular sector can never
// fall below the maximum body radius seen from the top of the drape region down
// to that height (+ clearance). So the cloth bulges out at protrusions (breast,
// belly, butt) and hangs straight down (vertical) across the recessed parts
// (under-belly, waist, groin) â€” it never tucks back toward the center. Because
// the body shell is dense (48Ã—96) and smooth, this is smooth and acts on EVERY
// drape vert regardless of how few cloth verts fall in a bin (the flaw that made
// the cloth-vert envelope weak and jagged).
static void ApplyBodyEnvelopeDrapePass(
    std::vector<Vec3>& finalVerts,
    const std::vector<float>& drapeEl,
    const BodyShell& shell,
    float clearance,
    float strength,
    float returnRate,          // how fast envelope can return inward per unit Z
                               // 0 = strict column (never narrows)
                               // 0.01-0.02 = loose drape (slow taper below belly)
    bool twoSided,             // false: only push verts that are INSIDE out to the
                               //   floor (one-sided minimum).
                               // true: also pull OUTWARD bumps in to the envelope
                               //   surface â†’ flattens the cloth onto the smooth
                               //   monotonic envelope. This is the radial collision
                               //   AND the de-jagger in one; the result is the
                               //   monotonic surface, so it still obeys the loose
                               //   rule (envR is >= body and never decreases down).
    StaticSolveStats& stats)
{
    if (!shell.valid || shell.morphR.empty()) return;
    const size_t n = finalVerts.size();
    const int RB = shell.radialBins;
    const int ZB = shell.zBins;
    if (RB <= 0 || ZB <= 0) return;

    const float dzPerBin = (ZB > 1 && shell.invDz > 0.0f)
        ? (1.0f / shell.invDz) : 1.0f;  // world-units per Z bin

    auto zBinOf = [&](float z) {
        int zb = static_cast<int>((z - shell.minZ) * shell.invDz);
        return std::max(0, std::min(ZB - 1, zb));
    };

    // Top of the drape region: highest Z among drape verts (the belt/waist).
    int drapeTopZb = 0;
    bool any = false;
    for (size_t i = 0; i < n && i <= 0xFFFF; ++i) {
        if (drapeEl[i] < 0.20f) continue;
        drapeTopZb = std::max(drapeTopZb, zBinOf(finalVerts[i].z));
        any = true;
    }
    if (!any) return;

    // Seed the running max at the top of the drape region (belt/waist). Seeding
    // higher (to capture the belly overhang) was tried but on a heavy body it
    // forced the whole skirt to the belly/hip width down to the hem â†’ a stiff
    // wide bell. The belly-overhang problem needs a different fix (per-contact
    // vertical drop, not a global radius floor), so keep the seed at the belt.
    int seedTopZb = drapeTopZb;

    // Per angular bin: envelope scans DOWN from seedTopZb (above the belly).
    // envelopeR = max(bodyR, prev_envelopeR - returnRate * dzPerBin).
    // returnRate=0  â†’ strict column (never narrows below the widest point).
    // returnRate>0  â†’ slow taper (hangs gradually inward after a protrusion).
    std::vector<float> envR((size_t)RB * ZB, -1.0f);
    for (int rb = 0; rb < RB; ++rb) {
        float runEnv = -1.0f;
        for (int zb = seedTopZb; zb >= 0; --zb) {
            float bodyR = shell.morphR[(size_t)zb * RB + rb];
            if (bodyR < 0.0f) bodyR = 0.0f;
            if (runEnv < 0.0f) {
                runEnv = bodyR;
            } else {
                runEnv = std::max(bodyR, runEnv - returnRate * dzPerBin);
            }
            envR[(size_t)zb * RB + rb] = runEnv;
        }
    }

    // Light angular smoothing (wrap-around) to avoid per-bin facets.
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<float> tmp = envR;
        for (int zb = 0; zb <= seedTopZb; ++zb) {
            for (int rb = 0; rb < RB; ++rb) {
                int rbm = (rb - 1 + RB) % RB, rbp = (rb + 1) % RB;
                float a = envR[(size_t)zb * RB + rbm];
                float b = envR[(size_t)zb * RB + rb];
                float c = envR[(size_t)zb * RB + rbp];
                float sum = 0.0f; int cnt = 0;
                if (a >= 0.0f) { sum += a; ++cnt; }
                if (b >= 0.0f) { sum += b; ++cnt; }
                if (c >= 0.0f) { sum += c; ++cnt; }
                if (cnt > 0) tmp[(size_t)zb * RB + rb] = sum / (float)cnt;
            }
        }
        envR = std::move(tmp);
    }

    // Project each drape vert outward to its envelope radius + clearance.
    for (size_t i = 0; i < n && i <= 0xFFFF; ++i) {
        if (drapeEl[i] < 0.20f) continue;
        float dirX = 0.0f, dirY = 0.0f, r = 0.0f;
        if (!RadialDir(finalVerts[i], dirX, dirY, r)) continue;
        int rb = RadialBin(dirX, dirY, RB);
        int zb = zBinOf(finalVerts[i].z);
        float target = envR[(size_t)zb * RB + rb];
        if (target < 0.0f) continue;
        target += clearance;
        bool pushOut = r < target - 0.01f;
        bool pullIn  = twoSided && r > target + 0.01f;
        if (pushOut || pullIn) {
            float newR = r + (target - r) * strength;
            finalVerts[i].x = g_refCenterX + dirX * newR;
            finalVerts[i].y = g_refCenterY + dirY * newR;
            ++stats.envelopeAdjusted;
        }
    }
}

// â”€â”€ Hanging-chain pass (user spec item 5) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Instead of reading the body shell only at each vertex's own Z (which is blind
// to a protrusion ABOVE the drape region, e.g. a belly that overhangs the belt),
// propagate the contact radius DOWN through the cloth's own edge graph:
//   process verts topâ†’bottom; each drape vert's radius is at least the radius of
//   the connected vert ABOVE it (minus a tiny per-Z taper) and at least the body
//   radius at its position. Anchored verts (drapeEl<0.2) are NOT moved â€” they are
//   the seeds/contacts (the bodice carries the belly/bust radius into the chain).
// Effect: once a vert is pushed out by a contact, everything hanging below it in
// the same cloth column stays out and falls vertical â€” it never returns inward
// unless a new (wider) contact appears. Two-sided projection also de-jags.
static void ApplyHangingChainPass(
    std::vector<Vec3>& finalVerts,
    const std::vector<float>& drapeEl,
    const std::vector<std::vector<uint32_t>>& adj,  // prebuilt vert adjacency
    const BodyShell* shell,
    float clearance,
    float returnRate,
    StaticSolveStats& stats)
{
    const size_t n = finalVerts.size();
    std::vector<float> radius(n, -1.0f), dirx(n, 0.0f), diry(n, 0.0f), zpos(n, 0.0f);
    for (size_t i = 0; i < n && i <= 0xFFFF; ++i) {
        float dx = 0.0f, dy = 0.0f, r = 0.0f;
        if (RadialDir(finalVerts[i], dx, dy, r)) { radius[i] = r; dirx[i] = dx; diry[i] = dy; }
        zpos[i] = finalVerts[i].z;
    }

    std::vector<uint32_t> order;
    order.reserve(n);
    for (size_t i = 0; i < n && i <= 0xFFFF; ++i)
        if (radius[i] >= 0.0f) order.push_back((uint32_t)i);
    std::sort(order.begin(), order.end(),
              [&](uint32_t a, uint32_t b) { return zpos[a] > zpos[b]; });

    for (uint32_t idx : order) {
        if (drapeEl[idx] < 0.20f) continue;  // anchored: seed only, never moved

        // Highest radius among edge-neighbours ABOVE this vert (chain parent).
        float maxAbove = -1.0f;
        for (uint32_t j : adj[idx]) {
            if (j >= n || radius[j] < 0.0f) continue;
            if (zpos[j] > zpos[idx]) {
                float cand = radius[j] - returnRate * (zpos[j] - zpos[idx]);
                if (cand > maxAbove) maxAbove = cand;
            }
        }

        float target = maxAbove;
        if (shell && shell->valid) {
            float baseR = 0.0f, morphR = 0.0f;
            if (SampleBodyShell(*shell, finalVerts[idx], baseR, morphR)) {
                float bodyTarget = morphR + clearance;
                if (bodyTarget > target) target = bodyTarget;
            }
        }
        if (target <= 0.0f) continue;

        if (std::fabs(radius[idx] - target) > 0.01f) {
            finalVerts[idx].x = g_refCenterX + dirx[idx] * target;
            finalVerts[idx].y = g_refCenterY + diry[idx] * target;
            radius[idx] = target;   // propagate to verts below in this pass
            ++stats.envelopeAdjusted;
        }
    }
}

struct BodyColumnPoint {
    uint16_t idx = 0;
    float z = 0.0f;
    float targetR = 0.0f;
    float dirX = 0.0f;
    float dirY = 0.0f;
    float follow = 1.0f;
};

static void ApplyBodyColumnDrapePass(std::vector<Vec3>& finalVerts,
                                     const std::vector<Vec3>& baseVerts,
                                     const std::vector<Vec3>& supportVerts,
                                     const std::vector<float>& follow,
                                     const BodyShell* bodyShell,
                                     const StaticSolveParams& params,
                                     StaticSolveStats& stats) {
    if (!bodyShell || !bodyShell->valid || params.bodyColumnStrength <= 0.0f ||
        params.bins <= 0) {
        return;
    }

    std::vector<BodyColumnPoint> points;
    points.reserve(finalVerts.size());
    std::vector<std::vector<size_t>> bins((size_t)params.bins);

    for (size_t i = 0; i < finalVerts.size() && i <= 0xFFFF; ++i) {
        float baseBodyR = 0.0f;
        float morphBodyR = 0.0f;
        if (!SampleBodyShell(*bodyShell, baseVerts[i], baseBodyR, morphBodyR))
            continue;

        float dirX = 0.0f, dirY = 0.0f, baseClothR = 0.0f;
        if (!RadialDir(baseVerts[i], dirX, dirY, baseClothR) &&
            !RadialDir(supportVerts[i], dirX, dirY, baseClothR)) {
            continue;
        }

        float baseGap = std::max(0.0f, baseClothR - baseBodyR);
        float rawSupportR = RadialRadius(supportVerts[i]);
        float targetR = morphBodyR + params.bodyClearance +
            baseGap * params.bodyGapPreserve;
        targetR = std::max(targetR, rawSupportR - params.clearanceSlack);

        BodyColumnPoint p;
        p.idx = static_cast<uint16_t>(i);
        p.z = supportVerts[i].z;
        p.targetR = targetR;
        p.dirX = dirX;
        p.dirY = dirY;
        p.follow = i < follow.size() ? Clamp01(follow[i]) : 1.0f;

        int bin = RadialBin(dirX, dirY, params.bins);
        bins[(size_t)bin].push_back(points.size());
        points.push_back(p);
    }

    for (auto& bin : bins) {
        if (bin.empty())
            continue;

        std::sort(bin.begin(), bin.end(), [&](size_t a, size_t b) {
            return points[a].z > points[b].z;
        });

        bool haveCarried = false;
        float carriedR = 0.0f;
        float lastZ = 0.0f;
        for (size_t pointIndex : bin) {
            const BodyColumnPoint& p = points[pointIndex];
            if (!haveCarried) {
                carriedR = p.targetR;
                lastZ = p.z;
                haveCarried = true;
            }
            else {
                float dz = std::max(0.0f, lastZ - p.z);
                carriedR = std::max(p.targetR,
                                    carriedR - dz * params.bodyColumnRelaxPerUnitZ);
                lastZ = p.z;
            }

            float loose = 1.0f - p.follow;
            float strength = params.bodyColumnStrength *
                ClampRange(0.15f + 0.85f * loose, 0.0f, 1.0f);
            if (MoveRadiallyOut(finalVerts[p.idx], p.dirX, p.dirY,
                                carriedR, strength)) {
                ++stats.bodyColumnAdjusted;
            }
        }
    }
}

struct StaticEnvelopePoint {
    uint16_t idx = 0;
    float z = 0.0f;
    float supportR = 0.0f;
    float dirX = 0.0f;
    float dirY = 0.0f;
    float follow = 1.0f;
};

static void ApplyEnvelopePass(std::vector<Vec3>& finalVerts,
                              const std::vector<Vec3>& supportVerts,
                              const std::vector<float>& follow,
                              const StaticSolveParams& params,
                              StaticSolveStats& stats) {
    if (params.envelopeStrength <= 0.0f || params.bins <= 0)
        return;

    std::vector<StaticEnvelopePoint> points;
    points.reserve(finalVerts.size());
    std::vector<std::vector<size_t>> bins((size_t)params.bins);

    for (size_t i = 0; i < finalVerts.size() && i <= 0xFFFF; ++i) {
        float dirX = 0.0f, dirY = 0.0f, supportR = 0.0f;
        if (!RadialDir(supportVerts[i], dirX, dirY, supportR))
            continue;

        StaticEnvelopePoint p;
        p.idx = static_cast<uint16_t>(i);
        p.z = supportVerts[i].z;
        p.supportR = supportR;
        p.dirX = dirX;
        p.dirY = dirY;
        p.follow = i < follow.size() ? Clamp01(follow[i]) : 1.0f;

        int bin = RadialBin(dirX, dirY, params.bins);
        bins[(size_t)bin].push_back(points.size());
        points.push_back(p);
    }

    for (auto& bin : bins) {
        if (bin.empty())
            continue;

        std::sort(bin.begin(), bin.end(), [&](size_t a, size_t b) {
            return points[a].z > points[b].z;
        });

        bool haveCarried = false;
        float carriedR = 0.0f;
        float lastZ = 0.0f;
        for (size_t pointIndex : bin) {
            const StaticEnvelopePoint& p = points[pointIndex];
            if (!haveCarried) {
                carriedR = p.supportR;
                lastZ = p.z;
                haveCarried = true;
            }
            else {
                float dz = std::max(0.0f, lastZ - p.z);
                carriedR = std::max(p.supportR, carriedR - dz * params.relaxPerUnitZ);
                lastZ = p.z;
            }

            float loose = 1.0f - p.follow;
            float strength = params.envelopeStrength *
                ClampRange(std::max(loose, g_opts.minEnvelopeStrength), 0.0f, 1.0f);
            if (MoveRadiallyOut(finalVerts[p.idx], p.dirX, p.dirY, carriedR, strength))
                ++stats.envelopeAdjusted;
        }
    }
}

static void ApplyLayerOrderPass(std::vector<Vec3>& finalVerts,
                                const std::vector<Vec3>& baseVerts,
                                const std::vector<EdgeConstraint>& edges,
                                const std::vector<float>& invMass,
                                const StaticSolveParams& params,
                                StaticSolveStats& stats) {
    if (params.layerStrength <= 0.0f || edges.empty())
        return;

    int iterations = std::max(0, params.layerIterations);
    for (int iter = 0; iter < iterations; ++iter) {
        for (const auto& e : edges) {
            if (e.a >= finalVerts.size() || e.b >= finalVerts.size())
                continue;

            float baseRa = RadialRadius(baseVerts[e.a]);
            float baseRb = RadialRadius(baseVerts[e.b]);
            float baseDelta = baseRa - baseRb;
            if (std::fabs(baseDelta) < 0.35f)
                continue;

            uint16_t outer = baseDelta > 0.0f ? e.a : e.b;
            uint16_t inner = baseDelta > 0.0f ? e.b : e.a;
            float outerBaseR = std::max(baseRa, baseRb);
            float innerBaseR = std::min(baseRa, baseRb);
            float minSep = ClampRange((outerBaseR - innerBaseR) * 0.20f, 0.04f, 1.25f);

            float curOuterR = RadialRadius(finalVerts[outer]);
            float curInnerR = RadialRadius(finalVerts[inner]);
            if (curOuterR + minSep >= curInnerR)
                continue;

            float dirX = 0.0f, dirY = 0.0f, r = 0.0f;
            if (!RadialDir(baseVerts[outer], dirX, dirY, r) &&
                !RadialDir(finalVerts[outer], dirX, dirY, r)) {
                continue;
            }

            float mobility = outer < invMass.size() ? Clamp01(invMass[outer]) : 1.0f;
            float targetR = curInnerR + minSep;
            if (MoveRadiallyOut(finalVerts[outer], dirX, dirY, targetR,
                                params.layerStrength * mobility)) {
                ++stats.layerAdjusted;
            }
        }
    }
}

static void ApplyEdgeConstraintPass(std::vector<Vec3>& finalVerts,
                                    const std::vector<EdgeConstraint>& edges,
                                    const std::vector<float>& invMass,
                                    const StaticSolveParams& params,
                                    StaticSolveStats& stats) {
    int iterations = std::max(0, params.constraintIterations);
    float k = Clamp01(params.stretchStiffness);
    if (iterations <= 0 || k <= 0.0f || edges.empty())
        return;

    float kPrime = 1.0f - std::pow(1.0f - k, 1.0f / (float)iterations);
    static constexpr float kLenEps = 1.0e-6f;

    for (int iter = 0; iter < iterations; ++iter) {
        for (const auto& e : edges) {
            if (e.a >= finalVerts.size() || e.b >= finalVerts.size())
                continue;

            float w1 = e.a < invMass.size() ? Clamp01(invMass[e.a]) : 1.0f;
            float w2 = e.b < invMass.size() ? Clamp01(invMass[e.b]) : 1.0f;
            float wSum = w1 + w2;
            if (wSum <= kLenEps)
                continue;

            Vec3 dir = Sub(finalVerts[e.a], finalVerts[e.b]);
            float len = Len(dir);
            if (len <= kLenEps)
                continue;

            Vec3 delta = Scale(dir, ((len - e.restLength) / len) * kPrime / wSum);
            if (w1 > 0.0f)
                finalVerts[e.a] = Sub(finalVerts[e.a], Scale(delta, w1));
            if (w2 > 0.0f)
                finalVerts[e.b] = Add(finalVerts[e.b], Scale(delta, w2));
            ++stats.stretchAdjusted;
        }
    }
}

// One OUTER garment's collider, keeping its identity (not merged with other outers).
// Holds the outer mesh's BASE and CURRENT surfaces as separate BVHs, plus its preset
// (for adaptive clearance). The anti-clip pass keeps THIS mesh tucked under each of
// these individually, so the nearest-point can never silently jump between meshes.
struct OuterCollider {
    BodyBVH base;
    BodyBVH current;
    const BodyBVH* baseRef = nullptr;
    const BodyBVH* currentRef = nullptr;
    int     preset = 0;
    std::string name;

    const BodyBVH& Base() const { return baseRef ? *baseRef : base; }
    const BodyBVH& Current() const { return currentRef ? *currentRef : current; }
};

struct SolverCapture {
    std::string shapeName;
    std::string sliderName;
    std::string projectKey;
    int preset = DRAPE_PRESET_AUTO;
    int processingIntensity = 1;
    float refCenterX = 0.0f;
    float refCenterY = 0.0f;
    float refMinZ = 0.0f;
    float refMaxZ = 0.0f;
    std::vector<Vec3> baseVerts;
    std::vector<VertexBoneProfile> profiles;
    std::vector<EdgeConstraint> edges;
    std::vector<BendingConstraint> bending;
    std::vector<std::array<uint16_t, 3>> tris;
    bool hasBodyShell = false;
    BodyShell bodyShell;
    bool hasBodyBVH = false;
    std::vector<Vec3> bodyBVHVerts;
    std::vector<std::array<uint16_t, 3>> bodyBVHTris;
    bool hasRefBodyBVH = false;
    std::vector<Vec3> refBodyBVHVerts;
    std::vector<std::array<uint16_t, 3>> refBodyBVHTris;
    std::unordered_map<uint16_t, float> presetMask;
    TargetDataDiffs rawDiff;
    std::vector<OuterCollider> outerColliders;
    std::vector<OuterCollider> innerColliders;
    bool hasExpected = false;
    bool expectedPassOk = false;
    TargetDataDiffs expectedDiff;
    std::vector<Vec3> expectedSolvedVerts;
};

static fs::path CaptureFlagPath() {
    return fs::path("E:/SkyrimDev/Drape/captures/enable_capture.flag");
}

static fs::path CaptureDefaultDir() {
    return fs::path("E:/SkyrimDev/Drape/captures/yarl");
}

static bool EnvFlagEnabled(const char* name) {
    // Capture writes large replay files and is expensive. Keep it file-gated so
    // a stale PowerShell/MO2 environment cannot accidentally slow production runs.
    return name && std::string(name) == "DRAPE_CAPTURE" && fs::exists(CaptureFlagPath());
}

static fs::path EnvPathOrDefault(const char* name, const fs::path& fallback) {
    if (name && std::string(name) == "DRAPE_CAPTURE_DIR" && !fs::exists(CaptureFlagPath()))
        return fallback;
    char buf[MAX_PATH * 4] = {};
    size_t len = 0;
    if (getenv_s(&len, buf, sizeof(buf), name) == 0 && len > 1)
        return fs::path(buf);
    if (name && std::string(name) == "DRAPE_CAPTURE_DIR" && fs::exists(CaptureFlagPath()))
        return CaptureDefaultDir();
    return fallback;
}

struct CaptureWriter {
    std::ofstream out;
    bool ok = false;

    explicit CaptureWriter(const fs::path& path)
        : out(path, std::ios::binary | std::ios::trunc), ok(out.good()) {}

    template <class T>
    void pod(const T& v) {
        if (!ok) return;
        out.write(reinterpret_cast<const char*>(&v), sizeof(T));
        ok = out.good();
    }

    void str(const std::string& s) {
        uint32_t n = (uint32_t)std::min<size_t>(s.size(), std::numeric_limits<uint32_t>::max());
        pod(n);
        if (!ok || n == 0) return;
        out.write(s.data(), n);
        ok = out.good();
    }

    template <class T, class Fn>
    void vec(const std::vector<T>& v, Fn fn) {
        uint32_t n = (uint32_t)std::min<size_t>(v.size(), std::numeric_limits<uint32_t>::max());
        pod(n);
        for (uint32_t i = 0; ok && i < n; ++i)
            fn(v[i]);
    }
};

struct CaptureReader {
    std::ifstream in;
    bool ok = false;

    explicit CaptureReader(const fs::path& path)
        : in(path, std::ios::binary), ok(in.good()) {}

    template <class T>
    void pod(T& v) {
        if (!ok) return;
        in.read(reinterpret_cast<char*>(&v), sizeof(T));
        ok = in.good();
    }

    void str(std::string& s) {
        uint32_t n = 0;
        pod(n);
        if (!ok) return;
        s.assign(n, '\0');
        if (n == 0) return;
        in.read(&s[0], n);
        ok = in.good();
    }

    template <class T, class Fn>
    void vec(std::vector<T>& v, Fn fn) {
        uint32_t n = 0;
        pod(n);
        if (!ok) return;
        v.clear();
        v.resize(n);
        for (uint32_t i = 0; ok && i < n; ++i)
            fn(v[i]);
    }
};

static void WriteVec3(CaptureWriter& w, const Vec3& v) {
    w.pod(v.x); w.pod(v.y); w.pod(v.z);
}

static void ReadVec3(CaptureReader& r, Vec3& v) {
    r.pod(v.x); r.pod(v.y); r.pod(v.z);
}

static void WriteTri(CaptureWriter& w, const std::array<uint16_t, 3>& t) {
    w.pod(t[0]); w.pod(t[1]); w.pod(t[2]);
}

static void ReadTri(CaptureReader& r, std::array<uint16_t, 3>& t) {
    r.pod(t[0]); r.pod(t[1]); r.pod(t[2]);
}

static void WriteProfile(CaptureWriter& w, const VertexBoneProfile& p) {
    w.pod(p.torso); w.pod(p.pelvis); w.pod(p.leg); w.pod(p.arm);
    w.pod(p.handFoot); w.pod(p.head); w.pod(p.custom); w.pod(p.maxWeight);
    w.pod(p.influences);
}

static void ReadProfile(CaptureReader& r, VertexBoneProfile& p) {
    r.pod(p.torso); r.pod(p.pelvis); r.pod(p.leg); r.pod(p.arm);
    r.pod(p.handFoot); r.pod(p.head); r.pod(p.custom); r.pod(p.maxWeight);
    r.pod(p.influences);
}

static void WriteEdge(CaptureWriter& w, const EdgeConstraint& e) {
    w.pod(e.a); w.pod(e.b); w.pod(e.restLength);
}

static void ReadEdge(CaptureReader& r, EdgeConstraint& e) {
    r.pod(e.a); r.pod(e.b); r.pod(e.restLength);
}

static void WriteBending(CaptureWriter& w, const BendingConstraint& b) {
    w.pod(b.c); w.pod(b.d); w.pod(b.restLength);
}

static void ReadBending(CaptureReader& r, BendingConstraint& b) {
    r.pod(b.c); r.pod(b.d); r.pod(b.restLength);
}

static void WriteBodyShell(CaptureWriter& w, const BodyShell& s) {
    w.pod(s.radialBins); w.pod(s.zBins); w.pod(s.minZ); w.pod(s.maxZ); w.pod(s.invDz);
    w.vec(s.baseR, [&](float v) { w.pod(v); });
    w.vec(s.morphR, [&](float v) { w.pod(v); });
    w.vec(s.occupied, [&](uint8_t v) { w.pod(v); });
    w.pod(s.valid);
}

static void ReadBodyShell(CaptureReader& r, BodyShell& s) {
    r.pod(s.radialBins); r.pod(s.zBins); r.pod(s.minZ); r.pod(s.maxZ); r.pod(s.invDz);
    r.vec(s.baseR, [&](float& v) { r.pod(v); });
    r.vec(s.morphR, [&](float& v) { r.pod(v); });
    r.vec(s.occupied, [&](uint8_t& v) { r.pod(v); });
    r.pod(s.valid);
}

static void WriteMask(CaptureWriter& w, const std::unordered_map<uint16_t, float>& m) {
    std::vector<std::pair<uint16_t, float>> items(m.begin(), m.end());
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    uint32_t n = (uint32_t)items.size();
    w.pod(n);
    for (const auto& kv : items) {
        w.pod(kv.first);
        w.pod(kv.second);
    }
}

static void ReadMask(CaptureReader& r, std::unordered_map<uint16_t, float>& m) {
    uint32_t n = 0;
    r.pod(n);
    m.clear();
    m.reserve(n);
    for (uint32_t i = 0; r.ok && i < n; ++i) {
        uint16_t k = 0; float v = 0.0f;
        r.pod(k); r.pod(v);
        m[k] = v;
    }
}

static void WriteDiff(CaptureWriter& w, const TargetDataDiffs& d) {
    std::vector<std::pair<uint16_t, Vector3>> items(d.begin(), d.end());
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    uint32_t n = (uint32_t)items.size();
    w.pod(n);
    for (const auto& kv : items) {
        w.pod(kv.first);
        w.pod(kv.second.x);
        w.pod(kv.second.y);
        w.pod(kv.second.z);
    }
}

static void ReadDiff(CaptureReader& r, TargetDataDiffs& d) {
    uint32_t n = 0;
    r.pod(n);
    d.clear();
    d.reserve(n);
    for (uint32_t i = 0; r.ok && i < n; ++i) {
        uint16_t k = 0; Vector3 v{};
        r.pod(k); r.pod(v.x); r.pod(v.y); r.pod(v.z);
        d[k] = v;
    }
}

static void WriteCollider(CaptureWriter& w, const OuterCollider& oc) {
    w.str(oc.name);
    w.pod(oc.preset);
    w.vec(oc.Base().verts, [&](const Vec3& v) { WriteVec3(w, v); });
    w.vec(oc.Base().tris, [&](const std::array<uint16_t, 3>& t) { WriteTri(w, t); });
    w.vec(oc.Current().verts, [&](const Vec3& v) { WriteVec3(w, v); });
    w.vec(oc.Current().tris, [&](const std::array<uint16_t, 3>& t) { WriteTri(w, t); });
}

static void ReadCollider(CaptureReader& r, OuterCollider& oc) {
    std::vector<Vec3> baseVerts, curVerts;
    std::vector<std::array<uint16_t, 3>> baseTris, curTris;
    r.str(oc.name);
    r.pod(oc.preset);
    r.vec(baseVerts, [&](Vec3& v) { ReadVec3(r, v); });
    r.vec(baseTris, [&](std::array<uint16_t, 3>& t) { ReadTri(r, t); });
    r.vec(curVerts, [&](Vec3& v) { ReadVec3(r, v); });
    r.vec(curTris, [&](std::array<uint16_t, 3>& t) { ReadTri(r, t); });
    BuildBodyBVH(baseVerts, baseTris, oc.base);
    BuildBodyBVH(curVerts, curTris, oc.current);
}

static bool WriteSolverCapture(const fs::path& path, const SolverCapture& c) {
    CaptureWriter w(path);
    if (!w.ok) return false;
    const uint32_t magic = 0x43505244u; // DRPC
    const uint32_t version = 3;
    w.pod(magic);
    w.pod(version);
    w.str(c.shapeName);
    w.str(c.sliderName);
    w.str(c.projectKey);
    w.pod(c.preset);
    w.pod(c.processingIntensity);
    w.pod(c.refCenterX); w.pod(c.refCenterY); w.pod(c.refMinZ); w.pod(c.refMaxZ);
    w.vec(c.baseVerts, [&](const Vec3& v) { WriteVec3(w, v); });
    w.vec(c.profiles, [&](const VertexBoneProfile& p) { WriteProfile(w, p); });
    w.vec(c.edges, [&](const EdgeConstraint& e) { WriteEdge(w, e); });
    w.vec(c.bending, [&](const BendingConstraint& b) { WriteBending(w, b); });
    w.vec(c.tris, [&](const std::array<uint16_t, 3>& t) { WriteTri(w, t); });
    w.pod(c.hasBodyShell);
    if (c.hasBodyShell) WriteBodyShell(w, c.bodyShell);
    w.pod(c.hasBodyBVH);
    if (c.hasBodyBVH) {
        w.vec(c.bodyBVHVerts, [&](const Vec3& v) { WriteVec3(w, v); });
        w.vec(c.bodyBVHTris, [&](const std::array<uint16_t, 3>& t) { WriteTri(w, t); });
    }
    w.pod(c.hasRefBodyBVH);
    if (c.hasRefBodyBVH) {
        w.vec(c.refBodyBVHVerts, [&](const Vec3& v) { WriteVec3(w, v); });
        w.vec(c.refBodyBVHTris, [&](const std::array<uint16_t, 3>& t) { WriteTri(w, t); });
    }
    WriteMask(w, c.presetMask);
    WriteDiff(w, c.rawDiff);
    w.vec(c.outerColliders, [&](const OuterCollider& oc) { WriteCollider(w, oc); });
    w.vec(c.innerColliders, [&](const OuterCollider& oc) { WriteCollider(w, oc); });
    w.pod(c.hasExpected);
    if (c.hasExpected) {
        w.pod(c.expectedPassOk);
        WriteDiff(w, c.expectedDiff);
        w.vec(c.expectedSolvedVerts, [&](const Vec3& v) { WriteVec3(w, v); });
    }
    return w.ok;
}

static bool ReadSolverCapture(const fs::path& path, SolverCapture& c) {
    CaptureReader r(path);
    if (!r.ok) return false;
    uint32_t magic = 0, version = 0;
    r.pod(magic);
    r.pod(version);
    if (!r.ok || magic != 0x43505244u || (version < 1 || version > 3)) return false;
    r.str(c.shapeName);
    r.str(c.sliderName);
    r.str(c.projectKey);
    r.pod(c.preset);
    r.pod(c.processingIntensity);
    r.pod(c.refCenterX); r.pod(c.refCenterY); r.pod(c.refMinZ); r.pod(c.refMaxZ);
    r.vec(c.baseVerts, [&](Vec3& v) { ReadVec3(r, v); });
    r.vec(c.profiles, [&](VertexBoneProfile& p) { ReadProfile(r, p); });
    r.vec(c.edges, [&](EdgeConstraint& e) { ReadEdge(r, e); });
    if (version >= 2)
        r.vec(c.bending, [&](BendingConstraint& b) { ReadBending(r, b); });
    else
        c.bending.clear();
    r.vec(c.tris, [&](std::array<uint16_t, 3>& t) { ReadTri(r, t); });
    r.pod(c.hasBodyShell);
    if (c.hasBodyShell) ReadBodyShell(r, c.bodyShell);
    r.pod(c.hasBodyBVH);
    if (c.hasBodyBVH) {
        r.vec(c.bodyBVHVerts, [&](Vec3& v) { ReadVec3(r, v); });
        r.vec(c.bodyBVHTris, [&](std::array<uint16_t, 3>& t) { ReadTri(r, t); });
    }
    r.pod(c.hasRefBodyBVH);
    if (c.hasRefBodyBVH) {
        r.vec(c.refBodyBVHVerts, [&](Vec3& v) { ReadVec3(r, v); });
        r.vec(c.refBodyBVHTris, [&](std::array<uint16_t, 3>& t) { ReadTri(r, t); });
    }
    ReadMask(r, c.presetMask);
    ReadDiff(r, c.rawDiff);
    r.vec(c.outerColliders, [&](OuterCollider& oc) { ReadCollider(r, oc); });
    if (version >= 3)
        r.vec(c.innerColliders, [&](OuterCollider& oc) { ReadCollider(r, oc); });
    else
        c.innerColliders.clear();
    r.pod(c.hasExpected);
    if (c.hasExpected) {
        r.pod(c.expectedPassOk);
        ReadDiff(r, c.expectedDiff);
        r.vec(c.expectedSolvedVerts, [&](Vec3& v) { ReadVec3(r, v); });
    }
    return r.ok;
}

static SolverCapture MakeSolverCapture(const std::string& shapeName,
                                       const std::string& sliderName,
                                       int preset,
                                       const std::vector<Vec3>& baseVerts,
                                       const std::vector<VertexBoneProfile>* profiles,
                                       const std::vector<EdgeConstraint>* edges,
                                       const std::vector<std::array<uint16_t, 3>>* tris,
                                       const BodyShell* bodyShell,
                                       const BodyBVH* bodyBVH,
                                       const std::unordered_map<uint16_t, float>& presetMask,
                                       const TargetDataDiffs& rawDiff,
                                       const std::vector<OuterCollider>* outerColliders,
                                       const std::vector<OuterCollider>* innerColliders) {
    SolverCapture c;
    c.shapeName = shapeName;
    c.sliderName = sliderName;
    c.projectKey = g_projectKey;
    c.preset = preset;
    c.processingIntensity = g_processingIntensity.load(std::memory_order_relaxed);
    c.refCenterX = g_refCenterX;
    c.refCenterY = g_refCenterY;
    c.refMinZ = g_refMinZ;
    c.refMaxZ = g_refMaxZ;
    c.baseVerts = baseVerts;
    if (profiles) c.profiles = *profiles;
    if (edges) c.edges = *edges;
    if (tris) c.tris = *tris;
    {
        auto bendingIt = g_shapeBending.find(shapeName);
        if (bendingIt != g_shapeBending.end()) c.bending = bendingIt->second;
    }
    if (bodyShell) {
        c.hasBodyShell = true;
        c.bodyShell = *bodyShell;
    }
    if (bodyBVH && bodyBVH->valid) {
        c.hasBodyBVH = true;
        c.bodyBVHVerts = bodyBVH->verts;
        c.bodyBVHTris = bodyBVH->tris;
    }
    if (g_refBodyBVH.valid) {
        c.hasRefBodyBVH = true;
        c.refBodyBVHVerts = g_refBodyBVH.verts;
        c.refBodyBVHTris = g_refBodyBVH.tris;
    }
    c.presetMask = presetMask;
    c.rawDiff = rawDiff;
    if (outerColliders) c.outerColliders = *outerColliders;
    if (innerColliders) c.innerColliders = *innerColliders;
    return c;
}

static fs::path CaptureOutputDir() {
    return EnvPathOrDefault("DRAPE_CAPTURE_DIR", TempDrapePath("drape_captures"));
}

static void MaybeWriteSolverCapture(SolverCapture capture,
                                    bool passOk,
                                    const TargetDataDiffs& solvedDiff,
                                    const std::vector<Vec3>& solvedVerts) {
    if (!EnvFlagEnabled("DRAPE_CAPTURE"))
        return;
    capture.hasExpected = true;
    capture.expectedPassOk = passOk;
    capture.expectedDiff = solvedDiff;
    capture.expectedSolvedVerts = solvedVerts;

    fs::path dir = CaptureOutputDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        HLog("capture: failed to create '%s' (%s)", dir.string().c_str(), ec.message().c_str());
        return;
    }

    std::string name = "drape_capture__" + SafeFilePart(g_projectKey) + "__" +
        SafeFilePart(capture.shapeName) + "__" + SafeFilePart(capture.sliderName) + ".drcap";
    fs::path path = dir / name;
    if (WriteSolverCapture(path, capture)) {
        HLog("capture: wrote '%s' verts=%zu raw=%zu solved=%zu outers=%zu inners=%zu",
             path.string().c_str(), capture.baseVerts.size(), capture.rawDiff.size(),
             solvedDiff.size(), capture.outerColliders.size(), capture.innerColliders.size());
    } else {
        HLog("capture: failed to write '%s'", path.string().c_str());
    }
}

// The real solver body. NEVER call this directly outside the ApplyStaticClothPass
// wrapper below (U1, crash containment) â€” this function and everything it calls
// is where a bug in ~8000 lines of solver code could raise a hardware exception
// (bad index, div-by-zero, etc). The wrapper catches that so it can never take
// OutfitStudio down with it.
static bool ApplyStaticClothPassImpl(const std::string& shapeName,
                                 const std::string& sliderName,
                                 int preset,
                                 const std::vector<Vec3>& baseVerts,
                                 const std::vector<VertexBoneProfile>* profiles,
                                 const std::vector<EdgeConstraint>* edges,
                                 const std::vector<std::array<uint16_t, 3>>* tris,
                                 const BodyShell* bodyShell,
                                 const BodyBVH* bodyBVH,
                                 const std::unordered_map<uint16_t, float>& presetMask,
                                 const TargetDataDiffs& rawDiff,
                                 // OUTER-mesh colliders, ONE PER outer garment (kept separate so the
                                 // nearest-point can't silently jump between cloak/belt/pants). Each
                                 // holds the outer mesh's base + current surface and its preset (for
                                 // adaptive clearance). The anti-clip is ONE-DIRECTIONAL: it only PULLS
                                 // THIS mesh IN under each outer one â€” never pushes the outer mesh out,
                                 // so the outermost layer (fur cloak) is never deformed (it gets an
                                 // empty list). Empty/null if this is the outermost mesh.
                                 const std::vector<OuterCollider>* outerColliders,
                                 // INNER-mesh colliders for external structured pieces (belts,
                                 // straps, rigid accessories). These push THIS outer piece away
                                 // from already-solved lower layers, instead of pulling cloth inward.
                                 const std::vector<OuterCollider>* innerColliders,
                                 // Receives this mesh's final solved positions, for the caller to
                                 // cache as an outerBVH source for later-solved inner meshes.
                                 std::vector<Vec3>* outSolvedVerts,
                                 TargetDataDiffs& outDiff,
                                 StaticSolveStats& stats) {
    stats = StaticSolveStats{};
    stats.rawDiffVerts = rawDiff.size();
    if (rawDiff.empty() || baseVerts.empty())
        return false;

    StaticSolveParams params = StaticParamsForPreset(preset);
    if (!params.enabled)
        return false;
    stats.physicsDrapeActive = params.physicsDrape;

    const int normPreset = NormalizePreset(preset);
    const bool isLoosePreset = normPreset == DRAPE_PRESET_LOOSE_DRAPED;

    size_t n = std::min<size_t>(baseVerts.size(), 0x10000);
    std::vector<Vec3> supportVerts(n);
    std::vector<Vec3> finalVerts(n);
    std::vector<float> follow(n, 1.0f);
    std::vector<float> invMass(n, 0.0f);
    std::vector<float> anchorWeight(n, 0.0f);

    for (size_t i = 0; i < n; ++i) {
        uint16_t idx = static_cast<uint16_t>(i);
        Vec3 raw = DiffAt(rawDiff, idx);
        auto maskIt = presetMask.find(idx);
        float f = maskIt != presetMask.end() ? Clamp01(maskIt->second) : 0.0f;
        follow[i] = f;

        supportVerts[i] = Add(baseVerts[i], raw);
        // Start from the conformed fit (supportVerts) in all modes. The Verlet
        // cloth solve then hangs the drape region down from this fitted start â€”
        // matching the diagnostic drop that looked correct. Starting from the slim
        // base mesh forced an unstable re-inflate that fought the body.
        finalVerts[i]   = supportVerts[i];

        float loose    = 1.0f - f;
        float mobility = ClampRange(0.08f + loose * 0.92f, 0.0f, 1.0f);
        float anchor   = f;
        if (profiles && i < profiles->size()) {
            float boneAnchor = AnchorWeightForPreset(preset, (*profiles)[i]);
            anchor   = std::max(anchor, boneAnchor);
            mobility *= ClampRange(1.0f - boneAnchor * 0.45f, 0.25f, 1.0f);
        }

        switch (NormalizePreset(preset)) {
        case DRAPE_PRESET_BELT:
            mobility = std::max(mobility, 0.45f);
            break;
        case DRAPE_PRESET_RIGID_ARMOR:
        case DRAPE_PRESET_RIGID_ACCESSORY:
            mobility = std::max(mobility, 0.35f);
            break;
        default:
            break;
        }

        // Physics drape: loose verts have no anchor â€” physics determines their
        // position. Only tight verts (high follow) are anchored to vanilla conform.
        if (params.physicsDrape && f < 0.35f) {
            anchor   = 0.0f;
            mobility = 0.90f;
        }

        invMass[i]      = Clamp01(mobility);
        anchorWeight[i] = Clamp01(anchor);
    }

    // Belt-as-cloth boundary condition. The BELT mask forces high follow everywhere, so
    // the drape solver would anchor the whole belt to the FLOATING vanilla conform (it
    // never touches the body -> the "hover"). Real cloth is pinned only where it CONTACTS
    // the body (the TOP line of each piece) and hangs from there. So per connected island
    // (wrap, hanging strap, buckle): seat its TOP band onto the body (rewrite the anchor
    // target = a real contact) and pin it; make the rest DRAPE-eligible so cloth physics
    // hangs it from that contact and bridges hollows (no uniform inward pull = no magnetism).
    if (params.physicsDrape && NormalizePreset(preset) == DRAPE_PRESET_BELT &&
        bodyBVH && bodyBVH->valid && tris && !tris->empty() && n > 0 && n <= 0xFFFF) {
        std::vector<int> parent(n);
        for (size_t i = 0; i < n; ++i) parent[i] = (int)i;
        auto findp = [&](int x) -> int {
            while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
            return x;
        };
        for (const auto& t : *tris) {
            if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
            parent[findp(t[0])] = findp(t[1]);
            parent[findp(t[1])] = findp(t[2]);
        }
        std::unordered_map<int, float> compZMax;   // top of each island
        for (size_t i = 0; i < n; ++i) {
            int r = findp((int)i);
            auto it = compZMax.find(r);
            if (it == compZMax.end() || supportVerts[i].z > it->second) compZMax[r] = supportVerts[i].z;
        }
        const float kPinBand = 2.5f;   // pin the top 2.5u of each island (the contact line)
        const float kCordGap = 0.4f;   // pinned contact sits this far off the body/dress
        int pinned = 0;
        for (size_t i = 0; i < n; ++i) {
            float topZ = compZMax[findp((int)i)];
            if (supportVerts[i].z >= topZ - kPinBand) {
                Vec3 cp{0,0,0}, nrm{0,0,1}; float d2 = 0.0f;
                if (BVHClosestPoint(*bodyBVH, supportVerts[i], cp, nrm, d2)) {
                    supportVerts[i] = Add(cp, Scale(nrm, kCordGap));  // seat onto body = contact
                    finalVerts[i]   = supportVerts[i];
                }
                follow[i] = 1.0f; anchorWeight[i] = 1.0f; invMass[i] = 0.05f;
                ++pinned;
            } else {
                follow[i] = 0.10f; anchorWeight[i] = 0.0f; invMass[i] = 0.90f;  // drape
            }
        }
        HLog("belt-as-cloth: islands=%zu pinnedTopVerts=%d of %zu", compZMax.size(), pinned, n);
    }

    static const std::vector<EdgeConstraint> kNoEdges;
    const std::vector<EdgeConstraint>& useEdges = edges ? *edges : kNoEdges;

    // Pre-clamp vanilla Conform All edge-stretch outliers BEFORE the solve.
    // On heavy morphs, vanilla occasionally mis-projects a lateral seam vertex,
    // creating a 4-5x stretched edge that reads as a visible tear. Clamping
    // supportVerts here lets the physics solve start from a sane position; body
    // collision then pushes the vert back to the actual surface. supportRestLen
    // is computed later from the clamped supportVerts, so the solve's stretch
    // constraints also reflect the corrected rest lengths.
    // Threshold 2.8x: above the highest legitimate vanilla expansion on any clean
    // mesh in harness testing (Cloak RM_Heavy ~2.24x), so clean outfits are unaffected.
    // 50/50 Gauss-Seidel: distribute correction to both endpoints each pass so
    // fixing one edge doesn't over-stretch its neighbours.
    if (params.verletCloth && !useEdges.empty()) {
        const float kVanillaMaxStretch = 2.8f;
        for (int gsPass = 0; gsPass < 8; ++gsPass) {
            bool anyFixed = false;
            for (const auto& e : useEdges) {
                if (e.a >= n || e.b >= n || e.restLength <= 1e-6f) continue;
                Vec3 dir = Sub(supportVerts[e.a], supportVerts[e.b]);
                float len = Len(dir);
                float maxLen = e.restLength * kVanillaMaxStretch;
                if (len <= maxLen || len <= 1e-6f) continue;
                anyFixed = true;
                Vec3 normDir = Scale(dir, 1.0f / len);
                float excess = len - maxLen;
                Vec3 half = Scale(normDir, excess * 0.5f);
                supportVerts[e.a] = Sub(supportVerts[e.a], half);
                supportVerts[e.b] = Add(supportVerts[e.b], half);
                finalVerts[e.a]   = supportVerts[e.a];
                finalVerts[e.b]   = supportVerts[e.b];
            }
            if (!anyFixed) break;
        }
    }

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // UNIFIED VERTEX CLASSIFICATION
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // ONE pass decides each vertex's behavior. Everything downstream (invMass,
    // anchorWeight, vertClearance, gravity participation) derives from a single
    // continuous value:
    //
    //   drapeAmount[i] âˆˆ [0,1] â€” how much this vertex behaves like free-hanging
    //                            cloth vs. anchored-to-body.
    //     ~0.0  â†’ anchored: bodice/collar/sleeve/cuff. Follows vanilla conform,
    //             stays fitted to the body, no gravity.
    //     ~1.0  â†’ free drape: skirt/robe panel. Gravity + collision determine it.
    //
    // This REPLACES the three previously-conflicting systems (the old drapeEl
    // bone formula, the island-clearance pass, and the anchor-boost loop) that
    // computed contradictory labels from the same bone weights. See the memory
    // note project-classification-architecture for the rationale.
    //
    // Key correction: the integrated belt drapes like cloth (it must NOT be
    // frozen as anchor just because it carries waist/torso weight). drapeAmount
    // therefore counts pelvis + leg + a share of the LOWER torso (waistline), so
    // a waist band hangs instead of clinging. Sleeves/arms still suppress drape.
    float bodyZRange = (g_refMaxZ > g_refMinZ + 1.0f)
        ? (g_refMaxZ - g_refMinZ) : 1.0f;

    std::vector<float> drapeAmount(n, 0.0f);
    std::vector<float> vertClearance(n, params.bodyClearance);
    // Surfaced to the rigid-island pass below: per-vert island id + per-island
    // "rigid plate/accessory" flag (medallion/buckle/chain â†’ translate, keep shape).
    std::vector<int>     islandIdOut;
    std::vector<uint8_t> islandRigidOut;
    if (params.verletCloth) {
        // Build adjacency from useEdges.
        std::vector<std::vector<uint32_t>> adj2(n);
        for (const auto& e : useEdges) {
            if (e.a < n && e.b < n) {
                adj2[e.a].push_back(e.b);
                adj2[e.b].push_back(e.a);
            }
        }

        // BFS to label each vert with its island id.
        std::vector<int> islandId(n, -1);
        int numIslands = 0;
        for (size_t start = 0; start < n; ++start) {
            if (islandId[start] >= 0) continue;
            std::vector<uint32_t> q;
            q.push_back((uint32_t)start);
            islandId[start] = numIslands;
            for (size_t qi = 0; qi < q.size(); ++qi) {
                for (uint32_t nb : adj2[q[qi]]) {
                    if (islandId[nb] < 0) {
                        islandId[nb] = numIslands;
                        q.push_back(nb);
                    }
                }
            }
            ++numIslands;
        }

        // Aggregate bone weight profile per island.
        struct IslandProfile {
            float pelvis = 0.0f, leg = 0.0f, torso = 0.0f, arm = 0.0f;
            int   count  = 0;
        };
        std::vector<IslandProfile> iprof((size_t)numIslands);
        if (profiles) {
            for (size_t i = 0; i < n; ++i) {
                int id = islandId[i];
                if (id < 0 || id >= numIslands) continue;
                const auto& bp = (*profiles)[i];
                auto& ip = iprof[(size_t)id];
                ip.pelvis += Clamp01(bp.pelvis);
                ip.leg    += Clamp01(bp.leg);
                ip.torso  += Clamp01(bp.torso);
                ip.arm    += Clamp01(bp.arm + bp.handFoot);
                ++ip.count;
            }
        }
        for (auto& ip : iprof) {
            if (ip.count > 0) {
                float inv = 1.0f / (float)ip.count;
                ip.pelvis *= inv; ip.leg *= inv; ip.torso *= inv; ip.arm *= inv;
            }
        }

        // â”€â”€ Geometric descriptors per island (validated in the Python prototype:
        //    bbox flatness + extents discriminate plates/chains from drape far
        //    better than SDF/raycast, which collapse on thin shell garments). â”€â”€
        std::vector<Vec3> islMin((size_t)numIslands, Vec3{ 1e30f, 1e30f, 1e30f});
        std::vector<Vec3> islMax((size_t)numIslands, Vec3{-1e30f,-1e30f,-1e30f});
        std::vector<int>  islandSize(numIslands, 0);
        for (size_t i = 0; i < n; ++i) {
            int id = islandId[i];
            if (id < 0 || id >= numIslands) continue;
            islMin[(size_t)id] = VMin(islMin[(size_t)id], baseVerts[i]);
            islMax[(size_t)id] = VMax(islMax[(size_t)id], baseVerts[i]);
            ++islandSize[(size_t)id];
        }
        // Mesh diagonal to normalize extents (scale-independent thresholds).
        float meshDiag = 1.0f;
        {
            Vec3 gmin{ 1e30f, 1e30f, 1e30f}, gmax{-1e30f,-1e30f,-1e30f};
            for (size_t i = 0; i < n; ++i) { gmin = VMin(gmin, baseVerts[i]); gmax = VMax(gmax, baseVerts[i]); }
            meshDiag = std::max(1e-3f, Len(Sub(gmax, gmin)));
        }

        // â”€â”€ STEP A: detect RIGID islands + per-island drape amount â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // islandRigid[id]: rigid plate/accessory (medallion/buckle/chain) â†’ handled
        // by the rigid-island translate pass. islandDrape[id] âˆˆ [0,1]: how much the
        // island behaves as free-hanging cloth, from its aggregate bone profile.
        std::vector<uint8_t> islandRigid((size_t)numIslands, 0);
        std::vector<float>   islandDrape((size_t)numIslands, 1.0f);

        // Largest island vert count â†’ used to gate "small" relative to the mesh.
        int maxIslandSize = 1;
        for (int id = 0; id < numIslands; ++id)
            maxIslandSize = std::max(maxIslandSize, islandSize[(size_t)id]);

        // Waist line in world Z: torso weight only contributes to drape BELOW this.
        float waistZ = g_refMinZ + bodyZRange * 0.55f;

        // Per-vertex lower-body drape signal. pelvis+leg always count; torso counts
        // only below the waist; sleeves suppress.
        auto vertDrapeSignal = [&](size_t i) -> float {
            if (!(profiles && i < profiles->size())) {
                float normZ = ClampRange((baseVerts[i].z - g_refMinZ) / bodyZRange, 0.0f, 1.0f);
                return (normZ < 0.50f) ? 0.55f : 0.05f;
            }
            const auto& bp = (*profiles)[i];
            float belowWaist = (baseVerts[i].z < waistZ) ? 1.0f : 0.0f;
            float lower  = Clamp01(bp.pelvis + bp.leg + bp.torso * 0.5f * belowWaist);
            float sleeve = Clamp01((bp.arm + bp.handFoot) * 1.6f);
            return Clamp01(lower * (1.0f - sleeve));
        };

        // Aggregate the per-vertex signal into a per-island mean (the stable,
        // unambiguous level), plus detect rigid plates by geometry.
        std::vector<float> islandSignalSum((size_t)numIslands, 0.0f);
        std::vector<int>   islandSignalCnt((size_t)numIslands, 0);
        for (size_t i = 0; i < n; ++i) {
            int id = islandId[i];
            if (id < 0 || id >= numIslands) continue;
            islandSignalSum[(size_t)id] += vertDrapeSignal(i);
            ++islandSignalCnt[(size_t)id];
        }

        for (int id = 0; id < numIslands; ++id) {
            const auto& ip = iprof[(size_t)id];
            Vec3 ext = Scale(Sub(islMax[(size_t)id], islMin[(size_t)id]), 1.0f / meshDiag);
            float e[3] = { std::fabs(ext.x), std::fabs(ext.y), std::fabs(ext.z) };
            std::sort(e, e + 3);                       // e[0]<=e[1]<=e[2]
            float maxExt   = e[2];

            // RIGID plate / accessory â†’ translate, keep shape. Conservative gate:
            // SMALL (few verts, absolute + relative) AND COMPACT (small extent) AND
            // NOT-CLOTH (low pelvis/leg). Avoids rigidifying cloth panels (white slab).
            bool small   = islandSize[(size_t)id] < 250 &&
                           islandSize[(size_t)id] < maxIslandSize / 4;
            bool compact = maxExt < 0.28f;
            bool notCloth = (ip.pelvis + ip.leg) < 0.20f;
            if (small && compact && notCloth) {
                islandRigid[(size_t)id] = 1;
                islandDrape[(size_t)id] = 0.0f;
                continue;
            }

            // WAISTBAND / sculpted belt â†’ rigid translate. These islands sit at the
            // waist Z-range, have strong pelvis+torso, but very low leg weight (unlike
            // the skirt). They form the ] cross-section of a belt and collapse under
            // cloth physics. Vanilla conform preserves their shape by translating them
            // with the body morph diff â€” we replicate that by flagging them as rigid.
            // Gate: pelvis+torso strong, leg weak, narrow Z-span (not a full skirt panel).
            {
                float zSpan = islMax[(size_t)id].z - islMin[(size_t)id].z;
                bool waistSig  = (ip.pelvis > 0.25f) && (ip.torso > 0.15f) && (ip.leg < 0.12f);
                bool narrowBand = zSpan < (meshDiag * 0.08f);
                // A real waistband/belt WRAPS the waist: it is a large island spanning a
                // wide horizontal arc. A small localized island with the same bone
                // signature is a decorative FLAP (e.g. the hip leather flap) that should
                // DRAPE with the body, not rigid-translate. Rigid-locking a flap pins it
                // to vanilla conform while the cloth beside it moves with physics â†’ the
                // two split open into a visible hole in the morph (user-reported, the
                // circled gap). Require a real band size so flaps are excluded.
                float xSpan = islMax[(size_t)id].x - islMin[(size_t)id].x;
                float ySpan = islMax[(size_t)id].y - islMin[(size_t)id].y;
                float horizSpan = std::sqrt(xSpan*xSpan + ySpan*ySpan);
                bool wrapsWaist = (islandSize[(size_t)id] >= 80) &&
                                  (horizSpan > meshDiag * 0.30f);
                if (waistSig && narrowBand && wrapsWaist) {
                    islandRigid[(size_t)id] = 1;
                    islandDrape[(size_t)id] = 0.0f;
                    continue;
                }
            }

            // Island drape level = mean per-vertex drape signal over the island.
            // (Sleeves are handled per-vertex by the sleeve-suppress term in
            // vertDrapeSignal â€” NOT by a whole-island sleeve gate, which would wrongly
            // fitted-ify a large dress island that has sleeves merged into it.)
            float islMeanSignal = islandSignalCnt[(size_t)id] > 0
                ? islandSignalSum[(size_t)id] / (float)islandSignalCnt[(size_t)id] : 0.0f;
            islandDrape[(size_t)id] = Clamp01(islMeanSignal);
        }

        // Surface island id + rigid flags to the rigid-island pass below.
        islandRigidOut.assign(islandRigid.begin(), islandRigid.end());
        islandIdOut.assign(islandId.begin(), islandId.end());

        int rigidIslands = 0;
        for (uint8_t r : islandRigid) if (r) ++rigidIslands;
        HLog("detour: islands shape='%s' total=%d rigid=%d meshDiag=%.2f",
             shapeName.c_str(), numIslands, rigidIslands, meshDiag);
        for (int dbgId = 0; dbgId < std::min(numIslands, 20); ++dbgId) {
            const auto& ip = iprof[(size_t)dbgId];
            float drapeW = ip.pelvis + ip.leg;
            // Z range of the island as a fraction of body height (0=feet, 1=head).
            float zLoF = ClampRange((islMin[(size_t)dbgId].z - g_refMinZ) / bodyZRange, 0.0f, 1.0f);
            float zHiF = ClampRange((islMax[(size_t)dbgId].z - g_refMinZ) / bodyZRange, 0.0f, 1.0f);
            const char* clsStr = islandRigid[(size_t)dbgId] ? "RIGID"
                               : (islandDrape[(size_t)dbgId] < 0.15f) ? "fitted"
                               : (islandDrape[(size_t)dbgId] < 0.55f) ? "soft-drape"
                               : "drape";
            HLog("  isl#%d verts=%d pelv=%.2f leg=%.2f torso=%.2f arm=%.2f z=[%.2f-%.2f] islDrape=%.3f cls=%s",
                 dbgId, islandSize[(size_t)dbgId], ip.pelvis, ip.leg, ip.torso, ip.arm,
                 zLoF, zHiF, islandDrape[(size_t)dbgId], clsStr);
            (void)drapeW;
        }

        // â”€â”€ STEP B: per-vertex drapeAmount â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // PURELY per-vertex: drape is decided by the vertex's own lower-body bone
        // signal + height (vertDrapeSignal). This distinguishes bodice from skirt
        // WITHIN a single connected island (a dress where bodice+skirt+belt are one
        // topological piece â€” the common case; the "belt" is not its own island).
        // We deliberately do NOT blend in the island mean here: a whole-dress island
        // has a low mean (dominated by the bodice) that would wrongly drag the skirt
        // verts toward anchor. The island pass is used ONLY to flag rigid plates.
        const float kSkirtClear = params.bodyClearance;  // full drape gap
        const float kHugClear   = 0.12f;                 // fitted hugs the body
        const float kRigidClear = 0.08f;                 // rigid trim near-zero

        // â”€â”€ Hybrid free-hang drape (Loose-Draped meshes only) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // The bone-based vertDrapeSignal only recognises LOWER-body draping
        // (pelvis+leg+below-waist torso).  A cloak / cape / tabard hangs from the
        // UPPER body (spine/shoulder weights) with ~zero pelvis+leg, so its signal
        // is ~0 and it gets treated as fitted â†’ it collapses to vanilla conform
        // (twisted clipping) instead of draping.
        //
        // Fix (scoped to meshes the USER marked Loose Draped, so no other case
        // changes): a vert that hangs in FREE SPACE away from the body surface is
        // draping cloth regardless of its bone weights.  Measure the gap from the
        // conformed (supportVerts) position to the morphed body via the BVH and
        // raise the drape signal for far-hanging verts.  Rigid-island verts are
        // already excluded by the `continue` above (belt stays protected), and a
        // belt/bodice sits TIGHT to the body (small gap) so it is never caught
        // here even within a Loose-Draped mesh.
        const bool kLooseHangEnabled =
            isLoosePreset && bodyBVH && bodyBVH->valid;
        // Gap thresholds: below kGapNear the vert hugs the body (keep bone signal);
        // above kGapFar it is unambiguously free-hanging cloth (full drape).
        const float kGapNear = 1.0f;
        const float kGapFar  = 4.0f;
        int hangVerts = 0;

        for (size_t i = 0; i < n; ++i) {
            int id = islandId[i];
            if (id < 0 || id >= numIslands) { drapeAmount[i] = 0.0f; continue; }
            if (islandRigid[(size_t)id]) {
                drapeAmount[i]   = 0.0f;
                vertClearance[i] = kRigidClear;
                continue;
            }
            float sig = vertDrapeSignal(i);

            if (kLooseHangEnabled) {
                // Signed gap from conformed position to nearest body surface.
                Vec3 cp{0,0,0}, nrm{0,0,1};
                float d2 = 0.0f;
                if (BVHClosestPoint(*bodyBVH, supportVerts[i], cp, nrm, d2)) {
                    float dist = std::sqrt(d2);
                    float sign = Dot(Sub(supportVerts[i], cp), nrm);  // >0 outside
                    if (sign > 0.0f && dist > kGapNear) {
                        // Map gap [near..far] â†’ hang factor [0..1].
                        float hang = ClampRange((dist - kGapNear) / (kGapFar - kGapNear),
                                                0.0f, 1.0f);
                        if (hang > 0.0f) {
                            float boosted = std::max(sig, hang);
                            if (boosted > sig + 0.01f) ++hangVerts;
                            sig = boosted;
                        }
                    }
                }
            }

            drapeAmount[i]   = sig;
            vertClearance[i] = kHugClear + sig * (kSkirtClear - kHugClear);

        }

        if (kLooseHangEnabled && hangVerts > 0)
            HLog("  free-hang: %d verts boosted to drape (gap>%.1f) shape='%s'",
                 hangVerts, kGapNear, shapeName.c_str());

    }

    // â”€â”€ Non-verlet presets: per-vertex drape from bones (Z fallback if none) â”€â”€
    if (!params.verletCloth) {
        for (size_t i = 0; i < n; ++i) {
            if (profiles && i < profiles->size()) {
                const auto& bp = (*profiles)[i];
                float lower  = Clamp01(bp.pelvis + bp.leg);
                float sleeve = Clamp01((bp.arm + bp.handFoot) * 1.6f);
                drapeAmount[i] = Clamp01(lower * (1.0f - sleeve));
            } else {
                float normZ = ClampRange((baseVerts[i].z - g_refMinZ) / bodyZRange, 0.0f, 1.0f);
                drapeAmount[i] = (normZ < 0.50f) ? 0.50f : 0.0f;
            }
        }
    }

    // drapeEl is the single source of truth downstream; alias it to drapeAmount.
    std::vector<float>& drapeEl = drapeAmount;
    for (size_t i = 0; i < n; ++i)
        if (drapeEl[i] > 0.25f) ++stats.drapeEligibleVerts;

    // Boost anchor for non-drape (upper body / sleeve) verts.
    for (size_t i = 0; i < n; ++i) {
        if (drapeEl[i] < 0.20f) {
            anchorWeight[i] = std::max(anchorWeight[i], 0.65f);
            invMass[i]      = 0.0f;  // fixed: stretch constraints cannot move anchor verts
            ++stats.anchoredVerts;
        }
    }

    // Reduce anchor for loose drape verts â€” let stretch + collision settle them.
    // BUT: enforce a minimum anchor (0.15) to prevent unrestricted drift, which
    // would propagate corrections to areas with no vanilla conform diff (ghost diffs).
    for (size_t i = 0; i < n; ++i) {
        if (drapeEl[i] > 0.25f) {
            float drapeScale = 1.0f - drapeEl[i] * (1.0f - follow[i]);
            // physicsDrape: loose drape verts must have anchor=0 â€” pure physics.
            // The 0.15 minimum was preventing physicsDrape from working at all:
            // 0.15 Ã— anchorStiffness per iter Ã— 80 iter â‰ˆ full pull to supportVerts.
            float minAnchor = params.physicsDrape ? 0.0f : 0.15f;
            anchorWeight[i]  = std::max(Clamp01(anchorWeight[i] * drapeScale), minAnchor);
        }
    }

    // â”€â”€ Slider-source locality â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // Compute per-vert influence: how much did the body actually change at this
    // outfit vert's 3D location for this specific slider?
    //
    // Belly slider  â†’ large change at abdomen, near-zero at ankles/sleeves
    // RM_Heavy      â†’ large change everywhere â†’ solver runs on all verts
    //
    // directInfluence[i]: normalised |morphR - baseR| from body shell
    // effectiveInfluence[i]: direct + mesh propagation (decays with hops)
    //
    // All solver corrections (inward suppression, body collision, drape envelope)
    // are scaled by effectiveInfluence[i], so low-influence verts stay at
    // vanilla conform (supportVerts) and don't accumulate spurious diffs.
    std::vector<float> directInfluence(n, 1.0f);
    std::vector<float> effectiveInfluence(n, 1.0f);

    if (bodyShell && bodyShell->valid) {
        float maxChange = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            directInfluence[i] = SampleBodyChangeMagnitude(*bodyShell, baseVerts[i]);
            maxChange = std::max(maxChange, directInfluence[i]);
        }

        if (maxChange < 1.0e-4f) {
            // Slider barely moves the body â†’ skip cloth pass entirely
            outDiff.clear();
            return false;
        }

        float invMax = 1.0f / maxChange;
        for (size_t i = 0; i < n; ++i)
            directInfluence[i] = Clamp01(directInfluence[i] * invMax);

        // Propagate through cloth mesh (only through drape-eligible verts).
        // Upper-body anchor verts (drapeEl â‰ˆ 0) block propagation so belly
        // influence cannot reach sleeves through the upper torso.
        effectiveInfluence = directInfluence;
        static constexpr float kPropDecay = 0.82f;
        for (int hop = 0; hop < 10; ++hop) {
            std::vector<float> next = effectiveInfluence;
            for (const auto& e : useEdges) {
                if (e.a >= n || e.b >= n) continue;
                if (drapeEl[e.a] > 0.05f)
                    next[e.a] = std::max(next[e.a], effectiveInfluence[e.b] * kPropDecay);
                if (drapeEl[e.b] > 0.05f)
                    next[e.b] = std::max(next[e.b], effectiveInfluence[e.a] * kPropDecay);
            }
            effectiveInfluence = std::move(next);
        }

        for (size_t i = 0; i < n; ++i) {
            if (effectiveInfluence[i] > 0.15f) {
                if (directInfluence[i] > 0.15f) ++stats.influenceDirectVerts;
                else                             ++stats.influencePropagVerts;
            } else {
                ++stats.influenceZeroVerts;
            }
        }
    }

    // â”€â”€ Inward suppression â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // Only used in correction mode (physicsDrape=false).
    // In physicsDrape mode the initial body-push (after collideVertex) handles it.
    // For drape-eligible loose verts: cancel the inward radial component of
    // the vanilla conform delta proportional to looseness.
    //
    // Concept: vanilla conform may follow body concavities (waist pull-in, inner
    // thigh, etc.). A loose skirt/hem should NOT follow inward â€” it should stay
    // at or outside its current radius. Outward conform moves (body expanding) are
    // always kept as-is. Only inward moves are selectively suppressed.
    //
    // For sliders that expand the body (RM_Heavy, BigHips, etc.), ALL diffs are
    // outward â†’ no suppression â†’ result equals vanilla conform. For concave sliders
    // (belly curve-in below a bulge) the inward component is cancelled.
    if (!params.physicsDrape && params.inwardSuppressionStrength > 0.0f) {
        for (size_t i = 0; i < n; ++i) {
            float looseness = Clamp01((1.0f - follow[i]) * drapeEl[i]);
            float suppStrength = Clamp01(looseness * params.inwardSuppressionStrength);
            if (suppStrength < 0.02f) continue;

            float dx = baseVerts[i].x - g_refCenterX;
            float dy = baseVerts[i].y - g_refCenterY;
            float r  = std::sqrt(dx * dx + dy * dy);
            if (r < 1.0e-5f) continue;
            float ox = dx / r, oy = dy / r;

            // Inward component of the full vanilla conform delta for this vert
            Vec3 fullDiff = Sub(supportVerts[i], baseVerts[i]);
            float inwardComp = -(fullDiff.x * ox + fullDiff.y * oy);
            if (inwardComp <= 0.0f) continue;  // outward or neutral: keep it

            // Move finalVerts outward to cancel the inward portion.
            // Scale by slider-source influence so off-body-change verts are untouched.
            float eff = (i < effectiveInfluence.size()) ? effectiveInfluence[i] : 1.0f;
            float correction = inwardComp * suppStrength * eff;
            finalVerts[i].x += ox * correction;
            finalVerts[i].y += oy * correction;
        }
    }

    // Body collision. Primary: 3D BVH (closest point on real morphed surface, push
    // out along real normal). Falls back to radial shell when no BVH available.
    // Body collision using per-vertex clearance: skirt verts get full
    // bodyClearance (cloth hangs free), belt/waist verts get a small clearance
    // (stays fitted), so both can live in the same mesh without the belt being
    // pushed out as far as the skirt.
    // Inner-mesh layer clearance: how far outside an inner garment this mesh must sit.
    auto collideVertex = [&](size_t i, float stiffness) -> bool {
        if (stiffness <= 0.0f) return false;
        float vc = vertClearance[i];
        // Inter-garment ordering is handled entirely by the bidirectional per-vertex
        // anti-clip post-pass (local order from the base mesh). The in-solve collider
        // only handles the BODY â€” pushing off other garments here would be one-
        // directional and could shove a vert to the wrong side of the local order.
        // Body collision (primary).
        if (bodyBVH && bodyBVH->valid) {
            Vec3 target = finalVerts[i];
            if (!BVHCollideVertex(*bodyBVH, target, vc))
                return false;
            finalVerts[i] = Add(finalVerts[i], Scale(Sub(target, finalVerts[i]), Clamp01(stiffness)));
            return true;
        }
        if (!bodyShell || !bodyShell->valid) return false;
        float baseBodyR = 0.0f, morphBodyR = 0.0f;
        if (!SampleBodyShell(*bodyShell, baseVerts[i], baseBodyR, morphBodyR))
            return false;
        float dirX = 0.0f, dirY = 0.0f, baseClothR = 0.0f;
        if (!RadialDir(finalVerts[i], dirX, dirY, baseClothR) &&
            !RadialDir(baseVerts[i], dirX, dirY, baseClothR)) {
            return false;
        }
        float baseGap = std::max(0.0f, RadialRadius(baseVerts[i]) - baseBodyR);
        float preservedGap = std::min(baseGap * params.bodyGapPreserve, params.bodyGapCap);
        float targetR = morphBodyR + vc + preservedGap;
        return MoveRadiallyOut(finalVerts[i], dirX, dirY, targetR, stiffness);
    };

    const int iterations = std::max(1, params.pbdIterations);
    const float gravityStep = params.gravity / (float)iterations;
    const float stretchK = Clamp01(params.stretchStiffness);
    const float stretchStep = 1.0f - std::pow(1.0f - stretchK, 1.0f / (float)iterations);
    static constexpr float kLenEps = 1.0e-6f;

    // New architecture, step 2: in-solve layer order constraints.
    // This only handles the fixable case: this vertex was under an outer garment in
    // the base mesh AND the current outer garment still leaves enough room above
    // the body. No-room coverage is intentionally left to the later collapse pass.
    struct OrderConstraint {
        uint16_t idx = 0;
        float allowedGap = 0.0f;    // max signed distance from body surface
    };
    std::vector<OrderConstraint> orderConstraints;

    auto orderMarginFor = [](int outerPreset) -> float {
        switch (NormalizePreset(outerPreset)) {
        case DRAPE_PRESET_LOOSE_DRAPED:    return 0.12f;
        case DRAPE_PRESET_FLEXIBLE_ARMOR:  return 0.24f;
        case DRAPE_PRESET_RIGID_ARMOR:     return 0.28f;
        case DRAPE_PRESET_BELT:            return 0.20f;
        case DRAPE_PRESET_RIGID_ACCESSORY: return 0.18f;
        default:                           return 0.14f;
        }
    };

    const int orderSelfPreset = NormalizePreset(preset);
    const bool orderSelfCanMove =
        orderSelfPreset != DRAPE_PRESET_RIGID_ARMOR &&
        orderSelfPreset != DRAPE_PRESET_RIGID_ACCESSORY;

    if (orderSelfCanMove &&
        params.orderConstraintStrength > 0.0f &&
        params.orderConstraintMaxStep > 0.0f &&
        outerColliders && !outerColliders->empty() &&
        bodyBVH && bodyBVH->valid) {
        const BodyBVH* baseBodyBVH = g_refBodyBVH.valid ? &g_refBodyBVH : bodyBVH;
        const float kOrderOverlap = 2.75f;
        const float kOrderOverlap2 = kOrderOverlap * kOrderOverlap;
        const float kBaseUnderSlack = 0.06f;

        orderConstraints.reserve(n / 8);
        for (size_t i = 0; i < n; ++i) {
            if (invMass[i] <= 0.0f) continue;
            if (!islandIdOut.empty() && i < islandIdOut.size()) {
                int id = islandIdOut[i];
                if (id >= 0 && id < (int)islandRigidOut.size() &&
                    islandRigidOut[(size_t)id]) {
                    continue;
                }
            }

            float myBaseGap = 0.0f;
            Vec3 tmpNrm{0.0f, 0.0f, 1.0f};
            if (!BVHSignedGap(*baseBodyBVH, baseVerts[i], myBaseGap, &tmpNrm))
                continue;

            bool found = false;
            float bestAllowed = 1.0e30f;
            for (const OuterCollider& oc : *outerColliders) {
                if (!oc.Base().valid || !oc.Current().valid) continue;

                Vec3 bcp{0.0f, 0.0f, 0.0f}, bnrm{0.0f, 0.0f, 1.0f};
                float bd2 = 0.0f;
                int baseTri = -1;
                float bu = 0.0f, bv = 0.0f, bw = 0.0f;
                if (!BVHClosestPointEx(oc.Base(), baseVerts[i], bcp, bnrm, bd2,
                                        &baseTri, &bu, &bv, &bw)) {
                    continue;
                }
                if (bd2 > kOrderOverlap2) continue;
                if (baseTri < 0 || baseTri >= (int)oc.Current().tris.size()) continue;

                float outerBaseGap = 0.0f;
                if (!BVHSignedGap(*baseBodyBVH, bcp, outerBaseGap, &tmpNrm))
                    continue;
                if (myBaseGap >= outerBaseGap + kBaseUnderSlack)
                    continue; // base mesh says this vertex is not below this outer.

                Vec3 currentOuterPoint = TriPointBary(oc.Current(), baseTri, bu, bv, bw);
                float outerGap = 0.0f;
                if (!BVHSignedGap(*bodyBVH, currentOuterPoint, outerGap, &tmpNrm))
                    continue;

                float allowed = outerGap - orderMarginFor(oc.preset);
                float feasibleFloor = std::max(params.orderConstraintBodyFloor,
                                               vertClearance[i] + 0.03f);
                if (allowed < feasibleFloor)
                    continue; // Problem B: no room to solve by position.

                bestAllowed = std::min(bestAllowed, allowed);
                found = true;
            }

            if (found)
                orderConstraints.push_back(OrderConstraint{static_cast<uint16_t>(i), bestAllowed});
        }

        if (!orderConstraints.empty()) {
            HLog("detour: order-constraints shape='%s' verts=%zu outers=%zu",
                 shapeName.c_str(), orderConstraints.size(), outerColliders->size());
        }
    }

    auto solveOrderConstraints = [&](const std::vector<float>& mass, float strength) {
        if (orderConstraints.empty() || !bodyBVH || !bodyBVH->valid || strength <= 0.0f)
            return;

        const float kMinViolation = 0.01f;
        const float maxStep = std::max(0.0f, params.orderConstraintMaxStep);
        for (const OrderConstraint& c : orderConstraints) {
            size_t i = (size_t)c.idx;
            if (i >= n) continue;
            float w = (i < mass.size()) ? Clamp01(mass[i]) : 1.0f;
            if (w <= 0.0f) continue;

            Vec3 nrm{0.0f, 0.0f, 1.0f};
            float myGap = 0.0f;
            if (!BVHSignedGap(*bodyBVH, finalVerts[i], myGap, &nrm))
                continue;
            float violation = myGap - c.allowedGap;
            if (violation <= kMinViolation)
                continue;

            float step = std::min(violation * Clamp01(strength) * w, maxStep);
            if (step <= 0.0f)
                continue;
            finalVerts[i] = Add(finalVerts[i], Scale(nrm, -step));
            ++stats.layerAdjusted;
        }
    };

    // Support rest lengths: edge lengths in the VANILLA CONFORM result (supportVerts),
    // not the base mesh. This prevents stretch constraints from fighting body-induced
    // expansions (e.g., belly expansion shouldn't pull arm area inward). With support
    // rest lengths, edges that are "correct" in vanilla conform have zero stretch force;
    // only edges displaced by inward suppression or body collision need relaxation.
    std::vector<float> supportRestLen(useEdges.size(), 0.0f);
    for (size_t ei = 0; ei < useEdges.size(); ++ei) {
        const auto& e = useEdges[ei];
        if (e.a >= n || e.b >= n) { supportRestLen[ei] = e.restLength; continue; }
        Vec3 d = Sub(supportVerts[e.a], supportVerts[e.b]);
        float l = Len(d);
        supportRestLen[ei] = l > kLenEps ? l : e.restLength;
    }

    // Support rest lengths for bending constraints (conformed garment distances).
    // Same rationale as supportRestLen: bending must not pull the cloth back to
    // its slim base shape when the body grows.
    const std::vector<BendingConstraint>* bendingPtr = nullptr;
    std::vector<float> supportBendRestLen;
    {
        auto bendingIt = g_shapeBending.find(shapeName);
        if (bendingIt != g_shapeBending.end() && !bendingIt->second.empty()) {
            bendingPtr = &bendingIt->second;
            const auto& bcs = *bendingPtr;
            supportBendRestLen.resize(bcs.size(), 0.0f);
            for (size_t bi = 0; bi < bcs.size(); ++bi) {
                const auto& bc = bcs[bi];
                if (bc.c >= n || bc.d >= n) { supportBendRestLen[bi] = bc.restLength; continue; }
                Vec3 d = Sub(supportVerts[bc.c], supportVerts[bc.d]);
                float l = Len(d);
                supportBendRestLen[bi] = l > kLenEps ? l : bc.restLength;
            }
        }
    }

    // â”€â”€ Physics drape: initial body-push â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // Starting from base mesh (slim body), push all verts outside the morphed
    // body BEFORE the PBD loop. This establishes the cloth's initial position
    // on the morphed body surface, after which gravity + stretch + envelope
    // refine the drape. Run at high stiffness for fast convergence.
    if (params.physicsDrape && !params.verletCloth && bodyShell && bodyShell->valid &&
        params.physicsDrapeInitPushPasses > 0) {
        for (int push = 0; push < params.physicsDrapeInitPushPasses; ++push) {
            for (size_t i = 0; i < n; ++i) {
                float freeMass = Clamp01(invMass[i] * 1.2f);
                collideVertex(i, params.physicsDrapeInitPushStiffness * freeMass);
            }
        }
    }

    // â”€â”€ Proximity constraints (synthetic edges for near-touching cross-island verts) â”€â”€
    // Problem: a belt or trim esculpted as part of a single dress mesh has no separate
    // island. Its vertices are topologically connected to the surrounding cloth and
    // move with it, but the belt straps can straddle seams where adjacent vert pairs
    // have NO edge between them (different UV islands â†’ no shared edge in the mesh).
    // During the Verlet solve those pairs drift freely, tearing the belt apart.
    //
    // Solution: before the solve, find all base-mesh vertex pairs that are physically
    // close (< kProxDist) but have NO topological edge connecting them (different islands
    // OR simply not in useEdges). Add them as synthetic stretch constraints with the
    // VANILLA rest length. They resist ONLY over-stretching (like all stretch constraints),
    // so the cloth can still drape â€” they just prevent the strap from tearing open.
    std::vector<EdgeConstraint> proxEdges;
    std::vector<float>          proxRestLen;
    std::vector<uint8_t>        proxIsBoundarySeam;
    struct SeamVectorPair {
        uint16_t a = 0;
        uint16_t b = 0;
        Vec3 supportOffset{0.0f, 0.0f, 0.0f};
        uint8_t boundary = 0;
    };
    std::vector<SeamVectorPair> seamVectorPairs;
    std::unordered_set<uint64_t> seamVectorSeen;
    std::vector<std::vector<uint32_t>> weldGroups;
    std::vector<std::vector<uint32_t>> exactSeamGroups;
    const bool needsExactSeamWeld =
        params.verletCloth ||
        normPreset == DRAPE_PRESET_LOOSE_DRAPED ||
        normPreset == DRAPE_PRESET_FITTED_CLOTH ||
        normPreset == DRAPE_PRESET_TUBULAR;
    if (needsExactSeamWeld && n <= 0xFFFF) {
        std::unordered_map<std::string, std::vector<uint32_t>> coincidence;
        coincidence.reserve(n);
        auto quantKey = [](const Vec3& p) -> std::string {
            auto q = [](float v) -> int32_t {
                return (int32_t)std::llround(v * 1000.0f);
            };
            return std::to_string(q(p.x)) + "|" +
                   std::to_string(q(p.y)) + "|" +
                   std::to_string(q(p.z));
        };
        for (size_t i = 0; i < n; ++i)
            coincidence[quantKey(baseVerts[i])].push_back((uint32_t)i);

        std::vector<std::string> coincidenceKeys;
        coincidenceKeys.reserve(coincidence.size());
        for (const auto& kv : coincidence)
            coincidenceKeys.push_back(kv.first);
        std::sort(coincidenceKeys.begin(), coincidenceKeys.end());

        const float kExactBaseDist2 = 0.003f * 0.003f;
        for (const std::string& key : coincidenceKeys) {
            auto& group = coincidence[key];
            if (group.size() <= 1) continue;
            std::sort(group.begin(), group.end());
            std::vector<std::vector<uint32_t>> clusters;
            for (uint32_t vi : group) {
                bool placed = false;
                for (auto& cluster : clusters) {
                    if (!cluster.empty() &&
                        Len2(Sub(baseVerts[vi], baseVerts[cluster.front()])) <= kExactBaseDist2) {
                        cluster.push_back(vi);
                        placed = true;
                        break;
                    }
                }
                if (!placed)
                    clusters.push_back({ vi });
            }
            for (auto& cluster : clusters) {
                if (cluster.size() <= 1) continue;
                exactSeamGroups.push_back(cluster);
                if (params.verletCloth)
                    weldGroups.push_back(cluster);
            }
        }
        if (!exactSeamGroups.empty())
            HLog("detour: exact-seam-groups shape='%s' groups=%zu",
                 shapeName.c_str(), exactSeamGroups.size());
    }
    const bool needsNonTopoSeamCaps =
        params.verletCloth ||
        normPreset == DRAPE_PRESET_FITTED_CLOTH ||
        normPreset == DRAPE_PRESET_TUBULAR;
    if (needsNonTopoSeamCaps && n <= 0xFFFF) {
        const float kProxDist  = 0.50f;
        const float kProxDist2 = kProxDist * kProxDist;
        const float pcell = kProxDist;
        auto pkey = [&](const Vec3& p) -> uint64_t {
            auto q = [&](float v) -> uint64_t { return (uint64_t)(uint32_t)(int32_t)std::llround(v / pcell); };
            return (q(p.x) * 73856093ull) ^ (q(p.y) * 19349663ull) ^ (q(p.z) * 83492791ull);
        };
        // Build spatial hash over base mesh positions.
        std::unordered_map<uint64_t, std::vector<uint32_t>> pgrid;
        pgrid.reserve(n);
        for (size_t i = 0; i < n; ++i) pgrid[pkey(baseVerts[i])].push_back((uint32_t)i);

        // Build a fast lookup for existing edges (so we don't duplicate them).
        std::unordered_set<uint64_t> edgeSet;
        edgeSet.reserve(useEdges.size() * 2);
        for (const auto& e : useEdges) {
            uint32_t lo = std::min((uint32_t)e.a, (uint32_t)e.b);
            uint32_t hi = std::max((uint32_t)e.a, (uint32_t)e.b);
            edgeSet.insert(((uint64_t)lo << 32) | hi);
        }
        std::unordered_set<uint64_t> proxSeen;
        proxSeen.reserve(n * 2);
        auto addProxEdge = [&](uint32_t a, uint32_t b, bool boundarySeam) -> bool {
            if (a >= n || b >= n || a == b) return false;
            uint32_t lo = std::min(a, b);
            uint32_t hi = std::max(a, b);
            uint64_t key = ((uint64_t)lo << 32) | hi;
            if (edgeSet.count(key)) return false;
            if (!proxSeen.insert(key).second) return false;
            float vLen = Len(Sub(baseVerts[hi], baseVerts[lo]));
            if (vLen < 1.0e-4f) return false;
            EdgeConstraint ec;
            ec.a = (uint16_t)lo; ec.b = (uint16_t)hi; ec.restLength = vLen;
            proxEdges.push_back(ec);
            proxRestLen.push_back(vLen);
            proxIsBoundarySeam.push_back(boundarySeam ? 1 : 0);
            return true;
        };
        seamVectorSeen.reserve(n * 2);
        auto addSeamVectorPair = [&](uint32_t a, uint32_t b, bool boundarySeam) -> bool {
            if (a >= n || b >= n || a == b) return false;
            uint32_t lo = std::min(a, b);
            uint32_t hi = std::max(a, b);
            uint64_t key = ((uint64_t)lo << 32) | hi;
            if (!seamVectorSeen.insert(key).second) return false;
            SeamVectorPair sv;
            sv.a = (uint16_t)a;
            sv.b = (uint16_t)b;
            sv.supportOffset = Sub(supportVerts[b], supportVerts[a]);
            sv.boundary = boundarySeam ? 1 : 0;
            seamVectorPairs.push_back(sv);
            return true;
        };

        for (size_t i = 0; i < n; ++i) {
            const Vec3& p = baseVerts[i];
            for (int dx=-1;dx<=1;++dx) for (int dy=-1;dy<=1;++dy) for (int dz=-1;dz<=1;++dz) {
                Vec3 qp{ p.x+dx*pcell, p.y+dy*pcell, p.z+dz*pcell };
                auto it = pgrid.find(pkey(qp)); if (it==pgrid.end()) continue;
                for (uint32_t j : it->second) {
                    if (j <= (uint32_t)i) continue;
                    if (Len2(Sub(baseVerts[j], p)) > kProxDist2) continue;
                    // Skip if there's already a real edge between i and j.
                    // Rest length from BASE mesh (not support/morphed): this preserves
                    // the sculpted 3D shape (e.g. belt ] cross-section thickness).
                    // Using support rest length was wrong â€” on body morph the support
                    // already collapses the belt sides together, so the constraint
                    // then actively flattened the belt to match that morphed distance.
                    addProxEdge((uint32_t)i, j, false);
                }
            }
        }
        int boundarySeamPairs = 0;
        const bool buildBoundarySeams =
            normPreset == DRAPE_PRESET_LOOSE_DRAPED &&
            params.verletCloth &&
            tris && !tris->empty() &&
            n <= 2500;
        if (buildBoundarySeams) {
            std::unordered_map<uint32_t, int> edgeUseCount;
            edgeUseCount.reserve(tris->size() * 3);
            auto countTriEdge = [&](uint16_t a, uint16_t b) {
                if (a >= n || b >= n || a == b) return;
                ++edgeUseCount[EdgeKey(a, b)];
            };
            for (const auto& t : *tris) {
                countTriEdge(t[0], t[1]);
                countTriEdge(t[1], t[2]);
                countTriEdge(t[2], t[0]);
            }

            struct BoundaryEdge {
                uint16_t a, b;
                Vec3 mid;
                float len;
            };
            std::vector<BoundaryEdge> boundaryEdges;
            boundaryEdges.reserve(edgeUseCount.size());
            auto addBoundaryEdge = [&](uint16_t a, uint16_t b) {
                if (a >= n || b >= n || a == b) return;
                auto it = edgeUseCount.find(EdgeKey(a, b));
                if (it == edgeUseCount.end() || it->second != 1) return;
                float len = Len(Sub(baseVerts[a], baseVerts[b]));
                if (len < 0.08f) return;
                boundaryEdges.push_back({a, b, Scale(Add(baseVerts[a], baseVerts[b]), 0.5f), len});
            };
            for (const auto& t : *tris) {
                addBoundaryEdge(t[0], t[1]);
                addBoundaryEdge(t[1], t[2]);
                addBoundaryEdge(t[2], t[0]);
            }

            struct BoundaryCandidate {
                int a, b;
                float score;
                bool flip;
            };
            std::vector<BoundaryCandidate> candidates;
            const float kMidNear2 = 0.90f * 0.90f;
            for (size_t ai = 0; ai < boundaryEdges.size(); ++ai) {
                const auto& ea = boundaryEdges[ai];
                for (size_t bi = ai + 1; bi < boundaryEdges.size(); ++bi) {
                    const auto& eb = boundaryEdges[bi];
                    if (ea.a == eb.a || ea.a == eb.b || ea.b == eb.a || ea.b == eb.b) continue;
                    float lenRatio = std::min(ea.len, eb.len) / std::max(ea.len, eb.len);
                    if (lenRatio < 0.55f) continue;
                    if (Len2(Sub(ea.mid, eb.mid)) > kMidNear2) continue;
                    float same = Len(Sub(baseVerts[ea.a], baseVerts[eb.a])) +
                                 Len(Sub(baseVerts[ea.b], baseVerts[eb.b]));
                    float flipped = Len(Sub(baseVerts[ea.a], baseVerts[eb.b])) +
                                    Len(Sub(baseVerts[ea.b], baseVerts[eb.a]));
                    bool useFlip = flipped < same;
                    float score = useFlip ? flipped : same;
                    float maxScore = 1.10f + 0.35f * std::max(ea.len, eb.len);
                    if (score > maxScore) continue;
                    candidates.push_back({(int)ai, (int)bi, score, useFlip});
                }
            }
            std::sort(candidates.begin(), candidates.end(),
                      [](const BoundaryCandidate& a, const BoundaryCandidate& b) {
                          return a.score < b.score;
                      });

            std::vector<uint8_t> usedBoundaryEdge(boundaryEdges.size(), 0);
            for (const auto& c : candidates) {
                if (usedBoundaryEdge[(size_t)c.a] || usedBoundaryEdge[(size_t)c.b]) continue;
                usedBoundaryEdge[(size_t)c.a] = 1;
                usedBoundaryEdge[(size_t)c.b] = 1;
                const auto& ea = boundaryEdges[(size_t)c.a];
                const auto& eb = boundaryEdges[(size_t)c.b];
                if (c.flip) {
                    addSeamVectorPair(ea.a, eb.b, true);
                    addSeamVectorPair(ea.b, eb.a, true);
                    boundarySeamPairs += addProxEdge(ea.a, eb.b, true) ? 1 : 0;
                    boundarySeamPairs += addProxEdge(ea.b, eb.a, true) ? 1 : 0;
                } else {
                    addSeamVectorPair(ea.a, eb.a, true);
                    addSeamVectorPair(ea.b, eb.b, true);
                    boundarySeamPairs += addProxEdge(ea.a, eb.a, true) ? 1 : 0;
                    boundarySeamPairs += addProxEdge(ea.b, eb.b, true) ? 1 : 0;
                }
            }

            HLog("detour: boundary-seam-constraints shape='%s' boundaryEdges=%zu candidates=%zu proxPairs=%d vectorPairs=%zu",
                 shapeName.c_str(), boundaryEdges.size(), candidates.size(),
                 boundarySeamPairs, seamVectorPairs.size());
        }
        if (!proxEdges.empty())
            HLog("detour: prox-constraints shape='%s' synthetic=%zu",
                 shapeName.c_str(), proxEdges.size());
    }

    // â”€â”€ Cloth drape solve (momentum-free relaxation) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // Keeps the key idea from johnBuffer/ClothSimulation â€” distance constraints
    // that resist ONLY stretching, so the cloth folds and hangs instead of
    // shrink-wrapping the body â€” but WITHOUT Verlet momentum. Momentum let the
    // constraint corrections feed back into velocity and accumulate energy, which
    // tore the mesh apart (hem verts reached escape velocity). Pure Gauss-Seidel
    // relaxation toward a gravity-loaded equilibrium always converges â†’ no tears.
    // A hard per-vert max-drop and a hard edge-length cap make it unconditionally
    // stable. Pinned verts (low drape eligibility) hold the garment at the waist.
    if (params.verletCloth) {
        std::vector<float> m(n, 0.0f);   // effective inv-mass; 0 = pinned
        for (size_t i = 0; i < n; ++i) {
            bool pinned = (invMass[i] <= 0.0f) || (drapeEl[i] < 0.20f);
            m[i] = pinned ? 0.0f : Clamp01(invMass[i]);
        }

        // â”€â”€ Seam welding â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // Game meshes split vertices at UV/material seams: two (or more) verts
        // share a 3D position but have different indices and NO edge between them.
        // Vanilla conform moves them identically; our physics moves each one based
        // on its own edge fan, so they drift apart â†’ visible slits with the body
        // poking through, and detached hem-trim flapping. Group verts that are
        // coincident in the base mesh and force them to move as one each pass.
        // Exact UV/material seam groups are built once before this branch and
        // already included in weldGroups for the cloth solve.

        // â”€â”€ Inter-island junction stitching â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // The dress bodice (fitted, stays put) and the skirt (drape, falls) are
        // SEPARATE islands that merely TOUCH at the waist with no shared edges
        // (verified in the log: bodice z=[0.63-0.97], skirt z=[0.05-0.64]). When
        // the body grows, the fitted bodice holds while the draping skirt moves,
        // and the unwelded gap between them opens into a visible hole at the back.
        // Coincidence welding misses it because the two borders aren't EXACTLY
        // coincident. Here we explicitly pair near-touching verts from DIFFERENT
        // islands and weld them so the skirt top tracks the bodice bottom.
        if (!islandIdOut.empty() && islandIdOut.size() == n) {
            // Spatial hash for O(n) neighbour lookup (cell â‰ˆ kStitchDist).
            const float kStitchDist  = 1.50f;
            const float kStitchDist2 = kStitchDist * kStitchDist;
            const float cell = kStitchDist;
            auto cellKey = [&](const Vec3& p) -> uint64_t {
                auto q = [&](float v) -> uint64_t {
                    return (uint64_t)(uint32_t)(int32_t)std::llround(v / cell);
                };
                return (q(p.x) * 73856093ull) ^ (q(p.y) * 19349663ull) ^ (q(p.z) * 83492791ull);
            };
            std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
            grid.reserve(n);
            for (size_t i = 0; i < n; ++i)
                grid[cellKey(baseVerts[i])].push_back((uint32_t)i);

            // For each vert, find the NEAREST vert in a DIFFERENT island within
            // kStitchDist and weld that PAIR (2 verts only â€” never a blob, so the
            // skirt border can't collapse to a point). Each pair is its own group;
            // the existing weldSeams keeps them coincident every substep.
            // Gate: the horizontal (XY) offset must be small â€” a bodiceâ†’skirt junction
            // is verts at the same circumference but different Z. This lets a large
            // VERTICAL gap stitch (the real hole) without pulling front-to-back.
            const float kStitchXY2 = 0.45f * 0.45f;
            int stitched = 0;
            std::unordered_map<uint64_t,int> islandPairCount;  // diag: which islands join
            for (size_t i = 0; i < n; ++i) {
                int idi = islandIdOut[i];
                if (idi < 0) continue;
                // Never stitch FROM a rigid island â€” rigid islands use translate pass,
                // not the PBD solver, so welding them to cloth verts drags the cloth
                // border to the rigid position and creates a crease at the boundary.
                if (idi < (int)islandRigidOut.size() && islandRigidOut[(size_t)idi]) continue;
                const Vec3& p = baseVerts[i];
                float bestD2 = kStitchDist2;
                int   bestJ  = -1;
                for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    Vec3 qp{ p.x + dx*cell, p.y + dy*cell, p.z + dz*cell };
                    auto it = grid.find(cellKey(qp));
                    if (it == grid.end()) continue;
                    for (uint32_t j : it->second) {
                        if ((size_t)j == i) continue;
                        if (islandIdOut[j] == idi) continue;     // same island: skip
                        // Also skip if partner is rigid â€” same reason.
                        int idj = islandIdOut[j];
                        if (idj >= 0 && idj < (int)islandRigidOut.size() && islandRigidOut[(size_t)idj]) continue;
                        float ddx = baseVerts[j].x - p.x, ddy = baseVerts[j].y - p.y;
                        if (ddx*ddx + ddy*ddy > kStitchXY2) continue;  // not vertically aligned
                        float d2 = Len2(Sub(baseVerts[j], p));
                        if (d2 < bestD2) { bestD2 = d2; bestJ = (int)j; }
                    }
                }
                // Only weld in one direction (i < bestJ) to avoid duplicate pairs.
                if (bestJ >= 0 && (size_t)bestJ > i) {
                    weldGroups.push_back({ (uint32_t)i, (uint32_t)bestJ });
                    ++stitched;
                    int a = idi, b = islandIdOut[bestJ];
                    if (a > b) std::swap(a, b);
                    ++islandPairCount[((uint64_t)a << 32) | (uint32_t)b];
                }
            }
            if (stitched > 0) {
                HLog("detour: junction-stitch shape='%s' pairs=%d", shapeName.c_str(), stitched);
                for (auto& kv : islandPairCount)
                    HLog("    stitch isl#%d<->isl#%d count=%d",
                         (int)(kv.first >> 32), (int)(kv.first & 0xFFFFFFFF), kv.second);
            }
        }

        auto weldSeams = [&]() {
            for (auto& g : weldGroups) {
                // If any member is pinned, the whole seam stays at the conformed
                // fit; otherwise move all members to their average position.
                int pinned = -1;
                for (uint32_t vi : g) if (m[vi] <= 0.0f) { pinned = (int)vi; break; }
                if (pinned >= 0) {
                    Vec3 p = supportVerts[pinned];
                    for (uint32_t vi : g) finalVerts[vi] = p;
                } else {
                    Vec3 avg{0.0f, 0.0f, 0.0f};
                    for (uint32_t vi : g) avg = Add(avg, finalVerts[vi]);
                    avg = Scale(avg, 1.0f / (float)g.size());
                    for (uint32_t vi : g) finalVerts[vi] = avg;
                }
            }
        };

        const int   substeps  = std::max(1, params.pbdIterations);
        const int   cIters    = std::max(1, params.constraintIterations);
        const float gStep     = params.gravity;            // per-step downward nudge
        const float stretchK2 = Clamp01(params.stretchStiffness);
        const float maxDrop   = params.clothMaxDrop;
        // Edges may elongate up to +10%: enough for gravity to reorient the skirt
        // off the body, but tight enough that the hem can't over-stretch into a
        // melted sag or tear open at the back. (Seam welding handles real splits.)
        const float kMaxStretch = 1.10f;

        auto solveStretch = [&](float k) {
            for (size_t ei = 0; ei < useEdges.size(); ++ei) {
                const auto& e = useEdges[ei];
                if (e.a >= n || e.b >= n) continue;
                float w1 = m[e.a], w2 = m[e.b];
                float wSum = w1 + w2;
                if (wSum <= kLenEps) continue;
                Vec3 dir = Sub(finalVerts[e.a], finalVerts[e.b]);
                float len = Len(dir);
                if (len <= kLenEps) continue;
                float rest = (ei < supportRestLen.size()) ? supportRestLen[ei] : e.restLength;
                if (len <= rest) continue;            // slack/compressed â†’ fold freely
                Vec3 delta = Scale(dir, ((len - rest) / len) * k / wSum);
                finalVerts[e.a] = Sub(finalVerts[e.a], Scale(delta, w1));
                finalVerts[e.b] = Add(finalVerts[e.b], Scale(delta, w2));
                ++stats.stretchAdjusted;
            }
        };

        // Proximity constraints: bidirectional distance-preserving for vert pairs
        // that are near-touching in the base mesh but have no topological edge.
        // Bidirectional (resist BOTH stretch and compression) so belt straps can't
        // drift apart OR bunch together â€” they hold their vanilla relative position.
        auto solveProxStretch = [&](float k) {
            for (size_t ei = 0; ei < proxEdges.size(); ++ei) {
                const auto& e = proxEdges[ei];
                if (e.a >= n || e.b >= n) continue;
                float w1 = m[e.a], w2 = m[e.b];
                float wSum = w1 + w2;
                if (wSum <= kLenEps) continue;
                Vec3 dir = Sub(finalVerts[e.a], finalVerts[e.b]);
                float len = Len(dir);
                if (len <= kLenEps) continue;
                float rest = proxRestLen[ei];
                if (std::fabs(len - rest) < 0.01f) continue;   // within tolerance
                Vec3 delta = Scale(dir, ((len - rest) / len) * k / wSum);
                finalVerts[e.a] = Sub(finalVerts[e.a], Scale(delta, w1));
                finalVerts[e.b] = Add(finalVerts[e.b], Scale(delta, w2));
            }
        };

        for (int s = 0; s < substeps; ++s) {
            // 1) Gravity nudge (NO momentum). Free verts only.
            for (size_t i = 0; i < n; ++i) {
                if (m[i] <= 0.0f) continue;
                finalVerts[i].z -= gStep;
                ++stats.gravityAdjusted;
            }

            // 2) Constraint relaxation.
            for (int it = 0; it < cIters; ++it) {
                solveStretch(stretchK2);                 // asymmetric: resist stretch only
                // Proxy constraints run at higher stiffness (1.0) to act as structural
                // braces â€” they preserve sculpted shapes (belt ] cross-section) which
                // are NOT topological edges and would otherwise collapse under gravity.
                if (!proxEdges.empty()) solveProxStretch(1.0f);

                // Light bending for fold smoothness (conformed rest distance).
                if (params.bendingStiffness > 0.0f && bendingPtr) {
                    const auto& bcs = *bendingPtr;
                    const float bK = Clamp01(params.bendingStiffness);
                    for (size_t bi = 0; bi < bcs.size(); ++bi) {
                        const auto& bc = bcs[bi];
                        if (bc.c >= n || bc.d >= n) continue;
                        float w1 = m[bc.c], w2 = m[bc.d];
                        float wSum = w1 + w2;
                        if (wSum <= kLenEps) continue;
                        Vec3 dir = Sub(finalVerts[bc.c], finalVerts[bc.d]);
                        float len = Len(dir);
                        if (len <= kLenEps) continue;
                        float bRest = (bi < supportBendRestLen.size()) ? supportBendRestLen[bi] : bc.restLength;
                        Vec3 delta = Scale(dir, ((len - bRest) / len) * bK / wSum);
                        finalVerts[bc.c] = Sub(finalVerts[bc.c], Scale(delta, w1));
                        finalVerts[bc.d] = Add(finalVerts[bc.d], Scale(delta, w2));
                        ++stats.bendingAdjusted;
                    }
                }

                // Body collision (BVH primary). Runs every substep to keep verts
                // on the real body surface throughout relaxation.
                for (size_t i = 0; i < n; ++i) {
                    if (m[i] <= 0.0f) continue;
                    if (collideVertex(i, params.collisionStiffness)) ++stats.bodyAdjusted;
                    ++stats.bodySamples;
                }

                solveOrderConstraints(m, params.orderConstraintStrength);

                // Re-pin pinned verts to the conformed fit.
                for (size_t i = 0; i < n; ++i)
                    if (m[i] <= 0.0f) finalVerts[i] = supportVerts[i];

                // Weld seams so coincident (duplicate) verts never split.
                weldSeams();
            }

            // 3) Hard stretch cap: no edge may exceed rest*kMaxStretch. This is the
            //    definitive anti-tear â€” even if relaxation under-converges, edges
            //    can never blow up. Two strong asymmetric passes against the cap.
            //    Also enforce proxy constraints so belt straps don't exceed cap either.
            if (!proxEdges.empty()) solveProxStretch(1.0f);
            for (int it = 0; it < 2; ++it) {
                for (size_t ei = 0; ei < useEdges.size(); ++ei) {
                    const auto& e = useEdges[ei];
                    if (e.a >= n || e.b >= n) continue;
                    Vec3 dir = Sub(finalVerts[e.a], finalVerts[e.b]);
                    float len = Len(dir);
                    float rest = (ei < supportRestLen.size()) ? supportRestLen[ei] : e.restLength;
                    float cap = rest * kMaxStretch;
                    if (len <= cap || len <= kLenEps) continue;
                    // Weights: use inverse-mass when available, but if BOTH verts are
                    // pinned (m==0) the edge would otherwise ESCAPE the cap entirely and
                    // could be stretched arbitrarily (measured 15x on the cloak collar,
                    // where most verts are anchored). A blown-up edge is a visible tear
                    // regardless of pinning, so cap it anyway by splitting the correction
                    // 50/50. Pinned verts only define the conformed fit â€” if that fit is
                    // itself torn (slim garment on a heavy body), it must still be capped.
                    float w1 = m[e.a], w2 = m[e.b];
                    float wSum = w1 + w2;
                    if (wSum <= kLenEps) { w1 = 0.5f; w2 = 0.5f; wSum = 1.0f; }
                    Vec3 delta = Scale(dir, ((len - cap) / len) / wSum);
                    finalVerts[e.a] = Sub(finalVerts[e.a], Scale(delta, w1));
                    finalVerts[e.b] = Add(finalVerts[e.b], Scale(delta, w2));
                }
            }

            // 4) Hard max-drop clamp: a drape vert can sag at most maxDrop below its
            //    conformed Z. Bounds the result and kills any runaway.
            if (maxDrop > 0.0f) {
                for (size_t i = 0; i < n; ++i) {
                    if (m[i] <= 0.0f) continue;
                    float floorZ = supportVerts[i].z - maxDrop;
                    if (finalVerts[i].z < floorZ) finalVerts[i].z = floorZ;
                }
            }

            // Final weld after the hard caps so seams end the step closed.
            weldSeams();
        }

        // TEAR DIAGNOSTIC: worst stretch right after the verlet cap, to isolate it.
        {
            float worst = 1.0f; int over = 0;
            for (size_t ei = 0; ei < useEdges.size(); ++ei) {
                const auto& e = useEdges[ei];
                if (e.a >= n || e.b >= n) continue;
                float rest = (ei < supportRestLen.size()) ? supportRestLen[ei] : e.restLength;
                if (rest <= 1e-6f) continue;
                float ratio = Len(Sub(finalVerts[e.a], finalVerts[e.b])) / rest;
                if (ratio > 1.5f) ++over;
                if (ratio > worst) worst = ratio;
            }
            HLog("detour: TEAR shape='%s' stage='post-verlet-cap' worstStretch=%.2f over1.5=%d",
                 shapeName.c_str(), worst, over);
        }
    }

    if (!params.verletCloth)
    for (int iter = 0; iter < iterations; ++iter) {
        if (params.gravity > 0.0f) {
            for (size_t i = 0; i < n; ++i) {
                // Only apply gravity to drape-eligible verts (lower panel).
                // Upper body, collar, chest, sleeves must NOT fall with gravity.
                if (drapeEl[i] < 0.25f) continue;
                float freeMass = invMass[i] * (1.0f - anchorWeight[i] * 0.65f);
                if (freeMass <= 0.0f) continue;
                finalVerts[i].z -= gravityStep * freeMass * drapeEl[i];
                ++stats.gravityAdjusted;
            }
        }

        if (stretchStep > 0.0f && !useEdges.empty()) {
            for (size_t ei = 0; ei < useEdges.size(); ++ei) {
                const auto& e = useEdges[ei];
                if (e.a >= finalVerts.size() || e.b >= finalVerts.size())
                    continue;

                float w1 = e.a < invMass.size() ? Clamp01(invMass[e.a]) : 1.0f;
                float w2 = e.b < invMass.size() ? Clamp01(invMass[e.b]) : 1.0f;
                float wSum = w1 + w2;
                if (wSum <= kLenEps)
                    continue;

                Vec3 dir = Sub(finalVerts[e.a], finalVerts[e.b]);
                float len = Len(dir);
                if (len <= kLenEps)
                    continue;

                // Use SUPPORT rest lengths (conformed garment edge length) in BOTH
                // modes. The conformed garment already fits the morphed body, so
                // stretch holds the cloth at that fitted size instead of fighting
                // body growth back toward the slim base mesh (which crumples it).
                float restLen = (ei < supportRestLen.size())
                    ? supportRestLen[ei] : e.restLength;
                Vec3 delta = Scale(dir, ((len - restLen) / len) * stretchStep / wSum);
                if (w1 > 0.0f)
                    finalVerts[e.a] = Sub(finalVerts[e.a], Scale(delta, w1));
                if (w2 > 0.0f)
                    finalVerts[e.b] = Add(finalVerts[e.b], Scale(delta, w2));
                ++stats.stretchAdjusted;
            }
        }

        if (bodyShell && bodyShell->valid) {
            float collisionStep = params.collisionStiffness;
            for (size_t i = 0; i < n; ++i) {
                float freeMass = ClampRange(invMass[i] * 1.15f, 0.0f, 1.0f);
                // Scale body collision by slider-source influence: body changes near
                // the belly only push belly area verts, not distant skirt hem verts.
                float eff = (i < effectiveInfluence.size()) ? effectiveInfluence[i] : 1.0f;
                // In physics drape, collision is the ONLY force holding the cloth on
                // the body against gravity. It must stay active for every slider, even
                // ones with low slider-source influence â€” otherwise the cloth sinks in.
                if (params.physicsDrape) eff = std::max(eff, 0.60f);
                if (collideVertex(i, collisionStep * freeMass * eff))
                    ++stats.bodyAdjusted;
                ++stats.bodySamples;
            }
        }

        solveOrderConstraints(invMass, params.orderConstraintStrength);

        if (params.anchorStiffness > 0.0f) {
            for (size_t i = 0; i < n; ++i) {
                float a = Clamp01(anchorWeight[i] * params.anchorStiffness);
                if (a <= 0.0f)
                    continue;
                finalVerts[i] = Add(finalVerts[i], Scale(Sub(supportVerts[i], finalVerts[i]), a));
                ++stats.anchorAdjusted;
            }
        }

        // Bending constraints: resist folding along shared edges to create smooth cloth surface.
        if (params.bendingStiffness > 0.0f && params.bendingIterations > 0 && bendingPtr) {
            const auto& bendingConstraints = *bendingPtr;
            const float bendingK = Clamp01(params.bendingStiffness);
            const float bendingStep = 1.0f - std::pow(1.0f - bendingK, 1.0f / (float)(params.bendingIterations));

            for (int bIter = 0; bIter < params.bendingIterations; ++bIter) {
                for (size_t bi = 0; bi < bendingConstraints.size(); ++bi) {
                    const auto& bc = bendingConstraints[bi];
                    if (bc.c >= n || bc.d >= n)
                        continue;

                    float wc = bc.c < invMass.size() ? Clamp01(invMass[bc.c]) : 1.0f;
                    float wd = bc.d < invMass.size() ? Clamp01(invMass[bc.d]) : 1.0f;
                    float wSum = wc + wd;
                    if (wSum <= kLenEps)
                        continue;

                    Vec3 dir = Sub(finalVerts[bc.c], finalVerts[bc.d]);
                    float len = Len(dir);
                    if (len <= kLenEps)
                        continue;

                    // Conformed rest distance (fits morphed body), not slim base.
                    float bRest = (bi < supportBendRestLen.size())
                        ? supportBendRestLen[bi] : bc.restLength;
                    Vec3 delta = Scale(dir, ((len - bRest) / len) * bendingStep / wSum);
                    if (wc > 0.0f)
                        finalVerts[bc.c] = Sub(finalVerts[bc.c], Scale(delta, wc));
                    if (wd > 0.0f)
                        finalVerts[bc.d] = Add(finalVerts[bc.d], Scale(delta, wd));

                    ++stats.bendingAdjusted;
                }
            }
        }
    }

    if (params.surfaceSmoothIterations > 0 &&
        params.surfaceSmoothStrength > 0.0f &&
        !useEdges.empty()) {
        for (int iter = 0; iter < params.surfaceSmoothIterations; ++iter) {
            std::vector<Vec3> sum(n, Vec3{});
            std::vector<int> count(n, 0);
            for (const auto& e : useEdges) {
                if (e.a >= n || e.b >= n)
                    continue;
                sum[e.a] = Add(sum[e.a], finalVerts[e.b]);
                sum[e.b] = Add(sum[e.b], finalVerts[e.a]);
                ++count[e.a];
                ++count[e.b];
            }

            for (size_t i = 0; i < n; ++i) {
                if (count[i] <= 0)
                    continue;
                Vec3 avg = Scale(sum[i], 1.0f / (float)count[i]);
                float s = params.surfaceSmoothStrength * invMass[i] *
                    ClampRange(1.0f - anchorWeight[i] * 0.75f, 0.0f, 1.0f);
                if (s <= 0.0f)
                    continue;
                finalVerts[i] = Add(finalVerts[i], Scale(Sub(avg, finalVerts[i]), s));
                if (collideVertex(i, params.collisionStiffness))
                    ++stats.collisionAdjusted;
                ++stats.smoothAdjusted;
            }
        }
    }

    // Post-PBD shape-level passes.
    // drapeFollow: override follow=1.0 for non-drape (upper body / sleeve) verts
    // so envelope and body-column passes leave them untouched (loose=0 â†’ strengthâ‰ˆ0).
    std::vector<float> drapeFollow(follow);
    for (size_t i = 0; i < n; ++i)
        if (drapeEl[i] < 0.20f) drapeFollow[i] = 1.0f;

    // Cloth drape envelope: prevent cloth from returning to body faster than
    // drapeReturnRate per unit Z after a protrusion (belly, hips, etc.).
    // In Verlet mode this runs LATER (after smoothing) as the dominant pass, so
    // the vertical drops survive; here it only runs for the legacy correction path.
    if (params.drapeReturnRate > 0.0f && !params.verletCloth) {
        ApplyClothDrapeEnvelopePass(finalVerts, drapeEl, effectiveInfluence,
                                    params.drapeEnvelopeBins,
                                    params.drapeReturnRate,
                                    params.drapeEnvelopeStrength, stats);
    }

    ApplyLayerOrderPass(finalVerts, baseVerts, useEdges, invMass, params, stats);
    // Skip the symmetric edge-relax in Verlet mode: it would pull edges back to
    // rest in both directions and flatten the folds/hang the Verlet solve created.
    if (!params.verletCloth)
        ApplyEdgeConstraintPass(finalVerts, useEdges, invMass, params, stats);

    // Belt finishing (seat + de-wrinkle). DEFINED here but INVOKED after the
    // influence-blend / idempotency / safety-clamp below: those run toward the
    // vanilla conform (which floats the belt ~2u off the body), so seating before
    // them lets the blend pull the belt back out. Invoking last makes the seat
    // authoritative.
    auto applyBeltFinishing = [&]() {
    // A belt mesh is usually the wide wrap band PLUS separate hanging appendages
    // (dangling straps, buckle/clasp) as their own connected components. The wrap
    // should seat onto the body; the appendages must hang FREELY (seating them to a
    // uniform body-gap twists them and can clip). So restrict all belt shaping to the
    // LARGEST connected component (the wrap) and leave the appendages as solved.
    std::vector<uint8_t> isMainBand;
    std::vector<int> compRoot;   // per-vertex connected-component id (or -1)
    if ((params.beltSeatIterations > 0 || params.beltDeWrinkleIterations > 0) &&
        tris && !tris->empty() && n > 0 && n <= 0xFFFF) {
        std::vector<int> parent(n);
        for (size_t i = 0; i < n; ++i) parent[i] = (int)i;
        auto findp = [&](int x) -> int {
            while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
            return x;
        };
        auto unite = [&](int a, int b) { if (a<(int)n && b<(int)n) parent[findp(a)] = findp(b); };
        for (const auto& t : *tris) {
            if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
            unite(t[0], t[1]); unite(t[1], t[2]);
        }
        std::unordered_map<int, int> compSize;
        for (size_t i = 0; i < n; ++i) ++compSize[findp((int)i)];
        int bestRoot = -1, bestSize = -1;
        for (const auto& kv : compSize)
            if (kv.second > bestSize) { bestSize = kv.second; bestRoot = kv.first; }
        isMainBand.assign(n, 0);
        compRoot.assign(n, -1);
        for (size_t i = 0; i < n; ++i) {
            int r = findp((int)i);
            compRoot[i] = r;
            if (r == bestRoot) isMainBand[i] = 1;
        }
    }
    auto inMainBand = [&](size_t i) -> bool {
        return isMainBand.empty() || (i < isMainBand.size() && isMainBand[i]);
    };

    // Predominantly-VERTICAL appendage (hanging strap) drape. A strap is a small,
    // separate island whose z-extent dwarfs its horizontal extent. vanilla Conform
    // hangs it roughly straight but floats it ~3u off the body. Physically it should
    // CONTACT the body only where the body pushes it out (its TOP, at the hip) and
    // hang straight from there - standing off wherever the leg narrows below. So we
    // translate the whole rigid island in by the TOP's float only (NOT the median):
    // the top contacts, the bottom keeps that same offset and naturally stands off.
    // No inward "magnetism" that drags the bottom onto the narrower thigh.
    if (!compRoot.empty() && bodyBVH && bodyBVH->valid) {
        std::unordered_map<int, std::vector<uint32_t>> comps;
        for (size_t i = 0; i < n; ++i)
            if (isMainBand.empty() || !isMainBand[i]) comps[compRoot[i]].push_back((uint32_t)i);
        const float kStrapTargetGap = 0.4f;
        for (auto& kv : comps) {
            auto& verts = kv.second;
            if (verts.size() < 6) continue;               // skip tiny buckle bits
            float xmin=1e30f,xmax=-1e30f,ymin=1e30f,ymax=-1e30f,zmin=1e30f,zmax=-1e30f;
            for (uint32_t i : verts) {
                const Vec3& p = finalVerts[i];
                xmin=std::min(xmin,p.x); xmax=std::max(xmax,p.x);
                ymin=std::min(ymin,p.y); ymax=std::max(ymax,p.y);
                zmin=std::min(zmin,p.z); zmax=std::max(zmax,p.z);
            }
            float dz = zmax - zmin;
            float horiz = std::sqrt((xmax-xmin)*(xmax-xmin) + (ymax-ymin)*(ymax-ymin));
            if (dz < 1.5f * std::max(horiz, 0.01f)) continue;   // not predominantly vertical
            // Inward direction + gap from the TOP 30% of the island only (the contact zone).
            float topZ = zmax - 0.30f * dz;
            Vec3 inward{0,0,0}; std::vector<float> topGaps;
            for (uint32_t i : verts) {
                if (finalVerts[i].z < topZ) continue;
                Vec3 cp{0,0,0}, nrm{0,0,1}; float d2=0.0f;
                if (!BVHClosestPoint(*bodyBVH, finalVerts[i], cp, nrm, d2)) continue;
                float dist = std::sqrt(d2);
                topGaps.push_back(Dot(Sub(finalVerts[i], cp), nrm) < 0.0f ? -dist : dist);
                inward = Sub(inward, nrm);   // -normal = toward body
            }
            if (topGaps.empty()) continue;
            std::sort(topGaps.begin(), topGaps.end());
            float topGap = topGaps[topGaps.size()/2];
            float L = Len(inward);
            if (L < 1.0e-4f) continue;
            inward = Scale(inward, 1.0f / L);
            float move = topGap - kStrapTargetGap;        // bring the TOP to contact only
            if (move <= 0.05f) continue;
            Vec3 t = Scale(inward, move);
            for (uint32_t i : verts) finalVerts[i] = Add(finalVerts[i], t);
            // collision push-out only (never pull in): the bottom stands off freely.
            for (uint32_t i : verts) {
                Vec3 cp{0,0,0}, nrm{0,0,1}; float d2=0.0f;
                if (!BVHClosestPoint(*bodyBVH, finalVerts[i], cp, nrm, d2)) continue;
                float dist = std::sqrt(d2);
                float gap = Dot(Sub(finalVerts[i], cp), nrm) < 0.0f ? -dist : dist;
                if (gap < 0.1f) finalVerts[i] = Add(cp, Scale(nrm, 0.1f));
            }
            HLog("belt strap drape: island n=%zu dz=%.1f topGap=%.2f movedTopToContact=%.2f",
                 verts.size(), dz, topGap, move);
        }
    }

    // Late "seat" pass (belt). vanilla Conform floats the wrap ~2u off the body, so it
    // reads as an inflated hula hoop hovering above the dress. Pull each wrap vert IN
    // along the body normal toward beltSeatTargetGap (chosen to land just above where
    // the dress conforms), clamped so it never crosses the target -> no body
    // penetration, and it follows the body contour instead of staying a rigid ring.
    if (params.beltSeatIterations > 0 && params.beltSeatStrength > 0.0f &&
        bodyBVH && bodyBVH->valid && n > 0 && !isMainBand.empty()) {
        auto isRigidSeat = [&](size_t i) -> bool {
            if (islandIdOut.empty() || i >= islandIdOut.size()) return false;
            int id = islandIdOut[i];
            return id >= 0 && id < (int)islandRigidOut.size() && islandRigidOut[(size_t)id];
        };
        // Split the wrap into an ANCHORED top cord and a DRAPING flap by height.
        float zmin = 1e30f, zmax = -1e30f;
        for (size_t i = 0; i < n; ++i)
            if (isMainBand[i]) { zmin = std::min(zmin, finalVerts[i].z);
                                 zmax = std::max(zmax, finalVerts[i].z); }
        float zspan = std::max(zmax - zmin, 1.0e-3f);
        float anchorZ = zmax - params.beltAnchorFrac * zspan;
        std::vector<uint8_t> isAnchor(n, 0);
        for (size_t i = 0; i < n; ++i)
            if (isMainBand[i] && !isRigidSeat(i) && finalVerts[i].z >= anchorZ)
                isAnchor[i] = 1;

        // (1) Seat the anchor cord onto the body (cinch): snap to target gap.
        const float kTarget = params.beltSeatTargetGap;
        const float kStrength = params.beltSeatStrength;
        int seated = 0;
        for (int iter = 0; iter < params.beltSeatIterations; ++iter) {
            for (size_t i = 0; i < n; ++i) {
                if (!isAnchor[i]) continue;
                Vec3 cp{0,0,0}, nrm{0,0,1}; float d2 = 0.0f;
                if (!BVHClosestPoint(*bodyBVH, finalVerts[i], cp, nrm, d2)) continue;
                float dist = std::sqrt(d2);
                float gap = Dot(Sub(finalVerts[i], cp), nrm) < 0.0f ? -dist : dist;
                if (gap <= kTarget + 0.01f) continue;
                Vec3 seatPos = Add(cp, Scale(nrm, kTarget));
                finalVerts[i] = Add(Scale(finalVerts[i], 1.0f - kStrength),
                                    Scale(seatPos, kStrength));
                ++seated;
            }
        }

        // (2) Drape the flap: hang from the anchored cord under gravity, holding the
        //     wrap's CURRENT edge lengths (leather keeps its shape) with one-way body
        //     collision (push OUT only) so it drapes instead of hugging the contour.
        int draped = 0;
        if (params.beltDrapeIterations > 0 && tris && !tris->empty()) {
            struct MBEdge { uint32_t a, b; float rest; };
            std::vector<MBEdge> mbEdges;
            mbEdges.reserve(tris->size() * 2);
            std::unordered_set<uint64_t> seen;
            auto addE = [&](uint32_t a, uint32_t b) {
                if (!isMainBand[a] || !isMainBand[b]) return;
                uint64_t k = a < b ? ((uint64_t)a << 32) | b : ((uint64_t)b << 32) | a;
                if (!seen.insert(k).second) return;
                mbEdges.push_back({a, b, Len(Sub(finalVerts[a], finalVerts[b]))});
            };
            for (const auto& t : *tris) {
                if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
                addE(t[0], t[1]); addE(t[1], t[2]); addE(t[2], t[0]);
            }
            const float kGrav = params.beltDrapeGravity;
            const float kFloor = params.beltDrapeFloor;
            const float kMaxGap = params.beltDrapeMaxGap;
            for (int iter = 0; iter < params.beltDrapeIterations; ++iter) {
                // gravity on the non-anchored flap verts
                for (size_t i = 0; i < n; ++i)
                    if (isMainBand[i] && !isAnchor[i] && !isRigidSeat(i))
                        finalVerts[i].z -= kGrav;
                // shape-preserving edge constraints (anchored ends immovable)
                for (int p = 0; p < params.beltDrapeEdgePasses; ++p) {
                    for (const MBEdge& e : mbEdges) {
                        Vec3 d = Sub(finalVerts[e.a], finalVerts[e.b]);
                        float L = Len(d);
                        if (L < 1.0e-5f) continue;
                        float wa = isAnchor[e.a] ? 0.0f : 1.0f;
                        float wb = isAnchor[e.b] ? 0.0f : 1.0f;
                        if (wa + wb <= 0.0f) continue;
                        Vec3 corr = Scale(d, (L - e.rest) / L);
                        finalVerts[e.a] = Sub(finalVerts[e.a], Scale(corr, wa / (wa + wb)));
                        finalVerts[e.b] = Add(finalVerts[e.b], Scale(corr, wb / (wa + wb)));
                    }
                }
                // one-way body collision: push out only (never pull in -> keeps drape)
                for (size_t i = 0; i < n; ++i) {
                    if (!isMainBand[i] || isAnchor[i] || isRigidSeat(i)) continue;
                    Vec3 cp{0,0,0}, nrm{0,0,1}; float d2 = 0.0f;
                    if (!BVHClosestPoint(*bodyBVH, finalVerts[i], cp, nrm, d2)) continue;
                    float dist = std::sqrt(d2);
                    float gap = Dot(Sub(finalVerts[i], cp), nrm) < 0.0f ? -dist : dist;
                    if (gap < kFloor) {
                        finalVerts[i] = Add(cp, Scale(nrm, kFloor));  // no penetration
                        ++draped;
                    } else if (gap > kMaxGap) {
                        // Flank/corner verts hang too far out (reads as hover). Pull them
                        // back toward the max-drape gap so the flap hangs close-ish while
                        // still draping (not shrink-wrapping).
                        finalVerts[i] = Add(cp, Scale(nrm, kMaxGap));
                        ++draped;
                    }
                }
            }
        }
        // (3) ANTI-MAGNETISM (convexity restore). The seat/drape can leave the belt
        //     following the body INTO its concavities (sides, under-hip, between the
        //     legs) - i.e. pulled toward the center where nothing pushes it. Real cloth
        //     BRIDGES hollows: it only contacts the convex high points. Here, any vert
        //     that sits more toward the body than its ring-neighbors is filling a hollow
        //     - push it back OUT to the neighbor level (along the body normal only, and
        //     ONLY ever outward, never inward). Iterating bridges the hollows while the
        //     convex contacts (bumps) are untouched.
        if (tris && !tris->empty()) {
            std::vector<std::vector<uint32_t>> ring(n);
            for (const auto& t : *tris) {
                if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
                for (int k = 0; k < 3; ++k) {
                    uint32_t a = t[k], b = t[(k+1)%3];
                    if (isMainBand[a] && isMainBand[b]) { ring[a].push_back(b); ring[b].push_back(a); }
                }
            }
            for (size_t i = 0; i < n; ++i) {
                auto& r = ring[i];
                std::sort(r.begin(), r.end());
                r.erase(std::unique(r.begin(), r.end()), r.end());
            }
            int bridged = 0;
            const float kBridgeStrength = 0.6f;
            for (int iter = 0; iter < 12; ++iter) {
                std::vector<Vec3> next = finalVerts;
                for (size_t i = 0; i < n; ++i) {
                    if (!isMainBand[i] || isRigidSeat(i)) continue;
                    const auto& r = ring[i];
                    if (r.size() < 2) continue;
                    Vec3 c{0,0,0};
                    for (uint32_t j : r) c = Add(c, finalVerts[j]);
                    c = Scale(c, 1.0f / (float)r.size());
                    Vec3 nrm{0,0,1}; float g = 0.0f;
                    if (!BVHSignedGap(*bodyBVH, finalVerts[i], g, &nrm)) continue;
                    float inwardness = Dot(Sub(c, finalVerts[i]), nrm);  // >0 => vert dips toward body
                    if (inwardness > 0.02f) {
                        finalVerts[i] = Add(finalVerts[i], Scale(nrm, inwardness * kBridgeStrength));
                        ++bridged;
                    }
                }
                (void)next;
            }
            HLog("belt anti-magnetism: bridged %d concave-dip moves", bridged);
        }

        HLog("belt drape: anchorZ=%.1f seated=%d drapedCollisions=%d edges", anchorZ, seated, draped);
        {   // DIAG: flap gap by flank (solver frame) to locate residual hover
            float cxb = 0.0f; int cn = 0;
            for (size_t i = 0; i < n; ++i) if (isMainBand[i]) { cxb += finalVerts[i].x; ++cn; }
            if (cn) cxb /= cn;
            std::vector<float> gl, gr;
            for (size_t i = 0; i < n; ++i) {
                if (!isMainBand[i] || isAnchor[i]) continue;
                float g = 0.0f; Vec3 nn{0,0,1};
                if (!BVHSignedGap(*bodyBVH, finalVerts[i], g, &nn)) continue;
                (finalVerts[i].x >= cxb ? gr : gl).push_back(g);
            }
            std::sort(gl.begin(), gl.end()); std::sort(gr.begin(), gr.end());
            auto med = [](std::vector<float>& v){ return v.empty()?0.0f:v[v.size()/2]; };
            auto mx  = [](std::vector<float>& v){ return v.empty()?0.0f:v.back(); };
            HLog("belt FLAP gap by flank: x>=cx median=%.2f max=%.2f n=%d | x<cx median=%.2f max=%.2f n=%d",
                 med(gr), mx(gr), (int)gr.size(), med(gl), mx(gl), (int)gl.size());
            // Verticality: mean angle-from-straight-down of flap edges that span z levels
            // (0deg = hanging vertical/90deg fold, 90deg = lying flat along the body).
            double angSum = 0.0; int angN = 0;
            if (tris) for (const auto& t : *tris) {
                for (int k = 0; k < 3; ++k) {
                    uint32_t a = t[k], b = t[(k + 1) % 3];
                    if (a >= n || b >= n) continue;
                    if (!isMainBand[a] || !isMainBand[b]) continue;
                    if (isAnchor[a] && isAnchor[b]) continue;
                    Vec3 d = Sub(finalVerts[a], finalVerts[b]);
                    float L = Len(d); if (L < 1.0e-4f) continue;
                    float vert = std::fabs(d.z) / L;      // 1 = vertical, 0 = horizontal
                    if (vert < 0.30f) continue;            // skip near-horizontal ring edges
                    angSum += std::acos(std::min(1.0f, vert)) * 57.2958f; ++angN;
                }
            }
            HLog("belt FLAP verticality: mean edge angle-from-vertical = %.1f deg (n=%d; 0=straight down)",
                 angN ? angSum / angN : 0.0, angN);
        }
    }

    // Late tangential de-wrinkle (belt). Runs AFTER the deforming passes above so
    // the flattening survives (an early surface smooth gets re-bunched by the
    // envelope/layer/edge passes). Slides each INTERIOR vert along the body tangent
    // toward its 1-ring centroid: tangential => body gap preserved (no new
    // penetration, no lift-off); boundary verts pinned (silhouette + hanging straps
    // keep their shape); each step capped to a fraction of the local edge so edges
    // can't tear.
    if (params.beltDeWrinkleIterations > 0 && params.beltDeWrinkleStrength > 0.0f &&
        tris && !tris->empty() && n > 0 && n <= 0xFFFF) {
        std::vector<std::vector<uint32_t>> ring(n);
        std::unordered_map<uint64_t, int> edgeTriCount;
        edgeTriCount.reserve(tris->size() * 3);
        auto ekey = [](uint32_t a, uint32_t b) -> uint64_t {
            if (a > b) std::swap(a, b);
            return ((uint64_t)a << 32) | (uint64_t)b;
        };
        for (const auto& t : *tris) {
            uint32_t a = t[0], b = t[1], c = t[2];
            if (a >= n || b >= n || c >= n) continue;
            ++edgeTriCount[ekey(a, b)];
            ++edgeTriCount[ekey(b, c)];
            ++edgeTriCount[ekey(c, a)];
            ring[a].push_back(b); ring[a].push_back(c);
            ring[b].push_back(a); ring[b].push_back(c);
            ring[c].push_back(a); ring[c].push_back(b);
        }
        std::vector<uint8_t> isBoundary(n, 0);
        for (const auto& kv : edgeTriCount) {
            if (kv.second == 1) {
                isBoundary[(size_t)(kv.first >> 32)] = 1;
                isBoundary[(size_t)(kv.first & 0xFFFFFFFFu)] = 1;
            }
        }
        for (size_t i = 0; i < n; ++i) {
            auto& r = ring[i];
            std::sort(r.begin(), r.end());
            r.erase(std::unique(r.begin(), r.end()), r.end());
        }
        auto isRigidVert = [&](size_t i) -> bool {
            if (islandIdOut.empty() || i >= islandIdOut.size()) return false;
            int id = islandIdOut[i];
            return id >= 0 && id < (int)islandRigidOut.size() && islandRigidOut[(size_t)id];
        };
        const float kStrength = params.beltDeWrinkleStrength;
        const float kEdgeCapFrac = 0.30f;  // max step/iter as fraction of mean edge
        const float kGapFloor = 0.02f;     // keep at least this body gap after a move
        int moved = 0;
        for (int iter = 0; iter < params.beltDeWrinkleIterations; ++iter) {
            std::vector<Vec3> next = finalVerts;  // Jacobi: read old, write new
            for (size_t i = 0; i < n; ++i) {
                if (!inMainBand(i)) continue;  // hanging straps/buckle stay free
                if (isBoundary[i] || isRigidVert(i)) continue;
                if (i < invMass.size() && invMass[i] <= 0.0f) continue;
                const auto& r = ring[i];
                if (r.size() < 2) continue;
                Vec3 centroid{0.0f, 0.0f, 0.0f};
                float meanEdge = 0.0f;
                for (uint32_t j : r) {
                    centroid = Add(centroid, finalVerts[j]);
                    meanEdge += Len(Sub(finalVerts[j], finalVerts[i]));
                }
                float invR = 1.0f / (float)r.size();
                centroid = Scale(centroid, invR);
                meanEdge *= invR;
                Vec3 delta = Sub(centroid, finalVerts[i]);
                Vec3 nrm{0.0f, 0.0f, 1.0f};
                float g0 = 0.0f;
                bool haveGap = bodyBVH && bodyBVH->valid &&
                               BVHSignedGap(*bodyBVH, finalVerts[i], g0, &nrm);
                if (haveGap)
                    delta = Sub(delta, Scale(nrm, Dot(delta, nrm)));  // tangential
                Vec3 step = Scale(delta, kStrength);
                float stepLen = Len(step);
                float cap = kEdgeCapFrac * meanEdge;
                if (stepLen > cap && stepLen > 1.0e-6f)
                    step = Scale(step, cap / stepLen);
                Vec3 cand = Add(finalVerts[i], step);
                if (haveGap) {
                    float g1 = 0.0f; Vec3 n1{0.0f, 0.0f, 1.0f};
                    if (BVHSignedGap(*bodyBVH, cand, g1, &n1)) {
                        if (g1 < kGapFloor)
                            cand = Add(cand, Scale(n1, kGapFloor - g1));  // no penetration
                        // NOTE: no re-seat-to-target here. The flap is meant to DRAPE
                        // (hang at gap > target); clamping it back to target would undo
                        // the drape. De-wrinkle only smooths tangentially + stops
                        // penetration; positioning is owned by the drape/seat above.
                    }
                }
                next[i] = cand;
                ++moved;
            }
            finalVerts = std::move(next);
        }
        HLog("belt de-wrinkle: iters=%d strength=%.2f moved=%d",
             params.beltDeWrinkleIterations, kStrength, moved);
    }
    };  // applyBeltFinishing (invoked after the clamp passes below)

    // Slider-source influence blend (CORRECTION MODE ONLY).
    // In physicsDrape mode, physics determines cloth position â€” no blend-back.
    // Blending back to supportVerts (vanilla conform) would kill the drape effect.
    if (!params.physicsDrape) {
        for (size_t i = 0; i < n; ++i) {
            float eff = (i < effectiveInfluence.size()) ? effectiveInfluence[i] : 1.0f;
            if (eff < 0.999f) {
                finalVerts[i] = Add(Scale(supportVerts[i], 1.0f - eff),
                                    Scale(finalVerts[i], eff));
            }
        }
    }

    // Idempotency: verts with near-zero vanilla conform diff stay at base.
    {
        static constexpr float kRawThreshold2 = 0.001f;
        for (size_t i = 0; i < n; ++i) {
            Vec3 rawV = DiffAt(rawDiff, static_cast<uint16_t>(i));
            if (Len2(rawV) < kRawThreshold2)
                finalVerts[i] = baseVerts[i];
        }
    }

    // Safety clamp (correction mode only; disabled when maxDisplacementMult=0).
    // In physicsDrape mode, cloth can deviate freely from rawDiff.
    if (params.maxDisplacementMult > 0.0f) {
        const float multSq = params.maxDisplacementMult * params.maxDisplacementMult;
        for (size_t i = 0; i < n; ++i) {
            Vec3 rawV = DiffAt(rawDiff, static_cast<uint16_t>(i));
            float rawM2 = Len2(rawV);
            if (rawM2 < 1.0e-6f) continue;
            Vec3 finalDiffV = Sub(finalVerts[i], baseVerts[i]);
            float finalM2 = Len2(finalDiffV);
            if (finalM2 > rawM2 * multSq) {
                float scale = std::sqrt(rawM2 * multSq / finalM2);
                finalVerts[i] = Add(baseVerts[i], Scale(finalDiffV, scale));
            }
        }
    }

    // Belt seat + de-wrinkle: run LAST so the influence-blend / safety-clamp above
    // can't pull the seated belt back toward the vanilla float. No-op for non-belt
    // presets (beltSeatIterations / beltDeWrinkleIterations default to 0).
    applyBeltFinishing();

    // â”€â”€ WRITE-PATH DIAGNOSTIC â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // Overrides the physics result with a brutal, obvious deformation so we can
    // visually confirm whether our SetResultDiff write actually reaches the mesh.
    if (kDiagWritePathTest) {
        int moved = 0;
        for (size_t i = 0; i < n; ++i) {
            // Shove drape-eligible verts straight down. Keep anchored/upper verts
            // at vanilla conform so the boundary stretch is obvious.
            if (drapeEl[i] > 0.25f) {
                finalVerts[i] = supportVerts[i];
                finalVerts[i].z -= kDiagDropZ;
                ++moved;
            } else {
                finalVerts[i] = supportVerts[i];
            }
        }
        HLog("detour: DIAG_WRITE_TEST shape='%s' slider='%s' dropped=%d verts by %.1f",
             shapeName.c_str(), sliderName.c_str(), moved, kDiagDropZ);
    }

    // â”€â”€ Finishing (verletCloth) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // Primary collider is now the 3D BVH: smooth, then push every drape vert onto
    // the REAL body surface (closest point + normal). This replaces the radial
    // shell envelope as the shaper â€” the result follows the actual belly/hip/bust
    // geometry instead of a central-axis approximation. The loose "don't tuck in"
    // rule will be layered back on top of this as an auxiliary constraint.
    if (params.verletCloth && bodyBVH && bodyBVH->valid) {
        // Finishing rounds scale with intensity: Low=5, Medium=9, High=14.
        static const int kFinishRounds[] = {5, 9, 14};
        int intensity = g_processingIntensity.load(std::memory_order_relaxed);
        int finishRounds = kFinishRounds[intensity > 2 ? 1 : intensity];
        for (int it = 0; it < finishRounds; ++it) {
            if (!useEdges.empty()) {
                std::vector<Vec3> sum(n, Vec3{});
                std::vector<int> cnt(n, 0);
                for (const auto& e : useEdges) {
                    if (e.a >= n || e.b >= n) continue;
                    sum[e.a] = Add(sum[e.a], finalVerts[e.b]); ++cnt[e.a];
                    sum[e.b] = Add(sum[e.b], finalVerts[e.a]); ++cnt[e.b];
                }
                for (size_t i = 0; i < n; ++i) {
                    if (cnt[i] <= 0 || drapeEl[i] < 0.20f) continue;
                    Vec3 avg = Scale(sum[i], 1.0f / (float)cnt[i]);
                    finalVerts[i] = Add(finalVerts[i], Scale(Sub(avg, finalVerts[i]), 0.38f));
                }
            }
            // 3D collision push-out (real surface). Runs LAST so it is the final state.
            for (size_t i = 0; i < n; ++i)
                if (drapeEl[i] >= 0.20f && collideVertex(i, 1.0f))
                    ++stats.collisionAdjusted;
        }

        // â”€â”€ Loose anti-tuck rule (BVH-based) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // Going DOWN the garment, dist-to-body can only INCREASE (new contact)
        // or hold steady (vertical fall), never decrease abruptly (tuck-in).
        // Per angular bin: running max of dist-to-body; verts below the floor are
        // pushed back out along the direction (vertâ†’body) inverted â€” NOT along the
        // body face normal, which over-extrapolates at large distances.
        if (params.drapeReturnRate >= 0.0f) {
            struct DVert { size_t i; float z, dist; Vec3 cp, awayDir; };
            const int AB = 48;
            std::vector<DVert> dv2;
            dv2.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                if (drapeEl[i] < 0.20f) continue;
                Vec3 cp{0,0,0}; Vec3 nrm{0,0,1}; float d2 = 0.0f;
                if (!BVHClosestPoint(*bodyBVH, finalVerts[i], cp, nrm, d2)) continue;
                float dist = std::sqrt(d2);
                Vec3 toVert = Sub(finalVerts[i], cp);
                float tl = Len(toVert);
                // Direction FROM body surface TOWARD vert â€” stable at any distance,
                // unlike the face normal which diverges when extrapolated far away.
                Vec3 awayDir = (tl > 1.0e-5f) ? Scale(toVert, 1.0f / tl) : nrm;
                dv2.push_back({i, finalVerts[i].z, dist, cp, awayDir});
            }
            std::sort(dv2.begin(), dv2.end(),
                      [](const DVert& a, const DVert& b) { return a.z > b.z; });
            std::vector<float> floorDist(AB, -1.0f);
            std::vector<float> lastZ(AB, 1e30f);
            for (auto& dv : dv2) {
                float dirX = 0.0f, dirY = 0.0f, r = 0.0f;
                if (!RadialDir(finalVerts[dv.i], dirX, dirY, r)) continue;
                int ab = RadialBin(dirX, dirY, AB);
                float dz = std::max(0.0f, lastZ[ab] - dv.z);
                float floor = (floorDist[ab] < 0.0f) ? dv.dist
                              : std::max(0.0f, floorDist[ab] - params.drapeReturnRate * dz);
                if (dv.dist < floor - 0.01f) {
                    finalVerts[dv.i] = Add(dv.cp, Scale(dv.awayDir, floor));
                    ++stats.envelopeAdjusted;
                }
                floorDist[ab] = std::max(floor, dv.dist);
                lastZ[ab] = dv.z;
            }
        }
    } else if (params.verletCloth && bodyShell && bodyShell->valid) {
        // Fallback: radial-shell two-sided envelope (no BVH available).
        for (int it = 0; it < 6; ++it) {
            if (!useEdges.empty()) {
                std::vector<Vec3> sum(n, Vec3{});
                std::vector<int> cnt(n, 0);
                for (const auto& e : useEdges) {
                    if (e.a >= n || e.b >= n) continue;
                    sum[e.a] = Add(sum[e.a], finalVerts[e.b]); ++cnt[e.a];
                    sum[e.b] = Add(sum[e.b], finalVerts[e.a]); ++cnt[e.b];
                }
                for (size_t i = 0; i < n; ++i) {
                    if (cnt[i] <= 0 || drapeEl[i] < 0.20f) continue;
                    Vec3 avg = Scale(sum[i], 1.0f / (float)cnt[i]);
                    finalVerts[i] = Add(finalVerts[i], Scale(Sub(avg, finalVerts[i]), 0.30f));
                }
            }
            ApplyBodyEnvelopeDrapePass(finalVerts, drapeEl, *bodyShell,
                                       params.bodyClearance, 1.0f,
                                       params.drapeReturnRate, /*twoSided=*/true, stats);
        }
    }

    // â”€â”€ Post-finishing stretch cap (THE TEAR FIX) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // The finishing passes above (BVH push-out, loose anti-tuck) move verts to
    // absolute distances from the body WITHOUT any edge-length limit. On a slim
    // garment over a heavy body this blew edges up to 15Ã— rest (measured: cloak went
    // from 1.7Ã— right after the verlet cap to 15Ã— by here). The verlet cap in the
    // solve loop can't help â€” these passes run AFTER it. Re-apply the SAME hard cap
    // here so no edge can leave this function over-stretched. Symmetric 50/50 split so
    // it works even between anchored verts (which have invMass 0). Global fix: any
    // garment, any preset that uses the finishing path.
    if (params.verletCloth && !useEdges.empty()) {
        const float kPostCap = 1.12f;     // tight cap, close to the solve cap (1.10)
        // Many iterations: a 10Ã—+ stretched cluster needs the correction to PROPAGATE
        // across the mesh (Gauss-Seidel), and 4 passes left the cloak at 11Ã— (28 torn
        // edges). 24 passes converge it down to the cap.
        for (int it = 0; it < 24; ++it) {
            for (size_t ei = 0; ei < useEdges.size(); ++ei) {
                const auto& e = useEdges[ei];
                if (e.a >= n || e.b >= n) continue;
                float rest = (ei < supportRestLen.size()) ? supportRestLen[ei] : e.restLength;
                if (rest <= 1e-6f) continue;
                Vec3 dir = Sub(finalVerts[e.a], finalVerts[e.b]);
                float len = Len(dir);
                float cap = rest * kPostCap;
                if (len <= cap || len <= 1e-6f) continue;
                // Don't move a PINNED vert if its partner is free (let the free one take
                // it); only split 50/50 when both are pinned (else the edge escapes).
                float wa = (e.a < invMass.size() && invMass[e.a] > 0.0f) ? 1.0f : 0.0f;
                float wb = (e.b < invMass.size() && invMass[e.b] > 0.0f) ? 1.0f : 0.0f;
                if (wa + wb <= 0.0f) { wa = 0.5f; wb = 0.5f; }
                float wSum = wa + wb;
                Vec3 delta = Scale(dir, ((len - cap) / len) / wSum);
                finalVerts[e.a] = Sub(finalVerts[e.a], Scale(delta, wa));
                finalVerts[e.b] = Add(finalVerts[e.b], Scale(delta, wb));
            }
        }
    }

    // Final seam weld (verletCloth): the post-PBD passes (surface smoothing,
    // drape envelope, layer order) move coincident verts independently and can
    // re-open seams the solver welded. Close them once more before writing.
    if (params.verletCloth) {
        std::unordered_map<uint64_t, std::vector<uint32_t>> coin;
        coin.reserve(n);
        auto qk = [](const Vec3& p) -> uint64_t {
            auto q = [](float v) -> uint64_t {
                return (uint64_t)(uint32_t)(int32_t)std::llround(v * 100.0f);
            };
            return (q(p.x) * 73856093ull) ^ (q(p.y) * 19349663ull) ^ (q(p.z) * 83492791ull);
        };
        for (size_t i = 0; i < n; ++i)
            coin[qk(baseVerts[i])].push_back((uint32_t)i);
        std::vector<uint64_t> coinKeys;
        coinKeys.reserve(coin.size());
        for (const auto& kv : coin)
            coinKeys.push_back(kv.first);
        std::sort(coinKeys.begin(), coinKeys.end());
        for (uint64_t key : coinKeys) {
            auto& g = coin[key];
            if (g.size() < 2) continue;
            int pinned = -1;
            for (uint32_t vi : g)
                if (invMass[vi] <= 0.0f || drapeEl[vi] < 0.20f) { pinned = (int)vi; break; }
            if (pinned >= 0) {
                Vec3 p = supportVerts[pinned];
                for (uint32_t vi : g) finalVerts[vi] = p;
            } else {
                Vec3 a{0.0f, 0.0f, 0.0f};
                for (uint32_t vi : g) a = Add(a, finalVerts[vi]);
                a = Scale(a, 1.0f / (float)g.size());
                for (uint32_t vi : g) finalVerts[vi] = a;
            }
        }
    }

    // â”€â”€ Final anti-clip pass (all verts, verletCloth + BVH) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // Anchored verts (drapeEl < 0.20) return to supportVerts (vanilla conform),
    // which can be inside the morphed body â€” they're never touched by the drape
    // collision. A small-clearance BVH push on every vert catches these without
    // disturbing verts that are already correctly positioned outside the body.
    // Clearance is intentionally smaller (0.15) than bodyClearance so it only
    // fixes real clips without pushing fitted/belted pieces off the body visibly.
    if (params.verletCloth && bodyBVH && bodyBVH->valid) {
        float clearFactor = 0.5f;
        for (size_t i = 0; i < n; ++i) {
            Vec3 target = finalVerts[i];
            float ac = std::max(0.10f, vertClearance[i] * clearFactor);
            if (BVHCollideVertex(*bodyBVH, target, ac))
                finalVerts[i] = target;
        }
    }

    // TEAR DIAGNOSTIC: measure worst edge over-stretch (current / rest) so we can see
    // WHICH stage rips the mesh. Logged here (after the solve, BEFORE post-passes) and
    // again at the very end. If it's already high here, the solve tears, not anti-clip.
    auto worstStretch = [&](const char* stage) {
        if (useEdges.empty()) return;
        float worst = 1.0f; int over = 0; size_t worstEi = 0;
        for (size_t ei = 0; ei < useEdges.size(); ++ei) {
            const auto& e = useEdges[ei];
            if (e.a >= n || e.b >= n) continue;
            float rest = (ei < supportRestLen.size()) ? supportRestLen[ei] : e.restLength;
            if (rest <= 1e-6f) continue;
            float len = Len(Sub(finalVerts[e.a], finalVerts[e.b]));
            float ratio = len / rest;
            if (ratio > 1.5f) ++over;
            if (ratio > worst) { worst = ratio; worstEi = ei; }
        }
        HLog("detour: TEAR shape='%s' stage='%s' worstStretch=%.2f over1.5=%d/%zu",
             shapeName.c_str(), stage, worst, over, useEdges.size());
    };
    worstStretch("after-solve");

    // NOTE: the per-vertex anti-clip pass runs LATER, AFTER seam-close â€” see below.
    // Running it here let seam-close drag the tucked verts back to a clipping position
    // (seam-close restores cross-island vanilla offsets and was undoing the anti-clip).

    // â”€â”€ Rigid-island pass â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // A medallion, buckle or plate should TRANSLATE with the morphing body but
    // keep its own shape â€” vanilla conform deforms it vertex-by-vertex (the
    // medallion bulges with the breast under it). Replace each rigid island's
    // per-vertex motion with a single rigid translation: the average
    // (supportVert - baseVert) over the island. Shape is preserved exactly.
    //
    // Two triggers:
    //   (a) Whole-mesh Rigid preset â†’ every island is rigid.
    //   (b) Loose/Fitted mesh with geometry-detected rigid islands (islandRigidOut)
    //       â†’ only those islands (the merged medallion/buckle) are rigidified; the
    //       skirt/sleeves keep their cloth solve. This is the "merged accessory"
    //       case the prototype validated.
    bool wholeMeshRigid = (normPreset == DRAPE_PRESET_RIGID_ACCESSORY ||
                           normPreset == DRAPE_PRESET_RIGID_ARMOR);
    // haveMergedRigid: geometry-detected rigid plates inside a Loose/Fitted mesh
    // (merged medallion case). Disabled for now â€” causes arm shrinkage regression
    // when the island BFS labelling doesn't perfectly match the rigid pass's view.
    // Re-enable once per-island logging confirms correct island identification.
    // bool haveMergedRigid = false;
    // for (uint8_t r : islandRigidOut) if (r) { haveMergedRigid = true; break; }

    if (wholeMeshRigid) {
        // Reuse the island labels from the clearance pass when available; otherwise
        // (whole-mesh rigid path, which skipped that pass) build them here.
        std::vector<int> island;
        int nIsl = 0;
        if (!islandIdOut.empty() && islandIdOut.size() == n) {
            island = islandIdOut;
            for (int id : island) nIsl = std::max(nIsl, id + 1);
        } else {
            island.assign(n, -1);
            std::vector<std::vector<uint32_t>> adjR(n);
            for (const auto& e : useEdges)
                if (e.a < n && e.b < n) { adjR[e.a].push_back(e.b); adjR[e.b].push_back(e.a); }
            for (size_t s = 0; s < n; ++s) {
                if (island[s] >= 0) continue;
                std::vector<uint32_t> q{(uint32_t)s};
                island[s] = nIsl;
                for (size_t qi = 0; qi < q.size(); ++qi)
                    for (uint32_t nb : adjR[q[qi]])
                        if (island[nb] < 0) { island[nb] = nIsl; q.push_back(nb); }
                ++nIsl;
            }
        }

        auto islandIsRigid = [&](int id) -> bool {
            if (wholeMeshRigid) return true;
            return id >= 0 && id < (int)islandRigidOut.size() && islandRigidOut[(size_t)id];
        };

        std::vector<Vec3> islandShift((size_t)nIsl, Vec3{0,0,0});
        std::vector<int>  islandCount((size_t)nIsl, 0);
        for (size_t i = 0; i < n; ++i) {
            int id = island[i];
            if (id < 0 || !islandIsRigid(id)) continue;
            // Include ALL verts in the average, not just rawDiff-touched ones.
            // Chain links and other accessories with near-zero skin weight produce
            // rawDiff â‰ˆ 0, making islandCount stay 0 and the island get no shift.
            // For rigid translate, every vert contributes its supportVert displacement.
            islandShift[(size_t)id] = Add(islandShift[(size_t)id], Sub(supportVerts[i], baseVerts[i]));
            ++islandCount[(size_t)id];
        }
        std::vector<int> islandSizeR(nIsl, 0);
        for (int id : island) if (id >= 0) ++islandSizeR[(size_t)id];
        for (int id = 0; id < nIsl; ++id) {
            if (islandCount[(size_t)id] > 0)
                islandShift[(size_t)id] = Scale(islandShift[(size_t)id], 1.0f / (float)islandCount[(size_t)id]);
        }

        // Compute per-island centroid for proximity-based shift inheritance.
        std::vector<Vec3> islandCentroid((size_t)nIsl, Vec3{0,0,0});
        std::vector<int>  islandCentCnt((size_t)nIsl, 0);
        for (size_t i = 0; i < n; ++i) {
            int id = island[i];
            if (id >= 0) {
                islandCentroid[(size_t)id] = Add(islandCentroid[(size_t)id], baseVerts[i]);
                ++islandCentCnt[(size_t)id];
            }
        }
        for (int id = 0; id < nIsl; ++id)
            if (islandCentCnt[(size_t)id] > 0)
                islandCentroid[(size_t)id] = Scale(islandCentroid[(size_t)id],
                                                    1.0f / (float)islandCentCnt[(size_t)id]);

        // Propagate shift from "anchor" islands (|shift| large) to "float" islands
        // (|shift| near zero). This fixes chain links that have near-zero skin weight:
        // sliders that move the torso barely touch chains, so their supportVerts delta
        // is ~0 even though the chain should follow the medallion it hangs from.
        // Strategy: for each low-shift rigid island, find the closest high-shift rigid
        // island by centroid distance and copy its shift.
        const float kShiftNearZero = 0.03f;  // shift below this is considered "floating"
        const float kShiftNearZeroSq = kShiftNearZero * kShiftNearZero;
        for (int id = 0; id < nIsl; ++id) {
            if (!islandIsRigid(id)) continue;
            if (Len2(islandShift[(size_t)id]) >= kShiftNearZeroSq) continue; // already has shift
            // Find nearest rigid island with a real shift.
            float bestDist2 = 1e30f;
            int   bestSrc   = -1;
            for (int src = 0; src < nIsl; ++src) {
                if (src == id || !islandIsRigid(src)) continue;
                if (Len2(islandShift[(size_t)src]) < kShiftNearZeroSq) continue;
                float d2 = Len2(Sub(islandCentroid[(size_t)id], islandCentroid[(size_t)src]));
                if (d2 < bestDist2) { bestDist2 = d2; bestSrc = src; }
            }
            if (bestSrc >= 0)
                islandShift[(size_t)id] = islandShift[(size_t)bestSrc];
        }

        // â”€â”€ Chain/cordÃ£o per-vert IDW shift â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // A necklace chain (isl#10 in Medallions) is a single large island that
        // spans between multiple medallion islands.  Applying one mean shift to all
        // 153 chain verts translates the whole chain as a block â†’ chain floats away
        // from the medallions when the body morphs.
        //
        // Fix: identify "chain" islands (large rigid islands whose mean shift is
        // plausible but whose individual verts need spatially-varying shifts) and
        // replace their uniform island shift with a per-vert IDW interpolation of
        // the shifts of the smaller "anchor" islands (the medallions).
        //
        // Chain heuristic: rigid island with > kChainVertThresh verts AND the ratio
        // of its bounding-box diagonal to the average medallion bbox diagonal is > 2.
        // In practice for this mesh: 153-vert chain vs 13/26-vert medallions.
        const int kChainVertThresh = 60;  // islands larger than this are chain candidates

        // Collect "anchor" islands: rigid, small (â‰¤ kChainVertThresh verts), real shift.
        struct AnchorIsl { int id; Vec3 centroid; Vec3 shift; };
        std::vector<AnchorIsl> anchors;
        for (int id = 0; id < nIsl; ++id) {
            if (!islandIsRigid(id)) continue;
            if (islandSizeR[(size_t)id] > kChainVertThresh) continue;
            if (Len2(islandShift[(size_t)id]) < kShiftNearZeroSq) continue;
            anchors.push_back({id, islandCentroid[(size_t)id], islandShift[(size_t)id]});
        }

        // For each chain island, build a per-vert shift table via IDW from anchors.
        // Key: island id â†’ vector<Vec3> indexed by position in the full vert array.
        // We store as a flat map vertIdxâ†’shift.
        std::unordered_map<size_t, Vec3> chainVertShift;
        if (!anchors.empty()) {
            for (size_t i = 0; i < n; ++i) {
                int id = island[i];
                if (id < 0 || !islandIsRigid(id)) continue;
                if (islandSizeR[(size_t)id] <= kChainVertThresh) continue;  // not a chain island
                // IDW: weight = 1/distÂ² to each anchor centroid.
                Vec3 sumShift{0,0,0};
                float sumW = 0.0f;
                for (const auto& a : anchors) {
                    float d2 = Len2(Sub(baseVerts[i], a.centroid));
                    if (d2 < 1e-6f) d2 = 1e-6f;
                    float w = 1.0f / d2;
                    sumShift = Add(sumShift, Scale(a.shift, w));
                    sumW += w;
                }
                if (sumW > 0.0f)
                    chainVertShift[i] = Scale(sumShift, 1.0f / sumW);
            }
            if (!chainVertShift.empty())
                HLog("  chain-IDW: %d chain verts remapped from %d anchors",
                     (int)chainVertShift.size(), (int)anchors.size());
        }

        // Log per-island rigid shift for diagnosis.
        for (int id = 0; id < nIsl; ++id) {
            if (wholeMeshRigid || (id < (int)islandRigidOut.size() && islandRigidOut[(size_t)id]))
                HLog("  rigid-isl#%d verts=%d cnt=%d shift=(%.3f,%.3f,%.3f)",
                     id, islandSizeR[(size_t)id], islandCount[(size_t)id],
                     islandShift[(size_t)id].x, islandShift[(size_t)id].y, islandShift[(size_t)id].z);
        }

        int rigidVerts = 0;
        for (size_t i = 0; i < n; ++i) {
            int id = island[i];
            if (id >= 0 && islandIsRigid(id)) {
                // Chain verts use per-vert IDW shift; medallion/plate verts use island mean.
                auto it = chainVertShift.find(i);
                if (it != chainVertShift.end())
                    finalVerts[i] = Add(baseVerts[i], it->second);
                else
                    finalVerts[i] = Add(baseVerts[i], islandShift[(size_t)id]);  // shape kept
                ++rigidVerts;
            }
        }
        stats.collisionAdjusted += rigidVerts;

        // â”€â”€ Rigid-border seam closure â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // After rigid translate, the border verts of rigid islands (belt, buckle)
        // sit at their correct translated positions. The adjacent cloth verts (bodice
        // bottom edge, skirt top edge) were placed by the PBD solver independently
        // and may have drifted away, creating a visible gap or V-crease at the border.
        //
        // For each near-touching base pair where ONE side is rigid and the other is
        // cloth, pull the CLOTH vert to maintain the vanilla relative offset from the
        // (now correctly placed) rigid vert. The rigid vert is never moved.
        if (!islandIdOut.empty() && islandIdOut.size() == n && nIsl > 0) {
            const float kBorderNear  = 0.80f;
            const float kBorderNear2 = kBorderNear * kBorderNear;
            const float bcell = 0.85f;
            auto bkey = [&](const Vec3& p) -> uint64_t {
                auto q = [&](float v) -> uint64_t { return (uint64_t)(uint32_t)(int32_t)std::llround(v / bcell); };
                return (q(p.x)*73856093ull)^(q(p.y)*19349663ull)^(q(p.z)*83492791ull);
            };
            std::unordered_map<uint64_t,std::vector<uint32_t>> bg;
            bg.reserve(n);
            for (size_t i = 0; i < n; ++i) bg[bkey(baseVerts[i])].push_back((uint32_t)i);

            int borderFixed = 0;
            for (int round = 0; round < 2; ++round) {
                for (size_t i = 0; i < n; ++i) {
                    int idi = islandIdOut[i];
                    if (idi < 0) continue;
                    bool ri = islandIsRigid(idi);
                    if (!ri) continue;  // start from rigid side only
                    const Vec3& p = baseVerts[i];
                    for (int dx=-1;dx<=1;++dx) for (int dy=-1;dy<=1;++dy) for (int dz=-1;dz<=1;++dz) {
                        Vec3 qp{p.x+dx*bcell,p.y+dy*bcell,p.z+dz*bcell};
                        auto it = bg.find(bkey(qp)); if (it==bg.end()) continue;
                        for (uint32_t j : it->second) {
                            if ((size_t)j == i) continue;
                            int idj = islandIdOut[j];
                            if (idj == idi) continue;           // same island
                            if (islandIsRigid(idj)) continue;   // both rigid â€” skip
                            if (Len2(Sub(baseVerts[j],p)) > kBorderNear2) continue;
                            // Pull cloth vert j to restore vanilla offset from rigid vert i.
                            Vec3 vanOff = Sub(supportVerts[j], supportVerts[i]);
                            Vec3 target = Add(finalVerts[i], vanOff);
                            // Blend: don't snap fully in one round, prevent fighting stretch solver.
                            float alpha = (round == 0) ? 0.6f : 0.9f;
                            finalVerts[j] = Add(finalVerts[j], Scale(Sub(target, finalVerts[j]), alpha));
                            if (round == 1) ++borderFixed;
                        }
                    }
                }
            }
            if (borderFixed > 0)
                HLog("detour: rigid-border shape='%s' fixed=%d", shapeName.c_str(), borderFixed);
        }
    }

    // â”€â”€ Cross-island seam closure (ROOT-CAUSE FIX for the back hole) â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // Root cause (proven by gap-diag instrumentation): the garment has separate
    // islands that BUTT against each other at the waist with NO triangle spanning
    // the join and only a small (~0.4u) vanilla separation. Vanilla conform moves
    // both sides by the same body diff, so the unmodeled seam stays sub-pixel. Our
    // physics moves each side independently (no edge links them, and the welds only
    // catch EXACTLY-coincident verts at 0.01u, not these 0.4u seams), so the sides
    // drift apart â€” measured up to 4.0u â€” opening the seam into a visible hole.
    //
    // Fix: for each near-touching cross-island base pair, restore the VANILLA
    // relative offset (supportVerts[j]-supportVerts[i]). Both sides keep the drape
    // motion the solver gave them, but their RELATIVE position is pinned to what
    // vanilla had, so the seam can never open wider than the body actually grew.
    // We move the higher-drape (looser) side toward the other to avoid fighting an
    // anchored side; if both are loose, split the correction.
    if (params.verletCloth && !islandIdOut.empty() && islandIdOut.size() == n) {
        // near-touching radius: 0.65u catches the back butt-joint (vanSep~0.4) AND
        // the belt-strap mini-island gaps (slightly wider). Front-to-back false
        // pairs are impossible â€” those are >5u apart (body width). Cell â‰¥ radius so
        // the 27-cell neighbour search covers the radius.
        const float kNear  = 0.65f;
        const float kNear2 = kNear * kNear;
        // RIGID-edge seams open WIDER: a rigid plate (the hip leather flap) has a step/
        // thickness, so its border verts and the adjacent cloth start farther apart than
        // 0.65u in the base mesh â€” the seam-close never even considered them, leaving a
        // visible gap when the plate rigid-translates away from the cloth in the morph
        // (the hole the user circled). Use a larger radius when ONE side is rigid so the
        // plate-to-cloth seam is caught and pinned to its vanilla offset. Cell scales up.
        const float kNearRigid  = 1.6f;
        const float kNearRigid2 = kNearRigid * kNearRigid;
        const float cell   = 1.65f;     // >= largest search radius
        auto ckey = [&](const Vec3& p) -> uint64_t {
            auto q = [&](float v) -> uint64_t { return (uint64_t)(uint32_t)(int32_t)std::llround(v / cell); };
            return (q(p.x) * 73856093ull) ^ (q(p.y) * 19349663ull) ^ (q(p.z) * 83492791ull);
        };
        std::unordered_map<uint64_t, std::vector<uint32_t>> g;
        g.reserve(n);
        for (size_t i = 0; i < n; ++i) g[ckey(baseVerts[i])].push_back((uint32_t)i);

        float worstBefore = 0.0f, worstAfter = 0.0f;
        int crossPairs = 0, fixedPairs = 0;
        // Two relaxation rounds so a vert shared by several cross pairs converges.
        for (int round = 0; round < 2; ++round) {
            for (size_t i = 0; i < n; ++i) {
                const Vec3& p = baseVerts[i];
                for (int dx=-1;dx<=1;++dx) for (int dy=-1;dy<=1;++dy) for (int dz=-1;dz<=1;++dz) {
                    Vec3 qp{ p.x+dx*cell, p.y+dy*cell, p.z+dz*cell };
                    auto it = g.find(ckey(qp)); if (it==g.end()) continue;
                    for (uint32_t j : it->second) {
                        if ((size_t)j<=i) continue;
                        if (islandIdOut[j]==islandIdOut[i]) continue;     // same island
                        // Rigid-island verts (medallion/buckle/hip-plate) must keep their
                        // exact translated shape â€” never move them. If BOTH sides are
                        // rigid, skip; if ONE is rigid, the loose side takes the full fix.
                        bool ri = (islandIdOut[i] >= 0 && islandIdOut[i] < (int)islandRigidOut.size()
                                   && islandRigidOut[(size_t)islandIdOut[i]]);
                        bool rj = (islandIdOut[j] >= 0 && islandIdOut[j] < (int)islandRigidOut.size()
                                   && islandRigidOut[(size_t)islandIdOut[j]]);
                        if (ri && rj) continue;
                        // Wider radius when one side is a rigid plate (stepped seam).
                        float near2 = (ri || rj) ? kNearRigid2 : kNear2;
                        if (Len2(Sub(baseVerts[j], p)) > near2) continue;  // near-touching only
                        // Target: keep the exact vanilla offset between the pair.
                        Vec3 vanOff = Sub(supportVerts[j], supportVerts[i]);
                        Vec3 curOff = Sub(finalVerts[j], finalVerts[i]);
                        Vec3 err = Sub(curOff, vanOff);             // how far we over-opened
                        float errLen = Len(err);
                        if (round == 0) {
                            ++crossPairs;
                            worstBefore = std::max(worstBefore, errLen);
                        }
                        if (errLen < 0.02f) continue;
                        // Distribute the correction: a rigid side never moves; otherwise
                        // the looser (higher drape) vert yields more.
                        float wi, wj;
                        if (ri)      { wi = 0.0f; wj = 1.0f; }   // i rigid â†’ only j moves
                        else if (rj) { wi = 1.0f; wj = 0.0f; }   // j rigid â†’ only i moves
                        else {
                            float di = drapeAmount[i], dj = drapeAmount[j];
                            float sum = di + dj;
                            wi = (sum > 1e-4f) ? dj / sum : 0.5f; // i takes share âˆ other's drape
                            wj = 1.0f - wi;
                        }
                        finalVerts[i] = Add(finalVerts[i], Scale(err, wi));
                        finalVerts[j] = Sub(finalVerts[j], Scale(err, wj));
                        if (round == 1) {
                            ++fixedPairs;
                            Vec3 newOff = Sub(finalVerts[j], finalVerts[i]);
                            worstAfter = std::max(worstAfter, Len(Sub(newOff, vanOff)));
                        }
                    }
                }
            }
        }
        if (crossPairs > 0)
            HLog("detour: seam-close shape='%s' crossPairs=%d fixed=%d worstBefore=%.3f worstAfter=%.3f",
                 shapeName.c_str(), crossPairs, fixedPairs, worstBefore, worstAfter);
    }

    // Final body recollision AFTER seam-close. Seam-close can move verts after the
    // earlier body collision; enforce only a tiny body floor here. Outer-layer room
    // conflicts are handled by the covered-vertex suppress pass below, not by
    // inflating the visible outer garment.
    if (params.verletCloth && bodyBVH && bodyBVH->valid) {
        const float kBodyFloorClear = 0.06f;
        int repushed = 0;
        for (size_t i = 0; i < n; ++i) {
            int id = islandIdOut.empty() ? -1 : (i < islandIdOut.size() ? islandIdOut[i] : -1);
            if (id >= 0 && id < (int)islandRigidOut.size() && islandRigidOut[(size_t)id])
                continue;                         // rigid plate: keep exact shape
            float ac = kBodyFloorClear;
            Vec3 target = finalVerts[i];
            if (BVHCollideVertex(*bodyBVH, target, ac)) {
                finalVerts[i] = target;
                ++repushed;
            }
        }
        if (repushed > 0) {
            stats.bodyAdjusted += repushed;
            HLog("detour: final-body-reclear shape='%s' mode='floor' clr=%.2f pushed=%d",
                 shapeName.c_str(), kBodyFloorClear, repushed);
        }
    }


    // â”€â”€ Anti-clip post-pass: keep THIS mesh tucked under its OUTER meshes â”€â”€â”€â”€
    // MUST run AFTER seam-close (which restores cross-island offsets and would undo it).
    // This is the LAST geometry pass â€” its corrections are the final word.
    //
    // ONE-DIRECTIONAL by layer rank (addresses the review): the collider is built ONLY
    // from garments ABOVE this one (strictly higher rank). The rule only ever PULLS THIS
    // mesh IN under them â€” it never pushes the outer mesh out, so the outermost layer (a
    // fur cloak) is never deformed (it gets no collider at all â†’ untouched).
    //
    // REAL CLEARANCE, not just order-sign inversion (addresses the review): a vert is
    // corrected whenever it fails to sit at least kClearance UNDER the outer surface â€”
    // i.e. it has popped outside, is coincident, OR is merely grazing within the margin.
    //
    // Metric = signed body-gap (distance to the body surface along the body normal),
    // which is well-defined on shoulders/back where radial-from-axis was degenerate.
    // BUT only act where the BASE mesh had THIS vert genuinely UNDER the outer mesh
    // (myBaseGap < otherBaseGap) â€” so we never invert a region where this mesh is
    // legitimately the outer one locally (dress-over-pants-at-hip etc. stays correct).
    // DISABLED: re-enabling the tuck made tearing WORSE on a second outfit (Yarl
    // Clothes 1) â€” generalized vertical rips across the dress. Moving inner-garment
    // verts along the body normal is fundamentally a tearing operation, no matter how
    // soft. The tuck is OFF until replaced by a non-moving approach (collapse-to-
    // neighbour suppression). Layer clipping is left to the user's zap slider for now.
    // Redesign step 3: final covered-region collapse.
    //
    // This is for the no-room case that movement constraints cannot solve: an inner
    // garment is covered by an outer garment in the base outfit, but the outer garment
    // sits too close to the body (or is a gappy fur sheet), so "move the inner cloth
    // under it while staying outside the body" has no valid position. Hide only the
    // covered connected regions, once, after all body recollision/seam passes.
    const bool kCoverCollapseEnabled = false;
    if (kCoverCollapseEnabled &&
        NormalizePreset(preset) != DRAPE_PRESET_AUTO &&
        NormalizePreset(preset) != DRAPE_PRESET_RIGID_ARMOR &&
        NormalizePreset(preset) != DRAPE_PRESET_RIGID_ACCESSORY &&
        outerColliders && !outerColliders->empty() &&
        bodyBVH && bodyBVH->valid &&
        !useEdges.empty()) {
        const BodyBVH* baseBodyBVH = g_refBodyBVH.valid ? &g_refBodyBVH : bodyBVH;
        const float kCoverOverlap = 4.25f;
        const float kCoverOverlap2 = kCoverOverlap * kCoverOverlap;
        const float kLooseCoverOverlap2 = 3.75f * 3.75f;
        const float kBaseUnderSlack = 0.10f;
        const float kCollapseDepth = 1.20f;

        auto coverMarginFor = [](int outerPreset) -> float {
            switch (NormalizePreset(outerPreset)) {
            case DRAPE_PRESET_LOOSE_DRAPED:    return 0.18f;
            case DRAPE_PRESET_FLEXIBLE_ARMOR:  return 0.36f;
            case DRAPE_PRESET_RIGID_ARMOR:     return 0.42f;
            case DRAPE_PRESET_BELT:            return 0.34f;
            case DRAPE_PRESET_RIGID_ACCESSORY: return 0.30f;
            default:                           return 0.24f;
            }
        };

        auto signedGap = [](const BodyBVH& bvh, const Vec3& p, float& gap) -> bool {
            Vec3 nrm{0.0f, 0.0f, 1.0f};
            return BVHSignedGap(bvh, p, gap, &nrm);
        };

        auto isRigidVertex = [&](size_t i) -> bool {
            if (islandIdOut.empty() || i >= islandIdOut.size()) return false;
            int id = islandIdOut[i];
            return id >= 0 && id < (int)islandRigidOut.size() && islandRigidOut[(size_t)id];
        };

        int coverDirect = 0;
        int coverNoRoom = 0;
        int coverLoose = 0;
        std::vector<uint8_t> covered(n, 0);

        auto sampleCovered = [&](const Vec3& baseP, const Vec3& curP, float floorGap) -> int {
            float myBaseGap = 0.0f;
            if (!signedGap(*baseBodyBVH, baseP, myBaseGap))
                return 0;
            float myGap = 0.0f;
            if (!signedGap(*bodyBVH, curP, myGap))
                return 0;

            for (const OuterCollider& oc : *outerColliders) {
                if (!oc.Base().valid || !oc.Current().valid) continue;

                Vec3 bcp{0.0f, 0.0f, 0.0f}, bnrm{0.0f, 0.0f, 1.0f};
                float bd2 = 0.0f;
                int baseTri = -1;
                float bu = 0.0f, bv = 0.0f, bw = 0.0f;
                if (!BVHClosestPointEx(oc.Base(), baseP, bcp, bnrm, bd2,
                                        &baseTri, &bu, &bv, &bw)) {
                    continue;
                }
                if (bd2 > kCoverOverlap2) continue;
                if (baseTri < 0 || baseTri >= (int)oc.Current().tris.size()) continue;

                float outerBaseGap = 0.0f;
                if (!signedGap(*baseBodyBVH, bcp, outerBaseGap))
                    continue;
                if (myBaseGap >= outerBaseGap + kBaseUnderSlack)
                    continue;

                Vec3 outerCur = TriPointBary(oc.Current(), baseTri, bu, bv, bw);
                float outerGap = 0.0f;
                if (!signedGap(*bodyBVH, outerCur, outerGap))
                    continue;

                float margin = coverMarginFor(oc.preset);
                float allowed = outerGap - margin;
                bool noRoom = allowed < floorGap;
                bool poking = myGap > allowed - 0.05f;
                bool looseCover =
                    NormalizePreset(oc.preset) == DRAPE_PRESET_LOOSE_DRAPED &&
                    bd2 <= kLooseCoverOverlap2;

                if (noRoom || poking || looseCover)
                    return looseCover ? 3 : (noRoom ? 2 : 1);
            }
            return 0;
        };

        for (size_t i = 0; i < n; ++i) {
            if (isRigidVertex(i)) continue;
            float floorGap = std::max(0.08f, vertClearance[i] + 0.02f);
            int reason = sampleCovered(baseVerts[i], finalVerts[i], floorGap);
            if (!reason) continue;
            covered[i] = 1;
            if (reason == 3) ++coverLoose;
            else if (reason == 2) ++coverNoRoom;
            else ++coverDirect;
        }

        // Face samples catch the visible case where a triangle crosses the cover even
        // though no single corner is the worst point. Mark the whole triangle; the
        // component erosion below protects the visible boundary.
        if (tris && !tris->empty()) {
            struct SampleBary { float a, b, c; };
            static const SampleBary kTriSamples[] = {
                {1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f},
                {0.5f, 0.5f, 0.0f},
                {0.5f, 0.0f, 0.5f},
                {0.0f, 0.5f, 0.5f},
            };
            auto triPoint = [](const std::vector<Vec3>& verts,
                               const std::array<uint16_t, 3>& t,
                               const SampleBary& s) -> Vec3 {
                return Add(Scale(verts[t[0]], s.a),
                           Add(Scale(verts[t[1]], s.b), Scale(verts[t[2]], s.c)));
            };

            int faceMarked = 0;
            for (const auto& t : *tris) {
                if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
                if (isRigidVertex(t[0]) || isRigidVertex(t[1]) || isRigidVertex(t[2]))
                    continue;
                bool mark = false;
                for (const SampleBary& sb : kTriSamples) {
                    Vec3 bp = triPoint(baseVerts, t, sb);
                    Vec3 cp = triPoint(finalVerts, t, sb);
                    if (sampleCovered(bp, cp, std::max(0.08f, params.orderConstraintBodyFloor))) {
                        mark = true;
                        break;
                    }
                }
                if (!mark) continue;
                covered[t[0]] = covered[t[1]] = covered[t[2]] = 1;
                ++faceMarked;
            }
            coverDirect += faceMarked;
        }

        int coveredCount = 0;
        for (uint8_t v : covered) if (v) ++coveredCount;

        if (coveredCount > 0) {
            std::vector<std::vector<uint32_t>> adj(n);
            for (const auto& e : useEdges) {
                if (e.a < n && e.b < n) {
                    adj[e.a].push_back(e.b);
                    adj[e.b].push_back(e.a);
                }
            }

            std::vector<uint8_t> seen(n, 0);
            std::vector<int> dist(n, -1);
            int components = 0;
            int collapsedVerts = 0;
            int skippedSmall = 0;

            for (size_t start = 0; start < n; ++start) {
                if (!covered[start] || seen[start]) continue;

                std::vector<uint32_t> comp;
                std::vector<uint32_t> q;
                q.push_back((uint32_t)start);
                seen[start] = 1;
                for (size_t qi = 0; qi < q.size(); ++qi) {
                    uint32_t v = q[qi];
                    comp.push_back(v);
                    for (uint32_t nb : adj[v]) {
                        if (nb >= n || !covered[nb] || seen[nb]) continue;
                        seen[nb] = 1;
                        q.push_back(nb);
                    }
                }

                if (comp.size() < 3) {
                    ++skippedSmall;
                    continue;
                }
                ++components;

                std::vector<uint32_t> ringQ;
                for (uint32_t v : comp) {
                    dist[v] = -1;
                    bool boundary = false;
                    for (uint32_t nb : adj[v]) {
                        if (nb >= n || !covered[nb]) {
                            boundary = true;
                            break;
                        }
                    }
                    if (boundary) {
                        dist[v] = 0;
                        ringQ.push_back(v);
                    }
                }

                for (size_t qi = 0; qi < ringQ.size(); ++qi) {
                    uint32_t v = ringQ[qi];
                    for (uint32_t nb : adj[v]) {
                        if (nb >= n || !covered[nb] || dist[nb] >= 0) continue;
                        dist[nb] = dist[v] + 1;
                        ringQ.push_back(nb);
                    }
                }

                int minRing = comp.size() >= 40 ? 1 : 0;
                std::vector<uint32_t> core;
                core.reserve(comp.size());
                for (uint32_t v : comp) {
                    int d = dist[v];
                    if (d < 0) d = minRing;
                    if (d >= minRing)
                        core.push_back(v);
                }
                if (core.size() < 3)
                    core = comp;

                Vec3 avgCP{0.0f, 0.0f, 0.0f};
                Vec3 avgNrm{0.0f, 0.0f, 0.0f};
                int cpCount = 0;
                for (uint32_t v : core) {
                    Vec3 cp{0.0f, 0.0f, 0.0f}, nrm{0.0f, 0.0f, 1.0f};
                    float d2 = 0.0f;
                    if (!BVHClosestPoint(*bodyBVH, finalVerts[v], cp, nrm, d2))
                        continue;
                    avgCP = Add(avgCP, cp);
                    avgNrm = Add(avgNrm, nrm);
                    ++cpCount;
                }
                if (cpCount <= 0)
                    continue;

                avgCP = Scale(avgCP, 1.0f / (float)cpCount);
                float nl = Len(avgNrm);
                Vec3 nrm = nl > 1.0e-5f ? Scale(avgNrm, 1.0f / nl) : Vec3{0.0f, 0.0f, 1.0f};
                Vec3 target = Add(avgCP, Scale(nrm, -kCollapseDepth));

                for (uint32_t v : core) {
                    finalVerts[v] = target;
                    ++collapsedVerts;
                }
            }

            if (collapsedVerts > 0) {
                HLog("detour: cover-collapse shape='%s' covered=%d collapsed=%d comps=%d small=%d direct=%d noRoom=%d loose=%d",
                     shapeName.c_str(), coveredCount, collapsedVerts, components, skippedSmall,
                     coverDirect, coverNoRoom, coverLoose);
            }
        }
    }

    // Soft occlusion diagnostics only. Mutating a partial set of covered verts
    // back toward the base mesh opens gaps between neighboring vertices on
    // layered outfits. Any visual zap/hide solution must live outside vertex
    // deformation; this path must keep geometry continuous.
    const bool kCoveredDiffMaskEnabled = false;
    const bool kCoveredClipDiagnosticEnabled = false;
    if ((kCoveredDiffMaskEnabled || kCoveredClipDiagnosticEnabled) &&
        NormalizePreset(preset) != DRAPE_PRESET_AUTO &&
        NormalizePreset(preset) != DRAPE_PRESET_RIGID_ARMOR &&
        NormalizePreset(preset) != DRAPE_PRESET_RIGID_ACCESSORY &&
        outerColliders && !outerColliders->empty() &&
        bodyBVH && bodyBVH->valid &&
        !useEdges.empty()) {
        const BodyBVH* baseBodyBVH = g_refBodyBVH.valid ? &g_refBodyBVH : bodyBVH;
        const float kMaskOverlap2 = 4.25f * 4.25f;
        const float kLooseMaskOverlap2 = 3.75f * 3.75f;
        const float kBaseUnderSlack = 0.10f;

        struct CoverHit {
            int reason = 0;       // 1=poking, 2=no-room, 3=loose-cover
            float myGap = 0.0f;
            float outerGap = 0.0f;
            float allowedGap = 0.0f;
            float myBaseGap = 0.0f;
            float outerBaseGap = 0.0f;
            float outerDist = 0.0f;
            std::string outerName;
        };

        auto maskMarginFor = [](int outerPreset) -> float {
            switch (NormalizePreset(outerPreset)) {
            case DRAPE_PRESET_LOOSE_DRAPED:    return 0.16f;
            case DRAPE_PRESET_FLEXIBLE_ARMOR:  return 0.34f;
            case DRAPE_PRESET_RIGID_ARMOR:     return 0.40f;
            case DRAPE_PRESET_BELT:            return 0.32f;
            case DRAPE_PRESET_RIGID_ACCESSORY: return 0.28f;
            default:                           return 0.22f;
            }
        };

        auto signedGap = [](const BodyBVH& bvh, const Vec3& p, float& gap) -> bool {
            Vec3 nrm{0.0f, 0.0f, 1.0f};
            return BVHSignedGap(bvh, p, gap, &nrm);
        };

        auto isRigidVertex = [&](size_t i) -> bool {
            if (islandIdOut.empty() || i >= islandIdOut.size()) return false;
            int id = islandIdOut[i];
            return id >= 0 && id < (int)islandRigidOut.size() && islandRigidOut[(size_t)id];
        };

        int maskDirect = 0;
        int maskNoRoom = 0;
        int maskLoose = 0;
        std::vector<uint8_t> covered(n, 0);
        std::vector<CoverHit> hits(n);

        auto sampleCovered = [&](const Vec3& baseP, const Vec3& curP,
                                 float floorGap, CoverHit* outHit) -> int {
            float myBaseGap = 0.0f;
            if (!signedGap(*baseBodyBVH, baseP, myBaseGap))
                return 0;
            float myGap = 0.0f;
            if (!signedGap(*bodyBVH, curP, myGap))
                return 0;

            for (const OuterCollider& oc : *outerColliders) {
                if (!oc.Base().valid || !oc.Current().valid) continue;

                Vec3 bcp{0.0f, 0.0f, 0.0f}, bnrm{0.0f, 0.0f, 1.0f};
                float bd2 = 0.0f;
                int baseTri = -1;
                float bu = 0.0f, bv = 0.0f, bw = 0.0f;
                if (!BVHClosestPointEx(oc.Base(), baseP, bcp, bnrm, bd2,
                                        &baseTri, &bu, &bv, &bw)) {
                    continue;
                }
                if (bd2 > kMaskOverlap2) continue;
                if (baseTri < 0 || baseTri >= (int)oc.Current().tris.size()) continue;

                float outerBaseGap = 0.0f;
                if (!signedGap(*baseBodyBVH, bcp, outerBaseGap))
                    continue;
                if (myBaseGap >= outerBaseGap + kBaseUnderSlack)
                    continue;

                Vec3 outerCur = TriPointBary(oc.Current(), baseTri, bu, bv, bw);
                float outerGap = 0.0f;
                if (!signedGap(*bodyBVH, outerCur, outerGap))
                    continue;

                float allowed = outerGap - maskMarginFor(oc.preset);
                bool noRoom = allowed < floorGap;
                bool poking = myGap > allowed - 0.03f;
                bool looseCover =
                    !kCoveredDiffMaskEnabled &&
                    NormalizePreset(oc.preset) == DRAPE_PRESET_LOOSE_DRAPED &&
                    bd2 <= kLooseMaskOverlap2;

                if ((noRoom && poking) || looseCover) {
                    int reason = looseCover ? 3 : 2;
                    if (outHit) {
                        outHit->reason = reason;
                        outHit->myGap = myGap;
                        outHit->outerGap = outerGap;
                        outHit->allowedGap = allowed;
                        outHit->myBaseGap = myBaseGap;
                        outHit->outerBaseGap = outerBaseGap;
                        outHit->outerDist = std::sqrt(std::max(0.0f, bd2));
                        outHit->outerName = oc.name;
                    }
                    return reason;
                }
            }
            return 0;
        };

        for (size_t i = 0; i < n; ++i) {
            if (isRigidVertex(i)) continue;
            float floorGap = std::max(0.08f, vertClearance[i] + 0.02f);
            int reason = sampleCovered(baseVerts[i], finalVerts[i], floorGap, &hits[i]);
            if (!reason) continue;
            covered[i] = 1;
            if (reason == 3) ++maskLoose;
            else if (reason == 2) ++maskNoRoom;
            else ++maskDirect;
        }

        if (tris && !tris->empty()) {
            struct SampleBary { float a, b, c; };
            static const SampleBary kTriSamples[] = {
                {1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f},
                {0.5f, 0.5f, 0.0f},
                {0.5f, 0.0f, 0.5f},
                {0.0f, 0.5f, 0.5f},
            };
            auto triPoint = [](const std::vector<Vec3>& verts,
                               const std::array<uint16_t, 3>& t,
                               const SampleBary& s) -> Vec3 {
                return Add(Scale(verts[t[0]], s.a),
                           Add(Scale(verts[t[1]], s.b), Scale(verts[t[2]], s.c)));
            };

            int faceMarked = 0;
            for (const auto& t : *tris) {
                if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
                if (isRigidVertex(t[0]) || isRigidVertex(t[1]) || isRigidVertex(t[2]))
                    continue;

                bool mark = false;
                for (const SampleBary& sb : kTriSamples) {
                    Vec3 bp = triPoint(baseVerts, t, sb);
                    Vec3 cp = triPoint(finalVerts, t, sb);
                    float floorGap = std::max(0.08f, vertClearance[t[0]] + 0.02f);
                    if (sampleCovered(bp, cp, floorGap, nullptr)) {
                        mark = true;
                        break;
                    }
                }
                if (!mark) continue;
                covered[t[0]] = covered[t[1]] = covered[t[2]] = 1;
                ++faceMarked;
            }
            maskDirect += faceMarked;
        }

        int coveredCount = 0;
        for (uint8_t v : covered) if (v) ++coveredCount;

        if (coveredCount > 0) {
            std::vector<std::vector<uint32_t>> adj(n);
            for (const auto& e : useEdges) {
                if (e.a < n && e.b < n) {
                    adj[e.a].push_back(e.b);
                    adj[e.b].push_back(e.a);
                }
            }

            std::vector<uint8_t> seen(n, 0);
            std::vector<float> maskWeight(n, 0.0f);
            std::vector<int> componentId(n, -1);
            int components = 0;
            int skippedSmall = 0;

            for (size_t start = 0; start < n; ++start) {
                if (!covered[start] || seen[start]) continue;

                std::vector<uint32_t> comp;
                std::vector<uint32_t> q;
                q.push_back((uint32_t)start);
                seen[start] = 1;
                for (size_t qi = 0; qi < q.size(); ++qi) {
                    uint32_t v = q[qi];
                    comp.push_back(v);
                    for (uint32_t nb : adj[v]) {
                        if (nb >= n || !covered[nb] || seen[nb]) continue;
                        seen[nb] = 1;
                        q.push_back(nb);
                    }
                }

                if (comp.size() < 4) {
                    ++skippedSmall;
                    continue;
                }
                ++components;
                int compId = components;
                for (uint32_t v : comp)
                    componentId[v] = compId;

                std::vector<int> dist(n, -1);
                std::vector<uint32_t> ringQ;
                for (uint32_t v : comp) {
                    bool boundary = false;
                    for (uint32_t nb : adj[v]) {
                        if (nb >= n || !covered[nb]) {
                            boundary = true;
                            break;
                        }
                    }
                    if (boundary) {
                        dist[v] = 0;
                        ringQ.push_back(v);
                    }
                }

                if (ringQ.empty()) {
                    for (uint32_t v : comp)
                        maskWeight[v] = std::max(maskWeight[v], 0.92f);
                    continue;
                }

                for (size_t qi = 0; qi < ringQ.size(); ++qi) {
                    uint32_t v = ringQ[qi];
                    for (uint32_t nb : adj[v]) {
                        if (nb >= n || !covered[nb] || dist[nb] >= 0) continue;
                        dist[nb] = dist[v] + 1;
                        ringQ.push_back(nb);
                    }
                }

                for (uint32_t v : comp) {
                    int d = dist[v];
                    float w = 0.0f;
                    if (d >= 3)      w = 0.95f;
                    else if (d == 2) w = 0.78f;
                    else if (d == 1) w = 0.48f;
                    else             w = 0.16f;  // covered boundary is under an outer garment; slight feather prevents cap pullback.
                    maskWeight[v] = std::max(maskWeight[v], w);
                }
            }

            int weightedVerts = 0;
            float maxWeight = 0.0f;
            for (size_t i = 0; i < n; ++i) {
                float w = Clamp01(maskWeight[i]);
                if (w <= 0.01f) continue;
                ++weightedVerts;
                maxWeight = std::max(maxWeight, w);
            }

            if (kCoveredClipDiagnosticEnabled) {
                std::string fileName =
                    "drape_cover_candidates__" +
                    SafeFilePart(g_projectKey) + "__" +
                    SafeFilePart(shapeName) + "__" +
                    SafeFilePart(sliderName) + ".csv";
                fs::path csvPath = TempDrapePath(fileName);

                auto csvText = [](const std::string& s) -> std::string {
                    std::string out = "\"";
                    for (char ch : s)
                        out.push_back(ch == '"' ? '\'' : ch);
                    out.push_back('"');
                    return out;
                };
                auto reasonText = [](int reason) -> const char* {
                    switch (reason) {
                    case 1: return "poking";
                    case 2: return "no_room";
                    case 3: return "loose_cover";
                    default: return "face_or_boundary";
                    }
                };

                std::ofstream csv(csvPath, std::ios::trunc);
                if (csv) {
                    csv << "kind,index,component,weight,reason,outer,"
                           "x,y,z,base_x,base_y,base_z,"
                           "my_gap,outer_gap,allowed_gap,my_base_gap,outer_base_gap,outer_distance\n";
                    for (size_t i = 0; i < n; ++i) {
                        if (!covered[i]) continue;
                        const CoverHit& h = hits[i];
                        csv << "vertex," << i << ','
                            << componentId[i] << ','
                            << Clamp01(maskWeight[i]) << ','
                            << reasonText(h.reason) << ','
                            << csvText(h.outerName) << ','
                            << finalVerts[i].x << ',' << finalVerts[i].y << ',' << finalVerts[i].z << ','
                            << baseVerts[i].x << ',' << baseVerts[i].y << ',' << baseVerts[i].z << ','
                            << h.myGap << ',' << h.outerGap << ',' << h.allowedGap << ','
                            << h.myBaseGap << ',' << h.outerBaseGap << ',' << h.outerDist << '\n';
                    }
                    HLog("detour: cover-diag shape='%s' slider='%s' covered=%d core=%d comps=%d small=%d direct=%d noRoom=%d loose=%d maxW=%.2f csv='%s'",
                         shapeName.c_str(), sliderName.c_str(), coveredCount, weightedVerts,
                         components, skippedSmall, maskDirect, maskNoRoom, maskLoose,
                         maxWeight, csvPath.string().c_str());
                } else {
                    HLog("detour: cover-diag shape='%s' slider='%s' covered=%d core=%d csv_write_failed='%s'",
                         shapeName.c_str(), sliderName.c_str(), coveredCount, weightedVerts,
                         csvPath.string().c_str());
                }
            }

            if (kCoveredDiffMaskEnabled && weightedVerts > 0) {
                int maskedVerts = 0;
                for (size_t i = 0; i < n; ++i) {
                    float w = Clamp01(maskWeight[i]);
                    if (w <= 0.01f) continue;
                    finalVerts[i] = Add(finalVerts[i], Scale(Sub(baseVerts[i], finalVerts[i]), w));
                    ++maskedVerts;
                }
                HLog("detour: cover-diff-mask shape='%s' covered=%d masked=%d comps=%d small=%d direct=%d noRoom=%d loose=%d maxW=%.2f",
                     shapeName.c_str(), coveredCount, maskedVerts, components, skippedSmall,
                     maskDirect, maskNoRoom, maskLoose, maxWeight);
            }
        }
    }

    const bool kEnableAntiClipTuck = false;
    // â”€â”€ REDESIGN STEP 1: master switch OFF for the whole tangled anti-clip block â”€â”€
    // Per the architecture review (ANTICLIP_ARCHITECTURE_REVIEW.md): disable the entire
    // post-solve anti-clip (tuck + collapse + suppress + cleanup) to establish a CLEAN,
    // tear-free BASE we can compare against. Code is kept (not deleted) until the new
    // in-solve order constraint + per-region collapse are proven. Flip to true only to
    // study the old behaviour. The new architecture will NOT live in this block.
    const bool kAntiClipLegacyEnabled = false;
    if (kAntiClipLegacyEnabled &&
        NormalizePreset(preset) != DRAPE_PRESET_AUTO &&
        outerColliders && !outerColliders->empty() &&
        bodyBVH && bodyBVH->valid) {
        const float kOverlap = 2.75f;   // base proximity to count as a real overlap
        const float kBaseUnderSlack = 0.06f; // tolerate near-coincident base layers
        const int   kRounds  = 4;
        auto bodyGap = [&](const Vec3& p, Vec3& outNrm) -> float {
            float gap = 0.0f;
            if (!BVHSignedGap(*bodyBVH, p, gap, &outNrm)) {
                outNrm = Vec3{0,0,1};
                return 0.0f;
            }
            return gap;
        };
        const BodyBVH* baseBodyBVH = g_refBodyBVH.valid ? &g_refBodyBVH : bodyBVH;
        auto baseBodyGap = [&](const Vec3& p, Vec3& outNrm) -> float {
            float gap = 0.0f;
            if (!BVHSignedGap(*baseBodyBVH, p, gap, &outNrm)) {
                outNrm = Vec3{0,0,1};
                return 0.0f;
            }
            return gap;
        };
        // Adaptive clearance by the OUTER mesh's preset (review #3): a thin gappy fur
        // cloak needs a small margin (don't crush the inner mesh into the body); rigid
        // armor/belt sits thicker and needs more.
        auto clearanceFor = [](int outerPreset) -> float {
            switch (NormalizePreset(outerPreset)) {
            case DRAPE_PRESET_LOOSE_DRAPED:    return 0.20f;  // cloak/cape â€” small
            case DRAPE_PRESET_RIGID_ARMOR:
            case DRAPE_PRESET_FLEXIBLE_ARMOR:  return 0.45f;  // armor â€” thick
            case DRAPE_PRESET_BELT:            return 0.40f;  // belt â€” thick
            default:                           return 0.30f;
            }
        };

        int tucked = 0;
        for (int round = 0; kEnableAntiClipTuck && round < kRounds; ++round) {
            int roundMoved = 0;
            for (size_t i = 0; i < n; ++i) {
                Vec3 myNrm{};
                float myGap = bodyGap(finalVerts[i], myNrm);

                // Against EACH outer garment separately (review #1: keep identity).
                for (const OuterCollider& oc : *outerColliders) {
                    if (!oc.Base().valid || !oc.Current().valid) continue;
                    const float kClearance = clearanceFor(oc.preset);

                    // Was I UNDER this outer mesh in the BASE config, and overlapping?
                    Vec3 bcp{0,0,0}, bnrm{0,0,1}; float bd2 = 0.0f;
                    int baseTri = -1; float bu = 0.0f, bv = 0.0f, bw = 0.0f;
                    if (!BVHClosestPointEx(oc.Base(), baseVerts[i], bcp, bnrm, bd2,
                                           &baseTri, &bu, &bv, &bw)) continue;
                    if (std::sqrt(bd2) > kOverlap) continue;
                    Vec3 dummy{};
                    if (baseBodyGap(baseVerts[i], dummy) >= baseBodyGap(bcp, dummy) + kBaseUnderSlack) continue;
                    if (baseTri < 0 || baseTri >= (int)oc.Current().tris.size()) continue;

                    // CURRENT: am I clearing THIS outer mesh by its clearance?
                    Vec3 cp = TriPointBary(oc.Current(), baseTri, bu, bv, bw);
                    float othGap  = bodyGap(cp, dummy);
                    float allowed = othGap - kClearance;
                    if (myGap <= allowed) continue;            // already tucked under this one

                    // BODY-CLAMP (review #2): never pull a vert closer to the body than a
                    // tiny floor â€” if the outer mesh hugs the body so tight there's no room
                    // for the inner garment, do NOT reintroduce body clipping. Tuck only as
                    // far as the floor allows (partial fix beats a body poke-through).
                    const float kBodyFloor = 0.06f;            // min gap to keep off the body
                    float target = std::max(allowed, kBodyFloor);
                    if (myGap <= target) continue;
                    // SOFT push (half-step), not a hard snap: moving each vert all the
                    // way at once tears the mesh (neighbours move by different amounts â†’
                    // edges over-stretch). The stretch relax below pulls neighbours along.
                    float step = (target - myGap) * 0.5f;
                    finalVerts[i] = Add(finalVerts[i], Scale(myNrm, step));
                    myGap += step;                             // update for next collider
                    if (round == 0) ++tucked;
                    ++roundMoved;
                }
            }
            if (roundMoved == 0) break;
        }
        // Relax over-stretch so the tuck pulls neighbours along instead of ripping.
        // Asymmetric (resist stretch only) against the conformed rest length â€” same
        // constraint the main solver uses, so it never fights body-induced expansion.
        if (tucked > 0 && !useEdges.empty()) {
            for (int it = 0; it < 6; ++it) {
                for (size_t ei = 0; ei < useEdges.size(); ++ei) {
                    const auto& e = useEdges[ei];
                    if (e.a >= n || e.b >= n) continue;
                    Vec3 dir = Sub(finalVerts[e.a], finalVerts[e.b]);
                    float len = Len(dir);
                    if (len <= 1e-6f) continue;
                    float rest = (ei < supportRestLen.size()) ? supportRestLen[ei] : e.restLength;
                    if (len <= rest) continue;                 // only resist over-stretch
                    Vec3 corr = Scale(dir, ((len - rest) / len) * 0.5f * 0.5f);
                    finalVerts[e.a] = Sub(finalVerts[e.a], corr);
                    finalVerts[e.b] = Add(finalVerts[e.b], corr);
                }
            }
        }
        if (tucked > 0)
            HLog("detour: anti-clip shape='%s' tuckedUnder=%d outers=%zu",
                 shapeName.c_str(), tucked, outerColliders->size());

        // â”€â”€ Covered-vertex COLLAPSE (zap-style hide, no rips) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // For verts that are genuinely COVERED by an outer garment AND have no room to
        // tuck (body conflict), we hide them WITHOUT moving visible geometry: collapse
        // each fully-covered vert onto a covered neighbour so its triangles become
        // zero-area and vanish. Unlike moving verts (which tears â€” neighbours move by
        // different amounts), a collapse only ever shrinks an edge to length 0, so no
        // edge over-stretches. The CRUCIAL rule: a vert is collapsible ONLY if it AND
        // all its topological neighbours are covered (erode the region by one ring), so
        // the visible boundary of the covered patch is never half-collapsed. This is the
        // correct, rip-free version of the disabled suppress pass below.
        const bool kEnableCollapse = true;
        if (kEnableCollapse && !useEdges.empty()) {
            // 1) Mark covered verts: under an outer garment in base, now poking through it.
            std::vector<uint8_t> covered(n, 0);
            for (size_t i = 0; i < n; ++i) {
                Vec3 myNrm{};
                float myGap = bodyGap(finalVerts[i], myNrm);
                for (const OuterCollider& oc : *outerColliders) {
                    if (!oc.Base().valid || !oc.Current().valid) continue;
                    const float kClearance = clearanceFor(oc.preset);
                    Vec3 bcp{0,0,0}, bnrm{0,0,1}; float bd2 = 0.0f;
                    int baseTri = -1; float bu=0,bv=0,bw=0;
                    if (!BVHClosestPointEx(oc.Base(), baseVerts[i], bcp, bnrm, bd2, &baseTri,&bu,&bv,&bw)) continue;
                    if (std::sqrt(bd2) > kOverlap) continue;
                    Vec3 dummy{};
                    if (baseBodyGap(baseVerts[i], dummy) >= baseBodyGap(bcp, dummy) + kBaseUnderSlack) continue;
                    if (baseTri < 0 || baseTri >= (int)oc.Current().tris.size()) continue;
                    Vec3 cp = TriPointBary(oc.Current(), baseTri, bu, bv, bw);
                    float outerGap = bodyGap(cp, dummy);
                    // This vert was UNDER the outer garment in the base (checked above via
                    // baseBodyGap), so it is meant to be hidden by it. It CLIPS if its
                    // current body-gap is at/above the outer surface (poking out from under
                    // the cloak). Hide it by collapse. We do NOT require a body conflict â€”
                    // any covered vert that pokes through is invisible-by-design and safe to
                    // collapse. Small tolerance so we only catch genuine pokethrough.
                    if (myGap > outerGap - 0.05f) {
                        covered[i] = 1;
                        break;
                    }
                }
            }
            // 2) Build vertex adjacency.
            std::vector<std::vector<uint32_t>> adj(n);
            for (const auto& e : useEdges)
                if (e.a < n && e.b < n) { adj[e.a].push_back(e.b); adj[e.b].push_back(e.a); }
            // 3) Collapse each COVERED vert that has at least one covered neighbour onto
            //    that neighbour (shared edge â†’ length 0 â†’ triangle vanishes). A covered
            //    vert with NO covered neighbour is an isolated pokethrough on the boundary;
            //    collapsing it would pull a visible edge, so we leave it. Reading targets
            //    from a SNAPSHOT of the pre-collapse positions avoids chained drift.
            // 3) HIDE the covered region by sinking each covered vert to the body
            //    surface, MINUS a depth â€” but only verts whose neighbours are ALSO covered
            //    move the full depth; boundary verts (some neighbour visible) blend, so the
            //    surface tapers smoothly into the body instead of ripping. Because the sink
            //    is along the body normal and the depth varies smoothly (full interior â†’
            //    zero at the visible boundary), no edge over-stretches: it is a smooth dent,
            //    not an isolated spike. The dented region is under the outer garment, so the
            //    dent is invisible and the pokethrough is gone.
            // Compute per-vert "interiorness" = fraction of neighbours that are covered.
            std::vector<float> interiorness(n, 0.0f);
            for (size_t i = 0; i < n; ++i) {
                if (!covered[i] || adj[i].empty()) continue;
                int c = 0; for (uint32_t nb : adj[i]) if (covered[nb]) ++c;
                interiorness[i] = (float)c / (float)adj[i].size();
            }
            // Smooth interiorness a couple of rings so the dent depth has no hard step.
            for (int it = 0; it < 3; ++it) {
                std::vector<float> nxt = interiorness;
                for (size_t i = 0; i < n; ++i) {
                    if (!covered[i] || adj[i].empty()) continue;
                    float s = interiorness[i]; int c = 1;
                    for (uint32_t nb : adj[i]) { s += interiorness[nb]; ++c; }
                    nxt[i] = s / (float)c;
                }
                interiorness.swap(nxt);
            }
            const float kSinkDepth = 1.2f;     // how far under the body the core sinks
            int collapsed = 0;
            for (size_t i = 0; i < n; ++i) {
                if (!covered[i]) continue;
                Vec3 cp{0,0,0}, nrm{0,0,1}; float d2 = 0.0f;
                if (!BVHClosestPoint(*bodyBVH, finalVerts[i], cp, nrm, d2)) continue;
                // Target gap = -kSinkDepth at the interior, easing to ~0 at the boundary.
                float w = ClampRange(interiorness[i], 0.0f, 1.0f);
                float targetGap = -kSinkDepth * w;
                Vec3 target = Add(cp, Scale(nrm, targetGap));
                finalVerts[i] = Add(finalVerts[i], Scale(Sub(target, finalVerts[i]), w));
                if (w > 0.05f) ++collapsed;
            }
            int coveredCount = 0; for (size_t i=0;i<n;++i) if (covered[i]) ++coveredCount;
            if (coveredCount > 0)
                HLog("detour: anti-clip-collapse shape='%s' covered=%d collapsed=%d",
                     shapeName.c_str(), coveredCount, collapsed);
        }

        // â”€â”€ Covered-vertex suppression (DISABLED) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // The idea (zap covered inner verts where there's no room) is sound, but this
        // implementation moves verts deep INTO the body to hide them, which tears the
        // mesh: a hidden vert pulled 1-2u inward while its neighbours stay put creates
        // stretched/ripped triangles (observed hidden=172 feathered=475 deepestGap=-2.25).
        // The correct "hide" is collapsing a covered vert ONTO a neighbour (zero-area),
        // not pushing it through the body â€” left for a future rewrite. For now the
        // tuck-only pass above handles the easy cases; residual covered clipping is left
        // for the user's zap slider (no rips). Kept compiled-but-dead for reference.
        const bool kEnableSuppress = false;
        if (kEnableSuppress) {
        const float kBodyFloor = 0.06f;
        std::vector<float> suppress(n, 0.0f);
        std::vector<float> suppressTargetGap(n, 0.0f);
        int suppressCore = 0;
        int suppressFaceTris = 0;
        int suppressFaceSamples = 0;
        float worstSuppressPen = 0.0f;
        float deepestSuppressGap = 0.0f;
        auto markSuppress = [&](size_t vi, float targetGap, float pen) {
            if (vi >= n) return;
            if (suppress[vi] < 1.0f) {
                suppress[vi] = 1.0f;
                ++suppressCore;
            }
            suppressTargetGap[vi] = std::min(suppressTargetGap[vi], targetGap);
            deepestSuppressGap = std::min(deepestSuppressGap, targetGap);
            worstSuppressPen = std::max(worstSuppressPen, pen);
        };
        for (size_t i = 0; i < n; ++i) {
            Vec3 myNrm{};
            float myGap = bodyGap(finalVerts[i], myNrm);
            for (const OuterCollider& oc : *outerColliders) {
                if (!oc.Base().valid || !oc.Current().valid) continue;
                const float kClearance = clearanceFor(oc.preset);

                Vec3 bcp{0,0,0}, bnrm{0,0,1}; float bd2 = 0.0f;
                int baseTri = -1; float bu = 0.0f, bv = 0.0f, bw = 0.0f;
                if (!BVHClosestPointEx(oc.Base(), baseVerts[i], bcp, bnrm, bd2,
                                       &baseTri, &bu, &bv, &bw)) continue;
                if (std::sqrt(bd2) > kOverlap) continue;
                Vec3 dummy{};
                if (baseBodyGap(baseVerts[i], dummy) >= baseBodyGap(bcp, dummy) + kBaseUnderSlack) continue;
                if (baseTri < 0 || baseTri >= (int)oc.Current().tris.size()) continue;

                Vec3 cp = TriPointBary(oc.Current(), baseTri, bu, bv, bw);
                float outerGap = bodyGap(cp, dummy);
                float allowed = outerGap - kClearance;
                float pen = myGap - allowed;
                if (pen <= 0.01f) continue;

                // Body conflict: the outer surface does not leave even the minimum
                // body floor for this inner layer, so a movement-only fix is invalid.
                if (allowed >= kBodyFloor) continue;
                if (suppress[i] < 1.0f) {
                    suppress[i] = 1.0f;
                    ++suppressCore;
                }
                float targetGap = ClampRange(allowed - 0.30f, -3.00f, -0.55f);
                markSuppress(i, targetGap, pen);
            }
        }

        // Vertex-only tests miss the most visible failure mode: a triangle face can
        // cross the outer layer even when none of its corners is a good sample.
        // Sample each inner triangle in base/current space and suppress the whole
        // triangle when a covered surface sample is in a body-conflict region.
        if (tris && !tris->empty()) {
            struct SampleBary { float a, b, c; };
            static const SampleBary kSamples[] = {
                {1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f},
                {0.5f, 0.5f, 0.0f},
                {0.5f, 0.0f, 0.5f},
                {0.0f, 0.5f, 0.5f},
                {0.60f, 0.20f, 0.20f},
                {0.20f, 0.60f, 0.20f},
                {0.20f, 0.20f, 0.60f},
            };
            auto triPoint = [](const std::vector<Vec3>& verts,
                               const std::array<uint16_t, 3>& t,
                               const SampleBary& s) -> Vec3 {
                return Add(Scale(verts[t[0]], s.a),
                           Add(Scale(verts[t[1]], s.b), Scale(verts[t[2]], s.c)));
            };

            for (const auto& t : *tris) {
                if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
                bool triMarked = false;
                float triTargetGap = 0.0f;
                float triWorstPen = 0.0f;
                for (const SampleBary& sb : kSamples) {
                    Vec3 baseSample = triPoint(baseVerts, t, sb);
                    Vec3 curSample  = triPoint(finalVerts, t, sb);
                    Vec3 myNrm{};
                    float myGap = bodyGap(curSample, myNrm);

                    for (const OuterCollider& oc : *outerColliders) {
                        if (!oc.Base().valid || !oc.Current().valid) continue;
                        const float kClearance = clearanceFor(oc.preset);

                        Vec3 bcp{0,0,0}, bnrm{0,0,1}; float bd2 = 0.0f;
                        int baseTri = -1; float bu = 0.0f, bv = 0.0f, bw = 0.0f;
                        if (!BVHClosestPointEx(oc.Base(), baseSample, bcp, bnrm, bd2,
                                               &baseTri, &bu, &bv, &bw)) continue;
                        if (std::sqrt(bd2) > kOverlap) continue;
                        Vec3 dummy{};
                        if (baseBodyGap(baseSample, dummy) >= baseBodyGap(bcp, dummy) + kBaseUnderSlack) continue;
                        if (baseTri < 0 || baseTri >= (int)oc.Current().tris.size()) continue;

                        Vec3 cp = TriPointBary(oc.Current(), baseTri, bu, bv, bw);
                        float outerGap = bodyGap(cp, dummy);
                        float allowed = outerGap - kClearance;
                        float pen = myGap - allowed;
                        if (pen <= 0.01f || allowed >= kBodyFloor) continue;

                        float targetGap = ClampRange(allowed - 0.35f, -3.00f, -0.65f);
                        triTargetGap = std::min(triTargetGap, targetGap);
                        triWorstPen = std::max(triWorstPen, pen);
                        triMarked = true;
                        ++suppressFaceSamples;
                    }
                }
                if (triMarked) {
                    markSuppress(t[0], triTargetGap, triWorstPen);
                    markSuppress(t[1], triTargetGap, triWorstPen);
                    markSuppress(t[2], triTargetGap, triWorstPen);
                    ++suppressFaceTris;
                }
            }
        }

        if (suppressCore > 0) {
            std::vector<float> weight = suppress;
            std::vector<float> targetGap = suppressTargetGap;
            if (edges && !edges->empty()) {
                static const float kFeatherDecay[] = {0.75f, 0.55f, 0.35f};
                for (int ring = 0; ring < 3; ++ring) {
                    std::vector<float> next = weight;
                    std::vector<float> nextTarget = targetGap;
                    for (const EdgeConstraint& e : *edges) {
                        if (e.a >= n || e.b >= n) continue;
                        float wa = weight[e.b] * kFeatherDecay[ring];
                        if (wa > next[e.a]) {
                            next[e.a] = wa;
                            nextTarget[e.a] = std::min(nextTarget[e.a], targetGap[e.b]);
                        }
                        float wb = weight[e.a] * kFeatherDecay[ring];
                        if (wb > next[e.b]) {
                            next[e.b] = wb;
                            nextTarget[e.b] = std::min(nextTarget[e.b], targetGap[e.a]);
                        }
                    }
                    weight.swap(next);
                    targetGap.swap(nextTarget);
                }
            }

            int hidden = 0, feathered = 0;
            for (size_t i = 0; i < n; ++i) {
                float w = Clamp01(weight[i]);
                if (w <= 0.01f) continue;
                Vec3 cp{0,0,0}, nrm{0,0,1};
                float d2 = 0.0f;
                if (!BVHClosestPoint(*bodyBVH, finalVerts[i], cp, nrm, d2)) continue;
                Vec3 target = Add(cp, Scale(nrm, targetGap[i]));
                finalVerts[i] = Add(finalVerts[i], Scale(Sub(target, finalVerts[i]), w));
                if (suppress[i] >= 0.99f) ++hidden;
                else ++feathered;
            }
            HLog("detour: anti-clip-suppress shape='%s' hidden=%d feathered=%d faceTris=%d faceSamples=%d outers=%zu worstPen=%.2f deepestGap=%.2f",
                 shapeName.c_str(), hidden, feathered,
                 suppressFaceTris, suppressFaceSamples, outerColliders->size(),
                 worstSuppressPen, deepestSuppressGap);
        }

        // One final forced cleanup for the exact vertices that would otherwise show
        // up in the residual report. This catches boundary cases where a vertex was
        // only feathered but the current outer surface still leaves no physical room.
        int forcedResidual = 0;
        float forcedDeepestGap = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            for (const OuterCollider& oc : *outerColliders) {
                if (!oc.Base().valid || !oc.Current().valid) continue;
                const float kClearance = clearanceFor(oc.preset);

                Vec3 bcp{0,0,0}, bn{0,0,1}; float bd2 = 0.0f;
                int baseTri = -1; float bu = 0.0f, bv = 0.0f, bw = 0.0f;
                if (!BVHClosestPointEx(oc.Base(), baseVerts[i], bcp, bn, bd2,
                                       &baseTri, &bu, &bv, &bw)) continue;
                if (std::sqrt(bd2) > kOverlap) continue;
                Vec3 dn{};
                if (baseBodyGap(baseVerts[i], dn) >= baseBodyGap(bcp, dn) + kBaseUnderSlack) continue;
                if (baseTri < 0 || baseTri >= (int)oc.Current().tris.size()) continue;

                Vec3 outerCP = TriPointBary(oc.Current(), baseTri, bu, bv, bw);
                float outerGap = bodyGap(outerCP, dn);
                float allowed = outerGap - kClearance;
                if (allowed >= kBodyFloor) continue;
                float pen = bodyGap(finalVerts[i], dn) - allowed;
                if (pen <= 0.01f) continue;

                Vec3 bodyCP{0,0,0}, nrm{0,0,1};
                float d2 = 0.0f;
                if (!BVHClosestPoint(*bodyBVH, finalVerts[i], bodyCP, nrm, d2)) continue;
                float targetGap = ClampRange(allowed - 0.50f, -3.50f, -0.85f);
                finalVerts[i] = Add(bodyCP, Scale(nrm, targetGap));
                forcedDeepestGap = std::min(forcedDeepestGap, targetGap);
                ++forcedResidual;
                break;
            }
        }
        if (forcedResidual > 0)
            HLog("detour: anti-clip-cleanup shape='%s' forced=%d deepestGap=%.2f",
                 shapeName.c_str(), forcedResidual, forcedDeepestGap);
        }  // end if (kEnableSuppress)

        // DIAGNOSTIC per OUTER shape (review #5): where residual clipping remains and
        // whether it's a body-conflict (no room) vs a genuine miss.
        for (const OuterCollider& oc : *outerColliders) {
            if (!oc.Base().valid || !oc.Current().valid) continue;
            const float kClearance = clearanceFor(oc.preset);
            int stillBad = 0, bodyConflict = 0; float worstPen = 0, worstZ = 0, sumZ = 0;
            float sumOuterGap = 0, minOuterGap = 1e9f;   // real bodyâ†”outer gap at conflict pts
            for (size_t i = 0; i < n; ++i) {
                Vec3 bcp{0,0,0}, bn{0,0,1}; float bd2 = 0.0f;
                int baseTri = -1; float bu = 0.0f, bv = 0.0f, bw = 0.0f;
                if (!BVHClosestPointEx(oc.Base(), baseVerts[i], bcp, bn, bd2,
                                       &baseTri, &bu, &bv, &bw)) continue;
                if (std::sqrt(bd2) > kOverlap) continue;
                Vec3 dn{};
                if (baseBodyGap(baseVerts[i], dn) >= baseBodyGap(bcp, dn) + kBaseUnderSlack) continue;
                if (baseTri < 0 || baseTri >= (int)oc.Current().tris.size()) continue;
                Vec3 cp = TriPointBary(oc.Current(), baseTri, bu, bv, bw);
                float outerGap = bodyGap(cp, dn);           // how far the OUTER sits off body
                float allowed = outerGap - kClearance;
                float pen = bodyGap(finalVerts[i], dn) - allowed;
                if (pen > 0.01f) {
                    ++stillBad; sumZ += finalVerts[i].z;
                    sumOuterGap += outerGap; minOuterGap = std::min(minOuterGap, outerGap);
                    if (allowed < 0.06f) ++bodyConflict;   // no room between body and outer
                    if (pen > worstPen) { worstPen = pen; worstZ = finalVerts[i].z; }
                }
            }
            if (stillBad > 0)
                HLog("detour: anti-clip-RESIDUAL shape='%s' outer='%s' stillBad=%d bodyConflict=%d avgZ=%.1f worstZ=%.1f worstPen=%.2f avgOuterGap=%.2f minOuterGap=%.2f",
                     shapeName.c_str(), oc.name.c_str(), stillBad, bodyConflict,
                     sumZ/(float)stillBad, worstZ, worstPen,
                     sumOuterGap/(float)stillBad, minOuterGap);
        }
    }

    const bool finalExactSeamForCompactLoose =
        normPreset == DRAPE_PRESET_LOOSE_DRAPED &&
        n <= 2500 &&
        exactSeamGroups.size() <= 260;
    std::vector<int> exactSeamGroupOf;
    if (finalExactSeamForCompactLoose && !exactSeamGroups.empty()) {
        exactSeamGroupOf.assign(n, -1);
        for (size_t gi = 0; gi < exactSeamGroups.size(); ++gi) {
            for (uint32_t vi : exactSeamGroups[gi]) {
                if (vi < n)
                    exactSeamGroupOf[vi] = (int)gi;
            }
        }
    }

    auto applyFinalSupportCap = [&](int rounds, const char* stageLabel) -> int {
        if (useEdges.empty()) return 0;
        const bool innerLooseSupportCap =
            isLoosePreset && outerColliders && !outerColliders->empty();
        const float kFinalSupportCap = innerLooseSupportCap ? 1.08f : 1.12f;
        auto applySupportDelta = [&](uint32_t v, const Vec3& delta) {
            if (v >= n) return;
            if (!exactSeamGroupOf.empty() && v < exactSeamGroupOf.size()) {
                int gi = exactSeamGroupOf[v];
                if (gi >= 0 && gi < (int)exactSeamGroups.size()) {
                    for (uint32_t member : exactSeamGroups[(size_t)gi]) {
                        if (member < n)
                            finalVerts[member] = Add(finalVerts[member], delta);
                    }
                    return;
                }
            }
            finalVerts[v] = Add(finalVerts[v], delta);
        };
        int finalCapAdjusted = 0;
        for (int it = 0; it < rounds; ++it) {
            int roundAdjusted = 0;
            for (size_t ei = 0; ei < useEdges.size(); ++ei) {
                const auto& e = useEdges[ei];
                if (e.a >= n || e.b >= n) continue;
                if (!exactSeamGroupOf.empty() &&
                    e.a < exactSeamGroupOf.size() &&
                    e.b < exactSeamGroupOf.size() &&
                    exactSeamGroupOf[e.a] >= 0 &&
                    exactSeamGroupOf[e.a] == exactSeamGroupOf[e.b])
                    continue;
                float rest = (ei < supportRestLen.size()) ? supportRestLen[ei] : e.restLength;
                if (rest <= 1.0e-6f) continue;
                Vec3 dir = Sub(finalVerts[e.a], finalVerts[e.b]);
                float len = Len(dir);
                float cap = rest * kFinalSupportCap;
                if (len <= cap || len <= 1.0e-6f) continue;
                float wa = (e.a < invMass.size() && invMass[e.a] > 0.0f) ? 1.0f : 0.0f;
                float wb = (e.b < invMass.size() && invMass[e.b] > 0.0f) ? 1.0f : 0.0f;
                if (wa + wb <= 0.0f) { wa = 0.5f; wb = 0.5f; }
                float wSum = wa + wb;
                Vec3 delta = Scale(dir, ((len - cap) / len) / wSum);
                applySupportDelta(e.a, Scale(delta, -wa));
                applySupportDelta(e.b, Scale(delta, wb));
                ++roundAdjusted;
            }
            finalCapAdjusted += roundAdjusted;
            if (roundAdjusted == 0) break;
        }
        if (stageLabel && finalCapAdjusted > 0)
            HLog("detour: %s shape='%s' adjusted=%d cap=%.2f",
                 stageLabel, shapeName.c_str(), finalCapAdjusted, kFinalSupportCap);
        return finalCapAdjusted;
    };

    auto isFinalRigidVertex = [&](uint32_t v) -> bool {
        if (islandIdOut.empty() || v >= islandIdOut.size()) return false;
        int id = islandIdOut[v];
        return id >= 0 && id < (int)islandRigidOut.size() &&
               islandRigidOut[(size_t)id];
    };

    auto proxCapForPair = [&](const EdgeConstraint& e, size_t ei) -> float {
        float baseRest = (ei < proxRestLen.size()) ? proxRestLen[ei] : e.restLength;
        if (baseRest <= 1.0e-6f) return 0.0f;
        float supportRest = baseRest;
        if (e.a < supportVerts.size() && e.b < supportVerts.size())
            supportRest = std::max(supportRest, Len(Sub(supportVerts[e.a], supportVerts[e.b])));
        if (ei < proxIsBoundarySeam.size() && proxIsBoundarySeam[ei])
            return std::max(baseRest * 1.03f + 0.018f,
                            supportRest * 1.02f + 0.012f);

        const float capMul =
            (normPreset == DRAPE_PRESET_LOOSE_DRAPED) ? 1.16f :
            (normPreset == DRAPE_PRESET_TUBULAR) ? 1.10f : 1.12f;
        const float capSlack =
            (normPreset == DRAPE_PRESET_LOOSE_DRAPED) ? 0.055f :
            (normPreset == DRAPE_PRESET_TUBULAR) ? 0.030f : 0.040f;
        const float supportSlack =
            (normPreset == DRAPE_PRESET_LOOSE_DRAPED) ? 0.035f : 0.020f;

        return std::max(baseRest * capMul + capSlack,
                        supportRest * 1.04f + supportSlack);
    };

    auto logProxSeamState = [&](const char* stageLabel) {
        if (!stageLabel || proxEdges.empty()) return;
        float worstRatio = 1.0f;
        float worstOpen = 0.0f;
        int overCap = 0;
        for (size_t ei = 0; ei < proxEdges.size(); ++ei) {
            const auto& e = proxEdges[ei];
            if (e.a >= n || e.b >= n) continue;
            float cap = proxCapForPair(e, ei);
            if (cap <= 1.0e-6f) continue;
            float len = Len(Sub(finalVerts[e.a], finalVerts[e.b]));
            worstRatio = std::max(worstRatio, len / cap);
            if (len > cap) {
                ++overCap;
                worstOpen = std::max(worstOpen, len - cap);
            }
        }
        if (overCap > 0)
            HLog("detour: prox-seam-state shape='%s' stage='%s' overCap=%d/%zu worstRatio=%.2f worstOpen=%.3f",
                 shapeName.c_str(), stageLabel, overCap, proxEdges.size(),
                 worstRatio, worstOpen);
    };

    auto bodySafeProxMove = [&](uint32_t, const Vec3& proposed) -> bool {
        if (!bodyBVH || !bodyBVH->valid) return true;
        float gap = 0.0f;
        if (!BVHSignedGap(*bodyBVH, proposed, gap, nullptr)) return true;
        return gap >= -0.005f;
    };

    auto applyFinalSeamVectorPairs = [&](int rounds, const char* stageLabel) -> int {
        if (seamVectorPairs.empty()) return 0;
        const float kEps = 0.006f;
        const float kMaxMove = 0.085f;
        auto applySeamDelta = [&](uint32_t v, const Vec3& delta) {
            if (v >= n) return;
            if (!exactSeamGroupOf.empty() && v < exactSeamGroupOf.size()) {
                int gi = exactSeamGroupOf[v];
                if (gi >= 0 && gi < (int)exactSeamGroups.size()) {
                    for (uint32_t member : exactSeamGroups[(size_t)gi]) {
                        if (member < n)
                            finalVerts[member] = Add(finalVerts[member], delta);
                    }
                    return;
                }
            }
            finalVerts[v] = Add(finalVerts[v], delta);
        };

        int adjusted = 0;
        float worstBefore = 0.0f;
        float worstAfter = 0.0f;
        for (int round = 0; round < rounds; ++round) {
            int roundAdjusted = 0;
            for (const SeamVectorPair& sv : seamVectorPairs) {
                if (!sv.boundary || sv.a >= n || sv.b >= n) continue;
                if (!exactSeamGroupOf.empty() &&
                    sv.a < exactSeamGroupOf.size() &&
                    sv.b < exactSeamGroupOf.size() &&
                    exactSeamGroupOf[sv.a] >= 0 &&
                    exactSeamGroupOf[sv.a] == exactSeamGroupOf[sv.b])
                    continue;

                Vec3 curOff = Sub(finalVerts[sv.b], finalVerts[sv.a]);
                Vec3 err = Sub(curOff, sv.supportOffset);
                float errLen = Len(err);
                if (round == 0)
                    worstBefore = std::max(worstBefore, errLen);
                if (errLen <= kEps) continue;

                bool rigidA = isFinalRigidVertex(sv.a);
                bool rigidB = isFinalRigidVertex(sv.b);
                if (rigidA && rigidB) continue;

                float wa = rigidA ? 0.0f : ((sv.a < invMass.size() && invMass[sv.a] > 0.0f) ? 1.0f : 0.0f);
                float wb = rigidB ? 0.0f : ((sv.b < invMass.size() && invMass[sv.b] > 0.0f) ? 1.0f : 0.0f);
                if (wa + wb <= 1.0e-6f) {
                    if (!rigidA) wa = 1.0f;
                    if (!rigidB) wb = 1.0f;
                }
                float wSum = wa + wb;
                if (wSum <= 1.0e-6f) continue;

                Vec3 delta = Scale(err, 0.65f / wSum);
                float deltaLen = Len(delta);
                if (deltaLen > kMaxMove && deltaLen > 1.0e-6f)
                    delta = Scale(delta, kMaxMove / deltaLen);

                Vec3 nextA = (wa > 0.0f) ? Add(finalVerts[sv.a], Scale(delta, wa)) : finalVerts[sv.a];
                Vec3 nextB = (wb > 0.0f) ? Sub(finalVerts[sv.b], Scale(delta, wb)) : finalVerts[sv.b];
                bool canA = wa <= 0.0f || bodySafeProxMove(sv.a, nextA);
                bool canB = wb <= 0.0f || bodySafeProxMove(sv.b, nextB);
                if (!canA || !canB) continue;

                applySeamDelta(sv.a, Scale(delta, wa));
                applySeamDelta(sv.b, Scale(delta, -wb));
                ++roundAdjusted;
            }
            adjusted += roundAdjusted;
            if (roundAdjusted == 0) break;
        }

        if (adjusted > 0) {
            for (const SeamVectorPair& sv : seamVectorPairs) {
                if (!sv.boundary || sv.a >= n || sv.b >= n) continue;
                Vec3 curOff = Sub(finalVerts[sv.b], finalVerts[sv.a]);
                worstAfter = std::max(worstAfter, Len(Sub(curOff, sv.supportOffset)));
            }
            if (stageLabel)
                HLog("detour: %s shape='%s' adjusted=%d worstBefore=%.4f worstAfter=%.4f pairs=%zu",
                     stageLabel, shapeName.c_str(), adjusted, worstBefore,
                     worstAfter, seamVectorPairs.size());
        }
        return adjusted;
    };

    auto applyFinalProxSeamCap = [&](int rounds, const char* stageLabel) -> int {
        if (proxEdges.empty()) return 0;
        const float kMaxMove =
            (normPreset == DRAPE_PRESET_LOOSE_DRAPED) ? 0.080f :
            (normPreset == DRAPE_PRESET_TUBULAR) ? 0.060f : 0.070f;
        int totalAdjusted = 0;
        float worstBefore = 1.0f;
        float worstAfter = 1.0f;
        for (int round = 0; round < rounds; ++round) {
            int roundAdjusted = 0;
            for (size_t ei = 0; ei < proxEdges.size(); ++ei) {
                const auto& e = proxEdges[ei];
                if (e.a >= n || e.b >= n) continue;
                float cap = proxCapForPair(e, ei);
                if (cap <= 1.0e-6f) continue;
                Vec3 dir = Sub(finalVerts[e.a], finalVerts[e.b]);
                float len = Len(dir);
                if (len <= cap || len <= 1.0e-6f) continue;

                worstBefore = std::max(worstBefore, len / cap);
                bool rigidA = isFinalRigidVertex(e.a);
                bool rigidB = isFinalRigidVertex(e.b);
                if (rigidA && rigidB) continue;

                float wa = rigidA ? 0.0f : ((e.a < invMass.size() && invMass[e.a] > 0.0f) ? 1.0f : 0.0f);
                float wb = rigidB ? 0.0f : ((e.b < invMass.size() && invMass[e.b] > 0.0f) ? 1.0f : 0.0f);
                if (wa + wb <= 1.0e-6f) {
                    if (!rigidA) wa = 1.0f;
                    if (!rigidB) wb = 1.0f;
                }
                float wSum = wa + wb;
                if (wSum <= 1.0e-6f) continue;

                float excess = len - cap;
                Vec3 delta = Scale(dir, (excess / len) / wSum);
                float deltaLen = Len(delta);
                if (deltaLen > kMaxMove && deltaLen > 1.0e-6f)
                    delta = Scale(delta, kMaxMove / deltaLen);

                Vec3 nextA = Sub(finalVerts[e.a], Scale(delta, wa));
                Vec3 nextB = Add(finalVerts[e.b], Scale(delta, wb));
                bool canA = wa <= 0.0f || bodySafeProxMove(e.a, nextA);
                bool canB = wb <= 0.0f || bodySafeProxMove(e.b, nextB);
                if (!canA || !canB) {
                    if (!canA) wa = 0.0f;
                    if (!canB) wb = 0.0f;
                    wSum = wa + wb;
                    if (wSum <= 1.0e-6f) continue;
                    delta = Scale(dir, (excess / len) / wSum);
                    deltaLen = Len(delta);
                    if (deltaLen > kMaxMove && deltaLen > 1.0e-6f)
                        delta = Scale(delta, kMaxMove / deltaLen);
                    nextA = (wa > 0.0f) ? Sub(finalVerts[e.a], Scale(delta, wa)) : finalVerts[e.a];
                    nextB = (wb > 0.0f) ? Add(finalVerts[e.b], Scale(delta, wb)) : finalVerts[e.b];
                    if ((wa > 0.0f && !bodySafeProxMove(e.a, nextA)) ||
                        (wb > 0.0f && !bodySafeProxMove(e.b, nextB)))
                        continue;
                }

                finalVerts[e.a] = nextA;
                finalVerts[e.b] = nextB;
                ++roundAdjusted;
            }
            totalAdjusted += roundAdjusted;
            if (roundAdjusted == 0) break;
        }

        if (totalAdjusted > 0) {
            for (size_t ei = 0; ei < proxEdges.size(); ++ei) {
                const auto& e = proxEdges[ei];
                if (e.a >= n || e.b >= n) continue;
                float cap = proxCapForPair(e, ei);
                if (cap <= 1.0e-6f) continue;
                float len = Len(Sub(finalVerts[e.a], finalVerts[e.b]));
                worstAfter = std::max(worstAfter, len / cap);
            }
            if (stageLabel)
                HLog("detour: %s shape='%s' adjusted=%d worstBefore=%.2f worstAfter=%.2f pairs=%zu",
                     stageLabel, shapeName.c_str(), totalAdjusted,
                     worstBefore, worstAfter, proxEdges.size());
        }
        return totalAdjusted;
    };

    auto exactSeamSpan = [&](const std::vector<uint32_t>& group) -> float {
        float worst = 0.0f;
        for (size_t ai = 0; ai < group.size(); ++ai) {
            uint32_t a = group[ai];
            if (a >= n) continue;
            for (size_t bi = ai + 1; bi < group.size(); ++bi) {
                uint32_t b = group[bi];
                if (b >= n) continue;
                worst = std::max(worst, Len(Sub(finalVerts[a], finalVerts[b])));
            }
        }
        return worst;
    };

    auto applyFinalExactSeamWeld = [&](const char* stageLabel) -> int {
        if (exactSeamGroups.empty()) return 0;
        if (normPreset != DRAPE_PRESET_FITTED_CLOTH &&
            normPreset != DRAPE_PRESET_TUBULAR &&
            !finalExactSeamForCompactLoose)
            return 0;
        int moved = 0;
        int groupsAdjusted = 0;
        float worstBefore = 0.0f;
        float worstAfter = 0.0f;
        const float kMoveEps = 0.0005f;
        const float kReportEps = 0.005f;
        const float kBodyGap = 0.01f;

        for (const auto& group : exactSeamGroups) {
            if (group.size() < 2) continue;
            float before = exactSeamSpan(group);
            worstBefore = std::max(worstBefore, before);
            if (before > kReportEps)
                ++groupsAdjusted;

            Vec3 avg{0.0f, 0.0f, 0.0f};
            int count = 0;
            for (uint32_t vi : group) {
                if (vi >= n) continue;
                avg = Add(avg, finalVerts[vi]);
                ++count;
            }
            if (count < 2) continue;
            avg = Scale(avg, 1.0f / (float)count);

            if (bodyBVH && bodyBVH->valid) {
                float gap = 0.0f;
                Vec3 nrm{0.0f, 0.0f, 1.0f};
                if (BVHSignedGap(*bodyBVH, avg, gap, &nrm) && gap < kBodyGap)
                    avg = Add(avg, Scale(nrm, kBodyGap - gap));
            }

            for (uint32_t vi : group) {
                if (vi >= n) continue;
                if (Len(Sub(finalVerts[vi], avg)) > kMoveEps)
                    ++moved;
                finalVerts[vi] = avg;
            }
            worstAfter = std::max(worstAfter, exactSeamSpan(group));
        }

        if (moved > 0 && stageLabel)
            HLog("detour: %s shape='%s' groups=%d/%zu moved=%d worstBefore=%.4f worstAfter=%.4f",
                 stageLabel, shapeName.c_str(), groupsAdjusted,
                 exactSeamGroups.size(), moved, worstBefore, worstAfter);
        return moved;
    };

    // Outermost loose garments must keep their base body volume. If the cloak
    // collapses toward/through the morphed body, inner layers have no valid place
    // to go: moving them inward hits the body, and masking them creates holes.
    // Enforce this upstream on the outer loose layer, using both vertices and face
    // samples so large cloak triangles cannot chord through the body between clear
    // vertices. Support caps run between rounds to prevent the old tear regression.
    const bool preserveOuterBodyVolume =
        isLoosePreset ||
        normPreset == DRAPE_PRESET_BELT ||
        normPreset == DRAPE_PRESET_FLEXIBLE_ARMOR ||
        normPreset == DRAPE_PRESET_RIGID_ARMOR;
    const bool intermediateLooseVolume =
        isLoosePreset &&
        outerColliders && !outerColliders->empty() &&
        n <= 600;
    const bool intermediateRigidAccessoryVolume =
        normPreset == DRAPE_PRESET_RIGID_ACCESSORY &&
        outerColliders && !outerColliders->empty() &&
        n <= 800;
    if ((preserveOuterBodyVolume || intermediateRigidAccessoryVolume) &&
        ((!outerColliders || outerColliders->empty()) || intermediateLooseVolume || intermediateRigidAccessoryVolume) &&
        bodyBVH && bodyBVH->valid &&
        g_refBodyBVH.valid) {
        const BodyBVH& baseBody = g_refBodyBVH;
        const bool volumeForBelt = normPreset == DRAPE_PRESET_BELT;
        const bool volumeForRigidOuter =
            normPreset == DRAPE_PRESET_FLEXIBLE_ARMOR ||
            normPreset == DRAPE_PRESET_RIGID_ARMOR;
        const bool volumeForIntermediateLoose = intermediateLooseVolume;
        const bool volumeForIntermediateRigidAccessory = intermediateRigidAccessoryVolume;
        const float kBaseGapMin = volumeForBelt ? 0.08f : (volumeForRigidOuter ? 0.04f : (volumeForIntermediateRigidAccessory ? 0.02f : (volumeForIntermediateLoose ? 0.16f : 0.35f)));
        const float kTargetSlack = volumeForBelt ? 0.04f : (volumeForRigidOuter ? 0.02f : (volumeForIntermediateRigidAccessory ? 0.02f : (volumeForIntermediateLoose ? 0.06f : 0.10f)));
        const float kTargetMin = volumeForBelt ? 0.42f : (volumeForRigidOuter ? 0.48f : (volumeForIntermediateRigidAccessory ? 0.36f : (volumeForIntermediateLoose ? 0.58f : std::max(0.85f, params.bodyClearance))));
        const float kTargetMax = volumeForBelt ? 1.10f : (volumeForRigidOuter ? 1.45f : (volumeForIntermediateRigidAccessory ? 0.95f : (volumeForIntermediateLoose ? 1.65f : 2.25f)));
        const float kVertexStep = volumeForBelt ? 0.24f : (volumeForRigidOuter ? 0.20f : (volumeForIntermediateRigidAccessory ? 0.16f : (volumeForIntermediateLoose ? 0.24f : 0.42f)));
        const float kFaceStep = volumeForBelt ? 0.16f : (volumeForRigidOuter ? 0.12f : (volumeForIntermediateRigidAccessory ? 0.08f : (volumeForIntermediateLoose ? 0.14f : 0.28f)));
        const float kStrength = volumeForBelt ? 0.55f : (volumeForRigidOuter ? 0.50f : (volumeForIntermediateRigidAccessory ? 0.45f : (volumeForIntermediateLoose ? 0.55f : 0.65f)));
        const int kVolumeRounds = volumeForBelt ? 5 : (volumeForRigidOuter ? 6 : (volumeForIntermediateRigidAccessory ? 5 : (volumeForIntermediateLoose ? 6 : 8)));

        auto isRigidVertex2 = [&](size_t i) -> bool {
            if (volumeForRigidOuter || volumeForIntermediateRigidAccessory) return false;
            if (islandIdOut.empty() || i >= islandIdOut.size()) return false;
            int id = islandIdOut[i];
            return id >= 0 && id < (int)islandRigidOut.size() &&
                   islandRigidOut[(size_t)id];
        };

        auto targetGapForBasePoint = [&](const Vec3& baseP, float& targetGap) -> bool {
            float baseGap = 0.0f;
            Vec3 nrm{0.0f, 0.0f, 1.0f};
            if (!BVHSignedGap(baseBody, baseP, baseGap, &nrm)) return false;
            if (baseGap < kBaseGapMin) return false;
            targetGap = ClampRange(std::max(kTargetMin, baseGap - kTargetSlack),
                                   kTargetMin, kTargetMax);
            return true;
        };

        auto pushVertexToGap = [&](size_t i, float targetGap,
                                   float maxStep, float strength) -> bool {
            if (i >= n || isRigidVertex2(i)) return false;
            float curGap = 0.0f;
            Vec3 nrm{0.0f, 0.0f, 1.0f};
            if (!BVHSignedGap(*bodyBVH, finalVerts[i], curGap, &nrm)) return false;
            float need = targetGap - curGap;
            if (need <= 0.015f) return false;
            float step = std::min(need * strength, maxStep);
            if (step <= 0.0f) return false;
            finalVerts[i] = Add(finalVerts[i], Scale(nrm, step));
            return true;
        };

        struct GapSample { float a, b, c; };
        static const GapSample kGapSamples[] = {
            {1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f},
            {0.5f, 0.5f, 0.0f},
            {0.5f, 0.0f, 0.5f},
            {0.0f, 0.5f, 0.5f},
        };
        auto sampleTri = [](const std::vector<Vec3>& verts,
                            const std::array<uint16_t, 3>& t,
                            const GapSample& s) -> Vec3 {
            return Add(Scale(verts[t[0]], s.a),
                       Add(Scale(verts[t[1]], s.b), Scale(verts[t[2]], s.c)));
        };

        int totalVolumeAdjusted = 0;
        int totalFaceAdjusted = 0;
        for (int round = 0; round < kVolumeRounds; ++round) {
            int roundAdjusted = 0;
            for (size_t i = 0; i < n; ++i) {
                float targetGap = 0.0f;
                if (!targetGapForBasePoint(baseVerts[i], targetGap)) continue;
                if (pushVertexToGap(i, targetGap, kVertexStep, kStrength))
                    ++roundAdjusted;
            }

            if (tris && !tris->empty()) {
                for (const auto& t : *tris) {
                    if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
                    for (const GapSample& s : kGapSamples) {
                        Vec3 baseP = sampleTri(baseVerts, t, s);
                        float targetGap = 0.0f;
                        if (!targetGapForBasePoint(baseP, targetGap)) continue;

                        Vec3 curP = sampleTri(finalVerts, t, s);
                        float curGap = 0.0f;
                        Vec3 nrm{0.0f, 0.0f, 1.0f};
                        if (!BVHSignedGap(*bodyBVH, curP, curGap, &nrm)) continue;
                        float need = targetGap - curGap;
                        if (need <= 0.015f) continue;
                        float step = std::min(need * 0.45f, kFaceStep);
                        if (step <= 0.0f) continue;

                        const float ws[3] = {s.a, s.b, s.c};
                        const uint16_t vi[3] = {t[0], t[1], t[2]};
                        for (int k = 0; k < 3; ++k) {
                            if (ws[k] <= 0.0f || isRigidVertex2(vi[k])) continue;
                            finalVerts[vi[k]] = Add(finalVerts[vi[k]],
                                                    Scale(nrm, step * ws[k]));
                        }
                        ++roundAdjusted;
                        ++totalFaceAdjusted;
                    }
                }
            }

            if (roundAdjusted == 0) break;
            totalVolumeAdjusted += roundAdjusted;
            applyFinalSupportCap(16, nullptr);
        }
        if (totalVolumeAdjusted > 0)
            HLog("detour: loose-outer-body-volume shape='%s' adjusted=%d faceSamples=%d target=[%.2f,%.2f]",
                 shapeName.c_str(), totalVolumeAdjusted, totalFaceAdjusted,
                 kTargetMin, kTargetMax);
    }

    auto applyOuterAccessoryInnerClearance = [&](const char* stageLabel) -> int {
        const bool structuredOuter =
            normPreset == DRAPE_PRESET_BELT ||
            normPreset == DRAPE_PRESET_RIGID_ACCESSORY ||
            normPreset == DRAPE_PRESET_FLEXIBLE_ARMOR ||
            normPreset == DRAPE_PRESET_RIGID_ARMOR;
        const bool intermediateOuterLayer =
            (normPreset == DRAPE_PRESET_LOOSE_DRAPED ||
             normPreset == DRAPE_PRESET_FITTED_CLOTH) &&
            outerColliders && !outerColliders->empty();
        if (!(structuredOuter || intermediateOuterLayer) ||
            !innerColliders || innerColliders->empty() ||
            !bodyBVH || !bodyBVH->valid ||
            !g_refBodyBVH.valid)
            return 0;

        const BodyBVH& baseBody = g_refBodyBVH;
        const float kOverlap = (normPreset == DRAPE_PRESET_BELT) ? 3.25f : (intermediateOuterLayer ? 3.00f : 2.75f);
        const float kOverlap2 = kOverlap * kOverlap;
        const float kBaseAboveSlack = 0.04f;
        const float kMinViolation = 0.012f;
        const float kStrength = intermediateOuterLayer ? 0.44f : ((normPreset == DRAPE_PRESET_BELT) ? 0.66f : 0.56f);
        const float kVertexStep = intermediateOuterLayer ? 0.075f : ((normPreset == DRAPE_PRESET_BELT) ? 0.105f : 0.075f);
        const float kFaceStep = intermediateOuterLayer ? 0.050f : ((normPreset == DRAPE_PRESET_BELT) ? 0.075f : 0.055f);
        const int kRounds = intermediateOuterLayer ? 5 : ((normPreset == DRAPE_PRESET_BELT) ? 8 : 5);

        auto innerMarginFor = [&](int innerPreset) -> float {
            if (intermediateOuterLayer) {
                switch (NormalizePreset(innerPreset)) {
                case DRAPE_PRESET_TUBULAR:         return 0.15f;
                case DRAPE_PRESET_FITTED_CLOTH:    return 0.14f;
                case DRAPE_PRESET_LOOSE_DRAPED:    return 0.12f;
                case DRAPE_PRESET_FLEXIBLE_ARMOR:  return 0.12f;
                case DRAPE_PRESET_RIGID_ARMOR:     return 0.12f;
                case DRAPE_PRESET_BELT:            return 0.12f;
                default:                           return 0.13f;
                }
            }
            switch (NormalizePreset(innerPreset)) {
            case DRAPE_PRESET_TUBULAR:         return 0.22f;
            case DRAPE_PRESET_FITTED_CLOTH:    return 0.20f;
            case DRAPE_PRESET_LOOSE_DRAPED:    return 0.18f;
            case DRAPE_PRESET_FLEXIBLE_ARMOR:  return 0.16f;
            case DRAPE_PRESET_RIGID_ARMOR:     return 0.14f;
            case DRAPE_PRESET_BELT:            return 0.14f;
            default:                           return 0.18f;
            }
        };
        auto signedGap3 = [](const BodyBVH& bvh, const Vec3& p, float& gap, Vec3* nrm) -> bool {
            Vec3 tmp{0.0f, 0.0f, 1.0f};
            bool ok = BVHSignedGap(bvh, p, gap, &tmp);
            if (nrm) *nrm = tmp;
            return ok;
        };

        int totalAdjusted = 0;
        int totalFaceSamples = 0;
        for (int round = 0; round < kRounds; ++round) {
            int roundAdjusted = 0;
            for (size_t i = 0; i < n; ++i) {
                float myBaseGap = 0.0f;
                if (!signedGap3(baseBody, baseVerts[i], myBaseGap, nullptr)) continue;
                float myGap = 0.0f;
                Vec3 myNrm{0.0f, 0.0f, 1.0f};
                if (!signedGap3(*bodyBVH, finalVerts[i], myGap, &myNrm)) continue;

                float targetGap = myGap;
                for (const OuterCollider& ic : *innerColliders) {
                    if (!ic.Base().valid || !ic.Current().valid) continue;
                    Vec3 bcp{0.0f, 0.0f, 0.0f}, bnrm{0.0f, 0.0f, 1.0f};
                    float bd2 = 0.0f;
                    int baseTri = -1;
                    float bu = 0.0f, bv = 0.0f, bw = 0.0f;
                    if (!BVHClosestPointEx(ic.Base(), baseVerts[i], bcp, bnrm, bd2,
                                            &baseTri, &bu, &bv, &bw)) continue;
                    if (bd2 > kOverlap2) continue;
                    if (baseTri < 0 || baseTri >= (int)ic.Current().tris.size()) continue;

                    float innerBaseGap = 0.0f;
                    if (!signedGap3(baseBody, bcp, innerBaseGap, nullptr)) continue;
                    if (myBaseGap + kBaseAboveSlack < innerBaseGap) continue;

                    Vec3 innerCur = TriPointBary(ic.Current(), baseTri, bu, bv, bw);
                    float innerGap = 0.0f;
                    if (!signedGap3(*bodyBVH, innerCur, innerGap, nullptr)) continue;
                    targetGap = std::max(targetGap, innerGap + innerMarginFor(ic.preset));
                }

                float need = targetGap - myGap;
                if (need <= kMinViolation) continue;
                float step = std::min(need * kStrength, kVertexStep);
                if (step <= 0.0f) continue;
                finalVerts[i] = Add(finalVerts[i], Scale(myNrm, step));
                ++roundAdjusted;
            }

            if (tris && !tris->empty()) {
                for (const auto& t : *tris) {
                    if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
                    for (const TriangleSample& s : kTriangleSamples) {
                        Vec3 baseP = SampleTrianglePoint(baseVerts, t, s);
                        float myBaseGap = 0.0f;
                        if (!signedGap3(baseBody, baseP, myBaseGap, nullptr)) continue;
                        Vec3 curP = SampleTrianglePoint(finalVerts, t, s);
                        float myGap = 0.0f;
                        Vec3 myNrm{0.0f, 0.0f, 1.0f};
                        if (!signedGap3(*bodyBVH, curP, myGap, &myNrm)) continue;

                        float targetGap = myGap;
                        for (const OuterCollider& ic : *innerColliders) {
                            if (!ic.Base().valid || !ic.Current().valid) continue;
                            Vec3 bcp{0.0f, 0.0f, 0.0f}, bnrm{0.0f, 0.0f, 1.0f};
                            float bd2 = 0.0f;
                            int baseTri = -1;
                            float bu = 0.0f, bv = 0.0f, bw = 0.0f;
                            if (!BVHClosestPointEx(ic.Base(), baseP, bcp, bnrm, bd2,
                                                    &baseTri, &bu, &bv, &bw)) continue;
                            if (bd2 > kOverlap2) continue;
                            if (baseTri < 0 || baseTri >= (int)ic.Current().tris.size()) continue;

                            float innerBaseGap = 0.0f;
                            if (!signedGap3(baseBody, bcp, innerBaseGap, nullptr)) continue;
                            if (myBaseGap + kBaseAboveSlack < innerBaseGap) continue;

                            Vec3 innerCur = TriPointBary(ic.Current(), baseTri, bu, bv, bw);
                            float innerGap = 0.0f;
                            if (!signedGap3(*bodyBVH, innerCur, innerGap, nullptr)) continue;
                            targetGap = std::max(targetGap, innerGap + innerMarginFor(ic.preset));
                        }

                        float need = targetGap - myGap;
                        if (need <= kMinViolation) continue;
                        float step = std::min(need * (kStrength * 0.78f), kFaceStep);
                        if (step <= 0.0f) continue;

                        const float ws[3] = {s.a, s.b, s.c};
                        const uint16_t vi[3] = {t[0], t[1], t[2]};
                        float sumSq = ws[0] * ws[0] + ws[1] * ws[1] + ws[2] * ws[2];
                        if (sumSq <= 1.0e-6f) continue;
                        for (int k = 0; k < 3; ++k) {
                            if (ws[k] <= 0.0f) continue;
                            finalVerts[vi[k]] = Add(finalVerts[vi[k]],
                                                    Scale(myNrm, step * ws[k] / sumSq));
                        }
                        ++roundAdjusted;
                        ++totalFaceSamples;
                    }
                }
            }

            if (roundAdjusted == 0) break;
            totalAdjusted += roundAdjusted;
            applyFinalSupportCap(14, nullptr);
        }

        if (totalAdjusted > 0)
            HLog("detour: outer-layer-inner-clearance shape='%s' stage='%s' adjusted=%d faceSamples=%d inners=%zu mode='%s'",
                 shapeName.c_str(), stageLabel ? stageLabel : "",
                 totalAdjusted, totalFaceSamples, innerColliders->size(),
                 intermediateOuterLayer ? "intermediate" : "structured");
        return totalAdjusted;
    };

    // Final feasible layer tuck. This is deliberately narrower than the legacy
    // anti-clip block: it only handles positions that can be solved while keeping
    // the vertex outside the body, moves in small bounded steps, and applies the
    // support cap between rounds. No masking, no collapse, no no-room forcing.
    const bool fittedLayerTuck =
        normPreset == DRAPE_PRESET_FITTED_CLOTH;
    const bool tubularLayerTuck =
        normPreset == DRAPE_PRESET_TUBULAR;
    auto applyFinalLayerTuck = [&](int roundOverride,
                                   bool postCapPass,
                                   const char* stageLabel) -> int {
        if (!((params.verletCloth || fittedLayerTuck || tubularLayerTuck) &&
              outerColliders && !outerColliders->empty() &&
              bodyBVH && bodyBVH->valid))
            return 0;

        const BodyBVH* baseBodyBVH = g_refBodyBVH.valid ? &g_refBodyBVH : bodyBVH;
        const float kOverlap2 = 4.25f * 4.25f;
        const float kFaceOverlap2 = 5.5f * 5.5f;
        const float kBaseUnderSlack = 0.10f;
        const float kBodyFloor = tubularLayerTuck ? 0.06f : (fittedLayerTuck ? 0.08f : 0.06f);
        // When the outer garment leaves no room at kBodyFloor (its own surface dips
        // close to or below the body there), Pants is never directly visible under a
        // covering outer layer, so let it embed toward the body instead of poking past
        // the outer. applyFinalBodyClearance still softens anything that goes too deep.
        const float kNoRoomFloor = tubularLayerTuck ? -0.50f : kBodyFloor;
        const float kMoveFloor = tubularLayerTuck ? kNoRoomFloor : kBodyFloor;
        const bool looseLayerTuck = isLoosePreset;
        const float kMoveStrength = tubularLayerTuck ? 0.76f : (fittedLayerTuck ? 0.62f : (intermediateLooseVolume ? 0.70f : (looseLayerTuck ? 0.65f : 0.55f)));
        const float kMaxStep = tubularLayerTuck ? 0.25f : (fittedLayerTuck ? 0.16f : (intermediateLooseVolume ? 0.24f : (looseLayerTuck ? 0.22f : 0.18f)));
        const float kFaceMoveStrength = tubularLayerTuck ? 0.82f : (fittedLayerTuck ? 0.54f : 0.50f);
        const float kFaceMaxStep = tubularLayerTuck ? 0.22f : (fittedLayerTuck ? 0.12f : 0.10f);
        const int defaultRounds = tubularLayerTuck ? 20 : (fittedLayerTuck ? 6 : (intermediateLooseVolume ? 8 : (looseLayerTuck ? 10 : 4)));
        const int kRounds = roundOverride > 0 ? roundOverride : defaultRounds;

        auto layerMarginFor = [&](int outerPreset) -> float {
            switch (NormalizePreset(outerPreset)) {
            case DRAPE_PRESET_LOOSE_DRAPED:    return tubularLayerTuck ? 0.34f : 0.16f;
            case DRAPE_PRESET_FLEXIBLE_ARMOR:  return 0.34f;
            case DRAPE_PRESET_RIGID_ARMOR:     return 0.40f;
            case DRAPE_PRESET_BELT:            return 0.32f;
            case DRAPE_PRESET_RIGID_ACCESSORY: return 0.28f;
            default:                           return 0.22f;
            }
        };
        auto signedGap2 = [](const BodyBVH& bvh, const Vec3& p, float& gap, Vec3* nrm) -> bool {
            Vec3 tmp{0.0f, 0.0f, 1.0f};
            bool ok = BVHSignedGap(bvh, p, gap, &tmp);
            if (nrm) *nrm = tmp;
            return ok;
        };
        auto isRigidVertex3 = [&](size_t i) -> bool {
            if (looseLayerTuck) return false;
            if (islandIdOut.empty() || i >= islandIdOut.size()) return false;
            int id = islandIdOut[i];
            return id >= 0 && id < (int)islandRigidOut.size() &&
                   islandRigidOut[(size_t)id];
        };

        int totalTucked = 0;
        int totalFaceTucked = 0;
        int noRoomSkipped = 0;
        for (int round = 0; round < kRounds; ++round) {
            int roundTucked = 0;
            for (size_t i = 0; i < n; ++i) {
                if (isRigidVertex3(i)) continue;
                float myBaseGap = 0.0f;
                if (!looseLayerTuck) {
                    if (!signedGap2(*baseBodyBVH, baseVerts[i], myBaseGap, nullptr)) continue;
                }
                float myGap = 0.0f;
                Vec3 myNrm{0.0f, 0.0f, 1.0f};
                if (!signedGap2(*bodyBVH, finalVerts[i], myGap, &myNrm)) continue;

                float targetGap = myGap;
                for (const OuterCollider& oc : *outerColliders) {
                    if (!oc.Base().valid || !oc.Current().valid) continue;
                    Vec3 bcp{0.0f, 0.0f, 0.0f}, bnrm{0.0f, 0.0f, 1.0f};
                    float bd2 = 0.0f;
                    int baseTri = -1;
                    float bu = 0.0f, bv = 0.0f, bw = 0.0f;
                    if (!BVHClosestPointEx(oc.Base(), baseVerts[i], bcp, bnrm, bd2,
                                            &baseTri, &bu, &bv, &bw)) continue;
                    if (bd2 > (looseLayerTuck ? kFaceOverlap2 : kOverlap2)) continue;
                    if (baseTri < 0 || baseTri >= (int)oc.Current().tris.size()) continue;

                    Vec3 outerCur = TriPointBary(oc.Current(), baseTri, bu, bv, bw);
                    float outerGap = 0.0f;
                    if (!signedGap2(*bodyBVH, outerCur, outerGap, nullptr)) continue;
                    if (looseLayerTuck) {
                        // current-morph guard: skip if outer face is significantly below us
                        if (outerGap + 0.15f <= myGap) continue;
                    } else {
                        float outerBaseGap = 0.0f;
                        if (!signedGap2(*baseBodyBVH, bcp, outerBaseGap, nullptr)) continue;
                        if (myBaseGap >= outerBaseGap + kBaseUnderSlack) continue;
                    }
                    float allowed = outerGap - layerMarginFor(oc.preset);
                    if (allowed < kBodyFloor) {
                        if (round == 0 && myGap > allowed + 0.01f) ++noRoomSkipped;
                        if (tubularLayerTuck) {
                            float tightTarget = std::max(allowed, kNoRoomFloor);
                            if (myGap > tightTarget + 0.01f)
                                targetGap = std::min(targetGap, tightTarget);
                        } else if (fittedLayerTuck && myGap > kBodyFloor + 0.01f) {
                            targetGap = std::min(targetGap, kBodyFloor);
                        }
                        continue;
                    }
                    if (myGap > allowed + 0.01f)
                        targetGap = std::min(targetGap, allowed);
                }

                if (targetGap >= myGap - 0.01f) continue;
                float step = std::min((myGap - targetGap) * kMoveStrength, kMaxStep);
                // Hard per-round cap on current position, not just intended step: surface
                // curvature can make a step move the gap further than the linear estimate,
                // which would let repeated rounds overshoot kMoveFloor. Recomputed from the
                // CURRENT gap each round, so it bounds the actual position, not just intent.
                step = std::min(step, myGap - kMoveFloor);
                if (step <= 0.0f) continue;
                finalVerts[i] = Add(finalVerts[i], Scale(myNrm, -step));
                ++roundTucked;
            }
            if (tris && !tris->empty()) {
                int roundFaceTucked = 0;
                for (const auto& t : *tris) {
                    if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
                    for (const TriangleSample& s : kTriangleSamples) {
                        Vec3 baseP = SampleTrianglePoint(baseVerts, t, s);
                        Vec3 curP = SampleTrianglePoint(finalVerts, t, s);
                        float myGap = 0.0f;
                        Vec3 myNrm{0.0f, 0.0f, 1.0f};
                        if (!signedGap2(*bodyBVH, curP, myGap, &myNrm)) continue;

                        float targetGap = myGap;
                        for (const OuterCollider& oc : *outerColliders) {
                            if (!oc.Base().valid || !oc.Current().valid) continue;
                            Vec3 bcp{0.0f, 0.0f, 0.0f}, bnrm{0.0f, 0.0f, 1.0f};
                            float bd2 = 0.0f;
                            int baseTri = -1;
                            float bu = 0.0f, bv = 0.0f, bw = 0.0f;
                            if (!BVHClosestPointEx(oc.Base(), baseP, bcp, bnrm, bd2,
                                                    &baseTri, &bu, &bv, &bw)) continue;
                            if (bd2 > kFaceOverlap2) continue;
                            if (baseTri < 0 || baseTri >= (int)oc.Current().tris.size()) continue;

                            Vec3 outerCur = TriPointBary(oc.Current(), baseTri, bu, bv, bw);
                            float outerGap = 0.0f;
                            if (!signedGap2(*bodyBVH, outerCur, outerGap, nullptr)) continue;
                            // Use current-morph gap instead of base-gap guard: skip only when
                            // the outer face is significantly below us (dress naturally above it).
                            // Tubular (Pants) is excluded: a large gap here IS the visible clip
                            // (pants sticking out past the dress) and must fall through to the
                            // no-room embed logic below instead of being skipped outright.
                            if (!tubularLayerTuck && outerGap + 0.15f <= myGap) continue;
                            float allowed = outerGap - layerMarginFor(oc.preset);
                            if (allowed < kBodyFloor) {
                                if (round == 0 && myGap > allowed + 0.01f) ++noRoomSkipped;
                                if (tubularLayerTuck) {
                                    float tightTarget = std::max(allowed, kNoRoomFloor);
                                    if (myGap > tightTarget + 0.01f)
                                        targetGap = std::min(targetGap, tightTarget);
                                } else if (fittedLayerTuck && myGap > kBodyFloor + 0.01f) {
                                    targetGap = std::min(targetGap, kBodyFloor);
                                }
                                continue;
                            }
                            if (myGap > allowed + 0.01f)
                                targetGap = std::min(targetGap, allowed);
                        }

                        if (targetGap >= myGap - 0.01f) continue;
                        float step = std::min((myGap - targetGap) * kFaceMoveStrength, kFaceMaxStep);
                        if (step <= 0.0f) continue;

                        const float ws[3] = {s.a, s.b, s.c};
                        const uint16_t vi[3] = {t[0], t[1], t[2]};
                        float sumSq = ws[0] * ws[0] + ws[1] * ws[1] + ws[2] * ws[2];
                        if (sumSq <= 1.0e-6f) continue;
                        bool moved = false;
                        for (int k = 0; k < 3; ++k) {
                            if (ws[k] <= 0.0f || isRigidVertex3(vi[k])) continue;
                            float vertexGap = 0.0f;
                            if (signedGap2(*bodyBVH, finalVerts[vi[k]], vertexGap, nullptr)) {
                                float room = vertexGap - kMoveFloor;
                                if (room <= 0.005f) continue;
                                float vStep = std::min(step * ws[k] / sumSq, room);
                                if (vStep <= 0.0f) continue;
                                finalVerts[vi[k]] = Add(finalVerts[vi[k]], Scale(myNrm, -vStep));
                                moved = true;
                            }
                        }
                        if (moved) {
                            ++roundTucked;
                            ++roundFaceTucked;
                        }
                    }
                }
                totalFaceTucked += roundFaceTucked;
            }
            if (roundTucked == 0) break;
            totalTucked += roundTucked;
            applyFinalSupportCap(12, nullptr);
        }
        if (totalTucked > 0)
            HLog("detour: final-feasible-layer-tuck shape='%s' stage='%s' tucked=%d faceSamples=%d noRoomSkipped=%d",
                 shapeName.c_str(), stageLabel ? stageLabel : "",
                 totalTucked, totalFaceTucked, noRoomSkipped);
        return totalTucked;
    };

    // De-spike pass: relax vertices that protrude far beyond their 1-ring neighbours
    // (vanilla-conform tent-poles) inward along the vertex normal. Capped at kSpikeCap
    // so genuine curvature is preserved; clamped to the body floor so no vertex is
    // pushed into the body. Only runs for draped/fitted cloth where these spikes occur.
    auto applyFinalDeSpike = [&](const char* stageLabel) -> int {
        if (!tris || tris->empty() || !bodyBVH || !bodyBVH->valid) return 0;
        if (!(isLoosePreset || fittedLayerTuck)) return 0;
        const float kSpikeCap = 0.8f;
        const float kRelax = 0.5f;
        const int kIters = 12;
        const float kBodyFloorDS = 0.06f;
        const float kMaxStepDS = 0.40f;
        const float kDeSpikeStretchCap = 1.12f;  // matches the loose support cap

        std::vector<std::vector<uint16_t>> adj(n);
        {
            std::vector<std::unordered_set<uint16_t>> tmp(n);
            for (const auto& t : *tris) {
                if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
                for (int a = 0; a < 3; ++a)
                    for (int b = 0; b < 3; ++b)
                        if (a != b) tmp[t[a]].insert(t[b]);
            }
            for (size_t i = 0; i < n; ++i)
                adj[i].assign(tmp[i].begin(), tmp[i].end());
        }
        // Per-vertex incident support edges with their vanilla rest lengths, so a
        // de-spike move can be rejected if it would over-stretch any support edge.
        std::vector<std::vector<std::pair<uint16_t, float>>> vEdges(n);
        for (size_t ei = 0; ei < useEdges.size(); ++ei) {
            const auto& e = useEdges[ei];
            if (e.a >= n || e.b >= n) continue;
            float rest = (ei < supportRestLen.size()) ? supportRestLen[ei] : e.restLength;
            if (rest < 1e-4f) continue;
            vEdges[e.a].emplace_back((uint16_t)e.b, rest);
            vEdges[e.b].emplace_back((uint16_t)e.a, rest);
        }

        int totalMoved = 0;
        for (int it = 0; it < kIters; ++it) {
            std::vector<Vec3> vn(n, Vec3{0.0f, 0.0f, 0.0f});
            for (const auto& t : *tris) {
                if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
                Vec3 fn = Cross(Sub(finalVerts[t[1]], finalVerts[t[0]]),
                                Sub(finalVerts[t[2]], finalVerts[t[0]]));
                for (int k = 0; k < 3; ++k) vn[t[k]] = Add(vn[t[k]], fn);
            }
            std::vector<Vec3> newPos(finalVerts.begin(), finalVerts.begin() + n);
            int moved = 0;
            for (size_t i = 0; i < n; ++i) {
                if (adj[i].empty()) continue;
                Vec3 cen{0.0f, 0.0f, 0.0f};
                for (uint16_t w : adj[i]) cen = Add(cen, finalVerts[w]);
                float k = (float)adj[i].size();
                cen = Scale(cen, 1.0f / k);
                float nl = std::sqrt(Dot(vn[i], vn[i]));
                if (nl < 1e-6f) continue;
                Vec3 nrm = Scale(vn[i], 1.0f / nl);
                float o = Dot(Sub(finalVerts[i], cen), nrm);   // protrusion along normal
                if (o <= kSpikeCap) continue;
                // Move only the NORMAL component of the protrusion inward (flatten the
                // spike height) without the large in-plane displacement that a full
                // Laplacian toward the centroid would add and which distorts triangles.
                float excess = std::min((o - kSpikeCap) * kRelax, kMaxStepDS);
                Vec3 cand = Sub(finalVerts[i], Scale(nrm, excess));
                // Body floor clamp: never push a vertex into (or under) the body. If the
                // move would cross the floor, shorten it to land at the floor instead of
                // skipping the spike entirely.
                float candGap = 0.0f;
                if (BVHSignedGap(*bodyBVH, cand, candGap, nullptr) && candGap < kBodyFloorDS) {
                    float curGap = 0.0f;
                    if (!BVHSignedGap(*bodyBVH, finalVerts[i], curGap, nullptr) ||
                        curGap <= kBodyFloorDS)
                        continue;
                    float denom = curGap - candGap;
                    if (denom <= 1e-4f) continue;
                    float scale = (curGap - kBodyFloorDS) / denom;
                    if (scale <= 0.0f) continue;
                    cand = Sub(finalVerts[i], Scale(nrm, excess * scale));
                }
                // Support-stretch guard: reject the move if it would push any incident
                // support edge past the cap. This stops the spike-flattening from
                // creating a new lateral tear (the along-normal move has a small lateral
                // component when the vertex normal is skewed).
                bool overStretch = false;
                for (const auto& pe : vEdges[i]) {
                    Vec3 d = Sub(cand, finalVerts[pe.first]);
                    if (Dot(d, d) > (kDeSpikeStretchCap * pe.second) *
                                    (kDeSpikeStretchCap * pe.second)) {
                        overStretch = true;
                        break;
                    }
                }
                if (overStretch) continue;
                newPos[i] = cand;
                ++moved;
            }
            for (size_t i = 0; i < n; ++i) finalVerts[i] = newPos[i];
            totalMoved += moved;
            if (moved == 0) break;
        }
        if (totalMoved > 0)
            HLog("detour: final-despike shape='%s' stage='%s' moved=%d",
                 shapeName.c_str(), stageLabel ? stageLabel : "", totalMoved);
        return totalMoved;
    };

    // Self-collision tuck: the dress is a thin double shell. Under heavy-body
    // inflation the vanilla conform pushes some INNER-shell verts out through the
    // OUTER shell (visible dark inner faces, mostly in the skirt). The solver has
    // no self-collision, so this finishing pass pulls each EXPOSED inner-facing
    // vertex back inward (toward the body) until outer-shell cloth covers it again.
    // Boundary/seam verts are excluded so it cannot reopen welded seams (the
    // mistake the de-spike made), and a support-stretch guard prevents new tears.
    // Detection mirrors scratchpad/pokethrough.py (thresholds x10 for nif units).
    auto applyFinalSelfCollisionTuck = [&](const char* stageLabel) -> int {
        if (!tris || tris->empty() || !bodyBVH || !bodyBVH->valid) return 0;
        if (!(isLoosePreset || fittedLayerTuck)) return 0;
        const float kFacingThresh = -0.25f;  // inner-facing if vertNormal . bodyOut < this
        const float kCoveredMin   = 0.30f;   // nif: min outward distance to count as "outside me"
        const float kPerpRadius   = 2.5f;    // nif: perpendicular radius of the coverage cone
        const float kSearchR      = 4.0f;    // nif: grid cell / neighbour search radius
        const float kStepSC       = 0.50f;   // nif: inward step per round
        const float kBodyFloorSC  = 0.10f;
        const int   kItersSC      = 8;
        const float kSCStretchCap = 1.12f;

        // Boundary verts = on an edge used by only one triangle. Exclude them.
        std::unordered_map<uint32_t, int> edgeCount;
        auto ekey = [](uint16_t a, uint16_t b) -> uint32_t {
            if (a > b) { uint16_t t = a; a = b; b = t; }
            return ((uint32_t)a << 16) | b;
        };
        for (const auto& t : *tris) {
            if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
            edgeCount[ekey(t[0], t[1])]++;
            edgeCount[ekey(t[1], t[2])]++;
            edgeCount[ekey(t[2], t[0])]++;
        }
        std::vector<bool> boundary(n, false);
        for (const auto& t : *tris) {
            if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
            const uint16_t es[3][2] = {{t[0], t[1]}, {t[1], t[2]}, {t[2], t[0]}};
            for (const auto& e : es)
                if (edgeCount[ekey(e[0], e[1])] == 1) {
                    boundary[e[0]] = true;
                    boundary[e[1]] = true;
                }
        }

        // Per-vertex incident support edges (stretch guard).
        std::vector<std::vector<std::pair<uint16_t, float>>> vEdges(n);
        for (size_t ei = 0; ei < useEdges.size(); ++ei) {
            const auto& e = useEdges[ei];
            if (e.a >= n || e.b >= n) continue;
            float rest = (ei < supportRestLen.size()) ? supportRestLen[ei] : e.restLength;
            if (rest < 1e-4f) continue;
            vEdges[e.a].emplace_back((uint16_t)e.b, rest);
            vEdges[e.b].emplace_back((uint16_t)e.a, rest);
        }

        const float inv = 1.0f / kSearchR;
        auto cellKey = [](int x, int y, int z) -> int64_t {
            return ((int64_t)(x + 4096) << 40) | ((int64_t)(y + 4096) << 20) |
                   (int64_t)(z + 4096);
        };
        int totalMoved = 0;
        for (int it = 0; it < kItersSC; ++it) {
            std::vector<Vec3> vn(n, Vec3{0.0f, 0.0f, 0.0f});
            for (const auto& t : *tris) {
                if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
                Vec3 fn = Cross(Sub(finalVerts[t[1]], finalVerts[t[0]]),
                                Sub(finalVerts[t[2]], finalVerts[t[0]]));
                for (int k = 0; k < 3; ++k) vn[t[k]] = Add(vn[t[k]], fn);
            }
            std::unordered_map<int64_t, std::vector<int>> grid;
            grid.reserve(n * 2);
            for (size_t i = 0; i < n; ++i) {
                int gx = (int)std::floor(finalVerts[i].x * inv);
                int gy = (int)std::floor(finalVerts[i].y * inv);
                int gz = (int)std::floor(finalVerts[i].z * inv);
                grid[cellKey(gx, gy, gz)].push_back((int)i);
            }
            // Precompute per-vertex body-outward direction and facing sign so the
            // coverage test can require an OUTER-facing coverer (two inner verts
            // poking out must not "cover" each other).
            std::vector<Vec3> bodyOutAll(n, Vec3{0.0f, 0.0f, 1.0f});
            std::vector<float> bodyGapAll(n, 0.0f);
            std::vector<float> facingAll(n, 0.0f);
            std::vector<uint8_t> okAll(n, 0);
            for (size_t i = 0; i < n; ++i) {
                float nl = std::sqrt(Dot(vn[i], vn[i]));
                if (nl < 1e-6f) continue;
                Vec3 vnorm = Scale(vn[i], 1.0f / nl);
                Vec3 bo{0.0f, 0.0f, 1.0f};
                float bg = 0.0f;
                if (!BVHSignedGap(*bodyBVH, finalVerts[i], bg, &bo)) continue;
                bodyOutAll[i] = bo;
                bodyGapAll[i] = bg;
                facingAll[i] = Dot(vnorm, bo);
                okAll[i] = 1;
            }
            std::vector<Vec3> newPos(finalVerts.begin(), finalVerts.begin() + n);
            int moved = 0;
            for (size_t i = 0; i < n; ++i) {
                if (boundary[i] || !okAll[i]) continue;
                Vec3 bodyOut = bodyOutAll[i];
                float bodyGap = bodyGapAll[i];
                if (facingAll[i] > kFacingThresh) continue;  // only inner-facing
                if (bodyGap <= kBodyFloorSC) continue;
                // Coverage test: is any OUTER-facing cloth vertex clearly outside me?
                bool covered = false;
                int gx = (int)std::floor(finalVerts[i].x * inv);
                int gy = (int)std::floor(finalVerts[i].y * inv);
                int gz = (int)std::floor(finalVerts[i].z * inv);
                for (int dx = -1; dx <= 1 && !covered; ++dx)
                for (int dy = -1; dy <= 1 && !covered; ++dy)
                for (int dz = -1; dz <= 1 && !covered; ++dz) {
                    auto itg = grid.find(cellKey(gx + dx, gy + dy, gz + dz));
                    if (itg == grid.end()) continue;
                    for (int j : itg->second) {
                        if (j == (int)i || !okAll[j]) continue;
                        if (facingAll[j] < 0.0f) continue;  // coverer must be outer-facing
                        Vec3 rel = Sub(finalVerts[j], finalVerts[i]);
                        float along = Dot(rel, bodyOut);
                        if (along <= kCoveredMin) continue;
                        float perp = std::sqrt(std::max(Dot(rel, rel) - along * along, 0.0f));
                        if (perp < kPerpRadius) { covered = true; break; }
                    }
                }
                if (covered) continue;
                // Exposed inner-shell vertex: push inward (toward body), bounded.
                float step = std::min(kStepSC, bodyGap - kBodyFloorSC);
                if (step <= 0.0f) continue;
                Vec3 cand = Sub(finalVerts[i], Scale(bodyOut, step));
                bool overStretch = false;
                for (const auto& pe : vEdges[i]) {
                    Vec3 d = Sub(cand, finalVerts[pe.first]);
                    if (Dot(d, d) > (kSCStretchCap * pe.second) * (kSCStretchCap * pe.second)) {
                        overStretch = true;
                        break;
                    }
                }
                if (overStretch) continue;
                newPos[i] = cand;
                ++moved;
            }
            for (size_t i = 0; i < n; ++i) finalVerts[i] = newPos[i];
            totalMoved += moved;
            if (moved == 0) break;
        }
        if (totalMoved > 0)
            HLog("detour: final-selfcollision-tuck shape='%s' stage='%s' moved=%d",
                 shapeName.c_str(), stageLabel ? stageLabel : "", totalMoved);
        return totalMoved;
    };

    applyOuterAccessoryInnerClearance("pre-cap");
    applyFinalLayerTuck(0, false, "pre-cap");
    // Do not close visual gaps by pulling arbitrary boundary vertices together here.
    // Replays showed that post-solve seam tugging increases support-edge stretch on
    // layered outfits. Geometry continuity must be preserved by the solve itself.
    // Final global support-length cap. Earlier caps only covered the loose cloth path,
    // but later rigid/border/body/layer passes can still over-stretch fitted, belt,
    // and tubular meshes. Cap against the vanilla-conformed support lengths so body
    // morph expansion is allowed, while solver-created tears are not.
    applyFinalSupportCap(64, "final-stretch-cap");
    // Final body clearance runs after the support cap because the cap can pull
    // a vertex slightly back into the body. Keep it deliberately limited: this
    // is a last-line penetration nudge, not another body-shaping pass.
    if (bodyBVH && bodyBVH->valid) {
        int finalBodyAdjusted = 0;
        const float kFinalBodyGap = 0.01f;
        const float kFinalBodyMaxStep = 0.25f;
        for (size_t i = 0; i < n; ++i) {
            float gap = 0.0f;
            Vec3 nrm{0.0f, 0.0f, 1.0f};
            if (!BVHSignedGap(*bodyBVH, finalVerts[i], gap, &nrm)) continue;
            if (gap >= kFinalBodyGap) continue;
            float step = std::min((kFinalBodyGap - gap) * 0.35f, kFinalBodyMaxStep);
            if (step <= 0.0f) continue;
            finalVerts[i] = Add(finalVerts[i], Scale(nrm, step));
            ++finalBodyAdjusted;
        }
        if (finalBodyAdjusted > 0)
            HLog("detour: final-body-clearance shape='%s' adjusted=%d",
                 shapeName.c_str(), finalBodyAdjusted);
    }
    applyOuterAccessoryInnerClearance("post-cap");
    applyFinalExactSeamWeld("pre-prox-exact-seam-weld");
    applyFinalSeamVectorPairs(18, "final-boundary-seam-vector");
    logProxSeamState("before-final-prox-cap");
    applyFinalProxSeamCap(24, "final-prox-seam-cap");
    applyFinalSupportCap(12, "post-prox-stretch-cap");
    applyFinalSeamVectorPairs(8, "post-prox-boundary-seam-vector");
    applyFinalProxSeamCap(10, "final-prox-seam-polish");
    int finalExactMoved = applyFinalExactSeamWeld("final-exact-seam-weld");
    if (finalExactMoved > 0 && finalExactSeamForCompactLoose)
        applyFinalSupportCap(8, "post-exact-stretch-cap");
    applyFinalSeamVectorPairs(4, "final-boundary-seam-polish");
    logProxSeamState("after-final-prox-cap");

    // NOTE: applyFinalDeSpike (defined above) is DISABLED. It chased nif-scale
    // protrusion bumps (a scale-relative red herring) and, running after the seam
    // welds, reopened the waist/arm seams. Kept defined for reference only.
    (void)applyFinalDeSpike;

    // Self-collision tuck runs LAST: it pulls exposed inner-shell verts back under
    // the outer shell. It excludes boundary/seam verts, so it does not disturb the
    // seam welds above (unlike the de-spike).
    applyFinalSelfCollisionTuck("final");

    worstStretch("final");   // TEAR DIAGNOSTIC: compare to 'after-solve' to find the ripping stage

    // Write only non-zero diffs (matches BodySlide convention).
    outDiff.clear();
    outDiff.reserve(rawDiff.size());
    // Belt SKIN-BIND override (Phase 1a). Bind each belt vert to a barycentric anchor on
    // the BASE body + its signed clearance, then re-evaluate on the MORPHED body so it
    // tracks the morph at the authored clearance (no float, no magnetism, by construction).
    // Requires base & morphed body to share topology (same tri indices). Uses the morphed
    // FACE normal for the offset (interior closest points -> ~exact; edge/vertex -> approx).
    if (kBeltSkinBind && NormalizePreset(preset) == DRAPE_PRESET_BELT &&
        bodyBVH && bodyBVH->valid && g_refBodyBVH.valid &&
        g_refBodyBVH.tris.size() == bodyBVH->tris.size() && !bodyBVH->faceNormal.empty()) {
        // The belt is authored LOOSE (~2u clearance). INSET it a fixed distance toward
        // the body (every vert moves ~the same amount along its normal) so it de-floats
        // near-uniformly -> preserves edge lengths (little added stretch) and preserves
        // the RELATIVE drape (no uniform target = no magnetism). kFloor keeps contacts
        // just off the body (no penetration).
        const float kInset = 0.0f;
        const float kFloor = 0.10f;
        int bound = 0;
        for (size_t i = 0; i < n; ++i) {
            Vec3 cp{0,0,0}, nrm{0,0,1}; float d2 = 0.0f;
            int tri = -1; float u = 0.0f, vv = 0.0f, w = 0.0f;
            if (!BVHClosestPointEx(g_refBodyBVH, baseVerts[i], cp, nrm, d2, &tri, &u, &vv, &w)) continue;
            if (tri < 0 || tri >= (int)bodyBVH->tris.size()) continue;
            float clearance = std::sqrt(d2);
            if (Dot(Sub(baseVerts[i], cp), nrm) < 0.0f) clearance = -clearance;   // signed
            clearance = std::max(clearance - kInset, kFloor);   // fixed inset toward body
            Vec3 mp = TriPointBary(*bodyBVH, tri, u, vv, w);      // same anchor, morphed body
            Vec3 mn = bodyBVH->faceNormal[(size_t)tri];
            Vec3 pos = Add(mp, Scale(mn, clearance));
            float g = 0.0f; Vec3 gn{0,0,1};
            if (BVHSignedGap(*bodyBVH, pos, g, &gn) && g < kFloor)
                pos = Add(pos, Scale(gn, kFloor - g));
            finalVerts[i] = pos;
            ++bound;
        }
        HLog("belt skin-bind: bound %d of %zu verts inset=%.2f (refTris=%zu bodyTris=%zu)",
             bound, n, kInset, g_refBodyBVH.tris.size(), bodyBVH->tris.size());
    }

    // ---- M1: belt bind + ARAP (replaces the nudge stack) ----
    // Bind each belt vert to a barycentric anchor on the SUPPORT's base surface plus its
    // authored signed offset; the SAME anchor on the support's CURRENT (morphed/solved)
    // surface is the target. Well-conditioned verts (interior closest point => accurate
    // face-normal offset) are ARAP handles; ARAP fills the rest, minimising distortion.
    // Support = a solved inner garment (the dress) if resting-on is enabled and available
    // (M1b, de-floats onto it); else the body (M1a, reproduces the authored clearance).
    // No uniform target anywhere => magnetism is structurally impossible.
    if (kBeltArap && NormalizePreset(preset) == DRAPE_PRESET_BELT &&
        tris && !tris->empty() && n > 0 && n <= 0xFFFF) {
        const BodyBVH* supBase = nullptr;
        const BodyBVH* supCur  = nullptr;
        std::string supName = "body";
        if (kBeltArapRestOn && innerColliders) {
            for (const OuterCollider& ic : *innerColliders) {
                if (NormalizePreset(ic.preset) != DRAPE_PRESET_LOOSE_DRAPED) continue;  // the dress
                if (!ic.Base().valid || !ic.Current().valid) continue;
                if (ic.Base().tris.size() != ic.Current().tris.size()) continue;
                if (ic.Current().faceNormal.empty()) continue;
                supBase = &ic.Base(); supCur = &ic.Current(); supName = ic.name;
                break;
            }
        }
        if (!supBase && bodyBVH && bodyBVH->valid && g_refBodyBVH.valid &&
            g_refBodyBVH.tris.size() == bodyBVH->tris.size() && !bodyBVH->faceNormal.empty()) {
            supBase = &g_refBodyBVH; supCur = bodyBVH; supName = "body";
        }
        if (supBase && supCur) {
            // 1) per-vert bind + morphed target; flag well-conditioned (interior) binds.
            std::vector<ArapVec3> arap(n);
            std::vector<ArapVec3> target(n);
            std::vector<uint8_t> hasTarget(n, 0), wellCond(n, 0);
            double offSum = 0.0; int offCnt = 0;  // DIAG: authored offset field magnitude
            for (size_t i = 0; i < n; ++i) {
                arap[i] = { baseVerts[i].x, baseVerts[i].y, baseVerts[i].z };  // ARAP rest = base
                Vec3 cp{0,0,0}, nrm{0,0,1}; float d2 = 0.0f;
                int tri = -1; float u = 0.0f, v = 0.0f, w = 0.0f;
                if (!BVHClosestPointEx(*supBase, baseVerts[i], cp, nrm, d2, &tri, &u, &v, &w)) continue;
                if (tri < 0 || tri >= (int)supCur->tris.size()) continue;
                float off = std::sqrt(d2);
                if (Dot(Sub(baseVerts[i], cp), nrm) < 0.0f) off = -off;   // signed
                offSum += off; ++offCnt;
                Vec3 mp = TriPointBary(*supCur, tri, u, v, w);
                Vec3 mn = supCur->faceNormal[(size_t)tri];
                Vec3 pos = Add(mp, Scale(mn, off));
                target[i] = { pos.x, pos.y, pos.z };
                hasTarget[i] = 1;
                if (u >= 0.05f && v >= 0.05f && w >= 0.05f) wellCond[i] = 1;  // accurate offset dir
            }
            // 2) connected components: each disconnected island (wrap/strap/buckle) needs
            //    its own handles or its ARAP block is singular (unconstrained rigid body).
            std::vector<int> parent(n);
            for (size_t i = 0; i < n; ++i) parent[i] = (int)i;
            auto findRoot = [&](int x) -> int {
                while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
                return x;
            };
            for (const auto& t : *tris) {
                if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
                parent[findRoot(t[0])] = findRoot(t[1]);
                parent[findRoot(t[1])] = findRoot(t[2]);
            }
            // 3) handles = well-conditioned + hasTarget; if a component has < kMinHandles,
            //    promote all its targeted verts (small rigid islands become pure skin-bind).
            std::vector<uint8_t> isHandle(n, 0);
            std::unordered_map<int, int> handleCountByComp;
            for (size_t i = 0; i < n; ++i)
                if (hasTarget[i] && wellCond[i]) { isHandle[i] = 1; ++handleCountByComp[findRoot((int)i)]; }
            const int kMinHandles = 4;
            for (size_t i = 0; i < n; ++i)
                if (hasTarget[i] && !isHandle[i] && handleCountByComp[findRoot((int)i)] < kMinHandles)
                    isHandle[i] = 1;
            // 4) assemble + solve.
            std::vector<std::array<uint32_t, 3>> aTris;
            aTris.reserve(tris->size());
            for (const auto& t : *tris)
                if (t[0] < n && t[1] < n && t[2] < n)
                    aTris.push_back({(uint32_t)t[0], (uint32_t)t[1], (uint32_t)t[2]});
            std::vector<uint32_t> hIdx; std::vector<ArapVec3> hTgt;
            for (size_t i = 0; i < n; ++i)
                if (isHandle[i]) { hIdx.push_back((uint32_t)i); hTgt.push_back(target[i]); }
            std::unordered_set<int> roots;
            for (size_t i = 0; i < n; ++i) roots.insert(findRoot((int)i));
            if (!hIdx.empty() && SolveArap(arap, aTris, hIdx, hTgt, 4)) {
                for (size_t i = 0; i < n; ++i)
                    finalVerts[i] = Vec3{ arap[i].x, arap[i].y, arap[i].z };
                // One-way body floor (the plan's kept safety net): ARAP has no body-
                // collision awareness, and negative authored offsets (belt inner shell
                // below the dress) can land verts inside the body. Push ONLY penetrating
                // verts back out to a thin clearance - one-directional, resolves a
                // violation, never a uniform inward pull. bodyBVH is the morphed body.
                int floored = 0;
                if (bodyBVH && bodyBVH->valid) {
                    const float kBodyFloor = 0.08f;
                    for (size_t i = 0; i < n; ++i) {
                        float g = 0.0f; Vec3 gn{0,0,1};
                        if (BVHSignedGap(*bodyBVH, finalVerts[i], g, &gn) && g < kBodyFloor) {
                            finalVerts[i] = Add(finalVerts[i], Scale(gn, kBodyFloor - g));
                            ++floored;
                        }
                    }
                }
                HLog("belt M1 ARAP: support='%s' verts=%zu handles=%zu comps=%zu bodyFloored=%d authoredOffsetMean=%.3f",
                     supName.c_str(), n, hIdx.size(), roots.size(), floored,
                     offCnt > 0 ? (float)(offSum / offCnt) : 0.0f);
            } else {
                HLog("belt M1 ARAP: SolveArap FAILED (support='%s' handles=%zu) - keeping pipeline result",
                     supName.c_str(), hIdx.size());
            }
        }
    }

    static constexpr float kWriteEps2 = 1.0e-10f;
    for (size_t i = 0; i < n; ++i) {
        Vec3 finalDiff = Sub(finalVerts[i], baseVerts[i]);
        if (Len2(finalDiff) > kWriteEps2) {
            outDiff[static_cast<uint16_t>(i)] = ToVector3(finalDiff);
            ++stats.written;
        }
    }
    for (const auto& kv : rawDiff) {
        if (kv.first >= n)
            outDiff[kv.first] = kv.second;
    }

    // Hand the final solved positions back so the caller can cache them as an
    // outer-collider source for inner meshes solved later in this slider.
    if (outSolvedVerts)
        *outSolvedVerts = finalVerts;

    return true;
}

// Crash-containment wrapper (plan U1). Calls the real implementation inside SEH so
// a hardware exception ANYWHERE in the ~8000-line solver (bad index, div-by-zero,
// stack overflow, etc) is caught HERE instead of propagating up through the
// OutfitStudio hook and taking the host process down with unsaved work. On any
// crash: outDiff/stats/outSolvedVerts are reset to empty so the caller sees "no
// result produced" and falls back cleanly (see Detour_GenResult: vanilla's result
// from g_origGenResult is already sitting in the host, so an empty diff here means
// vanilla stays - the U2 degradation ladder's bottom rung). Deliberately a THIN
// wrapper (no local C++ objects needing destructor unwinding) so SEH semantics are
// unambiguous, matching HostGetRawResultDiff / HostSetResultDiff / HostCopySourceDiff.
static bool ApplyStaticClothPass(const std::string& shapeName,
                                 const std::string& sliderName,
                                 int preset,
                                 const std::vector<Vec3>& baseVerts,
                                 const std::vector<VertexBoneProfile>* profiles,
                                 const std::vector<EdgeConstraint>* edges,
                                 const std::vector<std::array<uint16_t, 3>>* tris,
                                 const BodyShell* bodyShell,
                                 const BodyBVH* bodyBVH,
                                 const std::unordered_map<uint16_t, float>& presetMask,
                                 const TargetDataDiffs& rawDiff,
                                 const std::vector<OuterCollider>* outerColliders,
                                 const std::vector<OuterCollider>* innerColliders,
                                 std::vector<Vec3>* outSolvedVerts,
                                 TargetDataDiffs& outDiff,
                                 StaticSolveStats& stats) {
#if defined(_MSC_VER)
    __try {
        return ApplyStaticClothPassImpl(shapeName, sliderName, preset, baseVerts,
                                        profiles, edges, tris, bodyShell, bodyBVH,
                                        presetMask, rawDiff, outerColliders, innerColliders,
                                        outSolvedVerts, outDiff, stats);
    }
    __except (GetExceptionCode() == EXCEPTION_STACK_OVERFLOW
                  ? (_resetstkoflw(), EXCEPTION_EXECUTE_HANDLER)
                  : EXCEPTION_EXECUTE_HANDLER) {
        HLog("CRASH CONTAINED: ApplyStaticClothPass raised SEH 0x%08X for shape '%s' "
             "slider '%s' preset=%d - falling back to vanilla conform result",
             (unsigned)GetExceptionCode(), shapeName.c_str(), sliderName.c_str(), preset);
        outDiff.clear();
        stats = StaticSolveStats{};
        if (outSolvedVerts) outSolvedVerts->clear();
        return false;
    }
#else
    return ApplyStaticClothPassImpl(shapeName, sliderName, preset, baseVerts,
                                    profiles, edges, tris, bodyShell, bodyBVH,
                                    presetMask, rawDiff, outerColliders, innerColliders,
                                    outSolvedVerts, outDiff, stats);
#endif
}

// Output validation gate (plan U2, the degradation ladder's top rung). Even when
// ApplyStaticClothPass logically "succeeds" (passOk=true, no crash), its output
// could still be garbage - NaN from a divide-by-zero that didn't fault, or an
// "explosion" (a feedback loop in one of the ~20 finishing passes blowing a vertex
// out to absurd distance). Both have happened during development. This is a HARD
// SANITY ceiling, not a quality gate - thresholds are deliberately generous (far
// above anything legitimate this project has ever produced, including RM_Heavy's
// extreme morph) so it only fires on genuine breakage, never on normal tuning
// variance. On failure the live caller must NOT call HostSetResultDiff, leaving
// vanilla's already-computed result in place (Detour_GenResult runs
// g_origGenResult before any of this) - that is the ladder's bottom rung.
struct SolveValidation {
    bool ok = true;
    const char* reason = "";
    int nanCount = 0;
    float worstStretch = 0.0f;
    float maxDisplacement = 0.0f;
};

static SolveValidation ValidateSolveOutput(const std::vector<Vec3>& baseVerts,
                                           const std::vector<Vec3>& solvedVerts,
                                           const std::vector<EdgeConstraint>* edges) {
    SolveValidation v;
    static constexpr float kMaxStretchCeiling = 8.0f;     // no legitimate cloth edge does this
    static constexpr float kMaxDisplacementCeiling = 80.0f;  // absurd for any garment/morph seen
    size_t n = std::min(baseVerts.size(), solvedVerts.size());
    if (n == 0) return v;  // empty output is a no-op, not a failure

    for (size_t i = 0; i < n; ++i) {
        const Vec3& p = solvedVerts[i];
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            ++v.nanCount;
            continue;
        }
        v.maxDisplacement = std::max(v.maxDisplacement, Len(Sub(p, baseVerts[i])));
    }
    if (v.nanCount > 0) { v.ok = false; v.reason = "NaN/Inf vertex"; return v; }
    if (v.maxDisplacement > kMaxDisplacementCeiling) {
        v.ok = false; v.reason = "displacement exploded"; return v;
    }
    if (edges) {
        for (const EdgeConstraint& e : *edges) {
            if (e.a >= n || e.b >= n || e.restLength <= 1.0e-6f) continue;
            float stretch = Len(Sub(solvedVerts[e.a], solvedVerts[e.b])) / e.restLength;
            v.worstStretch = std::max(v.worstStretch, stretch);
        }
        if (v.worstStretch > kMaxStretchCeiling) {
            v.ok = false; v.reason = "edge stretch exploded"; return v;
        }
    }
    return v;
}

struct HarnessMetrics {
    float worstEdgeStretch = 0.0f;
    float worstSupportEdgeStretch = 0.0f;
    int bodyPenetrationCount = 0;
    float worstBodyGap = 0.0f;
    int garmentResidualCount = 0;
    int garmentFaceResidualCount = 0;
    int garmentBodyConflictCount = 0;
    int garmentFaceBodyConflictCount = 0;
    std::unordered_map<std::string, int> garmentResidualByOuter;
    std::unordered_map<std::string, int> garmentFaceResidualByOuter;
    std::unordered_map<std::string, int> garmentBodyConflictByOuter;
    std::unordered_map<std::string, int> garmentFaceBodyConflictByOuter;
    std::unordered_map<std::string, float> worstGarmentPenByOuter;
    std::unordered_map<std::string, float> worstGarmentFacePenByOuter;
    float worstGarmentPen = 0.0f;
    float worstGarmentFacePen = 0.0f;
    float maxDisplacement = 0.0f;
    int nanCount = 0;
    float maxExpectedVertError = 0.0f;
    float maxExpectedDiffError = 0.0f;
    int expectedDiffMismatches = 0;

    // ---- P0 defect-taxonomy metrics (plan Part 2G / Part 9.6) ----
    // Each pairs with a user-visible complaint this project has repeatedly hit
    // where the OLD metrics stayed flat. See BELT_AND_PIPELINE_REVIEW.md.

    // roughness: mean |vert - 1-ring centroid|. Complaint: "wrinkly/bunched
    // surface" (tent-pole lesson). Reported for both the solved output and the
    // vanilla-conformed input so a regression shows whether we made it WORSE.
    float roughnessSolved = 0.0f;
    float roughnessVanilla = 0.0f;

    // floatIndex: solved body-gap minus the vertex's AUTHORED base-pose body-gap
    // (from skin-bind: base garment vert -> nearest base-body point). Complaint:
    // "hovers / floats". Near 0 = tracking the AUTHORED clearance, not zero gap
    // (a garment authored loose is SUPPOSED to float - Part 1 belt finding).
    float floatIndexMean = 0.0f;
    float floatIndexWorst = 0.0f;   // signed; most-positive = floats most vs design

    // magnetismIndex: how far a vertex was pulled INWARD (toward the body)
    // relative to where VANILLA put it. Complaint: "gets sucked into hollows"
    // (the user's radial-hang rule, Part 9). Only counts inward moves; a garment
    // that legitimately drapes OUTWARD from vanilla scores 0 here, not negative.
    int magnetismCount = 0;         // verts pulled inward vs vanilla by > 0.05u
    float magnetismWorst = 0.0f;    // largest such inward pull

    // supportFloat / supportMagnetism (M2): the rest-on analogue of the two above.
    // For a garment that RESTS ON another (belt->dress), the body-relative float/
    // magnetism MISLEAD - a belt sitting on the dress legitimately moves closer to
    // the body, so magnetismCount rises even with zero magnetism vs its actual
    // support. These measure against the SUPPORT (the nearest inner collider):
    //   supportFloatIndex = solved gap-to-support minus AUTHORED base gap-to-support
    //     (0 = tracking the authored offset, e.g. the belt's 0.77u onto the dress).
    //   supportMagnetism  = verts pulled BELOW where vanilla sat vs the support.
    // hasSupport is false (and the four numbers 0) when no inner collider is present,
    // so a shape with no support is trivially distinguishable from one at offset 0.
    bool  hasSupport = false;
    float supportFloatIndexMean = 0.0f;
    float supportFloatIndexWorst = 0.0f;   // signed; most-positive = floats most vs authored
    int   supportMagnetismCount = 0;
    float supportMagnetismWorst = 0.0f;

    // interlayerGap: mean/worst SIGNED gap (not violation) from this garment to
    // each outer/inner collider's CURRENT solved surface. Unlike
    // garmentResidual*, this reports the plain distance even when it's not
    // clipping - shows trends like "belt sits Nu below the dress" before it
    // becomes a violation. Keyed by collider name, same style as residualByOuter.
    std::unordered_map<std::string, float> interlayerGapMeanByOuter;
    std::unordered_map<std::string, float> interlayerGapMeanByInner;

    // drapeVerticality: mean angle (degrees, 0=straight down) of edges in
    // DRAPE-eligible regions (both endpoints have low presetMask follow-weight,
    // matching the physicsDrape threshold used by the solver). Complaint: "cloth
    // doesn't hang straight below where the body recedes" (the user's rule).
    float drapeVerticality = 0.0f;
    int drapeVerticalityEdgeCount = 0;

    // Deformation energy split: mean squared relative deviation from rest, over
    // the structural STRETCH edges vs the diagonal BEND (dihedral-proxy) edges.
    // Complaint: "stretching instead of the faces bending around edges/vertices"
    // (Part 9.3). A drape change should grow bendEnergy while stretchEnergy stays
    // ~flat; if stretchEnergy jumps, verts are being translated, not hinged.
    float stretchEnergy = 0.0f;
    float bendEnergy = 0.0f;
};

static std::vector<Vec3> SolvedVertsFromDiff(const std::vector<Vec3>& baseVerts,
                                             const TargetDataDiffs& diff) {
    size_t n = std::min<size_t>(baseVerts.size(), 0x10000);
    std::vector<Vec3> out(n);
    for (size_t i = 0; i < n; ++i)
        out[i] = Add(baseVerts[i], DiffAt(diff, (uint16_t)i));
    return out;
}

static HarnessMetrics ComputeHarnessMetrics(const SolverCapture& cap,
                                            const TargetDataDiffs& solvedDiff,
                                            const std::vector<Vec3>& solvedVerts) {
    HarnessMetrics m;
    size_t n = std::min<size_t>(cap.baseVerts.size(), solvedVerts.size());
    for (size_t i = 0; i < n; ++i) {
        const Vec3& v = solvedVerts[i];
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
            ++m.nanCount;
            continue;
        }
        m.maxDisplacement = std::max(m.maxDisplacement, Len(Sub(v, cap.baseVerts[i])));
    }

    for (const EdgeConstraint& e : cap.edges) {
        if (e.a >= n || e.b >= n || e.restLength <= 1.0e-6f) continue;
        float len = Len(Sub(solvedVerts[e.a], solvedVerts[e.b]));
        m.worstEdgeStretch = std::max(m.worstEdgeStretch, len / e.restLength);
        Vec3 sa = Add(cap.baseVerts[e.a], DiffAt(cap.rawDiff, e.a));
        Vec3 sb = Add(cap.baseVerts[e.b], DiffAt(cap.rawDiff, e.b));
        float supportRest = Len(Sub(sa, sb));
        if (supportRest > 1.0e-6f)
            m.worstSupportEdgeStretch = std::max(m.worstSupportEdgeStretch, len / supportRest);
    }

    BodyBVH body;
    if (cap.hasBodyBVH)
        BuildBodyBVH(cap.bodyBVHVerts, cap.bodyBVHTris, body);
    if (body.valid) {
        for (size_t i = 0; i < n; ++i) {
            float gap = 0.0f;
            if (BVHSignedGap(body, solvedVerts[i], gap, nullptr)) {
                if (gap < -0.001f) ++m.bodyPenetrationCount;
                m.worstBodyGap = std::min(m.worstBodyGap, gap);
            }
        }
    }

    // ---- P0 defect-taxonomy metrics (see field comments on HarnessMetrics) ----
    {
        // roughness: 1-ring adjacency from cap.tris, shared by the roughness and
        // drapeVerticality computations below.
        std::vector<std::vector<uint32_t>> ring(n);
        if (!cap.tris.empty()) {
            for (const auto& t : cap.tris) {
                if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
                for (int k = 0; k < 3; ++k) {
                    uint32_t a = t[k], b = t[(k + 1) % 3];
                    ring[a].push_back(b); ring[b].push_back(a);
                }
            }
            for (size_t i = 0; i < n; ++i) {
                auto& r = ring[i];
                std::sort(r.begin(), r.end());
                r.erase(std::unique(r.begin(), r.end()), r.end());
            }
            auto roughnessOf = [&](const std::vector<Vec3>& verts) -> float {
                double total = 0.0; int cnt = 0;
                for (size_t i = 0; i < n; ++i) {
                    const auto& r = ring[i];
                    if (r.size() < 2) continue;
                    Vec3 c{0.0f, 0.0f, 0.0f};
                    for (uint32_t j : r) c = Add(c, verts[j]);
                    c = Scale(c, 1.0f / (float)r.size());
                    total += Len(Sub(verts[i], c));
                    ++cnt;
                }
                return cnt > 0 ? (float)(total / cnt) : 0.0f;
            };
            m.roughnessSolved = roughnessOf(solvedVerts);
            std::vector<Vec3> vanillaVerts(n);
            for (size_t i = 0; i < n; ++i)
                vanillaVerts[i] = Add(cap.baseVerts[i], DiffAt(cap.rawDiff, (uint16_t)std::min<size_t>(i, 0xFFFF)));
            m.roughnessVanilla = roughnessOf(vanillaVerts);
        }

        // floatIndex + magnetismIndex: both compare against the AUTHORED base-pose
        // relationship, so they need the reference (base) body BVH alongside the
        // morphed one.
        BodyBVH baseBody;
        if (cap.hasRefBodyBVH)
            BuildBodyBVH(cap.refBodyBVHVerts, cap.refBodyBVHTris, baseBody);
        if (body.valid && baseBody.valid) {
            double floatSum = 0.0; int floatCnt = 0;
            for (size_t i = 0; i < n; ++i) {
                float authoredGap = 0.0f, solvedGap = 0.0f;
                if (!BVHSignedGap(baseBody, cap.baseVerts[i], authoredGap, nullptr)) continue;
                if (!BVHSignedGap(body, solvedVerts[i], solvedGap, nullptr)) continue;
                float fi = solvedGap - authoredGap;
                floatSum += fi; ++floatCnt;
                if (std::fabs(fi) > std::fabs(m.floatIndexWorst)) m.floatIndexWorst = fi;
            }
            if (floatCnt > 0) m.floatIndexMean = (float)(floatSum / floatCnt);

            // magnetismIndex: solved position vs where VANILLA put the SAME vertex,
            // both measured against the morphed body. Positive = pulled inward
            // relative to vanilla (the "magnetism" the user's rule forbids).
            for (size_t i = 0; i < n; ++i) {
                Vec3 vanillaPos = Add(cap.baseVerts[i],
                                      DiffAt(cap.rawDiff, (uint16_t)std::min<size_t>(i, 0xFFFF)));
                float vanillaGap = 0.0f, solvedGap = 0.0f;
                if (!BVHSignedGap(body, vanillaPos, vanillaGap, nullptr)) continue;
                if (!BVHSignedGap(body, solvedVerts[i], solvedGap, nullptr)) continue;
                float pulledIn = vanillaGap - solvedGap;
                if (pulledIn > 0.05f) {
                    ++m.magnetismCount;
                    m.magnetismWorst = std::max(m.magnetismWorst, pulledIn);
                }
            }
        }

        // supportFloat / supportMagnetism (M2): same idea as float/magnetism but
        // measured against the SUPPORT the garment rests on (its nearest inner
        // collider) instead of the body. Honest for rest-on garments (belt->dress),
        // where body-relative numbers mislead (Part 11.3 caveat). Per vert, pick the
        // single nearest inner collider by its CURRENT surface, then compare authored
        // (base garment vert -> support Base) vs solved (solved vert -> support Current).
        if (!cap.innerColliders.empty()) {
            m.hasSupport = true;
            double sfSum = 0.0; int sfCnt = 0;
            for (size_t i = 0; i < n; ++i) {
                // nearest inner collider for this vert (by solved distance to Current).
                const OuterCollider* best = nullptr;
                float bestD2 = std::numeric_limits<float>::max();
                Vec3 bestCp{0,0,0}, bestNrm{0,0,1};
                for (const OuterCollider& ic : cap.innerColliders) {
                    if (!ic.Current().valid) continue;
                    Vec3 cp{0,0,0}, nrm{0,0,1}; float d2 = 0.0f;
                    if (!BVHClosestPoint(ic.Current(), solvedVerts[i], cp, nrm, d2)) continue;
                    if (d2 < bestD2) { bestD2 = d2; best = &ic; bestCp = cp; bestNrm = nrm; }
                }
                if (!best || !best->Base().valid) continue;
                float authoredGap = 0.0f;
                if (!BVHSignedGap(best->Base(), cap.baseVerts[i], authoredGap, nullptr)) continue;
                float solvedGap = Dot(Sub(solvedVerts[i], bestCp), bestNrm) < 0.0f
                                      ? -std::sqrt(bestD2) : std::sqrt(bestD2);
                float sf = solvedGap - authoredGap;
                sfSum += sf; ++sfCnt;
                if (std::fabs(sf) > std::fabs(m.supportFloatIndexWorst)) m.supportFloatIndexWorst = sf;

                // support-relative magnetism: verts pulled BELOW their AUTHORED offset
                // toward/into the support. Reference is the DESIGN clearance, NOT vanilla:
                // a belt de-floating from a 2u body-float down ONTO the dress moves closer
                // to the dress than vanilla, but that is the intended rest-on, not magnetism.
                // Only sitting closer than authored (sf < 0) is the "sucked in" the rule bans.
                float belowAuthored = -sf;   // + = sits closer to support than designed
                if (belowAuthored > 0.05f) {
                    ++m.supportMagnetismCount;
                    m.supportMagnetismWorst = std::max(m.supportMagnetismWorst, belowAuthored);
                }
            }
            if (sfCnt > 0) m.supportFloatIndexMean = (float)(sfSum / sfCnt);
        }

        // drapeVerticality: mean angle-from-vertical of edges where BOTH endpoints
        // are drape-eligible (low presetMask follow weight - matches the 0.35
        // physicsDrape threshold already used by the solver, see ApplyStaticClothPassImpl).
        if (!cap.tris.empty()) {
            auto followOf = [&](size_t i) -> float {
                auto it = cap.presetMask.find((uint16_t)std::min<size_t>(i, 0xFFFF));
                return it != cap.presetMask.end() ? it->second : 1.0f;
            };
            std::unordered_set<uint64_t> seenEdge;
            double angSum = 0.0;
            for (const auto& t : cap.tris) {
                if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
                for (int k = 0; k < 3; ++k) {
                    uint32_t a = t[k], b = t[(k + 1) % 3];
                    if (followOf(a) >= 0.35f || followOf(b) >= 0.35f) continue;
                    uint64_t key = a < b ? ((uint64_t)a << 32) | b : ((uint64_t)b << 32) | a;
                    if (!seenEdge.insert(key).second) continue;
                    Vec3 d = Sub(solvedVerts[a], solvedVerts[b]);
                    float len = Len(d);
                    if (len < 1.0e-4f) continue;
                    float vert = std::fabs(d.z) / len;   // 1=vertical, 0=horizontal
                    if (vert < 0.30f) continue;           // skip near-horizontal ring edges
                    angSum += std::acos(std::min(1.0f, vert)) * 57.29578;
                    ++m.drapeVerticalityEdgeCount;
                }
            }
            if (m.drapeVerticalityEdgeCount > 0)
                m.drapeVerticality = (float)(angSum / m.drapeVerticalityEdgeCount);
        }

        // Deformation energy split: mean squared relative deviation from rest,
        // over structural (stretch) edges vs the diagonal (bend-proxy) edges.
        if (!cap.edges.empty()) {
            double sum = 0.0; int cnt = 0;
            for (const EdgeConstraint& e : cap.edges) {
                if (e.a >= n || e.b >= n || e.restLength <= 1.0e-6f) continue;
                float ratio = Len(Sub(solvedVerts[e.a], solvedVerts[e.b])) / e.restLength - 1.0f;
                sum += (double)ratio * ratio; ++cnt;
            }
            if (cnt > 0) m.stretchEnergy = (float)(sum / cnt);
        }
        if (!cap.bending.empty()) {
            double sum = 0.0; int cnt = 0;
            for (const BendingConstraint& bc : cap.bending) {
                if (bc.c >= n || bc.d >= n || bc.restLength <= 1.0e-6f) continue;
                float ratio = Len(Sub(solvedVerts[bc.c], solvedVerts[bc.d])) / bc.restLength - 1.0f;
                sum += (double)ratio * ratio; ++cnt;
            }
            if (cnt > 0) m.bendEnergy = (float)(sum / cnt);
        }

        // interlayerGap: plain signed distance (not a violation) from this
        // garment's verts to each outer/inner collider's CURRENT surface, so
        // trends ("belt sits Nu below the dress") show up before they clip.
        const float kOverlap2 = 4.25f * 4.25f;  // same convention as garmentResidual
        auto accumulateInterlayer = [&](const std::vector<OuterCollider>& colliders,
                                        std::unordered_map<std::string, float>& outMean) {
            for (const OuterCollider& oc : colliders) {
                if (!oc.Current().valid) continue;
                double sum = 0.0; int cnt = 0;
                for (size_t i = 0; i < n; ++i) {
                    Vec3 cp{0.0f, 0.0f, 0.0f}, nrm{0.0f, 0.0f, 1.0f};
                    float d2 = 0.0f;
                    if (!BVHClosestPoint(oc.Current(), solvedVerts[i], cp, nrm, d2)) continue;
                    if (d2 > kOverlap2) continue;
                    float gap = Dot(Sub(solvedVerts[i], cp), nrm) < 0.0f ? -std::sqrt(d2) : std::sqrt(d2);
                    sum += gap; ++cnt;
                }
                if (cnt > 0) {
                    std::string name = oc.name.empty() ? std::string("<unnamed>") : oc.name;
                    outMean[name] = (float)(sum / cnt);
                }
            }
        };
        accumulateInterlayer(cap.outerColliders, m.interlayerGapMeanByOuter);
        accumulateInterlayer(cap.innerColliders, m.interlayerGapMeanByInner);
    }

    if (body.valid && !cap.outerColliders.empty()) {
        BodyBVH baseBody;
        if (cap.hasRefBodyBVH)
            BuildBodyBVH(cap.refBodyBVHVerts, cap.refBodyBVHTris, baseBody);
        const BodyBVH& baseBodyRef = baseBody.valid ? baseBody : body;
        auto signedGap = [](const BodyBVH& bvh, const Vec3& p, float& gap) -> bool {
            Vec3 nrm{0.0f, 0.0f, 1.0f};
            return BVHSignedGap(bvh, p, gap, &nrm);
        };
        auto residualMarginFor = [](int outerPreset) -> float {
            switch (NormalizePreset(outerPreset)) {
            case DRAPE_PRESET_LOOSE_DRAPED:    return 0.16f;
            case DRAPE_PRESET_FLEXIBLE_ARMOR:  return 0.34f;
            case DRAPE_PRESET_RIGID_ARMOR:     return 0.40f;
            case DRAPE_PRESET_BELT:            return 0.32f;
            case DRAPE_PRESET_RIGID_ACCESSORY: return 0.28f;
            default:                           return 0.22f;
            }
        };
        const float kOverlap2 = 4.25f * 4.25f;
        const float kBaseUnderSlack = 0.10f;
        const float kBodyFloor = 0.08f;
        auto recordResidual = [&](const OuterCollider& oc, float allowed, float pen, bool faceSample) {
            std::string outerName = oc.name.empty() ? std::string("<unnamed>") : oc.name;
            if (faceSample) {
                ++m.garmentFaceResidualCount;
                ++m.garmentFaceResidualByOuter[outerName];
                if (allowed < kBodyFloor) {
                    ++m.garmentFaceBodyConflictCount;
                    ++m.garmentFaceBodyConflictByOuter[outerName];
                }
                m.worstGarmentFacePen = std::max(m.worstGarmentFacePen, pen);
                m.worstGarmentFacePenByOuter[outerName] =
                    std::max(m.worstGarmentFacePenByOuter[outerName], pen);
            } else {
                ++m.garmentResidualCount;
                ++m.garmentResidualByOuter[outerName];
                if (allowed < kBodyFloor) {
                    ++m.garmentBodyConflictCount;
                    ++m.garmentBodyConflictByOuter[outerName];
                }
                m.worstGarmentPen = std::max(m.worstGarmentPen, pen);
                m.worstGarmentPenByOuter[outerName] =
                    std::max(m.worstGarmentPenByOuter[outerName], pen);
            }
        };
        for (size_t i = 0; i < n; ++i) {
            float myBaseGap = 0.0f;
            if (!signedGap(baseBodyRef, cap.baseVerts[i], myBaseGap)) continue;
            float myGap = 0.0f;
            if (!signedGap(body, solvedVerts[i], myGap)) continue;
            for (const OuterCollider& oc : cap.outerColliders) {
                if (!oc.Base().valid || !oc.Current().valid) continue;
                Vec3 bcp{0.0f, 0.0f, 0.0f}, bnrm{0.0f, 0.0f, 1.0f};
                float bd2 = 0.0f;
                int baseTri = -1;
                float bu = 0.0f, bv = 0.0f, bw = 0.0f;
                if (!BVHClosestPointEx(oc.Base(), cap.baseVerts[i], bcp, bnrm, bd2,
                                        &baseTri, &bu, &bv, &bw)) continue;
                if (bd2 > kOverlap2) continue;
                if (baseTri < 0 || baseTri >= (int)oc.Current().tris.size()) continue;
                float outerBaseGap = 0.0f;
                if (!signedGap(baseBodyRef, bcp, outerBaseGap)) continue;
                if (myBaseGap >= outerBaseGap + kBaseUnderSlack) continue;
                Vec3 outerCur = TriPointBary(oc.Current(), baseTri, bu, bv, bw);
                float outerGap = 0.0f;
                if (!signedGap(body, outerCur, outerGap)) continue;
                float allowed = outerGap - residualMarginFor(oc.preset);
                float pen = myGap - allowed;
                if (pen <= 0.01f) continue;
                recordResidual(oc, allowed, pen, false);
                break;
            }
        }
        if (!cap.tris.empty()) {
            for (const auto& t : cap.tris) {
                if (t[0] >= n || t[1] >= n || t[2] >= n) continue;
                for (const TriangleSample& s : kTriangleSamples) {
                    Vec3 baseP = SampleTrianglePoint(cap.baseVerts, t, s);
                    float myBaseGap = 0.0f;
                    if (!signedGap(baseBodyRef, baseP, myBaseGap)) continue;
                    Vec3 curP = SampleTrianglePoint(solvedVerts, t, s);
                    float myGap = 0.0f;
                    if (!signedGap(body, curP, myGap)) continue;
                    for (const OuterCollider& oc : cap.outerColliders) {
                        if (!oc.Base().valid || !oc.Current().valid) continue;
                        Vec3 bcp{0.0f, 0.0f, 0.0f}, bnrm{0.0f, 0.0f, 1.0f};
                        float bd2 = 0.0f;
                        int baseTri = -1;
                        float bu = 0.0f, bv = 0.0f, bw = 0.0f;
                        if (!BVHClosestPointEx(oc.Base(), baseP, bcp, bnrm, bd2,
                                                &baseTri, &bu, &bv, &bw)) continue;
                        if (bd2 > kOverlap2) continue;
                        if (baseTri < 0 || baseTri >= (int)oc.Current().tris.size()) continue;
                        float outerBaseGap = 0.0f;
                        if (!signedGap(baseBodyRef, bcp, outerBaseGap)) continue;
                        if (myBaseGap >= outerBaseGap + kBaseUnderSlack) continue;
                        Vec3 outerCur = TriPointBary(oc.Current(), baseTri, bu, bv, bw);
                        float outerGap = 0.0f;
                        if (!signedGap(body, outerCur, outerGap)) continue;
                        float allowed = outerGap - residualMarginFor(oc.preset);
                        float pen = myGap - allowed;
                        if (pen <= 0.01f) continue;
                        recordResidual(oc, allowed, pen, true);
                        break;
                    }
                }
            }
        }
    }

    if (cap.hasExpected && !cap.expectedSolvedVerts.empty()) {
        size_t vn = std::min(n, cap.expectedSolvedVerts.size());
        for (size_t i = 0; i < vn; ++i)
            m.maxExpectedVertError = std::max(m.maxExpectedVertError,
                                             Len(Sub(solvedVerts[i], cap.expectedSolvedVerts[i])));
    }

    if (cap.hasExpected) {
        std::vector<uint16_t> keys;
        keys.reserve(solvedDiff.size() + cap.expectedDiff.size());
        for (const auto& kv : solvedDiff) keys.push_back(kv.first);
        for (const auto& kv : cap.expectedDiff) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        for (uint16_t k : keys) {
            Vec3 a = DiffAt(solvedDiff, k);
            Vec3 b = DiffAt(cap.expectedDiff, k);
            float err = Len(Sub(a, b));
            m.maxExpectedDiffError = std::max(m.maxExpectedDiffError, err);
            if (err > 1.0e-4f)
                ++m.expectedDiffMismatches;
        }
    }

    return m;
}

struct ReplayOptions {
    fs::path resultObj;
    fs::path debugPly;
    fs::path inputObj;
    fs::path bodyObj;
    bool quiet = false;
};

struct ReplayOutcome {
    int code = 0;
    bool passOk = false;
    SolverCapture capture;
    TargetDataDiffs solvedDiff;
    std::vector<Vec3> solvedVerts;
    HarnessMetrics metrics;
};

static bool WriteResultObj(const fs::path& path,
                           const std::vector<Vec3>& verts,
                           const std::vector<std::array<uint16_t, 3>>& tris) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    for (const Vec3& v : verts)
        out << "v " << v.x << ' ' << v.y << ' ' << v.z << '\n';
    for (const auto& t : tris)
        out << "f " << (uint32_t)t[0] + 1 << ' ' << (uint32_t)t[1] + 1 << ' ' << (uint32_t)t[2] + 1 << '\n';
    return out.good();
}

static bool WriteDebugPly(const fs::path& path,
                          const SolverCapture& cap,
                          const std::vector<Vec3>& verts) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    BodyBVH body;
    if (cap.hasBodyBVH)
        BuildBodyBVH(cap.bodyBVHVerts, cap.bodyBVHTris, body);

    out << "ply\nformat ascii 1.0\n";
    out << "element vertex " << verts.size() << "\n";
    out << "property float x\nproperty float y\nproperty float z\n";
    out << "property uchar red\nproperty uchar green\nproperty uchar blue\n";
    out << "element face " << cap.tris.size() << "\n";
    out << "property list uchar int vertex_indices\nend_header\n";

    for (size_t i = 0; i < verts.size(); ++i) {
        float mask = 0.0f;
        auto mit = cap.presetMask.find((uint16_t)std::min<size_t>(i, 0xFFFF));
        if (mit != cap.presetMask.end()) mask = Clamp01(mit->second);
        int r = 30;
        int g = (int)ClampRange(mask * 255.0f, 0.0f, 255.0f);
        int b = (int)ClampRange((1.0f - mask) * 255.0f, 0.0f, 255.0f);
        // Magnetism viz (P0): magenta if this vertex was pulled INWARD toward the
        // body vs where vanilla put it (the "sucked into hollows" defect, see
        // magnetismCount/magnetismWorst). Overridden by red (penetration) below,
        // since an actual penetration is the more severe defect.
        if (body.valid && i < cap.baseVerts.size()) {
            Vec3 vanillaPos = Add(cap.baseVerts[i],
                                  DiffAt(cap.rawDiff, (uint16_t)std::min<size_t>(i, 0xFFFF)));
            float vanillaGap = 0.0f, solvedGap = 0.0f;
            if (BVHSignedGap(body, vanillaPos, vanillaGap, nullptr) &&
                BVHSignedGap(body, verts[i], solvedGap, nullptr) &&
                (vanillaGap - solvedGap) > 0.05f) {
                r = 220; g = 0; b = 220;
            }
        }
        if (body.valid) {
            float gap = 0.0f;
            if (BVHSignedGap(body, verts[i], gap, nullptr) && gap < -0.001f) {
                r = 255; g = 32; b = 32;
            }
        }
        out << verts[i].x << ' ' << verts[i].y << ' ' << verts[i].z << ' '
            << r << ' ' << g << ' ' << b << '\n';
    }
    for (const auto& t : cap.tris)
        out << "3 " << t[0] << ' ' << t[1] << ' ' << t[2] << '\n';
    return out.good();
}

static ReplayOutcome RunReplayCapture(const fs::path& path, const ReplayOptions& opts) {
    ReplayOutcome out;
    if (!ReadSolverCapture(path, out.capture)) {
        std::cerr << "Error: failed to read capture: " << path.string() << "\n";
        out.code = 2;
        return out;
    }

    const SolverCapture& cap = out.capture;
    g_projectKey = cap.projectKey;
    g_refCenterX = cap.refCenterX;
    g_refCenterY = cap.refCenterY;
    g_refMinZ = cap.refMinZ;
    g_refMaxZ = cap.refMaxZ;
    g_processingIntensity.store(cap.processingIntensity, std::memory_order_relaxed);
    g_refBodyBVH = BodyBVH{};
    if (cap.hasRefBodyBVH)
        BuildBodyBVH(cap.refBodyBVHVerts, cap.refBodyBVHTris, g_refBodyBVH);

    BodyBVH bodyBVH;
    if (cap.hasBodyBVH)
        BuildBodyBVH(cap.bodyBVHVerts, cap.bodyBVHTris, bodyBVH);

    std::vector<BendingConstraint> previousBending;
    bool hadPreviousBending = false;
    {
        auto it = g_shapeBending.find(cap.shapeName);
        if (it != g_shapeBending.end()) {
            previousBending = it->second;
            hadPreviousBending = true;
        }
    }
    if (!cap.bending.empty())
        g_shapeBending[cap.shapeName] = cap.bending;
    else
        g_shapeBending.erase(cap.shapeName);

    StaticSolveStats stats;
    out.passOk = ApplyStaticClothPass(cap.shapeName, cap.sliderName, cap.preset,
                                      cap.baseVerts,
                                      cap.profiles.empty() ? nullptr : &cap.profiles,
                                      cap.edges.empty() ? nullptr : &cap.edges,
                                      cap.tris.empty() ? nullptr : &cap.tris,
                                      cap.hasBodyShell ? &cap.bodyShell : nullptr,
                                      bodyBVH.valid ? &bodyBVH : nullptr,
                                      cap.presetMask, cap.rawDiff,
                                      cap.outerColliders.empty() ? nullptr : &cap.outerColliders,
                                      cap.innerColliders.empty() ? nullptr : &cap.innerColliders,
                                      &out.solvedVerts,
                                      out.solvedDiff, stats);
    if (hadPreviousBending)
        g_shapeBending[cap.shapeName] = std::move(previousBending);
    else
        g_shapeBending.erase(cap.shapeName);
    if (out.solvedVerts.empty() && out.passOk)
        out.solvedVerts = SolvedVertsFromDiff(cap.baseVerts, out.solvedDiff);

    out.metrics = ComputeHarnessMetrics(cap, out.solvedDiff, out.solvedVerts);

    if (!opts.inputObj.empty()) {
        std::vector<Vec3> inputVerts = SolvedVertsFromDiff(cap.baseVerts, cap.rawDiff);
        if (!WriteResultObj(opts.inputObj, inputVerts, cap.tris)) {
            std::cerr << "Error: failed to write input OBJ: " << opts.inputObj.string() << "\n";
            out.code = 6;
            return out;
        }
    }
    if (!opts.resultObj.empty() && !WriteResultObj(opts.resultObj, out.solvedVerts, cap.tris)) {
        std::cerr << "Error: failed to write OBJ: " << opts.resultObj.string() << "\n";
        out.code = 6;
        return out;
    }
    if (!opts.bodyObj.empty() && cap.hasBodyBVH &&
        !WriteResultObj(opts.bodyObj, cap.bodyBVHVerts, cap.bodyBVHTris)) {
        std::cerr << "Error: failed to write body OBJ: " << opts.bodyObj.string() << "\n";
        out.code = 6;
        return out;
    }
    if (!opts.debugPly.empty() && !WriteDebugPly(opts.debugPly, cap, out.solvedVerts)) {
        std::cerr << "Error: failed to write PLY: " << opts.debugPly.string() << "\n";
        out.code = 7;
        return out;
    }

    if (!opts.quiet) {
        std::cout << "REPLAY"
                  << " file=" << path.string()
                  << " shape=" << cap.shapeName
                  << " slider=" << cap.sliderName
                  << " preset=\"" << PresetName(cap.preset) << "\""
                  << " pass=" << (out.passOk ? 1 : 0)
                  << " expectedPass=" << (cap.hasExpected ? (cap.expectedPassOk ? 1 : 0) : -1)
                  << " raw=" << cap.rawDiff.size()
                  << " outers=" << cap.outerColliders.size()
                  << " written=" << out.solvedDiff.size()
                  << " verts=" << out.solvedVerts.size()
                  << " worstEdgeStretch=" << out.metrics.worstEdgeStretch
                  << " worstSupportEdgeStretch=" << out.metrics.worstSupportEdgeStretch
                  << " bodyPenetrations=" << out.metrics.bodyPenetrationCount
                  << " worstBodyGap=" << out.metrics.worstBodyGap
                  << " garmentResidual=" << out.metrics.garmentResidualCount
                  << " garmentFaceResidual=" << out.metrics.garmentFaceResidualCount
                  << " garmentBodyConflict=" << out.metrics.garmentBodyConflictCount
                  << " garmentFaceBodyConflict=" << out.metrics.garmentFaceBodyConflictCount
                  << " worstGarmentPen=" << out.metrics.worstGarmentPen
                  << " worstGarmentFacePen=" << out.metrics.worstGarmentFacePen
                  << " maxDisplacement=" << out.metrics.maxDisplacement
                  << " nanCount=" << out.metrics.nanCount
                  << " maxExpectedVertError=" << out.metrics.maxExpectedVertError
                  << " maxExpectedDiffError=" << out.metrics.maxExpectedDiffError
                  << " expectedDiffMismatches=" << out.metrics.expectedDiffMismatches
                  << " roughnessSolved=" << out.metrics.roughnessSolved
                  << " roughnessVanilla=" << out.metrics.roughnessVanilla
                  << " floatIndexMean=" << out.metrics.floatIndexMean
                  << " floatIndexWorst=" << out.metrics.floatIndexWorst
                  << " magnetismCount=" << out.metrics.magnetismCount
                  << " magnetismWorst=" << out.metrics.magnetismWorst
                  << " hasSupport=" << (out.metrics.hasSupport ? 1 : 0)
                  << " supportFloatIndexMean=" << out.metrics.supportFloatIndexMean
                  << " supportFloatIndexWorst=" << out.metrics.supportFloatIndexWorst
                  << " supportMagnetismCount=" << out.metrics.supportMagnetismCount
                  << " supportMagnetismWorst=" << out.metrics.supportMagnetismWorst
                  << " drapeVerticality=" << out.metrics.drapeVerticality
                  << " drapeVerticalityEdgeCount=" << out.metrics.drapeVerticalityEdgeCount
                  << " stretchEnergy=" << out.metrics.stretchEnergy
                  << " bendEnergy=" << out.metrics.bendEnergy;
        if (!out.metrics.garmentResidualByOuter.empty()) {
            std::cout << " residualByOuter=";
            bool firstOuterMetric = true;
            for (const auto& kv : out.metrics.garmentResidualByOuter) {
                if (!firstOuterMetric) std::cout << ",";
                firstOuterMetric = false;
                int body = 0;
                auto bit = out.metrics.garmentBodyConflictByOuter.find(kv.first);
                if (bit != out.metrics.garmentBodyConflictByOuter.end()) body = bit->second;
                float worst = 0.0f;
                auto wit = out.metrics.worstGarmentPenByOuter.find(kv.first);
                if (wit != out.metrics.worstGarmentPenByOuter.end()) worst = wit->second;
                std::cout << kv.first << ":" << kv.second << "/body=" << body << "/worst=" << worst;
            }
        }
        if (!out.metrics.garmentFaceResidualByOuter.empty()) {
            std::cout << " faceResidualByOuter=";
            bool firstOuterMetric = true;
            for (const auto& kv : out.metrics.garmentFaceResidualByOuter) {
                if (!firstOuterMetric) std::cout << ",";
                firstOuterMetric = false;
                int body = 0;
                auto bit = out.metrics.garmentFaceBodyConflictByOuter.find(kv.first);
                if (bit != out.metrics.garmentFaceBodyConflictByOuter.end()) body = bit->second;
                float worst = 0.0f;
                auto wit = out.metrics.worstGarmentFacePenByOuter.find(kv.first);
                if (wit != out.metrics.worstGarmentFacePenByOuter.end()) worst = wit->second;
                std::cout << kv.first << ":" << kv.second << "/body=" << body << "/worst=" << worst;
            }
        }
        if (!out.metrics.interlayerGapMeanByOuter.empty()) {
            std::cout << " interlayerGapByOuter=";
            bool first = true;
            for (const auto& kv : out.metrics.interlayerGapMeanByOuter) {
                if (!first) std::cout << ",";
                first = false;
                std::cout << kv.first << ":" << kv.second;
            }
        }
        if (!out.metrics.interlayerGapMeanByInner.empty()) {
            std::cout << " interlayerGapByInner=";
            bool first = true;
            for (const auto& kv : out.metrics.interlayerGapMeanByInner) {
                if (!first) std::cout << ",";
                first = false;
                std::cout << kv.first << ":" << kv.second;
            }
        }
        std::cout << "\n";
    }

    if (cap.hasExpected && out.passOk != cap.expectedPassOk) out.code = 3;
    else if (cap.hasExpected && (out.metrics.maxExpectedDiffError > 1.0e-4f || out.metrics.expectedDiffMismatches > 0)) out.code = 4;
    else if (out.metrics.nanCount > 0) out.code = 5;
    return out;
}

static int ReplaySolverCapture(const fs::path& path, const ReplayOptions& opts = {}) {
    return RunReplayCapture(path, opts).code;
}

// U3 slider-continuity check. Users SCRUB sliders; a pass with a hard threshold
// ("if gap < X, snap") produces a discontinuous jump (popping) between two nearby
// slider values even though the underlying morph changed smoothly. This compares
// the FULLY SOLVED output of two captures of the SAME shape (ideally at nearby
// slider percentages) and reports how far each vertex moved between them - large,
// isolated per-vertex jumps (vs a smooth spread) are the signature of a gate
// firing. Judging "is this delta too much for this slider gap" is left to the
// caller (a human, or a later CI check once real near-slider capture pairs
// exist); this tool's job is just to produce the number. With two IDENTICAL
// captures (capA==capB) this must report exactly zero delta - that's also a live
// determinism check (plan: "byte-identical output every run").
static int RunContinuityCheck(const fs::path& pathA, const fs::path& pathB) {
    // NOTE: deliberately checks passOk + nanCount, NOT the RunReplayCapture return
    // code. A capture's embedded "expected" self-test field (codes 3/4) can go
    // stale independent of whether the LIVE solve itself succeeded (e.g. the
    // capture predates a later solver tuning change) - that staleness is
    // irrelevant to comparing two live solves against each other, which is all
    // this check does. A genuine solve failure (crash/NaN/read error) still stops it.
    ReplayOutcome a = RunReplayCapture(pathA, {});
    if (!a.passOk || a.metrics.nanCount > 0 || a.solvedVerts.empty()) {
        std::cerr << "Error: capture A did not produce a usable solve (code " << a.code << "): "
                  << pathA.string() << "\n";
        return a.code != 0 ? a.code : 1;
    }
    ReplayOutcome b = RunReplayCapture(pathB, {});
    if (!b.passOk || b.metrics.nanCount > 0 || b.solvedVerts.empty()) {
        std::cerr << "Error: capture B did not produce a usable solve (code " << b.code << "): "
                  << pathB.string() << "\n";
        return b.code != 0 ? b.code : 1;
    }
    if (a.capture.shapeName != b.capture.shapeName) {
        std::cerr << "Error: continuity check requires the SAME shape; got '"
                  << a.capture.shapeName << "' vs '" << b.capture.shapeName << "'\n";
        return 7;
    }
    if (a.solvedVerts.size() != b.solvedVerts.size()) {
        std::cerr << "Error: vertex count mismatch (topology differs): "
                  << a.solvedVerts.size() << " vs " << b.solvedVerts.size() << "\n";
        return 7;
    }
    size_t n = a.solvedVerts.size();
    float maxDelta = 0.0f;
    double sumDelta = 0.0;
    size_t worstIdx = 0;
    for (size_t i = 0; i < n; ++i) {
        float d = Len(Sub(a.solvedVerts[i], b.solvedVerts[i]));
        sumDelta += d;
        if (d > maxDelta) { maxDelta = d; worstIdx = i; }
    }
    float meanDelta = n > 0 ? (float)(sumDelta / n) : 0.0f;
    std::cout << "CONTINUITY shapeA=" << pathA.string()
              << " shapeB=" << pathB.string()
              << " shape=" << a.capture.shapeName
              << " sliderA=" << a.capture.sliderName
              << " sliderB=" << b.capture.sliderName
              << " verts=" << n
              << " maxDelta=" << maxDelta
              << " meanDelta=" << meanDelta
              << " worstVertexIndex=" << worstIdx
              << "\n";
    return 0;
}

static int ReplayCaptureDir(const fs::path& dir) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!ec && e.is_regular_file() && e.path().extension() == ".drcap")
            files.push_back(e.path());
    }
    if (ec) {
        std::cerr << "Error: failed to list capture dir: " << dir.string() << " (" << ec.message() << ")\n";
        return 2;
    }
    std::unordered_map<std::string, int> dependencyCountByPath;
    dependencyCountByPath.reserve(files.size());
    for (const fs::path& f : files) {
        SolverCapture cap;
        if (ReadSolverCapture(f, cap)) {
            int key = (int)cap.outerColliders.size();
            // M1b rest-on: a BELT rests on the loose dress, so it must solve AFTER the
            // loose garments (support-availability). Bump it past every normal
            // outer-count so it sorts last. (This is the pilot heuristic - the general
            // layer graph / cycle handling is M2.)
            if (kBeltArapRestOn && NormalizePreset(cap.preset) == DRAPE_PRESET_BELT)
                key += 1000;
            dependencyCountByPath[f.string()] = key;
        }
    }
    std::sort(files.begin(), files.end(), [&](const fs::path& a, const fs::path& b) {
        int da = INT_MAX;
        int db = INT_MAX;
        auto ia = dependencyCountByPath.find(a.string());
        auto ib = dependencyCountByPath.find(b.string());
        if (ia != dependencyCountByPath.end()) da = ia->second;
        if (ib != dependencyCountByPath.end()) db = ib->second;
        if (da != db) return da < db;
        return a.filename().string() < b.filename().string();
    });

    struct ReplayedShape {
        int preset = DRAPE_PRESET_AUTO;
        std::vector<Vec3> baseVerts;
        std::vector<std::array<uint16_t, 3>> tris;
        std::vector<Vec3> solvedVerts;
    };
    std::unordered_map<std::string, ReplayedShape> solvedByName;

    fs::path seqDir = fs::path("tmp") / "harness_seq";
    fs::create_directories(seqDir, ec);

    int failures = 0;
    for (const fs::path& f : files) {
        SolverCapture cap;
        if (!ReadSolverCapture(f, cap)) {
            std::cerr << "Error: failed to read capture: " << f.string() << "\n";
            ++failures;
            continue;
        }

        int replacedOuters = 0;
        std::unordered_set<std::string> existingOuterNames;
        for (OuterCollider& oc : cap.outerColliders) {
            existingOuterNames.insert(oc.name);
            auto it = solvedByName.find(oc.name);
            if (it == solvedByName.end()) continue;
            const ReplayedShape& outer = it->second;
            if (outer.solvedVerts.empty() || outer.tris.empty()) continue;
            BodyBVH updatedCurrent;
            BuildBodyBVH(outer.solvedVerts, outer.tris, updatedCurrent);
            if (!updatedCurrent.valid) continue;
            oc.current = std::move(updatedCurrent);
            oc.preset = outer.preset;
            ++replacedOuters;
        }
        int addedSemanticOuters = 0;
        for (const auto& kv : solvedByName) {
            if (kv.first == cap.shapeName || existingOuterNames.count(kv.first)) continue;
            const ReplayedShape& outer = kv.second;
            if (!PresetTieBreaksOuter(outer.preset, cap.preset)) continue;
            if (outer.baseVerts.empty() || outer.solvedVerts.empty() || outer.tris.empty()) continue;
            if (outer.baseVerts.size() != outer.solvedVerts.size()) continue;

            OuterCollider oc;
            oc.name = kv.first;
            oc.preset = outer.preset;
            BuildBodyBVH(outer.baseVerts, outer.tris, oc.base);
            BuildBodyBVH(outer.solvedVerts, outer.tris, oc.current);
            if (!oc.Base().valid || !oc.Current().valid) continue;
            cap.outerColliders.push_back(std::move(oc));
            existingOuterNames.insert(kv.first);
            ++addedSemanticOuters;
        }
        if (replacedOuters > 0 || addedSemanticOuters > 0) {
            std::cout << "REPLAY_DIR_OUTER_UPDATE"
                      << " shape=" << cap.shapeName
                      << " replaced=" << replacedOuters
                      << " addedSemantic=" << addedSemanticOuters
                      << "\n";
        }

        // M1b rest-on: give a BELT the SOLVED loose garment (the dress) as an inner
        // support collider, so the M1 ARAP block binds the belt to the dress (de-float)
        // instead of the body. The belt sorts last (above), so the dress is already
        // solved and in solvedByName here.
        int addedInners = 0;
        if (kBeltArapRestOn && NormalizePreset(cap.preset) == DRAPE_PRESET_BELT) {
            std::unordered_set<std::string> existingInnerNames;
            for (const OuterCollider& ic : cap.innerColliders) existingInnerNames.insert(ic.name);
            for (const auto& kv : solvedByName) {
                if (kv.first == cap.shapeName || existingInnerNames.count(kv.first)) continue;
                const ReplayedShape& inner = kv.second;
                if (NormalizePreset(inner.preset) != DRAPE_PRESET_LOOSE_DRAPED) continue;
                if (inner.baseVerts.empty() || inner.solvedVerts.empty() || inner.tris.empty()) continue;
                if (inner.baseVerts.size() != inner.solvedVerts.size()) continue;
                OuterCollider ic;
                ic.name = kv.first;
                ic.preset = inner.preset;
                BuildBodyBVH(inner.baseVerts, inner.tris, ic.base);
                BuildBodyBVH(inner.solvedVerts, inner.tris, ic.current);
                if (!ic.Base().valid || !ic.Current().valid) continue;
                cap.innerColliders.push_back(std::move(ic));
                existingInnerNames.insert(kv.first);
                ++addedInners;
            }
            if (addedInners > 0)
                std::cout << "REPLAY_DIR_INNER_UPDATE shape=" << cap.shapeName
                          << " addedInner=" << addedInners << "\n";
        }

        // In sequence mode, expected diffs belong to the original one-shot capture.
        // We intentionally disable equality checks so the metrics reflect the new
        // upstream outer solve instead of failing because the result changed.
        cap.hasExpected = false;
        cap.expectedDiff.clear();
        cap.expectedSolvedVerts.clear();

        fs::path replayPath = seqDir / f.filename();
        if (!WriteSolverCapture(replayPath, cap)) {
            std::cerr << "Error: failed to write sequence capture: " << replayPath.string() << "\n";
            ++failures;
            continue;
        }

        ReplayOutcome out = RunReplayCapture(replayPath, ReplayOptions{});
        if (out.code != 0) ++failures;
        if (out.passOk && !out.solvedVerts.empty()) {
            ReplayedShape rs;
            rs.preset = out.capture.preset;
            rs.baseVerts = out.capture.baseVerts;
            rs.tris = out.capture.tris;
            rs.solvedVerts = out.solvedVerts;
            solvedByName[out.capture.shapeName] = std::move(rs);
        }
    }
    std::cout << "REPLAY_DIR dir=" << dir.string() << " files=" << files.size()
              << " failures=" << failures << " sequence=1\n";
    return failures == 0 ? 0 : 10;
}
static SolverCapture MakeSelfTestCapture() {
    SolverCapture cap;
    cap.shapeName = "SelfTestCloth";
    cap.sliderName = "SelfTestSlider";
    cap.projectKey = "self-test";
    cap.preset = DRAPE_PRESET_FITTED_CLOTH;
    cap.processingIntensity = 1;
    cap.refMinZ = 0.0f;
    cap.refMaxZ = 1.0f;
    cap.baseVerts = {{-1.0f, -1.0f, 0.2f}, {1.0f, -1.0f, 0.2f}, {1.0f, 1.0f, 0.2f}, {-1.0f, 1.0f, 0.2f}};
    cap.edges = {{0, 1, 2.0f}, {1, 2, 2.0f}, {2, 3, 2.0f}, {3, 0, 2.0f}, {0, 2, std::sqrt(8.0f)}};
    cap.tris = {{0, 1, 2}, {0, 2, 3}};
    for (uint16_t i = 0; i < 4; ++i) {
        cap.presetMask[i] = 1.0f;
        cap.rawDiff[i] = Vector3(0.0f, 0.0f, 0.1f);
    }
    cap.hasBodyBVH = true;
    cap.bodyBVHVerts = {{-2.0f, -2.0f, 0.0f}, {2.0f, -2.0f, 0.0f}, {2.0f, 2.0f, 0.0f}, {-2.0f, 2.0f, 0.0f}};
    cap.bodyBVHTris = {{0, 1, 2}, {0, 2, 3}};
    cap.hasRefBodyBVH = true;
    cap.refBodyBVHVerts = cap.bodyBVHVerts;
    cap.refBodyBVHTris = cap.bodyBVHTris;

    BodyBVH body;
    BuildBodyBVH(cap.bodyBVHVerts, cap.bodyBVHTris, body);
    TargetDataDiffs solved;
    StaticSolveStats stats;
    std::vector<Vec3> solvedVerts;
    g_projectKey = cap.projectKey;
    g_refMinZ = cap.refMinZ;
    g_refMaxZ = cap.refMaxZ;
    BuildBodyBVH(cap.refBodyBVHVerts, cap.refBodyBVHTris, g_refBodyBVH);
    cap.expectedPassOk = ApplyStaticClothPass(cap.shapeName, cap.sliderName, cap.preset,
                                              cap.baseVerts, nullptr, &cap.edges, &cap.tris,
                                              nullptr, &body, cap.presetMask, cap.rawDiff,
                                              nullptr, nullptr, &solvedVerts, solved, stats);
    cap.hasExpected = true;
    cap.expectedDiff = solved;
    cap.expectedSolvedVerts = solvedVerts;
    return cap;
}

static int WriteSelfTestCapture(const fs::path& path) {
    SolverCapture cap = MakeSelfTestCapture();
    if (!WriteSolverCapture(path, cap)) {
        std::cerr << "Error: failed to write self-test capture: " << path.string() << "\n";
        return 20;
    }
    std::cout << "WROTE_SELF_TEST_CAPTURE file=" << path.string() << "\n";
    return 0;
}

static int RunSelfTest() {
    fs::path path = fs::current_path() / "drape_harness_selftest.drcap";
    int writeCode = WriteSelfTestCapture(path);
    if (writeCode != 0)
        return writeCode;
    ReplayOptions opts;
    int code = ReplaySolverCapture(path, opts);
    std::error_code ec;
    fs::remove(path, ec);
    return code;
}

static std::string ArgValue(int argc, char** argv, const std::string& flag) {
    for (int i = 1; i + 1 < argc; ++i)
        if (argv[i] == flag) return argv[i + 1];
    return {};
}

static bool HasArg(int argc, char** argv, const std::string& flag) {
    for (int i = 1; i < argc; ++i)
        if (argv[i] == flag) return true;
    return false;
}

#if defined(DRAPE_HARNESS_BUILD)
int main(int argc, char** argv) {
    if (HasArg(argc, argv, "--self-test"))
        return RunSelfTest();
    std::string writeSelf = ArgValue(argc, argv, "--write-self-test");
    if (!writeSelf.empty())
        return WriteSelfTestCapture(fs::path(writeSelf));

    std::string replay = ArgValue(argc, argv, "--replay");
    std::string replayDir = ArgValue(argc, argv, "--replay-dir");
    if (!replay.empty()) {
        ReplayOptions opts;
        std::string obj = ArgValue(argc, argv, "--result-obj");
        std::string ply = ArgValue(argc, argv, "--debug-ply");
        std::string iobj = ArgValue(argc, argv, "--input-obj");
        std::string bobj = ArgValue(argc, argv, "--body-obj");
        if (!obj.empty()) opts.resultObj = fs::path(obj);
        if (!ply.empty()) opts.debugPly = fs::path(ply);
        if (!bobj.empty()) opts.bodyObj = fs::path(bobj);
        if (!iobj.empty()) opts.inputObj = fs::path(iobj);
        return ReplaySolverCapture(fs::path(replay), opts);
    }
    if (!replayDir.empty())
        return ReplayCaptureDir(fs::path(replayDir));

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--continuity") {
            if (i + 2 >= argc) {
                std::cerr << "Error: --continuity requires two capture paths\n";
                return 1;
            }
            return RunContinuityCheck(fs::path(argv[i + 1]), fs::path(argv[i + 2]));
        }
    }

    std::cerr
        << "Usage:\n"
        << "  drape-harness --self-test\n"
        << "  drape-harness --write-self-test <capture.drcap>\n"
        << "  drape-harness --replay <capture.drcap> [--result-obj out.obj] [--debug-ply out.ply]\n"
        << "  drape-harness --replay-dir <capture_dir>\n"
        << "  drape-harness --continuity <captureA.drcap> <captureB.drcap>\n";
    return 1;
}
#else

// M2 (kLayerSolveOrder): resolve a rest-on dependency depth-first so live solve ORDER
// can't rob the belt of its de-float. When the belt is solved before its LOOSE_DRAPED
// support (the dress) in OutfitStudio's call order, the support isn't in g_solvedShapes
// yet, so without this the belt would rest on the VANILLA-conformed dress (which still
// carries the magnetism we remove). This solves `supportName` NOW, once, caches it in
// g_solvedShapes[slider], and returns it, so the belt always rests on OUR solved dress.
//
// Depth cap: the on-demand support solve passes NO outer colliders (nullptr), so it can
// never trigger a further on-demand solve -> no recursion, no cycle. It is the natural
// "dress draped on the body" rest state; OutfitStudio's own later call re-solves the
// dress WITH the (now solved) belt as an outer collider and overwrites the cache - the
// dress<->belt fixpoint refinement is M2.2. Pure computation (ApplyStaticClothPass never
// re-enters the host/OS); deterministic. Requires g_cacheMx held by the caller.
// Returns a pointer into g_solvedShapes (stable across the call) or nullptr on failure.
static const SolvedMesh* EnsureSupportSolvedLocked(
        void* self, const std::string& supportName, const std::string& sliderName,
        const BodyShell* bodyShell, const BodyBVH* bodyBVH) {
    auto& slot = g_solvedShapes[sliderName];
    {
        auto existing = slot.find(supportName);
        if (existing != slot.end() && !existing->second.verts.empty())
            return &existing->second;
    }

    auto vIt = g_shapeVerts.find(supportName);
    auto wIt = g_shapeWeights.find(supportName);
    if (vIt == g_shapeVerts.end() || wIt == g_shapeWeights.end()) return nullptr;
    if (vIt->second.empty() || vIt->second.size() > 0xFFFF ||
        wIt->second.empty()) return nullptr;
    auto pIt = g_shapeBoneProfiles.find(supportName);
    auto eIt = g_shapeEdges.find(supportName);
    auto tIt = g_shapeTris.find(supportName);
    const auto* profiles = pIt != g_shapeBoneProfiles.end() ? &pIt->second : nullptr;
    const auto* edges    = eIt != g_shapeEdges.end() ? &eIt->second : nullptr;
    const auto* tris     = tIt != g_shapeTris.end() ? &tIt->second : nullptr;

    std::unordered_map<uint16_t, float> presetMask;
    int preset = DRAPE_PRESET_AUTO;
    BuildPresetMaskLocked(supportName, wIt->second, profiles, edges, presetMask, preset);
    if (presetMask.empty()) return nullptr;

    TargetDataDiffs rawDiff;
    if (!HostGetRawResultDiff(self, &supportName, &sliderName, rawDiff)) return nullptr;

    std::vector<Vec3> solvedVerts;
    TargetDataDiffs solvedDiff;
    StaticSolveStats stats;
    bool ok = ApplyStaticClothPass(supportName, sliderName, preset, vIt->second,
                                   profiles, edges, tris, bodyShell, bodyBVH,
                                   presetMask, rawDiff,
                                   /*outerColliders*/ nullptr, /*innerColliders*/ nullptr,
                                   &solvedVerts, solvedDiff, stats);
    if (!ok || solvedVerts.empty()) return nullptr;
    SolveValidation val = ValidateSolveOutput(vIt->second, solvedVerts, edges);
    if (!val.ok) {
        HLog("M2 on-demand support solve REJECTED for '%s' slider '%s' reason='%s'",
             supportName.c_str(), sliderName.c_str(), val.reason);
        return nullptr;
    }

    SolvedMesh sm;
    sm.verts = std::move(solvedVerts);
    { auto lit = g_shapeLayer.find(supportName);
      sm.layer = lit != g_shapeLayer.end() ? lit->second : 0; }
    if (tris && !tris->empty()) BuildBodyBVH(sm.verts, *tris, sm.bvh);
    auto ins = slot.emplace(supportName, std::move(sm));
    HLog("M2 on-demand support solve: '%s' for slider '%s' verts=%zu preset='%s' "
         "(solved BEFORE its dependent so the belt rests on OUR result)",
         supportName.c_str(), sliderName.c_str(), ins.first->second.verts.size(),
         PresetName(preset));
    return &ins.first->second;
}

static void Detour_GenResult(
    void* self,
    const std::string* shapeName,
    const std::string* sliderName,
    const std::string* refDataName,
    bool transformResults, int maxResults, bool noSqueeze, bool solidMode,
    bool axisX, bool axisY, bool axisZ)
{
    const bool smartCandidate =
        g_enabled.load(std::memory_order_acquire) && g_diffZeroVert &&
        self && shapeName && sliderName && !solidMode;
    if (smartCandidate && !SliderAllowedForSmartConform(*sliderName)) {
        RecordSliderFilterSkip(*sliderName);
        return;
    }

    // 1) Let the vanilla conform compute the selected slider's result diff.
    g_origGenResult(self, shapeName, sliderName, refDataName,
                    transformResults, maxResults, noSqueeze, solidMode,
                    axisX, axisY, axisZ);

    if (!smartCandidate) {
        return;
    }

    // Skip genital / body-part meshes entirely: they are body geometry, not garments,
    // so cloth + layer logic is pointless and can deform them. Vanilla conform already
    // ran above (g_origGenResult) â€” returning now leaves that correct result in place.
    {
        std::string ln = lower(*shapeName);
        static const char* kBodyPartTokens[] = {
            "vagina", "anus", "penis", "scrotum", "clitoris", "labia", "areola", "nipple",
        };
        for (const char* tok : kBodyPartTokens) {
            if (NameHas(ln, tok)) {
                if (g_missingShapesLogged.insert(*shapeName).second)
                    HLog("detour: SKIP body-part mesh '%s' â†’ vanilla conform", shapeName->c_str());
                return;
            }
        }
    }
    std::lock_guard<std::mutex> lk(g_cacheMx);

    // Rebuild cache if the open project changed.
    std::string proj = ProjFromTitle();
    if (proj != g_projectKey) RebuildCacheLocked();

    auto weightsIt = g_shapeWeights.find(*shapeName);
    auto vertsIt = g_shapeVerts.find(*shapeName);
    auto profilesIt = g_shapeBoneProfiles.find(*shapeName);
    auto edgesIt = g_shapeEdges.find(*shapeName);
    if (weightsIt == g_shapeWeights.end() || vertsIt == g_shapeVerts.end() ||
        weightsIt->second.empty() || vertsIt->second.empty()) {
        if (g_missingShapesLogged.insert(*shapeName).second)
            HLog("detour: no drape weights for shape '%s'", shapeName->c_str());
        return;  // body or unknown
    }

    // 2) Rewrite the just-generated result diff in memory. The main path keeps
    //    loose cloth outside a vertical envelope and relaxes mesh edges back
    //    toward rest length. ZeroVertDiff remains a conservative fallback.
    //    Result is keyed by dataName = shapeName + sliderName; build it with our
    //    own CRT (never receive a host-allocated std::string -> no /MT-/MD free).
    std::string setName = *shapeName + *sliderName;
    void* resultDiffData = reinterpret_cast<char*>(self) + drape_off::kResultDiffDataOffset;
    if (kEnableDirectDiffRewrite) {
        static const std::vector<EdgeConstraint> kNoEdges;
        const auto& edges = edgesIt != g_shapeEdges.end() ? edgesIt->second : kNoEdges;
        if (ApplyVerticalEnvelopeDrape(resultDiffData, setName, *shapeName, *sliderName,
                                       vertsIt->second, weightsIt->second, edges)) {
            return;
        }
    }

    std::unordered_map<uint16_t, float> presetMask;
    int preset = DRAPE_PRESET_AUTO;
    const auto* profiles = profilesIt != g_shapeBoneProfiles.end() ? &profilesIt->second : nullptr;
    const auto* edges = edgesIt != g_shapeEdges.end() ? &edgesIt->second : nullptr;
    auto trisIt = g_shapeTris.find(*shapeName);
    const auto* tris = trisIt != g_shapeTris.end() ? &trisIt->second : nullptr;
    BuildPresetMaskLocked(*shapeName, weightsIt->second, profiles, edges, presetMask, preset);

    const BodyShell* bodyShell = nullptr;
    const BodyBVH* bodyBVH = nullptr;
    if (refDataName && !refDataName->empty() && !g_refVerts.empty()) {
        auto shellIt = g_bodyShellCache.find(*refDataName);
        if (shellIt == g_bodyShellCache.end()) {
            TargetDataDiffs refDiff;
            BodyShell shell;
            if (HostCopySourceDiff(self, refDataName, refDiff) &&
                BuildBodyShell(g_refVerts, refDiff, shell)) {
                auto inserted = g_bodyShellCache.emplace(*refDataName, std::move(shell));
                shellIt = inserted.first;
                HLog("detour: body shell built for refData '%s' verts=%zu diffVerts=%zu",
                     refDataName->c_str(), g_refVerts.size(), refDiff.size());

                // Build the 3D BVH over the same morphed body (real collider).
                if (!g_refTris.empty()) {
                    std::vector<Vec3> morphed(g_refVerts.size());
                    for (size_t i = 0; i < g_refVerts.size(); ++i)
                        morphed[i] = Add(g_refVerts[i],
                                         DiffAt(refDiff, (uint16_t)std::min<size_t>(i, 0xFFFF)));
                    BodyBVH bvh;
                    auto t0 = std::chrono::steady_clock::now();
                    BuildBodyBVH(morphed, g_refTris, bvh);
                    auto t1 = std::chrono::steady_clock::now();
                    long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
                    HLog("detour: body BVH built for refData '%s' tris=%zu nodes=%zu %ldms",
                         refDataName->c_str(), bvh.tris.size(), bvh.nodes.size(), ms);
                    g_bodyBVHCache.emplace(*refDataName, std::move(bvh));
                }
            }
        }
        if (shellIt != g_bodyShellCache.end() && shellIt->second.valid)
            bodyShell = &shellIt->second;
        auto bvhIt = g_bodyBVHCache.find(*refDataName);
        if (bvhIt != g_bodyBVHCache.end() && bvhIt->second.valid)
            bodyBVH = &bvhIt->second;
    }

    // â”€â”€ Static cloth pass (main path) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    if (!presetMask.empty() && g_getRawResultDiff && g_setResultDiff) {
        TargetDataDiffs rawDiff;
        if (HostGetRawResultDiff(self, shapeName, sliderName, rawDiff)) {
            // rawDiff empty â†’ this slider has no conform overlap with this shape:
            // ApplyStaticClothPass would return false immediately and the result
            // diff is also empty/trivial. Fall through to ZeroVertDiff (no-op).
            // This is expected behavior; no log entry needed per slider.
            TargetDataDiffs solvedDiff;
            StaticSolveStats stats;

            // Build the OUTER-mesh colliders: ONE PER garment that sits ABOVE this one
            // (strictly higher layer rank), kept SEPARATE so the nearest-point can never
            // jump between cloak/belt/pants (review #1). Each carries the outer mesh's
            // base + current surface and its preset (for adaptive clearance). The
            // outermost mesh (highest rank) gets an empty list â†’ never deformed.
            int myLayerRank = 0;
            { auto it = g_shapeLayer.find(*shapeName); if (it != g_shapeLayer.end()) myLayerRank = it->second; }
            std::vector<OuterCollider> outerColliders;
            std::vector<OuterCollider> innerColliders;
            {
                auto slIt = g_solvedShapes.find(*sliderName);
                std::vector<std::string> colliderNames;
                colliderNames.reserve(g_shapeVerts.size());
                for (const auto& kv : g_shapeVerts)
                    colliderNames.push_back(kv.first);
                std::sort(colliderNames.begin(), colliderNames.end());
                for (const std::string& other : colliderNames) {
                    if (other == *shapeName) continue;
                    int otherPreset = PresetForShapeLocked(other);
                    if (NormalizePreset(otherPreset) == DRAPE_PRESET_AUTO)
                        continue;                                  // skip body meshes
                    int otherRank = 0;
                    { auto it = g_shapeLayer.find(other); if (it != g_shapeLayer.end()) otherRank = it->second; }
                    bool pairSaysOtherOuter = ShapeCoversInnerLocked(other, *shapeName);
                    bool pairSaysThisOuter = ShapeCoversInnerLocked(*shapeName, other);
                    bool otherIsOuter = false;
                    if (pairSaysOtherOuter && !pairSaysThisOuter) {
                        otherIsOuter = true;
                    } else if (!pairSaysThisOuter) {
                        bool outerByRank = otherRank > myLayerRank;
                        bool outerByPresetTie =
                            otherRank == myLayerRank &&
                            PresetTieBreaksOuter(otherPreset, preset);
                        otherIsOuter = outerByRank || outerByPresetTie;
                    }

                    bool otherIsInner = false;
                    if (pairSaysThisOuter && !pairSaysOtherOuter) {
                        otherIsInner = true;
                    } else if (!pairSaysOtherOuter) {
                        bool innerByRank = otherRank < myLayerRank;
                        bool innerByPresetTie =
                            otherRank == myLayerRank &&
                            PresetTieBreaksOuter(preset, otherPreset);
                        otherIsInner = innerByRank || innerByPresetTie;
                    }
                    if (!otherIsOuter && !otherIsInner) continue;
                    auto itIt = g_shapeTris.find(other);
                    if (itIt == g_shapeTris.end() || itIt->second.empty()) continue;
                    auto otherVertsIt = g_shapeVerts.find(other);
                    if (otherVertsIt == g_shapeVerts.end()) continue;
                    const auto& obase = otherVertsIt->second;
                    if (obase.empty() || obase.size() > 0xFFFF) continue;
                    auto baseBvhIt = g_shapeBaseBVH.find(other);
                    if (baseBvhIt == g_shapeBaseBVH.end() || !baseBvhIt->second.valid) continue;

                    // current config: solved if cached this slider, else conformed.
                    const std::vector<Vec3>* cur = nullptr;
                    const BodyBVH* curBvh = nullptr;
                    if (slIt != g_solvedShapes.end()) {
                        auto svIt = slIt->second.find(other);
                        if (svIt != slIt->second.end() && !svIt->second.verts.empty()) {
                            cur = &svIt->second.verts;
                            if (svIt->second.bvh.valid)
                                curBvh = &svIt->second.bvh;
                        }
                    }
                    // M2: rest-on solve-order guarantee. If this belt's LOOSE_DRAPED support
                    // (the dress) hasn't been solved yet in OutfitStudio's order, solve it on
                    // demand NOW so the belt rests on OUR de-floated dress, not the vanilla
                    // one. Scoped to the belt->dress relationship (the M1 pilot); depth-capped
                    // inside the helper (no recursion). See kLayerSolveOrder.
                    if (!cur && kLayerSolveOrder && otherIsInner &&
                        NormalizePreset(preset) == DRAPE_PRESET_BELT &&
                        NormalizePreset(otherPreset) == DRAPE_PRESET_LOOSE_DRAPED) {
                        const SolvedMesh* sm =
                            EnsureSupportSolvedLocked(self, other, *sliderName, bodyShell, bodyBVH);
                        if (sm && !sm->verts.empty()) {
                            cur = &sm->verts;
                            if (sm->bvh.valid) curBvh = &sm->bvh;
                            slIt = g_solvedShapes.find(*sliderName);  // helper may have inserted; refresh
                        }
                    }
                    std::vector<Vec3> conformed;
                    if (!cur) {
                        TargetDataDiffs oraw;
                        if (HostGetRawResultDiff(self, &other, sliderName, oraw)) {
                            conformed.resize(obase.size());
                            for (size_t k = 0; k < obase.size(); ++k)
                                conformed[k] = Add(obase[k],
                                    DiffAt(oraw, (uint16_t)std::min<size_t>(k, 0xFFFF)));
                            cur = &conformed;
                        }
                    }
                    if (!cur || cur->size() != obase.size()) continue;

                    OuterCollider oc;
                    oc.name = other;
                    oc.preset = otherPreset;
                    oc.baseRef = &baseBvhIt->second;
                    if (curBvh)
                        oc.currentRef = curBvh;
                    else
                        BuildBodyBVH(*cur, itIt->second, oc.current);
                    if (oc.Base().valid && oc.Current().valid) {
                        if (otherIsOuter)
                            outerColliders.push_back(oc);
                        if (otherIsInner)
                            innerColliders.push_back(std::move(oc));
                    }
                }
                if (!outerColliders.empty()) {
                    std::string names;
                    for (auto& oc : outerColliders) { if (!names.empty()) names += ","; names += oc.name; }
                    HLog("detour: anti-clip-collider shape='%s' slider='%s' myRank=%d outers=[%s]",
                         shapeName->c_str(), sliderName->c_str(), myLayerRank, names.c_str());
                }
                if (!innerColliders.empty()) {
                    std::string names;
                    for (auto& ic : innerColliders) { if (!names.empty()) names += ","; names += ic.name; }
                    HLog("detour: inner-clearance-collider shape='%s' slider='%s' myRank=%d inners=[%s]",
                         shapeName->c_str(), sliderName->c_str(), myLayerRank, names.c_str());
                }
            }

            std::vector<Vec3> solvedVerts;
            const bool captureThis = EnvFlagEnabled("DRAPE_CAPTURE");
            SolverCapture solverCapture;
            if (captureThis) {
                solverCapture = MakeSolverCapture(*shapeName, *sliderName, preset,
                                                  vertsIt->second, profiles, edges, tris,
                                                  bodyShell, bodyBVH,
                                                  presetMask, rawDiff,
                                                  outerColliders.empty() ? nullptr : &outerColliders,
                                                  innerColliders.empty() ? nullptr : &innerColliders);
            }
            bool passOk = ApplyStaticClothPass(*shapeName, *sliderName, preset,
                                               vertsIt->second, profiles, edges, tris,
                                               bodyShell, bodyBVH,
                                               presetMask, rawDiff,
                                               outerColliders.empty() ? nullptr : &outerColliders,
                                               innerColliders.empty() ? nullptr : &innerColliders,
                                               &solvedVerts,
                                               solvedDiff, stats);
            if (captureThis)
                MaybeWriteSolverCapture(std::move(solverCapture), passOk, solvedDiff, solvedVerts);
            // U2 degradation ladder, top rung: even a "successful" solve (no crash,
            // passOk=true) gets sanity-checked before it's allowed to overwrite the
            // vanilla result already sitting in the host. Any failure here means we
            // simply don't call HostSetResultDiff below - vanilla stays untouched.
            if (passOk) {
                SolveValidation validation = ValidateSolveOutput(vertsIt->second, solvedVerts, edges);
                if (!validation.ok) {
                    HLog("SOLVE REJECTED: shape='%s' slider='%s' preset='%s' reason='%s' "
                         "nanCount=%d worstStretch=%.2f maxDisplacement=%.2f - "
                         "falling back to vanilla conform result",
                         shapeName->c_str(), sliderName->c_str(), PresetName(preset),
                         validation.reason, validation.nanCount, validation.worstStretch,
                         validation.maxDisplacement);
                    passOk = false;
                }
            }
            if (passOk) {
                if (HostSetResultDiff(self, shapeName, sliderName, solvedDiff)) {
                    // Cache this mesh only after the host accepted the diff, so later
                    // meshes never collide against a solution that failed to publish.
                    if (!solvedVerts.empty()) {
                        int myLayer = 0;
                        auto mlit = g_shapeLayer.find(*shapeName);
                        if (mlit != g_shapeLayer.end()) myLayer = mlit->second;
                        SolvedMesh sm;
                        sm.verts = std::move(solvedVerts);
                        sm.layer = myLayer;
                        if (tris && !tris->empty())
                            BuildBodyBVH(sm.verts, *tris, sm.bvh);
                        g_solvedShapes[*sliderName][*shapeName] = std::move(sm);
                    }
                    // Log EVERY slider (no deduplication) so we can verify the write path.
                    size_t edgeCount = edges ? edges->size() : 0;
                    HLog("detour: STATIC_CLOTH shape='%s' slider='%s' preset='%s' phys=%d "
                         "raw=%zu written=%d body=%d/%d envelope=%d "
                         "stretch=%d bending=%d gravity=%d anchor=%d smooth=%d "
                         "drapeEl=%d anchored=%d "
                         "infl_direct=%d infl_prop=%d infl_zero=%d edges=%zu",
                         shapeName->c_str(), sliderName->c_str(), PresetName(preset),
                         (int)stats.physicsDrapeActive,
                         stats.rawDiffVerts, stats.written,
                         stats.bodyAdjusted, stats.bodySamples,
                         stats.envelopeAdjusted, stats.stretchAdjusted,
                         stats.bendingAdjusted,
                         stats.gravityAdjusted, stats.anchorAdjusted, stats.smoothAdjusted,
                         stats.drapeEligibleVerts, stats.anchoredVerts,
                         stats.influenceDirectVerts, stats.influencePropagVerts,
                         stats.influenceZeroVerts, edgeCount);
                    return;
                }
                // HostSetResultDiff failed â€” fall through to ZeroVertDiff
                HLog("detour: SKIPPED shape='%s' slider='%s' preset='%s' "
                     "reason='HostSetResultDiff failed' â†’ FALLBACK_SCALE",
                     shapeName->c_str(), sliderName->c_str(), PresetName(preset));
            } else if (!rawDiff.empty()) {
                // rawDiff was non-empty but pass still rejected (preset disabled, etc.)
                {
                    const char* reason = !StaticParamsForPreset(preset).enabled
                        ? "preset disables static pass (skin-tight)"
                        : "solver returned false";
                    HLog("detour: SKIPPED shape='%s' slider='%s' preset='%s' "
                         "reason='%s' â†’ FALLBACK_SCALE",
                         shapeName->c_str(), sliderName->c_str(), PresetName(preset), reason);
                }
            }
        } else {
            // HostGetRawResultDiff failed (ABI issue or missing RVA)
            if (g_fallbackShapesLogged.insert(*shapeName).second)
                HLog("detour: SKIPPED static cloth shape='%s' "
                     "reason='HostGetRawResultDiff failed' â†’ FALLBACK_SCALE",
                     shapeName->c_str());
        }
    } else if (!presetMask.empty()) {
        // Host methods not wired (offsets not confirmed for this exe build)
        if (g_fallbackShapesLogged.insert(*shapeName).second)
            HLog("detour: SKIPPED static cloth shape='%s' "
                 "reason='%s' â†’ FALLBACK_SCALE",
                 shapeName->c_str(),
                 !g_getRawResultDiff ? "GetRawResultDiff RVA not wired"
                 : "SetResultDiff RVA not wired");
    }

    // â”€â”€ Fallback: ZeroVertDiff mask scale â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // Scales each vanilla conform diff by the preset mask weight; drops near-zero
    // entries. This is the safety net, not the feature.
    if (!presetMask.empty()) {
        if (g_fallbackShapesLogged.insert(*shapeName).second) {
            float sumMask = 0.0f;
            for (const auto& kv : presetMask) sumMask += kv.second;
            float avgMask = presetMask.empty() ? 1.0f : sumMask / (float)presetMask.size();
            HLog("detour: FALLBACK_SCALE shape='%s' slider='%s' preset='%s' "
                 "maskVerts=%zu avg=%.3f",
                 shapeName->c_str(), sliderName->c_str(), PresetName(preset),
                 presetMask.size(), avgMask);
        }
        g_diffZeroVert(resultDiffData, &setName, shapeName, nullptr, &presetMask);
    }
}

// â”€â”€ Public API (called by version.dll after injection) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

extern "C" __declspec(dllexport)
int DrapeInstallConformHook(void* osHwnd, const char* bsDir,
                            float tightDistance, float looseDistance) {
    if (g_hookInstalled.load(std::memory_order_acquire))
        return 0;

    g_osHwnd = static_cast<HWND>(osHwnd);
    if (bsDir) g_bsDir = bsDir;
    g_opts.tightDistance = tightDistance;
    g_opts.looseDistance = looseDistance;

    if (!drape_off::Configured()) {
        HLog("install: offsets not configured for this build â€” hook disabled");
        return -2;
    }

    uint64_t exeSize = CurrentExeSize();
    if (exeSize != drape_off::kExpectedImageSize) {
        HLog("install: exe size mismatch (%llu != %llu) -- hook disabled",
             (unsigned long long)exeSize,
             (unsigned long long)drape_off::kExpectedImageSize);
        return -6;
    }

    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    HLog("install: module base=0x%p (expected imagebase 0x%llx)",
         (void*)base, (unsigned long long)drape_off::kImageBase);

    void* pGen = reinterpret_cast<void*>(base + drape_off::kGenerateResultDiff);
    g_diffZeroVert = reinterpret_cast<PfnDiffZeroVert>(base + drape_off::kDiffSetsZeroVertDiff);
    g_diffGetDiffSet = reinterpret_cast<PfnDiffGetDiffSet>(base + drape_off::kDiffSetsGetDiffSet);
    g_getRawResultDiff = reinterpret_cast<PfnGetRawResultDiff>(
        base + drape_off::kAutomorphGetRawResultDiff);
    g_setResultDiff = reinterpret_cast<PfnSetResultDiff>(
        base + drape_off::kAutomorphSetResultDiff);

    HLog("install: RVAs gen=0x%llx zero=0x%llx getDiff=0x%llx getRaw=0x%llx set=0x%llx",
         (unsigned long long)drape_off::kGenerateResultDiff,
         (unsigned long long)drape_off::kDiffSetsZeroVertDiff,
         (unsigned long long)drape_off::kDiffSetsGetDiffSet,
         (unsigned long long)drape_off::kAutomorphGetRawResultDiff,
         (unsigned long long)drape_off::kAutomorphSetResultDiff);

    if (MH_Initialize() != MH_OK) { HLog("install: MH_Initialize failed"); return -3; }
    if (MH_CreateHook(pGen, reinterpret_cast<void*>(&Detour_GenResult),
                      reinterpret_cast<void**>(&g_origGenResult)) != MH_OK) {
        HLog("install: MH_CreateHook failed"); return -4;
    }
    if (MH_EnableHook(pGen) != MH_OK) { HLog("install: MH_EnableHook failed"); return -5; }

    HLog("install: conform hook active (tight=%.2f loose=%.2f power=%.2f)",
         g_opts.tightDistance, g_opts.looseDistance, g_opts.falloffPower);
    g_hookInstalled.store(true, std::memory_order_release);
    return 0;
}

extern "C" __declspec(dllexport) void DrapeSetHookEnabled(int enabled) {
    if (!g_hookInstalled.load(std::memory_order_acquire)) {
        g_enabled.store(false, std::memory_order_release);
        return;
    }

    g_enabled.store(enabled != 0, std::memory_order_release);
    if (!enabled)
        LogSliderFilterSummary();
}

extern "C" __declspec(dllexport)
int DrapeGetCurrentShapes(char* outUtf8, int outSize) {
    std::lock_guard<std::mutex> lk(g_cacheMx);

    std::string proj = ProjFromTitle();
    if (proj != g_projectKey || g_shapeWeights.empty())
        RebuildCacheLocked();

    std::vector<std::string> names;
    names.reserve(g_shapeWeights.size());
    for (const auto& kv : g_shapeWeights)
        names.push_back(kv.first);
    std::sort(names.begin(), names.end());

    std::string payload;
    for (const auto& name : names) {
        payload += name;
        payload += '\n';
    }

    if (outUtf8 && outSize > 0) {
        int n = std::min<int>(outSize - 1, (int)payload.size());
        if (n > 0)
            memcpy(outUtf8, payload.data(), (size_t)n);
        outUtf8[n] = '\0';
    }

    HLog("preset-ui: listed %zu shape(s) for project '%s'", names.size(), g_projectKey.c_str());
    return (int)names.size();
}

extern "C" __declspec(dllexport)
int DrapeGetCurrentSliders(char* outUtf8, int outSize) {
    std::lock_guard<std::mutex> lk(g_cacheMx);

    std::string proj = ProjFromTitle();
    std::vector<std::string> names;
    if (!proj.empty()) {
        ResolvedProject resolved = ResolveProject(proj);
        if (!resolved.ospPath.empty())
            names = ParseSliderNamesFromOsp(resolved.ospPath, proj);
    }

    std::string payload;
    for (const auto& name : names) {
        payload += name;
        payload += '\n';
    }

    if (outUtf8 && outSize > 0) {
        int n = std::min<int>(outSize - 1, (int)payload.size());
        if (n > 0)
            memcpy(outUtf8, payload.data(), (size_t)n);
        outUtf8[n] = '\0';
    }

    HLog("preset-ui: listed %zu slider(s) for project '%s'", names.size(), proj.c_str());
    return (int)names.size();
}

extern "C" __declspec(dllexport)
int DrapeClearShapePresets() {
    std::lock_guard<std::mutex> lk(g_cacheMx);
    g_shapePresets.clear();
    g_appliedShapesLogged.clear();
    g_shapePresetMaskCache.clear();
    // Clear the per-slider solved-mesh cache too: it holds the PREVIOUS Conform run's
    // results, which would otherwise be used as stale colliders on the next run.
    g_solvedShapes.clear();
    HLog("preset-ui: cleared shape presets");
    return 0;
}

extern "C" __declspec(dllexport)
int DrapeSetShapePreset(const char* shapeNameUtf8, int preset) {
    if (!shapeNameUtf8 || !*shapeNameUtf8)
        return -1;

    int normalized = NormalizePreset(preset);
    std::lock_guard<std::mutex> lk(g_cacheMx);
    g_shapePresets[shapeNameUtf8] = normalized;
    g_shapePresetMaskCache.erase(shapeNameUtf8);
    g_appliedShapesLogged.clear();
    HLog("preset-ui: shape '%s' preset='%s'", shapeNameUtf8, PresetName(normalized));
    return 0;
}

extern "C" __declspec(dllexport)
int DrapeSetSliderFilter(const char* filterUtf8) {
    std::vector<std::string> filter = SplitSliderFilter(filterUtf8);
    size_t selected = filter.size();
    {
        std::lock_guard<std::mutex> lk(g_sliderFilterMx);
        g_sliderFilter = std::move(filter);
        g_sliderFilterSkipLogged.clear();
        g_sliderFilterSkippedCalls = 0;
        g_sliderFilterSkipExamples.clear();
    }

    HLog("preset-ui: slider filter selected=%zu", selected);
    return (int)selected;
}

extern "C" __declspec(dllexport)
void DrapeSetProcessingIntensity(int level) {
    int clamped = (level < 0) ? 0 : (level > 2) ? 2 : level;
    g_processingIntensity.store(clamped, std::memory_order_relaxed);
    static const char* kLabels[] = {"Low", "Medium", "High"};
    HLog("preset-ui: processing intensity set to %s (%d)", kLabels[clamped], clamped);
}
#endif // !DRAPE_HARNESS_BUILD
