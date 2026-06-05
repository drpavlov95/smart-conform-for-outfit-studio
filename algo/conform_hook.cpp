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
#include "../proxy/drape_iface.h"
#include "DiffData.h"
#include "NifFile.hpp"

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
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

// ── Config from version.dll ────────────────────────────────────────────
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

// ── 3D body collider: BVH over the morphed body triangles ──────────────────
// Real geometric collision (closest point on the actual body surface), the
// foundation that replaces the radial shell approximation. Built once per body
// morph (keyed by refDataName) and cached. Answers "where does the body actually
// push the cloth out?" using point-to-triangle closest-point queries + the real
// surface normal — works for belly, hips, sides, bust, and limbs alike.
struct BodyBVH {
    struct Node {
        Vec3 bmin{0.0f, 0.0f, 0.0f};
        Vec3 bmax{0.0f, 0.0f, 0.0f};
        int  left  = -1;   // child node index; -1 ⇒ leaf
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

// ── Per-shape weight cache (rebuilt when the open project changes) ─────
static std::mutex g_cacheMx;
static std::string g_projectKey;   // current resolved project name
static std::unordered_map<std::string, std::vector<float>> g_shapeWeights;
static std::unordered_map<std::string, std::vector<VertexBoneProfile>> g_shapeBoneProfiles;
static std::unordered_map<std::string, std::vector<Vec3>> g_shapeVerts;
static std::unordered_map<std::string, std::vector<EdgeConstraint>> g_shapeEdges;
static std::unordered_map<std::string, std::vector<BendingConstraint>> g_shapeBending;
static std::unordered_map<std::string, int> g_shapeGeometryPresets;
static std::vector<Vec3> g_refVerts;
static std::vector<std::array<uint16_t, 3>> g_refTris;  // body triangles (for BVH)
static std::unordered_map<std::string, BodyShell> g_bodyShellCache;
static std::unordered_map<std::string, BodyBVH> g_bodyBVHCache;
// Pre-built masks (vert -> weight) reused across a shape's sliders.
static std::unordered_map<std::string, std::unordered_map<uint16_t, float>> g_shapeMask;
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

// ── WRITE-PATH DIAGNOSTIC ──────────────────────────────────────────────
// When true, ApplyStaticClothPass ignores the physics result and instead writes
// an UNMISTAKABLE deformation (drape-eligible verts shoved 15 units down). If the
// outfit visibly drops/stretches in OutfitStudio after Smart Conform, the
// SetResultDiff write path works and the problem is the algorithm. If nothing
// moves, the write path is dead and that is the real root cause.
// MUST be false for normal use.
static constexpr bool kDiagWritePathTest = false;
static constexpr float kDiagDropZ = 15.0f;

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
// Resists folding along the shared edge — gives cloth a smooth, fabric-like
// appearance instead of the "chain mail" look from stretch-only solvers.
static std::vector<BendingConstraint> BuildBendingConstraints(
    NiShape* shape, const std::vector<Vec3>& verts)
{
    std::vector<BendingConstraint> bending;
    if (!shape || verts.empty()) return bending;

    std::vector<Triangle> tris;
    shape->GetTriangles(tris);
    if (tris.empty()) return bending;

    // Map each edge key → list of triangles that contain it (identified by
    // the opposite vertex — the vertex NOT on the shared edge).
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
    for (const auto& kv : edgeToOpposite) {
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
        // them. NO forced minimum here — let limbs reduce w to near-zero.
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

// ── BVH build (iterative median split over triangle centroids) ─────────────
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
static Vec3 ClosestPtTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    Vec3 ab = Sub(b, a), ac = Sub(c, a), ap = Sub(p, a);
    float d1 = Dot(ab, ap), d2 = Dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;
    Vec3 bp = Sub(p, b);
    float d3 = Dot(ab, bp), d4 = Dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        return Add(a, Scale(ab, v));
    }
    Vec3 cp = Sub(p, c);
    float d5 = Dot(ab, cp), d6 = Dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float w = d2 / (d2 - d6);
        return Add(a, Scale(ac, w));
    }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return Add(b, Scale(Sub(c, b), w));
    }
    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom, w = vc * denom;
    return Add(a, Add(Scale(ab, v), Scale(ac, w)));
}

static float Dist2PointAABB(const Vec3& p, const Vec3& bmin, const Vec3& bmax) {
    float dx = std::max(0.0f, std::max(bmin.x - p.x, p.x - bmax.x));
    float dy = std::max(0.0f, std::max(bmin.y - p.y, p.y - bmax.y));
    float dz = std::max(0.0f, std::max(bmin.z - p.z, p.z - bmax.z));
    return dx * dx + dy * dy + dz * dz;
}

// Closest point on the body surface to p, with that triangle's face normal.
static bool BVHClosestPoint(const BodyBVH& bvh, const Vec3& p,
                            Vec3& outCP, Vec3& outNormal, float& outDist2) {
    if (!bvh.valid || bvh.nodes.empty()) return false;
    float best = 1e30f;
    int bestTri = -1;
    Vec3 bestCP{0.0f, 0.0f, 0.0f};

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
                Vec3 cp = ClosestPtTriangle(p, bvh.verts[t[0]], bvh.verts[t[1]], bvh.verts[t[2]]);
                float d2 = Len2(Sub(p, cp));
                if (d2 < best) { best = d2; bestTri = ti; bestCP = cp; }
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
                return {sameMod.string(), referenceShape};
            }

            for (const auto& shapeRoot : roots) {
                fs::path candidate = shapeRoot / relShape;
                if (fs::exists(candidate)) {
                    HLog("resolve: project found in '%s'", ref.ospPath.string().c_str());
                    HLog("resolve: reference shape '%s'", referenceShape.c_str());
                    HLog("resolve: NIF found via VFS-style search '%s'", candidate.string().c_str());
                    return {candidate.string(), referenceShape};
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

// (Re)build per-shape weights for the currently open project. Caller holds lock.
static void RebuildCacheLocked() {
    g_shapeWeights.clear();
    g_shapeBoneProfiles.clear();
    g_shapeVerts.clear();
    g_shapeEdges.clear();
    g_shapeBending.clear();
    g_shapeGeometryPresets.clear();
    g_refVerts.clear();
    g_refTris.clear();
    g_bodyShellCache.clear();
    g_bodyBVHCache.clear();
    g_shapeMask.clear();
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
        // Pass boneProfiles before moving it — needed for TUBULAR/DRAPED detection.
        int geometryPreset = GuessPresetFromGeometry(outVerts, g_shapeWeights[name], &boneProfiles);
        if (geometryPreset != DRAPE_PRESET_AUTO)
            g_shapeGeometryPresets[name] = geometryPreset;
        auto edges   = BuildEdgeConstraints(s, outVerts);
        auto bending = BuildBendingConstraints(s, outVerts);
        g_shapeBoneProfiles[name] = std::move(boneProfiles);
        g_shapeVerts[name]   = std::move(outVerts);
        g_shapeEdges[name]   = std::move(edges);
        g_shapeBending[name] = std::move(bending);
        HLog("cache: shape '%s' created %zu bending constraints", name.c_str(), g_shapeBending[name].size());
        g_shapeMask[name]    = std::move(mask);
        ++built;
        HLog("cache: shape '%s' weights min=%.3f max=%.3f avg=%.3f full=%d partial=%d zero=%d edges=%zu boneVerts=%d avgTorsoBone=%.3f avgLimbBone=%.3f geomPreset='%s'",
             name.c_str(), minW, maxW, avgW, full, partial, zero,
             g_shapeEdges[name].size(), boneInfluenced, avgTorsoBone, avgLimbBone,
             PresetName(geometryPreset));
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

// ── Detour ─────────────────────────────────────────────────────────────

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
    int constraintIterations = 2;
    float stretchStiffness = 0.35f;
    int surfaceSmoothIterations = 1;
    float surfaceSmoothStrength = 0.08f;
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
    // 0 = disabled (stretch only → chain-mail look). ~0.001 = soft cloth.
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
        // loose. The Verlet here is only a gentle settle — kept weak so it doesn't
        // melt the hem with uneven vertical sag.
        p.verletCloth = true;
        p.collisionStiffness = 0.85f;
        // Tuned drape (2026-06-05): moderate values — visibly looser than the body
        // (clearance 0.9) with a real but bounded sag. gravity is PER-SUBSTEP and
        // there are ~50 substeps, so keep it small (0.06 × 50 ≈ 3 units of pull);
        // clothMaxDrop bounds the total sag so the hem can't melt/tear.
        p.bodyClearance = 0.90f;       // looser than 0.65, not the 2.0 overdrive
        p.bendingStiffness = 0.10f;    // a touch stiffer → fewer jagged folds
        p.bendingIterations = 2;
        p.surfaceSmoothIterations = 4; // extra smoothing → less jagged hem
        p.surfaceSmoothStrength = 0.18f;
        p.anchorStiffness = 0.50f;
        p.drapeReturnRate = 0.028f;
        p.drapeEnvelopeStrength = 1.0f;
        p.drapeEnvelopeBins = 96;
        p.gravity = 0.07f;             // per-substep; ×50 ≈ 3.5 units total sag
        p.clothMaxDrop = 8.0f;         // bounded sag — prevents melt/tear at hem
        p.stretchStiffness = 0.75f;    // stiffer cloth → resists tearing
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
    // going down) — the hardcoded "loose" rule. Only negative is invalid.
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

// ── Body-shell monotonic drape envelope (the hardcoded LOOSE rule) ──────────
// Implements the user's rule using the dense, smooth BODY SHELL instead of the
// sparse cloth verts: going DOWN, the cloth radius per angular sector can never
// fall below the maximum body radius seen from the top of the drape region down
// to that height (+ clearance). So the cloth bulges out at protrusions (breast,
// belly, butt) and hangs straight down (vertical) across the recessed parts
// (under-belly, waist, groin) — it never tucks back toward the center. Because
// the body shell is dense (48×96) and smooth, this is smooth and acts on EVERY
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
                               //   surface → flattens the cloth onto the smooth
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
    // forced the whole skirt to the belly/hip width down to the hem → a stiff
    // wide bell. The belly-overhang problem needs a different fix (per-contact
    // vertical drop, not a global radius floor), so keep the seed at the belt.
    int seedTopZb = drapeTopZb;

    // Per angular bin: envelope scans DOWN from seedTopZb (above the belly).
    // envelopeR = max(bodyR, prev_envelopeR - returnRate * dzPerBin).
    // returnRate=0  → strict column (never narrows below the widest point).
    // returnRate>0  → slow taper (hangs gradually inward after a protrusion).
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

// ── Hanging-chain pass (user spec item 5) ──────────────────────────────────
// Instead of reading the body shell only at each vertex's own Z (which is blind
// to a protrusion ABOVE the drape region, e.g. a belly that overhangs the belt),
// propagate the contact radius DOWN through the cloth's own edge graph:
//   process verts top→bottom; each drape vert's radius is at least the radius of
//   the connected vert ABOVE it (minus a tiny per-Z taper) and at least the body
//   radius at its position. Anchored verts (drapeEl<0.2) are NOT moved — they are
//   the seeds/contacts (the bodice carries the belly/bust radius into the chain).
// Effect: once a vert is pushed out by a contact, everything hanging below it in
// the same cloth column stays out and falls vertical — it never returns inward
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

static bool ApplyStaticClothPass(const std::string& shapeName,
                                 const std::string& sliderName,
                                 int preset,
                                 const std::vector<Vec3>& baseVerts,
                                 const std::vector<VertexBoneProfile>* profiles,
                                 const std::vector<EdgeConstraint>* edges,
                                 const BodyShell* bodyShell,
                                 const BodyBVH* bodyBVH,
                                 const std::unordered_map<uint16_t, float>& presetMask,
                                 const TargetDataDiffs& rawDiff,
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
        // cloth solve then hangs the drape region down from this fitted start —
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

        // Physics drape: loose verts have no anchor — physics determines their
        // position. Only tight verts (high follow) are anchored to vanilla conform.
        if (params.physicsDrape && f < 0.35f) {
            anchor   = 0.0f;
            mobility = 0.90f;
        }

        invMass[i]      = Clamp01(mobility);
        anchorWeight[i] = Clamp01(anchor);
    }

    static const std::vector<EdgeConstraint> kNoEdges;
    const std::vector<EdgeConstraint>& useEdges = edges ? *edges : kNoEdges;

    // ════════════════════════════════════════════════════════════════════════
    // UNIFIED VERTEX CLASSIFICATION
    // ════════════════════════════════════════════════════════════════════════
    // ONE pass decides each vertex's behavior. Everything downstream (invMass,
    // anchorWeight, vertClearance, gravity participation) derives from a single
    // continuous value:
    //
    //   drapeAmount[i] ∈ [0,1] — how much this vertex behaves like free-hanging
    //                            cloth vs. anchored-to-body.
    //     ~0.0  → anchored: bodice/collar/sleeve/cuff. Follows vanilla conform,
    //             stays fitted to the body, no gravity.
    //     ~1.0  → free drape: skirt/robe panel. Gravity + collision determine it.
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
    // "rigid plate/accessory" flag (medallion/buckle/chain → translate, keep shape).
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

        // ── Geometric descriptors per island (validated in the Python prototype:
        //    bbox flatness + extents discriminate plates/chains from drape far
        //    better than SDF/raycast, which collapse on thin shell garments). ──
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

        // ── STEP A: detect RIGID islands + per-island drape amount ──────────
        // islandRigid[id]: rigid plate/accessory (medallion/buckle/chain) → handled
        // by the rigid-island translate pass. islandDrape[id] ∈ [0,1]: how much the
        // island behaves as free-hanging cloth, from its aggregate bone profile.
        std::vector<uint8_t> islandRigid((size_t)numIslands, 0);
        std::vector<float>   islandDrape((size_t)numIslands, 1.0f);

        // Largest island vert count → used to gate "small" relative to the mesh.
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

            // RIGID plate / accessory → translate, keep shape. Conservative gate:
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

            // WAISTBAND / sculpted belt → rigid translate. These islands sit at the
            // waist Z-range, have strong pelvis+torso, but very low leg weight (unlike
            // the skirt). They form the ] cross-section of a belt and collapse under
            // cloth physics. Vanilla conform preserves their shape by translating them
            // with the body morph diff — we replicate that by flagging them as rigid.
            // Gate: pelvis+torso strong, leg weak, narrow Z-span (not a full skirt panel).
            {
                float zSpan = islMax[(size_t)id].z - islMin[(size_t)id].z;
                bool waistSig  = (ip.pelvis > 0.25f) && (ip.torso > 0.15f) && (ip.leg < 0.12f);
                bool narrowBand = zSpan < (meshDiag * 0.08f);
                if (waistSig && narrowBand) {
                    islandRigid[(size_t)id] = 1;
                    islandDrape[(size_t)id] = 0.0f;
                    continue;
                }
            }

            // Island drape level = mean per-vertex drape signal over the island.
            // (Sleeves are handled per-vertex by the sleeve-suppress term in
            // vertDrapeSignal — NOT by a whole-island sleeve gate, which would wrongly
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

        // ── STEP B: per-vertex drapeAmount ─────────────────────────────────
        // PURELY per-vertex: drape is decided by the vertex's own lower-body bone
        // signal + height (vertDrapeSignal). This distinguishes bodice from skirt
        // WITHIN a single connected island (a dress where bodice+skirt+belt are one
        // topological piece — the common case; the "belt" is not its own island).
        // We deliberately do NOT blend in the island mean here: a whole-dress island
        // has a low mean (dominated by the bodice) that would wrongly drag the skirt
        // verts toward anchor. The island pass is used ONLY to flag rigid plates.
        const float kSkirtClear = params.bodyClearance;  // full drape gap
        const float kHugClear   = 0.12f;                 // fitted hugs the body
        const float kRigidClear = 0.08f;                 // rigid trim near-zero
        for (size_t i = 0; i < n; ++i) {
            int id = islandId[i];
            if (id < 0 || id >= numIslands) { drapeAmount[i] = 0.0f; continue; }
            if (islandRigid[(size_t)id]) {
                drapeAmount[i]   = 0.0f;
                vertClearance[i] = kRigidClear;
                continue;
            }
            drapeAmount[i]   = vertDrapeSignal(i);
            vertClearance[i] = kHugClear + drapeAmount[i] * (kSkirtClear - kHugClear);
        }

    }

    // ── Non-verlet presets: per-vertex drape from bones (Z fallback if none) ──
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

    // Reduce anchor for loose drape verts — let stretch + collision settle them.
    // BUT: enforce a minimum anchor (0.15) to prevent unrestricted drift, which
    // would propagate corrections to areas with no vanilla conform diff (ghost diffs).
    for (size_t i = 0; i < n; ++i) {
        if (drapeEl[i] > 0.25f) {
            float drapeScale = 1.0f - drapeEl[i] * (1.0f - follow[i]);
            // physicsDrape: loose drape verts must have anchor=0 — pure physics.
            // The 0.15 minimum was preventing physicsDrape from working at all:
            // 0.15 × anchorStiffness per iter × 80 iter ≈ full pull to supportVerts.
            float minAnchor = params.physicsDrape ? 0.0f : 0.15f;
            anchorWeight[i]  = std::max(Clamp01(anchorWeight[i] * drapeScale), minAnchor);
        }
    }

    // ── Slider-source locality ───────────────────────────────────────────
    // Compute per-vert influence: how much did the body actually change at this
    // outfit vert's 3D location for this specific slider?
    //
    // Belly slider  → large change at abdomen, near-zero at ankles/sleeves
    // RM_Heavy      → large change everywhere → solver runs on all verts
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
            // Slider barely moves the body → skip cloth pass entirely
            outDiff.clear();
            return false;
        }

        float invMax = 1.0f / maxChange;
        for (size_t i = 0; i < n; ++i)
            directInfluence[i] = Clamp01(directInfluence[i] * invMax);

        // Propagate through cloth mesh (only through drape-eligible verts).
        // Upper-body anchor verts (drapeEl ≈ 0) block propagation so belly
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

    // ── Inward suppression ────────────────────────────────────────────────
    // Only used in correction mode (physicsDrape=false).
    // In physicsDrape mode the initial body-push (after collideVertex) handles it.
    // For drape-eligible loose verts: cancel the inward radial component of
    // the vanilla conform delta proportional to looseness.
    //
    // Concept: vanilla conform may follow body concavities (waist pull-in, inner
    // thigh, etc.). A loose skirt/hem should NOT follow inward — it should stay
    // at or outside its current radius. Outward conform moves (body expanding) are
    // always kept as-is. Only inward moves are selectively suppressed.
    //
    // For sliders that expand the body (RM_Heavy, BigHips, etc.), ALL diffs are
    // outward → no suppression → result equals vanilla conform. For concave sliders
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
    auto collideVertex = [&](size_t i, float stiffness) -> bool {
        if (stiffness <= 0.0f) return false;
        float vc = vertClearance[i];
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

    // ── Physics drape: initial body-push ─────────────────────────────────
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

    // ── Proximity constraints (synthetic edges for near-touching cross-island verts) ──
    // Problem: a belt or trim esculpted as part of a single dress mesh has no separate
    // island. Its vertices are topologically connected to the surrounding cloth and
    // move with it, but the belt straps can straddle seams where adjacent vert pairs
    // have NO edge between them (different UV islands → no shared edge in the mesh).
    // During the Verlet solve those pairs drift freely, tearing the belt apart.
    //
    // Solution: before the solve, find all base-mesh vertex pairs that are physically
    // close (< kProxDist) but have NO topological edge connecting them (different islands
    // OR simply not in useEdges). Add them as synthetic stretch constraints with the
    // VANILLA rest length. They resist ONLY over-stretching (like all stretch constraints),
    // so the cloth can still drape — they just prevent the strap from tearing open.
    std::vector<EdgeConstraint> proxEdges;
    std::vector<float>          proxRestLen;
    if (params.verletCloth && n <= 0xFFFF) {
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

        for (size_t i = 0; i < n; ++i) {
            const Vec3& p = baseVerts[i];
            for (int dx=-1;dx<=1;++dx) for (int dy=-1;dy<=1;++dy) for (int dz=-1;dz<=1;++dz) {
                Vec3 qp{ p.x+dx*pcell, p.y+dy*pcell, p.z+dz*pcell };
                auto it = pgrid.find(pkey(qp)); if (it==pgrid.end()) continue;
                for (uint32_t j : it->second) {
                    if (j <= (uint32_t)i) continue;
                    if (Len2(Sub(baseVerts[j], p)) > kProxDist2) continue;
                    // Skip if there's already a real edge between i and j.
                    uint64_t key = ((uint64_t)i << 32) | j;
                    if (edgeSet.count(key)) continue;
                    // Rest length from BASE mesh (not support/morphed): this preserves
                    // the sculpted 3D shape (e.g. belt ] cross-section thickness).
                    // Using support rest length was wrong — on body morph the support
                    // already collapses the belt sides together, so the constraint
                    // then actively flattened the belt to match that morphed distance.
                    float vLen = Len(Sub(baseVerts[j], baseVerts[i]));
                    if (vLen < 1.0e-4f) continue;  // degenerate pair
                    EdgeConstraint ec;
                    ec.a = (uint16_t)i; ec.b = (uint16_t)j; ec.restLength = vLen;
                    proxEdges.push_back(ec);
                    proxRestLen.push_back(vLen);
                }
            }
        }
        if (!proxEdges.empty())
            HLog("detour: prox-constraints shape='%s' synthetic=%zu",
                 shapeName.c_str(), proxEdges.size());
    }

    // ── Cloth drape solve (momentum-free relaxation) ───────────────────────
    // Keeps the key idea from johnBuffer/ClothSimulation — distance constraints
    // that resist ONLY stretching, so the cloth folds and hangs instead of
    // shrink-wrapping the body — but WITHOUT Verlet momentum. Momentum let the
    // constraint corrections feed back into velocity and accumulate energy, which
    // tore the mesh apart (hem verts reached escape velocity). Pure Gauss-Seidel
    // relaxation toward a gravity-loaded equilibrium always converges → no tears.
    // A hard per-vert max-drop and a hard edge-length cap make it unconditionally
    // stable. Pinned verts (low drape eligibility) hold the garment at the waist.
    if (params.verletCloth) {
        std::vector<float> m(n, 0.0f);   // effective inv-mass; 0 = pinned
        for (size_t i = 0; i < n; ++i) {
            bool pinned = (invMass[i] <= 0.0f) || (drapeEl[i] < 0.20f);
            m[i] = pinned ? 0.0f : Clamp01(invMass[i]);
        }

        // ── Seam welding ───────────────────────────────────────────────────
        // Game meshes split vertices at UV/material seams: two (or more) verts
        // share a 3D position but have different indices and NO edge between them.
        // Vanilla conform moves them identically; our physics moves each one based
        // on its own edge fan, so they drift apart → visible slits with the body
        // poking through, and detached hem-trim flapping. Group verts that are
        // coincident in the base mesh and force them to move as one each pass.
        std::unordered_map<uint64_t, std::vector<uint32_t>> coincidence;
        coincidence.reserve(n);
        auto quantKey = [](const Vec3& p) -> uint64_t {
            auto q = [](float v) -> uint64_t {
                return (uint64_t)(uint32_t)(int32_t)std::llround(v * 100.0f);
            };
            return (q(p.x) * 73856093ull) ^ (q(p.y) * 19349663ull) ^ (q(p.z) * 83492791ull);
        };
        for (size_t i = 0; i < n; ++i)
            coincidence[quantKey(baseVerts[i])].push_back((uint32_t)i);
        std::vector<std::vector<uint32_t>> weldGroups;
        for (auto& kv : coincidence)
            if (kv.second.size() > 1) weldGroups.push_back(std::move(kv.second));

        // ── Inter-island junction stitching ─────────────────────────────────
        // The dress bodice (fitted, stays put) and the skirt (drape, falls) are
        // SEPARATE islands that merely TOUCH at the waist with no shared edges
        // (verified in the log: bodice z=[0.63-0.97], skirt z=[0.05-0.64]). When
        // the body grows, the fitted bodice holds while the draping skirt moves,
        // and the unwelded gap between them opens into a visible hole at the back.
        // Coincidence welding misses it because the two borders aren't EXACTLY
        // coincident. Here we explicitly pair near-touching verts from DIFFERENT
        // islands and weld them so the skirt top tracks the bodice bottom.
        if (!islandIdOut.empty() && islandIdOut.size() == n) {
            // Spatial hash for O(n) neighbour lookup (cell ≈ kStitchDist).
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
            // kStitchDist and weld that PAIR (2 verts only — never a blob, so the
            // skirt border can't collapse to a point). Each pair is its own group;
            // the existing weldSeams keeps them coincident every substep.
            // Gate: the horizontal (XY) offset must be small — a bodice→skirt junction
            // is verts at the same circumference but different Z. This lets a large
            // VERTICAL gap stitch (the real hole) without pulling front-to-back.
            const float kStitchXY2 = 0.45f * 0.45f;
            int stitched = 0;
            std::unordered_map<uint64_t,int> islandPairCount;  // diag: which islands join
            for (size_t i = 0; i < n; ++i) {
                int idi = islandIdOut[i];
                if (idi < 0) continue;
                // Never stitch FROM a rigid island — rigid islands use translate pass,
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
                        // Also skip if partner is rigid — same reason.
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
                if (len <= rest) continue;            // slack/compressed → fold freely
                Vec3 delta = Scale(dir, ((len - rest) / len) * k / wSum);
                finalVerts[e.a] = Sub(finalVerts[e.a], Scale(delta, w1));
                finalVerts[e.b] = Add(finalVerts[e.b], Scale(delta, w2));
                ++stats.stretchAdjusted;
            }
        };

        // Proximity constraints: bidirectional distance-preserving for vert pairs
        // that are near-touching in the base mesh but have no topological edge.
        // Bidirectional (resist BOTH stretch and compression) so belt straps can't
        // drift apart OR bunch together — they hold their vanilla relative position.
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
                // braces — they preserve sculpted shapes (belt ] cross-section) which
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

                // Re-pin pinned verts to the conformed fit.
                for (size_t i = 0; i < n; ++i)
                    if (m[i] <= 0.0f) finalVerts[i] = supportVerts[i];

                // Weld seams so coincident (duplicate) verts never split.
                weldSeams();
            }

            // 3) Hard stretch cap: no edge may exceed rest*kMaxStretch. This is the
            //    definitive anti-tear — even if relaxation under-converges, edges
            //    can never blow up. Two strong asymmetric passes against the cap.
            //    Also enforce proxy constraints so belt straps don't exceed cap either.
            if (!proxEdges.empty()) solveProxStretch(1.0f);
            for (int it = 0; it < 2; ++it) {
                for (size_t ei = 0; ei < useEdges.size(); ++ei) {
                    const auto& e = useEdges[ei];
                    if (e.a >= n || e.b >= n) continue;
                    float w1 = m[e.a], w2 = m[e.b];
                    float wSum = w1 + w2;
                    if (wSum <= kLenEps) continue;
                    Vec3 dir = Sub(finalVerts[e.a], finalVerts[e.b]);
                    float len = Len(dir);
                    float rest = (ei < supportRestLen.size()) ? supportRestLen[ei] : e.restLength;
                    float cap = rest * kMaxStretch;
                    if (len <= cap || len <= kLenEps) continue;
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
                // ones with low slider-source influence — otherwise the cloth sinks in.
                if (params.physicsDrape) eff = std::max(eff, 0.60f);
                if (collideVertex(i, collisionStep * freeMass * eff))
                    ++stats.bodyAdjusted;
                ++stats.bodySamples;
            }
        }

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
    // so envelope and body-column passes leave them untouched (loose=0 → strength≈0).
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

    // Slider-source influence blend (CORRECTION MODE ONLY).
    // In physicsDrape mode, physics determines cloth position — no blend-back.
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

    // ── WRITE-PATH DIAGNOSTIC ──────────────────────────────────────────────
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

    // ── Finishing (verletCloth) ────────────────────────────────────────────
    // Primary collider is now the 3D BVH: smooth, then push every drape vert onto
    // the REAL body surface (closest point + normal). This replaces the radial
    // shell envelope as the shaper — the result follows the actual belly/hip/bust
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

        // ── Loose anti-tuck rule (BVH-based) ─────────────────────────────────
        // Going DOWN the garment, dist-to-body can only INCREASE (new contact)
        // or hold steady (vertical fall), never decrease abruptly (tuck-in).
        // Per angular bin: running max of dist-to-body; verts below the floor are
        // pushed back out along the direction (vert→body) inverted — NOT along the
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
                // Direction FROM body surface TOWARD vert — stable at any distance,
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
        for (auto& kv : coin) {
            auto& g = kv.second;
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

    // ── Final anti-clip pass (all verts, verletCloth + BVH) ──────────────────
    // Anchored verts (drapeEl < 0.20) return to supportVerts (vanilla conform),
    // which can be inside the morphed body — they're never touched by the drape
    // collision. A small-clearance BVH push on every vert catches these without
    // disturbing verts that are already correctly positioned outside the body.
    // Clearance is intentionally smaller (0.15) than bodyClearance so it only
    // fixes real clips without pushing fitted/belted pieces off the body visibly.
    if (params.verletCloth && bodyBVH && bodyBVH->valid) {
        for (size_t i = 0; i < n; ++i) {
            Vec3 target = finalVerts[i];
            // Use per-vert clearance; belt verts use their small clearance,
            // skirt verts use their full clearance. Floor at 0.10 to catch
            // any genuine penetration even for fully-anchored verts.
            float ac = std::max(0.10f, vertClearance[i] * 0.5f);
            if (BVHCollideVertex(*bodyBVH, target, ac))
                finalVerts[i] = target;
        }
    }

    // ── Rigid-island pass ────────────────────────────────────────────────────
    // A medallion, buckle or plate should TRANSLATE with the morphing body but
    // keep its own shape — vanilla conform deforms it vertex-by-vertex (the
    // medallion bulges with the breast under it). Replace each rigid island's
    // per-vertex motion with a single rigid translation: the average
    // (supportVert - baseVert) over the island. Shape is preserved exactly.
    //
    // Two triggers:
    //   (a) Whole-mesh Rigid preset → every island is rigid.
    //   (b) Loose/Fitted mesh with geometry-detected rigid islands (islandRigidOut)
    //       → only those islands (the merged medallion/buckle) are rigidified; the
    //       skirt/sleeves keep their cloth solve. This is the "merged accessory"
    //       case the prototype validated.
    int normPreset = NormalizePreset(preset);
    bool wholeMeshRigid = (normPreset == DRAPE_PRESET_RIGID_ACCESSORY ||
                           normPreset == DRAPE_PRESET_RIGID_ARMOR);
    // haveMergedRigid: geometry-detected rigid plates inside a Loose/Fitted mesh
    // (merged medallion case). Disabled for now — causes arm shrinkage regression
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
            // rawDiff ≈ 0, making islandCount stay 0 and the island get no shift.
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

        // ── Chain/cordão per-vert IDW shift ─────────────────────────────────
        // A necklace chain (isl#10 in Medallions) is a single large island that
        // spans between multiple medallion islands.  Applying one mean shift to all
        // 153 chain verts translates the whole chain as a block → chain floats away
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

        // Collect "anchor" islands: rigid, small (≤ kChainVertThresh verts), real shift.
        struct AnchorIsl { int id; Vec3 centroid; Vec3 shift; };
        std::vector<AnchorIsl> anchors;
        for (int id = 0; id < nIsl; ++id) {
            if (!islandIsRigid(id)) continue;
            if (islandSizeR[(size_t)id] > kChainVertThresh) continue;
            if (Len2(islandShift[(size_t)id]) < kShiftNearZeroSq) continue;
            anchors.push_back({id, islandCentroid[(size_t)id], islandShift[(size_t)id]});
        }

        // For each chain island, build a per-vert shift table via IDW from anchors.
        // Key: island id → vector<Vec3> indexed by position in the full vert array.
        // We store as a flat map vertIdx→shift.
        std::unordered_map<size_t, Vec3> chainVertShift;
        if (!anchors.empty()) {
            for (size_t i = 0; i < n; ++i) {
                int id = island[i];
                if (id < 0 || !islandIsRigid(id)) continue;
                if (islandSizeR[(size_t)id] <= kChainVertThresh) continue;  // not a chain island
                // IDW: weight = 1/dist² to each anchor centroid.
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

        // ── Rigid-border seam closure ───────────────────────────────────────
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
                            if (islandIsRigid(idj)) continue;   // both rigid — skip
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

    // ── Cross-island seam closure (ROOT-CAUSE FIX for the back hole) ─────────
    // Root cause (proven by gap-diag instrumentation): the garment has separate
    // islands that BUTT against each other at the waist with NO triangle spanning
    // the join and only a small (~0.4u) vanilla separation. Vanilla conform moves
    // both sides by the same body diff, so the unmodeled seam stays sub-pixel. Our
    // physics moves each side independently (no edge links them, and the welds only
    // catch EXACTLY-coincident verts at 0.01u, not these 0.4u seams), so the sides
    // drift apart — measured up to 4.0u — opening the seam into a visible hole.
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
        // pairs are impossible — those are >5u apart (body width). Cell ≥ radius so
        // the 27-cell neighbour search covers the radius.
        const float kNear  = 0.65f;
        const float kNear2 = kNear * kNear;
        const float cell   = 0.70f;
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
                        if (Len2(Sub(baseVerts[j], p)) > kNear2) continue; // near-touching only
                        // Rigid-island verts (medallion/buckle) must keep their exact
                        // translated shape — never move them. If BOTH sides are rigid,
                        // skip; if ONE is rigid, the loose side takes the full fix.
                        bool ri = (islandIdOut[i] >= 0 && islandIdOut[i] < (int)islandRigidOut.size()
                                   && islandRigidOut[(size_t)islandIdOut[i]]);
                        bool rj = (islandIdOut[j] >= 0 && islandIdOut[j] < (int)islandRigidOut.size()
                                   && islandRigidOut[(size_t)islandIdOut[j]]);
                        if (ri && rj) continue;
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
                        if (ri)      { wi = 0.0f; wj = 1.0f; }   // i rigid → only j moves
                        else if (rj) { wi = 1.0f; wj = 0.0f; }   // j rigid → only i moves
                        else {
                            float di = drapeAmount[i], dj = drapeAmount[j];
                            float sum = di + dj;
                            wi = (sum > 1e-4f) ? dj / sum : 0.5f; // i takes share ∝ other's drape
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

    // Write only non-zero diffs (matches BodySlide convention).
    outDiff.clear();
    outDiff.reserve(rawDiff.size());
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

    return true;
}

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

    if (!g_enabled.load(std::memory_order_acquire) || !g_diffZeroVert ||
        !self || !shapeName || !sliderName || solidMode) {
        return;
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

    // ── Static cloth pass (main path) ──────────────────────────────────────
    if (!presetMask.empty() && g_getRawResultDiff && g_setResultDiff) {
        TargetDataDiffs rawDiff;
        if (HostGetRawResultDiff(self, shapeName, sliderName, rawDiff)) {
            // rawDiff empty → this slider has no conform overlap with this shape:
            // ApplyStaticClothPass would return false immediately and the result
            // diff is also empty/trivial. Fall through to ZeroVertDiff (no-op).
            // This is expected behavior; no log entry needed per slider.
            TargetDataDiffs solvedDiff;
            StaticSolveStats stats;
            bool passOk = ApplyStaticClothPass(*shapeName, *sliderName, preset,
                                               vertsIt->second, profiles, edges, bodyShell, bodyBVH,
                                               presetMask, rawDiff, solvedDiff, stats);
            if (passOk) {
                if (HostSetResultDiff(self, shapeName, sliderName, solvedDiff)) {
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
                // HostSetResultDiff failed — fall through to ZeroVertDiff
                HLog("detour: SKIPPED shape='%s' slider='%s' preset='%s' "
                     "reason='HostSetResultDiff failed' → FALLBACK_SCALE",
                     shapeName->c_str(), sliderName->c_str(), PresetName(preset));
            } else if (!rawDiff.empty()) {
                // rawDiff was non-empty but pass still rejected (preset disabled, etc.)
                {
                    const char* reason = !StaticParamsForPreset(preset).enabled
                        ? "preset disables static pass (skin-tight)"
                        : "solver returned false";
                    HLog("detour: SKIPPED shape='%s' slider='%s' preset='%s' "
                         "reason='%s' → FALLBACK_SCALE",
                         shapeName->c_str(), sliderName->c_str(), PresetName(preset), reason);
                }
            }
        } else {
            // HostGetRawResultDiff failed (ABI issue or missing RVA)
            if (g_fallbackShapesLogged.insert(*shapeName).second)
                HLog("detour: SKIPPED static cloth shape='%s' "
                     "reason='HostGetRawResultDiff failed' → FALLBACK_SCALE",
                     shapeName->c_str());
        }
    } else if (!presetMask.empty()) {
        // Host methods not wired (offsets not confirmed for this exe build)
        if (g_fallbackShapesLogged.insert(*shapeName).second)
            HLog("detour: SKIPPED static cloth shape='%s' "
                 "reason='%s' → FALLBACK_SCALE",
                 shapeName->c_str(),
                 !g_getRawResultDiff ? "GetRawResultDiff RVA not wired"
                 : "SetResultDiff RVA not wired");
    }

    // ── Fallback: ZeroVertDiff mask scale ──────────────────────────────────
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

// ── Public API (called by version.dll after injection) ─────────────────

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
        HLog("install: offsets not configured for this build — hook disabled");
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
int DrapeClearShapePresets() {
    std::lock_guard<std::mutex> lk(g_cacheMx);
    g_shapePresets.clear();
    g_appliedShapesLogged.clear();
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
    g_appliedShapesLogged.clear();
    HLog("preset-ui: shape '%s' preset='%s'", shapeNameUtf8, PresetName(normalized));
    return 0;
}

extern "C" __declspec(dllexport)
void DrapeSetProcessingIntensity(int level) {
    int clamped = (level < 0) ? 0 : (level > 2) ? 2 : level;
    g_processingIntensity.store(clamped, std::memory_order_relaxed);
    static const char* kLabels[] = {"Low", "Medium", "High"};
    HLog("preset-ui: processing intensity set to %s (%d)", kLabels[clamped], clamped);
}
